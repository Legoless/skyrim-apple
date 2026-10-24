#import "nvn_metal.h"
#import <Foundation/Foundation.h>
#include <assert.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* These fixtures bind only a uniform buffer, resolved by the real Metal pool
 * registry. No NVN texture/sampler descriptor table is involved. */
void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *state) {
    assert(!state->texture_pool&&!state->sampler_pool);
}
typedef struct {pthread_mutex_t lock;pthread_cond_t condition;int done;} completion_t;
static void completed(void *context) {
    completion_t *c=context;pthread_mutex_lock(&c->lock);c->done=1;
    pthread_cond_signal(&c->condition);pthread_mutex_unlock(&c->lock);
}
static void submit(sky_mtl_command_queue_t *queue,sky_mtl_operation_t *operations) {
    completion_t c={.lock=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    assert(sky_mtl_command_queue_submit_operations(queue,operations,completed,&c));
    pthread_mutex_lock(&c.lock);while(!c.done)pthread_cond_wait(&c.condition,&c.lock);pthread_mutex_unlock(&c.lock);
    pthread_cond_destroy(&c.condition);pthread_mutex_destroy(&c.lock);assert(!sky_mtl_command_queue_get_error(queue));
}
static void fixture(uint64_t serial,int stage,const char *name) {
    char executable[PATH_MAX];uint32_t length=sizeof executable;assert(!_NSGetExecutablePath(executable,&length));
    char *slash=strrchr(executable,'/');assert(slash);*slash=0;char source[PATH_MAX],reflection[PATH_MAX];
    assert(snprintf(source,sizeof source,"%s/../tests/fixtures/shaders/%s.metal",executable,name)<(int)sizeof source);
    assert(snprintf(reflection,sizeof reflection,"%s/../tests/fixtures/shaders/%s.json",executable,name)<(int)sizeof reflection);
    assert(sky_mtl_program_set_shader(serial,stage,source,reflection));
}
static sky_mtl_texture_t *texture_shape(unsigned format,unsigned width,unsigned height) {
    sky_mtl_texture_properties_t p={.pixel_format=format,.texture_type=SKY_MTL_TEXTURE_TYPE_2D,
        .storage_mode=SKY_MTL_STORAGE_PRIVATE,.width=width,.height=height,.depth=1,.array_length=1,
        .mipmap_level_count=1,.sample_count=1,.usage=SKY_MTL_TEXTURE_USAGE_RENDER_TARGET|SKY_MTL_TEXTURE_USAGE_SHADER_READ};
    sky_mtl_texture_t *t=sky_mtl_texture_create(&p);assert(t);return t;
}
static sky_mtl_texture_t *texture(unsigned format) {return texture_shape(format,4,4);}
static unsigned checks;
static uint8_t render_case(sky_mtl_command_queue_t *queue,sky_mtl_texture_t *color,sky_mtl_texture_t *depth,
    const sky_mtl_render_state_t *state,int attached,uint8_t seed,int visible,float expected_depth,int expected_stencil) {
    sky_mtl_operation_t *ops=calloc(2,sizeof *ops);assert(ops);
    ops[0].kind=SKY_MTL_OP_CLEAR;ops[0].targets_bound=1;ops[0].next=ops+1;
    ops[0].targets.color_count=1;ops[0].targets.textures[0]=color;ops[0].targets.depth_texture=depth;
    ops[0].targets.has_color_clear=1;ops[0].targets.colors[0][3]=1;
    ops[0].targets.depth_mask=1;ops[0].targets.clear_depth=.5f;
    ops[0].targets.stencil_mask=255;ops[0].targets.clear_stencil=seed;
    ops[1].kind=SKY_MTL_OP_DRAW;ops[1].targets_bound=1;ops[1].targets.color_count=1;
    ops[1].targets.textures[0]=color;ops[1].targets.depth_texture=attached?depth:NULL;
    ops[1].state=*state;ops[1].primitive=5;ops[1].count=4;ops[1].instances=1;
    submit(queue,ops);free(ops);
    uint8_t rgba[16*4],stencil[16];float z[16];
    assert(sky_mtl_texture_readback(color,0,0,SKY_MTL_READ_COLOR,rgba,sizeof rgba,NULL));
    assert(sky_mtl_texture_readback(depth,0,0,SKY_MTL_READ_DEPTH,z,sizeof z,NULL));
    assert(sky_mtl_texture_readback(depth,0,0,SKY_MTL_READ_STENCIL,stencil,sizeof stencil,NULL));
    for(unsigned i=0;i<16;i++) {
        assert(rgba[i*4]==(visible?255:0)&&rgba[i*4+1]==rgba[i*4]&&rgba[i*4+2]==rgba[i*4]&&rgba[i*4+3]==255);
        assert(z[i]==expected_depth&&stencil[i]==stencil[0]);
        if(expected_stencil>=0)assert(stencil[i]==expected_stencil);
    }
    checks++;return stencil[0];
}
static uint8_t stencil_result(unsigned operation,uint8_t seed,uint8_t reference) {
    switch(operation) {
        case 2:return 0;case 3:return reference;case 4:return seed==255?255:seed+1;
        case 5:return seed?seed-1:0;case 6:return seed^255;case 7:return seed+1;case 8:return seed-1;
        default:return seed;
    }
}
static void sequence_color(sky_mtl_texture_t *texture,const uint8_t left[4],const uint8_t right[4]) {
    uint8_t rgba[64];assert(sky_mtl_texture_readback(texture,0,0,SKY_MTL_READ_COLOR,rgba,sizeof rgba,NULL));
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)assert(!memcmp(rgba+(y*4+x)*4,x<2?left:right,4));
}
static void sequence_clear(sky_mtl_operation_t *op,sky_mtl_texture_t *color,sky_mtl_texture_t *depth) {
    op->kind=SKY_MTL_OP_CLEAR;op->targets_bound=1;op->targets.color_count=1;op->targets.textures[0]=color;
    op->targets.depth_texture=depth;op->targets.has_color_clear=1;op->targets.colors[0][3]=1;
    op->targets.depth_mask=!!depth;op->targets.clear_depth=.5;op->targets.stencil_mask=depth?255:0;
}
static void sequence_draw(sky_mtl_operation_t *op,const sky_mtl_render_state_t *base,
    sky_mtl_texture_t *color,sky_mtl_texture_t *depth,uint64_t uniform) {
    op->kind=SKY_MTL_OP_DRAW;op->targets_bound=1;op->targets.color_count=1;op->targets.textures[0]=color;
    op->targets.depth_texture=depth;op->state=*base;op->state.uniform[1][0]=(sky_mtl_buffer_binding_t){uniform,16};
    op->primitive=5;op->count=4;op->instances=1;
}
static void sequence_submit(sky_mtl_command_queue_t *queue,sky_mtl_operation_t *ops,unsigned count) {
    for(unsigned i=0;i<count;i++)ops[i].next=i+1<count?ops+i+1:NULL;submit(queue,ops);
}
static int sequence_sync(void *context) {(*(unsigned *)context)++;return 1;}
static void test_attachment_render_area(sky_mtl_command_queue_t *queue,sky_mtl_texture_t *depth,
    const sky_mtl_render_state_t *original) {
    sky_mtl_texture_t *large=texture_shape(70,8,8);sky_mtl_operation_t *ops=calloc(2,sizeof *ops);assert(ops);
    sky_mtl_render_state_t state=*original;state.depth_test=state.depth_write=state.stencil_test=0;
    sequence_clear(ops,large,depth);sequence_draw(ops+1,&state,large,depth,state.uniform[1][0].address);
    ops[1].count=3;ops[1].state.has_viewport=1;ops[1].state.has_scissor=0;
    sequence_submit(queue,ops,2);uint8_t reference[256],actual[256];
    assert(sky_mtl_texture_readback(large,0,0,SKY_MTL_READ_COLOR,reference,sizeof reference,NULL));
    unsigned white=0;for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)white+=reference[(y*8+x)*4]==255;
    assert(white>0&&white<16);
    for(unsigned explicit_scissor=0;explicit_scissor<2;explicit_scissor++) {
        ops[1].state.has_viewport=0;ops[1].state.has_scissor=explicit_scissor;
        memcpy(ops[1].state.scissor,(int[]){0,0,100,100},16);sequence_submit(queue,ops,2);
        assert(sky_mtl_texture_readback(large,0,0,SKY_MTL_READ_COLOR,actual,sizeof actual,NULL));
        for(unsigned y=0;y<4;y++)assert(!memcmp(actual+y*8*4,reference+y*8*4,4*4));
    }
    free(ops);sky_mtl_texture_destroy(large);
    puts("NVN_RENDER_AREA_GPU_OK default-viewport-and-scissor-use-minimum-color-depth-dimensions");
}
static void test_visibility_offsets(sky_mtl_command_queue_t *queue,sky_mtl_texture_t *color,sky_mtl_texture_t *depth,
    const sky_mtl_render_state_t *original,void *memory,uint64_t address) {
    enum {DRAWS=4097};sky_mtl_operation_t *ops=calloc(DRAWS+3,sizeof *ops);assert(ops);
    sequence_clear(ops,color,depth);ops[1].kind=SKY_MTL_OP_RESET_COUNTER;ops[1].counter_type=1;
    sky_mtl_render_state_t state=*original;state.depth_test=state.depth_write=state.stencil_test=0;
    sequence_draw(ops+2,&state,color,depth,address);
    for(unsigned i=1;i<DRAWS;i++)ops[2+i]=ops[2];
    ops[DRAWS+2].kind=SKY_MTL_OP_REPORT_COUNTER;ops[DRAWS+2].counter_type=1;ops[DRAWS+2].destination_address=address+8192;
    sequence_submit(queue,ops,DRAWS+3);free(ops);
    const uint64_t *counter=(const uint64_t *)((uint8_t *)memory+8192);
    assert(counter[0]==DRAWS*16ull&&counter[1]);
    sequence_color(color,(uint8_t[]){255,255,255,255},(uint8_t[]){255,255,255,255});
    puts("NVN_VISIBILITY_REUSE_GPU_OK per-draw-offsets-4096-slot-rollover-and-ordered-counter-report");
}
static void test_encoder_sequences(sky_mtl_command_queue_t *queue,sky_mtl_texture_t *a,sky_mtl_texture_t *depth,
    const sky_mtl_render_state_t *original,void *memory,uint64_t address) {
    const float red[4]={1,0,0,1},green[4]={0,1,0,1},blue[4]={0,0,1,1};
    const uint8_t r[4]={255,0,0,255},g[4]={0,255,0,255},b[4]={0,0,255,255};
    memcpy((uint8_t *)memory+64,red,16);memcpy((uint8_t *)memory+128,green,16);memcpy((uint8_t *)memory+192,blue,16);
    sky_mtl_render_state_t base=*original;base.depth_test=base.depth_write=base.stencil_test=0;
    sky_mtl_texture_t *other=texture(70);sky_mtl_operation_t *ops=calloc(12,sizeof *ops);assert(ops);
    sequence_clear(ops,a,depth);sequence_draw(ops+1,&base,a,depth,address+64);
    ops[1].state.viewport[2]=2;ops[1].state.has_scissor=1;memcpy(ops[1].state.scissor,(int[]){0,0,2,4},16);
    ops[2].kind=SKY_MTL_OP_STATE;ops[2].state.changed=SKY_MTL_STATE_BLEND_COLOR;
    sequence_draw(ops+3,&base,a,depth,address+128);ops[3].state.has_viewport=ops[3].state.has_scissor=0;
    sequence_draw(ops+4,&base,a,depth,address+192);ops[4].state.has_scissor=1;memcpy(ops[4].state.scissor,(int[]){2,0,2,4},16);
    sequence_submit(queue,ops,5);sequence_color(a,g,b);
    memset(ops,0,12*sizeof *ops);sequence_clear(ops,a,depth);sequence_draw(ops+1,&base,a,depth,address+64);
    sequence_draw(ops+2,&base,other,depth,address+128);sequence_draw(ops+3,&base,a,depth,address+192);
    ops[3].state.has_scissor=1;memcpy(ops[3].state.scissor,(int[]){2,0,2,4},16);
    sequence_submit(queue,ops,4);sequence_color(a,r,b);sequence_color(other,g,g);
    memset(ops,0,12*sizeof *ops);memcpy((uint8_t *)memory+1024,red,16);
    sequence_clear(ops,a,depth);sequence_draw(ops+1,&base,a,depth,address+1024);
    ops[2].kind=SKY_MTL_OP_BARRIER;ops[2].barriers=UINT32_MAX;
    ops[3].kind=SKY_MTL_OP_UPDATE_BUFFER;ops[3].destination_address=address+1024;ops[3].payload=(void *)green;ops[3].payload_size=16;
    sequence_draw(ops+4,&base,a,depth,address+1024);ops[4].state.has_scissor=1;memcpy(ops[4].state.scissor,(int[]){2,0,2,4},16);
    sequence_submit(queue,ops,5);sequence_color(a,r,g);
    memset(ops,0,12*sizeof *ops);sequence_draw(ops,&base,a,depth,address+64);sequence_clear(ops+1,a,depth);
    sequence_draw(ops+2,&base,a,depth,address+192);ops[2].state.has_scissor=1;memcpy(ops[2].state.scissor,(int[]){0,0,2,4},16);
    ops[3].kind=SKY_MTL_OP_REPORT_COUNTER;ops[3].counter_type=0;ops[3].destination_address=address+2048;
    sequence_draw(ops+4,&base,a,depth,address+64);ops[4].state.has_scissor=1;memcpy(ops[4].state.scissor,(int[]){2,0,2,4},16);
    unsigned sync_calls=0;ops[5].kind=SKY_MTL_OP_SYNC;ops[5].sync_action=sequence_sync;ops[5].sync_context=&sync_calls;
    sequence_draw(ops+6,&base,a,depth,address+128);ops[6].state.has_scissor=1;memcpy(ops[6].state.scissor,(int[]){0,0,2,4},16);
    sequence_submit(queue,ops,7);sequence_color(a,g,r);assert(sync_calls==1);
    const uint64_t *counter=(const uint64_t *)((uint8_t *)memory+2048);assert(!counter[0]&&counter[1]);
    memset(ops,0,12*sizeof *ops);sequence_clear(ops,a,depth);sequence_draw(ops+1,&base,a,depth,address+64);
    ops[2].kind=SKY_MTL_OP_TRANSFER;ops[2].transfer.kind=SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE;
    ops[2].transfer.source=a;ops[2].transfer.destination=other;ops[2].transfer.source_format=ops[2].transfer.destination_format=0x25;
    memcpy(ops[2].transfer.source_region,(int[]){0,0,0,4,4,1},24);memcpy(ops[2].transfer.destination_region,ops[2].transfer.source_region,24);
    sequence_draw(ops+3,&base,a,depth,address+128);ops[3].state.has_scissor=1;memcpy(ops[3].state.scissor,(int[]){2,0,2,4},16);
    sequence_submit(queue,ops,4);sequence_color(a,r,g);sequence_color(other,r,r);
    /* Real compute and writable vertex/fragment stages publish data consumed
     * by a later draw. None may share a render encoder across that hazard. */
    char directory[]="/tmp/sky-render-sequence-XXXXXX";assert(mkdtemp(directory));
    const char *sources[]={
        "#include <metal_stdlib>\nusing namespace metal;struct A{device float4* out [[id(0)]];};kernel void main0(constant A&a [[buffer(0)]]){a.out[0]=float4(0,1,0,1);}",
        "#include <metal_stdlib>\nusing namespace metal;struct A{device float4* out [[id(0)]];};struct O{float4 p [[position]];};vertex O main0(constant A&a [[buffer(0)]],uint i [[vertex_id]]){a.out[i]=float4(0,1,0,1);return {float4(float(int(i&1)*2-1),float(int(i>>1)*2-1),0,1)};}",
        "#include <metal_stdlib>\nusing namespace metal;struct A{device float4* out [[id(0)]];};fragment float4 main0(constant A&a [[buffer(0)]],float4 p [[position]]){a.out[uint(p.y)*4+uint(p.x)]=float4(0,1,0,1);return float4(1,0,0,1);}"
    };
    for(unsigned test=0;test<3;test++) {
        char source[PATH_MAX],reflection[PATH_MAX];assert(snprintf(source,sizeof source,"%s/%u.metal",directory,test)<(int)sizeof source);
        assert(snprintf(reflection,sizeof reflection,"%s/%u.json",directory,test)<(int)sizeof reflection);
        FILE *f=fopen(source,"w");assert(f&&fputs(sources[test],f)>=0&&!fclose(f));f=fopen(reflection,"w");assert(f);
        assert(fputs("{\"route\":\"B\",\"entry\":\"main0\",\"bindings\":[{\"kind\":\"sbuf\",\"reg\":0,\"arg_id\":0,\"rw\":true}],\"vertex_inputs\":[],\"threadgroup_size\":[1,1,1]}",f)>=0&&!fclose(f));
        uint64_t serial=UINT64_C(0x400000010)+test;unsigned stage=test==0?5:test-1;
        assert(sky_mtl_program_set_shader(serial,stage,source,reflection));
        memcpy((uint8_t *)memory+4096,red,16);memset(ops,0,12*sizeof *ops);sequence_clear(ops,a,depth);
        sequence_draw(ops+1,&base,a,depth,test==0?address+4096:address+64);
        unsigned last=2;
        if(test==0){ops[2].kind=SKY_MTL_OP_DISPATCH;ops[2].groups[0]=ops[2].groups[1]=ops[2].groups[2]=1;last=3;}
        sky_mtl_render_state_t *write=&ops[test==0?2:1].state;
        write->program_mask|=1u<<stage;write->programs[stage]=serial;write->storage_bound[stage][0]=1;
        write->storage[stage][0]=(sky_mtl_buffer_binding_t){address+4096,256};
        sequence_draw(ops+last,&base,a,depth,address+4096);ops[last].state.has_scissor=1;memcpy(ops[last].state.scissor,(int[]){2,0,2,4},16);
        sequence_submit(queue,ops,last+1);sequence_color(a,r,g);sky_mtl_program_remove(serial);
        assert(!unlink(source)&&!unlink(reflection));
    }
    assert(!rmdir(directory));free(ops);sky_mtl_texture_destroy(other);
    puts("NVN_ENCODER_SEQUENCE_GPU_OK defaults-arguments-attachments-clear-barrier-update-transfer-compute-writable-stages-counter-sync");
}
int main(void) {
    @autoreleasepool {
        const uint64_t serial=UINT64_C(0x400000001);
        fixture(serial,0,"vertex_id_quad");
        fixture(serial,1,"uniform_color");
        void *memory=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(memory!=MAP_FAILED);
        memcpy(memory,(float[]){1,1,1,1},16);sky_mtl_memory_pool_t *pool=sky_mtl_memory_pool_create(memory,65536);assert(pool);
        sky_mtl_command_queue_t *queue=sky_mtl_command_queue_create();assert(queue);
        sky_mtl_texture_t *color=texture(70),*depth=texture(260);
        sky_mtl_render_state_t base={0};base.programs[0]=base.programs[1]=serial;base.program_mask=63;
        base.uniform_bound[1][0]=1;base.uniform[1][0]=(sky_mtl_buffer_binding_t){sky_mtl_memory_pool_gpu_address(pool),65536};
        base.changed=UINT32_MAX;base.channel_mask=15;base.has_viewport=1;base.viewport[2]=base.viewport[3]=4;base.viewport[5]=1;
        base.depth_test=base.depth_write=1;base.depth_func=8;
        for(unsigned i=0;i<3;i++)render_case(queue,color,depth,&base,i==1,0xa5,1,i==1?0:.5f,0xa5);
        sky_mtl_render_state_t state=base;state.depth_write=0;
        render_case(queue,color,depth,&state,1,0xa5,1,.5f,0xa5);state.depth_func=1;
        render_case(queue,color,depth,&state,1,0xa5,0,.5f,0xa5);state.depth_func=255;
        render_case(queue,color,depth,&state,1,0xa5,1,.5f,0xa5);state.depth_test=0;state.depth_write=1;state.depth_func=1;
        render_case(queue,color,depth,&state,1,0xa5,1,.5f,0xa5);
        test_encoder_sequences(queue,color,depth,&base,memory,sky_mtl_memory_pool_gpu_address(pool));
        test_attachment_render_area(queue,depth,&base);
        test_visibility_offsets(queue,color,depth,&base,memory,sky_mtl_memory_pool_gpu_address(pool));
        base.depth_test=base.depth_write=0;base.stencil_test=1;
        for(unsigned face=0;face<2;face++) {
            base.stencil[face].function=8;base.stencil[face].read_mask=base.stencil[face].write_mask=255;
            base.stencil[face].fail=base.stencil[face].depth_fail=base.stencil[face].pass=1;
        }
        state=base;state.stencil[0].pass=state.stencil[1].pass=3;
        state.stencil[0].reference=0x3c;state.stencil[1].reference=0x7e;
        uint8_t facing=render_case(queue,color,depth,&state,1,0xa5,1,.5f,-1);
        assert(facing==0x3c||facing==0x7e);unsigned front_winding=facing==0x3c?0:1;
        state.front_face=1;render_case(queue,color,depth,&state,1,0xa5,1,.5f,facing==0x3c?0x7e:0x3c);
        for(unsigned face=0;face<2;face++) {
            for(unsigned branch=0;branch<3;branch++)for(unsigned operation=1;operation<=8;operation++) {
                state=base;state.front_face=face?front_winding^1:front_winding;
                state.stencil[face].reference=0x3c;
                if(branch==0){state.stencil[face].function=1;state.stencil[face].fail=operation;}
                else if(branch==1){state.depth_test=1;state.depth_func=1;state.stencil[face].depth_fail=operation;}
                else state.stencil[face].pass=operation;
                render_case(queue,color,depth,&state,1,0xa5,branch==2,.5f,stencil_result(operation,0xa5,0x3c));
            }
            for(unsigned operation=4;operation<=8;operation++)if(operation!=6) {
                state=base;state.front_face=face?front_winding^1:front_winding;state.stencil[face].pass=operation;
                uint8_t seed=(operation==4||operation==7)?255:0;
                render_case(queue,color,depth,&state,1,seed,1,.5f,stencil_result(operation,seed,0));
            }
            state=base;state.front_face=face?front_winding^1:front_winding;state.stencil[face].function=3;
            state.stencil[face].reference=0x35;state.stencil[face].read_mask=15;
            render_case(queue,color,depth,&state,1,0xa5,1,.5f,0xa5);state.stencil[face].read_mask=255;
            render_case(queue,color,depth,&state,1,0xa5,0,.5f,0xa5);
            state.stencil[face].function=8;state.stencil[face].pass=3;state.stencil[face].reference=0x3c;
            state.stencil[face].write_mask=15;render_case(queue,color,depth,&state,1,0xa5,1,.5f,0xac);
            state.stencil[face].write_mask=240;render_case(queue,color,depth,&state,1,0xa5,1,.5f,0x35);
            state.stencil[face].write_mask=255;render_case(queue,color,depth,&state,1,0xa5,1,.5f,0x3c);
            state.stencil[face].reference=0x7e;render_case(queue,color,depth,&state,1,0xa5,1,.5f,0x7e);
        }
        state=base;state.stencil_test=0;memset(state.stencil,255,sizeof state.stencil);
        render_case(queue,color,depth,&state,1,0xa5,1,.5f,0xa5);
        sky_mtl_command_queue_destroy(queue);sky_mtl_texture_destroy(color);sky_mtl_texture_destroy(depth);
        sky_mtl_memory_pool_destroy(pool);assert(!munmap(memory,65536));sky_mtl_program_remove(serial);
        assert(!sky_mtl_texture_active_count()&&!sky_mtl_command_queue_active_count()&&!sky_mtl_memory_pool_active_count());
        printf("T_NVN_RENDER_STATE_OK checks=%u attachment-depth-write-compare-front-back-stencil-operations-masks-dynamic-reference\n",checks);
    }
    return 0;
}
