#import "nvn_metal.h"
#import "nvn/nvn_tex.h"
#import "nvn/nvn_fmt_mtl.h"
#import "nvn/nvn_copy_pitch.h"
#import "nvn_bc.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

struct sky_mtl_memory_pool {
    struct sky_mtl_memory_pool *next;
    const void *buffer;
    void *memory;
    uintptr_t base;
    uint64_t size;
    uint64_t wrapped_size;
    uint64_t gpu_address;
    uint8_t retained_clone;
    uint8_t shadowed_by_newer;
};

static id<MTLDevice> g_device;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_active_count;
static sky_mtl_memory_pool_t *g_pools;
static sky_mtl_memory_pool_t *g_last_pool;
enum {SKY_MTL_BUFFER_LOOKUP_SLOTS=4096};
typedef struct {uint64_t address,size;sky_mtl_memory_pool_t *pool;} sky_mtl_buffer_lookup_t;
static sky_mtl_buffer_lookup_t g_buffer_lookup[SKY_MTL_BUFFER_LOOKUP_SLOTS];

static size_t metal_buffer_lookup_slot(uint64_t address,uint64_t size) {
    uint64_t hash=address^((size<<1)|(size>>63));
    hash^=hash>>33;hash*=UINT64_C(0xff51afd7ed558ccd);hash^=hash>>33;
    return (size_t)hash&(SKY_MTL_BUFFER_LOOKUP_SLOTS-1);
}
static int metal_pool_contains(const sky_mtl_memory_pool_t *pool,uint64_t address,uint64_t size) {
    return address>=pool->gpu_address&&address-pool->gpu_address<pool->size&&
        size<=pool->size-(address-pool->gpu_address);
}
static void metal_update_pool_shadowing(void) {
    for (sky_mtl_memory_pool_t *p = g_pools; p; p = p->next) {
        p->shadowed_by_newer = 0;
        for (sky_mtl_memory_pool_t *newer = g_pools; newer && newer != p; newer = newer->next) {
            if (p->gpu_address < newer->gpu_address + newer->size &&
                newer->gpu_address < p->gpu_address + p->size) {
                p->shadowed_by_newer = 1;
                break;
            }
        }
    }
}

static id<MTLDevice> shared_device(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_device) g_device = MTLCreateSystemDefaultDevice();
    id<MTLDevice> device = g_device;
    pthread_mutex_unlock(&g_lock);
    return device;
}

sky_mtl_memory_pool_t *sky_mtl_memory_pool_create(void *memory, uint64_t size) {
    if (!memory || !size || size > UINTPTR_MAX) return NULL;
    uintptr_t address = (uintptr_t)memory;
    if (address > UINTPTR_MAX - (uintptr_t)size) return NULL;
    uintptr_t end = address + (uintptr_t)size;
    uintptr_t page = (uintptr_t)getpagesize();
    if (end > UINTPTR_MAX - (page - 1)) return NULL;
    uintptr_t base = address & ~(page - 1);
    uintptr_t wrapped_end = (end + page - 1) & ~(page - 1);
    if (wrapped_end <= base) return NULL;

    @autoreleasepool {
        id<MTLDevice> device = shared_device();
        /* MTLSimDevice reports nonunified memory even when an mmap-backed
         * Shared no-copy buffer aliases CPU memory coherently. Validate that
         * individual buffer below instead of rejecting the whole device. */
        if (!device || (!TARGET_OS_SIMULATOR && !device.hasUnifiedMemory) ||
            wrapped_end - base > device.maxBufferLength) return NULL;
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:(void *)base
                                                          length:wrapped_end - base
                                                         options:MTLResourceStorageModeShared
                                                     deallocator:nil];
        if (!buffer || buffer.storageMode != MTLStorageModeShared || buffer.contents != (void *)base)
            return NULL;
        uint64_t offset = address - base;
        if (buffer.gpuAddress > UINT64_MAX - offset) return NULL;
        sky_mtl_memory_pool_t *pool = calloc(1, sizeof *pool);
        if (!pool) return NULL;
        pool->buffer = CFBridgingRetain(buffer);
        pool->memory = memory;
        pool->base = base;
        pool->size = size;
        pool->wrapped_size = wrapped_end - base;
        pool->gpu_address = buffer.gpuAddress + offset;
        pthread_mutex_lock(&g_lock);
        pool->next = g_pools;
        g_pools = pool;
        metal_update_pool_shadowing();
        /* GPU ranges can alias. A new head may shadow a cached exact query. */
        memset(g_buffer_lookup,0,sizeof g_buffer_lookup);
        g_last_pool = NULL;
        g_active_count++;
        pthread_mutex_unlock(&g_lock);
        return pool;
    }
}

sky_mtl_memory_pool_t *sky_mtl_memory_pool_retain(sky_mtl_memory_pool_t *pool) {
    if (!pool || !pool->buffer) return NULL;
    sky_mtl_memory_pool_t *copy = calloc(1, sizeof *copy);
    if (!copy) return NULL;
    *copy = *pool;
    copy->next = NULL;
    copy->retained_clone = 1;
    CFRetain(copy->buffer);
    pthread_mutex_lock(&g_lock);
    g_active_count++;
    pthread_mutex_unlock(&g_lock);
    return copy;
}

void sky_mtl_memory_pool_destroy(sky_mtl_memory_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_lock);
    if (!pool->retained_clone) {
        sky_mtl_memory_pool_t **link = &g_pools;
        while (*link && *link != pool) link = &(*link)->next;
        if (*link) *link = pool->next;
        metal_update_pool_shadowing();
        /* Invalidate borrowed pool pointers before releasing native storage. */
        memset(g_buffer_lookup,0,sizeof g_buffer_lookup);
        g_last_pool = NULL;
    }
    if (g_active_count) g_active_count--;
    pthread_mutex_unlock(&g_lock);
    if(pool->buffer)CFRelease(pool->buffer);
    pool->buffer=NULL;
    free(pool);
}

void *sky_mtl_memory_pool_contents(const sky_mtl_memory_pool_t *pool) {
    if (!pool || !pool->buffer) return NULL;
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)pool->buffer;
    return (uint8_t *)buffer.contents + ((uintptr_t)pool->memory - pool->base);
}

uint64_t sky_mtl_memory_pool_gpu_address(const sky_mtl_memory_pool_t *pool) {
    return pool ? pool->gpu_address : 0;
}

uint64_t sky_mtl_memory_pool_wrapped_size(const sky_mtl_memory_pool_t *pool) {
    return pool ? pool->wrapped_size : 0;
}

int sky_mtl_memory_pool_is_shared(const sky_mtl_memory_pool_t *pool) {
    if (!pool || !pool->buffer) return 0;
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)pool->buffer;
    return buffer.storageMode == MTLStorageModeShared;
}

void sky_mtl_memory_pool_set_debug_label(sky_mtl_memory_pool_t *pool, const char *label) {
    if (!pool || !pool->buffer) return;
    @autoreleasepool {
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)pool->buffer;
        buffer.label = label ? [NSString stringWithUTF8String:label] : nil;
    }
}

size_t sky_mtl_memory_pool_active_count(void) {
    pthread_mutex_lock(&g_lock);
    size_t count = g_active_count;
    pthread_mutex_unlock(&g_lock);
    return count;
}

typedef struct {
    size_t offset, row, image;
    uint64_t width, height, slices;
} sky_mtl_bc_level_t;

typedef struct {
    unsigned references;
    pthread_mutex_t lock;
    uint64_t levels;
    size_t size;
    uint8_t *bytes;
    sky_mtl_bc_level_t level[];
} sky_mtl_bc_backing_t;

struct sky_mtl_texture {
    const void *texture;
    sky_mtl_texture_properties_t properties;
    uint64_t resource_id;
    /* Properties retain the guest's compressed identity. Metal sees the
     * decoded format, while views share original blocks for lossless copies. */
    sky_mtl_bc_backing_t *bc;
    uint64_t bc_level, bc_slice;
};

static uint64_t texture_slices(const sky_mtl_texture_properties_t *p,uint64_t level) {
    if(p->texture_type==SKY_MTL_TEXTURE_TYPE_3D)return MAX(p->depth>>level,1ull);
    uint64_t count=p->array_length;
    if(p->texture_type==SKY_MTL_TEXTURE_TYPE_CUBE||p->texture_type==SKY_MTL_TEXTURE_TYPE_CUBE_ARRAY) {
        if(count>UINT64_MAX/6)return 0;
        count*=6;
    }
    return count;
}

static sky_mtl_bc_backing_t *bc_backing_create(const sky_mtl_texture_properties_t *p,size_t block_bytes) {
    if(!p->width||!p->height||!p->depth||!p->array_length||!p->mipmap_level_count||
       p->mipmap_level_count>64||p->width>UINT32_MAX||p->height>UINT32_MAX)return NULL;
    sky_mtl_bc_backing_t *out=calloc(1,sizeof(*out)+p->mipmap_level_count*sizeof(out->level[0]));
    if(!out)return NULL;
    out->levels=p->mipmap_level_count;
    for(uint64_t level=0;level<out->levels;level++) {
        sky_mtl_bc_level_t *l=&out->level[level];
        l->width=MAX(p->width>>level,1ull);l->height=MAX(p->height>>level,1ull);
        l->slices=texture_slices(p,level);
        size_t rows=((size_t)l->height+3)/4,blocks=((size_t)l->width+3)/4;
        if(!l->slices||blocks>SIZE_MAX/block_bytes)goto fail;
        l->row=blocks*block_bytes;
        if(rows>SIZE_MAX/l->row)goto fail;
        l->image=l->row*rows;l->offset=out->size;
        if(l->slices>SIZE_MAX/l->image||l->image*l->slices>SIZE_MAX-out->size)goto fail;
        out->size+=l->image*l->slices;
        if(out->size>(512u<<20))goto fail;
    }
    out->bytes=calloc(1,out->size);
    if(!out->bytes||pthread_mutex_init(&out->lock,NULL))goto fail;
    out->references=1;return out;
fail:
    free(out->bytes);free(out);return NULL;
}

static void bc_backing_release(sky_mtl_bc_backing_t *backing) {
    if(backing&&__atomic_sub_fetch(&backing->references,1,__ATOMIC_ACQ_REL)==0) {
        pthread_mutex_destroy(&backing->lock);free(backing->bytes);free(backing);
    }
}

static int bc_same_family(uint32_t a,uint32_t b) {
    if(a==b)return 1;
    return (a==130||a==131||a==132||a==133||a==134||a==135||a==152||a==153)&&((a^b)==1);
}

int sky_mtl_texture_is_bc_fallback(const sky_mtl_texture_t *texture) {
    return texture&&texture->bc!=NULL;
}

static int bc_region(const sky_mtl_texture_t *texture,uint64_t level,uint64_t slice,
    const int region[6],sky_bc_format_info_t *info,size_t *row,size_t *rows,uint64_t *first) {
    if(!texture||!texture->texture||!region||level>=texture->properties.mipmap_level_count||
       !sky_bc_format_info(texture->properties.pixel_format,info))return 0;
    int x=region[0],y=region[1],z=region[2],w=region[3],h=region[4],d=MAX(region[5],1);
    if(x<0||y<0||z<0||w<=0||h<=0||region[5]<0||x%4||y%4)return 0;
    uint64_t width=MAX(texture->properties.width>>level,1ull),height=MAX(texture->properties.height>>level,1ull);
    uint64_t slices=texture_slices(&texture->properties,level);
    int volume=texture->properties.texture_type==SKY_MTL_TEXTURE_TYPE_3D;
    if((volume&&slice)||slice>=slices||(uint64_t)z>=slices-slice||(uint64_t)d>slices-slice-z||
       (uint64_t)x>=width||(uint64_t)w>width-x||(uint64_t)y>=height||(uint64_t)h>height-y||
       (w%4&&(uint64_t)x+w!=width)||(h%4&&(uint64_t)y+h!=height))return 0;
    *row=((size_t)w+3)/4*info->block_bytes;*rows=((size_t)h+3)/4;*first=slice+(uint64_t)z;
    return 1;
}

static int bc_backing_region(sky_mtl_texture_t *texture,uint64_t level,uint64_t slice,
    const int region[6],void *data,size_t row_pitch,size_t image_pitch,int write) {
    sky_bc_format_info_t info;size_t row,rows;uint64_t first;
    if(!texture->bc||!bc_region(texture,level,slice,region,&info,&row,&rows,&first)||row_pitch<row||
       rows>SIZE_MAX/row_pitch||image_pitch<rows*row_pitch)return 0;
    sky_mtl_bc_backing_t *backing=texture->bc;
    if(texture->bc_level+level>=backing->levels)return 0;
    sky_mtl_bc_level_t *l=&backing->level[texture->bc_level+level];
    uint64_t start=texture->bc_slice+first,count=(uint64_t)MAX(region[5],1);
    if(start>=l->slices||count>l->slices-start)return 0;
    size_t x=(size_t)region[0]/4*info.block_bytes,y=(size_t)region[1]/4;
    pthread_mutex_lock(&backing->lock);
    for(uint64_t z=0;z<count;z++)for(size_t r=0;r<rows;r++) {
        uint8_t *stored=backing->bytes+l->offset+(start+z)*l->image+(y+r)*l->row+x;
        uint8_t *linear=(uint8_t *)data+z*image_pitch+r*row_pitch;
        if(write)memcpy(stored,linear,row);else memcpy(linear,stored,row);
    }
    pthread_mutex_unlock(&backing->lock);return 1;
}

typedef struct {
    uint64_t submits,completed_submits,cpu_ns,wall_ns,wait_ns,gpu_ns,command_buffers;
    uint64_t draws,dispatches,render_encoders,merged_draws,compute_encoders,blit_encoders;
    uint64_t argument_buffers,depth_hits,depth_misses;
    uint64_t barriers,writable_draws,feedback_draws,pass_changes;
} sky_mtl_profile_t;
struct sky_mtl_command_queue {
    const void *queue;
    uint64_t counters[32], errors;
    sky_mtl_operation_t *replay;
    const void *visibility;
    uint32_t visibility_used,visibility_enabled;
    sky_mtl_profile_t profile;
};

static int metal_profile_enabled(void) {
    static dispatch_once_t once;static int enabled;
    dispatch_once(&once,^{const char *value=getenv("SKY_METAL_PROFILE");enabled=value&&*value&&strcmp(value,"0")!=0;});
    return enabled;
}
static int metal_reuse_encoders_enabled(void) {
    static dispatch_once_t once;static int enabled;
    dispatch_once(&once,^{const char *value=getenv("SKY_METAL_REUSE_ENCODERS");enabled=!value||strcmp(value,"0")!=0;});
    return enabled;
}
static uint64_t metal_clock_ns(clockid_t clock) {
    struct timespec time;if(clock_gettime(clock,&time))return 0;
    return (uint64_t)time.tv_sec*UINT64_C(1000000000)+(uint64_t)time.tv_nsec;
}
#define PROFILE_ADD(q,field,value) __atomic_add_fetch(&(q)->profile.field,(uint64_t)(value),__ATOMIC_RELAXED)
#define PROFILE_LOAD(q,field) __atomic_load_n(&(q)->profile.field,__ATOMIC_RELAXED)
static void metal_profile_gpu(sky_mtl_command_queue_t *queue,id<MTLCommandBuffer> buffer) {
    double start=buffer.GPUStartTime,end=buffer.GPUEndTime;
    if(end>start&&start>0)PROFILE_ADD(queue,gpu_ns,(uint64_t)((end-start)*1.e9));
    PROFILE_ADD(queue,command_buffers,1);
}
static void metal_profile_log(const sky_mtl_command_queue_t *queue,const char *reason) {
    fprintf(stderr,"NVN_METAL_PROFILE queue=%p reason=%s time_ns=%llu submits=%llu completed_submits=%llu cpu_encode_ns=%llu encode_wall_ns=%llu sync_wait_ns=%llu gpu_ns=%llu command_buffers=%llu draws=%llu dispatches=%llu render_encoders=%llu merged_draws=%llu compute_encoders=%llu blit_encoders=%llu argument_buffers=%llu depth_hits=%llu depth_misses=%llu barriers=%llu writable_draws=%llu feedback_draws=%llu pass_changes=%llu\n",
        (const void *)queue,reason,(unsigned long long)metal_clock_ns(CLOCK_MONOTONIC),
        (unsigned long long)PROFILE_LOAD(queue,submits),(unsigned long long)PROFILE_LOAD(queue,completed_submits),
        (unsigned long long)PROFILE_LOAD(queue,cpu_ns),(unsigned long long)PROFILE_LOAD(queue,wall_ns),
        (unsigned long long)PROFILE_LOAD(queue,wait_ns),(unsigned long long)PROFILE_LOAD(queue,gpu_ns),
        (unsigned long long)PROFILE_LOAD(queue,command_buffers),(unsigned long long)PROFILE_LOAD(queue,draws),
        (unsigned long long)PROFILE_LOAD(queue,dispatches),(unsigned long long)PROFILE_LOAD(queue,render_encoders),
        (unsigned long long)PROFILE_LOAD(queue,merged_draws),(unsigned long long)PROFILE_LOAD(queue,compute_encoders),
        (unsigned long long)PROFILE_LOAD(queue,blit_encoders),(unsigned long long)PROFILE_LOAD(queue,argument_buffers),
        (unsigned long long)PROFILE_LOAD(queue,depth_hits),(unsigned long long)PROFILE_LOAD(queue,depth_misses),
        (unsigned long long)PROFILE_LOAD(queue,barriers),(unsigned long long)PROFILE_LOAD(queue,writable_draws),
        (unsigned long long)PROFILE_LOAD(queue,feedback_draws),(unsigned long long)PROFILE_LOAD(queue,pass_changes));
}

static size_t g_command_queue_count;
static void metal_replay_destroy(sky_mtl_command_queue_t *queue);

sky_mtl_command_queue_t *sky_mtl_command_queue_create(void) {
    @autoreleasepool {
        id<MTLDevice> device = shared_device();
        if (!device) return NULL;
        id<MTLCommandQueue> command_queue = [device newCommandQueue];
        if (!command_queue) return NULL;
        sky_mtl_command_queue_t *queue = calloc(1, sizeof *queue);
        if (!queue) return NULL;
        queue->queue = CFBridgingRetain(command_queue);
        pthread_mutex_lock(&g_lock);
        g_command_queue_count++;
        pthread_mutex_unlock(&g_lock);
        return queue;
    }
}

void sky_mtl_command_queue_destroy(sky_mtl_command_queue_t *queue) {
    if (!queue) return;
    if(metal_profile_enabled()&&PROFILE_LOAD(queue,submits))metal_profile_log(queue,"destroy");
    metal_replay_destroy(queue);
    if(queue->visibility)CFRelease(queue->visibility);
    if (queue->queue) CFRelease(queue->queue);
    queue->queue = NULL;
    pthread_mutex_lock(&g_lock);
    if (g_command_queue_count) g_command_queue_count--;
    pthread_mutex_unlock(&g_lock);
    free(queue);
}

size_t sky_mtl_command_queue_active_count(void) {
    pthread_mutex_lock(&g_lock);
    size_t count = g_command_queue_count;
    pthread_mutex_unlock(&g_lock);
    return count;
}

