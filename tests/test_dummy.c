
#include <stdint.h>


#define DMA_LENGTH 4096
#define TEST_SIZE (10 * 1024 * 1024) // 10MiB write
#define CLEAR_VALUE 0xAA
#define TEST_VALUE 0x55

static volatile struct PI_regs_s {
    /** @brief Uncached address in RAM where data should be found */
    volatile void * ram_address;
    /** @brief Address of data on peripheral */
    uint32_t pi_address;
    /** @brief How much data to read from RAM into the peripheral */
    uint32_t read_length;
    /** @brief How much data to write to RAM from the peripheral */
    uint32_t write_length;
    /** @brief Status of the PI, including DMA busy */
    uint32_t status;
    /** @brief Cartridge domain 1 latency in RCP clock cycles. Requires DMA status bit guards to work reliably */
    uint32_t dom1_latency;
    /** @brief Cartridge domain 1 pulse width in RCP clock cycles. Requires DMA status bit guards to work reliably */
    uint32_t dom1_pulse_width;
    // TODO: add remaining registers
} * const PI_regs = (struct PI_regs_s *)0xa4600000;

static void benchmark_pi(uint8_t *data, uint32_t rom_addr, size_t size) {
    // Start benchmark - serial DMA_LENGTH pure write
    uint32_t start_time = get_ticks_ms();
    for(int i = 0; i < (TEST_SIZE/DMA_LENGTH); i++) {
        dma_write_raw_async(&data, rom_addr, size);
        dma_wait();
    }
    float time_taken = get_ticks_ms() - start_time;

    debugf("%d bytes serial write speed: %.2f MiB/sec \n", DMA_LENGTH, (TEST_SIZE / (1024 * 1024)) / (time_taken / 1000.0f));

    start_time = get_ticks_ms();
    // Start benchmark - serial DMA_LENGTH pure read
    for(int i = 0; i < (TEST_SIZE/DMA_LENGTH); i++) {
        dma_read_raw_async(&data, rom_addr, size);
        dma_wait();
    }
    time_taken = get_ticks_ms() - start_time;
    debugf("%d bytes serial read speed: %.2f MiB/sec  \n", DMA_LENGTH, (TEST_SIZE / (1024 * 1024)) / (time_taken / 1000.0f));
}

void test_rom_write(TestContext *ctx)
{
    uint32_t rom_addr = dfs_rom_addr("counter.dat");
    ASSERT(rom_addr != 0, "counter.dat not found by dfs_rom_addr");

    uint8_t data[DMA_LENGTH] __attribute__((aligned(16)));
    memset(&data, CLEAR_VALUE, sizeof(data));

    // Sanity check to make sure we can read
    data_cache_hit_writeback_invalidate(data, sizeof(data));
    dma_read_raw_async(data, rom_addr, sizeof(data));
    dma_wait();

    uint8_t expected[DMA_LENGTH] = {0, 1, 2, 3};
    ASSERT_EQUAL_MEM(expected, data, 4, "initial read error");


    benchmark_pi(data, rom_addr, sizeof(data));

    uint32_t old_pw = PI_regs->dom1_pulse_width;
    uint32_t old_lat = PI_regs->dom1_latency;

    // Overclock the PI - My cart seem to go down to 0x03 with no issues.
    // Will need to experiment with this with other ed64s when the time comes.
    // Looks like we can easily hit 10+ MiB/sec which should work fine for a
    // page file.
    // I don't know if this is limited by the inserted sd (technically it is
    // but ed64 can already have its own limit irrespective of that)
    io_write((uint32_t)&PI_regs->dom1_pulse_width, 0x03);
    // This have very little effect on performance as expected
    io_write((uint32_t)&PI_regs->dom1_latency, 0x00);

    benchmark_pi(data, rom_addr, sizeof(data));

    // Verify we can keep data integrity with the overclock

    // Write 0x55 to flash cart
    memset(&data, TEST_VALUE, sizeof(data));
    data_cache_hit_writeback(data, sizeof(data));
    dma_write_raw_async(&data, rom_addr, sizeof(data));
    dma_wait();

    // Read back
    memset(&data, CLEAR_VALUE, sizeof(data));
    data_cache_hit_writeback_invalidate(data, sizeof(data));
    dma_read_raw_async(&data, rom_addr, sizeof(data));
    dma_wait();

    memset(&expected, TEST_VALUE, sizeof(expected));
    ASSERT_EQUAL_MEM(expected, data, sizeof(expected), "not equal after write/read");

    // Restore clocks
    io_write((uint32_t)&PI_regs->dom1_pulse_width, old_pw);
    io_write((uint32_t)&PI_regs->dom1_latency, old_lat);
}
