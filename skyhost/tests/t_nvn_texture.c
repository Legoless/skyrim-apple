#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include "vi.h"
#include "nvn/nvn_deswizzle.h"
#include <assert.h>
#include <unistd.h>

_Static_assert(sizeof(sky_nvn_texture_builder_t) <= 0x80, "texture builder exceeds Skyrim storage");
_Static_assert(sizeof(sky_nvn_texture_t) <= 0x80, "texture exceeds Skyrim storage");

#define ASSERT_LOOKUP(name, function) \
    assert(sky_nvn_bootstrap_loader(name) == (void *)(function))

static void setup(sky_nvn_device_t *device, sky_nvn_memory_pool_t *memory, void **allocation) {
    sky_nvn_device_builder_t device_builder = {0};
    assert(sky_nvn_device_initialize(device, &device_builder) == 1);
    assert(posix_memalign(allocation, (size_t)getpagesize(), 16 << 20) == 0);
    memset(*allocation, 0xa5, 16 << 20);
    sky_nvn_memory_pool_builder_t builder = {0};
    sky_nvn_memory_pool_builder_set_device(&builder, device);
    sky_nvn_memory_pool_builder_set_storage(&builder, *allocation, 16 << 20);
    sky_nvn_memory_pool_builder_set_flags(&builder, 0x22);
    assert(sky_nvn_memory_pool_initialize(memory, &builder) == 1);
}

static void make_builder(sky_nvn_texture_builder_t *builder, sky_nvn_device_t *device,
                         sky_nvn_memory_pool_t *memory) {
    memset(builder, 0xa5, sizeof *builder);
    builder->dev = device;
    sky_nvn_texture_builder_set_defaults(builder);
    assert(builder->dev == device && builder->target == 1);
    assert(builder->width == 1 && builder->height == 1 && builder->depth == 1 && builder->levels == 1);
    sky_nvn_texture_builder_set_format(builder, 0x25);
    sky_nvn_texture_builder_set_size_2d(builder, 1600, 900);
    sky_nvn_texture_builder_set_flags(builder, 0x9);
    sky_nvn_texture_builder_set_storage(builder, memory, 0x10000);
}

static void expect_properties(const sky_nvn_texture_t *texture, uint32_t pixel_format,
                              uint32_t texture_type, uint64_t usage) {
    sky_mtl_texture_properties_t properties;
    assert(sky_mtl_texture_get_properties(texture->metal, &properties) == 1);
    assert(properties.pixel_format == pixel_format);
    assert(properties.texture_type == texture_type);
    assert(properties.storage_mode == SKY_MTL_STORAGE_PRIVATE);
    assert(properties.usage == usage);
}