int sky_mtl_command_queue_submit_empty(sky_mtl_command_queue_t *queue, uint64_t delay_ns,
                                       sky_mtl_completion_fn completion, void *context) {
    if (!queue || !queue->queue || !completion) return 0;
    id<MTLCommandQueue> object = (__bridge id<MTLCommandQueue>)queue->queue;
    void (^submit)(void) = ^{
        @autoreleasepool {
            id<MTLCommandBuffer> buffer = [object commandBuffer];
            [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                (void)completed;
                completion(context);
            }];
            [buffer commit];
        }
    };
    if (delay_ns) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay_ns),
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), submit);
    } else {
        submit();
    }
    return 1;
}

int sky_mtl_command_queue_submit_clear(sky_mtl_command_queue_t *queue, const sky_mtl_clear_pass_t *pass,
                                       sky_mtl_completion_fn completion, void *context) {
    if (!queue || !queue->queue || !pass || !completion || pass->color_count > 8) return 0;
    @autoreleasepool {
        id<MTLCommandQueue> object = (__bridge id<MTLCommandQueue>)queue->queue;
        id<MTLCommandBuffer> buffer = [object commandBuffer];
        if (!buffer) return 0;
        MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        for (NSUInteger index = 0; index < pass->color_count; index++) {
            sky_mtl_texture_t *wrapped = pass->textures[index];
            id<MTLTexture> texture = wrapped && wrapped->texture
                ? (__bridge id<MTLTexture>)wrapped->texture : nil;
            if (!texture) continue;
            descriptor.colorAttachments[index].texture = texture;
            descriptor.colorAttachments[index].storeAction = MTLStoreActionStore;
            if (pass->has_color_clear && index == pass->clear_index) {
                descriptor.colorAttachments[index].loadAction = MTLLoadActionClear;
                descriptor.colorAttachments[index].clearColor = MTLClearColorMake(
                    pass->colors[index][0], pass->colors[index][1],
                    pass->colors[index][2], pass->colors[index][3]);
            } else {
                descriptor.colorAttachments[index].loadAction = MTLLoadActionLoad;
            }
        }
        if (pass->depth_texture) {
            sky_mtl_texture_t *wrapped = pass->depth_texture;
            id<MTLTexture> texture = wrapped && wrapped->texture
                ? (__bridge id<MTLTexture>)wrapped->texture : nil;
            if (pass->depth_mask) {
                descriptor.depthAttachment.texture = texture;
                descriptor.depthAttachment.loadAction = MTLLoadActionClear;
                descriptor.depthAttachment.storeAction = MTLStoreActionStore;
                descriptor.depthAttachment.clearDepth = pass->clear_depth;
            }
            if (pass->stencil_mask &&
                (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ||
                 texture.pixelFormat == MTLPixelFormatStencil8)) {
                descriptor.stencilAttachment.texture = texture;
                descriptor.stencilAttachment.loadAction = MTLLoadActionClear;
                descriptor.stencilAttachment.storeAction = MTLStoreActionStore;
                descriptor.stencilAttachment.clearStencil = pass->clear_stencil;
            }
        }
        [[buffer renderCommandEncoderWithDescriptor:descriptor] endEncoding];
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            (void)completed;
            completion(context);
        }];
        [buffer commit];
    }
    return 1;
}

static int sky_mtl_texture_upload_blit(sky_mtl_texture_t *texture, uint64_t level, uint64_t slice,
                                       const int region[6], const void *data, size_t source_pitch,
                                       size_t block_bytes, int block_width, int block_height) {
    if (!texture || !texture->texture || !region || !data || level >= texture->properties.mipmap_level_count)
        return 0;
    int x = region[0], y = region[1], z = region[2];
    int width = region[3], height = region[4], depth = region[5]?region[5]:1;
    if (x < 0 || y < 0 || z < 0 || width <= 0 || height <= 0 || depth <= 0 ||
        !block_bytes || !block_width || !block_height) return 0;
    uint32_t type = texture->properties.texture_type;
    int is3d=type==SKY_MTL_TEXTURE_TYPE_3D;
    uint64_t layers=is3d?MAX(texture->properties.depth>>level,1ull):texture->properties.array_length;
    if(type==SKY_MTL_TEXTURE_TYPE_CUBE||type==SKY_MTL_TEXTURE_TYPE_CUBE_ARRAY)layers*=6;
    if((is3d&&slice)||(!is3d&&slice>=layers))return 0;
    uint64_t level_width=MAX(texture->properties.width>>level,1ull),level_height=MAX(texture->properties.height>>level,1ull);
    if((uint64_t)x+width>level_width||(uint64_t)y+height>level_height||
        (uint64_t)z+depth>layers-(is3d?0:slice)||x%block_width||y%block_height)return 0;
    size_t row_blocks = ((size_t)width + block_width - 1) / block_width;
    if (row_blocks > SIZE_MAX / block_bytes || source_pitch < row_blocks * block_bytes) return 0;
    size_t rows = ((size_t)height + block_height - 1) / block_height;
    if (rows > SIZE_MAX / source_pitch) return 0;
    size_t image = source_pitch * rows;
    if ((size_t)depth > SIZE_MAX / image) return 0;
    size_t expected = image * (size_t)depth;
    if (!expected || expected > (512u << 20) || expected > UINT32_MAX) return 0;
    uint8_t *cpu = malloc(expected);
    if (!cpu) return 0;
    vm_size_t copied = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(), (vm_address_t)data, (vm_size_t)expected,
                                             (vm_address_t)cpu, &copied);
    if (result != KERN_SUCCESS || copied != expected) {
        free(cpu);
        return 0;
    }
    int valid = 0;
    @autoreleasepool {
        id<MTLDevice> device = shared_device();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> source = [device newBufferWithBytes:cpu length:expected options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        id<MTLTexture> object = (__bridge id<MTLTexture>)texture->texture;
        if (source && buffer && sky_mtl_linear_texture_encode_sync((__bridge void *)buffer,(__bridge void *)object)) {
            id<MTLBlitCommandEncoder> encoder = [buffer blitCommandEncoder];
            if(!encoder){free(cpu);return 0;}
            for(int layer=0;layer<(is3d?1:depth);layer++) {
                [encoder copyFromBuffer:source sourceOffset:is3d?0:(NSUInteger)layer*image
                    sourceBytesPerRow:source_pitch sourceBytesPerImage:image
                    sourceSize:MTLSizeMake(width,height,is3d?depth:1) toTexture:object
                    destinationSlice:is3d?0:slice+z+(NSUInteger)layer destinationLevel:level
                    destinationOrigin:MTLOriginMake(x,y,is3d?z:0) options:MTLBlitOptionNone];
            }
            [encoder endEncoding];
            if(!sky_mtl_linear_texture_encode_flush((__bridge void *)buffer,(__bridge void *)object)) {
                free(cpu);return 0;
            }
            [buffer commit];
            [buffer waitUntilCompleted];
            valid = buffer.status == MTLCommandBufferStatusCompleted;
        }
    }
    free(cpu);
    return valid;
}

static int bc_upload_blocks(sky_mtl_texture_t *texture,uint64_t level,uint64_t slice,
    const int region[6],const void *data,size_t pitch,size_t image) {
    sky_bc_format_info_t info;size_t row,rows;uint64_t first;
    if(!data||!bc_region(texture,level,slice,region,&info,&row,&rows,&first)||pitch<row||
       rows>SIZE_MAX/pitch||image<pitch*rows)return 0;
    size_t count=(size_t)MAX(region[5],1);
    if(count>SIZE_MAX/image||image*count>(512u<<20))return 0;
    size_t bytes=image*count;
    uint8_t *blocks=malloc(bytes);
    if(!blocks)return 0;
    vm_size_t copied=0;
    if(vm_read_overwrite(mach_task_self(),(vm_address_t)data,bytes,(vm_address_t)blocks,&copied)!=KERN_SUCCESS||copied!=bytes) {
        free(blocks);return 0;
    }
    int valid=1;
    if(!texture->bc) {
        for(size_t z=0;z<count&&valid;z++) {
            int part[6];memcpy(part,region,sizeof part);part[2]+=(int)z;part[5]=1;
            valid=sky_mtl_texture_upload_blit(texture,level,slice,part,blocks+z*image,pitch,info.block_bytes,4,4);
        }
    } else {
        size_t width=(size_t)region[3],height=(size_t)region[4];
        if(width>(SIZE_MAX-255)/info.bytes_per_pixel){free(blocks);return 0;}
        size_t decoded_row=(width*info.bytes_per_pixel+255)&~(size_t)255;
        if(height>SIZE_MAX/decoded_row||count>SIZE_MAX/(decoded_row*height)||
           decoded_row*height*count>(512u<<20)){free(blocks);return 0;}
        size_t decoded_image=decoded_row*height,total=decoded_image*count;
        uint8_t *decoded=calloc(1,total);
        if(!decoded){free(blocks);return 0;}
        for(size_t z=0;z<count&&valid;z++)
            valid=sky_bc_decode_region(texture->properties.pixel_format,(uint32_t)width,(uint32_t)height,
                blocks+z*image,image,pitch,decoded+z*decoded_image,decoded_image,decoded_row);
        if(valid)valid=sky_mtl_texture_upload_blit(texture,level,slice,region,decoded,decoded_row,info.bytes_per_pixel,1,1);
        if(valid)valid=bc_backing_region(texture,level,slice,region,blocks,pitch,image,1);
        free(decoded);
    }
    free(blocks);return valid;
}

int sky_mtl_texture_upload(sky_mtl_texture_t *texture, uint64_t level, uint64_t slice,
                           const int region[6], const void *data) {
    if (!texture) return 0;
    uint32_t format = texture->properties.pixel_format;
    size_t bpp = 0;
    if (format == 10) bpp = 1;
    else if (format == 20 || format == 25 || format == 30 ||
             (format >= 40 && format <= 43)) bpp = 2;
    else if (format == 55 || format == 60 || format == 62 || format == 63 || format == 64 || format == 65 ||
             (format >= 70 && format <= 74) || format == 80 || format == 81 ||
             (format >= 90 && format <= 94)) bpp = 4;
    else if ((format >= 103 && format <= 105) || format == 110 ||
             (format >= 112 && format <= 115)) bpp = 8;
    else if (format >= 123 && format <= 125) bpp = 16;
    else return 0;
    if (!region || region[3] <= 0 || (size_t)region[3] > SIZE_MAX / bpp) return 0;
    size_t pitch = (size_t)region[3] * bpp;
    return sky_mtl_texture_upload_blit(texture, level, slice, region, data, pitch, bpp, 1, 1);
}

int sky_mtl_texture_upload_packaged(sky_mtl_texture_t *texture, int nvn_format,
                                    uint64_t level, uint64_t slice, const int region[6], const void *data) {
    int block_width = 0, block_height = 0;
    int block_bytes = nvn_fmt_bpb(nvn_format, &block_width, &block_height);
    if (!block_bytes || !region || region[3] <= 0) return 0;
    size_t row_blocks = ((size_t)region[3] + block_width - 1) / block_width;
    if (row_blocks > SIZE_MAX / (size_t)block_bytes) return 0;
    size_t pitch = row_blocks * (size_t)block_bytes;
    if(texture&&texture->bc) {
        uint32_t logical=0;
        if(nvn_fmt_to_mtl(nvn_format,&logical,NULL)!=1||logical!=texture->properties.pixel_format||
           region[4]<=0||((size_t)region[4]+3)/4>SIZE_MAX/pitch)return 0;
        return bc_upload_blocks(texture,level,slice,region,data,pitch,pitch*(((size_t)region[4]+3)/4));
    }
    return sky_mtl_texture_upload_blit(texture, level, slice, region, data, pitch,
                                       (size_t)block_bytes, block_width, block_height);
}

int sky_mtl_texture_readback(sky_mtl_texture_t *texture, uint64_t level, uint64_t slice, int aspect,
                             void *out, size_t out_size, size_t *read_size) {
    if (!texture || !texture->texture || !out || level >= texture->properties.mipmap_level_count)
        return 0;
    id<MTLTexture> object = (__bridge id<MTLTexture>)texture->texture;
    int is3d=object.textureType==MTLTextureType3D;
    NSUInteger slices=is3d?MAX(object.depth>>level,1ul):object.arrayLength;
    if(object.textureType==MTLTextureTypeCube||object.textureType==MTLTextureTypeCubeArray)slices*=6;
    if(slice>=slices)return 0;
    uint32_t format = texture->properties.pixel_format;
    size_t bpp = 0;
    MTLBlitOption options = MTLBlitOptionNone;
    if (aspect == SKY_MTL_READ_DEPTH) {
        if (format == 250) bpp = 2;
        else if (format == 252) bpp = 4;
        else if (format == 260) bpp = 4;
        else return 0;
        options = MTLBlitOptionDepthFromDepthStencil;
    } else if (aspect == SKY_MTL_READ_STENCIL) {
        if (format != 253 && format != 260) return 0;
        bpp = 1;
        options = MTLBlitOptionStencilFromDepthStencil;
    } else {
        if (format == 10) bpp = 1;
        else if (format == 20 || format == 25) bpp = 2;
        else if (format == 30) bpp = 2;
        else if (format == 55 || format == 60 || format == 65 || format == 92) bpp = 4;
        else if (format == 70 || format == 71 || format == 72 || format == 73 || format == 74 ||
                 format == 80 || format == 81 || format == 90 || format == 91) bpp = 4;
        else if (format == 110 || format == 112 || format == 113 || format == 114 || format == 115) bpp = 8;
        else if (format == 103 || format == 104 || format == 105) bpp = 8;
        else if (format == 123 || format == 124 || format == 125) bpp = 16;
        else return 0;
    }
    NSUInteger width = (NSUInteger)MAX(texture->properties.width >> level, 1ull);
    NSUInteger height = (NSUInteger)MAX(texture->properties.height >> level, 1ull);
    size_t expected = (size_t)width * (size_t)height * bpp;
    if (out_size < expected) return 0;
    @autoreleasepool {
        id<MTLDevice> device = shared_device();
        if (!device) return 0;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        id<MTLBuffer> destination = [device newBufferWithLength:expected options:MTLResourceStorageModeShared];
        if (!buffer || !destination) return 0;
        if(!sky_mtl_linear_texture_encode_sync((__bridge void *)buffer,(__bridge void *)object))return 0;
        id<MTLBlitCommandEncoder> encoder = [buffer blitCommandEncoder];
        [encoder copyFromTexture:object
                     sourceSlice:is3d?0:slice
                     sourceLevel:level
                    sourceOrigin:MTLOriginMake(0, 0,is3d?slice:0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:destination
               destinationOffset:0
          destinationBytesPerRow:width * bpp
        destinationBytesPerImage:expected
                         options:options];
        [encoder endEncoding];
        [buffer commit];
        [buffer waitUntilCompleted];
        if (buffer.status != MTLCommandBufferStatusCompleted) return 0;
        memcpy(out, destination.contents, expected);
    }
    if (read_size) *read_size = expected;
    return 1;
}

int sky_mtl_texture_readback_decoded(sky_mtl_texture_t *texture,uint64_t level,uint64_t slice,
                                    void *out,size_t out_size,size_t *read_size) {
    if(!texture||!texture->texture)return 0;
    sky_mtl_texture_t physical=*texture;
    physical.properties.pixel_format=(uint32_t)((__bridge id<MTLTexture>)texture->texture).pixelFormat;
    return sky_mtl_texture_readback(&physical,level,slice,SKY_MTL_READ_COLOR,out,out_size,read_size);
}

/* The caller must have completed preceding commands before reading a guest
 * buffer or original blocks. Ordinary transfer_encode borrows its command
 * buffer and cannot create that boundary itself. */
static int bc_read_blocks(sky_mtl_texture_t *texture,uint64_t level,uint64_t slice,
    const int region[6],void *data,size_t row_pitch,size_t image_pitch) {
    sky_bc_format_info_t info;size_t row,rows;uint64_t first;
    if(!data||!bc_region(texture,level,slice,region,&info,&row,&rows,&first)||row_pitch<row||
       rows>SIZE_MAX/row_pitch||image_pitch<row_pitch*rows)return 0;
    if(texture->bc)return bc_backing_region(texture,level,slice,region,data,row_pitch,image_pitch,0);
    size_t count=(size_t)MAX(region[5],1);
    if(row>SIZE_MAX-255)return 0;
    size_t staging_row=(row+255)&~(size_t)255;
    if(rows>SIZE_MAX/staging_row||count>SIZE_MAX/(rows*staging_row)||count*rows*staging_row>(512u<<20))return 0;
    size_t staging_image=staging_row*rows,total=staging_image*count;
    @autoreleasepool {
        id<MTLDevice> device=shared_device();id<MTLCommandQueue> queue=[device newCommandQueue];
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBuffer> output=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
        if(!command||!output)return 0;
        id<MTLTexture> native=(__bridge id<MTLTexture>)texture->texture;
        id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
        if(!encoder)return 0;
        int volume=texture->properties.texture_type==SKY_MTL_TEXTURE_TYPE_3D;
        for(size_t z=0;z<count;z++)
            [encoder copyFromTexture:native sourceSlice:volume?0:first+z sourceLevel:level
                sourceOrigin:MTLOriginMake(region[0],region[1],volume?first+z:0)
                sourceSize:MTLSizeMake(region[3],region[4],1) toBuffer:output destinationOffset:z*staging_image
                destinationBytesPerRow:staging_row destinationBytesPerImage:staging_image];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted)return 0;
        for(size_t z=0;z<count;z++)for(size_t y=0;y<rows;y++)
            memcpy((uint8_t *)data+z*image_pitch+y*row_pitch,
                (uint8_t *)output.contents+z*staging_image+y*staging_row,row);
        return 1;
    }
}

int sky_mtl_transfer_requires_cpu_boundary(const sky_mtl_transfer_t *transfer) {
    return transfer&&(sky_mtl_texture_is_bc_fallback(transfer->source)||
                      sky_mtl_texture_is_bc_fallback(transfer->destination));
}

static int bc_transfer_texture(sky_mtl_texture_t *texture,int format,uint64_t level,uint64_t slice,
    const int32_t region[6],sky_bc_format_info_t *info,size_t *row,size_t *rows) {
    uint32_t logical=0;uint64_t first;
    return texture&&nvn_fmt_to_mtl(format,&logical,NULL)==1&&logical==texture->properties.pixel_format&&
        bc_region(texture,level,slice,region,info,row,rows,&first);
}

