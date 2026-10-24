#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include "nvn/nvn_tex.h"
#include <assert.h>
#include <unistd.h>

_Static_assert(sizeof(sky_nvn_texture_view_t) == 0x10, "texture view must fit Skyrim storage");

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
    assert(builder->depth_stencil_mode == 0);
    assert(builder->swizzle[0] == 2 && builder->swizzle[1] == 3);
    assert(builder->swizzle[2] == 4 && builder->swizzle[3] == 5);
    sky_nvn_texture_builder_set_format(builder, 0x25);
    sky_nvn_texture_builder_set_size_2d(builder, 64, 64);
    sky_nvn_texture_builder_set_levels(builder, 4);
    sky_nvn_texture_builder_set_flags(builder, 0x9);
    sky_nvn_texture_builder_set_storage(builder, memory, 0x10000);
}

static void expect_view(const void *metal_view, uint32_t pixel_format, uint64_t levels,
                        uint64_t usage, const uint8_t swizzle[4]) {
    assert(metal_view);
    sky_mtl_texture_properties_t properties;
    assert(sky_mtl_texture_get_properties(metal_view, &properties) == 1);
    assert(properties.pixel_format == pixel_format);
    assert(properties.texture_type == SKY_MTL_TEXTURE_TYPE_2D);
    assert(properties.mipmap_level_count == levels);
    assert(properties.usage == usage);
    assert(!memcmp(properties.swizzle, swizzle, 4));
}

static void expect_rejected_slice(sky_nvn_command_buffer_t *command,sky_nvn_queue_t *queue,
                                  sky_nvn_texture_t *texture,int depth,int base,int count) {
    sky_nvn_texture_view_t view;
    sky_nvn_texture_view_set_defaults(&view);
    sky_nvn_texture_view_set_target(&view,1);
    sky_nvn_texture_view_set_layers(&view,base,count);
    sky_nvn_command_buffer_begin_recording(command);
    void *targets[]={texture},*views[]={&view};
    sky_nvn_command_buffer_set_render_targets(command,depth?0:1,depth?NULL:targets,
        depth?NULL:views,depth?texture:NULL,depth?&view:NULL);
    uint64_t handle=sky_nvn_command_buffer_end_recording(command),before=queue->submits;
    assert(handle);
    sky_nvn_queue_submit_commands(queue,1,&handle);assert(queue->submits==before);
}

