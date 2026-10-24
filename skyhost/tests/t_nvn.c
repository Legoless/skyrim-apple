#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include "vi.h"
#include <assert.h>

typedef void (*set_defaults_fn)(void *);
typedef void (*set_flags_fn)(void *, int);
typedef int (*get_flags_fn)(const void *);
typedef uint32_t (*initialize_fn)(void *, const void *);
typedef void (*finalize_fn)(void *);
typedef void *(*get_proc_fn)(const void *, const char *);

_Static_assert(sizeof(sky_nvn_device_t) <= 0x3000, "device exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_sync_t) <= 0x40, "sync exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_queue_builder_t) <= 0x40, "queue builder exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_queue_t) <= 0x2000, "queue exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_window_builder_t) <= 0x40, "window builder exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_window_t) <= 384, "window exceeds NVN storage");

int main(int argc, char **argv) {
    set_defaults_fn set_defaults = (set_defaults_fn)sky_nvn_bootstrap_loader("nvnDeviceBuilderSetDefaults");
    set_flags_fn set_flags = (set_flags_fn)sky_nvn_bootstrap_loader("nvnDeviceBuilderSetFlags");
    get_flags_fn get_flags = (get_flags_fn)sky_nvn_bootstrap_loader("nvnDeviceBuilderGetFlags");
    initialize_fn initialize = (initialize_fn)sky_nvn_bootstrap_loader("nvnDeviceInitialize");
    finalize_fn finalize = (finalize_fn)sky_nvn_bootstrap_loader("nvnDeviceFinalize");
    get_proc_fn get_proc = (get_proc_fn)sky_nvn_bootstrap_loader("nvnDeviceGetProcAddress");
    assert(set_defaults && set_flags && get_flags && initialize && finalize && get_proc);
    if (argc > 1 && !strcmp(argv[1], "unknown")) {
        void (*unknown)(void) = (void (*)(void))get_proc(NULL, "nvnDeviceGetCurrentTimestampInNanoseconds");
        assert(unknown);
        unknown();
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "textures")) {
        sky_nvn_window_builder_t builder = {0};
        sky_nvn_window_builder_set_textures(&builder, 5, NULL);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "device-info")) {
        void (*boundary)(const void *, int, int *) =
            (void (*)(const void *, int, int *))get_proc(NULL, "nvnDeviceGetInteger");
        int value = 0x12345678;
        assert(boundary);
        boundary(NULL, 0x60, &value);
        return 1;
    }