int sky_mtl_transfer_execute_cpu(const sky_mtl_transfer_t *transfer) {
    if(!sky_mtl_transfer_requires_cpu_boundary(transfer)||transfer->kind>SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE||
       (transfer->flags&~3u))return 0;
    sky_bc_format_info_t info;size_t row,rows;
    if(transfer->kind==SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE) {
        sky_bc_format_info_t to_info;size_t to_row,to_rows;
        if(!bc_transfer_texture(transfer->source,transfer->source_format,transfer->source_level,
                transfer->source_layer,transfer->source_region,&info,&row,&rows)||
           !bc_transfer_texture(transfer->destination,transfer->destination_format,transfer->destination_level,
                transfer->destination_layer,transfer->destination_region,&to_info,&to_row,&to_rows)||
           transfer->source->properties.pixel_format!=transfer->destination->properties.pixel_format||
           transfer->source_region[3]!=transfer->destination_region[3]||
           transfer->source_region[4]!=transfer->destination_region[4]||
           MAX(transfer->source_region[5],1)!=MAX(transfer->destination_region[5],1))return 0;
        size_t count=(size_t)MAX(transfer->source_region[5],1);
        if(rows>SIZE_MAX/row||count>SIZE_MAX/(row*rows)||count*row*rows>(512u<<20))return 0;
        size_t image=row*rows;uint8_t *blocks=malloc(image*count);
        if(!blocks)return 0;
        /* The temporary also makes overlapping views/raw regions well-defined. */
        int valid=bc_read_blocks(transfer->source,transfer->source_level,transfer->source_layer,
            transfer->source_region,blocks,row,image)&&
            bc_upload_blocks(transfer->destination,transfer->destination_level,transfer->destination_layer,
                transfer->destination_region,blocks,row,image);
        free(blocks);return valid;
    }
    int upload=transfer->kind==SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE;
    sky_mtl_texture_t *texture=upload?transfer->destination:transfer->source;
    int format=upload?transfer->destination_format:transfer->source_format;
    unsigned level=upload?transfer->destination_level:transfer->source_level;
    unsigned layer=upload?transfer->destination_layer:transfer->source_layer;
    const int32_t *region=upload?transfer->destination_region:transfer->source_region;
    size_t pitch,image,extent;
    if(!bc_transfer_texture(texture,format,level,layer,region,&info,&row,&rows)||
       !nvn_copy_pitch(region[3],region[4],region[5],4,4,(int)info.block_bytes,
            transfer->row_stride,transfer->image_stride,&pitch,&image,&extent)||
       pitch%info.block_bytes||image%info.block_bytes)return 0;
    uint64_t offset=0;
    id<MTLBuffer> buffer=(__bridge id<MTLBuffer>)sky_mtl_buffer_native_handle(transfer->buffer_address,extent,&offset);
    if(!buffer||buffer.storageMode!=MTLStorageModeShared||!buffer.contents||
       offset>buffer.length||extent>buffer.length-offset)return 0;
    uint8_t *bytes=(uint8_t *)buffer.contents+offset;
    int valid=upload?bc_upload_blocks(texture,level,layer,region,bytes,pitch,image):
        bc_read_blocks(texture,level,layer,region,bytes,pitch,image);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);return valid;
}

static size_t g_texture_count;

static sky_mtl_texture_t *texture_wrap(id<MTLTexture> texture) {
    if (!texture) return NULL;
    sky_mtl_texture_t *out = calloc(1, sizeof *out);
    if (!out) return NULL;
    out->texture = CFBridgingRetain(texture);
    out->properties = (sky_mtl_texture_properties_t){
        (uint32_t)texture.pixelFormat, (uint32_t)texture.textureType, (uint32_t)texture.storageMode,
        texture.width, texture.height, texture.depth, texture.arrayLength,
        texture.mipmapLevelCount, texture.sampleCount, texture.usage,
        {texture.swizzle.red, texture.swizzle.green, texture.swizzle.blue, texture.swizzle.alpha},
    };
    out->resource_id = texture.gpuResourceID._impl;
    __atomic_add_fetch(&g_texture_count, 1, __ATOMIC_RELAXED);
    return out;
}

sky_mtl_texture_t *sky_mtl_texture_create(const sky_mtl_texture_properties_t *properties) {
    if (!properties) return NULL;
    @autoreleasepool {
        id<MTLDevice> device = shared_device();
        if (!device) return NULL;
        uint32_t format=properties->pixel_format;sky_bc_format_info_t bc_info;
        const char *forced=getenv("SKY_METAL_FORCE_BC_DECODE");
        int fallback=sky_bc_format_info(format,&bc_info)&&
            (!device.supportsBCTextureCompression||(forced&&*forced&&strcmp(forced,"0")));
        if(fallback&&(properties->sample_count!=1||
            (properties->usage&(SKY_MTL_TEXTURE_USAGE_SHADER_WRITE|SKY_MTL_TEXTURE_USAGE_RENDER_TARGET)))) {
            fprintf(stderr,"NVN_METAL_TEXTURE_ERROR reason=unsupported-bc-writable-usage format=%u\n",format);return NULL;
        }
        if(![device supportsTextureSampleCount:properties->sample_count]) {
            fprintf(stderr,"NVN_METAL_TEXTURE_ERROR reason=unsupported-sample-count samples=%llu\n",(unsigned long long)properties->sample_count);
            return NULL;
        }
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
        descriptor.pixelFormat = (MTLPixelFormat)(fallback?bc_info.decoded_format:format);
        descriptor.textureType = (MTLTextureType)properties->texture_type;
        descriptor.storageMode = (MTLStorageMode)properties->storage_mode;
        descriptor.width = properties->width;
        descriptor.height = properties->height;
        descriptor.depth = properties->depth;
        descriptor.arrayLength = properties->array_length;
        descriptor.mipmapLevelCount = properties->mipmap_level_count;
        descriptor.sampleCount = properties->sample_count;
        descriptor.usage = (MTLTextureUsage)properties->usage;
        if (properties->swizzle[0] || properties->swizzle[1] || properties->swizzle[2] || properties->swizzle[3])
            descriptor.swizzle = MTLTextureSwizzleChannelsMake(
                (MTLTextureSwizzle)properties->swizzle[0], (MTLTextureSwizzle)properties->swizzle[1],
                (MTLTextureSwizzle)properties->swizzle[2], (MTLTextureSwizzle)properties->swizzle[3]);
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        sky_mtl_texture_t *out=texture_wrap(texture);
        if(out&&fallback) {
            out->bc=bc_backing_create(properties,bc_info.block_bytes);
            if(!out->bc){sky_mtl_texture_destroy(out);return NULL;}
            out->properties.pixel_format=format;
            static uint64_t logged;
            uint64_t mask=UINT64_C(1)<<(format-130);
            if(!(__atomic_fetch_or(&logged,mask,__ATOMIC_RELAXED)&mask))
                fprintf(stderr,"NVN_METAL_BC_FALLBACK logical_format=%u decoded_format=%u device=%s\n",format,bc_info.decoded_format,device.name.UTF8String);
        }
        return out;
    }
}

sky_mtl_texture_t *sky_mtl_texture_retain(sky_mtl_texture_t *texture) {
    if (!texture || !texture->texture) return NULL;
    sky_mtl_texture_t *out = malloc(sizeof *out);
    if (!out) return NULL;
    *out = *texture;
    CFRetain(out->texture);
    if(out->bc)__atomic_add_fetch(&out->bc->references,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&g_texture_count, 1, __ATOMIC_RELAXED);
    return out;
}

sky_mtl_texture_t *sky_mtl_texture_view_create(sky_mtl_texture_t *base, uint32_t pixel_format,
    uint32_t texture_type, uint64_t level_start, uint64_t level_count,
    uint64_t slice_start, uint64_t slice_count, const uint8_t swizzle[4]) {
    if (!base || !base->texture || !level_count || !slice_count || !swizzle) return NULL;
    if (pixel_format != base->properties.pixel_format &&
        !(base->properties.usage & SKY_MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW)) return NULL;
    if(level_start>=base->properties.mipmap_level_count||level_count>base->properties.mipmap_level_count-level_start)return NULL;
    uint64_t slices=base->properties.texture_type==SKY_MTL_TEXTURE_TYPE_3D?1:texture_slices(&base->properties,level_start);
    if(slice_start>=slices||slice_count>slices-slice_start)return NULL;
    sky_bc_format_info_t decoded;
    if(base->bc&&(!bc_same_family(base->properties.pixel_format,pixel_format)||!sky_bc_format_info(pixel_format,&decoded)))return NULL;
    @autoreleasepool {
        id<MTLTexture> object = (__bridge id<MTLTexture>)base->texture;
        MTLTextureSwizzleChannels channels = MTLTextureSwizzleChannelsMake(
            (MTLTextureSwizzle)swizzle[0], (MTLTextureSwizzle)swizzle[1],
            (MTLTextureSwizzle)swizzle[2], (MTLTextureSwizzle)swizzle[3]);
        id<MTLTexture> view = [object newTextureViewWithPixelFormat:(MTLPixelFormat)(base->bc?decoded.decoded_format:pixel_format)
                                                       textureType:(MTLTextureType)texture_type
                                                             levels:NSMakeRange(level_start, level_count)
                                                             slices:NSMakeRange(slice_start, slice_count)
                                                            swizzle:channels];
        sky_mtl_texture_t *out=texture_wrap(view);
        if(out&&base->bc) {
            out->bc=base->bc;__atomic_add_fetch(&out->bc->references,1,__ATOMIC_RELAXED);
            out->bc_level=base->bc_level+level_start;out->bc_slice=base->bc_slice+slice_start;
            out->properties.pixel_format=pixel_format;
        }
        return out;
    }
}

void sky_mtl_texture_destroy(sky_mtl_texture_t *texture) {
    if (!texture) return;
    if (texture->texture) CFRelease(texture->texture);
    bc_backing_release(texture->bc);
    texture->texture = NULL;
    __atomic_sub_fetch(&g_texture_count, 1, __ATOMIC_RELAXED);
    free(texture);
}

int sky_mtl_texture_get_properties(const sky_mtl_texture_t *texture, sky_mtl_texture_properties_t *properties) {
    if (!texture || !texture->texture || !properties) return 0;
    *properties = texture->properties;
    return 1;
}

uint64_t sky_mtl_texture_resource_id(const sky_mtl_texture_t *texture) {
    return texture ? texture->resource_id : 0;
}

void sky_mtl_texture_set_debug_label(sky_mtl_texture_t *texture, const char *label) {
    if (!texture || !texture->texture) return;
    @autoreleasepool {
        id<MTLTexture> object = (__bridge id<MTLTexture>)texture->texture;
        object.label = label ? [NSString stringWithUTF8String:label] : nil;
    }
}

size_t sky_mtl_texture_active_count(void) {
    return __atomic_load_n(&g_texture_count, __ATOMIC_RELAXED);
}

/* Keep shader installation cheap. Skyrim creates thousands of programs before its
 * first frame, so compilation happens only when a stage is actually submitted. */
typedef enum {
    SKY_MTL_BIND_UNKNOWN = 0,
    SKY_MTL_BIND_TEX,
    SKY_MTL_BIND_IMG,
    SKY_MTL_BIND_TBUF,
    SKY_MTL_BIND_SAMP,
    SKY_MTL_BIND_SAMP_TEX,
    SKY_MTL_BIND_CBUF,
    SKY_MTL_BIND_SBUF,
    SKY_MTL_BIND_UBO_PTR,
    SKY_MTL_BIND_SUPPORT,
    SKY_MTL_BIND_C1,
    SKY_MTL_BIND_DRVCB,
} sky_mtl_bind_kind_t;

typedef struct {
    sky_mtl_bind_kind_t kind;
    NSUInteger arg_id;
    int reg;
    int bank;
    uint8_t rw;
    char kind_name[16];
} sky_mtl_shader_binding_t;

typedef struct {
    int location;
    MTLVertexFormat fallback_format;
} sky_mtl_vertex_input_t;

@interface SkyMetalShader : NSObject {
@public
    sky_mtl_shader_binding_t *parsed_bindings;
    uint32_t binding_count;
    uint32_t driver_texture_base;
    uint8_t has_writable_binding;
    sky_mtl_vertex_input_t *vertex_inputs;
    uint32_t vertex_input_count;
}
@property NSString *sourcePath;
@property NSDictionary *reflection;
@property id<MTLFunction> function;
@property id<MTLArgumentEncoder> arguments;
@property id<MTLBuffer> constants;
@property id<MTLComputePipelineState> compute;
@property BOOL failed;
@end
@implementation SkyMetalShader
- (void)dealloc {
    free(self->parsed_bindings);
    free(self->vertex_inputs);
}
@end

static void metal_shader_parse_reflection(SkyMetalShader *shader, NSDictionary *reflection) {
    NSNumber *dtb = reflection[@"driver_texture_base"];
    shader->driver_texture_base = dtb ? [dtb unsignedIntValue] : 0x168u;
    NSArray *bindings = reflection[@"bindings"];
    uint32_t count = (uint32_t)bindings.count;
    shader->binding_count = count;
    if (count > 0) {
        shader->parsed_bindings = calloc(count, sizeof(sky_mtl_shader_binding_t));
        uint32_t idx = 0;
        for (NSDictionary *binding in bindings) {
            sky_mtl_shader_binding_t *dst = &shader->parsed_bindings[idx++];
            NSString *kind = binding[@"kind"];
            const char *kstr = kind ? kind.UTF8String : "";
            snprintf(dst->kind_name, sizeof dst->kind_name, "%s", kstr ? kstr : "");
            if ([kind isEqualToString:@"tex"]) dst->kind = SKY_MTL_BIND_TEX;
            else if ([kind isEqualToString:@"img"]) dst->kind = SKY_MTL_BIND_IMG;
            else if ([kind isEqualToString:@"tbuf"]) dst->kind = SKY_MTL_BIND_TBUF;
            else if ([kind isEqualToString:@"samp"]) dst->kind = SKY_MTL_BIND_SAMP;
            else if ([kind isEqualToString:@"samp_tex"]) dst->kind = SKY_MTL_BIND_SAMP_TEX;
            else if ([kind isEqualToString:@"cbuf"]) dst->kind = SKY_MTL_BIND_CBUF;
            else if ([kind isEqualToString:@"sbuf"]) dst->kind = SKY_MTL_BIND_SBUF;
            else if ([kind isEqualToString:@"ubo_ptr"]) dst->kind = SKY_MTL_BIND_UBO_PTR;
            else if ([kind isEqualToString:@"support"]) dst->kind = SKY_MTL_BIND_SUPPORT;
            else if ([kind isEqualToString:@"c1"]) dst->kind = SKY_MTL_BIND_C1;
            else if ([kind isEqualToString:@"drvcb"]) dst->kind = SKY_MTL_BIND_DRVCB;
            else dst->kind = SKY_MTL_BIND_UNKNOWN;
            dst->arg_id = [binding[@"arg_id"] unsignedIntegerValue];
            dst->reg = [binding[@"reg"] intValue];
            dst->bank = [binding[@"bank"] intValue];
            dst->rw = [binding[@"rw"] boolValue] ? 1 : 0;
            if (dst->rw || dst->kind == SKY_MTL_BIND_IMG) shader->has_writable_binding = 1;
        }
    }
    NSArray *vinputs = reflection[@"vertex_inputs"];
    if ([vinputs isKindOfClass:[NSArray class]] && vinputs.count > 0) {
        uint32_t vcount = (uint32_t)vinputs.count;
        shader->vertex_inputs = calloc(vcount, sizeof(sky_mtl_vertex_input_t));
        shader->vertex_input_count = vcount;
        uint32_t vidx = 0;
        for (NSDictionary *input in vinputs) {
            int attr = [input[@"location"] intValue];
            NSString *type = input[@"msl_type"];
            MTLVertexFormat fallback = [type hasPrefix:@"uint"] ? MTLVertexFormatUInt4 :
                [type hasPrefix:@"int"] ? MTLVertexFormatInt4 : MTLVertexFormatFloat4;
            shader->vertex_inputs[vidx++] = (sky_mtl_vertex_input_t){attr, fallback};
        }
    }
}

@interface SkyMetalSubmitState : NSObject {
@public
    id<MTLRenderCommandEncoder> encoder;
    MTLRenderPassDescriptor *pass;
    id<MTLBuffer> visibility;
    const void *colors[8],*depth;
    uint32_t color_count;
    uint16_t vertex_mask;
    BOOL reuse;
    uint64_t render_encoders,merged_draws,compute_encoders,blit_encoders,argument_buffers,depth_hits,depth_misses,wait_ns;
    uint64_t barriers,writable_draws,feedback_draws,pass_changes;
}
@end
@implementation SkyMetalSubmitState
@end

static void metal_render_end(SkyMetalSubmitState *submit) {
    if(submit->encoder)[submit->encoder endEncoding];
    submit->encoder=nil;submit->pass=nil;submit->visibility=nil;
}

static NSMutableDictionary<NSNumber *, NSMutableDictionary<NSNumber *, SkyMetalShader *> *> *g_programs;
static NSMutableDictionary<NSString *, SkyMetalShader *> *g_shaders;
static NSMutableDictionary<NSNumber *, NSNumber *> *g_program_references;
static NSMutableSet<NSNumber *> *g_program_removed;
static pthread_mutex_t g_shader_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

enum { SKY_MTL_SHADER_CACHE_SLOTS = 256 };
typedef struct {
    uint64_t serial;
    int stage;
    const void *shader;
} sky_mtl_shader_cache_entry_t;
static sky_mtl_shader_cache_entry_t g_shader_cache[SKY_MTL_SHADER_CACHE_SLOTS];

int sky_mtl_program_set_shader(uint64_t serial, int stage, const char *msl_path,
                               const char *reflection_path) {
    if (!serial || stage < 0 || stage > 5 || !msl_path || !reflection_path) return 0;
    @autoreleasepool {
        NSString *path = [NSString stringWithUTF8String:msl_path];
        NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:reflection_path]];
        NSError *error = nil;
        NSDictionary *reflection = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        if (![reflection isKindOfClass:[NSDictionary class]] ||
            ![reflection[@"route"] isEqual:@"B"] || ![reflection[@"bindings"] isKindOfClass:[NSArray class]]) {
            fprintf(stderr, "NVN_METAL_SHADER_ERROR serial=%llu stage=%d reason=invalid-reflection path=%s\n",
                    (unsigned long long)serial, stage, reflection_path);
            return 0;
        }
        pthread_mutex_lock(&g_shader_lock);
        if (!g_programs) g_programs = [NSMutableDictionary new];
        if (!g_shaders) g_shaders = [NSMutableDictionary new];
        SkyMetalShader *shader = g_shaders[path];
        if (!shader) {
            shader = [SkyMetalShader new];
            shader.sourcePath = path;
            shader.reflection = reflection;
            metal_shader_parse_reflection(shader, reflection);
            g_shaders[path] = shader;
        }
        if (!g_programs[@(serial)]) g_programs[@(serial)] = [NSMutableDictionary new];
        g_programs[@(serial)][@(stage)] = shader;
        memset(g_shader_cache, 0, sizeof g_shader_cache);
        pthread_mutex_unlock(&g_shader_lock);
        return 1;
    }
}

void sky_mtl_program_retain(uint64_t serial) {
    if(!serial)return;
    pthread_mutex_lock(&g_shader_lock);
    if(!g_program_references)g_program_references=[NSMutableDictionary new];
    g_program_references[@(serial)]=@([g_program_references[@(serial)] unsignedLongLongValue]+1);
    pthread_mutex_unlock(&g_shader_lock);
}
void sky_mtl_program_release(uint64_t serial) {
    if(!serial)return;
    pthread_mutex_lock(&g_shader_lock);
    uint64_t refs=[g_program_references[@(serial)] unsignedLongLongValue];
    if(refs>1)g_program_references[@(serial)]=@(refs-1);
    else {
        [g_program_references removeObjectForKey:@(serial)];
        if([g_program_removed containsObject:@(serial)]){
            [g_programs removeObjectForKey:@(serial)];[g_program_removed removeObject:@(serial)];
            memset(g_shader_cache, 0, sizeof g_shader_cache);
        }
    }
    pthread_mutex_unlock(&g_shader_lock);
}
void sky_mtl_program_remove(uint64_t serial) {
    pthread_mutex_lock(&g_shader_lock);
    if([g_program_references[@(serial)] unsignedLongLongValue]){
        if(!g_program_removed)g_program_removed=[NSMutableSet new];
        [g_program_removed addObject:@(serial)];
    } else {
        [g_programs removeObjectForKey:@(serial)];
        memset(g_shader_cache, 0, sizeof g_shader_cache);
    }
    pthread_mutex_unlock(&g_shader_lock);
}

