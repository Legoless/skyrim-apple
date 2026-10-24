#import "nvn_metal.h"
#import <Metal/Metal.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *state) {
    (void)state;assert(!"unexpected NVN recorder access in buffer lookup test");
}

typedef struct {
    sky_mtl_memory_pool_t *pool;
    uint8_t *memory;
    uint64_t address,size,wrapped;
    uintptr_t page_base;
    char label[32];
} pool_case_t;

static void create_case(pool_case_t *c,uint8_t *memory,uint64_t size,unsigned number) {
    memset(c,0,sizeof *c);c->pool=sky_mtl_memory_pool_create(memory,size);assert(c->pool);
    c->memory=memory;c->size=size;c->address=sky_mtl_memory_pool_gpu_address(c->pool);
    c->page_base=(uintptr_t)memory&~((uintptr_t)getpagesize()-1);
    c->wrapped=sky_mtl_memory_pool_wrapped_size(c->pool);
    snprintf(c->label,sizeof c->label,"lookup-%u",number);sky_mtl_memory_pool_set_debug_label(c->pool,c->label);
}
/* Reference contract captured from the original lookup: first/newest pool
 * containing the entire span; a zero-byte query still excludes one-past-end. */
static const pool_case_t *reference(const pool_case_t *p,unsigned count,uint64_t address,uint64_t size) {
    for(unsigned i=count;i-->0;) {
        if(!p[i].pool||address<p[i].address||address-p[i].address>=p[i].size||
            size>p[i].size-(address-p[i].address))continue;
        return p+i;
    }
    return NULL;
}
static id<MTLBuffer> check(const pool_case_t *p,unsigned count,uint64_t address,uint64_t size) {
    const pool_case_t *expected=reference(p,count,address,size);
    uint64_t offset=UINT64_MAX;
    id<MTLBuffer> actual=(__bridge id<MTLBuffer>)sky_mtl_buffer_native_handle(address,size,&offset);
    if(!expected){assert(!actual&&offset==0);return nil;}
    assert(actual&&[actual.label isEqualToString:[NSString stringWithUTF8String:expected->label]]);
    assert(actual.contents==(void *)expected->page_base&&actual.length==expected->wrapped);
    assert(offset==address-expected->address+(uintptr_t)expected->memory-expected->page_base);
    assert(actual.gpuAddress+offset==address);
    return actual;
}
static void gpu_copy(id<MTLCommandQueue> queue,id<MTLBuffer> source,uint64_t offset,const void *expected,size_t size) {
    assert(!(offset&3)&&!(size&3));
    id<MTLBuffer> out=[queue.device newBufferWithLength:size options:MTLResourceStorageModeShared];assert(out);
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
    [encoder copyFromBuffer:source sourceOffset:offset toBuffer:out destinationOffset:0 size:size];[encoder endEncoding];
    [command commit];[command waitUntilCompleted];
    assert(command.status==MTLCommandBufferStatusCompleted&&!command.error&&!memcmp(out.contents,expected,size));
}
static void gpu_query(id<MTLCommandQueue> queue,const pool_case_t *p,unsigned count,uint64_t address) {
    const pool_case_t *expected=reference(p,count,address,64);assert(expected);
    id<MTLBuffer> buffer=check(p,count,address,64);uint64_t offset=0;
    assert(sky_mtl_buffer_native_handle(address,64,&offset)==(__bridge void *)buffer);
    gpu_copy(queue,buffer,offset,expected->memory+address-expected->address,64);
}
static void boundaries(const pool_case_t *p,unsigned count) {
    for(unsigned i=0;i<count;i++)if(p[i].pool) {
        uint64_t a=p[i].address,n=p[i].size;
        const uint64_t addresses[]={a-1,a,a+1,a+n-1,a+n,a+n+1};
        const uint64_t sizes[]={0,1,2,n-1,n,n+1,UINT64_MAX};
        for(unsigned x=0;x<sizeof addresses/sizeof *addresses;x++)
            for(unsigned y=0;y<sizeof sizes/sizeof *sizes;y++)
                for(unsigned hit=0;hit<2;hit++)check(p,count,addresses[x],sizes[y]);
    }
    assert(!sky_mtl_buffer_native_handle(p[0].address,1,NULL));
}
typedef struct {const pool_case_t *base;int started,stop;uint64_t reads;} reader_t;
static void *lookup_reader(void *context) {
    reader_t *r=context;
    @autoreleasepool {
        __atomic_store_n(&r->started,1,__ATOMIC_RELEASE);
        do {
            check(r->base,1,r->base[0].address+5,64);
            r->reads++;
        } while(!__atomic_load_n(&r->stop,__ATOMIC_ACQUIRE));
    }
    return NULL;
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();assert(device);
        id<MTLCommandQueue> queue=[device newCommandQueue];assert(queue);
        size_t page=(size_t)getpagesize();uint8_t *memory=NULL;
        assert(posix_memalign((void **)&memory,page,8*page)==0);
        for(size_t i=0;i<8*page;i++)memory[i]=(uint8_t)(i*29+7);
        pool_case_t cases[4];memset(cases,0,sizeof cases);
        create_case(cases,memory+123,2*page+17,0);
        /* The host ranges overlap deliberately. The reference also handles
         * overlapping GPU ranges if Metal aliases those virtual addresses. */
        create_case(cases+1,memory+page+64,page+32,1);
        create_case(cases+2,memory+320,page/2,2);
        boundaries(cases,3);
        gpu_query(queue,cases,3,cases[0].address+5);
        gpu_query(queue,cases,3,cases[1].address);
        gpu_query(queue,cases,3,cases[2].address);

        /* More distinct keys than cache slots exercise collisions/eviction;
         * a hit for one size must never validate a different requested span. */
        uint64_t random=1;
        for(unsigned i=0;i<4096;i++) {
            random=random*UINT64_C(6364136223846793005)+1;
            unsigned pool=(unsigned)(random>>32)%3;
            uint64_t address=cases[pool].address+(random%(cases[pool].size+2));
            uint64_t size=(random>>17)%(cases[pool].size+16);
            check(cases,3,address,size);check(cases,3,address,size);
        }

        /* Keep an actual native buffer alive across registry removal. Already
         * encoded work/native owners remain valid, but future lookup must use
         * only the remaining registry (never a freed cached pool pointer). */
        uint64_t old=cases[2].address,offset=0;
        id<MTLBuffer> retained=(__bridge id<MTLBuffer>)sky_mtl_buffer_native_handle(old,64,&offset);assert(retained);
        sky_mtl_memory_pool_destroy(cases[2].pool);cases[2].pool=NULL;
        check(cases,3,old,64);boundaries(cases,3);
        gpu_copy(queue,retained,offset,memory+320,64);retained=nil;
        create_case(cases+3,memory+320,page/2,3);
        check(cases,4,old,64);boundaries(cases,4);gpu_query(queue,cases,4,cases[3].address);

        reader_t reader={.base=cases};pthread_t thread;
        assert(pthread_create(&thread,NULL,lookup_reader,&reader)==0);
        while(!__atomic_load_n(&reader.started,__ATOMIC_ACQUIRE))sched_yield();
        for(unsigned i=0;i<64;i++) {
            sky_mtl_memory_pool_t *temporary=sky_mtl_memory_pool_create(memory+5*page+7,page-16);assert(temporary);
            sky_mtl_memory_pool_destroy(temporary);
        }
        __atomic_store_n(&reader.stop,1,__ATOMIC_RELEASE);assert(pthread_join(thread,NULL)==0&&reader.reads);
        gpu_query(queue,cases,4,cases[0].address+5);
        for(unsigned i=4;i-->0;)if(cases[i].pool)sky_mtl_memory_pool_destroy(cases[i].pool);
        assert(!sky_mtl_memory_pool_active_count());
        uint64_t empty=UINT64_MAX;assert(!sky_mtl_buffer_native_handle(old,64,&empty)&&empty==0);
        free(memory);
        puts("T_NVN_BUFFER_LOOKUP_OK original-reference-boundaries-overlapping-host-ranges-collisions-destroy-recreate-concurrent-mutations-and-GPU-readback");
    }
    return 0;
}