static void test_parent_slice_rendering(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory) {
    enum { WIDTH=4,HEIGHT=4,PIXELS=WIDTH*HEIGHT };
    sky_nvn_queue_builder_t queue_builder={0};
    sky_nvn_queue_builder_set_device(&queue_builder,device);
    sky_nvn_queue_builder_set_defaults(&queue_builder);
    uint64_t queue_size=sky_nvn_queue_builder_get_queue_memory_size(&queue_builder);
    void *queue_memory=NULL;assert(posix_memalign(&queue_memory,(size_t)getpagesize(),queue_size)==0);
    sky_nvn_queue_builder_set_queue_memory(&queue_builder,queue_memory,queue_size);
    sky_nvn_queue_t queue;assert(sky_nvn_queue_initialize(&queue,&queue_builder));
    sky_nvn_command_buffer_t command;assert(sky_nvn_command_buffer_initialize(&command,device));

    sky_nvn_texture_builder_t builder;
    make_builder(&builder,device,memory);
    sky_nvn_texture_builder_set_target(&builder,8);
    sky_nvn_texture_builder_set_size_2d(&builder,WIDTH,HEIGHT);
    sky_nvn_texture_builder_set_levels(&builder,1);
    sky_nvn_texture_builder_set_storage(&builder,memory,0x200000);
    sky_nvn_texture_t cube;assert(sky_nvn_texture_initialize(&cube,&builder));
    uint8_t pixels[PIXELS*4];
    for(int face=0;face<6;face++) {
        for(int p=0;p<PIXELS;p++)memcpy(pixels+p*4,(uint8_t[]){face+1,17,34,255},4);
        assert(sky_mtl_texture_upload(cube.metal,0,face,(int[]){0,0,0,WIDTH,HEIGHT,1},pixels));
    }
    sky_nvn_texture_view_t face5;
    sky_nvn_texture_view_set_defaults(&face5);
    sky_nvn_texture_view_set_layers(&face5,5,1);
    sky_nvn_texture_view_set_target(&face5,1);
    assert(face5.state==UINT64_C(0x58d040000401400a)); /* Actual Native22 view. */
    void *targets[]={&cube},*views[]={&face5};const float magenta[]={1,0,1,1};
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,views,NULL,NULL);
    sky_nvn_command_buffer_clear_color(&command,0,magenta,15);
    uint64_t handle=sky_nvn_command_buffer_end_recording(&command);
    sky_nvn_queue_submit_commands(&queue,1,&handle);assert(queue.submits==1);
    sky_nvn_queue_finish(&queue);
    for(int face=0;face<6;face++) {
        size_t size=0;assert(sky_mtl_texture_readback(cube.metal,0,face,SKY_MTL_READ_COLOR,pixels,sizeof pixels,&size));
        assert(size==sizeof pixels);
        for(int p=0;p<PIXELS;p++)assert(!memcmp(pixels+p*4,face==5?(uint8_t[]){255,0,255,255}:(uint8_t[]){face+1,17,34,255},4));
    }

    make_builder(&builder,device,memory);
    sky_nvn_texture_builder_set_target(&builder,4);
    sky_nvn_texture_builder_set_size_3d(&builder,WIDTH,HEIGHT,2);
    sky_nvn_texture_builder_set_format(&builder,0x32);
    sky_nvn_texture_builder_set_levels(&builder,1);
    sky_nvn_texture_builder_set_flags(&builder,8);
    sky_nvn_texture_builder_set_storage(&builder,memory,0x210000);
    sky_nvn_texture_t depth;assert(sky_nvn_texture_initialize(&depth,&builder));
    sky_nvn_command_buffer_begin_recording(&command);
    for(int layer=0;layer<2;layer++) {
        sky_nvn_texture_view_t view;
        sky_nvn_texture_view_set_defaults(&view);
        sky_nvn_texture_view_set_layers(&view,layer,1);
        sky_nvn_texture_view_set_target(&view,1);
        if(layer==1)assert(view.state==UINT64_C(0x58d040000400400a));
        sky_nvn_command_buffer_set_render_targets(&command,0,NULL,NULL,&depth,&view);
        sky_nvn_command_buffer_clear_depth_stencil(&command,layer?0.75f:0.25f,1,0,0);
    }
    handle=sky_nvn_command_buffer_end_recording(&command);
    sky_nvn_queue_submit_commands(&queue,1,&handle);assert(queue.submits==2);
    sky_nvn_queue_finish(&queue);
    for(int layer=0;layer<2;layer++) {
        uint16_t values[PIXELS];size_t size=0;
        assert(sky_mtl_texture_readback(depth.metal,0,layer,SKY_MTL_READ_DEPTH,values,sizeof values,&size));
        assert(size==sizeof values);
        for(int p=0;p<PIXELS;p++)assert(abs((int)values[p]-(layer?49151:16384))<=1);
    }
    assert(sky_nvn_queue_get_error(&queue,NULL)==0);
    expect_rejected_slice(&command,&queue,&cube,0,6,1);
    expect_rejected_slice(&command,&queue,&cube,0,5,2);
    expect_rejected_slice(&command,&queue,&cube,0,4,2);
    expect_rejected_slice(&command,&queue,&depth,1,2,1);
    expect_rejected_slice(&command,&queue,&depth,1,1,2);
    expect_rejected_slice(&command,&queue,&depth,1,0,2);
    sky_nvn_command_buffer_finalize(&command);
    sky_nvn_queue_finalize(&queue);free(queue_memory);
    sky_nvn_texture_finalize(&cube);sky_nvn_texture_finalize(&depth);
    puts("NVN_PARENT_SLICE_GPU_OK cube-face5-depth-array-layer1-neighbor-preservation-and-bounds");
}

