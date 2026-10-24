#include "nvn_metal.h"
#include <assert.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH=4, HEIGHT=4, WARM_DRAWS=256, BATCH_DRAWS=2048, TRIALS=5 };
static const uint64_t program_serial=UINT64_C(0x200000001);

static uint64_t now_ns(void) {
    struct timespec ts;assert(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
}
static uint64_t thread_ns(void) {
    struct timespec ts;assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)==0);
    return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
}
typedef struct {pthread_mutex_t lock;pthread_cond_t condition;int done;uint64_t completed_ns;} completion_t;
static void completed(void *context) {
    completion_t *c=context;
    pthread_mutex_lock(&c->lock);c->completed_ns=now_ns();c->done=1;
    pthread_cond_signal(&c->condition);pthread_mutex_unlock(&c->lock);
}
typedef struct {double encode_ms,completion_wait_ms,completion_total_ms,cpu_thread_ms;} measurement_t;
static measurement_t submit(sky_mtl_command_queue_t *queue,const sky_mtl_operation_t *ops,unsigned draws) {
    completion_t c={.lock=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    sky_mtl_graphics_stats_t before=sky_mtl_graphics_stats();
    uint64_t cpu_start=thread_ns(),start=now_ns();
    assert(sky_mtl_command_queue_submit_operations(queue,ops,completed,&c));
    uint64_t submitted=now_ns(),cpu_end=thread_ns();
    pthread_mutex_lock(&c.lock);while(!c.done)pthread_cond_wait(&c.condition,&c.lock);
    uint64_t end=c.completed_ns;pthread_mutex_unlock(&c.lock);
    pthread_cond_destroy(&c.condition);pthread_mutex_destroy(&c.lock);
    sky_mtl_graphics_stats_t after=sky_mtl_graphics_stats();
    assert(after.draws-before.draws==draws&&after.dispatches==before.dispatches&&!after.command_errors);
    assert(!sky_mtl_command_queue_get_error(queue));
    return (measurement_t){(submitted-start)/1.e6,(end>submitted?end-submitted:0)/1.e6,(end-start)/1.e6,(cpu_end-cpu_start)/1.e6};
}
static void install_fixture(int stage,const char *key) {
    char executable[PATH_MAX];uint32_t length=sizeof executable;
    assert(_NSGetExecutablePath(executable,&length)==0);
    char *slash=strrchr(executable,'/');assert(slash);*slash=0;
    char source[PATH_MAX],reflection[PATH_MAX];
    assert(snprintf(source,sizeof source,"%s/../tests/fixtures/shaders/%s.metal",executable,key)<(int)sizeof source);
    assert(snprintf(reflection,sizeof reflection,"%s/../tests/fixtures/shaders/%s.json",executable,key)<(int)sizeof reflection);
    assert(sky_mtl_program_set_shader(program_serial,stage,source,reflection));
}
static sky_mtl_operation_t *make_batch(unsigned draws,sky_mtl_texture_t *color,sky_mtl_texture_t *depth,uint64_t uniform) {
    sky_mtl_operation_t *ops=calloc(draws+1,sizeof *ops);assert(ops);
    ops[0].kind=SKY_MTL_OP_CLEAR;ops[0].targets_bound=1;
    ops[0].targets.color_count=1;ops[0].targets.textures[0]=color;ops[0].targets.depth_texture=depth;
    ops[0].targets.has_color_clear=1;ops[0].targets.clear_index=0;
    ops[0].targets.depth_mask=1;ops[0].targets.clear_depth=1;
    sky_mtl_operation_t prototype={0};prototype.kind=SKY_MTL_OP_DRAW;prototype.targets_bound=1;
    prototype.targets.color_count=1;prototype.targets.textures[0]=color;prototype.targets.depth_texture=depth;
    prototype.primitive=5;prototype.count=4;prototype.instances=1;
    sky_mtl_render_state_t *s=&prototype.state;
    s->programs[0]=s->programs[1]=program_serial;s->program_mask=3;
    s->uniform[1][0]=(sky_mtl_buffer_binding_t){uniform,65536};s->uniform_bound[1][0]=1;
    s->changed=SKY_MTL_STATE_CHANNEL|SKY_MTL_STATE_COLOR|SKY_MTL_STATE_DEPTH|SKY_MTL_STATE_POLYGON|
        SKY_MTL_STATE_VIEWPORT|SKY_MTL_STATE_DEPTH_RANGE|SKY_MTL_STATE_ATTRIBUTES|SKY_MTL_STATE_STREAMS;
    s->channel_mask=15;s->depth_test=s->depth_write=1;s->depth_func=8;
    s->front_face=1;s->polygon_mode=2;s->has_viewport=1;
    s->viewport[2]=WIDTH;s->viewport[3]=HEIGHT;s->viewport[5]=1;
    s->blend_mask=1;s->blend[0].rgb_op=s->blend[0].alpha_op=1;
    s->blend[0].src_rgb=s->blend[0].src_alpha=2;s->blend[0].dst_rgb=s->blend[0].dst_alpha=1;
    for(unsigned i=1;i<=draws;i++)ops[i]=prototype;
    for(unsigned i=0;i<draws;i++)ops[i].next=ops+i+1;
    return ops;
}
static void verify(sky_mtl_texture_t *color,sky_mtl_texture_t *depth,const uint8_t rgba[4]) {
    uint8_t pixels[WIDTH*HEIGHT*4];uint16_t z[WIDTH*HEIGHT];size_t size=0;
    assert(sky_mtl_texture_readback(color,0,0,SKY_MTL_READ_COLOR,pixels,sizeof pixels,&size)&&size==sizeof pixels);
    assert(sky_mtl_texture_readback(depth,0,0,SKY_MTL_READ_DEPTH,z,sizeof z,&size)&&size==sizeof z);
    for(unsigned i=0;i<WIDTH*HEIGHT;i++)assert(!memcmp(pixels+4*i,rgba,4)&&z[i]==0);
}
static int compare_double(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);
}
int main(int argc,char **argv) {
    unsigned noise_count=0;
    assert(argc<=2);
    if(argc==2){char *end;unsigned long count=strtoul(argv[1],&end,10);assert(*argv[1]&&!*end&&count<=2048);noise_count=(unsigned)count;}
    install_fixture(0,"vertex_id_quad");
    install_fixture(1,"uniform_color");
    void *uniform=NULL;assert(posix_memalign(&uniform,(size_t)getpagesize(),65536)==0);memset(uniform,0,65536);
    sky_mtl_memory_pool_t *pool=sky_mtl_memory_pool_create(uniform,65536);assert(pool);
    /* Put the real uniform pool behind a bounded registry like the world
     * profile. Dummy pools are never GPU inputs and are created outside timing. */
    void *noise_memory=NULL;sky_mtl_memory_pool_t **noise=NULL;
    if(noise_count) {
        size_t page=(size_t)getpagesize();assert(posix_memalign(&noise_memory,page,page*noise_count)==0);
        noise=calloc(noise_count,sizeof *noise);assert(noise);
        for(unsigned i=0;i<noise_count;i++){noise[i]=sky_mtl_memory_pool_create((uint8_t *)noise_memory+page*i,page);assert(noise[i]);}
    }
    sky_mtl_texture_properties_t properties={.pixel_format=70,.texture_type=SKY_MTL_TEXTURE_TYPE_2D,
        .storage_mode=SKY_MTL_STORAGE_PRIVATE,.width=WIDTH,.height=HEIGHT,.depth=1,.array_length=1,
        .mipmap_level_count=1,.sample_count=1,.usage=SKY_MTL_TEXTURE_USAGE_RENDER_TARGET|SKY_MTL_TEXTURE_USAGE_SHADER_READ};
    sky_mtl_texture_t *color=sky_mtl_texture_create(&properties);assert(color);
    properties.pixel_format=250;sky_mtl_texture_t *depth=sky_mtl_texture_create(&properties);assert(depth);
    sky_mtl_command_queue_t *queue=sky_mtl_command_queue_create();assert(queue);
    sky_mtl_operation_t *warm=make_batch(WARM_DRAWS,color,depth,sky_mtl_memory_pool_gpu_address(pool));
    sky_mtl_operation_t *ops=make_batch(BATCH_DRAWS,color,depth,sky_mtl_memory_pool_gpu_address(pool));
    const float colors[2][4]={{.2f,.4f,.6f,1},{.8f,.6f,.4f,1}};
    const uint8_t bytes[2][4]={{51,102,153,255},{204,153,102,255}};
    memcpy(uniform,colors[0],sizeof colors[0]);submit(queue,warm,WARM_DRAWS);verify(color,depth,bytes[0]);
    double encode[TRIALS],wait[TRIALS],total[TRIALS],cpu[TRIALS];
    printf("NVN_PIPELINE_BENCH warm_draws=%u draws_per_trial=%u trials=%u operation_bytes=%zu pool_count=%u\n",WARM_DRAWS,BATCH_DRAWS,TRIALS,sizeof *ops,noise_count+1);
    for(unsigned i=0;i<TRIALS;i++) {
        memcpy(uniform,colors[i&1],sizeof colors[0]);
        measurement_t result=submit(queue,ops,BATCH_DRAWS);verify(color,depth,bytes[i&1]);
        encode[i]=result.encode_ms;wait[i]=result.completion_wait_ms;total[i]=result.completion_total_ms;
        cpu[i]=result.cpu_thread_ms;
        printf("NVN_PIPELINE_BENCH_TRIAL trial=%u encode_wall_ms=%.6f cpu_thread_ms=%.6f completion_wait_ms=%.6f completion_total_ms=%.6f cpu_ns_per_draw=%.3f\n",
            i+1,encode[i],cpu[i],wait[i],total[i],cpu[i]*1.e6/BATCH_DRAWS);fflush(stdout);
    }
    qsort(encode,TRIALS,sizeof *encode,compare_double);qsort(wait,TRIALS,sizeof *wait,compare_double);qsort(total,TRIALS,sizeof *total,compare_double);
    qsort(cpu,TRIALS,sizeof *cpu,compare_double);
    printf("NVN_PIPELINE_BENCH_RESULT median_encode_wall_ms=%.6f median_cpu_thread_ms=%.6f median_completion_wait_ms=%.6f median_completion_total_ms=%.6f median_cpu_ns_per_draw=%.3f completed_draws=%llu pixels_depth=verified\n",
        encode[TRIALS/2],cpu[TRIALS/2],wait[TRIALS/2],total[TRIALS/2],cpu[TRIALS/2]*1.e6/BATCH_DRAWS,(unsigned long long)sky_mtl_graphics_stats().draws);
    /* Exercise different PSO keys, then return to the already cached original.
     * Uniform contents remain dynamic data rather than part of the PSO key. */
    sky_mtl_operation_t *variant=make_batch(1,color,depth,sky_mtl_memory_pool_gpu_address(pool));
    memcpy(uniform,colors[0],sizeof colors[0]);variant[1].state.channel_mask=1;
    submit(queue,variant,1);verify(color,depth,(uint8_t[]){51,0,0,0});
    variant[1].state.channel_mask=15;variant[1].state.blend_enable=1;
    variant[1].state.blend[0].src_rgb=5;variant[1].state.blend[0].dst_rgb=6;
    memcpy(uniform,(float[]){1,0,0,.5f},16);
    submit(queue,variant,1);verify(color,depth,(uint8_t[]){128,0,0,128});
    variant[1].state.blend_enable=0;variant[1].state.blend[0].src_rgb=2;variant[1].state.blend[0].dst_rgb=1;
    memcpy(uniform,colors[1],sizeof colors[1]);submit(queue,variant,1);verify(color,depth,bytes[1]);
    free(variant);
    puts("NVN_PIPELINE_BENCH_CACHE_EQUIVALENCE_OK dynamic-uniform-channel-mask-blend-and-original-key-reuse");
    sky_mtl_command_queue_destroy(queue);free(warm);free(ops);
    sky_mtl_texture_destroy(color);sky_mtl_texture_destroy(depth);sky_mtl_memory_pool_destroy(pool);free(uniform);
    for(unsigned i=0;i<noise_count;i++)sky_mtl_memory_pool_destroy(noise[i]);
    free(noise);free(noise_memory);
    sky_mtl_program_remove(program_serial);
    assert(!sky_mtl_texture_active_count()&&!sky_mtl_memory_pool_active_count()&&!sky_mtl_command_queue_active_count());
    return 0;
}
