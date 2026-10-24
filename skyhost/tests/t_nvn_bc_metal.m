#import "nvn_metal.h"
#import "nvn_bc.h"
#import <Metal/Metal.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* These tests submit explicit texture transfers, not NVN descriptor pools. */
void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *state) {
    for(int s=0;s<6;s++) {
        for(int i=0;i<128;i++)assert(!state->texture_bound[s][i]&&!state->sampler_bound[s][i]);
        for(int i=0;i<32;i++)assert(!state->image_bound[s][i]);
    }
}

static sky_mtl_texture_t *make_texture(unsigned format,unsigned type,unsigned width,unsigned height,
                                      unsigned depth,unsigned layers,unsigned levels) {
    sky_mtl_texture_properties_t p={.pixel_format=format,.texture_type=type,.storage_mode=SKY_MTL_STORAGE_PRIVATE,
        .width=width,.height=height,.depth=depth,.array_length=layers,.mipmap_level_count=levels,.sample_count=1,
        .usage=SKY_MTL_TEXTURE_USAGE_SHADER_READ|SKY_MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW,.swizzle={2,3,4,5}};
    sky_mtl_texture_t *texture=sky_mtl_texture_create(&p);assert(texture&&sky_mtl_texture_is_bc_fallback(texture));
    sky_mtl_texture_properties_t actual;assert(sky_mtl_texture_get_properties(texture,&actual));
    assert(actual.pixel_format==format&&actual.width==width&&actual.height==height);
    return texture;
}

static void color_block(uint8_t block[8],uint16_t color) {
    memset(block,0,8);block[0]=(uint8_t)color;block[1]=(uint8_t)(color>>8);
}

static void sample(sky_mtl_texture_t *texture,float x,float y,float output[4]) {
    id<MTLTexture> native=(__bridge id<MTLTexture>)sky_mtl_texture_native_handle(texture);
    id<MTLDevice> device=native.device;
    NSString *code=@"#include <metal_stdlib>\nusing namespace metal; kernel void sample_pixel(texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]], constant float2 &uv [[buffer(1)]]) { constexpr sampler s(coord::normalized,filter::nearest); o[0]=t.sample(s,uv); }";
    NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithSource:code options:nil error:&error];
    assert(library&&!error);
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"sample_pixel"] error:&error];
    assert(pipeline&&!error);
    id<MTLBuffer> result=[device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];float uv[2]={x,y};
    [encoder setComputePipelineState:pipeline];[encoder setTexture:native atIndex:0];
    [encoder setBuffer:result offset:0 atIndex:0];[encoder setBytes:uv length:sizeof uv atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    assert(command.status==MTLCommandBufferStatusCompleted&&!command.error);memcpy(output,result.contents,16);
}

