#import "nvn_metal.h"
#import <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *state) {
    (void)state;assert(!"unexpected recorder access in memory mapping test");
}

static void finish(id<MTLCommandBuffer> command) {
    [command commit];[command waitUntilCompleted];
    assert(command.status==MTLCommandBufferStatusCompleted&&!command.error);
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();assert(device);
        id<MTLCommandQueue> queue=[device newCommandQueue];assert(queue);
        size_t page=(size_t)getpagesize();
        uint8_t *region=mmap(NULL,2*page,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
        assert(region!=MAP_FAILED&&!mprotect(region,2*page,PROT_READ|PROT_WRITE));
        /* Match the guest arena: wrap a subrange of committed anonymous VM,
         * including a nonzero offset within the Metal buffer's aligned page. */
        uint8_t *memory=region+page+64;size_t size=page-128;
        sky_mtl_memory_pool_t *pool=sky_mtl_memory_pool_create(memory,size);assert(pool);
        assert(sky_mtl_memory_pool_is_shared(pool)&&sky_mtl_memory_pool_contents(pool)==memory);
        uint64_t address=sky_mtl_memory_pool_gpu_address(pool),offset=0;
        id<MTLBuffer> buffer=(__bridge id<MTLBuffer>)sky_mtl_buffer_native_handle(address,size,&offset);
        assert(buffer&&buffer.contents==region+page&&offset==64&&buffer.gpuAddress+offset==address);
        id<MTLBuffer> readback=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];assert(readback);
        for(unsigned frame=0;frame<2;frame++) {
            uint8_t value=frame?0x6c:0x11;memset(memory,value,64);
            id<MTLCommandBuffer> command=[queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
            [encoder copyFromBuffer:buffer sourceOffset:offset toBuffer:readback destinationOffset:0 size:64];
            [encoder endEncoding];finish(command);
            for(unsigned i=0;i<64;i++)assert(((uint8_t *)readback.contents)[i]==value);
        }
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
        [encoder fillBuffer:buffer range:NSMakeRange(offset,64) value:0x7d];[encoder endEncoding];finish(command);
        for(unsigned i=0;i<64;i++)assert(memory[i]==0x7d);
        sky_mtl_memory_pool_destroy(pool);
        assert(!sky_mtl_buffer_native_handle(address,64,&offset)&&!offset);
        buffer=nil;assert(!sky_mtl_memory_pool_active_count());assert(!munmap(region,2*page));
        printf("T_NVN_MEMORY_MAPPING_OK guest-mmap-subrange-two-CPU-frames-GPU-writeback-and-retirement unified=%d\n",device.hasUnifiedMemory);
    }
    return 0;
}
