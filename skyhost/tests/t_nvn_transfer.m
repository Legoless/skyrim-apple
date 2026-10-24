#import "nvn_metal.h"
#import "nvn_transfer.h"
#import <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The direct transfer API never replays an NVN command recorder. Keep that
 * unrelated backend hook out of this standalone GPU test and fail if called. */
void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *state) {
    (void)state;
    assert(!"unexpected NVN recorder access in direct transfer test");
}

static sky_mtl_texture_t *make_texture(uint32_t type, uint32_t pixel_format,
    unsigned width, unsigned height, unsigned depth, unsigned layers, unsigned levels) {
    sky_mtl_texture_properties_t properties = {
        .texture_type = type, .pixel_format = pixel_format, .storage_mode = SKY_MTL_STORAGE_PRIVATE,
        .width = width, .height = height, .depth = depth, .array_length = layers,
        .mipmap_level_count = levels, .sample_count = 1,
        .usage = SKY_MTL_TEXTURE_USAGE_SHADER_READ | SKY_MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW,
        .swizzle = {2, 3, 4, 5},
    };
    sky_mtl_texture_t *texture = sky_mtl_texture_create(&properties);
    assert(texture);
    return texture;
}

static void finish(id<MTLCommandBuffer> command) {
    [command commit];
    [command waitUntilCompleted];
    if (command.error) fprintf(stderr, "TRANSFER_TEST_GPU_ERROR %s\n", command.error.description.UTF8String);
    assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
}

static void copy_region(int32_t target[6], int x, int y, int z, int w, int h, int d) {
    int32_t region[6] = {x, y, z, w, h, d};
    memcpy(target, region, sizeof region);
}

static void check_rows(const uint8_t *actual, const uint8_t *source,
    size_t actual_row, size_t actual_image, size_t source_row, size_t source_image,
    size_t tight, unsigned rows, unsigned layers, uint8_t guard) {
    for (unsigned z = 0; z < layers; z++) {
        for (unsigned y = 0; y < rows; y++) {
            assert(!memcmp(actual + z * actual_image + y * actual_row,
                           source + z * source_image + y * source_row, tight));
            for (size_t x = tight; x < actual_row; x++)
                assert(actual[z * actual_image + y * actual_row + x] == guard);
        }
        for (size_t x = rows * actual_row; x < actual_image; x++)
            assert(actual[z * actual_image + x] == guard);
    }
}

static void test_array_mip_strides(id<MTLCommandQueue> queue, uint8_t *memory, uint64_t address) {
    sky_mtl_texture_t *source = make_texture(SKY_MTL_TEXTURE_TYPE_2D_ARRAY, 70, 16, 12, 1, 3, 3);
    sky_mtl_texture_t *destination = make_texture(SKY_MTL_TEXTURE_TYPE_2D_ARRAY, 70, 16, 12, 1, 3, 3);
    size_t input = 268, output = 0x2004;
    memset(memory + input, 0x61, 320);
    memset(memory + output - 4, 0xcd, 264);
    for (unsigned z = 0; z < 2; z++) for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 20; x++)
        memory[input + z * 160 + y * 32 + x] = (uint8_t)(7 + z * 79 + y * 31 + x);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    sky_mtl_transfer_t transfer = {.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
        .destination = source, .destination_format = 0x25, .destination_level = 1, .destination_layer = 1,
        .buffer_address = address + input, .row_stride = 32, .image_stride = 160};
    copy_region(transfer.destination_region, 1, 1, 0, 5, 3, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE,
        .source = source, .destination = destination, .source_format = 0x25, .destination_format = 0x25,
        .source_level = 1, .source_layer = 1, .destination_level = 1};
    copy_region(transfer.source_region, 1, 1, 0, 5, 3, 2);
    copy_region(transfer.destination_region, 2, 2, 0, 5, 3, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
        .source = destination, .source_format = 0x25, .source_level = 1,
        .buffer_address = address + output, .row_stride = 28, .image_stride = 128};
    copy_region(transfer.source_region, 2, 2, 0, 5, 3, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    finish(command);
    check_rows(memory + output, memory + input, 28, 128, 32, 160, 20, 3, 2, 0xcd);
    for (unsigned i = 0; i < 4; i++) assert(memory[output - 4 + i] == 0xcd && memory[output + 256 + i] == 0xcd);
    sky_mtl_texture_destroy(source);
    sky_mtl_texture_destroy(destination);
}