static void test_formats_and_edges(void) {
    const unsigned formats[]={130,131,132,133,134,135,140,141,142,143,150,151,152,153};
    const int nvn[]={0x42,0x46,0x43,0x47,0x44,0x48,0x49,0x4a,0x4b,0x4c,0x4f,0x50,0x4d,0x4e};
    for(unsigned i=0;i<14;i++) {
        sky_bc_format_info_t info;assert(sky_bc_format_info(formats[i],&info));
        uint8_t block[16]={0},blocks[64],expected[5*7*8],actual[5*7*8];
        if(formats[i]<=135) {
            unsigned offset=formats[i]<132?0:8;color_block(block+offset,0xf800);
            if(formats[i]>=132)memset(block,255,8);
        } else if(formats[i]<=143) {
            block[0]=(formats[i]&1)?0x81:0xff;
            if(formats[i]>=142)block[8]=(formats[i]&1)?0x7f:0x80;
        } else if(formats[i]>=152)block[0]=0x40; /* Valid BC7 mode6, black endpoints. */
        for(unsigned b=0;b<4;b++)memcpy(blocks+b*info.block_bytes,block,info.block_bytes);
        assert(sky_bc_decode_region(formats[i],5,7,blocks,4*info.block_bytes,2*info.block_bytes,
            expected,sizeof expected,5*info.bytes_per_pixel));
        sky_mtl_texture_t *texture=make_texture(formats[i],SKY_MTL_TEXTURE_TYPE_2D,5,7,1,1,1);
        id<MTLTexture> native=(__bridge id<MTLTexture>)sky_mtl_texture_native_handle(texture);
        assert(native.pixelFormat==info.decoded_format);
        int region[6]={0,0,0,5,7,1};
        assert(sky_mtl_texture_upload_packaged(texture,nvn[i],0,0,region,blocks));
        size_t read=0;assert(!sky_mtl_texture_readback(texture,0,0,SKY_MTL_READ_COLOR,actual,sizeof actual,&read));
        assert(sky_mtl_texture_readback_decoded(texture,0,0,actual,sizeof actual,&read));
        assert(read==5*7*info.bytes_per_pixel&&!memcmp(actual,expected,read));
        if(formats[i]==141||formats[i]==143) {
            float value[4];sample(texture,.5f,.5f,value);assert(fabsf(value[0]+1)<.001f);
            if(formats[i]==143)assert(fabsf(value[1]-1)<.001f);
        }
        sky_mtl_texture_destroy(texture);
    }
    sky_mtl_texture_t *edge=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D,5,7,1,1,1);
    uint8_t red[32],blue[8],pixels[5*7*4];
    for(unsigned i=0;i<4;i++)color_block(red+i*8,0xf800);color_block(blue,0x001f);
    int full[6]={0,0,0,5,7,1},corner[6]={4,4,0,1,3,1},bad[6]={0,0,0,1,3,1};
    assert(sky_mtl_texture_upload_packaged(edge,0x42,0,0,full,red));
    assert(!sky_mtl_texture_upload_packaged(edge,0x42,0,0,bad,blue));
    assert(sky_mtl_texture_upload_packaged(edge,0x42,0,0,corner,blue));
    assert(sky_mtl_texture_readback_decoded(edge,0,0,pixels,sizeof pixels,NULL));
    for(unsigned y=0;y<7;y++)for(unsigned x=0;x<5;x++) {
        uint8_t *p=pixels+(y*5+x)*4;int is_blue=x==4&&y>=4;
        assert(p[0]==(is_blue?0:255)&&p[1]==0&&p[2]==(is_blue?255:0)&&p[3]==255);
    }
    sky_mtl_texture_destroy(edge);
}

static float srgb(float value) { return value<=.04045f?value/12.92f:powf((value+.055f)/1.055f,2.4f); }

static void test_srgb_swizzle_lifetime(void) {
    uint8_t blocks[32],gray[8];color_block(gray,0x8410);
    for(unsigned i=0;i<4;i++)memcpy(blocks+i*8,gray,8);
    sky_mtl_texture_t *base=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D,8,8,1,1,1);
    int region[6]={0,0,0,8,8,1};assert(sky_mtl_texture_upload_packaged(base,0x42,0,0,region,blocks));
    const uint8_t identity[4]={2,3,4,5},swap[4]={4,3,2,5};
    sky_mtl_texture_t *view=sky_mtl_texture_view_create(base,131,SKY_MTL_TEXTURE_TYPE_2D,0,1,0,1,identity);
    assert(view);sky_mtl_texture_t *held=sky_mtl_texture_retain(view);assert(held);
    assert(!sky_mtl_texture_view_create(base,134,SKY_MTL_TEXTURE_TYPE_2D,0,1,0,1,identity));
    float linear[4],gamma[4];sample(base,.5,.5,linear);sample(view,.5,.5,gamma);
    assert(fabsf(linear[0]-132.f/255)<.002&&fabsf(gamma[0]-srgb(132.f/255))<.002);
    sky_mtl_texture_destroy(base);sky_mtl_texture_destroy(view);
    uint8_t red[8];color_block(red,0xf800);int part[6]={0,0,0,4,4,1};
    assert(sky_mtl_texture_upload_packaged(held,0x46,0,0,part,red));
    sky_mtl_texture_t *swizzled=sky_mtl_texture_view_create(held,131,SKY_MTL_TEXTURE_TYPE_2D,0,1,0,1,swap);
    assert(swizzled);sample(swizzled,.1,.1,gamma);
    assert(gamma[0]<.001&&gamma[1]<.001&&gamma[2]>.999&&gamma[3]>.999);
    sky_mtl_texture_destroy(held);sky_mtl_texture_destroy(swizzled);
}