int main(int argc, char **argv) {
    ASSERT_LOOKUP("nvnTextureBuilderSetDefaults", sky_nvn_texture_builder_set_defaults);
    ASSERT_LOOKUP("nvnTextureBuilderSetSize2D", sky_nvn_texture_builder_set_size_2d);
    ASSERT_LOOKUP("nvnTextureBuilderSetFormat", sky_nvn_texture_builder_set_format);
    ASSERT_LOOKUP("nvnTextureBuilderGetStorageSize", sky_nvn_texture_builder_get_storage_size);
    ASSERT_LOOKUP("nvnTextureBuilderGetStorageAlignment", sky_nvn_texture_builder_get_storage_alignment);
    ASSERT_LOOKUP("nvnTextureBuilderSetPackagedTextureData", sky_nvn_texture_builder_set_packaged_texture_data);
    ASSERT_LOOKUP("nvnTextureInitialize", sky_nvn_texture_initialize);
    ASSERT_LOOKUP("nvnTextureFinalize", sky_nvn_texture_finalize);
    ASSERT_LOOKUP("nvnTextureWriteTexels", sky_nvn_texture_write_texels);
    ASSERT_LOOKUP("nvnTextureGetTextureAddress", sky_nvn_texture_get_texture_address);
    ASSERT_LOOKUP("nvnQueueFenceSync", sky_nvn_queue_fence_sync);
    ASSERT_LOOKUP("nvnTextureViewSetDefaults", sky_nvn_texture_view_set_defaults);

    if (argc > 1 && !strcmp(argv[1], "queue-fence")) {
        sky_nvn_queue_fence_sync(NULL, NULL, 0, 0);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "texture-view")) {
        sky_nvn_texture_view_set_defaults(NULL);
        return 1;
    }

    sky_nvn_device_t device;
    sky_nvn_memory_pool_t memory;
    void *allocation = NULL;
    setup(&device, &memory, &allocation);

    if (argc > 1 && !strcmp(argv[1], "view-registration")) {
        sky_nvn_texture_builder_t builder;
        make_builder(&builder, &device, &memory);
        sky_nvn_texture_t texture;
        assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
        sky_nvn_texture_pool_t pool;
        assert(sky_nvn_texture_pool_initialize(&pool, &memory, 0, 300) == 1);
        sky_nvn_texture_pool_register_texture(&pool, 256, &texture, (const void *)1);
        return 1;
    }

    sky_nvn_texture_builder_t builder;
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_t texture;
    sky_nvn_texture_builder_set_format(&builder, 0);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0 && texture.magic == 0);
    sky_nvn_texture_builder_set_format(&builder, 0x19);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    sky_nvn_texture_builder_set_format(&builder, 0x51);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_target(&builder, 10);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_flags(&builder, 0x2);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_width(&builder, 0);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_samples(&builder, 2);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_storage(&builder, &memory, 1);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    sky_nvn_texture_builder_set_storage(&builder, &memory, memory.size);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    sky_nvn_texture_builder_set_storage(&builder, NULL, 0x10000);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 0);
    assert(sky_mtl_texture_active_count() == 0);

    make_builder(&builder, &device, &memory);
    assert(sky_nvn_texture_builder_get_storage_size(&builder) == 0x640000);
    assert(sky_nvn_texture_builder_get_storage_alignment(&builder) == 512);
    assert(sky_nvn_texture_builder_get_device(&builder) == &device);
    assert(sky_nvn_texture_builder_get_flags(&builder) == 0x9);
    assert(sky_nvn_texture_builder_get_target(&builder) == 1);
    assert(sky_nvn_texture_builder_get_width(&builder) == 1600);
    assert(sky_nvn_texture_builder_get_height(&builder) == 900);
    assert(sky_nvn_texture_builder_get_depth(&builder) == 1);
    assert(sky_nvn_texture_builder_get_levels(&builder) == 1);
    assert(sky_nvn_texture_builder_get_format(&builder) == 0x25);
    assert(sky_nvn_texture_builder_get_samples(&builder) == 0);
    assert(sky_nvn_texture_builder_get_memory_pool(&builder) == &memory);
    assert(sky_nvn_texture_builder_get_memory_offset(&builder) == 0x10000);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    assert(texture.magic == 0x00584554 && texture.dev == &device && texture.pool == &memory);
    assert(texture.storage_size == 0x640000 && texture.storage_alignment == 512);
    assert(texture.storage_address == memory.buffer_address + 0x10000);
    expect_properties(&texture, 70, SKY_MTL_TEXTURE_TYPE_2D, 0x5);
    assert(sky_nvn_texture_get_flags(&texture) == 0x9);
    assert(sky_nvn_texture_get_target(&texture) == 1);
    assert(sky_nvn_texture_get_width(&texture) == 1600);
    assert(sky_nvn_texture_get_height(&texture) == 900);
    assert(sky_nvn_texture_get_depth(&texture) == 1);
    assert(sky_nvn_texture_get_levels(&texture) == 1);
    assert(sky_nvn_texture_get_format(&texture) == 0x25);
    assert(sky_nvn_texture_get_samples(&texture) == 0);
    assert(sky_nvn_texture_get_memory_pool(&texture) == &memory);
    assert(sky_nvn_texture_get_memory_offset(&texture) == 0x10000);
    assert(sky_nvn_texture_get_storage_size(&texture) == 0x640000);
    assert(sky_nvn_texture_get_texture_address(&texture) == texture.storage_address);
    assert(sky_mtl_texture_active_count() == 1);

    sky_vi_initialize();
    sky_vi_display_t *display = NULL;
    sky_vi_layer_t *layer = NULL;
    sky_vi_native_window_t *native = NULL;
    assert(sky_vi_open_default_display(&display) == 0);
    assert(sky_vi_create_layer(&layer, display) == 0);
    assert(sky_vi_get_native_window(&native, layer) == 0);
    sky_nvn_window_builder_t window_builder = {0};
    sky_nvn_window_builder_set_device(&window_builder, &device);
    sky_nvn_window_builder_set_defaults(&window_builder);
    sky_nvn_window_builder_set_native_window(&window_builder, native);
    sky_nvn_window_builder_set_present_interval(&window_builder, 2);
    void *window_textures[1] = {&texture};
    sky_nvn_window_builder_set_textures(&window_builder, 1, window_textures);
    sky_nvn_window_t window;
    assert(sky_nvn_window_initialize(&window, &window_builder) == 1);
    assert(window.texture_count == 1 && window.textures[0] == &texture && window.metal_textures[0]);
    assert(window.active_texture_count==0);
    sky_nvn_sync_t acquired_sync;assert(sky_nvn_sync_initialize(&acquired_sync,&device));
    int acquired=-1;assert(sky_nvn_window_acquire_texture(&window,&acquired_sync,&acquired)==0&&acquired==0);
    assert(sky_nvn_sync_wait(&acquired_sync,0)==0);sky_nvn_sync_finalize(&acquired_sync);
    sky_nvn_queue_builder_t acquire_builder={0};sky_nvn_queue_builder_set_device(&acquire_builder,&device);
    sky_nvn_queue_builder_set_defaults(&acquire_builder);
    uint64_t acquire_size=sky_nvn_queue_builder_get_queue_memory_size(&acquire_builder);
    void *acquire_memory=NULL;assert(posix_memalign(&acquire_memory,(size_t)getpagesize(),acquire_size)==0);
    sky_nvn_queue_builder_set_queue_memory(&acquire_builder,acquire_memory,acquire_size);
    sky_nvn_queue_t acquire_queue;assert(sky_nvn_queue_initialize(&acquire_queue,&acquire_builder));
    assert(sky_nvn_queue_acquire_texture(&acquire_queue,&window,&acquired)==0&&acquired==0);
    sky_nvn_queue_finalize(&acquire_queue);free(acquire_memory);
    assert(sky_mtl_texture_active_count() == 2);

    sky_nvn_texture_pool_t pool;
    assert(sky_nvn_texture_pool_initialize(&pool, &memory, 0, 300) == 1);
    sky_nvn_texture_pool_register_texture(&pool, -1, &texture, NULL);
    sky_nvn_texture_pool_register_texture(&pool, 300, &texture, NULL);
    sky_nvn_texture_pool_register_texture(&pool, 256, NULL, NULL);
    sky_nvn_texture_pool_register_texture(&pool, 256, &texture, NULL);
    const sky_nvn_texture_t *registered = sky_nvn_texture_pool_get_registered_texture(&pool, 256);
    assert(registered && registered != &texture && registered->magic == 0x00584554);
    assert(registered->metal && registered->metal != texture.metal);
    expect_properties(registered, 70, SKY_MTL_TEXTURE_TYPE_2D, 0x5);
    assert(registered->pool && registered->pool != &memory);
    assert(registered->storage_address == texture.storage_address);
    assert(sky_mtl_texture_active_count() == 3 && sky_mtl_memory_pool_active_count() == 2);

    sky_nvn_texture_finalize(&texture);
    assert(texture.magic == 0 && sky_mtl_texture_active_count() == 2);
    assert(sky_nvn_texture_pool_get_registered_texture(&pool, 256) == registered);
    sky_nvn_window_finalize(&window);
    assert(sky_mtl_texture_active_count() == 1);
    sky_nvn_texture_pool_finalize(&pool);
    assert(sky_mtl_texture_active_count() == 0 && sky_mtl_memory_pool_active_count() == 1);
    assert(sky_nvn_texture_pool_get_registered_texture(&pool, 256) == NULL);

    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_target(&builder, 4);
    sky_nvn_texture_builder_set_size_3d(&builder, 16, 8, 3);
    sky_nvn_texture_builder_set_levels(&builder, 2);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    expect_properties(&texture, 70, SKY_MTL_TEXTURE_TYPE_2D_ARRAY, 0x5);
    sky_mtl_texture_properties_t properties;
    assert(sky_mtl_texture_get_properties(texture.metal, &properties) == 1);
    assert(properties.array_length == 3 && properties.mipmap_level_count == 2);
    sky_nvn_texture_finalize(&texture);

    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_target(&builder, 5);
    sky_nvn_texture_builder_set_size_2d(&builder, 64, 64);
    sky_nvn_texture_builder_set_samples(&builder, 4);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    expect_properties(&texture, 70, SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE, 0x5);
    assert(sky_mtl_texture_get_properties(texture.metal, &properties) == 1);
    assert(properties.sample_count == 4 && properties.mipmap_level_count == 1);
    sky_nvn_texture_finalize(&texture);

    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_target(&builder, 0);
    sky_nvn_texture_builder_set_size_1d(&builder, 64);
    sky_nvn_texture_builder_set_format(&builder, 1);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    expect_properties(&texture, 10, SKY_MTL_TEXTURE_TYPE_1D, 0x5);
    assert(sky_mtl_texture_get_properties(texture.metal, &properties) == 1);
    assert(properties.height == 1 && properties.depth == 1);
    sky_nvn_texture_finalize(&texture);

    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_size_2d(&builder, 16, 4);
    sky_nvn_texture_builder_set_flags(&builder, 0x10);
    sky_nvn_texture_builder_set_stride(&builder, 256);
    assert(sky_nvn_texture_builder_get_storage_size(&builder) == 1024);
    assert(sky_nvn_texture_builder_get_storage_alignment(&builder) == 32);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    expect_properties(&texture, 70, SKY_MTL_TEXTURE_TYPE_2D, 0x5);
    assert(sky_nvn_texture_get_stride(&texture) == 256);
    sky_nvn_texture_finalize(&texture);

    /* LINEAR_RENDER_TARGET (0x40) is also linear: the game's 320x192 save
       screenshot texture must report stride*height, not block-linear size. */
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_size_2d(&builder, 320, 192);
    sky_nvn_texture_builder_set_format(&builder, 0x25);
    sky_nvn_texture_builder_set_flags(&builder, 0x40);
    sky_nvn_texture_builder_set_stride(&builder, 1280);
    assert(sky_nvn_texture_builder_get_storage_size(&builder) == 320 * 4 * 192);
    assert(sky_nvn_texture_builder_get_storage_alignment(&builder) == 128);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    assert(sky_nvn_texture_get_storage_size(&texture) == 320 * 4 * 192);
    sky_nvn_texture_finalize(&texture);

    make_builder(&builder, &device, &memory);
    uint8_t packaged_layout[8] = {2, 0, 0, 0, 0, 0, 0, 0};
    sky_nvn_texture_builder_set_packaged_texture_layout(&builder, packaged_layout);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    assert(texture.has_layout && texture.pkg_block_h == 3);
    expect_properties(&texture, 70, SKY_MTL_TEXTURE_TYPE_2D, 0x1);
    sky_nvn_texture_finalize(&texture);

    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_size_2d(&builder, 4, 4);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    uint8_t zero_pixels[4 * 4 * 4] = {0};
    sky_nvn_copy_region_t full_region = {0, 0, 0, 4, 4, 1};
    sky_nvn_texture_write_texels(&texture, NULL, &full_region, zero_pixels);
    const uint8_t patch[16] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255,
    };
    sky_nvn_copy_region_t patch_region = {1, 1, 0, 2, 2, 1};
    sky_nvn_texture_view_t upload_view;
    sky_nvn_texture_view_set_defaults(&upload_view);
    sky_nvn_texture_write_texels(&texture, &upload_view, &patch_region, patch);
    uint8_t expected[4 * 4 * 4] = {0};
    memcpy(expected + (1 * 4 + 1) * 4, patch, 4);
    memcpy(expected + (1 * 4 + 2) * 4, patch + 4, 4);
    memcpy(expected + (2 * 4 + 1) * 4, patch + 8, 4);
    memcpy(expected + (2 * 4 + 2) * 4, patch + 12, 4);
    uint8_t pixels[4 * 4 * 4];
    size_t read_size = 0;
    assert(sky_mtl_texture_readback(texture.metal, 0, 0, SKY_MTL_READ_COLOR,
                                    pixels, sizeof pixels, &read_size) == 1);
    assert(read_size == sizeof pixels && !memcmp(pixels, expected, sizeof pixels));
    sky_nvn_texture_finalize(&texture);

    nvn_texdesc packaged_desc = {
        .target = 1, .format = 0x25, .flags = 0x9,
        .width = 4, .height = 4, .depth = 1, .levels = 1,
    };
    nvn_texlayout packaged_layout_info;
    nvn_tex_layout(&packaged_desc, &packaged_layout_info);
    uint8_t packaged[0x400] = {0};
    assert(packaged_layout_info.size <= sizeof packaged);
    assert(nvn_swizzle(&packaged_desc, 0, 0, expected, 16, packaged) == 0);
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_size_2d(&builder, 4, 4);
    sky_nvn_texture_builder_set_packaged_texture_data(&builder, packaged);
    assert(builder.packaged_data == packaged);
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    memset(pixels, 0, sizeof pixels);
    read_size = 0;
    assert(sky_mtl_texture_readback(texture.metal, 0, 0, SKY_MTL_READ_COLOR,
                                    pixels, sizeof pixels, &read_size) == 1);
    assert(read_size == sizeof pixels && !memcmp(pixels, expected, sizeof pixels));
    sky_nvn_texture_finalize(&texture);

    assert(sky_mtl_texture_active_count() == 0);
    assert(((uint8_t *)memory.mem)[0] == 0xa5 && ((uint8_t *)memory.mem)[0x10000] == 0xa5);
    sky_nvn_memory_pool_finalize(&memory);
    assert(sky_mtl_memory_pool_active_count() == 0);
    free(allocation);
    sky_nvn_device_finalize(&device);
    puts("T_NVN_TEXTURE_OK metal-properties-registration-window-cleanup");
    return 0;
}
