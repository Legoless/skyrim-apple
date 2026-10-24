#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include <assert.h>
#include <unistd.h>

#define ASSERT_LOOKUP(name, function) \
    assert(sky_nvn_bootstrap_loader(name) == (void *)(function))

int main(void) {
    ASSERT_LOOKUP("nvnQueueFenceSync", sky_nvn_queue_fence_sync);
    ASSERT_LOOKUP("nvnQueueFlush", sky_nvn_queue_flush);
    ASSERT_LOOKUP("nvnQueueFinish", sky_nvn_queue_finish);
    ASSERT_LOOKUP("nvnSyncWait", sky_nvn_sync_wait);

    sky_nvn_device_builder_t device_builder = {0};
    sky_nvn_device_t device;
    assert(sky_nvn_device_initialize(&device, &device_builder) == 1);
    size_t page = (size_t)getpagesize();
    sky_nvn_queue_builder_t builder = {0};
    sky_nvn_queue_builder_set_device(&builder, &device);
    sky_nvn_queue_builder_set_defaults(&builder);
    void *memory = NULL;
    uint64_t memory_size = sky_nvn_queue_builder_get_queue_memory_size(&builder);
    assert(posix_memalign(&memory, page, memory_size) == 0);
    sky_nvn_queue_builder_set_queue_memory(&builder, memory, memory_size);
    sky_nvn_queue_t queue;
    assert(sky_nvn_queue_initialize(&queue, &builder) == 1);
    assert(queue.submits == 0 && queue.completed == 0);

    sky_nvn_sync_t sync;
    assert(sky_nvn_sync_initialize(&sync, &device) == 1);
    sky_nvn_queue_fence_sync(&queue, &sync, 0, 0);
    assert(sync.fenced == 1 && sync.fence == 0 && sync.queue == &queue);
    assert(sky_nvn_sync_wait(&sync, 0) == 0);
    sky_nvn_queue_flush(&queue);
    assert(queue.submits == 0 && queue.completed == 0);
    sky_nvn_sync_finalize(&sync);
    assert(sky_nvn_sync_wait(&sync, 0) == 3);

    assert(sky_nvn_sync_initialize(&sync, &device) == 1);
    assert(sky_nvn_queue_submit_empty_for_testing(&queue, 200000000ull) == 1);
    assert(queue.submits == 1 && queue.completed == 0);
    sky_nvn_queue_fence_sync(&queue, &sync, 0, 0);
    assert(sync.fence == 1);
    assert(sky_nvn_sync_wait(&sync, 0) == 2);
    sky_nvn_queue_flush(&queue);
    assert(queue.completed == 0);
    assert(sky_nvn_sync_wait(&sync, 20000000ull) == 2);
    assert(sky_nvn_sync_wait(&sync, 500000000ull) == 1);
    assert(queue.completed == 1);
    sky_nvn_sync_finalize(&sync);
    sky_nvn_queue_finish(&queue);
    assert(queue.completed == queue.submits);

    assert(sky_nvn_queue_submit_empty_for_testing(&queue, 50000000ull) == 1);
    sky_nvn_queue_finish(&queue);
    assert(queue.completed == queue.submits);

    assert(sky_nvn_queue_submit_empty_for_testing(&queue, 50000000ull) == 1);
    sky_nvn_queue_finalize(&queue);
    assert(sky_mtl_command_queue_active_count() == 0);
    assert(sky_mtl_memory_pool_active_count() == 0);
    free(memory);
    sky_nvn_device_finalize(&device);
    puts("T_NVN_SYNC_OK idle-outstanding-timeout-wake-cleanup");
    return 0;
}