static void test_nested_views(void) {
    sky_mtl_texture_t *root=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D_ARRAY,16,8,1,2,3);
    uint8_t blocks[64],blue[8];for(unsigned i=0;i<8;i++)color_block(blocks+i*8,0xf800);color_block(blue,0x001f);
    for(unsigned level=0;level<3;level++)for(unsigned layer=0;layer<2;layer++) {
        int region[6]={0,0,0,16>>level,8>>level,1};
        assert(sky_mtl_texture_upload_packaged(root,0x42,level,layer,region,blocks));
    }
    const uint8_t identity[4]={2,3,4,5};
    sky_mtl_texture_t *first=sky_mtl_texture_view_create(root,130,SKY_MTL_TEXTURE_TYPE_2D,1,2,1,1,identity);
    sky_mtl_texture_t *nested=sky_mtl_texture_view_create(first,130,SKY_MTL_TEXTURE_TYPE_2D,1,1,0,1,identity);
    assert(first&&nested);sky_mtl_texture_destroy(first);
    int region[6]={0,0,0,4,2,1};assert(sky_mtl_texture_upload_packaged(nested,0x42,0,0,region,blue));
    uint8_t pixels[32];assert(sky_mtl_texture_readback_decoded(root,2,1,pixels,sizeof pixels,NULL));
    for(unsigned i=0;i<8;i++)assert(pixels[i*4]==0&&pixels[i*4+2]==255&&pixels[i*4+3]==255);
    assert(sky_mtl_texture_readback_decoded(root,2,0,pixels,sizeof pixels,NULL));
    for(unsigned i=0;i<8;i++)assert(pixels[i*4]==255&&pixels[i*4+2]==0);
    sky_mtl_texture_destroy(root);float value[4];sample(nested,.5,.5,value);assert(value[2]>.999);
    sky_mtl_texture_destroy(nested);

    sky_mtl_texture_t *cube=make_texture(130,SKY_MTL_TEXTURE_TYPE_CUBE_ARRAY,8,8,1,2,2);
    sky_mtl_texture_t *faces=sky_mtl_texture_view_create(cube,130,SKY_MTL_TEXTURE_TYPE_2D_ARRAY,1,1,5,2,identity);
    assert(faces);uint8_t pair[16];color_block(pair,0xf800);color_block(pair+8,0x07e0);
    int two[6]={0,0,0,4,4,2};assert(sky_mtl_texture_upload_packaged(faces,0x42,0,0,two,pair));
    uint8_t face_pixels[64];
    assert(sky_mtl_texture_readback_decoded(cube,1,5,face_pixels,sizeof face_pixels,NULL));assert(face_pixels[0]==255);
    assert(sky_mtl_texture_readback_decoded(cube,1,6,face_pixels,sizeof face_pixels,NULL));assert(face_pixels[1]==255);
    sky_mtl_texture_destroy(cube);sky_mtl_texture_destroy(faces);
}

static void completed(void *context) { dispatch_semaphore_signal((__bridge dispatch_semaphore_t)context); }

