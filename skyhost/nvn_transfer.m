#import "nvn_transfer.h"
#import "nvn_metal.h"
#import "nvn/nvn_tex.h"
#import "nvn/nvn_copy_pitch.h"
#import "nvn/nvn_fmt_mtl.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#import <objc/runtime.h>
#include <stdio.h>

@interface SkyLinearTextureBacking : NSObject
@property(nonatomic, strong) id<MTLBuffer> source;
@property(nonatomic) uint64_t offset;
@property(nonatomic) uint64_t row_stride;
@end
@implementation SkyLinearTextureBacking
@end

@interface SkyLinearTexturePipelines : NSObject
@property(nonatomic, strong) id<MTLComputePipelineState> upload;
@property(nonatomic, strong) id<MTLComputePipelineState> download;
@end
@implementation SkyLinearTexturePipelines
@end

static char linear_texture_backing_key;
static int linear_texture_has_mirrors;

int sky_mtl_linear_texture_has_mirrors(void) {
    return __atomic_load_n(&linear_texture_has_mirrors, __ATOMIC_ACQUIRE);
}

static SkyLinearTextureBacking *linear_texture_backing(id<MTLTexture> texture,
                                                       id<MTLTexture> *base) {
    while (texture) {
        SkyLinearTextureBacking *backing = objc_getAssociatedObject(texture, &linear_texture_backing_key);
        if (backing) {
            if (base) *base = texture;
            return backing;
        }
        id<MTLTexture> parent = texture.parentTexture;
        if (parent == texture) break;
        texture = parent;
    }
    return nil;
}

static SkyLinearTexturePipelines *linear_texture_pipelines(id<MTLDevice> device) {
    static NSObject *lock;
    static NSMapTable *pipelines;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        lock = [NSObject new];
        pipelines = [NSMapTable strongToStrongObjectsMapTable];
    });
    @synchronized(lock) {
        SkyLinearTexturePipelines *cached = [pipelines objectForKey:device];
        if (cached) return cached;
        /* Authored copies use byte-addressed source rows. Buffer-texture blits
         * have platform-specific pitch alignment; these preserve NVN's pitch
         * and arbitrary valid offsets without changing CPU storage. */
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Rows { ulong offset; ulong stride; };\n"
            "kernel void copy_to_texture(device const uchar *bytes [[buffer(0)]],"
            "constant Rows &rows [[buffer(1)]],texture2d<float,access::write> image [[texture(0)]],"
            "uint2 p [[thread_position_in_grid]]) {"
            "if(p.x<image.get_width()&&p.y<image.get_height())"
            "image.write(float4(float(bytes[rows.offset+ulong(p.y)*rows.stride+p.x])/255.0f,0,0,1),p);}\n"
            "kernel void copy_to_buffer(device uchar *bytes [[buffer(0)]],"
            "constant Rows &rows [[buffer(1)]],texture2d<float,access::read> image [[texture(0)]],"
            "uint2 p [[thread_position_in_grid]]) {"
            "if(p.x<image.get_width()&&p.y<image.get_height())"
            "bytes[rows.offset+ulong(p.y)*rows.stride+p.x]=uchar(round(clamp(image.read(p).r,0.0f,1.0f)*255.0f));}\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) {
            fprintf(stderr, "NVN_LINEAR_TEXTURE_ERROR reason=copy-shader error=%s\n", error.description.UTF8String);
            return nil;
        }
        SkyLinearTexturePipelines *created = [SkyLinearTexturePipelines new];
        created.upload = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"copy_to_texture"] error:&error];
        created.download = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"copy_to_buffer"] error:&error];
        if (!created.upload || !created.download) {
            fprintf(stderr, "NVN_LINEAR_TEXTURE_ERROR reason=copy-pipeline error=%s\n", error.description.UTF8String);
            return nil;
        }
        [pipelines setObject:created forKey:device];
        return created;
    }
}

static id<MTLBuffer> linear_source_buffer(uint64_t gpu_address, uint32_t pixel_format,
    uint64_t width, uint64_t height, uint64_t row_stride, uint64_t *offset) {
    if (pixel_format != MTLPixelFormatR8Unorm || !width || !height || width > 16384 || height > 16384 ||
        row_stride < width || row_stride > SIZE_MAX / height) return nil;
    uint64_t extent = row_stride * height;
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)sky_mtl_buffer_native_handle(gpu_address, extent, offset);
    if (!buffer || buffer.storageMode != MTLStorageModeShared ||
        *offset > buffer.length || extent > buffer.length - *offset) return nil;
    return buffer;
}