static SkyMetalShader *metal_shader(uint64_t serial, int stage) {
    if (!serial) return nil;
    unsigned slot = (unsigned)(((serial * 7u) ^ (unsigned)stage) & (SKY_MTL_SHADER_CACHE_SLOTS - 1));
    SkyMetalShader *shader = (g_shader_cache[slot].serial == serial && g_shader_cache[slot].stage == stage)
        ? (__bridge SkyMetalShader *)g_shader_cache[slot].shader : nil;
    if (!shader) {
        shader = g_programs[@(serial)][@(stage)];
        if (shader) g_shader_cache[slot] = (sky_mtl_shader_cache_entry_t){serial, stage, (__bridge const void *)shader};
    }
    if (!shader || shader.failed) return nil;
    if (shader.function) return shader;
    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:shader.sourcePath encoding:NSUTF8StringEncoding error:&error];
    MTLCompileOptions *options = [MTLCompileOptions new];
    if(@available(macOS 15.0,iOS 18.0,*)) options.mathMode = MTLMathModeSafe;
    else {
        /* Safe math before the replacement property was introduced. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    id<MTLLibrary> library = source ? [shared_device() newLibraryWithSource:source options:options error:&error] : nil;
    NSString *entry = shader.reflection[@"entry"];
    id<MTLFunction> function = entry ? [library newFunctionWithName:entry] : nil;
    if (!function) for (NSString *name in library.functionNames) {
        id<MTLFunction> candidate = [library newFunctionWithName:name];
        MTLFunctionType type = stage == 0 ? MTLFunctionTypeVertex : stage == 1 ? MTLFunctionTypeFragment : MTLFunctionTypeKernel;
        if (candidate.functionType == type) { function = candidate; break; }
    }
    if (!function) {
        fprintf(stderr, "NVN_METAL_SHADER_ERROR serial=%llu stage=%d reason=compile path=%s error=%s\n",
                (unsigned long long)serial, stage, shader.sourcePath.UTF8String, error.description.UTF8String);
        shader.failed = YES;
        return nil;
    }
    shader.function = function;
    if (shader->binding_count > 0) shader.arguments = [function newArgumentEncoderWithBufferIndex:0];
    NSString *hex = shader.reflection[@"c1"];
    if (hex.length && !(hex.length & 1)) {
        if(hex.length/2>65536){shader.failed=YES;return nil;}
        NSMutableData *constants = [NSMutableData dataWithLength:65536];
        uint8_t *bytes = constants.mutableBytes;
        for (NSUInteger i = 0; i < hex.length / 2; i++) {
            unsigned value = 0;
            [[NSScanner scannerWithString:[hex substringWithRange:NSMakeRange(i * 2, 2)]] scanHexInt:&value];
            bytes[i] = (uint8_t)value;
        }
        shader.constants = [shared_device() newBufferWithBytes:constants.bytes length:constants.length options:MTLResourceStorageModeShared];
    }
    fprintf(stderr, "NVN_METAL_SHADER_COMPILED serial=%llu stage=%d entry=%s path=%s\n",
            (unsigned long long)serial, stage, function.name.UTF8String, shader.sourcePath.UTF8String);
    return shader;
}

struct sky_mtl_sampler { const void *sampler; };
sky_mtl_sampler_t *sky_mtl_sampler_create(const sky_mtl_sampler_properties_t *p) {
    if (!p) return NULL;
    @autoreleasepool {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.supportArgumentBuffers = YES;
        d.magFilter = p->mag_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter = (p->min_filter & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = p->min_filter < 2 ? MTLSamplerMipFilterNotMipmapped :
            p->min_filter < 4 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        MTLSamplerAddressMode modes[] = {MTLSamplerAddressModeClampToBorderColor, MTLSamplerAddressModeRepeat,
            MTLSamplerAddressModeMirrorClampToEdge, MTLSamplerAddressModeMirrorClampToEdge,
            MTLSamplerAddressModeMirrorClampToEdge, MTLSamplerAddressModeClampToBorderColor,
            MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToEdge};
        d.sAddressMode = modes[p->wrap[0] >= 0 && p->wrap[0] < 8 ? p->wrap[0] : 7];
        d.tAddressMode = modes[p->wrap[1] >= 0 && p->wrap[1] < 8 ? p->wrap[1] : 7];
        d.rAddressMode = modes[p->wrap[2] >= 0 && p->wrap[2] < 8 ? p->wrap[2] : 7];
        d.lodMinClamp = MAX(p->lod_min, 0.f);
        d.lodMaxClamp = MAX(p->lod_max, d.lodMinClamp);
        d.maxAnisotropy = (NSUInteger)MAX(1.f, MIN(p->max_anisotropy, 16.f));
        if(@available(macOS 26.0,iOS 26.0,*)) {
#if TARGET_OS_IOS
            /* The OS exposes these properties on older iPhones as well, but
             * their hardware implementation starts at the A19/Apple10 GPU. */
            if(![shared_device() supportsFamily:MTLGPUFamilyApple10]) {
                if(p->lod_bias!=0||p->reduction_filter!=0) {
                    fprintf(stderr,"NVN_METAL_SAMPLER_ERROR reason=lod-bias-or-reduction-requires-apple10\n");return NULL;
                }
            } else {
#endif
            d.lodBias=p->lod_bias;
            d.reductionMode=p->reduction_filter==1?MTLSamplerReductionModeMinimum:p->reduction_filter==2?MTLSamplerReductionModeMaximum:MTLSamplerReductionModeWeightedAverage;
#if TARGET_OS_IOS
            }
#endif
        } else if(p->lod_bias!=0||p->reduction_filter!=0) {
            fprintf(stderr,"NVN_METAL_SAMPLER_ERROR reason=lod-bias-or-reduction-requires-os26\n");return NULL;
        }
        d.compareFunction = p->compare_mode ? (MTLCompareFunction)MAX(0, MIN(p->compare_func - 1, 7)) : MTLCompareFunctionNever;
        id<MTLSamplerState> object = [shared_device() newSamplerStateWithDescriptor:d];
        if (!object) return NULL;
        sky_mtl_sampler_t *sampler = calloc(1, sizeof *sampler);
        if (sampler) sampler->sampler = CFBridgingRetain(object);
        return sampler;
    }
}
sky_mtl_sampler_t *sky_mtl_sampler_retain(sky_mtl_sampler_t *sampler) {
    if (!sampler || !sampler->sampler) return NULL;
    sky_mtl_sampler_t *copy = malloc(sizeof *copy);
    if (copy) { *copy = *sampler; CFRetain(copy->sampler); }
    return copy;
}
void sky_mtl_sampler_destroy(sky_mtl_sampler_t *sampler) {
    if (!sampler) return;
    if (sampler->sampler) CFRelease(sampler->sampler);
    free(sampler);
}

