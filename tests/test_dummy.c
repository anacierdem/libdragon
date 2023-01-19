
#define DMA_LENGTH 16
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


    // Write CC
    memset(&data, 0xCC, sizeof(data));
    data_cache_hit_writeback(data, sizeof(data));
    dma_write_raw_async(&data, rom_addr, sizeof(data));
    dma_wait();
    memset(&data, CLEAR_VALUE, sizeof(data));


    // Read back
    memset(&data, CLEAR_VALUE, sizeof(data));
    data_cache_hit_writeback_invalidate(data, sizeof(data));
    dma_read_raw_async(&data, rom_addr, sizeof(data));
    dma_wait();

    memset(&expected, 0xCC, sizeof(expected));
    ASSERT_EQUAL_MEM(expected, data, sizeof(expected), "not equal");
}