sky_mtl_texture_t *sky_mtl_texture_create_linear_copy_from_buffer(uint64_t gpu_address,
    uint32_t pixel_format, uint64_t width, uint64_t height, uint64_t row_stride) {
    @autoreleasepool {
        uint64_t offset = 0;
        id<MTLBuffer> buffer = linear_source_buffer(gpu_address, pixel_format, width, height, row_stride, &offset);
        if (!buffer) return NULL;
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        /* ShaderWrite is used only by the internal byte-copy kernel. NVN's
         * video-plane path still excludes shader-write and render-target flags. */
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> texture = [buffer.device newTextureWithDescriptor:descriptor];
        if (!texture) return NULL;
        SkyLinearTextureBacking *backing = [SkyLinearTextureBacking new];
        backing.source = buffer;
        backing.offset = offset;
        backing.row_stride = row_stride;
        objc_setAssociatedObject(texture, &linear_texture_backing_key, backing, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        sky_mtl_texture_t *wrapped = sky_mtl_texture_wrap_native((__bridge void *)texture);
        if (wrapped) __atomic_store_n(&linear_texture_has_mirrors, 1, __ATOMIC_RELEASE);
        return wrapped;
    }
}

int sky_mtl_linear_texture_needs_sync(void *native_texture) {
    return native_texture && sky_mtl_linear_texture_has_mirrors() &&
        linear_texture_backing((__bridge id<MTLTexture>)native_texture, NULL) != nil;
}

static int encode_linear_texture_copy(void *metal_command_buffer, void *native_texture, int flush) {
    if (!metal_command_buffer || !native_texture) return 0;
    @autoreleasepool {
        id<MTLTexture> base = nil;
        SkyLinearTextureBacking *backing = linear_texture_backing((__bridge id<MTLTexture>)native_texture, &base);
        if (!backing) return 1;
        id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)metal_command_buffer;
        if (command.device != base.device || backing.source.device != base.device) return 0;
        SkyLinearTexturePipelines *pipelines = linear_texture_pipelines(base.device);
        if (!pipelines) return 0;
        id<MTLComputePipelineState> pipeline = flush ? pipelines.download : pipelines.upload;
        if (!pipeline.maxTotalThreadsPerThreadgroup) return 0;
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!encoder) return 0;
        uint64_t rows[2] = {backing.offset, backing.row_stride};
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:backing.source offset:0 atIndex:0];
        [encoder setBytes:rows length:sizeof rows atIndex:1];
        [encoder setTexture:base atIndex:0];
        NSUInteger group_width = MIN((NSUInteger)16, pipeline.maxTotalThreadsPerThreadgroup);
        NSUInteger group_height = MIN((NSUInteger)16, pipeline.maxTotalThreadsPerThreadgroup / group_width);
        [encoder dispatchThreads:MTLSizeMake(base.width, base.height, 1)
            threadsPerThreadgroup:MTLSizeMake(group_width, group_height, 1)];
        [encoder endEncoding];
        return 1;
    }
}

int sky_mtl_linear_texture_encode_sync(void *metal_command_buffer, void *native_texture) {
    return encode_linear_texture_copy(metal_command_buffer, native_texture, 0);
}

int sky_mtl_linear_texture_encode_flush(void *metal_command_buffer, void *native_texture) {
    return encode_linear_texture_copy(metal_command_buffer, native_texture, 1);
}

sky_mtl_texture_t *sky_mtl_texture_create_linear_from_buffer(uint64_t gpu_address,
    uint32_t pixel_format, uint64_t width, uint64_t height, uint64_t row_stride) {
    /* This path is deliberately restricted to the observed uncompressed,
     * single-channel Bink planes. Render targets keep their private storage. */
#if TARGET_OS_SIMULATOR
    return sky_mtl_texture_create_linear_copy_from_buffer(gpu_address, pixel_format, width, height, row_stride);
#else
    @autoreleasepool {
        uint64_t offset = 0;
        id<MTLBuffer> buffer = linear_source_buffer(gpu_address, pixel_format, width, height, row_stride, &offset);
        if (!buffer) return NULL;
        id<MTLDevice> device = buffer.device;
        NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pixel_format];
        if (!alignment || offset % alignment || row_stride % alignment) return NULL;
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pixel_format width:width height:height mipmapped:NO];
        descriptor.resourceOptions = buffer.resourceOptions;
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [buffer newTextureWithDescriptor:descriptor offset:offset bytesPerRow:row_stride];
        return texture ? sky_mtl_texture_wrap_native((__bridge void *)texture) : NULL;
    }