static void test_volume(id<MTLCommandQueue> queue, uint8_t *memory, uint64_t address) {
    sky_mtl_texture_t *source = make_texture(SKY_MTL_TEXTURE_TYPE_3D, 70, 8, 6, 4, 1, 1);
    sky_mtl_texture_t *destination = make_texture(SKY_MTL_TEXTURE_TYPE_3D, 70, 8, 6, 4, 1, 1);
    size_t input = 0x4000, output = 0x5000;
    memset(memory + input, 0x55, 200);
    memset(memory + output, 0xe7, 256);
    for (unsigned z = 0; z < 2; z++) for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 12; x++)
        memory[input + z * 100 + y * 20 + x] = (uint8_t)(z * 93 + y * 17 + x);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    sky_mtl_transfer_t transfer = {.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
        .destination = source, .destination_format = 0x25, .buffer_address = address + input,
        .row_stride = 20, .image_stride = 100};
    copy_region(transfer.destination_region, 1, 2, 1, 3, 2, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE,
        .source = source, .destination = destination, .source_format = 0x25, .destination_format = 0x25};
    copy_region(transfer.source_region, 1, 2, 1, 3, 2, 2);
    copy_region(transfer.destination_region, 2, 1, 0, 3, 2, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
        .source = destination, .source_format = 0x25, .buffer_address = address + output,
        .row_stride = 24, .image_stride = 128};
    copy_region(transfer.source_region, 2, 1, 0, 3, 2, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    finish(command);
    check_rows(memory + output, memory + input, 24, 128, 20, 100, 12, 2, 2, 0xe7);
    sky_mtl_texture_destroy(source);
    sky_mtl_texture_destroy(destination);
}

static void test_compressed_blocks(id<MTLCommandQueue> queue, uint8_t *memory, uint64_t address) {
    sky_mtl_texture_t *source = make_texture(SKY_MTL_TEXTURE_TYPE_2D_ARRAY, 130, 12, 8, 1, 2, 3);
    sky_mtl_texture_t *destination = make_texture(SKY_MTL_TEXTURE_TYPE_2D_ARRAY, 130, 12, 8, 1, 2, 3);
    size_t input = 0x6000, output = 0x7000;
    for (unsigned i = 0; i < 160; i++) memory[input + i] = (uint8_t)(i * 71 + 13);
    memset(memory + output, 0x69, 192);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    sky_mtl_transfer_t transfer = {.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
        .destination = source, .destination_format = 0x42, .buffer_address = address + input,
        .row_stride = 24, .image_stride = 80};
    copy_region(transfer.destination_region, 4, 0, 0, 8, 8, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE, .flags = 3,
        .source = source, .destination = destination, .source_format = 0x42, .destination_format = 0x42};
    copy_region(transfer.source_region, 4, 0, 0, 8, 8, 2);
    copy_region(transfer.destination_region, 0, 0, 0, 8, 8, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
        .source = destination, .source_format = 0x42, .buffer_address = address + output,
        .row_stride = 32, .image_stride = 96};
    copy_region(transfer.source_region, 0, 0, 0, 8, 8, 2);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    /* A 3x2 mip is one compressed block; whole-mip edge copies are legal. */
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
        .destination = source, .destination_format = 0x42, .destination_level = 2,
        .destination_layer = 1, .buffer_address = address + input};
    copy_region(transfer.destination_region, 0, 0, 0, 3, 2, 0);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
        .source = source, .source_format = 0x42, .source_level = 2, .source_layer = 1,
        .buffer_address = address + output + 192};
    copy_region(transfer.source_region, 0, 0, 0, 3, 2, 0);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    finish(command);
    check_rows(memory + output, memory + input, 32, 96, 24, 80, 16, 2, 2, 0x69);
    assert(!memcmp(memory + output + 192, memory + input, 8));
    sky_mtl_texture_destroy(source);
    sky_mtl_texture_destroy(destination);
}