#define ASSERT_LOOKUP(name, function) assert(get_proc(NULL, name) == (void *)(function))
    ASSERT_LOOKUP("nvnDeviceGetInteger", sky_nvn_device_get_integer);
    ASSERT_LOOKUP("nvnDeviceSetDebugLabel", sky_nvn_device_set_debug_label);
    ASSERT_LOOKUP("nvnDeviceSetIntermediateShaderCache", sky_nvn_device_set_intermediate_shader_cache);
    ASSERT_LOOKUP("nvnDeviceSetWindowOriginMode", sky_nvn_device_set_window_origin_mode);
    ASSERT_LOOKUP("nvnDeviceSetDepthMode", sky_nvn_device_set_depth_mode);
    ASSERT_LOOKUP("nvnDeviceGetWindowOriginMode", sky_nvn_device_get_window_origin_mode);
    ASSERT_LOOKUP("nvnDeviceGetDepthMode", sky_nvn_device_get_depth_mode);
    ASSERT_LOOKUP("nvnDeviceSetAutomaticCheckpointsEnable", sky_nvn_device_set_automatic_checkpoints_enable);
    ASSERT_LOOKUP("nvnDeviceSetAutomaticCheckpointsMask", sky_nvn_device_set_automatic_checkpoints_mask);
    ASSERT_LOOKUP("nvnDeviceSetAutomaticCheckpointsSamplingInterval", sky_nvn_device_set_automatic_checkpoints_sampling_interval);
    ASSERT_LOOKUP("nvnDeviceSetAutomaticCheckpointsByDebugGroup", sky_nvn_device_set_automatic_checkpoints_by_debug_group);
    ASSERT_LOOKUP("nvnDeviceSetCheckpointCallStacksEnable", sky_nvn_device_set_checkpoint_call_stacks_enable);
    ASSERT_LOOKUP("nvnDeviceSetCheckpointMaxCallStacksDepth", sky_nvn_device_set_checkpoint_max_call_stacks_depth);
    ASSERT_LOOKUP("nvnSyncInitialize", sky_nvn_sync_initialize);
    ASSERT_LOOKUP("nvnSyncFinalize", sky_nvn_sync_finalize);
    ASSERT_LOOKUP("nvnMemoryPoolBuilderSetDefaults", sky_nvn_memory_pool_builder_set_defaults);
    ASSERT_LOOKUP("nvnTexturePoolInitialize", sky_nvn_texture_pool_initialize);
    ASSERT_LOOKUP("nvnQueueBuilderSetDefaults", sky_nvn_queue_builder_set_defaults);
    ASSERT_LOOKUP("nvnQueueBuilderSetDevice", sky_nvn_queue_builder_set_device);
    ASSERT_LOOKUP("nvnQueueBuilderSetFlags", sky_nvn_queue_builder_set_flags);
    ASSERT_LOOKUP("nvnQueueBuilderSetCommandMemorySize", sky_nvn_queue_builder_set_command_memory_size);
    ASSERT_LOOKUP("nvnQueueBuilderSetComputeMemorySize", sky_nvn_queue_builder_set_compute_memory_size);
    ASSERT_LOOKUP("nvnQueueBuilderSetControlMemorySize", sky_nvn_queue_builder_set_control_memory_size);
    ASSERT_LOOKUP("nvnQueueBuilderSetCommandFlushThreshold", sky_nvn_queue_builder_set_command_flush_threshold);
    ASSERT_LOOKUP("nvnQueueBuilderGetQueueMemorySize", sky_nvn_queue_builder_get_queue_memory_size);
    ASSERT_LOOKUP("nvnQueueBuilderSetQueueMemory", sky_nvn_queue_builder_set_queue_memory);
    ASSERT_LOOKUP("nvnQueueInitialize", sky_nvn_queue_initialize);
    ASSERT_LOOKUP("nvnQueueFinalize", sky_nvn_queue_finalize);
    ASSERT_LOOKUP("nvnWindowBuilderSetDefaults", sky_nvn_window_builder_set_defaults);
    ASSERT_LOOKUP("nvnWindowBuilderSetDevice", sky_nvn_window_builder_set_device);
    ASSERT_LOOKUP("nvnWindowBuilderSetNativeWindow", sky_nvn_window_builder_set_native_window);
    ASSERT_LOOKUP("nvnWindowBuilderSetTextures", sky_nvn_window_builder_set_textures);
    ASSERT_LOOKUP("nvnWindowBuilderSetPresentInterval", sky_nvn_window_builder_set_present_interval);
    ASSERT_LOOKUP("nvnWindowBuilderSetNumActiveTextures", sky_nvn_window_builder_set_num_active_textures);
    ASSERT_LOOKUP("nvnWindowBuilderGetDevice", sky_nvn_window_builder_get_device);
    ASSERT_LOOKUP("nvnWindowBuilderGetNumTextures", sky_nvn_window_builder_get_num_textures);
    ASSERT_LOOKUP("nvnWindowBuilderGetTexture", sky_nvn_window_builder_get_texture);
    ASSERT_LOOKUP("nvnWindowBuilderGetNativeWindow", sky_nvn_window_builder_get_native_window);
    ASSERT_LOOKUP("nvnWindowBuilderGetPresentInterval", sky_nvn_window_builder_get_present_interval);
    ASSERT_LOOKUP("nvnWindowBuilderGetNumActiveTextures", sky_nvn_window_builder_get_num_active_textures);
    ASSERT_LOOKUP("nvnWindowInitialize", sky_nvn_window_initialize);
    ASSERT_LOOKUP("nvnWindowFinalize", sky_nvn_window_finalize);
    ASSERT_LOOKUP("nvnWindowSetPresentInterval", sky_nvn_window_set_present_interval);
    ASSERT_LOOKUP("nvnWindowSetNumActiveTextures", sky_nvn_window_set_num_active_textures);
    ASSERT_LOOKUP("nvnWindowSetCrop", sky_nvn_window_set_crop);
    ASSERT_LOOKUP("nvnWindowGetCrop", sky_nvn_window_get_crop);
    ASSERT_LOOKUP("nvnWindowGetNativeWindow", sky_nvn_window_get_native_window);
    ASSERT_LOOKUP("nvnWindowGetNumTextures", sky_nvn_window_get_num_textures);
    ASSERT_LOOKUP("nvnWindowGetNumActiveTextures", sky_nvn_window_get_num_active_textures);
    ASSERT_LOOKUP("nvnWindowGetPresentInterval", sky_nvn_window_get_present_interval);

    sky_nvn_device_builder_t builder;
    set_defaults(&builder);
    assert(builder.flags == 0);
    set_flags(&builder, 0x12);
    assert(get_flags(&builder) == 0x12);
    sky_nvn_device_t device;
    memset(&device, 0xa5, sizeof device);
    assert(initialize(&device, &builder) == 1);
    assert(device.magic == 0x44455630 && device.flags == 0x12);
    assert(device.debug_label == NULL && device.intermediate_shader_cache == 0);
    assert(device.origin_mode == 0 && device.depth_mode == 0);
    assert(device.automatic_checkpoints == 0 && device.automatic_checkpoints_mask == 0);
    assert(device.automatic_checkpoints_interval == 0 && device.automatic_checkpoints_by_debug_group == 0);
    assert(device.checkpoint_call_stacks == 0 && device.checkpoint_max_call_stacks_depth == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nvn_device_initialize"));
    assert(get_proc(NULL, "nvnDeviceInitialize") == (void *)initialize);

    sky_nvn_device_set_debug_label(&device, "renderer");
    sky_nvn_device_set_intermediate_shader_cache(&device, 1);
    sky_nvn_device_set_window_origin_mode(&device, 1);
    sky_nvn_device_set_depth_mode(&device, 1);
    sky_nvn_device_set_automatic_checkpoints_enable(&device, 1);
    sky_nvn_device_set_automatic_checkpoints_mask(&device, 0xfedcba9876543210ull);
    sky_nvn_device_set_automatic_checkpoints_sampling_interval(&device, 60);
    sky_nvn_device_set_automatic_checkpoints_by_debug_group(&device, 7);
    sky_nvn_device_set_checkpoint_call_stacks_enable(&device, 1);
    sky_nvn_device_set_checkpoint_max_call_stacks_depth(&device, 31);
    assert(!strcmp(device.debug_label, "renderer"));
    assert(device.intermediate_shader_cache == 1);
    assert(sky_nvn_device_get_window_origin_mode(&device) == 1);
    assert(sky_nvn_device_get_depth_mode(&device) == 1);
    assert(device.automatic_checkpoints == 1);
    assert(device.automatic_checkpoints_mask == 0xfedcba9876543210ull);
    assert(device.automatic_checkpoints_interval == 60);
    assert(device.automatic_checkpoints_by_debug_group == 7);
    assert(device.checkpoint_call_stacks == 1);
    assert(device.checkpoint_max_call_stacks_depth == 31);
    static const struct { int item, value; } expected_device_info[] = {
        {0x54, 16384}, {0x51, 65536}, {0x0f, 32}, {0x24, 1048576},
        {0x10, 32}, {0x12, 256}, {0x25, 4096}, {0x39, 128},
    };
    for (unsigned index = 0; index < sizeof expected_device_info / sizeof expected_device_info[0]; index++) {
        int device_info = 0;
        sky_nvn_device_get_integer(&device, expected_device_info[index].item, &device_info);
        assert(device_info == expected_device_info[index].value);
    }

    sky_nvn_sync_t sync;
    memset(&sync, 0xa5, sizeof sync);
    assert(sky_nvn_sync_initialize(&sync, &device) == 1);
    assert(sync.magic == 0x434e5953 && sync.dev == &device);
    assert(sync.fenced == 0 && sync.fence == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nvn_sync_initialize"));

    sky_nvn_queue_builder_t queue_builder = {0};
    sky_nvn_queue_builder_set_device(&queue_builder, &device);
    sky_nvn_queue_builder_set_defaults(&queue_builder);
    assert(queue_builder.dev == &device && queue_builder.flags == 0);
    assert(queue_builder.command_size == 65536);
    assert(queue_builder.compute_size == 262144);
    assert(queue_builder.control_size == 16384);
    assert(queue_builder.flush_threshold == 4096);
    assert(queue_builder.memory == NULL && queue_builder.memory_size == 0);
    assert(sky_nvn_queue_builder_get_queue_memory_size(&queue_builder) == 344064);
    sky_nvn_queue_builder_set_flags(&queue_builder, 3);
    sky_nvn_queue_builder_set_command_memory_size(&queue_builder, 4097);
    sky_nvn_queue_builder_set_compute_memory_size(&queue_builder, 3);
    sky_nvn_queue_builder_set_control_memory_size(&queue_builder, 4093);
    sky_nvn_queue_builder_set_command_flush_threshold(&queue_builder, 8192);
    uint8_t queue_memory[12288] = {0};
    uint64_t queue_memory_size = sky_nvn_queue_builder_get_queue_memory_size(&queue_builder);
    assert(queue_memory_size == sizeof queue_memory);
    sky_nvn_queue_t queue;
    sky_nvn_queue_builder_set_queue_memory(&queue_builder, queue_memory, queue_memory_size - 1);
    assert(sky_nvn_queue_initialize(&queue, &queue_builder) == 0);
    assert(queue.magic == 0 && sky_mtl_command_queue_active_count() == 0);
    assert(sky_mtl_memory_pool_active_count() == 0);
    sky_nvn_queue_builder_set_queue_memory(&queue_builder, queue_memory, queue_memory_size);
    memset(&queue, 0xa5, sizeof queue);
    assert(sky_nvn_queue_initialize(&queue, &queue_builder) == 1);
    assert(queue.magic == 0x55455551 && queue.dev == &device && queue.flags == 3);
    assert(queue.submits == 0);
    assert(queue.command_size == 4097 && queue.compute_size == 3 && queue.control_size == 4093);
    assert(queue.flush_threshold == 8192);
    assert(queue.memory == queue_memory && queue.memory_size == sizeof queue_memory);
    assert(queue.metal != NULL && queue.metal_memory != NULL);
    assert(sky_mtl_command_queue_active_count() == 1);
    assert(sky_mtl_memory_pool_active_count() == 1);
    assert(sky_mtl_memory_pool_is_shared(queue.metal_memory) == 1);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nvn_queue_initialize"));

    sky_vi_initialize();
    sky_vi_display_t *display = NULL;
    sky_vi_layer_t *layer = NULL;
    sky_vi_native_window_t *native = NULL;
    assert(sky_vi_open_default_display(&display) == 0);
    assert(sky_vi_create_layer(&layer, display) == 0);
    assert(sky_vi_get_native_window(&native, layer) == 0);
    assert(native != NULL && native->layer == layer);

    sky_nvn_window_builder_t window_builder = {0};
    sky_nvn_window_builder_set_device(&window_builder, &device);
    sky_nvn_window_builder_set_defaults(&window_builder);
    assert(sky_nvn_window_builder_get_device(&window_builder) == &device);
    assert(sky_nvn_window_builder_get_native_window(&window_builder) == NULL);
    assert(sky_nvn_window_builder_get_num_textures(&window_builder) == 0);
    assert(sky_nvn_window_builder_get_num_active_textures(&window_builder) == 0);
    assert(sky_nvn_window_builder_get_present_interval(&window_builder) == 1);
    sky_nvn_window_t window;
    assert(sky_nvn_window_initialize(&window, &window_builder) == 0);
    assert(window.magic == 0);
    sky_nvn_window_builder_set_native_window(&window_builder, native);
    sky_nvn_window_builder_set_present_interval(&window_builder, 2);
    sky_nvn_window_builder_set_num_active_textures(&window_builder, 3);
    assert(sky_nvn_window_builder_get_num_active_textures(&window_builder) == 3);
    sky_nvn_window_builder_set_num_active_textures(&window_builder, 0);
    sky_nvn_window_builder_set_textures(&window_builder, 0, NULL);
    assert(sky_nvn_window_builder_get_native_window(&window_builder) == native);
    assert(sky_nvn_window_builder_get_present_interval(&window_builder) == 2);
    assert(sky_nvn_window_builder_get_num_textures(&window_builder) == 0);
    assert(sky_nvn_window_builder_get_texture(&window_builder, 0) == NULL);
    assert(sky_nvn_window_builder_get_texture(&window_builder, -1) == NULL);
    assert(sky_nvn_window_builder_get_texture(&window_builder, 4) == NULL);
    memset(&window, 0xa5, sizeof window);
    assert(sky_nvn_window_initialize(&window, &window_builder) == 1);
    assert(window.magic == 0x444e4957 && window.dev == &device && window.native == native);
    assert(window.texture_count == 0 && window.active_texture_count == 0 && window.present_interval == 2);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nvn_window_initialize"));
    assert(sky_nvn_window_get_native_window(&window) == native);
    assert(sky_nvn_window_get_num_textures(&window) == 0);
    assert(sky_nvn_window_get_num_active_textures(&window) == 0);
    assert(sky_nvn_window_get_present_interval(&window) == 2);
    sky_nvn_window_set_present_interval(&window, 1);
    sky_nvn_window_set_num_active_textures(&window, 0);
    sky_nvn_window_set_crop(&window, 3, 5, 1280, 720);
    sky_nvn_rectangle_t crop = {0};
    sky_nvn_window_get_crop(&window, &crop);
    assert(sky_nvn_window_get_present_interval(&window) == 1);
    assert(crop.x == 3 && crop.y == 5 && crop.width == 1280 && crop.height == 720);

    sky_nvn_window_finalize(&window);
    assert(window.magic == 0);
    sky_nvn_queue_finalize(&queue);
    assert(queue.magic == 0 && queue.metal == NULL && queue.metal_memory == NULL);
    assert(sky_mtl_command_queue_active_count() == 0);
    assert(sky_mtl_memory_pool_active_count() == 0);
    sky_nvn_sync_finalize(&sync);
    assert(sync.magic == 0);
    finalize(&device);
    assert(device.magic == 0);
    puts("T_NVN OK bootstrap-device-debug-sync-queue-window-named-traps");
    return 0;
}