#endif
}

static int transfer_error(const sky_mtl_transfer_t *transfer, const char *reason) {
    fprintf(stderr, "NVN_METAL_TRANSFER_ERROR kind=%u flags=%#x reason=%s\n",
            transfer ? transfer->kind : UINT32_MAX, transfer ? transfer->flags : 0, reason);
    return 0;
}

static NSUInteger slice_count(id<MTLTexture> texture) {
    switch (texture.textureType) {
        case MTLTextureTypeCube: return 6;
        case MTLTextureTypeCubeArray: return texture.arrayLength * 6;
        case MTLTextureType1DArray: case MTLTextureType2DArray:
        case MTLTextureType2DMultisampleArray: return texture.arrayLength;
        default: return 1;
    }
}

static int valid_region(id<MTLTexture> texture, unsigned level, unsigned layer,
                        const int32_t region[6], int format) {
    if (!texture || level >= texture.mipmapLevelCount || texture.sampleCount != 1 ||
        region[0] < 0 || region[1] < 0 || region[2] < 0 ||
        region[3] <= 0 || region[4] <= 0 || region[5] < 0) return 0;
    NSUInteger width = MAX(texture.width >> level, 1u), height = MAX(texture.height >> level, 1u);
    NSUInteger depth = texture.textureType == MTLTextureType3D ? MAX(texture.depth >> level, 1u) : slice_count(texture);
    NSUInteger z = (NSUInteger)region[2] + (texture.textureType == MTLTextureType3D ? 0 : layer);
    NSUInteger count = MAX(region[5], 1);
    if ((NSUInteger)region[0] > width || (NSUInteger)region[3] > width - region[0] ||
        (NSUInteger)region[1] > height || (NSUInteger)region[4] > height - region[1] ||
        z >= depth || count > depth - z || (texture.textureType == MTLTextureType3D && layer)) return 0;
    int bw, bh;
    if (!nvn_fmt_bpb(format, &bw, &bh)) return 0;
    if (region[0] % bw || region[1] % bh) return 0;
    /* The last mip/block may end between block boundaries. Interior copies may not. */
    if ((region[3] % bw && (NSUInteger)(region[0] + region[3]) != width) ||
        (region[4] % bh && (NSUInteger)(region[1] + region[4]) != height)) return 0;
    return 1;
}

static MTLOrigin texture_origin(id<MTLTexture> texture, const int32_t region[6], unsigned z) {
    return MTLOriginMake(region[0], region[1], texture.textureType == MTLTextureType3D ? region[2] + z : 0);
}
static NSUInteger texture_slice(id<MTLTexture> texture, unsigned layer, const int32_t region[6], unsigned z) {
    return texture.textureType == MTLTextureType3D ? 0 : (NSUInteger)layer + region[2] + z;
}

