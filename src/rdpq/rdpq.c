#include "rdpq.h"
#include "rdpq_block.h"
#include "rdpq_constants.h"
#include "rspq.h"
#include "rspq/rspq_commands.h"
#include "rspq_constants.h"
#include "rdp_commands.h"
#include "interrupt.h"
#include <string.h>
#include <stdbool.h>

#define RDPQ_MAX_COMMAND_SIZE 44
#define RDPQ_BLOCK_MIN_SIZE   64
#define RDPQ_BLOCK_MAX_SIZE   4192

#define RDPQ_OVL_ID (0xC << 28)

static void rdpq_assert_handler(rsp_snapshot_t *state, uint16_t assert_code);

DEFINE_RSP_UCODE(rsp_rdpq, 
    .assert_handler=rdpq_assert_handler);

typedef struct rdpq_state_s {
    uint64_t sync_full;
    uint32_t address_table[RDPQ_ADDRESS_TABLE_SIZE];
    uint64_t other_modes;
    uint64_t scissor_rect;
    uint32_t fill_color;
    uint32_t rdram_state_address;
    uint8_t target_bitdepth;
} rdpq_state_t;

typedef struct rdpq_block_s {
    rdpq_block_t *next;
    uint32_t padding;
    uint32_t cmds[];
} rdpq_block_t;

bool __rdpq_inited = false;

static volatile uint32_t *rdpq_block_ptr;
static volatile uint32_t *rdpq_block_end;

static bool rdpq_block_active;
static uint8_t rdpq_config;

static uint32_t rdpq_autosync_state[2];

static rdpq_block_t *rdpq_block;
static int rdpq_block_size;

static volatile uint32_t *last_rdp_cmd;

static void __rdpq_interrupt(void) {
    rdpq_state_t *rdpq_state = UncachedAddr(rspq_overlay_get_state(&rsp_rdpq));

    assert(*SP_STATUS & SP_STATUS_SIG_RDPSYNCFULL);

    // The state has been updated to contain a copy of the last SYNC_FULL command
    // that was sent to RDP. The command might contain a callback to invoke.
    // Extract it to local variables.
    uint32_t w0 = (rdpq_state->sync_full >> 32) & 0x00FFFFFF;
    uint32_t w1 = (rdpq_state->sync_full >>  0) & 0xFFFFFFFF;

    // Notify the RSP that we've serviced this SYNC_FULL interrupt. If others
    // are pending, they can be scheduled now, even as we execute the callback.
    MEMORY_BARRIER();
    *SP_STATUS = SP_WSTATUS_CLEAR_SIG_RDPSYNCFULL;

    // If there was a callback registered, call it.
    if (w0) {
        void (*callback)(void*) = (void (*)(void*))CachedAddr(w0 | 0x80000000);
        void* arg = (void*)w1;

        callback(arg);
    }
}

void rdpq_init()
{
    rdpq_state_t *rdpq_state = UncachedAddr(rspq_overlay_get_state(&rsp_rdpq));

    memset(rdpq_state, 0, sizeof(rdpq_state_t));
    rdpq_state->rdram_state_address = PhysicalAddr(rdpq_state);
    rdpq_state->other_modes = ((uint64_t)RDPQ_OVL_ID << 32) + ((uint64_t)RDPQ_CMD_SET_OTHER_MODES << 56);

    // The (1 << 12) is to prevent underflow in case set other modes is called before any set scissor command.
    // Depending on the cycle mode, 1 subpixel is subtracted from the right edge of the scissor rect.
    rdpq_state->scissor_rect = (((uint64_t)RDPQ_OVL_ID << 32) + ((uint64_t)RDPQ_CMD_SET_SCISSOR_EX_FIX << 56)) | (1 << 12);

    rspq_init();
    rspq_overlay_register_static(&rsp_rdpq, RDPQ_OVL_ID);

    rdpq_block = NULL;
    rdpq_block_active = false;
    rdpq_config = RDPQ_CFG_AUTOSYNCPIPE | RDPQ_CFG_AUTOSYNCLOAD | RDPQ_CFG_AUTOSYNCTILE;
    rdpq_autosync_state[0] = 0;

    __rdpq_inited = true;

    register_DP_handler(__rdpq_interrupt);
    set_DP_interrupt(1);
}

void rdpq_close()
{
    rspq_overlay_unregister(RDPQ_OVL_ID);
    __rdpq_inited = false;

    set_DP_interrupt( 0 );
    unregister_DP_handler(__rdpq_interrupt);
}

uint32_t rdpq_get_config(void)
{
    return rdpq_config;
}

void rdpq_set_config(uint32_t cfg)
{
    rdpq_config = cfg;
}