static void test_swizzled_view_is_raw_copy(id<MTLCommandQueue> queue, uint8_t *memory, uint64_t address) {
    sky_mtl_texture_t *base = make_texture(SKY_MTL_TEXTURE_TYPE_2D, 70, 4, 3, 1, 1, 1);
    const uint8_t swizzle[4] = {4, 3, 2, 5};
    sky_mtl_texture_t *view = sky_mtl_texture_view_create(base, 70, SKY_MTL_TEXTURE_TYPE_2D,
        0, 1, 0, 1, swizzle);
    sky_mtl_texture_t *destination = make_texture(SKY_MTL_TEXTURE_TYPE_2D, 70, 4, 3, 1, 1, 1);
    assert(view);
    for (unsigned i = 0; i < 48; i++) memory[0x8000 + i] = (uint8_t)(19 + i * 7);
    memset(memory + 0x9000, 0xa7, 48);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    sky_mtl_transfer_t transfer = {.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
        .destination = view, .destination_format = 0x25, .buffer_address = address + 0x8000};
    copy_region(transfer.destination_region, 0, 0, 0, 4, 3, 1);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE,
        .source = view, .destination = destination, .source_format = 0x25, .destination_format = 0x25};
    copy_region(transfer.source_region, 0, 0, 0, 4, 3, 1);
    copy_region(transfer.destination_region, 0, 0, 0, 4, 3, 1);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
        .source = destination, .source_format = 0x25, .buffer_address = address + 0x9000};
    copy_region(transfer.source_region, 0, 0, 0, 4, 3, 1);
    assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    finish(command);
    assert(!memcmp(memory + 0x8000, memory + 0x9000, 48));
    sky_mtl_texture_destroy(view);
    sky_mtl_texture_destroy(base);
    sky_mtl_texture_destroy(destination);
}