static void test_ordered_queue_roundtrip(void) {
    size_t size=(size_t)getpagesize()*4;uint8_t *memory=NULL;assert(!posix_memalign((void **)&memory,getpagesize(),size));
    memset(memory,0xcd,size);sky_mtl_memory_pool_t *pool=sky_mtl_memory_pool_create(memory,size);assert(pool);
    uint64_t address=sky_mtl_memory_pool_gpu_address(pool);uint8_t green[8];color_block(green,0x07e0);color_block(memory+256,0xf800);
    sky_mtl_texture_t *source=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D,4,4,1,1,1);
    sky_mtl_texture_t *destination=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D,4,4,1,1,1);
    sky_mtl_operation_t state={.kind=SKY_MTL_OP_STATE},update={.kind=SKY_MTL_OP_UPDATE_BUFFER},upload={.kind=SKY_MTL_OP_TRANSFER},copy={.kind=SKY_MTL_OP_TRANSFER},read={.kind=SKY_MTL_OP_TRANSFER};
    state.next=&update;update.next=&upload;upload.next=&copy;copy.next=&read;
    update.destination_address=address+256;update.payload=green;update.payload_size=sizeof green;
    upload.state.changed=SKY_MTL_STATE_COPY_ROW|SKY_MTL_STATE_COPY_IMAGE;upload.state.copy_row_stride=24;upload.state.copy_image_stride=24;
    upload.transfer=(sky_mtl_transfer_t){.kind=SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,.destination=source,.destination_format=0x42,.buffer_address=address+256,
        .destination_region={0,0,0,4,4,1}};
    copy.transfer=(sky_mtl_transfer_t){.kind=SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE,.source=source,.destination=destination,
        .source_format=0x42,.destination_format=0x42,.source_region={0,0,0,4,4,1},.destination_region={0,0,0,4,4,1}};
    read.state.changed=SKY_MTL_STATE_COPY_ROW|SKY_MTL_STATE_COPY_IMAGE;read.state.copy_row_stride=32;read.state.copy_image_stride=64;
    read.transfer=(sky_mtl_transfer_t){.kind=SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,.source=destination,.source_format=0x42,
        .buffer_address=address+1024,.source_region={0,0,0,4,4,1}};
    sky_mtl_command_queue_t *queue=sky_mtl_command_queue_create();assert(queue);
    dispatch_semaphore_t semaphore=dispatch_semaphore_create(0);
    assert(sky_mtl_command_queue_submit_operations(queue,&state,completed,(__bridge void *)semaphore));
    assert(!dispatch_semaphore_wait(semaphore,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)));
    assert(!sky_mtl_command_queue_get_error(queue)&&!memcmp(memory+1024,green,8));
    for(unsigned i=8;i<64;i++)assert(memory[1024+i]==0xcd);
    float pixel[4];sample(destination,.5,.5,pixel);assert(pixel[0]<.001&&pixel[1]>.999&&pixel[2]<.001);
    sky_mtl_command_queue_destroy(queue);sky_mtl_texture_destroy(source);sky_mtl_texture_destroy(destination);
    sky_mtl_memory_pool_destroy(pool);free(memory);
}