uint32_t rdpq_change_config(uint32_t on, uint32_t off)
{
    uint32_t old = rdpq_config;
    rdpq_config |= on;
    rdpq_config &= ~off;
    return old;
}


void rdpq_fence(void)
{
    rdpq_sync_full(NULL, NULL);
    rspq_int_write(RSPQ_CMD_RDP_WAIT_IDLE);
}

static void rdpq_assert_handler(rsp_snapshot_t *state, uint16_t assert_code)
{
    switch (assert_code)
    {
    case RDPQ_ASSERT_FLIP_COPY:
        printf("TextureRectangleFlip cannot be used in copy mode\n");
        break;
    
    default:
        printf("Unknown assert\n");
        break;
    }
}

static void autosync_use(uint32_t res) { 
    rdpq_autosync_state[0] |= res;
}

static void autosync_change(uint32_t res) {
    res &= rdpq_autosync_state[0];
    if (res) {
        if ((res & AUTOSYNC_TILES) && (rdpq_config & RDPQ_CFG_AUTOSYNCPIPE))
            rdpq_sync_pipe();
        if ((res & AUTOSYNC_TMEMS) && (rdpq_config & RDPQ_CFG_AUTOSYNCLOAD))
            rdpq_sync_load();
        if ((res & AUTOSYNC_PIPE)  && (rdpq_config & RDPQ_CFG_AUTOSYNCPIPE))
            rdpq_sync_pipe();
    }
}

void __rdpq_reset_buffer()
{
    last_rdp_cmd = NULL;
}

void __rdpq_block_flush(uint32_t *start, uint32_t *end)
{
    assertf(((uint32_t)start & 0x7) == 0, "start not aligned to 8 bytes: %lx", (uint32_t)start);
    assertf(((uint32_t)end & 0x7) == 0, "end not aligned to 8 bytes: %lx", (uint32_t)end);

    uint32_t phys_start = PhysicalAddr(start);
    uint32_t phys_end = PhysicalAddr(end);

    // FIXME: Updating the previous command won't work across buffer switches
    uint32_t diff = rdpq_block_ptr - last_rdp_cmd;
    if (diff == 2 && (*last_rdp_cmd&0xFFFFFF) == phys_start) {
        // Update the previous command
        *last_rdp_cmd = (RSPQ_CMD_RDP<<24) | phys_end;
    } else {
        // Put a command in the regular RSP queue that will submit the last buffer of RDP commands.
        last_rdp_cmd = rdpq_block_ptr;
        rspq_int_write(RSPQ_CMD_RDP, phys_end, phys_start);
    }
}

void __rdpq_block_switch_buffer(uint32_t *new, uint32_t size)
{
    assert(size >= RDPQ_MAX_COMMAND_SIZE);

    rdpq_block_ptr = new;
    rdpq_block_end = new + size - RDPQ_MAX_COMMAND_SIZE;

    // Enqueue a command that will point RDP to the start of the block so that static fixup commands still work.
    // Those commands rely on the fact that DP_END always points to the end of the current static block.
    __rdpq_block_flush((uint32_t*)rdpq_block_ptr, (uint32_t*)rdpq_block_ptr);
}

void __rdpq_block_next_buffer()
{
    // Allocate next chunk (double the size of the current one).
    // We use doubling here to reduce overheads for large blocks
    // and at the same time start small.
    if (rdpq_block_size < RDPQ_BLOCK_MAX_SIZE) rdpq_block_size *= 2;
    rdpq_block->next = malloc_uncached(sizeof(rdpq_block_t) + rdpq_block_size*sizeof(uint32_t));
    rdpq_block = rdpq_block->next;
    rdpq_block->next = NULL;

    // Switch to new buffer
    __rdpq_block_switch_buffer(rdpq_block->cmds, rdpq_block_size);
}

void __rdpq_block_begin()
{
    rdpq_block_active = true;
}

void __rdpq_block_end()
{
    rdpq_block_active = false;
    rdpq_block = NULL;
}

void __rdpq_block_free(rdpq_block_t *block)
{
    while (block) {
        void *b = block;
        block = block->next;
        free_uncached(b);
    }
}

__attribute__((noinline))
static void __rdpq_block_create(void)
{
    extern void __rspq_block_begin_rdp(rdpq_block_t*);

    rdpq_block_size = RDPQ_BLOCK_MIN_SIZE;
    rdpq_block = malloc_uncached(sizeof(rdpq_block_t) + rdpq_block_size*sizeof(uint32_t));
    rdpq_block->next = NULL;
    __rdpq_reset_buffer();
    __rdpq_block_switch_buffer(rdpq_block->cmds, rdpq_block_size);
    __rspq_block_begin_rdp(rdpq_block);
}