int sky_mtl_transfer_encode(void *metal_command_buffer, const sky_mtl_transfer_t *transfer) {
    if (!metal_command_buffer || !transfer) return transfer_error(transfer, "invalid-argument");
    if (transfer->kind > SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE)
        return transfer_error(transfer, "invalid-kind");
    if (sky_mtl_texture_is_bc_fallback(transfer->source) ||
        sky_mtl_texture_is_bc_fallback(transfer->destination))
        return transfer_error(transfer, "bc-transfer-requires-command-boundary");
    /* Engine selection has no data effect. Filtering has no data effect for
     * equal-size copies. Mirroring/scaling require a sampling pass. */
    if (transfer->flags & ~3u) return transfer_error(transfer, "unsupported-copy-flags");
    @autoreleasepool {
        id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)metal_command_buffer;
        id<MTLTexture> source = (__bridge id<MTLTexture>)sky_mtl_texture_native_handle(transfer->source);
        id<MTLTexture> destination = (__bridge id<MTLTexture>)sky_mtl_texture_native_handle(transfer->destination);
        if (transfer->kind == SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE) {
            if (!valid_region(source, transfer->source_level, transfer->source_layer,
                              transfer->source_region, transfer->source_format) ||
                !valid_region(destination, transfer->destination_level, transfer->destination_layer,
                              transfer->destination_region, transfer->destination_format))
                return transfer_error(transfer, "invalid-texture-region");
            const int32_t *from = transfer->source_region, *to = transfer->destination_region;
            if (from[3] != to[3] || from[4] != to[4] || MAX(from[5], 1) != MAX(to[5], 1) ||
                source.pixelFormat != destination.pixelFormat)
                return transfer_error(transfer, "unsupported-scaling-or-format-conversion");
            if (!sky_mtl_linear_texture_encode_sync(metal_command_buffer, (__bridge void *)source) ||
                !sky_mtl_linear_texture_encode_sync(metal_command_buffer, (__bridge void *)destination))
                return transfer_error(transfer, "linear-texture-sync");
            id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
            if (!encoder) return transfer_error(transfer, "encoder-creation");
            for (unsigned z = 0; z < (unsigned)MAX(from[5], 1); z++)
                [encoder copyFromTexture:source sourceSlice:texture_slice(source, transfer->source_layer, from, z)
                    sourceLevel:transfer->source_level sourceOrigin:texture_origin(source, from, z)
                    sourceSize:MTLSizeMake(from[3], from[4], 1) toTexture:destination
                    destinationSlice:texture_slice(destination, transfer->destination_layer, to, z)
                    destinationLevel:transfer->destination_level destinationOrigin:texture_origin(destination, to, z)];
            [encoder endEncoding];
            return sky_mtl_linear_texture_encode_flush(metal_command_buffer, (__bridge void *)destination);
        }
        int upload = transfer->kind == SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE;
        id<MTLTexture> texture = upload ? destination : source;
        int format = upload ? transfer->destination_format : transfer->source_format;
        unsigned level = upload ? transfer->destination_level : transfer->source_level;
        unsigned layer = upload ? transfer->destination_layer : transfer->source_layer;
        const int32_t *region = upload ? transfer->destination_region : transfer->source_region;
        if (!valid_region(texture, level, layer, region, format))
            return transfer_error(transfer, "invalid-texture-region");
        uint32_t pixel_format;
        if (nvn_fmt_to_mtl(format, &pixel_format, NULL) != 1 || pixel_format != texture.pixelFormat)
            return transfer_error(transfer, "buffer-format-requires-conversion");
        int bw, bh, bpb = nvn_fmt_bpb(format, &bw, &bh);
        size_t row, image, extent;
        if (!nvn_copy_pitch(region[3], region[4], region[5], bw, bh, bpb,
                            transfer->row_stride, transfer->image_stride, &row, &image, &extent) ||
            row % (size_t)bpb || image % (size_t)bpb)
            return transfer_error(transfer, "invalid-buffer-pitch");
        uint64_t offset = 0;
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)sky_mtl_buffer_native_handle(transfer->buffer_address, extent, &offset);
        if (!buffer || offset > buffer.length || extent > buffer.length - offset)
            return transfer_error(transfer, "unregistered-buffer-range");
        if (!sky_mtl_linear_texture_encode_sync(metal_command_buffer, (__bridge void *)texture))
            return transfer_error(transfer, "linear-texture-sync");
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        if (!encoder) return transfer_error(transfer, "encoder-creation");
        for (unsigned z = 0; z < (unsigned)MAX(region[5], 1); z++) {
            MTLOrigin origin = texture_origin(texture, region, z);
            NSUInteger slice = texture_slice(texture, layer, region, z);
            MTLSize size = MTLSizeMake(region[3], region[4], 1);
            if (upload)
                [encoder copyFromBuffer:buffer sourceOffset:offset + z * image sourceBytesPerRow:row
                    sourceBytesPerImage:image sourceSize:size toTexture:texture destinationSlice:slice
                    destinationLevel:level destinationOrigin:origin];
            else
                [encoder copyFromTexture:texture sourceSlice:slice sourceLevel:level sourceOrigin:origin
                    sourceSize:size toBuffer:buffer destinationOffset:offset + z * image
                    destinationBytesPerRow:row destinationBytesPerImage:image];
        }
        [encoder endEncoding];
        return upload ? sky_mtl_linear_texture_encode_flush(metal_command_buffer, (__bridge void *)texture) : 1;
    }
}