static id<MTLBuffer> metal_buffer(uint64_t address, uint64_t size, NSUInteger *offset) {
    id<MTLBuffer> result = nil;
    pthread_mutex_lock(&g_lock);
    sky_mtl_memory_pool_t *p = g_last_pool;
    if (!p || p->shadowed_by_newer || !metal_pool_contains(p, address, size)) {
        sky_mtl_buffer_lookup_t *cached=&g_buffer_lookup[metal_buffer_lookup_slot(address,size)];
        p=cached->address==address&&cached->size==size?cached->pool:NULL;
        if(!p||!metal_pool_contains(p,address,size)) {
            /* Keep the authoritative newest-first fallback, including overlap
             * priority, zero-length queries, and exact requested-span bounds. */
            for(p=g_pools;p;p=p->next)if(metal_pool_contains(p,address,size))break;
        }
        if(p) {
            *cached=(sky_mtl_buffer_lookup_t){address,size,p};
            if(!p->shadowed_by_newer)g_last_pool=p;
        }
    }
    if(p) {
        result = (__bridge id<MTLBuffer>)p->buffer;
        *offset = (NSUInteger)(address - p->gpu_address + (uintptr_t)p->memory - p->base);
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

static id<MTLBuffer> zero_buffer(void) {
    static id<MTLBuffer> buffer;
    if (!buffer) buffer = [shared_device() newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    return buffer;
}
static id<MTLBuffer> support_buffer(void) {
    static id<MTLBuffer> buffer;
    if (!buffer) {
        buffer = [shared_device() newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        for (int i = 0; i < 73; i++) *(float *)((uint8_t *)buffer.contents + 192 + 16 * i) = 1.f;
    }
    return buffer;
}

enum {
    SKY_MTL_ARGBUF_RING_BYTES = 4u * 1024u * 1024u,
    SKY_MTL_DRVCB_SLICE_BYTES = 2560u,
    SKY_MTL_DRVCB_RING_BYTES = 16u * 1024u * 1024u,
    SKY_MTL_DRVCB_TAIL_BYTES = 65536u,
};
static id<MTLBuffer> g_argbuf_ring;
static NSUInteger g_argbuf_cursor;
static id<MTLBuffer> g_drvcb_ring;
static NSUInteger g_drvcb_cursor;

static id<MTLBuffer> alloc_argument_slice(NSUInteger length, NSUInteger *out_offset) {
    NSUInteger aligned = (MAX(length, 16u) + 255u) & ~(NSUInteger)255u;
    if (aligned > SKY_MTL_ARGBUF_RING_BYTES) {
        *out_offset = 0;
        return [shared_device() newBufferWithLength:aligned options:MTLResourceStorageModeShared];
    }
    if (!g_argbuf_ring) {
        g_argbuf_ring = [shared_device() newBufferWithLength:SKY_MTL_ARGBUF_RING_BYTES options:MTLResourceStorageModeShared];
        g_argbuf_cursor = 0;
    }
    if (!g_argbuf_ring) return nil;
    if (g_argbuf_cursor + aligned > SKY_MTL_ARGBUF_RING_BYTES) g_argbuf_cursor = 0;
    NSUInteger offset = g_argbuf_cursor;
    g_argbuf_cursor += aligned;
    memset((uint8_t *)g_argbuf_ring.contents + offset, 0, aligned);
    *out_offset = offset;
    return g_argbuf_ring;
}

static id<MTLBuffer> alloc_drvcb_slice(NSUInteger *out_offset, uint8_t **out_bytes) {
    if (!g_drvcb_ring) {
        g_drvcb_ring = [shared_device() newBufferWithLength:SKY_MTL_DRVCB_RING_BYTES + SKY_MTL_DRVCB_TAIL_BYTES
                                                    options:MTLResourceStorageModeShared];
        g_drvcb_cursor = 0;
    }
    if (!g_drvcb_ring) return nil;
    if (g_drvcb_cursor + SKY_MTL_DRVCB_SLICE_BYTES > SKY_MTL_DRVCB_RING_BYTES) g_drvcb_cursor = 0;
    NSUInteger offset = g_drvcb_cursor;
    g_drvcb_cursor += SKY_MTL_DRVCB_SLICE_BYTES;
    uint8_t *bytes = (uint8_t *)g_drvcb_ring.contents + offset;
    memset(bytes, 0, SKY_MTL_DRVCB_SLICE_BYTES);
    *out_offset = offset;
    *out_bytes = bytes;
    return g_drvcb_ring;
}

static int encode_shader_arguments(id<MTLCommandEncoder> encoder, SkyMetalShader *shader,
    const sky_mtl_render_state_t *s, int stage,SkyMetalSubmitState *submit) {
    id<MTLArgumentEncoder> arguments = shader.arguments;
    if (!arguments) {
        if(stage==0)[(id<MTLRenderCommandEncoder>)encoder setVertexBuffer:nil offset:0 atIndex:0];
        else if(stage==1)[(id<MTLRenderCommandEncoder>)encoder setFragmentBuffer:nil offset:0 atIndex:0];
        return shader->binding_count == 0;
    }
    NSUInteger arg_offset = 0;
    id<MTLBuffer> argumentBuffer = alloc_argument_slice(arguments.encodedLength, &arg_offset);
    if(argumentBuffer)submit->argument_buffers++;
    [arguments setArgumentBuffer:argumentBuffer offset:arg_offset];
    for (uint32_t b_idx = 0; b_idx < shader->binding_count; b_idx++) {
        const sky_mtl_shader_binding_t *binding = &shader->parsed_bindings[b_idx];
        sky_mtl_bind_kind_t kind = binding->kind;
        NSUInteger aid = binding->arg_id;
        int reg = binding->reg;
        id<MTLResource> resource = nil;
        MTLResourceUsage usage = MTLResourceUsageRead;
        if (kind == SKY_MTL_BIND_TEX || kind == SKY_MTL_BIND_IMG || kind == SKY_MTL_BIND_TBUF) {
            sky_mtl_texture_t *wrapped = reg >= 0 && reg < (kind == SKY_MTL_BIND_IMG ? 32 : 128)
                ? (kind == SKY_MTL_BIND_IMG ? s->images[stage][reg] : s->textures[stage][reg]) : NULL;
            id<MTLTexture> texture = wrapped ? (__bridge id<MTLTexture>)wrapped->texture : nil;
            if(kind==SKY_MTL_BIND_IMG&&wrapped&&wrapped->bc) {
                fprintf(stderr,"NVN_METAL_ARGUMENT_ERROR reason=bc-image-write-unsupported stage=%d reg=%d\n",stage,reg);return 0;
            }
            if (!texture) {
                fprintf(stderr, "NVN_METAL_ARGUMENT_ERROR stage=%d kind=%s reg=%d reason=unbound-texture\n", stage, binding->kind_name, reg);
                return 0;
            }
            [arguments setTexture:texture atIndex:aid];
            resource = texture;
            if (kind == SKY_MTL_BIND_IMG) usage |= MTLResourceUsageWrite;
        } else if (kind == SKY_MTL_BIND_SAMP || kind == SKY_MTL_BIND_SAMP_TEX) {
            sky_mtl_sampler_t *wrapped = reg >= 0 && reg < 128 ? s->samplers[stage][reg] : NULL;
            id<MTLSamplerState> sampler = wrapped ? (__bridge id<MTLSamplerState>)wrapped->sampler : nil;
            if (!sampler) {
                static sky_mtl_sampler_t *default_sampler;
                if(s->texture_handles[stage][reg]>>20 || s->sampler_handles[stage][reg]>>20) {
                    fprintf(stderr,"NVN_METAL_ARGUMENT_ERROR stage=%d reg=%d reason=unregistered-sampler\n",stage,reg);return 0;
                }
                if (!default_sampler) {
                    fprintf(stderr,"NVN_METAL_UNBOUND_SAMPLER default=nearest-repeat\n");
                    sky_mtl_sampler_properties_t p = {.wrap={1,1,1},.lod_max = 1000.f, .max_anisotropy = 1.f};
                    default_sampler = sky_mtl_sampler_create(&p);
                }
                sampler = default_sampler ? (__bridge id<MTLSamplerState>)default_sampler->sampler : nil;
            }
            [arguments setSamplerState:sampler atIndex:aid];
        } else {
            id<MTLBuffer> buffer = nil;
            NSUInteger offset = 0;
            if (kind == SKY_MTL_BIND_CBUF || kind == SKY_MTL_BIND_SBUF || kind == SKY_MTL_BIND_UBO_PTR) {
                const sky_mtl_buffer_binding_t *b = reg >= 0 && reg < 16 ?
                    (kind == SKY_MTL_BIND_SBUF ? &s->storage[stage][reg] : &s->uniform[stage][reg]) : NULL;
                if (b && b->address) {
                    buffer = metal_buffer(b->address, MAX(b->size, 1u), &offset);
                    if(!buffer){fprintf(stderr,"NVN_METAL_ARGUMENT_ERROR stage=%d kind=%s reg=%d address=%#llx size=%llu reason=unmapped-buffer\n",stage,binding->kind_name,reg,(unsigned long long)b->address,(unsigned long long)b->size);return 0;}
                }
                if (binding->rw) usage |= MTLResourceUsageWrite;
            } else if (kind == SKY_MTL_BIND_SUPPORT) buffer = support_buffer();
            else if (kind == SKY_MTL_BIND_C1) buffer = shader.constants;
            else if (kind == SKY_MTL_BIND_DRVCB) {
                uint8_t *bytes = NULL;
                buffer = alloc_drvcb_slice(&offset, &bytes);
                if (!buffer || !bytes) return 0;
                if (binding->bank == 2) {
                    NSUInteger texture_base = shader->driver_texture_base;
                    int texture_count = texture_base == 32 ? 32 : 128;
                    for (int r = 0; r < texture_count; r++) {
                        uint32_t th = (uint32_t)s->texture_handles[stage][r];
                        memcpy(bytes + texture_base + 8 * r, &th, 4);
                    }
                    for(int r=0;r<128;r++) {
                        uint32_t sh=(uint32_t)s->sampler_handles[stage][r];memcpy(bytes+0x568+8*r,&sh,4);
                    }
                    for(int r=0;r<32;r++) {
                        uint32_t image=(uint32_t)s->image_handles[stage][r];memcpy(bytes+0x120+8*r,&image,4);
                    }
                } else {
                    int base = stage == 0 ? 0x110 : stage == 1 ? 0x510 : stage == 5 ? 0x310 : -1;
                    if (base < 0) return 0;
                    for (int r = 0; r < 16; r++) {
                        uint64_t address = s->storage[stage][r].address;
                        uint32_t size = (uint32_t)s->storage[stage][r].size;
                        memcpy(bytes + base + 16 * r, &address, 8);
                        memcpy(bytes + base + 16 * r + 8, &size, 4);
                    }
                }
            } else {
                fprintf(stderr, "NVN_METAL_ARGUMENT_ERROR stage=%d kind=%s reason=unsupported-kind\n", stage, binding->kind_name);
                return 0;
            }
            if (!buffer) { buffer = zero_buffer(); offset = 0; }
            [arguments setBuffer:buffer offset:offset atIndex:aid];
            resource = buffer;
        }
        if (resource) {
            if (stage == 5) [(id<MTLComputeCommandEncoder>)encoder useResource:resource usage:usage];
            else [(id<MTLRenderCommandEncoder>)encoder useResource:resource usage:usage
                stages:stage == 0 ? MTLRenderStageVertex : MTLRenderStageFragment];
        }
    }
    if (stage == 0) [(id<MTLRenderCommandEncoder>)encoder setVertexBuffer:argumentBuffer offset:arg_offset atIndex:0];
    else if (stage == 1) [(id<MTLRenderCommandEncoder>)encoder setFragmentBuffer:argumentBuffer offset:arg_offset atIndex:0];
    else [(id<MTLComputeCommandEncoder>)encoder setBuffer:argumentBuffer offset:arg_offset atIndex:0];
    return 1;
}

static MTLVertexFormat metal_vertex_format(int f) {
    /* NVN's explicit integer-to-float formats select the same unnormalized
     * Metal fetch storage; the MSL float attribute requests numeric conversion. */
    if(f>=0x51&&f<=0x68) {
        static const int integer_formats[24]={3,4,8,9,11,12,15,16,20,21,23,24,27,28,32,33,35,36,39,40,44,45,47,48};
        f=integer_formats[f-0x51];
    }
    switch (f) {
        case 0x01: return MTLVertexFormatUCharNormalized; case 0x02: return MTLVertexFormatCharNormalized; case 0x03: return MTLVertexFormatUChar; case 0x04: return MTLVertexFormatChar;
        case 0x05: return MTLVertexFormatHalf; case 0x06: return MTLVertexFormatUShortNormalized; case 0x07: return MTLVertexFormatShortNormalized;
        case 0x08: return MTLVertexFormatUShort; case 0x09: return MTLVertexFormatShort; case 0x0A: return MTLVertexFormatFloat; case 0x0B: return MTLVertexFormatUInt; case 0x0C: return MTLVertexFormatInt;
        case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x0E: return MTLVertexFormatChar2Normalized; case 0x0F: return MTLVertexFormatUChar2; case 0x10: return MTLVertexFormatChar2;
        case 0x11: return MTLVertexFormatHalf2; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x13: return MTLVertexFormatShort2Normalized; case 0x14: return MTLVertexFormatUShort2;
        case 0x15: return MTLVertexFormatShort2; case 0x16: return MTLVertexFormatFloat2; case 0x17: return MTLVertexFormatUInt2; case 0x18: return MTLVertexFormatInt2;
        case 0x19: return MTLVertexFormatUChar3Normalized; case 0x1A: return MTLVertexFormatChar3Normalized; case 0x1B: return MTLVertexFormatUChar3; case 0x1C: return MTLVertexFormatChar3;
        case 0x1D: return MTLVertexFormatHalf3; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x1F: return MTLVertexFormatShort3Normalized; case 0x20: return MTLVertexFormatUShort3;
        case 0x21: return MTLVertexFormatShort3; case 0x22: return MTLVertexFormatFloat3; case 0x23: return MTLVertexFormatUInt3; case 0x24: return MTLVertexFormatInt3;
        case 0x25: return MTLVertexFormatUChar4Normalized; case 0x26: return MTLVertexFormatChar4Normalized; case 0x27: return MTLVertexFormatUChar4; case 0x28: return MTLVertexFormatChar4;
        case 0x29: return MTLVertexFormatHalf4; case 0x2A: return MTLVertexFormatUShort4Normalized; case 0x2B: return MTLVertexFormatShort4Normalized; case 0x2C: return MTLVertexFormatUShort4;
        case 0x2D: return MTLVertexFormatShort4; case 0x2E: return MTLVertexFormatFloat4; case 0x2F: return MTLVertexFormatUInt4; case 0x30: return MTLVertexFormatInt4;
        case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x3F: return MTLVertexFormatFloatRG11B10; case 0x9A: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    return MTLVertexFormatInvalid;
}
static MTLBlendFactor metal_blend_factor(int f) {
    switch (f) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color; case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
        case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor; case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha; }
    return MTLBlendFactorOne;
}
static MTLBlendOperation metal_blend_operation(int e) { switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; } return MTLBlendOperationAdd; }

static MTLStencilOperation metal_stencil_operation(int nvn);
enum {SKY_MTL_DEPTH_KEY_SIZE=15,SKY_MTL_DEPTH_CACHE_BUCKETS=64};
typedef struct sky_mtl_depth_entry {
    struct sky_mtl_depth_entry *next;
    uint8_t key[SKY_MTL_DEPTH_KEY_SIZE];
    const void *state;
} sky_mtl_depth_entry_t;
static sky_mtl_depth_entry_t *g_depth_states[SKY_MTL_DEPTH_CACHE_BUCKETS];

/* Called under g_shader_lock. Key only the descriptor's effective values;
 * references remain dynamic encoder state, and disabled stencil fields do not
 * create distinct descriptors. The shared device is immutable. */
static id<MTLDepthStencilState> metal_depth_stencil_state(const sky_mtl_render_state_t *s,const sky_mtl_clear_pass_t *targets,SkyMetalSubmitState *submit) {
    uint8_t key[SKY_MTL_DEPTH_KEY_SIZE]={0};
    key[0]=s->depth_test?(uint8_t)MAX(0,MIN(s->depth_func-1,7)):MTLCompareFunctionAlways;
    key[1]=!!(s->depth_test&&s->depth_write&&targets->depth_texture);
    key[2]=!!s->stencil_test;
    if(key[2])for(unsigned face=0;face<2;face++) {
        uint8_t *k=key+3+face*6;
        k[0]=(uint8_t)MAX(0,MIN(s->stencil[face].function-1,7));
        k[1]=(uint8_t)metal_stencil_operation(s->stencil[face].fail);
        k[2]=(uint8_t)metal_stencil_operation(s->stencil[face].depth_fail);
        k[3]=(uint8_t)metal_stencil_operation(s->stencil[face].pass);
        k[4]=s->stencil[face].read_mask;k[5]=s->stencil[face].write_mask;
    }
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<sizeof key;i++)hash=(hash^key[i])*16777619u;
    unsigned slot=hash&(SKY_MTL_DEPTH_CACHE_BUCKETS-1);
    for(sky_mtl_depth_entry_t *entry=g_depth_states[slot];entry;entry=entry->next)
        if(!memcmp(entry->key,key,sizeof key)){submit->depth_hits++;return (__bridge id<MTLDepthStencilState>)entry->state;}
    submit->depth_misses++;
    MTLDepthStencilDescriptor *descriptor=[MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction=(MTLCompareFunction)key[0];descriptor.depthWriteEnabled=key[1];
    if(key[2])for(unsigned face=0;face<2;face++) {
        const uint8_t *k=key+3+face*6;MTLStencilDescriptor *stencil=[MTLStencilDescriptor new];
        stencil.stencilCompareFunction=(MTLCompareFunction)k[0];
        stencil.stencilFailureOperation=(MTLStencilOperation)k[1];
        stencil.depthFailureOperation=(MTLStencilOperation)k[2];
        stencil.depthStencilPassOperation=(MTLStencilOperation)k[3];
        stencil.readMask=k[4];stencil.writeMask=k[5];
        if(!face)descriptor.frontFaceStencil=stencil;else descriptor.backFaceStencil=stencil;
    }
    id<MTLDepthStencilState> state=[shared_device() newDepthStencilStateWithDescriptor:descriptor];
    if(!state)return nil;
    sky_mtl_depth_entry_t *entry=malloc(sizeof *entry);
    if(entry) {
        memcpy(entry->key,key,sizeof key);entry->state=CFBridgingRetain(state);
        entry->next=g_depth_states[slot];g_depth_states[slot]=entry;
    }
    return state;
}


static MTLRenderPassDescriptor *metal_render_targets(const sky_mtl_clear_pass_t *p) {
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    for (uint32_t i = 0; i < p->color_count; i++) {
        sky_mtl_texture_t *t = p->textures[i];
        if (!t) continue;
        d.colorAttachments[i].texture = (__bridge id<MTLTexture>)t->texture;
        d.colorAttachments[i].loadAction = p->has_color_clear && p->clear_index == i ? MTLLoadActionClear : MTLLoadActionLoad;
        d.colorAttachments[i].storeAction = MTLStoreActionStore;
        d.colorAttachments[i].clearColor = MTLClearColorMake(p->colors[i][0], p->colors[i][1], p->colors[i][2], p->colors[i][3]);
    }
    sky_mtl_texture_t *wrapped = p->depth_texture;
    id<MTLTexture> depth = wrapped ? (__bridge id<MTLTexture>)wrapped->texture : nil;
    if (depth && wrapped->properties.pixel_format != MTLPixelFormatStencil8) {
        d.depthAttachment.texture = depth;
        d.depthAttachment.loadAction = p->depth_mask ? MTLLoadActionClear : MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.depthAttachment.clearDepth = p->clear_depth;
    }
    if (depth && (wrapped->properties.pixel_format == MTLPixelFormatDepth32Float_Stencil8 || wrapped->properties.pixel_format == MTLPixelFormatStencil8)) {
        d.stencilAttachment.texture = depth;
        d.stencilAttachment.loadAction = p->stencil_mask ? MTLLoadActionClear : MTLLoadActionLoad;
        d.stencilAttachment.storeAction = MTLStoreActionStore;
        d.stencilAttachment.clearStencil = p->clear_stencil;
    }
    return d;
}

static id<MTLTexture> metal_texture_root(id<MTLTexture> texture) {
    id<MTLTexture> parent;
    while((parent=texture.parentTexture))texture=parent;
    return texture;
}
static int metal_texture_aliases(id<MTLTexture> a,id<MTLTexture> b) {
    if(a==b)return 1;
    a=metal_texture_root(a);b=metal_texture_root(b);
    if(a==b)return 1;
    /* Separate native textures can alias external storage without sharing a
     * parentTexture. Keep all such pairs isolated rather than guessing their
     * backing allocation identity or byte ranges. */
    return (a.buffer||a.iosurface)&&(b.buffer||b.iosurface);
}
static int metal_merge_safe(SkyMetalShader *shader,const sky_mtl_render_state_t *state,int stage,
    const sky_mtl_clear_pass_t *targets,unsigned *reason) {
    if (shader->has_writable_binding) { *reason = 1; return 0; }
    for (uint32_t b_idx = 0; b_idx < shader->binding_count; b_idx++) {
        const sky_mtl_shader_binding_t *binding = &shader->parsed_bindings[b_idx];
        if (binding->kind != SKY_MTL_BIND_TEX && binding->kind != SKY_MTL_BIND_TBUF) continue;
        int reg = binding->reg; if (reg < 0 || reg >= 128) return 0;
        sky_mtl_texture_t *wrapped = state->textures[stage][reg];
        id<MTLTexture> texture = wrapped ? (__bridge id<MTLTexture>)wrapped->texture : nil;
        if (!texture) return 0;
        for (uint32_t i = 0; i < targets->color_count; i++) {
            wrapped = targets->textures[i];
            if (wrapped && metal_texture_aliases(texture, (__bridge id<MTLTexture>)wrapped->texture)) { *reason = 2; return 0; }
        }
        wrapped = targets->depth_texture;
        if (wrapped && metal_texture_aliases(texture, (__bridge id<MTLTexture>)wrapped->texture)) { *reason = 2; return 0; }
    }
    return 1;
}
static int metal_render_matches(SkyMetalSubmitState *submit,const sky_mtl_clear_pass_t *targets,id<MTLBuffer> visibility) {
    if(!submit->encoder||submit->visibility!=visibility||submit->color_count!=targets->color_count||
        targets->has_color_clear||targets->depth_mask||targets->stencil_mask)return 0;
    for(uint32_t i=0;i<targets->color_count;i++) {
        sky_mtl_texture_t *texture=targets->textures[i];
        if(submit->colors[i]!=(texture?texture->texture:NULL))return 0;
    }
    sky_mtl_texture_t *depth=targets->depth_texture;
    return submit->depth==(depth?depth->texture:NULL);
}

enum { SKY_MTL_PIPELINE_CACHE_BUCKETS = 256 };
typedef struct {
    uintptr_t vertex_shader;
    uintptr_t fragment_shader;
    struct { uint32_t format, offset, stream; } attributes[16];
    struct { uint32_t stride, divisor; } streams[16];
    uint32_t channel_mask;
    uint32_t blend_enable;
    struct { uint8_t rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha; } blend[8];
    uint32_t formats[10];
} sky_mtl_pipeline_key_t;

typedef struct sky_mtl_pipeline_entry {
    struct sky_mtl_pipeline_entry *next;
    sky_mtl_pipeline_key_t key;
    const void *state;
} sky_mtl_pipeline_entry_t;
static sky_mtl_pipeline_entry_t *g_pipeline_cache[SKY_MTL_PIPELINE_CACHE_BUCKETS];

static id<MTLRenderPipelineState> metal_render_pipeline(const sky_mtl_render_state_t *s,
    const sky_mtl_clear_pass_t *targets, SkyMetalShader *vertex, SkyMetalShader *fragment) {
    sky_mtl_pipeline_key_t key = {0};
    key.vertex_shader = (uintptr_t)(__bridge const void *)vertex;
    key.fragment_shader = (uintptr_t)(__bridge const void *)fragment;
    memcpy(key.attributes, s->attributes, sizeof key.attributes);
    memcpy(key.streams, s->streams, sizeof key.streams);
    key.channel_mask = s->channel_mask;
    key.blend_enable = s->blend_enable;
    memcpy(key.blend, s->blend, sizeof key.blend);
    for (uint32_t i = 0; i < targets->color_count; i++) {
        sky_mtl_texture_t *wrapped = targets->textures[i];
        if (!wrapped || !wrapped->texture) continue;
        key.formats[i] = wrapped->properties.pixel_format;
        key.formats[9] = (uint32_t)wrapped->properties.sample_count;
    }
    sky_mtl_texture_t *wrapped = targets->depth_texture;
    if (wrapped && wrapped->texture) {
        key.formats[8] = wrapped->properties.pixel_format;
        key.formats[9] = (uint32_t)wrapped->properties.sample_count;
    }
    uint32_t hash = 2166136261u;
    const uint32_t *words = (const uint32_t *)&key;
    for (unsigned i = 0; i < sizeof(key) / sizeof(uint32_t); i++) hash = (hash ^ words[i]) * 16777619u;
    unsigned slot = hash & (SKY_MTL_PIPELINE_CACHE_BUCKETS - 1);
    for (sky_mtl_pipeline_entry_t *entry = g_pipeline_cache[slot]; entry; entry = entry->next) {
        if (!memcmp(&entry->key, &key, sizeof key)) return (__bridge id<MTLRenderPipelineState>)entry->state;
    }
    /* The key above is unchanged; descriptor setup is needed only on a miss. */
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vertex.function;
    d.fragmentFunction = fragment.function;
    for (uint32_t i = 0; i < targets->color_count; i++) {
        if (!key.formats[i]) continue;
        d.colorAttachments[i].pixelFormat = key.formats[i];
        uint32_t mask = s->channel_mask >> (i * 4) & 15;
        d.colorAttachments[i].writeMask =
            (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
            (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
        d.colorAttachments[i].blendingEnabled = (s->blend_enable >> i & 1) != 0;
        d.colorAttachments[i].rgbBlendOperation = metal_blend_operation(s->blend[i].rgb_op);
        d.colorAttachments[i].alphaBlendOperation = metal_blend_operation(s->blend[i].alpha_op);
        d.colorAttachments[i].sourceRGBBlendFactor = metal_blend_factor(s->blend[i].src_rgb);
        d.colorAttachments[i].destinationRGBBlendFactor = metal_blend_factor(s->blend[i].dst_rgb);
        d.colorAttachments[i].sourceAlphaBlendFactor = metal_blend_factor(s->blend[i].src_alpha);
        d.colorAttachments[i].destinationAlphaBlendFactor = metal_blend_factor(s->blend[i].dst_alpha);
    }
    if (key.formats[8]) {
        if (key.formats[8] != MTLPixelFormatStencil8) d.depthAttachmentPixelFormat = key.formats[8];
        if (key.formats[8] == MTLPixelFormatStencil8 || key.formats[8] == MTLPixelFormatDepth32Float_Stencil8)
            d.stencilAttachmentPixelFormat = key.formats[8];
    }
    d.rasterSampleCount = MAX(key.formats[9], 1u);
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t v_idx = 0; v_idx < vertex->vertex_input_count; v_idx++) {
        int attr = vertex->vertex_inputs[v_idx].location;
        if (attr < 0 || attr >= 16) return nil;
        uint32_t stream = s->attributes[attr].stream;
        MTLVertexFormat format = metal_vertex_format(s->attributes[attr].format);
        if(format==MTLVertexFormatInvalid&&s->attributes[attr].format){
            fprintf(stderr,"NVN_METAL_PIPELINE_ERROR reason=unsupported-vertex-format attribute=%d format=%#x\n",attr,s->attributes[attr].format);return nil;
        }
        if(stream>=16){fprintf(stderr,"NVN_METAL_PIPELINE_ERROR reason=invalid-vertex-stream stream=%u\n",stream);return nil;}
        if (format == MTLVertexFormatInvalid) {
            vd.attributes[attr].format = vertex->vertex_inputs[v_idx].fallback_format;
            vd.attributes[attr].bufferIndex = 24;
            vd.layouts[24].stride = 16;
            vd.layouts[24].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[24].stepRate=0;
            continue;
        }
        vd.attributes[attr].format = format;
        vd.attributes[attr].offset = s->attributes[attr].offset;
        vd.attributes[attr].bufferIndex = 8 + stream;
        vd.layouts[8+stream].stride=s->streams[stream].stride?s->streams[stream].stride:4;
        vd.layouts[8+stream].stepFunction=!s->streams[stream].stride?MTLVertexStepFunctionConstant:s->streams[stream].divisor?MTLVertexStepFunctionPerInstance:MTLVertexStepFunctionPerVertex;
        vd.layouts[8+stream].stepRate=!s->streams[stream].stride?0:MAX(s->streams[stream].divisor,1u);
    }
    d.vertexDescriptor = vd;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [shared_device() newRenderPipelineStateWithDescriptor:d error:&error];
    if (!pipeline) fprintf(stderr, "NVN_METAL_PIPELINE_ERROR vs=%llu fs=%llu error=%s\n",
        (unsigned long long)s->programs[0], (unsigned long long)s->programs[1], error.description.UTF8String);
    else {
        sky_mtl_pipeline_entry_t *entry = malloc(sizeof *entry);
        if (entry) {
            entry->key = key;
            entry->state = CFBridgingRetain(pipeline);
            entry->next = g_pipeline_cache[slot];
            g_pipeline_cache[slot] = entry;
        }
    }
    return pipeline;
}

static MTLStencilOperation metal_stencil_operation(int nvn) {
    static const MTLStencilOperation map[] = {MTLStencilOperationKeep, MTLStencilOperationKeep,
        MTLStencilOperationZero, MTLStencilOperationReplace, MTLStencilOperationIncrementClamp,
        MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
        MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap};
    return map[nvn >= 0 && nvn < 9 ? nvn : 0];
}

static void metal_dump_draw(const sky_mtl_operation_t *op,const sky_mtl_render_state_t *s,SkyMetalShader *fragment,uint64_t number) {
    fprintf(stderr,"NVN_DRAW_DIAGNOSTIC number=%llu primitive=%u first=%u count=%u instances=%u index=%#llx base_vertex=%d cull=%u front=%u polygon=%u channel=%#x blend=%#x depth={test:%u,write:%u,func:%u} viewport={enabled:%u,%g,%g,%g,%g,%g,%g} scissor={enabled:%u,%d,%d,%d,%d} blend0={op:%u/%u,src:%u/%u,dst:%u/%u}\n",
        (unsigned long long)number,op->primitive,op->first,op->count,op->instances,(unsigned long long)op->index_address,op->base_vertex,
        s->cull_face,s->front_face,s->polygon_mode,s->channel_mask,s->blend_enable,s->depth_test,s->depth_write,s->depth_func,
        s->has_viewport,s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3],s->viewport[4],s->viewport[5],
        s->has_scissor,s->scissor[0],s->scissor[1],s->scissor[2],s->scissor[3],
        s->blend[0].rgb_op,s->blend[0].alpha_op,s->blend[0].src_rgb,s->blend[0].src_alpha,s->blend[0].dst_rgb,s->blend[0].dst_alpha);
    uint32_t stream=s->attributes[0].stream;
    if(stream<16&&s->vertex[stream].address) {
        NSUInteger offset=0;id<MTLBuffer> buffer=metal_buffer(s->vertex[stream].address,1,&offset);
        fprintf(stderr,"NVN_VERTEX_DIAGNOSTIC stream=%u address=%#llx size=%llu stride=%u divisor=%u buffer_offset=%llu\n",stream,
            (unsigned long long)s->vertex[stream].address,(unsigned long long)s->vertex[stream].size,s->streams[stream].stride,s->streams[stream].divisor,(unsigned long long)offset);
        for(uint32_t i=0;i<MIN(op->count,4u);i++) {
            int64_t vertex=(int64_t)op->first+i;
            if(op->index_address) {
                NSUInteger io=0;id<MTLBuffer> indices=metal_buffer(op->index_address,((uint64_t)i+1)*(1u<<op->index_type),&io);
                if(!indices||!indices.contents)break;
                uint32_t value=0;memcpy(&value,(const uint8_t *)indices.contents+io+(i<<op->index_type),1u<<op->index_type);vertex=(int64_t)value+op->base_vertex;
            }
            uint64_t at=offset+(uint64_t)MAX(vertex,0)*s->streams[stream].stride+s->attributes[0].offset;
            if(vertex<0||!buffer.contents||s->attributes[0].format!=0x2e||at+16>buffer.length)continue;
            float position[4];memcpy(position,(const uint8_t *)buffer.contents+at,16);
            fprintf(stderr,"NVN_VERTEX_DIAGNOSTIC vertex=%lld position=%g,%g,%g,%g",(long long)vertex,position[0],position[1],position[2],position[3]);
            if(s->attributes[1].format==0x11&&s->attributes[1].stream==stream){
                uint64_t uv_at=offset+(uint64_t)vertex*s->streams[stream].stride+s->attributes[1].offset;
                if(uv_at+4<=buffer.length){__fp16 uv[2];memcpy(uv,(const uint8_t *)buffer.contents+uv_at,4);fprintf(stderr," uv=%g,%g",(double)uv[0],(double)uv[1]);}
            }
            fprintf(stderr,"\n");
        }
    }
    if(fragment.constants){const float *f=fragment.constants.contents;fprintf(stderr,"NVN_C1_DIAGNOSTIC fs=%llu first=%g,%g,%g,%g\n",(unsigned long long)s->programs[1],f[0],f[1],f[2],f[3]);}
    for(int slot=0;slot<3;slot++) {
        sky_mtl_texture_t *wrapped=s->textures[1][slot];id<MTLTexture> t=wrapped?(__bridge id<MTLTexture>)wrapped->texture:nil;
        if(!t)continue;
        fprintf(stderr,"NVN_TEXTURE_DIAGNOSTIC slot=%d handle=%#llx format=%lu size=%lux%lu storage=%lu",slot,(unsigned long long)s->texture_handles[1][slot],(unsigned long)t.pixelFormat,(unsigned long)t.width,(unsigned long)t.height,(unsigned long)t.storageMode);
        id<MTLBuffer> backing=t.buffer;
        if(backing&&backing.contents&&t.pixelFormat==MTLPixelFormatR8Unorm){
            const uint8_t *p=(const uint8_t *)backing.contents+t.bufferOffset;NSUInteger pitch=t.bufferBytesPerRow;
            unsigned lo=255,hi=0;uint64_t sum=0,n=0;
            for(NSUInteger y=0;y<t.height;y+=MAX(t.height/32,1ul))for(NSUInteger x=0;x<t.width;x+=MAX(t.width/32,1ul)){
                unsigned value=p[y*pitch+x];lo=MIN(lo,value);hi=MAX(hi,value);sum+=value;n++;
            }
            fprintf(stderr," row=%lu offset=%lu sample_min=%u sample_max=%u sample_mean=%g",(unsigned long)pitch,(unsigned long)t.bufferOffset,lo,hi,n?(double)sum/n:0);
        }
        fprintf(stderr,"\n");
    }
}

static int metal_encode_draw(id<MTLCommandBuffer> buffer,const sky_mtl_operation_t *op,const sky_mtl_render_state_t *s,
    const sky_mtl_clear_pass_t *targets,id<MTLBuffer> visibility,NSUInteger visibility_offset,SkyMetalSubmitState *submit) {
    static uint64_t layouts[64];static unsigned layout_count;
    if (layout_count < 64) {
        uint64_t layout_hash=1469598103934665603ull;
        const uint8_t *layout_bytes=(const uint8_t *)s->attributes;
        for(size_t i=0;i<sizeof s->attributes;i++)layout_hash=(layout_hash^layout_bytes[i])*1099511628211ull;
        int known=0;for(unsigned i=0;i<layout_count;i++)if(layouts[i]==layout_hash)known=1;
        if(!known){
            layouts[layout_count++]=layout_hash;
            fprintf(stderr,"NVN_VERTEX_LAYOUT vs=%llu attributes=%u",(unsigned long long)s->programs[0],s->attribute_count);
            for(unsigned i=0;i<s->attribute_count&&i<16;i++)fprintf(stderr," attr%u={format:%#x,stream:%u,offset:%u}",i,s->attributes[i].format,s->attributes[i].stream,s->attributes[i].offset);
            fprintf(stderr,"\n");
        }
    }
    SkyMetalShader *vertex = metal_shader(s->programs[0], 0);
    SkyMetalShader *fragment = s->programs[1] ? metal_shader(s->programs[1], 1) : nil;
    if (!vertex || (s->programs[1] && !fragment) || s->programs[2] || s->programs[3] || s->programs[4]) {
        fprintf(stderr, "NVN_METAL_DRAW_ERROR reason=missing-or-unsupported-shader-stage vs=%llu fs=%llu\n",
                (unsigned long long)s->programs[0], (unsigned long long)s->programs[1]);
        return 0;
    }
    static uint64_t diagnostic_count;uint64_t diagnostic_number=++diagnostic_count;
    int diagnostic=diagnostic_number<=4||diagnostic_number==60||diagnostic_number==120||diagnostic_number==180;
    if(diagnostic){
        metal_dump_draw(op,s,fragment,diagnostic_number);
        if(!visibility){
            visibility=[shared_device() newBufferWithLength:8 options:MTLResourceStorageModeShared];visibility_offset=0;
            id<MTLBuffer> result=visibility;
            if(result)[buffer addCompletedHandler:^(id<MTLCommandBuffer> completed){
                if(completed.status==MTLCommandBufferStatusCompleted)fprintf(stderr,"NVN_RASTER_DIAGNOSTIC number=%llu samples=%llu\n",(unsigned long long)diagnostic_number,(unsigned long long)*(const uint64_t *)result.contents);
            }];
        }
    }
    id<MTLRenderPipelineState> pipeline = metal_render_pipeline(s, targets, vertex, fragment);
    if (!pipeline) return 0;
    MTLPrimitiveType primitive;
    switch (op->primitive) {
        case 0: primitive = MTLPrimitiveTypePoint; break;
        case 1: primitive = MTLPrimitiveTypeLine; break;
        case 3: primitive = MTLPrimitiveTypeLineStrip; break;
        case 4: primitive = MTLPrimitiveTypeTriangle; break;
        case 5: primitive = MTLPrimitiveTypeTriangleStrip; break;
        default: fprintf(stderr, "NVN_METAL_DRAW_ERROR reason=unsupported-topology primitive=%u\n", op->primitive); return 0;
    }
    unsigned exclusion=0;
    int merge=submit->reuse&&metal_merge_safe(vertex,s,0,targets,&exclusion)&&
        (!fragment||metal_merge_safe(fragment,s,1,targets,&exclusion));
    if(exclusion==1)submit->writable_draws++;else if(exclusion==2)submit->feedback_draws++;
    if(!merge||!metal_render_matches(submit,targets,visibility)) {
        if(merge&&submit->encoder)submit->pass_changes++;
        metal_render_end(submit);
        submit->pass=metal_render_targets(targets);submit->pass.visibilityResultBuffer=visibility;
        submit->encoder=[buffer renderCommandEncoderWithDescriptor:submit->pass];
        if(!submit->encoder)return 0;
        submit->render_encoders++;submit->visibility=visibility;submit->color_count=targets->color_count;submit->vertex_mask=0;
        for(uint32_t i=0;i<targets->color_count;i++) {
            sky_mtl_texture_t *t=targets->textures[i];submit->colors[i]=t?t->texture:NULL;
        }
        sky_mtl_texture_t *t=targets->depth_texture;submit->depth=t?t->texture:NULL;
    } else submit->merged_draws++;
    id<MTLRenderCommandEncoder> encoder=submit->encoder;
    if(visibility)[encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:visibility_offset];
    [encoder setRenderPipelineState:pipeline];
    id<MTLDepthStencilState> depth_state=metal_depth_stencil_state(s,targets,submit);
    if(!depth_state){metal_render_end(submit);return 0;}
    [encoder setDepthStencilState:depth_state];
    [encoder setStencilFrontReferenceValue:s->stencil[0].reference backReferenceValue:s->stencil[1].reference];
    [encoder setFrontFacingWinding:s->front_face ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [encoder setCullMode:s->cull_face == 1 ? MTLCullModeFront : s->cull_face == 2 ? MTLCullModeBack : MTLCullModeNone];
    [encoder setTriangleFillMode:s->polygon_mode == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    /* NVN uses factor, units, clamp; Metal takes units before slopeScale.
     * Offset enables select the polygon rasterization mode: point/line/fill. */
    int offset_enabled=s->polygon_offset_enables&(1u<<s->polygon_mode);
    [encoder setDepthBias:offset_enabled?s->polygon_offset_units:0
              slopeScale:offset_enabled?s->polygon_offset_factor:0
                   clamp:offset_enabled?s->polygon_offset_clamp:0];
    [encoder setBlendColorRed:s->blend_color[0] green:s->blend_color[1] blue:s->blend_color[2] alpha:s->blend_color[3]];
    /* Rebind the defaults as well: a previous draw in this encoder may have
     * installed a viewport/scissor even when this draw has none. */
    sky_mtl_texture_t *attachment=targets->depth_texture;
    uint64_t width=attachment?attachment->properties.width:UINT64_MAX,height=attachment?attachment->properties.height:UINT64_MAX;
    for(uint32_t i=0;i<targets->color_count;i++)if((attachment=targets->textures[i])) {
        width=MIN(width,attachment->properties.width);height=MIN(height,attachment->properties.height);
    }
    if(width==UINT64_MAX)width=0;if(height==UINT64_MAX)height=0;
    [encoder setViewport:s->has_viewport?(MTLViewport){s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3],s->viewport[4],s->viewport[5]}:
        (MTLViewport){0,0,width,height,0,1}];
    if (s->has_scissor) {
        int64_t left=MAX(s->scissor[0],0),top=MAX(s->scissor[1],0);
        int64_t right=MIN((int64_t)s->scissor[0]+s->scissor[2],(int64_t)width);
        int64_t bottom=MIN((int64_t)s->scissor[1]+s->scissor[3],(int64_t)height);
        [encoder setScissorRect:(MTLScissorRect){MIN((uint64_t)left,width),MIN((uint64_t)top,height),MAX(right-left,0),MAX(bottom-top,0)}];
    } else [encoder setScissorRect:(MTLScissorRect){0,0,width,height}];
    int valid = encode_shader_arguments(encoder, vertex, s, 0,submit) && (!fragment || encode_shader_arguments(encoder, fragment, s, 1,submit));
    for (int stream = 0; stream < 16 && valid; stream++) {
        if (!s->vertex[stream].address) {
            if(submit->vertex_mask&(1u<<stream)){[encoder setVertexBuffer:nil offset:0 atIndex:8+stream];submit->vertex_mask&=~(1u<<stream);}
            continue;
        }
        NSUInteger offset = 0;
        id<MTLBuffer> vertices = metal_buffer(s->vertex[stream].address, MAX(s->vertex[stream].size, 1u), &offset);
        if (!vertices) { valid = 0; break; }
        [encoder setVertexBuffer:vertices offset:offset atIndex:8 + stream];
        submit->vertex_mask|=1u<<stream;
    }
    const float default_attribute[4] = {0, 0, 0, 1};
    [encoder setVertexBytes:default_attribute length:sizeof default_attribute atIndex:24];
    if (valid && op->index_address) {
        NSUInteger offset = 0;
        uint64_t index_bytes = (uint64_t)op->count * (1u << op->index_type);
        id<MTLBuffer> indices = metal_buffer(op->index_address, index_bytes, &offset);
        if (!indices || op->index_type > 2) valid = 0;
        else {
            if (op->index_type == 0) {
                id<MTLBuffer> converted = [shared_device() newBufferWithLength:MAX(op->count * 2ull, 2ull) options:MTLResourceStorageModeShared];
                const uint8_t *source = (const uint8_t *)indices.contents + offset;
                uint16_t *destination = converted.contents;
                for (uint32_t i = 0; i < op->count; i++) destination[i] = source[i];
                indices = converted;
                offset = 0;
            }
            [encoder drawIndexedPrimitives:primitive indexCount:op->count
                indexType:op->index_type == 2 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                indexBuffer:indices indexBufferOffset:offset instanceCount:op->instances
                baseVertex:op->base_vertex baseInstance:op->base_instance];
        }
    } else if (valid) [encoder drawPrimitives:primitive vertexStart:op->first vertexCount:op->count
        instanceCount:op->instances baseInstance:op->base_instance];
    if(!merge||!valid)metal_render_end(submit);
    return valid;
}

static int metal_clear_texture(id<MTLCommandBuffer> buffer,const sky_mtl_operation_t *op,SkyMetalSubmitState *submit) {
    sky_mtl_texture_t *wrapped=op->targets.textures[0];
    id<MTLTexture> texture=wrapped?(__bridge id<MTLTexture>)wrapped->texture:nil;
    if(!texture||!(texture.usage&MTLTextureUsageRenderTarget)){
        fprintf(stderr,"NVN_METAL_CLEAR_TEXTURE_ERROR reason=texture-not-renderable\n");return 0;
    }
    int64_t x=op->region[0],y=op->region[1],z=op->region[2],w=op->region[3],h=op->region[4],n=op->region[5];
    if(x<0||y<0||z<0||w<=0||h<=0||n<=0||x+w>(int64_t)texture.width||y+h>(int64_t)texture.height)return 0;
    BOOL is3d=texture.textureType==MTLTextureType3D;
    uint64_t slices=is3d?texture.depth:texture.arrayLength;
    if(texture.textureType==MTLTextureTypeCube||texture.textureType==MTLTextureTypeCubeArray)slices*=6;
    if((uint64_t)(z+n)>slices)return 0;
    MTLPixelFormat format=texture.pixelFormat;
    BOOL depth=format==MTLPixelFormatDepth16Unorm||format==MTLPixelFormatDepth32Float||format==MTLPixelFormatDepth32Float_Stencil8;
    BOOL stencil=format==MTLPixelFormatStencil8||format==MTLPixelFormatDepth32Float_Stencil8;
    BOOL full=x==0&&y==0&&w==(int64_t)texture.width&&h==(int64_t)texture.height&&op->clear_mask==15;
    if((depth||stencil)&&!full){fprintf(stderr,"NVN_METAL_CLEAR_TEXTURE_ERROR reason=partial-depth-stencil\n");return 0;}
    id<MTLRenderPipelineState> pipeline=nil;
    if(!full) {
        static NSMutableDictionary<NSString *,id<MTLRenderPipelineState>> *pipelines;
        if(!pipelines)pipelines=[NSMutableDictionary new];
        NSString *key=[NSString stringWithFormat:@"%lu-%u-%u",(unsigned long)format,op->clear_mask,op->clear_value_type];
        pipeline=pipelines[key];
        if(!pipeline) {
            NSString *type=op->clear_value_type==1?@"int4":op->clear_value_type==2?@"uint4":@"float4";
            NSString *source=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 vs(uint i [[vertex_id]]) {return float4(float2((i<<1)&2,i&2)*2-1,0,1);} fragment %@ fs(constant %@& c [[buffer(0)]]) {return c;}",type,type];
            NSError *error=nil;id<MTLLibrary> library=[shared_device() newLibraryWithSource:source options:nil error:&error];
            MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
            d.vertexFunction=[library newFunctionWithName:@"vs"];d.fragmentFunction=[library newFunctionWithName:@"fs"];
            d.colorAttachments[0].pixelFormat=format;
            uint32_t m=op->clear_mask;
            d.colorAttachments[0].writeMask=(m&1?MTLColorWriteMaskRed:0)|(m&2?MTLColorWriteMaskGreen:0)|(m&4?MTLColorWriteMaskBlue:0)|(m&8?MTLColorWriteMaskAlpha:0);
            d.rasterSampleCount=texture.sampleCount;
            pipeline=[shared_device() newRenderPipelineStateWithDescriptor:d error:&error];
            if(!pipeline){fprintf(stderr,"NVN_METAL_CLEAR_TEXTURE_ERROR reason=pipeline error=%s\n",error.description.UTF8String);return 0;}
            pipelines[key]=pipeline;
        }
    }
    for(int64_t slice=z;slice<z+n;slice++) {
        MTLRenderPassDescriptor *d=[MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassAttachmentDescriptor *attachment=depth?d.depthAttachment:stencil?d.stencilAttachment:d.colorAttachments[0];
        attachment.texture=texture;attachment.storeAction=MTLStoreActionStore;
        attachment.loadAction=full?MTLLoadActionClear:MTLLoadActionLoad;
        if(is3d)attachment.depthPlane=(NSUInteger)slice;else attachment.slice=(NSUInteger)slice;
        if(depth) {
            d.depthAttachment.clearDepth=op->clear_value[0];
            if(stencil){d.stencilAttachment.texture=texture;d.stencilAttachment.slice=(NSUInteger)slice;d.stencilAttachment.storeAction=MTLStoreActionStore;d.stencilAttachment.loadAction=MTLLoadActionClear;d.stencilAttachment.clearStencil=(uint32_t)op->clear_value[1];}
        } else if(stencil)d.stencilAttachment.clearStencil=(uint32_t)op->clear_value[0];
        else d.colorAttachments[0].clearColor=MTLClearColorMake(op->clear_value[0],op->clear_value[1],op->clear_value[2],op->clear_value[3]);
        id<MTLRenderCommandEncoder> encoder=[buffer renderCommandEncoderWithDescriptor:d];
        if(encoder)submit->render_encoders++;
        if(!encoder)return 0;
        if(!full) {
            [encoder setRenderPipelineState:pipeline];[encoder setScissorRect:(MTLScissorRect){x,y,w,h}];
            uint32_t values[4];for(int i=0;i<4;i++) {
                if(op->clear_value_type==1)values[i]=(uint32_t)(int32_t)op->clear_value[i];
                else if(op->clear_value_type==2)values[i]=(uint32_t)op->clear_value[i];
                else {float f=(float)op->clear_value[i];memcpy(values+i,&f,4);}
            }
            [encoder setFragmentBytes:values length:sizeof values atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [encoder endEncoding];
    }
    return 1;
}

static inline uint64_t metal_load_u64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

void sky_mtl_render_state_merge(sky_mtl_render_state_t *d,const sky_mtl_render_state_t *s) {
    for(int stage=0;stage<6;stage++) {
        if(s->program_mask&(1u<<stage))d->programs[stage]=s->programs[stage];
        for(int base=0;base<16;base+=8) {
            if(metal_load_u64(&s->uniform_bound[stage][base])) {
                for(int slot=base;slot<base+8;slot++)if(s->uniform_bound[stage][slot]){d->uniform[stage][slot]=s->uniform[stage][slot];d->uniform_bound[stage][slot]=1;}
            }
            if(metal_load_u64(&s->storage_bound[stage][base])) {
                for(int slot=base;slot<base+8;slot++)if(s->storage_bound[stage][slot]){d->storage[stage][slot]=s->storage[stage][slot];d->storage_bound[stage][slot]=1;}
            }
        }
        for(int base=0;base<128;base+=8) {
            if(metal_load_u64(&s->texture_bound[stage][base])) {
                for(int slot=base;slot<base+8;slot++)if(s->texture_bound[stage][slot]){d->texture_handles[stage][slot]=s->texture_handles[stage][slot];d->texture_bound[stage][slot]=1;}
            }
            if(metal_load_u64(&s->sampler_bound[stage][base])) {
                for(int slot=base;slot<base+8;slot++)if(s->sampler_bound[stage][slot]){d->sampler_handles[stage][slot]=s->sampler_handles[stage][slot];d->sampler_bound[stage][slot]=1;}
            }
        }
        for(int base=0;base<32;base+=8) {
            if(metal_load_u64(&s->image_bound[stage][base])) {
                for(int slot=base;slot<base+8;slot++)if(s->image_bound[stage][slot]){d->image_handles[stage][slot]=s->image_handles[stage][slot];d->image_bound[stage][slot]=1;}
            }
        }
    }
    for(int base=0;base<16;base+=8) {
        if(metal_load_u64(&s->vertex_bound[base])) {
            for(int slot=base;slot<base+8;slot++)if(s->vertex_bound[slot]){d->vertex[slot]=s->vertex[slot];d->vertex_bound[slot]=1;}
        }
    }
    if(s->pool_mask&1)d->texture_pool=s->texture_pool;
    if(s->pool_mask&2)d->sampler_pool=s->sampler_pool;
    uint32_t m=s->changed;
    if(m&SKY_MTL_STATE_ATTRIBUTES){memcpy(d->attributes,s->attributes,sizeof d->attributes);d->attribute_count=s->attribute_count;}
    if(m&SKY_MTL_STATE_STREAMS){memcpy(d->streams,s->streams,sizeof d->streams);d->stream_count=s->stream_count;}
    if(m&SKY_MTL_STATE_CHANNEL)d->channel_mask=s->channel_mask;
    if(m&SKY_MTL_STATE_COLOR)d->blend_enable=s->blend_enable;
    if(m&SKY_MTL_STATE_DEPTH){
        d->depth_test=s->depth_test;d->depth_write=s->depth_write;d->depth_func=s->depth_func;d->stencil_test=s->stencil_test;
        for(int i=0;i<2;i++){
            d->stencil[i].function=s->stencil[i].function;d->stencil[i].fail=s->stencil[i].fail;
            d->stencil[i].depth_fail=s->stencil[i].depth_fail;d->stencil[i].pass=s->stencil[i].pass;
        }
    }
    if(m&SKY_MTL_STATE_POLYGON){d->cull_face=s->cull_face;d->front_face=s->front_face;d->polygon_mode=s->polygon_mode;d->polygon_offset_enables=s->polygon_offset_enables;}
    if(m&SKY_MTL_STATE_POLYGON_OFFSET){d->polygon_offset_factor=s->polygon_offset_factor;d->polygon_offset_units=s->polygon_offset_units;d->polygon_offset_clamp=s->polygon_offset_clamp;}
    if(m&SKY_MTL_STATE_VIEWPORT){memcpy(d->viewport,s->viewport,4*sizeof(double));d->has_viewport=s->has_viewport;}
    if(m&SKY_MTL_STATE_DEPTH_RANGE){d->viewport[4]=s->viewport[4];d->viewport[5]=s->viewport[5];}
    if(m&SKY_MTL_STATE_SCISSOR){memcpy(d->scissor,s->scissor,sizeof d->scissor);d->has_scissor=s->has_scissor;}
    if(m&SKY_MTL_STATE_BLEND_COLOR)memcpy(d->blend_color,s->blend_color,sizeof d->blend_color);
    if(s->blend_mask)for(int i=0;i<8;i++)if(s->blend_mask&(1u<<i))d->blend[i]=s->blend[i];
    for(int i=0;i<2;i++) {
        if(m&(SKY_MTL_STATE_STENCIL_READ_FRONT<<i))d->stencil[i].read_mask=s->stencil[i].read_mask;
        if(m&(SKY_MTL_STATE_STENCIL_WRITE_FRONT<<i))d->stencil[i].write_mask=s->stencil[i].write_mask;
        if(m&(SKY_MTL_STATE_STENCIL_REF_FRONT<<i))d->stencil[i].reference=s->stencil[i].reference;
    }
    if(m&SKY_MTL_STATE_COPY_ROW)d->copy_row_stride=s->copy_row_stride;
    if(m&SKY_MTL_STATE_COPY_IMAGE)d->copy_image_stride=s->copy_image_stride;
    d->changed|=s->changed;d->program_mask|=s->program_mask;d->pool_mask|=s->pool_mask;d->blend_mask|=s->blend_mask;
}

static void metal_replay_state_release(sky_mtl_render_state_t *state) {
    for(int stage=0;stage<6;stage++) {
        if(state->programs[stage])sky_mtl_program_release(state->programs[stage]);
        for(int slot=0;slot<128;slot++){
            if(state->textures[stage][slot])sky_mtl_texture_destroy(state->textures[stage][slot]);
            if(state->samplers[stage][slot])sky_mtl_sampler_destroy(state->samplers[stage][slot]);
        }
        for(int slot=0;slot<32;slot++)if(state->images[stage][slot])sky_mtl_texture_destroy(state->images[stage][slot]);
    }
}
static void metal_replay_destroy(sky_mtl_command_queue_t *queue) {
    if(!queue->replay)return;
    metal_replay_state_release(&queue->replay->state);
    for(int i=0;i<8;i++)sky_mtl_texture_destroy(queue->replay->targets.textures[i]);
    sky_mtl_texture_destroy(queue->replay->targets.depth_texture);free(queue->replay);queue->replay=NULL;
}
static void metal_replay_apply(sky_mtl_command_queue_t *queue,const sky_mtl_operation_t *op,sky_mtl_clear_pass_t *out_targets,int *resolved_in_submit) {
    if(!queue->replay) {
        queue->replay=calloc(1,sizeof *queue->replay);
        queue->replay->state.channel_mask=UINT32_MAX;queue->replay->state.viewport[5]=1;
        queue->replay->state.front_face=1;queue->replay->state.polygon_mode=2;
        for(int i=0;i<8;i++){
            queue->replay->state.blend[i].rgb_op=queue->replay->state.blend[i].alpha_op=1;
            queue->replay->state.blend[i].src_rgb=queue->replay->state.blend[i].src_alpha=2;
            queue->replay->state.blend[i].dst_rgb=queue->replay->state.blend[i].dst_alpha=1;
        }
        for(int i=0;i<2;i++)queue->replay->state.stencil[i].read_mask=queue->replay->state.stencil[i].write_mask=255;
    }
    sky_mtl_render_state_t *prev=&queue->replay->state;
    void *new_tex_pool=(op->state.pool_mask&1)?op->state.texture_pool:prev->texture_pool;
    void *new_smp_pool=(op->state.pool_mask&2)?op->state.sampler_pool:prev->sampler_pool;
    int full_resolve=!*resolved_in_submit || new_tex_pool!=prev->texture_pool || new_smp_pool!=prev->sampler_pool;
    if(full_resolve) {
        for(int stage=0;stage<6;stage++) {
            uint64_t prog=(op->state.program_mask&(1u<<stage))?op->state.programs[stage]:prev->programs[stage];
            sky_mtl_program_retain(prog);
        }
        metal_replay_state_release(prev);
        sky_mtl_render_state_merge(prev,&op->state);
        sky_nvn_graphics_resources_resolve(prev);
        *resolved_in_submit=1;
    } else {
        if(op->state.program_mask) {
            for(int stage=0;stage<6;stage++) {
                if((op->state.program_mask&(1u<<stage))&&op->state.programs[stage]!=prev->programs[stage]) {
                    sky_mtl_program_retain(op->state.programs[stage]);
                    sky_mtl_program_release(prev->programs[stage]);
                }
            }
        }
        sky_mtl_render_state_t delta;
        int need_resolve=0;
        for(int stage=0;stage<6;stage++) {
            for(int base=0;base<128;base+=8) {
                if(!(metal_load_u64(&op->state.texture_bound[stage][base])|metal_load_u64(&op->state.sampler_bound[stage][base])))continue;
                for(int slot=base;slot<base+8;slot++) {
                    int tex_changed=op->state.texture_bound[stage][slot]&&(!prev->texture_bound[stage][slot]||prev->texture_handles[stage][slot]!=op->state.texture_handles[stage][slot]);
                    int smp_changed=op->state.sampler_bound[stage][slot]&&(!prev->sampler_bound[stage][slot]||prev->sampler_handles[stage][slot]!=op->state.sampler_handles[stage][slot]);
                    if(!tex_changed&&!smp_changed)continue;
                    if(!need_resolve){
                        memset(&delta,0,sizeof delta);
                        delta.texture_pool=new_tex_pool;
                        delta.sampler_pool=new_smp_pool;
                        need_resolve=1;
                    }
                    uint8_t merged_tex_bound=op->state.texture_bound[stage][slot]?1:prev->texture_bound[stage][slot];
                    uint64_t merged_tex_handle=op->state.texture_bound[stage][slot]?op->state.texture_handles[stage][slot]:prev->texture_handles[stage][slot];
                    uint8_t merged_smp_bound=op->state.sampler_bound[stage][slot]?1:prev->sampler_bound[stage][slot];
                    uint64_t merged_smp_handle=op->state.sampler_bound[stage][slot]?op->state.sampler_handles[stage][slot]:prev->sampler_handles[stage][slot];
                    if(tex_changed) {
                        delta.texture_bound[stage][slot]=1;
                        delta.texture_handles[stage][slot]=merged_tex_handle;
                    }
                    if(smp_changed||(tex_changed&&!merged_smp_bound)) {
                        delta.sampler_bound[stage][slot]=merged_smp_bound;
                        delta.sampler_handles[stage][slot]=merged_smp_handle;
                        delta.texture_bound[stage][slot]=merged_tex_bound;
                        delta.texture_handles[stage][slot]=merged_tex_handle;
                    }
                }
            }
            for(int base=0;base<32;base+=8) {
                if(!metal_load_u64(&op->state.image_bound[stage][base]))continue;
                for(int slot=base;slot<base+8;slot++) {
                    if(op->state.image_bound[stage][slot]&&(!prev->image_bound[stage][slot]||prev->image_handles[stage][slot]!=op->state.image_handles[stage][slot])) {
                        if(!need_resolve){
                            memset(&delta,0,sizeof delta);
                            delta.texture_pool=new_tex_pool;
                            delta.sampler_pool=new_smp_pool;
                            need_resolve=1;
                        }
                        delta.image_bound[stage][slot]=1;
                        delta.image_handles[stage][slot]=op->state.image_handles[stage][slot];
                    }
                }
            }
        }
        if(need_resolve) {
            sky_nvn_graphics_resources_resolve(&delta);
            for(int stage=0;stage<6;stage++) {
                for(int base=0;base<128;base+=8) {
                    if(!(metal_load_u64(&delta.texture_bound[stage][base])|metal_load_u64(&delta.sampler_bound[stage][base])))continue;
                    for(int slot=base;slot<base+8;slot++) {
                        int tex_changed=op->state.texture_bound[stage][slot]&&(!prev->texture_bound[stage][slot]||prev->texture_handles[stage][slot]!=op->state.texture_handles[stage][slot]);
                        int smp_changed=op->state.sampler_bound[stage][slot]&&(!prev->sampler_bound[stage][slot]||prev->sampler_handles[stage][slot]!=op->state.sampler_handles[stage][slot]);
                        uint8_t merged_smp_bound=op->state.sampler_bound[stage][slot]?1:prev->sampler_bound[stage][slot];
                        if(delta.texture_bound[stage][slot]&&tex_changed) {
                            sky_mtl_texture_destroy(prev->textures[stage][slot]);
                            prev->textures[stage][slot]=delta.textures[stage][slot];
                        } else if(delta.textures[stage][slot]) {
                            sky_mtl_texture_destroy(delta.textures[stage][slot]);
                        }
                        if(smp_changed||(tex_changed&&!merged_smp_bound)) {
                            sky_mtl_sampler_destroy(prev->samplers[stage][slot]);
                            prev->samplers[stage][slot]=delta.samplers[stage][slot];
                        } else if(delta.samplers[stage][slot]) {
                            sky_mtl_sampler_destroy(delta.samplers[stage][slot]);
                        }
                    }
                }
                for(int base=0;base<32;base+=8) {
                    if(!metal_load_u64(&delta.image_bound[stage][base]))continue;
                    for(int slot=base;slot<base+8;slot++) {
                        if(delta.image_bound[stage][slot]) {
                            sky_mtl_texture_destroy(prev->images[stage][slot]);
                            prev->images[stage][slot]=delta.images[stage][slot];
                        }
                    }
                }
            }
        }
        sky_mtl_render_state_merge(prev,&op->state);
    }
    if(op->targets_bound) {
        for(int i=0;i<8;i++){
            sky_mtl_texture_t *new=sky_mtl_texture_retain(op->targets.textures[i]);
            sky_mtl_texture_destroy(queue->replay->targets.textures[i]);queue->replay->targets.textures[i]=new;
        }
        sky_mtl_texture_t *new=sky_mtl_texture_retain(op->targets.depth_texture);
        sky_mtl_texture_destroy(queue->replay->targets.depth_texture);queue->replay->targets.depth_texture=new;
        queue->replay->targets.color_count=op->targets.color_count;
    }
    if(out_targets) {
        *out_targets=queue->replay->targets;
        out_targets->has_color_clear=op->targets.has_color_clear;out_targets->clear_index=op->targets.clear_index;
        memcpy(out_targets->colors,op->targets.colors,sizeof out_targets->colors);
        out_targets->clear_depth=op->targets.clear_depth;out_targets->depth_mask=op->targets.depth_mask;
        out_targets->stencil_mask=op->targets.stencil_mask;out_targets->clear_stencil=op->targets.clear_stencil;
    }
}

enum {SKY_MTL_VISIBILITY_SLOTS=4096};
static void metal_visibility_resolve(sky_mtl_command_queue_t *queue) {
    if(!queue->visibility)return;
    id<MTLBuffer> results=(__bridge id<MTLBuffer>)queue->visibility;
    const uint64_t *counts=results.contents;
    for(uint32_t i=0;i<queue->visibility_used;i++)queue->counters[1]+=counts[i];
    memset(results.contents,0,results.length);queue->visibility_used=0;
}
static int metal_visibility_reset(sky_mtl_command_queue_t *queue) {
    id<MTLBuffer> results=[shared_device() newBufferWithLength:SKY_MTL_VISIBILITY_SLOTS*8 options:MTLResourceStorageModeShared];
    if(!results)return 0;
    if(queue->visibility)CFRelease(queue->visibility);
    queue->visibility=CFBridgingRetain(results);queue->visibility_used=0;queue->visibility_enabled=1;queue->counters[1]=0;
    return 1;
}

static int metal_compute_group_size(SkyMetalShader *shader,MTLSize *group) {
    NSArray *size=shader.reflection[@"threadgroup_size"];
    if(size.count!=3)return 0;
    id<MTLDevice> device=shared_device();
    MTLSize maximum=device.maxThreadsPerThreadgroup;
    uint64_t axis[3]={maximum.width,maximum.height,maximum.depth};
    uint64_t count[3],total=1,limit=shader.compute.maxTotalThreadsPerThreadgroup;
    for(unsigned i=0;i<3;i++) {
        count[i]=[size[i] unsignedLongLongValue];
        if(!count[i]||count[i]>axis[i]||count[i]>limit/total) {
            fprintf(stderr,"NVN_METAL_COMPUTE_ERROR reason=unsupported-threadgroup-size axis=%u value=%llu max_axis=%llu max_threads=%llu\n",
                i,(unsigned long long)count[i],(unsigned long long)axis[i],(unsigned long long)limit);
            return 0;
        }
        total*=count[i];
    }
    if(shader.compute.staticThreadgroupMemoryLength>device.maxThreadgroupMemoryLength) {
        fprintf(stderr,"NVN_METAL_COMPUTE_ERROR reason=unsupported-threadgroup-memory\n");return 0;
    }
    *group=MTLSizeMake(count[0],count[1],count[2]);return 1;
}

static int metal_linear_resources(id<MTLCommandBuffer> buffer,const sky_mtl_render_state_t *state,
                                  int first_stage,int last_stage,int flush,SkyMetalSubmitState *submit) {
    if(!sky_mtl_linear_texture_has_mirrors())return 1;
    int ended=0;
    for(int stage=first_stage;stage<=last_stage;stage++)for(int kind=0;kind<2;kind++) {
        if(flush&&!kind)continue;
        int count=kind?32:128;
        for(int i=0;i<count;i++) {
            sky_mtl_texture_t *texture=kind?state->images[stage][i]:state->textures[stage][i];
            void *native=sky_mtl_texture_native_handle(texture);
            if(!sky_mtl_linear_texture_needs_sync(native))continue;
            if(!ended){metal_render_end(submit);ended=1;}
            int valid=flush?sky_mtl_linear_texture_encode_flush((__bridge void *)buffer,native):
                sky_mtl_linear_texture_encode_sync((__bridge void *)buffer,native);
            if(!valid)return 0;
        }
    }
    return 1;
}

static uint64_t g_completed_draws, g_completed_dispatches, g_command_errors,g_sync_actions;
sky_mtl_graphics_stats_t sky_mtl_graphics_stats(void) {
    return (sky_mtl_graphics_stats_t){__atomic_load_n(&g_completed_draws,__ATOMIC_RELAXED),
        __atomic_load_n(&g_completed_dispatches,__ATOMIC_RELAXED),__atomic_load_n(&g_command_errors,__ATOMIC_RELAXED),__atomic_load_n(&g_sync_actions,__ATOMIC_RELAXED)};
}

int sky_mtl_command_queue_submit_operations(sky_mtl_command_queue_t *queue,
    const sky_mtl_operation_t *operations, sky_mtl_completion_fn completion, void *context) {
    if (!queue || !queue->queue || !completion) return 0;
    @autoreleasepool {
        id<MTLCommandQueue> object = (__bridge id<MTLCommandQueue>)queue->queue;
        int profile=metal_profile_enabled();
        uint64_t cpu_start=profile?metal_clock_ns(CLOCK_THREAD_CPUTIME_ID):0;
        uint64_t wall_start=profile?metal_clock_ns(CLOCK_MONOTONIC):0;
        id<MTLCommandBuffer> buffer = [object commandBuffer];
        if (!buffer) return 0;
        SkyMetalSubmitState *submit=[SkyMetalSubmitState new];submit->reuse=metal_reuse_encoders_enabled();
        int valid = 1, resolved_in_submit = 0;
        uint64_t draws=0,dispatches=0;
        pthread_mutex_lock(&g_shader_lock);
        for (const sky_mtl_operation_t *op = operations; op && valid; op = op->next) {
            sky_mtl_clear_pass_t targets;
            if(op->kind==SKY_MTL_OP_STATE||op->kind==SKY_MTL_OP_DRAW||op->kind==SKY_MTL_OP_DISPATCH||op->kind==SKY_MTL_OP_CLEAR||op->kind==SKY_MTL_OP_TRANSFER){
                metal_replay_apply(queue,op,(op->kind==SKY_MTL_OP_DRAW||op->kind==SKY_MTL_OP_CLEAR)?&targets:NULL,&resolved_in_submit);
            }
            if(op->kind==SKY_MTL_OP_STATE)continue;
            const sky_mtl_render_state_t *replayed_state=&queue->replay->state;
            int is_visibility_counter=(op->kind==SKY_MTL_OP_REPORT_COUNTER&&op->counter_type==1&&queue->visibility_enabled&&queue->visibility_used>0);
            if(op->kind!=SKY_MTL_OP_DRAW&&op->kind!=SKY_MTL_OP_REPORT_VALUE&&!(op->kind==SKY_MTL_OP_REPORT_COUNTER&&!is_visibility_counter))
                metal_render_end(submit);
            if(op->kind==SKY_MTL_OP_BARRIER){submit->barriers++;continue;}
            if(op->kind==SKY_MTL_OP_UPDATE_BUFFER){
                NSUInteger offset=0;id<MTLBuffer> destination=metal_buffer(op->destination_address,op->payload_size,&offset);
                id<MTLBuffer> source=[shared_device() newBufferWithBytes:op->payload length:op->payload_size options:MTLResourceStorageModeShared];
                if(!destination||!source){fprintf(stderr,"NVN_METAL_UPDATE_UNIFORM_ERROR reason=unmapped-or-unallocated-buffer\n");valid=0;break;}
                id<MTLBlitCommandEncoder> encoder=[buffer blitCommandEncoder];
                if(encoder)submit->blit_encoders++;
                [encoder copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:offset size:op->payload_size];
                [encoder endEncoding];continue;
            }
            if(op->kind==SKY_MTL_OP_TRANSFER){
                sky_mtl_transfer_t transfer=op->transfer;
                transfer.row_stride=replayed_state->copy_row_stride;
                transfer.image_stride=replayed_state->copy_image_stride;
                if(sky_mtl_transfer_requires_cpu_boundary(&transfer)) {
                    /* CPU decoding must observe UPDATE_BUFFER/compute writes
                     * already encoded in this submit, and later draws must
                     * observe the upload. Correctness takes priority here. */
                    pthread_mutex_unlock(&g_shader_lock);
                    uint64_t wait_start=profile?metal_clock_ns(CLOCK_MONOTONIC):0;
                    [buffer commit];[buffer waitUntilCompleted];
                    if(profile){submit->wait_ns+=metal_clock_ns(CLOCK_MONOTONIC)-wait_start;metal_profile_gpu(queue,buffer);}
                    valid=buffer.status==MTLCommandBufferStatusCompleted&&sky_mtl_transfer_execute_cpu(&transfer);
                    pthread_mutex_lock(&g_shader_lock);
                    if(!valid){fprintf(stderr,"NVN_METAL_TRANSFER_ERROR reason=bc-conversion-or-prefix-failed\n");break;}
                    buffer=[object commandBuffer];if(!buffer){valid=0;break;}
                    continue;
                }
                valid=sky_mtl_transfer_encode((__bridge void *)buffer,&transfer);
                if(valid)submit->blit_encoders++;continue;
            }
            if (op->kind == SKY_MTL_OP_CLEAR) {
                id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:metal_render_targets(&targets)];
                valid = encoder != nil;
                if(encoder)submit->render_encoders++;
                [encoder endEncoding];
            } else if(op->kind==SKY_MTL_OP_REPORT_COUNTER||op->kind==SKY_MTL_OP_REPORT_VALUE) {
                if(is_visibility_counter) {
                    pthread_mutex_unlock(&g_shader_lock);
                    uint64_t wait_start=profile?metal_clock_ns(CLOCK_MONOTONIC):0;
                    [buffer commit];[buffer waitUntilCompleted];
                    if(profile){submit->wait_ns+=metal_clock_ns(CLOCK_MONOTONIC)-wait_start;metal_profile_gpu(queue,buffer);}
                    pthread_mutex_lock(&g_shader_lock);
                    if(buffer.status==MTLCommandBufferStatusError){fprintf(stderr,"NVN_METAL_COUNTER_ERROR reason=gpu-command-error\n");valid=0;break;}
                }
                NSUInteger offset=0;size_t bytes=op->kind==SKY_MTL_OP_REPORT_VALUE?4:16;
                id<MTLBuffer> destination=metal_buffer(op->destination_address,bytes,&offset);
                if(!destination||!destination.contents){fprintf(stderr,"NVN_METAL_COUNTER_ERROR reason=unmapped-destination address=%#llx\n",(unsigned long long)op->destination_address);valid=0;break;}
                if(op->kind==SKY_MTL_OP_REPORT_VALUE)memcpy((uint8_t *)destination.contents+offset,&op->report_value,4);
                else {
                    uint64_t gpu_ns=metal_clock_ns(CLOCK_MONOTONIC);
                    if(!gpu_ns)gpu_ns=1;
                    uint64_t timestamp=(uint64_t)((__uint128_t)gpu_ns*12/625),value=0;
                    /* Timestamp reports use the counter word as a zero completion
                     * marker. Skyrim sets it to one and polls for zero before
                     * reading the timestamp from the second word. */
                    if(op->counter_type==0||op->counter_type==16)value=0;
                    else if(op->counter_type==1&&queue->visibility_enabled){if(is_visibility_counter)metal_visibility_resolve(queue);value=queue->counters[1];}
                    else if(op->counter_type==2||op->counter_type==3)value=queue->counters[op->counter_type];
                    else {fprintf(stderr,"NVN_METAL_COUNTER_ERROR reason=unsupported-counter type=%u\n",op->counter_type);valid=0;break;}
                    uint64_t *result=(uint64_t *)((uint8_t *)destination.contents+offset);
                    __atomic_store_n(result+1,timestamp,__ATOMIC_RELAXED);
                    __atomic_store_n(result,value,__ATOMIC_RELEASE);
                }
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                if(is_visibility_counter) {
                    buffer=[object commandBuffer];if(!buffer)valid=0;
                }
            } else if(op->kind==SKY_MTL_OP_RESET_COUNTER) {
                if(op->counter_type>=32){valid=0;break;}
                queue->counters[op->counter_type]=0;
                if(op->counter_type==1)valid=metal_visibility_reset(queue);
            } else if(op->kind==SKY_MTL_OP_CLEAR_TEXTURE) {
                valid=metal_clear_texture(buffer,op,submit);
            } else if(op->kind==SKY_MTL_OP_SYNC) {
                pthread_mutex_unlock(&g_shader_lock);
                uint64_t wait_start=profile?metal_clock_ns(CLOCK_MONOTONIC):0;
                [buffer commit];[buffer waitUntilCompleted];
                if(profile){submit->wait_ns+=metal_clock_ns(CLOCK_MONOTONIC)-wait_start;metal_profile_gpu(queue,buffer);}
                pthread_mutex_lock(&g_shader_lock);
                if(buffer.status==MTLCommandBufferStatusError){
                    __atomic_add_fetch(&g_command_errors,1,__ATOMIC_RELAXED);
                __atomic_add_fetch(&queue->errors,1,__ATOMIC_RELAXED);
                    fprintf(stderr,"NVN_METAL_SYNC_ERROR error=%s\n",buffer.error.description.UTF8String);valid=0;break;
                }
                pthread_mutex_unlock(&g_shader_lock);
                __atomic_add_fetch(&g_sync_actions,1,__ATOMIC_RELAXED);
                valid=op->sync_action&&op->sync_action(op->sync_context);
                pthread_mutex_lock(&g_shader_lock);
                buffer=[object commandBuffer];if(!buffer)valid=0;
            } else if (op->kind == SKY_MTL_OP_DRAW) {
                if(!metal_linear_resources(buffer,replayed_state,0,1,0,submit)){valid=0;break;}
                if(queue->visibility_enabled&&queue->visibility_used==SKY_MTL_VISIBILITY_SLOTS) {
                    metal_render_end(submit);pthread_mutex_unlock(&g_shader_lock);
                    uint64_t wait_start=profile?metal_clock_ns(CLOCK_MONOTONIC):0;
                    [buffer commit];[buffer waitUntilCompleted];
                    if(profile){submit->wait_ns+=metal_clock_ns(CLOCK_MONOTONIC)-wait_start;metal_profile_gpu(queue,buffer);}
                    pthread_mutex_lock(&g_shader_lock);
                    if(buffer.status==MTLCommandBufferStatusError){valid=0;break;}
                    metal_visibility_resolve(queue);buffer=[object commandBuffer];if(!buffer){valid=0;break;}
                }
                id<MTLBuffer> visibility=queue->visibility_enabled?(__bridge id<MTLBuffer>)queue->visibility:nil;
                valid = metal_encode_draw(buffer,op,replayed_state,&targets,visibility,(NSUInteger)queue->visibility_used*8,submit);
                if(valid)valid=metal_linear_resources(buffer,replayed_state,0,1,1,submit);
                if(valid){
                    if(visibility)queue->visibility_used++;
                    draws++;uint64_t vertices=(uint64_t)op->count*op->instances,primitives=0;
                    if(op->primitive==0)primitives=op->count;
                    else if(op->primitive==1)primitives=op->count/2;
                    else if(op->primitive==3)primitives=op->count?op->count-1:0;
                    else if(op->primitive==4)primitives=op->count/3;
                    else if(op->primitive==5)primitives=op->count>2?op->count-2:0;
                    queue->counters[2]+=vertices;queue->counters[3]+=primitives*op->instances;
                    if(!op->index_address)queue->counters[4]+=vertices;
                }
            }
            else if (op->kind == SKY_MTL_OP_DISPATCH) {
                if(!metal_linear_resources(buffer,replayed_state,5,5,0,submit)){valid=0;break;}
                SkyMetalShader *shader = metal_shader(replayed_state->programs[5], 5);
                if (!shader) { valid = 0; break; }
                if (!shader.compute) {
                    NSError *error = nil;
                    shader.compute = [shared_device() newComputePipelineStateWithFunction:shader.function error:&error];
                    if (!shader.compute) fprintf(stderr, "NVN_METAL_COMPUTE_ERROR error=%s\n", error.description.UTF8String);
                }
                if (!shader.compute) { valid = 0; break; }
                MTLSize group;
                if(!metal_compute_group_size(shader,&group)){valid=0;break;}
                id<MTLComputeCommandEncoder> encoder = [buffer computeCommandEncoder];
                if(encoder)submit->compute_encoders++;
                [encoder setComputePipelineState:shader.compute];
                valid = encode_shader_arguments(encoder, shader, replayed_state, 5,submit);
                if (valid) [encoder dispatchThreadgroups:MTLSizeMake(op->groups[0], op->groups[1], op->groups[2])
                    threadsPerThreadgroup:group];
                [encoder endEncoding];
                if(valid)valid=metal_linear_resources(buffer,replayed_state,5,5,1,submit);
                if(valid)dispatches++;
            } else valid = 0;
        }
        metal_render_end(submit);pthread_mutex_unlock(&g_shader_lock);
        if (!valid) return 0;
        /* Encoding-phase timing includes initial acquisition and prefix
         * commit/waits, but excludes final commit and completion callbacks. */
        if(profile) {
            PROFILE_ADD(queue,submits,1);PROFILE_ADD(queue,cpu_ns,metal_clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu_start);
            PROFILE_ADD(queue,wall_ns,metal_clock_ns(CLOCK_MONOTONIC)-wall_start);PROFILE_ADD(queue,wait_ns,submit->wait_ns);
            PROFILE_ADD(queue,draws,draws);PROFILE_ADD(queue,dispatches,dispatches);
            PROFILE_ADD(queue,render_encoders,submit->render_encoders);PROFILE_ADD(queue,merged_draws,submit->merged_draws);
            PROFILE_ADD(queue,compute_encoders,submit->compute_encoders);PROFILE_ADD(queue,blit_encoders,submit->blit_encoders);
            PROFILE_ADD(queue,argument_buffers,submit->argument_buffers);PROFILE_ADD(queue,depth_hits,submit->depth_hits);
            PROFILE_ADD(queue,depth_misses,submit->depth_misses);
            PROFILE_ADD(queue,barriers,submit->barriers);PROFILE_ADD(queue,writable_draws,submit->writable_draws);
            PROFILE_ADD(queue,feedback_draws,submit->feedback_draws);PROFILE_ADD(queue,pass_changes,submit->pass_changes);
        }
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if(profile) {
                metal_profile_gpu(queue,completed);uint64_t count=PROFILE_ADD(queue,completed_submits,1);
                if(count==1||count%256==0)metal_profile_log(queue,"completed");
            }
            if (completed.status == MTLCommandBufferStatusError) {
                __atomic_add_fetch(&g_command_errors,1,__ATOMIC_RELAXED);
                __atomic_add_fetch(&queue->errors,1,__ATOMIC_RELAXED);
                fprintf(stderr, "NVN_METAL_COMMAND_ERROR error=%s\n", completed.error.description.UTF8String);
            } else {
                uint64_t total=__atomic_add_fetch(&g_completed_draws,draws,__ATOMIC_RELAXED);
                uint64_t compute=__atomic_add_fetch(&g_completed_dispatches,dispatches,__ATOMIC_RELAXED);
                static uint64_t work_logs;
                uint64_t log_idx=(draws||dispatches)?__atomic_add_fetch(&work_logs,1,__ATOMIC_RELAXED):0;
                if((draws||dispatches)&&(log_idx<=64||log_idx%256==0))
                    fprintf(stderr,"NVN_METAL_WORK_COMPLETED draws=%llu dispatches=%llu total_draws=%llu total_dispatches=%llu\n",
                        (unsigned long long)draws,(unsigned long long)dispatches,(unsigned long long)total,(unsigned long long)compute);
            }
            completion(context);
        }];
        [buffer commit];
        return 1;
    }
}

int sky_mtl_command_queue_present_texture(sky_mtl_command_queue_t *queue,
    sky_mtl_texture_t *texture,void *layer_pointer,int present_interval,const int32_t crop[4],sky_mtl_completion_fn completion,void *context) {
    if (!queue || !queue->queue || !texture || !texture->texture || !layer_pointer || !completion) return 0;
    @autoreleasepool {
        CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_pointer;
        layer.device = shared_device();
        layer.framebufferOnly = NO;
#if TARGET_OS_OSX
        layer.displaySyncEnabled=present_interval!=0;
#endif
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) return 0;
        id<MTLTexture> source = (__bridge id<MTLTexture>)texture->texture;
        int64_t left=0,top=0,right=source.width,bottom=source.height;
        if(crop&&crop[2]>0&&crop[3]>0) {
            left=MAX((int64_t)crop[0],0);top=MAX((int64_t)crop[1],0);
            right=MIN((int64_t)crop[0]+crop[2],(int64_t)source.width);
            bottom=MIN((int64_t)crop[1]+crop[3],(int64_t)source.height);
            if(left>=right||top>=bottom){fprintf(stderr,"NVN_METAL_PRESENT_ERROR reason=empty-window-crop\n");return 0;}
        }
        float uv_rect[4]={(float)left/source.width,(float)top/source.height,(float)(right-left)/source.width,(float)(bottom-top)/source.height};
        float uv_bounds[4]={(left+.5f)/source.width,(top+.5f)/source.height,(right-.5f)/source.width,(bottom-.5f)/source.height};
        id<MTLCommandQueue> object = (__bridge id<MTLCommandQueue>)queue->queue;
        id<MTLCommandBuffer> buffer = [object commandBuffer];
        if(!buffer||!sky_mtl_linear_texture_encode_sync((__bridge void *)buffer,(__bridge void *)source))return 0;
        static id<MTLLibrary> library;
        static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipelines;
        if (!library) {
            NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; float2 uv; }; vertex V vs(uint id [[vertex_id]], constant float4& rect [[buffer(0)]]) { float2 p=float2((id<<1)&2,id&2); return {float4(p*2-1,0,1),rect.xy+float2(p.x,1-p.y)*rect.zw}; } fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant float4& bounds [[buffer(0)]]) { constexpr sampler s(filter::linear); return t.sample(s,clamp(in.uv,bounds.xy,bounds.zw)); }";
            library = [shared_device() newLibraryWithSource:msl options:nil error:nil];
            pipelines = [NSMutableDictionary new];
        }
        id<MTLRenderPipelineState> pipeline = pipelines[@(drawable.texture.pixelFormat)];
        if (!pipeline) {
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [library newFunctionWithName:@"vs"];
            d.fragmentFunction = [library newFunctionWithName:@"fs"];
            d.colorAttachments[0].pixelFormat = drawable.texture.pixelFormat;
            pipeline = [shared_device() newRenderPipelineStateWithDescriptor:d error:nil];
            if (pipeline) pipelines[@(drawable.texture.pixelFormat)] = pipeline;
        }
        if (!pipeline) return 0;
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = drawable.texture;
        d.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:d];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBytes:uv_rect length:sizeof uv_rect atIndex:0];
        [encoder setFragmentBytes:uv_bounds length:sizeof uv_bounds atIndex:0];
        [encoder setFragmentTexture:source atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        /* NVN intervals are Switch display vblanks at 60 Hz. Keep interval 2
         * at 30 Hz even on ProMotion hosts, using the previous drawable's real
         * presentation time instead of altering the guest clock. */
#if TARGET_OS_SIMULATOR
        /* Simulator exposes only immediate/absolute-time presentation. The
         * NVN window's monotonic pacer enforces its interval before enqueue. */
        (void)present_interval;
        [buffer presentDrawable:drawable];
#else
        if(present_interval>0)[buffer presentDrawable:drawable afterMinimumDuration:present_interval/60.0];
        else [buffer presentDrawable:drawable];
        [drawable addPresentedHandler:^(id<MTLDrawable> presented){
            static uint64_t last_time,count;
            uint64_t now=(uint64_t)(presented.presentedTime*1.e9);
            if(now){
                uint64_t previous=__atomic_exchange_n(&last_time,now,__ATOMIC_RELAXED);
                uint64_t n=__atomic_add_fetch(&count,1,__ATOMIC_RELAXED);
                if(n<=4||n%120==0)fprintf(stderr,"NVN_DRAWABLE_PRESENTED count=%llu interval=%d time_ns=%llu delta_ns=%llu\n",
                    (unsigned long long)n,present_interval,(unsigned long long)now,(unsigned long long)(previous?now-previous:0));
            }
        }];
#endif
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if(metal_profile_enabled())metal_profile_gpu(queue,completed);
            if (completed.status == MTLCommandBufferStatusError) {
                __atomic_add_fetch(&g_command_errors,1,__ATOMIC_RELAXED);
                __atomic_add_fetch(&queue->errors,1,__ATOMIC_RELAXED);
                fprintf(stderr, "NVN_METAL_PRESENT_ERROR error=%s\n", completed.error.description.UTF8String);
            }
            completion(context);
        }];
        [buffer commit];
        return 1;
    }
}

void *sky_mtl_texture_native_handle(const sky_mtl_texture_t *texture) {
    return texture?(void *)texture->texture:NULL;
}
void *sky_mtl_buffer_native_handle(uint64_t address,uint64_t size,uint64_t *offset) {
    if(!offset)return NULL;NSUInteger native_offset=0;
    id<MTLBuffer> buffer=metal_buffer(address,size,&native_offset);
    *offset=native_offset;return (__bridge void *)buffer;
}

int sky_mtl_command_queue_get_error(const sky_mtl_command_queue_t *queue) {return !queue||__atomic_load_n(&queue->errors,__ATOMIC_RELAXED)!=0;}

sky_mtl_texture_t *sky_mtl_texture_wrap_native(void *native_texture) {
    if(!native_texture)return NULL;
    @autoreleasepool {return texture_wrap((__bridge id<MTLTexture>)native_texture);}
}