int main(void) {
    ASSERT_LOOKUP("nvnTextureViewSetDefaults", sky_nvn_texture_view_set_defaults);
    ASSERT_LOOKUP("nvnTextureViewSetLevels", sky_nvn_texture_view_set_levels);
    ASSERT_LOOKUP("nvnTextureViewSetLayers", sky_nvn_texture_view_set_layers);
    ASSERT_LOOKUP("nvnTextureViewSetFormat", sky_nvn_texture_view_set_format);
    ASSERT_LOOKUP("nvnTextureViewSetSwizzle", sky_nvn_texture_view_set_swizzle);
    ASSERT_LOOKUP("nvnTextureViewSetDepthStencilMode", sky_nvn_texture_view_set_depth_stencil_mode);
    ASSERT_LOOKUP("nvnTextureViewSetTarget", sky_nvn_texture_view_set_target);
    ASSERT_LOOKUP("nvnTextureViewGetLevels", sky_nvn_texture_view_get_levels);
    ASSERT_LOOKUP("nvnTextureViewGetLayers", sky_nvn_texture_view_get_layers);
    ASSERT_LOOKUP("nvnTextureViewGetFormat", sky_nvn_texture_view_get_format);
    ASSERT_LOOKUP("nvnTextureViewGetSwizzle", sky_nvn_texture_view_get_swizzle);
    ASSERT_LOOKUP("nvnTextureViewGetDepthStencilMode", sky_nvn_texture_view_get_depth_stencil_mode);
    ASSERT_LOOKUP("nvnTextureViewGetTarget", sky_nvn_texture_view_get_target);

    sky_nvn_device_t device;
    sky_nvn_memory_pool_t memory;
    void *allocation = NULL;
    setup(&device, &memory, &allocation);
    int device_info = 0;
    sky_nvn_device_get_integer(&device, 0x0a, &device_info);
    assert(device_info == 16);
    sky_nvn_device_get_integer(&device, 0x38, &device_info);
    assert(device_info == 32);
    sky_nvn_device_get_integer(&device, 0x39, &device_info);
    assert(device_info == 128);

    sky_nvn_texture_builder_t builder;
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_depth_stencil_mode(&builder, 1);
    sky_nvn_texture_builder_set_swizzle(&builder, 4, 2, 3, 1);
    int r, g, b, a;
    sky_nvn_texture_builder_get_swizzle(&builder, &r, &g, &b, &a);
    assert(r == 4 && g == 2 && b == 3 && a == 1);
    assert(sky_nvn_texture_builder_get_depth_stencil_mode(&builder) == 1);
    uint8_t custom_layout[8] = {2, 0, 0, 0, 0, 0, 0, 0};
    sky_nvn_texture_builder_set_packaged_texture_layout(&builder, custom_layout);
    sky_nvn_texture_t texture;
    assert(sky_nvn_texture_initialize(&texture, &builder) == 1);
    assert(sky_nvn_texture_get_depth_stencil_mode(&texture) == 1);
    sky_nvn_texture_get_swizzle(&texture, &r, &g, &b, &a);
    assert(r == 4 && g == 2 && b == 3 && a == 1);
    sky_mtl_texture_properties_t base_properties;
    assert(sky_mtl_texture_get_properties(texture.metal, &base_properties) == 1);
    assert(base_properties.swizzle[0] == 4 && base_properties.swizzle[1] == 2);
    assert(base_properties.swizzle[2] == 3 && base_properties.swizzle[3] == 1);

    sky_nvn_texture_pool_t pool;
    assert(sky_nvn_texture_pool_initialize(&pool, &memory, 0, 300) == 1);
    sky_nvn_texture_view_t view;
    sky_nvn_texture_view_set_defaults(&view);
    assert(view.magic == 0x57454956);
    int base = -1, count = -1, minimum = -1, format = -1, target = -1, mode = -1;
    assert(sky_nvn_texture_view_get_levels(&view, &base, &count) == 0);
    assert(base == 0 && count == 0);
    assert(sky_nvn_texture_view_get_layers(&view, &minimum, &count) == 0);
    assert(minimum == 0 && count == 0);
    assert(sky_nvn_texture_view_get_format(&view, &format) == 0 && format == 0);
    assert(sky_nvn_texture_view_get_target(&view, &target) == 0 && target == 0);
    assert(sky_nvn_texture_view_get_depth_stencil_mode(&view, &mode) == 0 && mode == 0);
    assert(sky_nvn_texture_view_get_swizzle(&view, &r, &g, &b, &a) == 0);
    assert(r == 2 && g == 3 && b == 4 && a == 5);
    sky_nvn_texture_view_t same;
    sky_nvn_texture_view_set_defaults(&same);
    assert(sky_nvn_texture_view_compare(&view, &same) == 1);

    sky_nvn_texture_view_set_levels(&view, 1, 2);
    sky_nvn_texture_view_set_layers(&view, 0, 0);
    sky_nvn_texture_view_set_format(&view, 0x25);
    sky_nvn_texture_view_set_target(&view, 1);
    sky_nvn_texture_view_set_swizzle(&view, 4, 3, 2, 1);
    sky_nvn_texture_view_set_depth_stencil_mode(&view, 1);
    assert(sky_nvn_texture_view_compare(&view, &same) == 0);
    assert(sky_nvn_texture_view_get_levels(&view, &base, &count) == 1);
    assert(base == 1 && count == 2);
    assert(sky_nvn_texture_view_get_layers(&view, &minimum, &count) == 1);
    assert(minimum == 0 && count == 0);
    assert(sky_nvn_texture_view_get_format(&view, &format) == 1 && format == 0x25);
    assert(sky_nvn_texture_view_get_target(&view, &target) == 1 && target == 1);
    assert(sky_nvn_texture_view_get_depth_stencil_mode(&view, &mode) == 1 && mode == 1);
    assert(sky_nvn_texture_view_get_swizzle(&view, &r, &g, &b, &a) == 1);
    assert(r == 4 && g == 3 && b == 2 && a == 1);
    const uint8_t expected_swizzle[4] = {4, 3, 2, 1};
    sky_nvn_texture_pool_register_texture(&pool, 256, &texture, &view);
    const void *metal_view = sky_nvn_texture_pool_get_registered_metal_view(&pool, 256);
    expect_view(metal_view, 70, 2, 0x1, expected_swizzle);
    const sky_nvn_texture_t *registered = sky_nvn_texture_pool_get_registered_texture(&pool, 256);
    assert(registered && registered->magic == 0x00584554);

    nvn_texdesc desc = {
        texture.target, texture.format, texture.flags, texture.samples,
        texture.width, texture.height, texture.depth, texture.levels,
        (uint32_t)texture.stride, texture.pkg_block_h,
    };
    assert(sky_nvn_texture_get_view_offset(&texture, &view) == (int64_t)nvn_tex_level_offset(&desc, 1));

    sky_nvn_texture_view_t invalid;
    sky_nvn_texture_view_set_defaults(&invalid);
    sky_nvn_texture_view_set_levels(&invalid, 3, 2);
    sky_nvn_texture_pool_register_texture(&pool, 257, &texture, &invalid);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 257) == NULL);
    sky_nvn_texture_view_set_defaults(&invalid);
    sky_nvn_texture_view_set_layers(&invalid, 1, 1);
    sky_nvn_texture_pool_register_texture(&pool, 258, &texture, &invalid);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 258) == NULL);
    sky_nvn_texture_view_set_defaults(&invalid);
    sky_nvn_texture_view_set_target(&invalid, 10);
    sky_nvn_texture_pool_register_texture(&pool, 259, &texture, &invalid);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 259) == NULL);
    sky_nvn_texture_view_set_defaults(&invalid);
    sky_nvn_texture_view_set_format(&invalid, 0x19);
    sky_nvn_texture_pool_register_texture(&pool, 260, &texture, &invalid);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 260) == NULL);
    sky_nvn_texture_view_set_defaults(&invalid);
    sky_nvn_texture_view_set_format(&invalid, 0xa);
    sky_nvn_texture_pool_register_texture(&pool, 261, &texture, &invalid);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 261) == NULL);
    sky_nvn_texture_view_set_defaults(&invalid);
    sky_nvn_texture_view_set_swizzle(&invalid, 6, 0, 0, 0);
    assert(sky_nvn_texture_view_get_swizzle(&invalid, &r, &g, &b, &a) == 0);
    sky_nvn_texture_view_set_target(&invalid, 99);
    assert(sky_nvn_texture_view_get_target(&invalid, &target) == 0);
    sky_nvn_texture_pool_register_texture(&pool, 262, &texture, NULL);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 262) == NULL);

    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_format(&builder, 0x35);
    sky_nvn_texture_builder_set_levels(&builder, 1);
    sky_nvn_texture_t depth;
    assert(sky_nvn_texture_initialize(&depth, &builder) == 1);
    {
        sky_mtl_texture_properties_t properties;
        assert(sky_mtl_texture_get_properties(depth.metal, &properties) == 1);
        assert(properties.pixel_format == 260 && properties.usage == 0x5);
    }
    sky_nvn_texture_view_set_defaults(&view);
    sky_nvn_texture_pool_register_texture(&pool, 270, &depth, &view);
    const uint8_t default_swizzle[4] = {2, 3, 4, 5};
    expect_view(sky_nvn_texture_pool_get_registered_metal_view(&pool, 270), 260, 1, 0x5, default_swizzle);

    assert(!setenv("SKY_METAL_FORCE_BC_DECODE", "1", 1));
    make_builder(&builder, &device, &memory);
    sky_nvn_texture_builder_set_format(&builder, 0x42);
    sky_nvn_texture_builder_set_size_2d(&builder, 16, 16);
    sky_nvn_texture_builder_set_levels(&builder, 1);
    sky_nvn_texture_builder_set_storage(&builder, &memory, 0x220000);
    nvn_texdesc bc_desc = {
        .target = 1, .format = 0x42, .flags = 0x9,
        .width = 16, .height = 16, .depth = 1, .levels = 1,
    };
    nvn_texlayout bc_layout;
    nvn_tex_layout(&bc_desc, &bc_layout);
    uint8_t bc_pack[512] = {0};
    assert(bc_layout.size == sizeof bc_pack);
    const unsigned bc_offsets[16] = {0, 8, 32, 40, 16, 24, 48, 56, 64, 72, 96, 104, 80, 88, 112, 120};
    for (unsigned i = 0; i < 16; i++) {
        uint16_t color = (uint16_t)(i << 11);
        unsigned offset = bc_offsets[i];
        bc_pack[offset] = bc_pack[offset + 2] = (uint8_t)color;
        bc_pack[offset + 1] = bc_pack[offset + 3] = (uint8_t)(color >> 8);
    }
    sky_nvn_texture_builder_set_packaged_texture_data(&builder, bc_pack);
    sky_nvn_texture_t bc;
    assert(sky_nvn_texture_initialize(&bc, &builder) == 1);
    assert(sky_mtl_texture_is_bc_fallback(bc.metal));
    sky_mtl_texture_properties_t bc_properties;
    assert(sky_mtl_texture_get_properties(bc.metal, &bc_properties));
    assert(bc_properties.pixel_format == 130 && bc_properties.texture_type == SKY_MTL_TEXTURE_TYPE_2D &&
           bc_properties.storage_mode == SKY_MTL_STORAGE_PRIVATE && bc_properties.usage == 0x11);
    uint8_t bc_pixels[16 * 16 * 4];
    size_t bc_read = 0;
    assert(sky_mtl_texture_readback_decoded(bc.metal, 0, 0, bc_pixels, sizeof bc_pixels, &bc_read));
    assert(bc_read == sizeof bc_pixels);
    const uint8_t bc_red[16] = {0, 8, 16, 25, 33, 41, 49, 58, 66, 74, 82, 90, 99, 107, 115, 123};
    for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++) {
        unsigned i = (y / 4) * 4 + x / 4;
        uint8_t expected[4] = {bc_red[i], 0, 0, 255};
        assert(!memcmp(bc_pixels + (y * 16 + x) * 4, expected, 4));
    }
    sky_nvn_texture_view_set_defaults(&view);
    sky_nvn_texture_view_set_format(&view, 0x46);
    sky_nvn_texture_pool_register_texture(&pool, 271, &bc, &view);
    expect_view(sky_nvn_texture_pool_get_registered_metal_view(&pool, 271),
        131, 1, 0x11, default_swizzle);
    sky_nvn_texture_view_set_format(&view, 0x44);
    sky_nvn_texture_pool_register_texture(&pool, 272, &bc, &view);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 272) == NULL);
    sky_nvn_texture_finalize(&bc);
    assert(!unsetenv("SKY_METAL_FORCE_BC_DECODE"));
    puts("NVN_BC_NVN_PACKAGED_SRGB_VIEW_OK deswizzle-decode-real-builder-and-family-guard");

    sky_nvn_texture_finalize(&texture);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 256) == metal_view);
    sky_nvn_texture_finalize(&depth);
    sky_nvn_texture_pool_finalize(&pool);
    assert(sky_nvn_texture_pool_get_registered_metal_view(&pool, 256) == NULL);
    test_parent_slice_rendering(&device,&memory);
    assert(sky_mtl_texture_active_count() == 0);
    assert(((uint8_t *)memory.mem)[0] == 0xa5);
    sky_nvn_memory_pool_finalize(&memory);
    free(allocation);
    sky_nvn_device_finalize(&device);
    puts("T_NVN_VIEWS_OK ranges-swizzle-depth-stencil-ownership-cleanup");
    return 0;
}
