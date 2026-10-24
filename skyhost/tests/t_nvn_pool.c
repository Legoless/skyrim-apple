#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include <assert.h>
#include <unistd.h>

_Static_assert(sizeof(sky_nvn_memory_pool_builder_t) <= 0x40, "memory pool builder exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_memory_pool_t) <= 0x100, "memory pool exceeds NVN storage");

#define ASSERT_LOOKUP(name, function) \
    assert(sky_nvn_bootstrap_loader(name) == (void *)(function))

int main(void) {
    ASSERT_LOOKUP("nvnMemoryPoolBuilderSetDefaults", sky_nvn_memory_pool_builder_set_defaults);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderSetDevice", sky_nvn_memory_pool_builder_set_device);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderSetStorage", sky_nvn_memory_pool_builder_set_storage);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderSetFlags", sky_nvn_memory_pool_builder_set_flags);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderGetDevice", sky_nvn_memory_pool_builder_get_device);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderGetMemory", sky_nvn_memory_pool_builder_get_memory);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderGetSize", sky_nvn_memory_pool_builder_get_size);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderGetFlags", sky_nvn_memory_pool_builder_get_flags);
    ASSERT_LOOKUP("nvnMemoryPoolInitialize", sky_nvn_memory_pool_initialize);
    ASSERT_LOOKUP("nvnMemoryPoolSetDebugLabel", sky_nvn_memory_pool_set_debug_label);
    ASSERT_LOOKUP("nvnMemoryPoolFinalize", sky_nvn_memory_pool_finalize);
    ASSERT_LOOKUP("nvnMemoryPoolMap", sky_nvn_memory_pool_map);
    ASSERT_LOOKUP("nvnMemoryPoolFlushMappedRange", sky_nvn_memory_pool_flush_mapped_range);
    ASSERT_LOOKUP("nvnMemoryPoolInvalidateMappedRange", sky_nvn_memory_pool_invalidate_mapped_range);
    ASSERT_LOOKUP("nvnMemoryPoolGetBufferAddress", sky_nvn_memory_pool_get_buffer_address);
    ASSERT_LOOKUP("nvnMemoryPoolGetSize", sky_nvn_memory_pool_get_size);
    ASSERT_LOOKUP("nvnMemoryPoolGetFlags", sky_nvn_memory_pool_get_flags);
    ASSERT_LOOKUP("nvnBufferBuilderSetDevice", sky_nvn_buffer_builder_set_device);
    ASSERT_LOOKUP("nvnBufferBuilderSetDefaults", sky_nvn_buffer_builder_set_defaults);
    ASSERT_LOOKUP("nvnBufferBuilderSetStorage", sky_nvn_buffer_builder_set_storage);
    ASSERT_LOOKUP("nvnBufferBuilderGetSize", sky_nvn_buffer_builder_get_size);
    ASSERT_LOOKUP("nvnBufferInitialize", sky_nvn_buffer_initialize);
    ASSERT_LOOKUP("nvnBufferFinalize", sky_nvn_buffer_finalize);
    ASSERT_LOOKUP("nvnBufferMap", sky_nvn_buffer_map);
    ASSERT_LOOKUP("nvnBufferGetAddress", sky_nvn_buffer_get_address);

    sky_nvn_device_builder_t device_builder = {0};
    sky_nvn_device_t device;
    assert(sky_nvn_device_initialize(&device, &device_builder) == 1);
    size_t page = (size_t)getpagesize();
    void *allocation = NULL;
    assert(posix_memalign(&allocation, page, 4 * page) == 0);
    uint8_t *memory = (uint8_t *)allocation + 123;
    uint64_t size = 2 * page + 17;

    sky_nvn_memory_pool_builder_t builder;
    memset(&builder, 0xa5, sizeof builder);
    builder.dev = &device;
    sky_nvn_memory_pool_builder_set_defaults(&builder);
    assert(builder.dev == NULL && builder.mem == NULL && builder.size == 0 && builder.flags == 0);
    sky_nvn_memory_pool_builder_set_device(&builder, &device);
    sky_nvn_memory_pool_builder_set_storage(&builder, memory, size);
    sky_nvn_memory_pool_builder_set_flags(&builder, 0x22);
    assert(sky_nvn_memory_pool_builder_get_device(&builder) == &device);
    assert(sky_nvn_memory_pool_builder_get_memory(&builder) == memory);
    assert(sky_nvn_memory_pool_builder_get_size(&builder) == size);
    assert(sky_nvn_memory_pool_builder_get_flags(&builder) == 0x22);

    sky_nvn_memory_pool_t pool;
    memset(&pool, 0xa5, sizeof pool);
    sky_nvn_memory_pool_builder_set_storage(&builder, NULL, size);
    assert(sky_nvn_memory_pool_initialize(&pool, &builder) == 0);
    assert(pool.magic == 0 && sky_mtl_memory_pool_active_count() == 0);
    sky_nvn_memory_pool_builder_set_storage(&builder, memory, 0);
    assert(sky_nvn_memory_pool_initialize(&pool, &builder) == 0);
    assert(pool.magic == 0 && sky_mtl_memory_pool_active_count() == 0);
    sky_nvn_memory_pool_builder_set_storage(&builder, memory, size);
    sky_nvn_memory_pool_builder_set_flags(&builder, SKY_NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    assert(sky_nvn_memory_pool_initialize(&pool, &builder) == 0);
    assert(pool.magic == 0 && sky_mtl_memory_pool_active_count() == 0);
    sky_nvn_memory_pool_builder_set_flags(&builder, 0x22);
    sky_nvn_memory_pool_builder_set_device(&builder, NULL);
    assert(sky_nvn_memory_pool_initialize(&pool, &builder) == 0);
    assert(pool.magic == 0 && sky_mtl_memory_pool_active_count() == 0);
    sky_nvn_memory_pool_builder_set_device(&builder, &device);

    assert(sky_nvn_memory_pool_initialize(&pool, &builder) == 1);
    assert(pool.magic == 0x4c4f4f50 && pool.dev == &device);
    assert(pool.mem == memory && pool.size == size && pool.flags == 0x22);
    assert(pool.metal != NULL && sky_mtl_memory_pool_active_count() == 1);
    assert(sky_mtl_memory_pool_is_shared(pool.metal) == 1);
    assert(sky_mtl_memory_pool_wrapped_size(pool.metal) == 3 * page);
    assert(sky_mtl_memory_pool_contents(pool.metal) == memory);
    assert(pool.buffer_address != 0);
    assert(pool.buffer_address == sky_mtl_memory_pool_gpu_address(pool.metal));
    assert(sky_nvn_memory_pool_map(&pool) == memory);
    assert(sky_nvn_memory_pool_get_buffer_address(&pool) == pool.buffer_address);
    assert(sky_nvn_memory_pool_get_size(&pool) == size);
    assert(sky_nvn_memory_pool_get_flags(&pool) == 0x22);

    memory[0] = 0x5a;
    memory[size - 1] = 0xa5;
    uint8_t *shared = sky_mtl_memory_pool_contents(pool.metal);
    assert(shared[0] == 0x5a && shared[size - 1] == 0xa5);
    shared[1] = 0x33;
    assert(memory[1] == 0x33);
    sky_nvn_memory_pool_set_debug_label(&pool, "pool-test");
    assert(!strcmp(pool.debug_label, "pool-test"));

    sky_nvn_memory_pool_flush_mapped_range(&pool, 1, 17);
    assert(pool.flush_count == 1 && pool.last_range_offset == 1 && pool.last_range_size == 17);
    sky_nvn_memory_pool_invalidate_mapped_range(&pool, 0, size);
    assert(pool.invalidate_count == 1 && pool.last_range_offset == 0 && pool.last_range_size == size);
    sky_nvn_memory_pool_flush_mapped_range(&pool, -1, 1);
    sky_nvn_memory_pool_flush_mapped_range(&pool, (int64_t)size - 1, 2);
    sky_nvn_memory_pool_invalidate_mapped_range(&pool, 0, size + 1);
    assert(pool.flush_count == 1 && pool.invalidate_count == 1);

    void *no_cpu_allocation = NULL;
    assert(posix_memalign(&no_cpu_allocation, page, page) == 0);
    sky_nvn_memory_pool_builder_set_storage(&builder, no_cpu_allocation, page);
    sky_nvn_memory_pool_builder_set_flags(&builder, 0xa1);
    sky_nvn_memory_pool_t no_cpu;
    assert(sky_nvn_memory_pool_initialize(&no_cpu, &builder) == 1);
    assert(sky_mtl_memory_pool_active_count() == 2);
    assert(sky_nvn_memory_pool_map(&no_cpu) == NULL);
    sky_nvn_memory_pool_flush_mapped_range(&no_cpu, 0, 1);
    assert(no_cpu.flush_count == 0);
    sky_nvn_memory_pool_finalize(&no_cpu);
    assert(no_cpu.magic == 0 && no_cpu.metal == NULL);
    assert(sky_mtl_memory_pool_active_count() == 1);
    free(no_cpu_allocation);

    sky_nvn_buffer_builder_t buffer_builder = {0};
    sky_nvn_buffer_builder_set_device(&buffer_builder, &device);
    sky_nvn_buffer_builder_set_defaults(&buffer_builder);
    sky_nvn_buffer_builder_set_storage(&buffer_builder, &pool, -1, 128);
    assert(sky_nvn_buffer_builder_get_size(&buffer_builder) == 128);
    sky_nvn_buffer_t buffer;
    assert(sky_nvn_buffer_initialize(&buffer, &buffer_builder) == 0);
    sky_nvn_buffer_builder_set_storage(&buffer_builder, &pool, 64, 128);
    assert(sky_nvn_buffer_initialize(&buffer, &buffer_builder) == 1);
    assert(buffer.dev == &device && buffer.pool == &pool && buffer.offset == 64 && buffer.size == 128);
    assert(sky_nvn_buffer_map(&buffer) == memory + 64);
    ((uint8_t *)sky_nvn_buffer_map(&buffer))[0] = 0x6c;
    assert(memory[64] == 0x6c);
    assert(sky_nvn_buffer_get_address(&buffer) == pool.buffer_address + 64);
    sky_nvn_buffer_finalize(&buffer);
    assert(buffer.magic == 0 && sky_nvn_buffer_map(&buffer) == NULL);
    assert(sky_nvn_buffer_get_address(&buffer) == 0);

    sky_nvn_memory_pool_finalize(&pool);
    assert(pool.magic == 0 && pool.metal == NULL);
    assert(sky_nvn_memory_pool_map(&pool) == NULL);
    assert(sky_nvn_memory_pool_get_buffer_address(&pool) == 0);
    assert(sky_nvn_memory_pool_get_size(&pool) == 0);
    assert(sky_nvn_memory_pool_get_flags(&pool) == 0);
    assert(sky_mtl_memory_pool_active_count() == 0);
    assert(memory[0] == 0x5a && memory[1] == 0x33 && memory[size - 1] == 0xa5);
    sky_nvn_memory_pool_finalize(&pool);
    assert(sky_mtl_memory_pool_active_count() == 0);
    free(allocation);
    sky_nvn_device_finalize(&device);
    puts("T_NVN_POOL_OK shared-zero-copy-validation-map-address-ranges-cleanup");
    return 0;
}
