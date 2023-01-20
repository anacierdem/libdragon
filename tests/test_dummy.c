
#define DMA_LENGTH 4096
#define TEST_SIZE (10 * 1024 * 1024) // 10MiB write
#define CLEAR_VALUE 0xAA

void test_rom_write(TestContext *ctx)
{
    uint32_t rom_addr = dfs_rom_addr("counter.dat");
    ASSERT(rom_addr != 0, "counter.dat not found by dfs_rom_addr");

    uint8_t data[DMA_LENGTH] __attribute__((aligned(16)));
    memset(&data, CLEAR_VALUE, sizeof(data));

    data_cache_hit_writeback_invalidate(data, sizeof(data));
    dma_read_raw_async(data, rom_addr, sizeof(data));
    dma_wait();


    uint8_t expected[DMA_LENGTH] = {0, 1, 2, 3};
    ASSERT_EQUAL_MEM(expected, data, 4, "initial read error");

    // Write CC to flash cart
    memset(&data, 0xCC, sizeof(data));
    data_cache_hit_writeback(data, sizeof(data));
    dma_write_raw_async(&data, rom_addr, sizeof(data));
    dma_wait();

    // Read back
    memset(&data, CLEAR_VALUE, sizeof(data));
    data_cache_hit_writeback_invalidate(data, sizeof(data));
    dma_read_raw_async(&data, rom_addr, sizeof(data));
    dma_wait();

    memset(&expected, 0xCC, sizeof(expected));
    ASSERT_EQUAL_MEM(expected, data, sizeof(expected), "not equal after write/read");

    // Start benchmark - serial DMA_LENGTH pure write
    uint32_t start_time = get_ticks_ms();
    for(int i = 0; i < (TEST_SIZE/DMA_LENGTH); i++) {
        dma_write_raw_async(&data, rom_addr, sizeof(data));
        dma_wait();
    }
    float time_taken = get_ticks_ms() - start_time;

    debugf("%d bytes serial write speed: %.2f MiB/sec \n", DMA_LENGTH, (TEST_SIZE / (1024 * 1024)) / (time_taken / 1000.0f));

    start_time = get_ticks_ms();
    // Start benchmark - serial DMA_LENGTH pure read
    for(int i = 0; i < (TEST_SIZE/DMA_LENGTH); i++) {
        dma_read_raw_async(&data, rom_addr, sizeof(data));
        dma_wait();
    }
    time_taken = get_ticks_ms() - start_time;
    debugf("%d bytes serial read speed: %.2f MiB/sec  \n", DMA_LENGTH, (TEST_SIZE / (1024 * 1024)) / (time_taken / 1000.0f));
}