static void test_queued_view_regions(void) {
    size_t size=(size_t)getpagesize()*4;uint8_t *memory=NULL;assert(!posix_memalign((void **)&memory,getpagesize(),size));
    memset(memory,0xcd,size);sky_mtl_memory_pool_t *pool=sky_mtl_memory_pool_create(memory,size);assert(pool);
    uint64_t address=sky_mtl_memory_pool_gpu_address(pool);uint8_t payload[96];memset(payload,0xe1,sizeof payload);
    color_block(payload,0xf800);color_block(payload+48,0x07e0);
    sky_mtl_texture_t *source=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D_ARRAY,16,8,1,3,3);
    sky_mtl_texture_t *destination=make_texture(130,SKY_MTL_TEXTURE_TYPE_2D_ARRAY,16,8,1,3,3);
    uint8_t blue[16];color_block(blue,0x001f);color_block(blue+8,0x001f);int full[6]={0,0,0,8,4,1};
    for(unsigned i=0;i<3;i++)assert(sky_mtl_texture_upload_packaged(destination,0x42,1,i,full,blue));
    const uint8_t identity[4]={2,3,4,5};
    sky_mtl_texture_t *src=sky_mtl_texture_view_create(source,130,SKY_MTL_TEXTURE_TYPE_2D_ARRAY,1,1,1,2,identity);
    sky_mtl_texture_t *dst=sky_mtl_texture_view_create(destination,130,SKY_MTL_TEXTURE_TYPE_2D_ARRAY,1,1,0,2,identity);
    assert(src&&dst);sky_mtl_texture_destroy(source);sky_mtl_texture_destroy(destination);
    sky_mtl_operation_t state={.kind=SKY_MTL_OP_STATE},update={.kind=SKY_MTL_OP_UPDATE_BUFFER},upload={.kind=SKY_MTL_OP_TRANSFER},copy={.kind=SKY_MTL_OP_TRANSFER},read={.kind=SKY_MTL_OP_TRANSFER};
    state.next=&update;update.next=&upload;upload.next=&copy;copy.next=&read;
    update.destination_address=address+256;update.payload=payload;update.payload_size=sizeof payload;
    upload.state.changed=SKY_MTL_STATE_COPY_ROW|SKY_MTL_STATE_COPY_IMAGE;upload.state.copy_row_stride=24;upload.state.copy_image_stride=48;
    upload.transfer=(sky_mtl_transfer_t){.kind=SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,.destination=src,.destination_format=0x42,
        .buffer_address=address+256,.destination_region={4,0,0,4,4,2}};
    copy.transfer=(sky_mtl_transfer_t){.kind=SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE,.flags=3,.source=src,.destination=dst,
        .source_format=0x42,.destination_format=0x42,.source_region={4,0,0,4,4,2},.destination_region={0,0,0,4,4,2}};
    read.state.changed=SKY_MTL_STATE_COPY_ROW|SKY_MTL_STATE_COPY_IMAGE;read.state.copy_row_stride=32;read.state.copy_image_stride=64;
    read.transfer=(sky_mtl_transfer_t){.kind=SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,.source=dst,.source_format=0x42,
        .buffer_address=address+1024,.source_region={0,0,0,4,4,2}};
    sky_mtl_command_queue_t *queue=sky_mtl_command_queue_create();assert(queue);dispatch_semaphore_t sem=dispatch_semaphore_create(0);
    assert(sky_mtl_command_queue_submit_operations(queue,&state,completed,(__bridge void *)sem));
    assert(!dispatch_semaphore_wait(sem,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)));
    assert(!sky_mtl_command_queue_get_error(queue));
    for(unsigned layer=0;layer<2;layer++) {
        assert(!memcmp(memory+1024+layer*64,payload+layer*48,8));
        for(unsigned p=8;p<64;p++)assert(memory[1024+layer*64+p]==0xcd);
        uint8_t pixels[8*4*4];assert(sky_mtl_texture_readback_decoded(dst,0,layer,pixels,sizeof pixels,NULL));
        for(unsigned y=0;y<4;y++)for(unsigned x=0;x<8;x++) {
            uint8_t *pixel=pixels+(y*8+x)*4;
            assert(pixel[0]==(x<4&&layer==0?255:0));
            assert(pixel[1]==(x<4&&layer==1?255:0));
            assert(pixel[2]==(x>=4?255:0)&&pixel[3]==255);
        }
    }
    sky_mtl_command_queue_destroy(queue);sky_mtl_texture_destroy(src);sky_mtl_texture_destroy(dst);
    sky_mtl_memory_pool_destroy(pool);free(memory);
}

int main(void) {
    @autoreleasepool {
        assert(!setenv("SKY_METAL_FORCE_BC_DECODE","1",1));size_t before=sky_mtl_texture_active_count();
        test_formats_and_edges();test_srgb_swizzle_lifetime();test_nested_views();test_ordered_queue_roundtrip();test_queued_view_regions();
        assert(sky_mtl_texture_active_count()==before);
        puts("NVN_BC_METAL_OK formats=14 actual-pixels signed srgb partial-edge nested-mips cube-faces retained-views gpu-update-ordered-lossless-roundtrip queued-view-regions-padded-strides");
    }
    return 0;
}