static void __rdpq_block_check(void)
{
    if (rdpq_block_active && rdpq_block == NULL)
        __rdpq_block_create();
}

/// @cond

#define _rdpq_write_arg(arg) \
    *ptr++ = (arg);

/// @endcond

/* static bool __debug_command_is_first = false;
static uint32_t __debug_command_id = 0;
static void __rdpq_debug_arg(uint32_t arg) {
    // FIXME: only do this in debug mode
    if (__debug_command_is_first) {
        __debug_command_is_first = false;
        debugf("RDP command: %08lX ", arg | (RDPQ_OVL_ID + ((__debug_command_id)<<24)));
    } else {
        debugf("%08lX ", arg);
    }
}
#define _rdpq_debug_arg(arg) __rdpq_debug_arg(arg);

#define rdpq_dynamic_write_debug(cmd_id, ...) ({ \
    __debug_command_is_first = true; \
    __debug_command_id = cmd_id; \
    __CALL_FOREACH(_rdpq_debug_arg, ##__VA_ARGS__); \
    debugf("\n");\
}) */

// rdpq_dynamic_write_debug(cmd_id, ##__VA_ARGS__);
#define rdpq_dynamic_write(cmd_id, ...) ({ \
    rspq_write(RDPQ_OVL_ID, (cmd_id), ##__VA_ARGS__); \
})

#define rdpq_static_write(cmd_id, arg0, ...) ({ \
    volatile uint32_t *ptr = rdpq_block_ptr; \
    *ptr++ = (RDPQ_OVL_ID + ((cmd_id)<<24)) | (arg0); \
    __CALL_FOREACH(_rdpq_write_arg, ##__VA_ARGS__); \
    __rdpq_block_flush((uint32_t*)rdpq_block_ptr, (uint32_t*)ptr); \
    rdpq_block_ptr = ptr; \
    if (__builtin_expect(rdpq_block_ptr > rdpq_block_end, 0)) \
        __rdpq_block_next_buffer(); \
})

#define rdpq_static_skip(size) ({ \
    for (int i = 0; i < (size); i++) rdpq_block_ptr++; \
    if (__builtin_expect(rdpq_block_ptr > rdpq_block_end, 0)) \
        __rdpq_block_next_buffer(); \
})

static inline bool in_block(void) {
    return rdpq_block_active;
}

#define rdpq_write(cmd_id, arg0, ...) ({ \
    if (in_block()) { \
        __rdpq_block_check(); \
        rdpq_static_write(cmd_id, arg0, ##__VA_ARGS__); \
    } else { \
        rdpq_dynamic_write(cmd_id, arg0, ##__VA_ARGS__); \
    } \
})

#define rdpq_fixup_write(cmd_id_dyn, cmd_id_fix, skip_size, arg0, ...) ({ \
    if (in_block()) { \
        __rdpq_block_check(); \
        rdpq_dynamic_write(cmd_id_fix, arg0, ##__VA_ARGS__); \
        rdpq_static_skip(skip_size); \
    } else { \
        rdpq_dynamic_write(cmd_id_dyn, arg0, ##__VA_ARGS__); \
    } \
})

__attribute__((noinline))
void rdpq_fixup_write8(uint32_t cmd_id_dyn, uint32_t cmd_id_fix, int skip_size, uint32_t arg0, uint32_t arg1)
{
    rdpq_fixup_write(cmd_id_dyn, cmd_id_fix, skip_size, arg0, arg1);
}

__attribute__((noinline))
void __rdpq_dynamic_write8(uint32_t cmd_id, uint32_t arg0, uint32_t arg1)
{
    rdpq_dynamic_write(cmd_id, arg0, arg1);
}

__attribute__((noinline))
void __rdpq_write8(uint32_t cmd_id, uint32_t arg0, uint32_t arg1)
{
    rdpq_write(cmd_id, arg0, arg1);
}

__attribute__((noinline))
void __rdpq_write8_syncchange(uint32_t cmd_id, uint32_t arg0, uint32_t arg1, uint32_t autosync)
{
    autosync_change(autosync);
    __rdpq_write8(cmd_id, arg0, arg1);
}

__attribute__((noinline))
void __rdpq_write8_syncuse(uint32_t cmd_id, uint32_t arg0, uint32_t arg1, uint32_t autosync)
{
    autosync_use(autosync);
    __rdpq_write8(cmd_id, arg0, arg1);
}

__attribute__((noinline))
void __rdpq_write16(uint32_t cmd_id, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    rdpq_write(cmd_id, arg0, arg1, arg2, arg3);    
}