static void test_rejections(id<MTLCommandQueue> queue, uint64_t address) {
    sky_mtl_texture_t *texture = make_texture(SKY_MTL_TEXTURE_TYPE_2D, 70, 8, 8, 1, 1, 1);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    sky_mtl_transfer_t transfer = {.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
        .destination = texture, .destination_format = 0x25, .buffer_address = address};
    copy_region(transfer.destination_region, 0, 0, 0, 4, 4, 1);
    transfer.row_stride = 8;
    assert(!sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer.row_stride = 0;
    transfer.buffer_address = 1;
    assert(!sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer.buffer_address = address;
    transfer.destination_layer = 1;
    assert(!sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer.destination_layer = 0;
    transfer.flags = 4;
    assert(!sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    transfer.flags = 0;
    transfer.destination_region[0] = 6;
    assert(!sky_mtl_transfer_encode((__bridge void *)command, &transfer));
    assert(!sky_mtl_transfer_encode(NULL, &transfer));
    finish(command);
    sky_mtl_texture_destroy(texture);
}

static void test_cpu_linear_planes(id<MTLCommandQueue> queue, uint8_t *memory, uint64_t address, int mirror) {
    id<MTLDevice> device = queue.device;
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void sample_plane(texture2d<float> t [[texture(0)]],device uint *o [[buffer(0)]],uint i [[thread_position_in_grid]]) {"
        "constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);"
        "uint w=t.get_width(),h=t.get_height(),x=(i*127)%w,y=(i*43)%h;"
        "o[i]=uint(round(t.sample(s,(float2(x,y)+0.5)/float2(w,h),level(0)).r*255.0));}";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    assert(library && !error);
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"sample_plane"] error:&error];
    assert(pipeline && !error);
    id<MTLBuffer> output = [device newBufferWithLength:256 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    assert(output);
    /* Full-size observed Bink planes, with extra row padding to prove that the
     * NVN pitch, rather than the visible width, addresses successive rows. */
    const unsigned widths[2] = {1920, 960}, heights[2] = {1080, 544};
    /* Mirrors must accept the guest's byte offsets/padding even when those
     * rows cannot be used as a native buffer-backed texture. */
    const unsigned pitches[2] = {1952 + (unsigned)mirror, 992 + (unsigned)mirror};
    size_t offsets[2] = {0x100000 + mirror, 0x100000 + mirror + (size_t)pitches[0] * heights[0]};
    sky_mtl_texture_t *planes[2];
    for (unsigned plane = 0; plane < 2; plane++) {
        planes[plane] = mirror
            ? sky_mtl_texture_create_linear_copy_from_buffer(address + offsets[plane],
                MTLPixelFormatR8Unorm, widths[plane], heights[plane], pitches[plane])
            : sky_mtl_texture_create_linear_from_buffer(address + offsets[plane],
                MTLPixelFormatR8Unorm, widths[plane], heights[plane], pitches[plane]);
        assert(planes[plane]);
        id<MTLTexture> native = (__bridge id<MTLTexture>)sky_mtl_texture_native_handle(planes[plane]);
        if (mirror) {
            assert(!native.buffer && native.storageMode == MTLStorageModePrivate &&
                   native.usage == (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite));
            const uint8_t swizzle[4] = {2, 2, 2, 5};
            sky_mtl_texture_t *view = sky_mtl_texture_view_create(planes[plane], MTLPixelFormatR8Unorm,
                SKY_MTL_TEXTURE_TYPE_2D, 0, 1, 0, 1, swizzle);
            assert(view);
            sky_mtl_texture_destroy(planes[plane]);
            planes[plane] = sky_mtl_texture_retain(view);
            sky_mtl_texture_destroy(view);
            assert(planes[plane]);
        } else {
            assert(native.buffer && native.storageMode == MTLStorageModeShared &&
                   native.usage == MTLTextureUsageShaderRead && native.bufferBytesPerRow == pitches[plane]);
        }
        assert(sky_mtl_linear_texture_needs_sync(sky_mtl_texture_native_handle(planes[plane])) == mirror);
    }
    for (unsigned frame = 0; frame < 2; frame++) for (unsigned plane = 0; plane < 2; plane++) {
        uint8_t *bytes = memory + offsets[plane];
        memset(bytes, 0xed, (size_t)pitches[plane] * heights[plane]);
        for (unsigned y = 0; y < heights[plane]; y++) for (unsigned x = 0; x < widths[plane]; x++)
            bytes[(size_t)y * pitches[plane] + x] = (uint8_t)(frame * 71 + plane * 33 + x + y * 3);
        /* No guest upload, descriptor replacement or explicit CPU cache flush
         * occurs. The backend copies mirror storage in the consumer's command
         * buffer; coherent aliases require no copy. */
        id<MTLCommandBuffer> command = [queue commandBuffer];
        assert(sky_mtl_linear_texture_encode_sync((__bridge void *)command,
            sky_mtl_texture_native_handle(planes[plane])));
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:(__bridge id<MTLTexture>)sky_mtl_texture_native_handle(planes[plane]) atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        finish(command);
        const uint32_t *values = output.contents;
        for (unsigned i = 0; i < 256; i++) {
            unsigned x = i * 127 % widths[plane], y = i * 43 % heights[plane];
            int expected = (uint8_t)(frame * 71 + plane * 33 + x + y * 3);
            assert(abs((int)values[i] - expected) <= 1);
        }
    }
    if (mirror) {
        /* A GPU write must reach the original guest bytes. Otherwise the next
         * CPU-to-texture refresh would undo it. Partial writes retain every
         * unaffected texel and the source's odd row padding. */
        const size_t input = 0x80004;
        memset(memory + input, 0xca, 36);
        for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 5; x++)
            memory[input + y * 12 + x] = (uint8_t)(201 + y * 9 + x);
        sky_mtl_transfer_t transfer = {.kind = SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
            .destination = planes[0], .destination_format = 1,
            .buffer_address = address + input, .row_stride = 12};
        copy_region(transfer.destination_region, 3, 2, 0, 5, 3, 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
        finish(command);
        for (unsigned y = 0; y < heights[0]; y++) for (unsigned x = 0; x < pitches[0]; x++) {
            uint8_t expected = x >= widths[0] ? 0xed : (uint8_t)(71 + x + y * 3);
            if (x >= 3 && x < 8 && y >= 2 && y < 5)
                expected = memory[input + (y - 2) * 12 + x - 3];
            assert(memory[offsets[0] + (size_t)y * pitches[0] + x] == expected);
        }
        transfer = (sky_mtl_transfer_t){.kind = SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
            .source = planes[0], .source_format = 1,
            .buffer_address = address + 0xa0000, .row_stride = 8};
        copy_region(transfer.source_region, 3, 2, 0, 5, 3, 1);
        memset(memory + 0xa0000, 0xcd, 24);
        command = [queue commandBuffer];
        assert(sky_mtl_transfer_encode((__bridge void *)command, &transfer));
        finish(command);
        check_rows(memory + 0xa0000, memory + input, 8, 24, 12, 36, 5, 3, 1, 0xcd);
    }
    assert(!sky_mtl_texture_create_linear_from_buffer(address, 70, 16, 16, 64));
    assert(!sky_mtl_texture_create_linear_from_buffer(address + 1, 10, 1920, 1080, 1920));
    assert(!sky_mtl_texture_create_linear_from_buffer(address, 10, 1920, 1080, 1921));
    assert(!sky_mtl_texture_create_linear_from_buffer(address, 10, 1920, 1080, 1919));
    assert(!sky_mtl_texture_create_linear_from_buffer(address + (8u << 20) - 16, 10, 1920, 1080, 1920));
    assert(!sky_mtl_texture_create_linear_from_buffer(address, 10, 20000, 1, 20000));
    assert(!sky_mtl_texture_create_linear_copy_from_buffer(address, 70, 16, 16, 64));
    assert(!sky_mtl_texture_create_linear_copy_from_buffer(address, 10, 0, 1080, 1920));
    assert(!sky_mtl_texture_create_linear_copy_from_buffer(address, 10, 1920, 1080, 1919));
    assert(!sky_mtl_texture_create_linear_copy_from_buffer(address + (8u << 20) - 16, 10, 1920, 1080, 1920));
    assert(!sky_mtl_texture_create_linear_copy_from_buffer(address, 10, 20000, 1, 20000));
    assert(!sky_mtl_linear_texture_encode_sync(NULL, sky_mtl_texture_native_handle(planes[0])));
    assert(!sky_mtl_linear_texture_encode_flush(NULL, sky_mtl_texture_native_handle(planes[0])));
    assert(!sky_mtl_linear_texture_needs_sync(NULL));
    sky_mtl_texture_destroy(planes[0]);
    sky_mtl_texture_destroy(planes[1]);
}

static void test_linear_mirror_retains_source(id<MTLCommandQueue> queue) {
    uint8_t *memory = NULL;
    assert(!posix_memalign((void **)&memory, (size_t)getpagesize(), (size_t)getpagesize()));
    memset(memory, 0xcd, (size_t)getpagesize());
    sky_mtl_memory_pool_t *pool = sky_mtl_memory_pool_create(memory, (size_t)getpagesize());
    assert(pool);
    sky_mtl_texture_t *base = sky_mtl_texture_create_linear_copy_from_buffer(
        sky_mtl_memory_pool_gpu_address(pool) + 3, MTLPixelFormatR8Unorm, 7, 3, 13);
    assert(base);
    const uint8_t swizzle[4] = {2, 3, 4, 5};
    sky_mtl_texture_t *view = base;
    for (unsigned depth = 0; depth < 40; depth++) {
        sky_mtl_texture_t *parent = view;
        view = sky_mtl_texture_view_create(parent, MTLPixelFormatR8Unorm,
            SKY_MTL_TEXTURE_TYPE_2D, 0, 1, 0, 1, swizzle);
        assert(view);
        sky_mtl_texture_destroy(parent);
    }
    sky_mtl_memory_pool_destroy(pool);
    for (unsigned frame = 0; frame < 2; frame++) {
        for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 7; x++)
            memory[3 + y * 13 + x] = (uint8_t)(x + y * 11 + frame * 57);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        assert(sky_mtl_linear_texture_encode_sync((__bridge void *)command, sky_mtl_texture_native_handle(view)));
        finish(command);
        uint8_t result[21];
        assert(sky_mtl_texture_readback(view, 0, 0, SKY_MTL_READ_COLOR, result, sizeof result, NULL));
        for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 7; x++)
            assert(result[y * 7 + x] == memory[3 + y * 13 + x]);
    }
    sky_mtl_texture_destroy(view);
    free(memory);
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        assert(device && device.hasUnifiedMemory && device.supportsBCTextureCompression);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        assert(queue);
        uint8_t *memory = NULL;
        assert(!posix_memalign((void **)&memory, (size_t)getpagesize(), 8u << 20));
        memset(memory, 0x5a, 8u << 20);
        /* Reject impossible native allocations before invoking Metal's
         * validation layer, including a span beyond the device buffer limit. */
        assert(device.maxBufferLength <= UINT64_MAX - (uint64_t)getpagesize());
        assert(!sky_mtl_memory_pool_create(memory,device.maxBufferLength+(uint64_t)getpagesize()));
        sky_mtl_texture_properties_t invalid_samples={
            .texture_type=SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE,.pixel_format=MTLPixelFormatRGBA8Unorm,
            .storage_mode=SKY_MTL_STORAGE_PRIVATE,.width=4,.height=4,.depth=1,.array_length=1,
            .mipmap_level_count=1,.sample_count=3,.usage=SKY_MTL_TEXTURE_USAGE_RENDER_TARGET,
        };
        assert(![device supportsTextureSampleCount:3]);
        assert(!sky_mtl_texture_create(&invalid_samples));
        sky_mtl_memory_pool_t *pool = sky_mtl_memory_pool_create(memory, 8u << 20);
        assert(pool);
        uint64_t address = sky_mtl_memory_pool_gpu_address(pool);
        test_array_mip_strides(queue, memory, address);
        test_volume(queue, memory, address);
        test_compressed_blocks(queue, memory, address);
        test_swizzled_view_is_raw_copy(queue, memory, address);
        test_rejections(queue, address);
        assert(!sky_mtl_linear_texture_has_mirrors());
        test_cpu_linear_planes(queue, memory, address, 0);
        assert(!sky_mtl_linear_texture_has_mirrors());
        test_cpu_linear_planes(queue, memory, address, 1);
        assert(sky_mtl_linear_texture_has_mirrors());
        test_linear_mirror_retains_source(queue);
        sky_mtl_memory_pool_destroy(pool);
        free(memory);
        puts("T_NVN_TRANSFER_OK ordered-buffer-texture-array-mip-volume-bc-stride-guards-cpu-linear-video-private-mirror");
    }
    return 0;
}