__attribute__((noinline))
void __rdpq_write16_syncchange(uint32_t cmd_id, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t autosync)
{
    autosync_change(autosync);
    __rdpq_write16(cmd_id, arg0, arg1, arg2, arg3);
}

__attribute__((noinline))
void __rdpq_write16_syncuse(uint32_t cmd_id, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t autosync)
{
    autosync_use(autosync);
    __rdpq_write16(cmd_id, arg0, arg1, arg2, arg3);
}

__attribute__((noinline))
void __rdpq_fill_triangle(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, uint32_t w5, uint32_t w6, uint32_t w7)
{
    autosync_use(AUTOSYNC_PIPE);
    rdpq_write(RDPQ_CMD_TRI, w0, w1, w2, w3, w4, w5, w6, w7);
}

__attribute__((noinline))
void __rdpq_texture_rectangle(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    autosync_use(AUTOSYNC_PIPE);
    rdpq_fixup_write(RDPQ_CMD_TEXTURE_RECTANGLE_EX, RDPQ_CMD_TEXTURE_RECTANGLE_EX_FIX, 4, w0, w1, w2, w3);
}

__attribute__((noinline))
void __rdpq_set_scissor(uint32_t w0, uint32_t w1)
{
    // NOTE: SET_SCISSOR does not require SYNC_PIPE
    rdpq_fixup_write8(RDPQ_CMD_SET_SCISSOR_EX, RDPQ_CMD_SET_SCISSOR_EX_FIX, 2, w0, w1);
}

__attribute__((noinline))
void __rdpq_set_fill_color(uint32_t w1)
{
    autosync_change(AUTOSYNC_PIPE);
    rdpq_fixup_write8(RDPQ_CMD_SET_FILL_COLOR_32, RDPQ_CMD_SET_FILL_COLOR_32_FIX, 2, 0, w1);
}

__attribute__((noinline))
void __rdpq_set_fixup_image(uint32_t cmd_id_dyn, uint32_t cmd_id_fix, uint32_t w0, uint32_t w1)
{
    autosync_change(AUTOSYNC_PIPE);
    rdpq_fixup_write8(cmd_id_dyn, cmd_id_fix, 2, w0, w1);
}

__attribute__((noinline))
void __rdpq_set_color_image(uint32_t w0, uint32_t w1)
{
    autosync_change(AUTOSYNC_PIPE);
    rdpq_fixup_write8(RDPQ_CMD_SET_COLOR_IMAGE, RDPQ_CMD_SET_COLOR_IMAGE_FIX, 4, w0, w1);
}

__attribute__((noinline))
void __rdpq_set_other_modes(uint32_t w0, uint32_t w1)
{
    autosync_change(AUTOSYNC_PIPE);
    if (in_block()) {
        __rdpq_block_check(); \
        // Write set other modes normally first, because it doesn't need to be modified
        rdpq_static_write(RDPQ_CMD_SET_OTHER_MODES, w0, w1);
        // This command will just record the other modes to DMEM and output a set scissor command
        rdpq_dynamic_write(RDPQ_CMD_SET_OTHER_MODES_FIX, w0, w1);
        // Placeholder for the set scissor
        rdpq_static_skip(2);
    } else {
        // The regular dynamic command will output both the set other modes and the set scissor commands
        rdpq_dynamic_write(RDPQ_CMD_SET_OTHER_MODES, w0, w1);
    }
}

__attribute__((noinline))
void __rdpq_modify_other_modes(uint32_t w0, uint32_t w1, uint32_t w2)
{
    autosync_change(AUTOSYNC_PIPE);
    rdpq_fixup_write(RDPQ_CMD_MODIFY_OTHER_MODES, RDPQ_CMD_MODIFY_OTHER_MODES_FIX, 4, w0, w1, w2);
}

void rdpq_sync_full(void (*callback)(void*), void* arg)
{
    uint32_t w0 = PhysicalAddr(callback);
    uint32_t w1 = (uint32_t)arg;

    // We encode in the command (w0/w1) the callback for the RDP interrupt,
    // and we need that to be forwarded to RSP dynamic command.
    if (in_block()) {
        // In block mode, schedule the command in both static and dynamic mode.
        __rdpq_block_check();
        rdpq_dynamic_write(RDPQ_CMD_SYNC_FULL_FIX, w0, w1);
        rdpq_static_write(RDPQ_CMD_SYNC_FULL, w0, w1);
    } else {
        rdpq_dynamic_write(RDPQ_CMD_SYNC_FULL, w0, w1);
    }

    // The RDP is fully idle after this command, so no sync is necessary.
    rdpq_autosync_state[0] = 0;
}

void rdpq_sync_pipe(void)
{
    __rdpq_write8(RDPQ_CMD_SYNC_PIPE, 0, 0);
    rdpq_autosync_state[0] &= ~AUTOSYNC_PIPE;
}

void rdpq_sync_tile(void)
{
    __rdpq_write8(RDPQ_CMD_SYNC_TILE, 0, 0);
    rdpq_autosync_state[0] &= ~AUTOSYNC_TILES;
}

void rdpq_sync_load(void)
{
    __rdpq_write8(RDPQ_CMD_SYNC_LOAD, 0, 0);
    rdpq_autosync_state[0] &= ~AUTOSYNC_TMEMS;
}

/** 
 * @brief Prints out a disassemby of the RDP commands pointed by start_at
 *
 * @param[in] start_at
 *            The address to start reading from
 * @param[in] size
 *            How many bytes to disassemble. Must be a multiple of 8
 */
void rdpq_disasm(uint32_t* start_at, size_t size)
{
    assertf(size % 8 == 0, "size must be a multiple of 8: %d", size);

    for (size_t i = 0; i*8 < size;)
    {
        uint64_t command_word = ((uint64_t*)start_at)[i];
        uint64_t command_id = (command_word >> 56) & 0x3F;
        // debugf("here %u %016llx::%016llx \n", i, command_word, command_id);

        debugf("%08lX", (uint32_t)start_at + i*8);
        debugf("  ");
        debugf("%016llX : ", command_word);

        if (command_id == RDPQ_CMD_SET_COLOR_IMAGE)
        {
            // FIXME: add details for format and size
            debugf("Set Color Image, format: %llu size: %llu width: %llu DRAM addr: %08llX\n",
                (command_word >> 53) & 0x7,
                (command_word >> 51) & 0x3,
                (command_word >> 32) & 0x3FF,
                (command_word >> 0) & 0x1FFFFFF
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_TEXTURE_IMAGE)
        {
            // FIXME: add details for format and size
            debugf("Set Texture Image, format: %llu size: %llu width: %llu DRAM addr: %08llX\n",
                (command_word >> 53) & 0x7,
                (command_word >> 51) & 0x3,
                (command_word >> 32) & 0x3FF,
                (command_word >> 0) & 0x1FFFFFF
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_Z_IMAGE)
        {
            debugf("Set Z Image, DRAM addr: %08llX\n",
                (command_word >> 0) & 0x1FFFFFF
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_TILE)
        {
            // FIXME: add details for format and size
            debugf("Set Tile, format: %llu size: %llu line: %llu TMEM addr: %03llX tile: %llu pallette: %llu ct: %llu mt: %llu mask T: %llu shift T: %llu cs: %llu ms: %llu mask S: %llu shift S: %llu\n",
                (command_word >> 53) & 0x7,
                (command_word >> 51) & 0x3,
                (command_word >> 41) & 0x1FF,
                (command_word >> 32) & 0x1FF,
                (command_word >> 24) & 0x7,
                (command_word >> 20) & 0xF,
                (command_word >> 19) & 0x1,
                (command_word >> 18) & 0x1,
                (command_word >> 14) & 0xF,
                (command_word >> 10) & 0xF,
                (command_word >> 9) & 0x1,
                (command_word >> 8) & 0x1,
                (command_word >> 4) & 0xF,
                (command_word >> 0) & 0xF
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_LOAD_TILE)
        {
            debugf("Load Tile, ");
        }

        if (command_id == RDPQ_CMD_SET_TILE_SIZE)
        {
            debugf("Set Tile Size, ");
        }

        if (command_id == RDPQ_CMD_LOAD_TLUT)
        {
            debugf("Load TLUT, ");
        }

        if (command_id == RDPQ_CMD_LOAD_TLUT || command_id == RDPQ_CMD_LOAD_TILE || command_id == RDPQ_CMD_SET_TILE_SIZE)
        {
            debugf("SL: %llu.%02llu TL: %llu.%02llu tile: %llu SH: %llu.%02llu TH: %llu.%02llu\n",
                (command_word >> 46) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 44) & 0x3),
                (command_word >> 34) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 32) & 0x3),
                (command_word >> 24) & 0x3,
                (command_word >> 14) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 12) & 0x3),
                (command_word >> 2) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 0) & 0x3)
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_LOAD_BLOCK)
        {
            debugf("Load Block, SL: %llu TL: %llu tile: %llu SH: %llu DxT: %llu.%02llu\n",
                (command_word >> 44) & 0xFFF,
                (command_word >> 32) & 0xFFF,
                (command_word >> 24) & 0x7,
                (command_word >> 12) & 0xFFF,
                (command_word >> 11) & 0x1,
                RDPQ_FRAC_11_DECIMAL((command_word >> 0) & 0x7FF)
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_FILL_RECTANGLE)
        {
            debugf("Fill Rectangle, XL: %llu.%02llu YL: %llu.%02llu XH: %llu.%02llu YH: %llu.%02llu\n",
                (command_word >> 46) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 44) & 0x3),
                (command_word >> 34) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 32) & 0x3),
                (command_word >> 14) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 12) & 0x3),
                (command_word >> 2) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 0) & 0x3)
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_TEXTURE_RECTANGLE)
        {
            // FIXME: implement
            debugf("Texture Rectangle\n");
            i = i + 2;
            continue;
        }

        if (command_id == RDPQ_CMD_TEXTURE_RECTANGLE_FLIP)
        {
            // FIXME: implement
            debugf("Texture Rectangle Flip\n");
            i = i + 2;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_COMBINE_MODE)
        {
            static const char* sub_a_map[] =
            {
                "combined",         // 0
                "tex0",             // 1
                "tex1",             // 2
                "prim",             // 3
                "shade",            // 4
                "env",              // 5
                "1.0",              // 6
                "noise",            // 7
                "0.0",              // 8
            };

            static const char* sub_b_map[] =
            {
                "combined",         // 0
                "tex0",             // 1
                "tex1",             // 2
                "prim",             // 3
                "shade",            // 4
                "env",              // 5
                "keycenter",        // 6
                "k4",               // 7
                "0.0",              // 8
            };

            static const char* mul_map[] =
            {
                "combined",         // 0
                "tex0",             // 1
                "tex1",             // 2
                "prim",             // 3
                "shade",            // 4
                "env",              // 5
                "keyscale",         // 6
                "combinedAlpha",    // 7
                "tex0Alpha",        // 8
                "tex1Alpha",        // 9
                "primAlpha",        // 10
                "shadeAlpha",       // 11
                "envAlpha",         // 12
                "LODfrac",          // 13
                "primLODfrac",      // 14
                "k5",               // 15
                "0.0",              // 16
            };

            static const char* add_map[] =
            {
                "combined",         // 0
                "tex0",             // 1
                "tex1",             // 2
                "prim",             // 3
                "shade",            // 4
                "env",              // 5
                "1.0",              // 6
                "0.0",              // 7
            };

            uint64_t sub_a = ((command_word >> 52) & 0xF);
            uint64_t sub_b = ((command_word >> 28) & 0xF);
            uint64_t mul = ((command_word >> 47) & 0x1F);
            uint64_t add = ((command_word >> 15) & 0x7);

            debugf("Set Combine Mode, COLOR1: (%s - %s) x %s + %s\n",
                sub_a_map[sub_a],
                sub_b_map[sub_b],
                mul_map[mul],
                add_map[add]
            );

            sub_a = ((command_word >> 37) & 0xF);
            sub_b = ((command_word >> 24) & 0xF);
            mul = ((command_word >> 32) & 0x1F);
            add = ((command_word >> 6) & 0x7);

            debugf("                                               COLOR2: (%s - %s) x %s + %s\n",
                sub_a_map[sub_a],
                sub_b_map[sub_b],
                mul_map[mul],
                add_map[add]
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_OTHER_MODES)
        {
            debugf("Set Other Modes, cycle: ");
            uint64_t cycle = ((command_word >> 52) & 0x3);
            switch (cycle)
            {
                case 0:
                    debugf("1");
                    break;
                case 1:
                    debugf("2");
                    break;
                case 2:
                    debugf("copy");
                    break;
                case 3:
                    debugf("fill");
                    break;
            }

            debugf("\n                             tlut-type: ");
            debugf(((command_word >> 46) & 0x1) ? "IA" : "RGBA");

            debugf(" sample-type: ");
            debugf(((command_word >> 45) & 0x1) ? "2x2" : "point");
            debugf(" ");

            debugf("rgb-dither: ");
            uint64_t dither = ((command_word >> 38) & 0x3);
            switch (dither)
            {
                case 0:
                    debugf("magic");
                    break;
                case 1:
                    debugf("bayer");
                    break;
                case 2:
                    debugf("noise");
                    break;
                case 3:
                    debugf("no");
                    break;
            }

            debugf(" alpha-dither: ");
            dither = ((command_word >> 36) & 0x3);
            switch (dither)
            {
                case 0:
                    debugf("pattern");
                    break;
                case 1:
                    debugf("!pattern");
                    break;
                case 2:
                    debugf("noise");
                    break;
                case 3:
                    debugf("no");
                    break;
            }

            static const char* p_m_map[] =
            {
                "pixelRGB",        // 0
                "memRGB",          // 1
                "blendRGB",        // 2
                "fogRGB",          // 3
            };

            static const char* a_map[] =
            {
                "colorCombineOutAlpha",     // 0
                "fogAlpha",                 // 1
                "shadeAlpha",               // 2
                "0.0",                      // 3
            };

            static const char* b_map[] =
            {
                "(1.0-A)",       // 0
                "memAlpha",         // 1
                "1.0",              // 2
                "0.0",              // 3
            };


            uint64_t p = ((command_word >> 30) & 0x3);
            uint64_t a = ((command_word >> 26) & 0x3);
            uint64_t m = ((command_word >> 22) & 0x3);
            uint64_t b = ((command_word >> 18) & 0x3);
            debugf("\n                             blender1: (%s*%s + %s*%s)", p_m_map[p], a_map[a], p_m_map[m], b_map[b]);

            p = ((command_word >> 28) & 0x3);
            a = ((command_word >> 24) & 0x3);
            m = ((command_word >> 20) & 0x3);
            b = ((command_word >> 16) & 0x3);
            debugf("\n                             blender2: (%s*%s + %s*%s)", p_m_map[p], a_map[a], p_m_map[m], b_map[b]);

            debugf("\n                             z-mode: ");
            uint64_t z_mode = ((command_word >> 10) & 0x3);
            switch (z_mode)
            {
                case 0:
                    debugf("opaque");
                    break;
                case 1:
                    debugf("interpenetrating");
                    break;
                case 2:
                    debugf("transparent");
                    break;
                case 3:
                    debugf("decal");
                    break;
            }

            debugf(" cvg-dst: ");
            uint64_t cvg_dst = ((command_word >> 8) & 0x3);
            switch (cvg_dst)
            {
                case 0:
                    debugf("clamp");
                    break;
                case 1:
                    debugf("wrap");
                    break;
                case 2:
                    debugf("zap");
                    break;
                case 3:
                    debugf("save");
                    break;
            }

            debugf("\n                             flags: [");
            if ((command_word >> 55) & 0x1) debugf("atomic ");
            if ((command_word >> 51) & 0x1) debugf("pers-tex ");
            if ((command_word >> 50) & 0x1) debugf("detail-tex ");
            if ((command_word >> 49) & 0x1) debugf("sharp-tex ");
            if ((command_word >> 48) & 0x1) debugf("lod ");
            if ((command_word >> 47) & 0x1) debugf("tlut ");
            if ((command_word >> 44) & 0x1) debugf("mid-texel ");
            if ((command_word >> 43) & 0x1) debugf("lerp-0 ");
            if ((command_word >> 42) & 0x1) debugf("lerp-1 ");
            if ((command_word >> 41) & 0x1) debugf("convert-one ");
            if ((command_word >> 40) & 0x1) debugf("key ");
            if ((command_word >> 14) & 0x1) debugf("blend ");
            if ((command_word >> 13) & 0x1) debugf("alpha-cvg ");
            if ((command_word >> 12) & 0x1) debugf("cvgXalpha ");
            if ((command_word >> 7) & 0x1) debugf("color-on-cvg ");
            if ((command_word >> 6) & 0x1) debugf("read ");
            if ((command_word >> 5) & 0x1) debugf("z-update ");
            if ((command_word >> 4) & 0x1) debugf("z-compare ");
            if ((command_word >> 3) & 0x1) debugf("AA ");
            // FIXME: which is the primitive Z?
            // if ((command_word >> 2) & 0x1) debugf(" ");
            if ((command_word >> 1) & 0x1) debugf("random-alpha ");
            if ((command_word >> 0) & 0x1) debugf("alpha-compare ");

            debugf("]\n");

            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_ENV_COLOR)
        {
            // FIXME: implement
            debugf("Set Env Color\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_PRIM_COLOR)
        {
            // FIXME: implement
            debugf("Set Prim Color\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_BLEND_COLOR)
        {
            // FIXME: implement
            debugf("Set Blend Color\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_FOG_COLOR)
        {
            // FIXME: implement
            debugf("Set Fog Color\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_FILL_COLOR)
        {
            debugf("Set Fill Color %08llX\n", (command_word >> 0) & 0xFFFFFFFF);
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_PRIM_DEPTH)
        {
            // FIXME: implement
            debugf("Set Prim Depth\n");
            i++;
            continue;
        }


        if (command_id == RDPQ_CMD_SET_SCISSOR)
        {
            debugf("Set Scissor, XH: %llu.%02llu YH: %llu.%02llu f: %1llu o: %1llu XL: %llu.%02llu YL: %llu.%02llu\n",
                (command_word >> 46) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 44) & 0x3),
                (command_word >> 34) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 32) & 0x3),
                (command_word >> 25) & 0x1,
                (command_word >> 24) & 0x1,
                (command_word >> 14) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 12) & 0x3),
                (command_word >> 2) & 0x3FF,
                RDPQ_FRAC_2_DECIMAL((command_word >> 0) & 0x3)
            );
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_CONVERT)
        {
            // FIXME: implement
            debugf("Set Convert\n" );
            i++;
            continue;
        }


        if (command_id == RDPQ_CMD_SET_KEY_R)
        {
            // FIXME: implement
            debugf("Set Key R\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SET_KEY_GB)
        {
            // FIXME: implement
            debugf("Set Key GB\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SYNC_FULL)
        {
            // FIXME: implement
            debugf("Sync Full\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SYNC_LOAD)
        {
            // FIXME: implement
            debugf("Sync Load\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SYNC_PIPE)
        {
            // FIXME: implement
            debugf("Sync Pipe\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_SYNC_TILE)
        {
            // FIXME: implement
            debugf("Sync Tile\n");
            i++;
            continue;
        }

        if (command_id == RDPQ_CMD_NOOP)
        {
            // FIXME: implement
            debugf("NOOP\n");
            i++;
            continue;
        }


        if (command_id & RDPQ_CMD_TRI)
        {
            bool shaded = (command_id >> 2) & 0x1;
            bool textured = (command_id >> 1) & 0x1;
            bool zbuffer = (command_id >> 0) & 0x1;

            if (shaded) printf("Shaded ");
            if (textured) printf("Textured ");
            if (zbuffer) printf("ZBuf ");

            debugf("Triangle, dir: %llu level: %llu tile: %llu YL: %llu.%02llu YM: %llu.%02llu YH: %llu.%02llu\n",
                (command_word >> 55) & 0x1,
                (command_word >> 51) & 0x7,
                (command_word >> 48) & 0x7,
                ((int64_t)command_word << 18 >> 52),
                RDPQ_FRAC_2_DECIMAL((command_word >> 32) & 0x3),
                ((int64_t)command_word << 34 >> 52),
                RDPQ_FRAC_2_DECIMAL((command_word >> 16) & 0x3),
                ((int64_t)command_word << 50 >> 52),
                RDPQ_FRAC_2_DECIMAL((command_word >> 0) & 0x3)
            );

            command_word = ((uint64_t*)start_at)[++i];
            debugf("%08lX  %016llX : ", (uint32_t)start_at + i*8, command_word);
            debugf("XL: %lli.%08llu DxLDy: %lli.%08llu\n",
                ((int64_t)command_word >> 48),
                RDPQ_FRAC_16_DECIMAL((command_word >> 32) & 0xFFFF),
                ((int64_t)command_word << 32 >> 48),
                RDPQ_FRAC_16_DECIMAL((command_word >> 0) & 0xFFFF)
            );

            command_word = ((uint64_t*)start_at)[++i];
            debugf("%08lX  %016llX : ", (uint32_t)start_at + i*8, command_word);
            debugf("XH: %lli.%08llu DxHDy: %lli.%08llu\n",
                ((int64_t)command_word >> 48),
                RDPQ_FRAC_16_DECIMAL((command_word >> 32) & 0xFFFF),
                ((int64_t)command_word << 32 >> 48),
                RDPQ_FRAC_16_DECIMAL((command_word >> 0) & 0xFFFF)
            );

            command_word = ((uint64_t*)start_at)[++i];
            debugf("%08lX  %016llX : ", (uint32_t)start_at + i*8, command_word);
            debugf("XM: %lli.%08llu DxMDy: %lli.%08llu\n",
                ((int64_t)command_word >> 48) & 0xFFFF,
                RDPQ_FRAC_16_DECIMAL((command_word >> 32) & 0xFFFF),
                ((int64_t)command_word << 32 >> 48) & 0xFFFF,
                RDPQ_FRAC_16_DECIMAL((command_word >> 0) & 0xFFFF)
            );

            // FIXME: Implement other coefficients
            if (shaded) i = i + 8;
            if (textured) i = i + 8;
            if (zbuffer) i = i + 2;
            i++;
            continue;
        }

        debugf("Unknown \n");
        i++;
        continue;
    }
}

/* Extern inline instantiations. */
extern inline void rdpq_set_fill_color(color_t color);
extern inline void rdpq_set_color_image(void* dram_ptr, tex_format_t format, uint32_t width, uint32_t height, uint32_t stride);
extern inline void rdpq_fill_triangle(bool flip, int16_t yl, int16_t ym, int16_t yh, int32_t xl, int32_t dxldy, int32_t xh, int32_t dxhdy, int32_t xm, int32_t dxmdy);
