#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include <assert.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <time.h>

_Static_assert(sizeof(sky_nvn_command_buffer_t) <= 0xa0, "command buffer exceeds NVN storage");

#define ASSERT_LOOKUP(name, function) \
    assert(sky_nvn_bootstrap_loader(name) == (void *)(function))

enum { TEST_WIDTH = 4, TEST_HEIGHT = 4, TEST_PIXELS = TEST_WIDTH * TEST_HEIGHT };

static void setup_memory(sky_nvn_device_t *device, sky_nvn_memory_pool_t *memory, void **allocation) {
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

static void setup_queue(sky_nvn_device_t *device, sky_nvn_queue_t *queue, void **allocation) {
    sky_nvn_queue_builder_t builder = {0};
    sky_nvn_queue_builder_set_device(&builder, device);
    sky_nvn_queue_builder_set_defaults(&builder);
    uint64_t size = sky_nvn_queue_builder_get_queue_memory_size(&builder);
    assert(posix_memalign(allocation, (size_t)getpagesize(), size) == 0);
    memset(*allocation, 0, size);
    sky_nvn_queue_builder_set_queue_memory(&builder, *allocation, size);
    assert(sky_nvn_queue_initialize(queue, &builder) == 1);
}

static void make_texture(sky_nvn_texture_t *texture, sky_nvn_device_t *device,
                         sky_nvn_memory_pool_t *memory, int format, int64_t offset) {
    sky_nvn_texture_builder_t builder = {0};
    sky_nvn_texture_builder_set_device(&builder, device);
    sky_nvn_texture_builder_set_defaults(&builder);
    sky_nvn_texture_builder_set_format(&builder, format);
    sky_nvn_texture_builder_set_size_2d(&builder, TEST_WIDTH, TEST_HEIGHT);
    sky_nvn_texture_builder_set_flags(&builder, 0x9);
    sky_nvn_texture_builder_set_storage(&builder, memory, offset);
    assert(sky_nvn_texture_initialize(texture, &builder) == 1);
}

static void memory_callback(void *data, int event, uint64_t size, void *callback_data) {
    (void)data;
    (void)event;
    (void)size;
    (void)callback_data;
    assert(0 && "clear-only recording must not request command memory");
}

typedef struct {
    sky_nvn_device_t *device;
    sky_nvn_command_buffer_t *command;
    sky_nvn_memory_pool_t pool;
    void *allocation;
    uint8_t control[16384+16];
    unsigned command_callbacks,control_callbacks;
    int refuse;
} command_refill_t;

static void command_refill(void *command_buffer,int event,uint64_t minimum,void *data) {
    command_refill_t *refill=data;
    assert(command_buffer==refill->command&&sky_nvn_command_buffer_is_recording(command_buffer));
    assert(event==0||event==1);
    if(event==0)refill->command_callbacks++;else refill->control_callbacks++;
    if(refill->refuse)return;
    if(event==0) {
        /* A game callback can allocate/register a new native pool reentrantly. */
        if(!refill->allocation) {
            assert(posix_memalign(&refill->allocation,(size_t)getpagesize(),4*131072)==0);
            memset(refill->allocation,0xa5,4*131072);
            sky_nvn_memory_pool_builder_t builder={0};
            sky_nvn_memory_pool_builder_set_device(&builder,refill->device);
            sky_nvn_memory_pool_builder_set_storage(&builder,refill->allocation,4*131072);
            sky_nvn_memory_pool_builder_set_flags(&builder,0x22);
            assert(sky_nvn_memory_pool_initialize(&refill->pool,&builder));
        }
        assert(refill->command_callbacks<=4);
        sky_nvn_command_buffer_add_command_memory(command_buffer,&refill->pool,(refill->command_callbacks-1)*131072,131072);
        assert(sky_nvn_command_buffer_get_command_memory_free(command_buffer)==131072);
    } else {
        assert(minimum<=16384);
        sky_nvn_command_buffer_add_control_memory(command_buffer,refill->control+8,16384);
        assert(sky_nvn_command_buffer_get_control_memory_free(command_buffer)==16384);
    }
}

static void expect_rejected(sky_nvn_queue_t *queue, uint64_t handle) {
    uint64_t submits = queue->submits;
    uint64_t completed = queue->completed;
    sky_nvn_queue_submit_commands(queue, 1, &handle);
    assert(queue->submits == submits);
    assert(queue->completed == completed);
}

static void expect_color(sky_nvn_texture_t *texture, const uint8_t expected[4]) {
    uint8_t pixels[TEST_PIXELS * 4];
    size_t read_size = 0;
    assert(sky_mtl_texture_readback(texture->metal, 0, 0, SKY_MTL_READ_COLOR,
                                    pixels, sizeof pixels, &read_size) == 1);
    assert(read_size == sizeof pixels);
    for (int index = 0; index < TEST_PIXELS; index++)
        assert(memcmp(pixels + index * 4, expected, 4) == 0);
}

static void test_invalid_command_diagnostics(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,
                                             sky_nvn_queue_t *queue) {
    FILE *capture=tmpfile();assert(capture);
    fflush(stderr);int saved=dup(STDERR_FILENO);assert(saved>=0);
    assert(dup2(fileno(capture),STDERR_FILENO)>=0);
    sky_nvn_command_buffer_t command={0};assert(sky_nvn_command_buffer_initialize(&command,device));
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_scissor(&command,0,0,-1,4);
    /* A later validation failure must not replace the original cause. */
    sky_nvn_command_buffer_set_render_targets(&command,9,NULL,NULL,NULL,NULL);
    uint64_t invalid=sky_nvn_command_buffer_end_recording(&command);expect_rejected(queue,invalid);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_call_commands(&command,1,&invalid);
    expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
    uint8_t control[32]={0};
    sky_nvn_command_buffer_add_control_memory(&command,control,sizeof control);
    sky_nvn_command_buffer_add_control_memory(&command,NULL,4096);
    sky_nvn_command_buffer_begin_recording(&command);
    expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
    /* Record retirement also retains a concrete reason and the exact handle. */
    sky_nvn_command_buffer_add_control_memory(&command,control,sizeof control);
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x3f0000,64);
    sky_nvn_command_buffer_begin_recording(&command);
    uint64_t retired=sky_nvn_command_buffer_end_recording(&command);
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x3f0000,64);expect_rejected(queue,retired);
    sky_nvn_command_buffer_finalize(&command);
    fflush(stderr);assert(dup2(saved,STDERR_FILENO)>=0);close(saved);
    rewind(capture);char text[12288];size_t length=fread(text,1,sizeof text-1,capture);text[length]=0;fclose(capture);
    assert(strstr(text,"api=sky_nvn_command_buffer_set_scissor"));
    assert(strstr(text,"invalid_api=sky_nvn_command_buffer_set_scissor"));
    assert(strstr(text,"invalid_reason=invalid-scissor"));
    assert(strstr(text,"invalid_reason=invalid-called-handle"));
    assert(!strstr(text,"invalid-color-target-count"));
    assert(strstr(text,"NVN_COMMAND_BUFFER_CONTROL_REVOKED"));
    assert(!strstr(text,"NVN_COMMAND_BUFFER_CONTROL_ERROR"));
    assert(strstr(text,"invalid_reason=control-memory-exhausted"));
    assert(strstr(text,"invalid_reason=command-memory-reused"));
    char handle[64];snprintf(handle,sizeof handle,"handle=%#llx",(unsigned long long)invalid);assert(strstr(text,handle));
    snprintf(handle,sizeof handle,"handle=%#llx",(unsigned long long)retired);assert(strstr(text,handle));
    puts("NVN_COMMAND_INVALID_DIAGNOSTICS_OK first-cause-handle-retirement-and-control-revocation");
}

static void test_command_memory(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,
                                sky_nvn_queue_t *queue,sky_nvn_texture_t *target) {
    uint64_t (*command_free)(const void *)=sky_nvn_bootstrap_loader("nvnCommandBufferGetCommandMemoryFree");
    uint64_t (*control_free)(const void *)=sky_nvn_bootstrap_loader("nvnCommandBufferGetControlMemoryFree");
    assert(command_free==sky_nvn_command_buffer_get_command_memory_free);
    assert(control_free==sky_nvn_command_buffer_get_control_memory_free);
    sky_nvn_command_buffer_t command={0};
    assert(!command_free(NULL)&&!control_free(&command));
    assert(sky_nvn_command_buffer_initialize(&command,device));
    assert(!command_free(&command)&&!control_free(&command));
    command_refill_t refill={.device=device,.command=&command};
    memset(refill.control,0xa5,sizeof refill.control);
    sky_nvn_command_buffer_set_memory_callback(&command,command_refill);
    sky_nvn_command_buffer_set_memory_callback_data(&command,&refill);
    uint8_t *cpu=sky_nvn_memory_pool_map(memory),control[32];memset(control,0xa5,sizeof control);
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x300000,64);
    sky_nvn_command_buffer_add_control_memory(&command,control+8,16);
    assert(command_free(&command)==64&&control_free(&command)==16);
    sky_nvn_command_buffer_begin_recording(&command);
    assert(command_free(&command)==48);
    sky_nvn_command_buffer_report_value(&command,123,memory->buffer_address+0x320000);
    uint64_t first=sky_nvn_command_buffer_end_recording(&command);
    assert(command_free(&command)==16&&control_free(&command)==0);
    assert(!refill.command_callbacks&&!refill.control_callbacks);

    /* Same-frame recording keeps the old cursor. Its next operation triggers
     * the guest's fixed 128 KiB / 16 KiB replenishment callbacks. */
    sky_nvn_command_buffer_begin_recording(&command);assert(command_free(&command)==0);
    sky_nvn_command_buffer_report_value(&command,222,memory->buffer_address+0x320000);
    void *targets[]={target};const float cyan[]={0,1,1,1};
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_clear_color(&command,0,cyan,15);
    uint64_t second=sky_nvn_command_buffer_end_recording(&command);
    assert(refill.command_callbacks==1&&refill.control_callbacks==1);
    assert(command_free(&command)==131072-48&&control_free(&command)==16384-16);
    assert(cpu[0x2fffff]==0xa5&&cpu[0x300040]==0xa5);
    for(int i=0;i<8;i++)assert(control[i]==0xa5&&control[i+24]==0xa5);
    for(int i=0;i<8;i++)assert(refill.control[i]==0xa5&&refill.control[i+16392]==0xa5);
    sky_nvn_queue_submit_commands(queue,1,&first);sky_nvn_queue_finish(queue);
    uint32_t value;memcpy(&value,cpu+0x320000,sizeof value);assert(value==123);
    uint64_t submits=queue->submits;
    sky_nvn_queue_submit_commands(queue,1,&second);assert(queue->submits==submits+1);
    /* The second handle spans BOTH grants. Reusing its first segment retires
     * the snapshot, while the already submitted GPU work remains valid. */
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x300000,64);
    expect_rejected(queue,first);expect_rejected(queue,second);
    sky_nvn_queue_finish(queue);memcpy(&value,cpu+0x320000,sizeof value);assert(value==222);
    expect_color(target,(uint8_t[]){0,255,255,255});

    /* Inline update bytes consume finite command space and retain their data
     * after the caller changes its original payload. */
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x300100,128);
    sky_nvn_command_buffer_begin_recording(&command);
    uint32_t payload[]={1,2,3,4,5};
    sky_nvn_command_buffer_update_uniform_buffer(&command,memory->buffer_address+0x320010,20,0,20,payload);
    assert(command_free(&command)==72);
    uint64_t update=sky_nvn_command_buffer_end_recording(&command);assert(command_free(&command)==56);
    memset(payload,0xff,sizeof payload);
    sky_nvn_queue_submit_commands(queue,1,&update);sky_nvn_queue_finish(queue);
    assert(!memcmp(cpu+0x320010,(uint32_t[]){1,2,3,4,5},20));

    /* Skyrim ignores minimumSize and always grants fixed chunks. An oversized
     * request or a callback that supplies nothing must fail after one call. */
    sky_nvn_command_buffer_begin_recording(&command);
    uint8_t *large=calloc(1,131080);assert(large);
    sky_nvn_command_buffer_update_uniform_buffer(&command,memory->buffer_address+0x340000,131080,0,131080,large);
    free(large);expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
    assert(refill.command_callbacks==2);
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x300200,16);
    refill.refuse=1;
    sky_nvn_command_buffer_begin_recording(&command);
    expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
    assert(refill.command_callbacks==3);
    /* NULL is used by Skyrim to revoke control memory during teardown. A new
     * recording must refill instead of retaining the potentially freed pointer. */
    refill.refuse=0;
    sky_nvn_command_buffer_add_control_memory(&command,NULL,4096);assert(!control_free(&command));
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x300300,128);
    sky_nvn_command_buffer_begin_recording(&command);
    uint64_t after_revoke=sky_nvn_command_buffer_end_recording(&command);
    assert(refill.control_callbacks==2&&control_free(&command)==16384-16);
    sky_nvn_queue_submit_commands(queue,1,&after_revoke);sky_nvn_queue_finish(queue);
    sky_nvn_command_buffer_set_memory_callback(&command,NULL);
    sky_nvn_command_buffer_add_control_memory(&command,NULL,4096);assert(!control_free(&command));
    sky_nvn_command_buffer_begin_recording(&command);
    expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
    sky_nvn_command_buffer_finalize(&command);assert(!command_free(&command)&&!control_free(&command));
    sky_nvn_memory_pool_finalize(&refill.pool);free(refill.allocation);
    puts("NVN_COMMAND_MEMORY_OK finite-free-cursors-inline-payload-refill-reentrancy-segment-lifetime-and-exhaustion");
}


static void install_fixture(uint64_t serial,int stage,const char *key) {
    char executable[PATH_MAX];uint32_t length=sizeof executable;
    assert(_NSGetExecutablePath(executable,&length)==0);
    char *slash=strrchr(executable,'/');assert(slash);*slash=0;
    char source[PATH_MAX],reflection[PATH_MAX];
    assert(snprintf(source,sizeof source,"%s/../tests/fixtures/shaders/%s.metal",executable,key)<(int)sizeof source);
    assert(snprintf(reflection,sizeof reflection,"%s/../tests/fixtures/shaders/%s.json",executable,key)<(int)sizeof reflection);
    assert(sky_mtl_program_set_shader(serial,stage,source,reflection));
}

static void unexpected_compute_completion(void *context) {
    (void)context;assert(!"invalid compute work must not be submitted");
}
static void test_compute_device_limits(sky_nvn_queue_t *queue) {
    char directory[]="/tmp/sky-nvn-compute-limits-XXXXXX";assert(mkdtemp(directory));
    char source[PATH_MAX],reflection[PATH_MAX];
    assert(snprintf(reflection,sizeof reflection,"%s/test.json",directory)<(int)sizeof reflection);
    const uint32_t groups[][3]={{0,1,1},{UINT32_MAX,1,1},{1024,1024,1}};
    sky_mtl_operation_t *operation=calloc(1,sizeof *operation);assert(operation);
    operation->kind=SKY_MTL_OP_DISPATCH;operation->state.program_mask=1u<<5;
    operation->groups[0]=operation->groups[1]=operation->groups[2]=1;
    for(unsigned i=0;i<sizeof groups/sizeof groups[0];i++) {
        /* Shader paths identify immutable cached source+reflection pairs. */
        assert(snprintf(source,sizeof source,"%s/test%u.metal",directory,i)<(int)sizeof source);
        FILE *file=fopen(source,"w");assert(file);
        assert(fputs("#include <metal_stdlib>\nusing namespace metal;kernel void main0() {}\n",file)>=0);assert(!fclose(file));
        file=fopen(reflection,"w");assert(file);
        assert(fprintf(file,"{\"route\":\"B\",\"entry\":\"main0\",\"stage\":\"cs\",\"bindings\":[],\"threadgroup_size\":[%u,%u,%u]}",
            groups[i][0],groups[i][1],groups[i][2])>0);assert(!fclose(file));
        uint64_t serial=UINT64_C(0x200000100)+i;
        assert(sky_mtl_program_set_shader(serial,5,source,reflection));
        operation->state.programs[5]=serial;
        assert(!sky_mtl_command_queue_submit_operations(queue->metal,operation,unexpected_compute_completion,NULL));
        sky_mtl_program_remove(serial);
        assert(!unlink(source));
    }
    free(operation);assert(!unlink(reflection));assert(!rmdir(directory));
    puts("NVN_COMPUTE_LIMITS_OK zero-axis-oversized-axis-and-thread-product-rejected-before-dispatch");
}

typedef struct {sky_nvn_queue_t *queue;uint64_t handle;} sky_submit_thread_t;
static void *submit_on_thread(void *context) {
    sky_submit_thread_t *s=context;sky_nvn_queue_submit_commands(s->queue,1,&s->handle);return NULL;
}
static void test_cross_queue_wait(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,sky_nvn_queue_t *consumer) {
    sky_nvn_queue_t producer;void *allocation=NULL;setup_queue(device,&producer,&allocation);
    sky_nvn_sync_t future;assert(sky_nvn_sync_initialize(&future,device));future.fenced=1;future.queue=&producer;future.fence=1;
    sky_nvn_command_buffer_t producer_command={0},consumer_command={0};
    assert(sky_nvn_command_buffer_initialize(&producer_command,device));assert(sky_nvn_command_buffer_initialize(&consumer_command,device));
    sky_nvn_command_buffer_begin_recording(&producer_command);
    sky_nvn_command_buffer_report_value(&producer_command,0x12345678,memory->buffer_address+0x130040);
    uint64_t producer_handle=sky_nvn_command_buffer_end_recording(&producer_command);
    sky_nvn_command_buffer_begin_recording(&consumer_command);sky_nvn_command_buffer_wait_sync(&consumer_command,&future);
    sky_nvn_command_buffer_report_value(&consumer_command,0x87654321,memory->buffer_address+0x130044);
    sky_submit_thread_t context={consumer,sky_nvn_command_buffer_end_recording(&consumer_command)};
    uint64_t actions=sky_mtl_graphics_stats().sync_actions;
    pthread_t thread;assert(pthread_create(&thread,NULL,submit_on_thread,&context)==0);
    for(int i=0;i<1000&&sky_mtl_graphics_stats().sync_actions==actions;i++)usleep(1000);
    assert(sky_mtl_graphics_stats().sync_actions>actions);
    /* The consumer has entered a wait on an unsent producer. It must release
     * renderer locks so the second queue can submit the signal-producing work. */
    sky_nvn_queue_submit_commands(&producer,1,&producer_handle);assert(pthread_join(thread,NULL)==0);
    sky_nvn_queue_finish(consumer);sky_nvn_queue_finish(&producer);
    const uint32_t *words=(const uint32_t *)((const uint8_t *)memory->mem+0x130040);
    assert(words[0]==0x12345678&&words[1]==0x87654321);
    sky_nvn_command_buffer_finalize(&producer_command);sky_nvn_command_buffer_finalize(&consumer_command);
    sky_nvn_sync_finalize(&future);sky_nvn_queue_finalize(&producer);free(allocation);
}

static void test_direct_texture_uploads(void) {
    uint8_t pixels[6*TEST_PIXELS*4],readback[TEST_PIXELS*4];
    for(int layer=0;layer<6;layer++)memset(pixels+layer*TEST_PIXELS*4,20+layer,TEST_PIXELS*4);
    sky_mtl_texture_properties_t p={.pixel_format=70,.texture_type=SKY_MTL_TEXTURE_TYPE_CUBE,.storage_mode=SKY_MTL_STORAGE_PRIVATE,
        .width=TEST_WIDTH,.height=TEST_HEIGHT,.depth=1,.array_length=1,.mipmap_level_count=1,.sample_count=1,.usage=SKY_MTL_TEXTURE_USAGE_SHADER_READ};
    sky_mtl_texture_t *cube=sky_mtl_texture_create(&p);assert(cube);
    int region[6]={0,0,0,TEST_WIDTH,TEST_HEIGHT,6};assert(sky_mtl_texture_upload_packaged(cube,0x25,0,0,region,pixels));
    for(int face=0;face<6;face++){
        assert(sky_mtl_texture_readback(cube,0,face,SKY_MTL_READ_COLOR,readback,sizeof readback,NULL));
        assert(memcmp(readback,pixels+face*TEST_PIXELS*4,sizeof readback)==0);
    }
    sky_mtl_texture_destroy(cube);
    p.texture_type=SKY_MTL_TEXTURE_TYPE_2D_ARRAY;p.array_length=2;
    sky_mtl_texture_t *array=sky_mtl_texture_create(&p);assert(array);region[5]=2;
    assert(sky_mtl_texture_upload_packaged(array,0x25,0,0,region,pixels));
    assert(sky_mtl_texture_readback(array,0,1,SKY_MTL_READ_COLOR,readback,sizeof readback,NULL));
    assert(memcmp(readback,pixels+TEST_PIXELS*4,sizeof readback)==0);sky_mtl_texture_destroy(array);
    p.texture_type=SKY_MTL_TEXTURE_TYPE_3D;p.width=p.height=p.depth=8;p.array_length=1;p.mipmap_level_count=2;
    sky_mtl_texture_t *volume=sky_mtl_texture_create(&p);assert(volume);region[5]=4;
    assert(sky_mtl_texture_upload_packaged(volume,0x25,1,0,region,pixels));
    assert(sky_mtl_texture_readback(volume,1,3,SKY_MTL_READ_COLOR,readback,sizeof readback,NULL));
    assert(memcmp(readback,pixels+3*TEST_PIXELS*4,sizeof readback)==0);sky_mtl_texture_destroy(volume);
}

static void test_frame_capture(sky_nvn_device_t *device,sky_nvn_queue_t *queue,sky_nvn_texture_t *texture) {
    const char *names[]={"SKY_FRAME_CAPTURE","SKY_FRAME_CAPTURE_EVERY","SKY_PRESENT_LIMIT"};
    char *saved[3]={0};for(int i=0;i<3;i++){const char *value=getenv(names[i]);if(value)saved[i]=strdup(value);}
    char path[]="/tmp/skyrim-frame-test-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);assert(unlink(path)==0);
    assert(setenv(names[0],path,1)==0);assert(setenv(names[1],"2",1)==0);assert(setenv(names[2],"0",1)==0);
    sky_nvn_window_builder_t builder={.dev=device,.native=(void *)1,.texture_count=1,.textures={texture}};
    sky_nvn_window_t window;assert(sky_nvn_window_initialize(&window,&builder));
    for(int frame=1;frame<=4;frame++) {
        sky_nvn_queue_present_texture(queue,&window,0);sky_nvn_queue_finish(queue);
        if(frame==3){assert(access(path,F_OK)!=0);continue;}
        FILE *file=fopen(path,"rb");assert(file);char line[32];
        assert(fgets(line,sizeof line,file)&&!strcmp(line,"P6\n"));
        assert(fgets(line,sizeof line,file)&&!strcmp(line,"4 4\n"));
        assert(fgets(line,sizeof line,file)&&!strcmp(line,"255\n"));
        uint8_t rgb[TEST_PIXELS*3];assert(fread(rgb,1,sizeof rgb,file)==sizeof rgb);assert(fgetc(file)==EOF);fclose(file);
        for(int y=0;y<TEST_HEIGHT;y++)for(int x=0;x<TEST_WIDTH;x++) {
            const uint8_t expected[3]={(uint8_t)(x*40),(uint8_t)(y*50),100};assert(memcmp(rgb+3*(y*TEST_WIDTH+x),expected,3)==0);
        }
        assert(unlink(path)==0);
    }
    sky_nvn_window_finalize(&window);
    for(int i=0;i<3;i++){if(saved[i]){assert(setenv(names[i],saved[i],1)==0);free(saved[i]);}else assert(unsetenv(names[i])==0);}
}

static void test_boolean_abi(sky_nvn_device_t *device,sky_nvn_queue_t *queue,sky_nvn_texture_t *depth) {
    uint32_t channel=0;sky_nvn_channel_mask_state_set_defaults(&channel);
    sky_nvn_channel_mask_state_set_channel_mask(&channel,0,0xdead0000,0xdead0001,0xbeef00ff,0xdead0100);assert((channel&15)==6);
    uint32_t color=0;sky_nvn_color_state_set_blend_enable(&color,2,0x7a000001);assert(color&4);
    sky_nvn_color_state_set_blend_enable(&color,2,0x7a000000);assert(!(color&4));
    uint64_t ds=0;sky_nvn_depth_stencil_state_set_defaults(&ds);
    sky_nvn_depth_stencil_state_set_depth_test_enable(&ds,0xbeef0001);assert(ds&1);
    sky_nvn_depth_stencil_state_set_depth_test_enable(&ds,0xbeef0000);assert(!(ds&1));
    sky_nvn_depth_stencil_state_set_stencil_test_enable(&ds,0xbeef0001);assert(ds&(1ull<<6));
    sky_nvn_depth_stencil_state_set_stencil_test_enable(&ds,0xbeef0000);assert(!(ds&(1ull<<6)));
    sky_nvn_depth_stencil_state_set_depth_write_enable(&ds,0xbeef0001);assert(ds&2);
    sky_nvn_depth_stencil_state_set_depth_write_enable(&ds,0xbeef0000);assert(!(ds&2));
    sky_nvn_command_buffer_t command={0};assert(sky_nvn_command_buffer_initialize(&command,device));
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_depth_bounds(&command,0xbeef0000,.25f,.75f);
    sky_nvn_command_buffer_set_render_targets(&command,0,NULL,NULL,depth,NULL);
    sky_nvn_command_buffer_clear_depth_stencil(&command,.25f,0xbeef0001,0,0);
    uint64_t handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    float values[TEST_PIXELS];assert(sky_mtl_texture_readback(depth->metal,0,0,SKY_MTL_READ_DEPTH,values,sizeof values,NULL));
    for(int i=0;i<TEST_PIXELS;i++)assert(values[i]==.25f);
    sky_nvn_command_buffer_finalize(&command);
}

static void test_depth_only_clear(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,
                                  sky_nvn_queue_t *queue,sky_nvn_texture_t *combined) {
    sky_nvn_command_buffer_t command;assert(sky_nvn_command_buffer_initialize(&command,device));
    for(int format=0;format<2;format++) {
        sky_nvn_texture_t depth;make_texture(&depth,device,memory,format?0x34:0x32,0x380000+format*0x10000);
        sky_nvn_command_buffer_begin_recording(&command);
        sky_nvn_command_buffer_set_render_targets(&command,0,NULL,NULL,&depth,NULL);
        sky_nvn_command_buffer_clear_depth_stencil(&command,.25f,1,0xa5,0xff);
        uint64_t handle=sky_nvn_command_buffer_end_recording(&command),before=queue->submits;
        sky_nvn_queue_submit_commands(queue,1,&handle);assert(queue->submits==before+1);sky_nvn_queue_finish(queue);
        for(int stencil_only=0;stencil_only<2;stencil_only++) {
            if(stencil_only) {
                sky_nvn_command_buffer_begin_recording(&command);
                sky_nvn_command_buffer_set_render_targets(&command,0,NULL,NULL,&depth,NULL);
                sky_nvn_command_buffer_clear_depth_stencil(&command,.875f,0,0x5a,0xff);
                handle=sky_nvn_command_buffer_end_recording(&command);before=queue->submits;
                sky_nvn_queue_submit_commands(queue,1,&handle);assert(queue->submits==before+1);sky_nvn_queue_finish(queue);
            }
            size_t bytes=0;
            if(format) {
                float pixels[TEST_PIXELS];
                assert(sky_mtl_texture_readback(depth.metal,0,0,SKY_MTL_READ_DEPTH,pixels,sizeof pixels,&bytes));
                assert(bytes==sizeof pixels);for(int p=0;p<TEST_PIXELS;p++)assert(pixels[p]==.25f);
            } else {
                uint16_t pixels[TEST_PIXELS];
                assert(sky_mtl_texture_readback(depth.metal,0,0,SKY_MTL_READ_DEPTH,pixels,sizeof pixels,&bytes));
                assert(bytes==sizeof pixels);for(int p=0;p<TEST_PIXELS;p++)assert(abs((int)pixels[p]-16384)<=1);
            }
        }
        sky_nvn_texture_finalize(&depth);
    }
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,0,NULL,NULL,combined,NULL);
    sky_nvn_command_buffer_clear_depth_stencil(&command,.625f,1,0x6b,0xff);
    uint64_t handle=sky_nvn_command_buffer_end_recording(&command),before=queue->submits;
    sky_nvn_queue_submit_commands(queue,1,&handle);assert(queue->submits==before+1);sky_nvn_queue_finish(queue);
    float depth[TEST_PIXELS];uint8_t stencil[TEST_PIXELS];
    assert(sky_mtl_texture_readback(combined->metal,0,0,SKY_MTL_READ_DEPTH,depth,sizeof depth,NULL));
    assert(sky_mtl_texture_readback(combined->metal,0,0,SKY_MTL_READ_STENCIL,stencil,sizeof stencil,NULL));
    for(int p=0;p<TEST_PIXELS;p++)assert(depth[p]==.625f&&stencil[p]==0x6b);
    sky_nvn_texture_t invalid;make_texture(&invalid,device,memory,0x25,0x3a0000);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,0,NULL,NULL,&invalid,NULL);
    sky_nvn_command_buffer_clear_depth_stencil(&command,.25f,1,0,0xff);
    expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
    sky_nvn_command_buffer_finalize(&command);sky_nvn_texture_finalize(&invalid);
    puts("NVN_DEPTH_ONLY_CLEAR_GPU_OK depth16-depth32-absent-stencil-inert-combined-aspects-preserved-invalid-format-rejected");
}

typedef struct {sky_nvn_memory_pool_t *memory;unsigned calls;} retire_refill_t;
static void retire_source_on_refill(void *command_buffer,int event,uint64_t size,void *data) {
    retire_refill_t *refill=data;assert(event==0&&size<=256&&refill->calls++==0);
    sky_nvn_command_buffer_add_command_memory(command_buffer,refill->memory,0x3b0000,256);
}
static void test_called_handle_refill_retirement(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,
                                                sky_nvn_queue_t *queue) {
    for(int payload=0;payload<2;payload++) {
        sky_nvn_command_buffer_t command;assert(sky_nvn_command_buffer_initialize(&command,device));
        sky_nvn_command_buffer_add_command_memory(&command,memory,0x3b0000,256);
        sky_nvn_command_buffer_begin_recording(&command);
        if(payload) {
            uint32_t bytes[8]={1,2,3,4,5,6,7,8};
            sky_nvn_command_buffer_update_uniform_buffer(&command,memory->buffer_address+0x3c0000,sizeof bytes,0,sizeof bytes,bytes);
        }
        uint64_t source=sky_nvn_command_buffer_end_recording(&command);
        sky_nvn_command_buffer_add_command_memory(&command,memory,0x3b1000,payload?64:32);
        retire_refill_t refill={memory,0};
        sky_nvn_command_buffer_set_memory_callback(&command,retire_source_on_refill);
        sky_nvn_command_buffer_set_memory_callback_data(&command,&refill);
        sky_nvn_command_buffer_begin_recording(&command);
        sky_nvn_command_buffer_call_commands(&command,1,&source);
        assert(refill.calls==1);
        expect_rejected(queue,sky_nvn_command_buffer_end_recording(&command));
        sky_nvn_command_buffer_finalize(&command);
    }
    puts("NVN_CALLED_HANDLE_REFILL_RETIREMENT_OK command-reference-and-inline-payload-callbacks");
}

static uint64_t pacing_now_ns(void) {
    struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
typedef struct {
    sky_nvn_queue_t *queue;sky_nvn_window_t *window;
    pthread_mutex_t lock;pthread_cond_t ready;
    int started,done;
} paced_present_t;
static void *paced_present(void *context) {
    paced_present_t *p=context;
    pthread_mutex_lock(&p->lock);p->started=1;pthread_cond_signal(&p->ready);pthread_mutex_unlock(&p->lock);
    sky_nvn_queue_present_texture(p->queue,p->window,0);
    __atomic_store_n(&p->done,1,__ATOMIC_RELEASE);return NULL;
}
static void wait_for_paced_present(sky_nvn_window_t *window) {
    uint64_t deadline=pacing_now_ns()+1000000000;
    while(!sky_nvn_window_present_waiters_for_testing(window)) {
        assert(pacing_now_ns()<deadline);
        struct timespec delay={0,100000};nanosleep(&delay,NULL);
    }
}
static void test_headless_present_pacing(sky_nvn_device_t *device,sky_nvn_queue_t *queue,
                                         sky_nvn_texture_t *texture) {
    sky_nvn_window_builder_t builder={0};
    sky_nvn_window_builder_set_device(&builder,device);sky_nvn_window_builder_set_defaults(&builder);
    sky_nvn_window_builder_set_native_window(&builder,(void *)1);
    void *textures[]={texture};sky_nvn_window_builder_set_textures(&builder,1,textures);
    sky_nvn_window_builder_set_present_interval(&builder,2);
    sky_nvn_window_t first,second;assert(sky_nvn_window_initialize(&first,&builder));
    sky_nvn_window_builder_set_present_interval(&builder,5);assert(sky_nvn_window_initialize(&second,&builder));
    uint64_t times[4],paced_start=pacing_now_ns();
    for(int i=0;i<4;i++){sky_nvn_queue_present_texture(queue,&first,0);times[i]=sky_nvn_window_last_present_ns_for_testing(&first);}
    for(int i=1;i<4;i++)assert(times[i]-times[i-1]>=30000000);
    assert(pacing_now_ns()-paced_start>=99000000);
    sky_nvn_window_set_present_interval(&first,1);
    sky_nvn_queue_present_texture(queue,&first,0);uint64_t changed=sky_nvn_window_last_present_ns_for_testing(&first);
    assert(changed-times[3]>=14000000);
    sky_nvn_window_set_present_interval(&first,2);
    sky_nvn_queue_present_texture(queue,&first,0);assert(sky_nvn_window_last_present_ns_for_testing(&first)-changed>=30000000);

    /* Interval zero submits immediately instead of inheriting an old deadline. */
    sky_nvn_window_set_present_interval(&first,0);uint64_t start=pacing_now_ns();
    for(int i=0;i<20;i++)sky_nvn_queue_present_texture(queue,&first,0);
    sky_nvn_queue_finish(queue);uint64_t immediate_ns=pacing_now_ns()-start;assert(immediate_ns<150000000);
    sky_nvn_window_set_present_interval(&first,5);sky_nvn_queue_present_texture(queue,&first,0);
    /* The fresh second window has no previous present; the first window's
     * deadline must not delay it, even though they share one GPU queue. */
    start=pacing_now_ns();sky_nvn_queue_present_texture(queue,&second,0);
    uint64_t independent_ns=pacing_now_ns()-start;assert(independent_ns<70000000);
    sky_nvn_queue_finish(queue);

    sky_nvn_queue_present_texture(queue,&first,0);
    sky_nvn_queue_finish(queue);
    paced_present_t job={.queue=queue,.window=&first,.lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};
    pthread_t thread;assert(pthread_create(&thread,NULL,paced_present,&job)==0);
    pthread_mutex_lock(&job.lock);while(!job.started)pthread_cond_wait(&job.ready,&job.lock);pthread_mutex_unlock(&job.lock);
    wait_for_paced_present(&first);
    assert(pthread_mutex_trylock(&queue->lock)==0);pthread_mutex_unlock(&queue->lock);
    /* A paced producer must leave both queue and renderer locks available.
     * Real GPU work can finish while that window is waiting for its next slot. */
    sky_nvn_command_buffer_t command;assert(sky_nvn_command_buffer_initialize(&command,device));
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,textures,NULL,NULL,NULL);
    sky_nvn_command_buffer_clear_color(&command,0,(float[]){1,0,1,1},15);
    uint64_t handle=sky_nvn_command_buffer_end_recording(&command);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    sky_nvn_window_set_present_interval(&first,0);
    assert(pthread_join(thread,NULL)==0);sky_nvn_queue_finish(queue);
    assert(__atomic_load_n(&job.done,__ATOMIC_ACQUIRE)&&first.presents==29&&second.presents==1);
    expect_color(texture,(uint8_t[]){255,0,255,255});
    /* Finalization cancels and drains a producer already inside the pacer.
     * Cancellation must not count a GPU submission or poison its queue. */
    sky_nvn_queue_present_texture(queue,&second,0);sky_nvn_queue_finish(queue);
    job.window=&second;job.started=job.done=0;
    assert(pthread_create(&thread,NULL,paced_present,&job)==0);
    wait_for_paced_present(&second);pthread_mutex_lock(&queue->lock);uint64_t before=queue->submits;pthread_mutex_unlock(&queue->lock);
    sky_nvn_window_finalize(&second);
    assert(pthread_join(thread,NULL)==0&&second.magic==0);
    pthread_mutex_lock(&queue->lock);assert(queue->submits==before);pthread_mutex_unlock(&queue->lock);
    assert(sky_nvn_queue_get_error(queue,NULL)==0);
    pthread_cond_destroy(&job.ready);pthread_mutex_destroy(&job.lock);
    sky_nvn_command_buffer_finalize(&command);
    sky_nvn_window_finalize(&first);sky_nvn_window_finalize(&second);
    uint64_t minimum=times[1]-times[0];
    for(int i=2;i<4;i++)if(times[i]-times[i-1]<minimum)minimum=times[i]-times[i-1];
    printf("NVN_HEADLESS_PACING_TIMING interval2_min_ns=%llu immediate20_ns=%llu independent_first_ns=%llu\n",
        (unsigned long long)minimum,(unsigned long long)immediate_ns,(unsigned long long)independent_ns);
    puts("NVN_HEADLESS_PACING_OK monotonic-intervals-immediate-zero-window-independence-and-unblocked-GPU-completion");
}

static void begin_polygon_depth_case(sky_nvn_command_buffer_t *command,sky_nvn_program_t *program,
    sky_nvn_memory_pool_t *memory,sky_nvn_texture_t *color,sky_nvn_texture_t *depth,int enables) {
    sky_nvn_command_buffer_begin_recording(command);void *colors[]={color};
    sky_nvn_command_buffer_set_render_targets(command,1,colors,NULL,depth,NULL);
    sky_nvn_command_buffer_clear_depth_stencil(command,1,1,0,0);
    sky_nvn_command_buffer_bind_program(command,program,3);sky_nvn_command_buffer_set_viewport(command,0,0,TEST_WIDTH,TEST_HEIGHT);
    sky_nvn_command_buffer_set_scissor(command,0,0,TEST_WIDTH,TEST_HEIGHT);sky_nvn_command_buffer_set_depth_range(command,0,1);
    uint32_t attributes[2]={0};for(int i=0;i<2;i++)sky_nvn_vertex_attrib_state_set_format(attributes+i,0x2e,i*16);
    uint32_t stream[2]={32,0};sky_nvn_command_buffer_bind_vertex_attrib_state(command,2,attributes);
    sky_nvn_command_buffer_bind_vertex_stream_state(command,1,stream);
    sky_nvn_command_buffer_bind_vertex_buffer(command,0,memory->buffer_address+0x1b0000,128);
    sky_nvn_command_buffer_bind_uniform_buffer(command,1,0,memory->buffer_address+0x1b1000,16);
    uint64_t ds=0;sky_nvn_depth_stencil_state_set_defaults(&ds);sky_nvn_depth_stencil_state_set_depth_test_enable(&ds,1);
    sky_nvn_depth_stencil_state_set_depth_write_enable(&ds,1);sky_nvn_depth_stencil_state_set_depth_func(&ds,8);
    sky_nvn_command_buffer_bind_depth_stencil_state(command,&ds);
    uint32_t polygon=0;sky_nvn_polygon_state_set_defaults(&polygon);sky_nvn_polygon_state_set_polygon_offset_enables(&polygon,enables);
    sky_nvn_command_buffer_bind_polygon_state(command,&polygon);
    uint32_t channel=UINT32_MAX,color_state=0;sky_nvn_command_buffer_bind_channel_mask_state(command,&channel);
    sky_nvn_command_buffer_bind_color_state(command,&color_state);
}
static void expect_polygon_depth(sky_nvn_texture_t *depth,float base,float x_step,float offset) {
    uint16_t values[TEST_PIXELS];assert(sky_mtl_texture_readback(depth->metal,0,0,SKY_MTL_READ_DEPTH,values,sizeof values,NULL));
    for(int y=0;y<TEST_HEIGHT;y++)for(int x=0;x<TEST_WIDTH;x++) {
        int expected=(int)((base+(x+.5f)*x_step+offset)*65535.f+.5f);
        int difference=(int)values[y*TEST_WIDTH+x]-expected;
        if(difference < -2 || difference > 2)fprintf(stderr,"POLYGON_DEPTH_MISMATCH x=%d y=%d actual=%u expected=%d\n",x,y,values[y*TEST_WIDTH+x],expected);
        assert(difference>=-2&&difference<=2);
    }
}
static void submit_polygon_depth(sky_nvn_queue_t *queue,uint64_t handle,sky_nvn_texture_t *depth,float base,float x_step,float offset) {
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);assert(sky_nvn_queue_get_error(queue,NULL)==0);
    expect_polygon_depth(depth,base,x_step,offset);
}
static void test_polygon_offset(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,
    sky_nvn_queue_t *queue,sky_nvn_texture_t *color) {
    const uint64_t serial=0x100000005ull;
    install_fixture(serial,0,"position_uv");
    install_fixture(serial,1,"uniform_color");
    sky_nvn_program_t program={.magic=0x474f5250,.dev=device,.stage_count=2,.stage_mask=3,.serial=serial};
    sky_nvn_texture_t depth;make_texture(&depth,device,memory,0x32,0x1c0000);
    uint8_t *cpu=memory->mem;float vertices[4][8]={
        {-1,-1,.25f,1,.5f,.5f,0,0},{1,-1,.25f,1,.5f,.5f,0,0},
        {-1,1,.25f,1,.5f,.5f,0,0},{1,1,.25f,1,.5f,.5f,0,0}};
    memcpy(cpu+0x1b0000,vertices,sizeof vertices);memcpy(cpu+0x1b1000,(float[]){0,1,0,1},16);
    sky_nvn_command_buffer_t command={0};assert(sky_nvn_command_buffer_initialize(&command,device));
    void (*offset_abi)(void *,float,float,float)=sky_nvn_bootstrap_loader("nvnCommandBufferSetPolygonOffsetClamp");
    assert(offset_abi==sky_nvn_command_buffer_set_polygon_offset_clamp);
    begin_polygon_depth_case(&command,&program,memory,color,&depth,4);offset_abi(&command,0,32768,.125f);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);uint64_t biased=sky_nvn_command_buffer_end_recording(&command);
    begin_polygon_depth_case(&command,&program,memory,color,&depth,4);offset_abi(&command,0,0,0);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);uint64_t unbiased=sky_nvn_command_buffer_end_recording(&command);
    /* A later recording must not overwrite the earlier bias state. */
    submit_polygon_depth(queue,biased,&depth,.25f,0,.125f);submit_polygon_depth(queue,unbiased,&depth,.25f,0,0);
    begin_polygon_depth_case(&command,&program,memory,color,&depth,2);offset_abi(&command,0,32768,.125f);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);uint64_t handle=sky_nvn_command_buffer_end_recording(&command);
    submit_polygon_depth(queue,handle,&depth,.25f,0,0); /* Line-only offset does not affect filled triangles. */
    begin_polygon_depth_case(&command,&program,memory,color,&depth,4);offset_abi(&command,0,16,0);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);handle=sky_nvn_command_buffer_end_recording(&command);
    submit_polygon_depth(queue,handle,&depth,.25f,0,16.f/65536.f);
    for(int i=0;i<4;i++)vertices[i][2]=vertices[i][0]<0?.25f:.75f;memcpy(cpu+0x1b0000,vertices,sizeof vertices);
    sky_nvn_command_buffer_t setup={0};assert(sky_nvn_command_buffer_initialize(&setup,device));
    sky_nvn_command_buffer_begin_recording(&setup);offset_abi(&setup,.5f,0,.125f);
    uint64_t setup_handle=sky_nvn_command_buffer_end_recording(&setup);
    begin_polygon_depth_case(&command,&program,memory,color,&depth,4);sky_nvn_command_buffer_call_commands(&command,1,&setup_handle);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);handle=sky_nvn_command_buffer_end_recording(&command);
    submit_polygon_depth(queue,handle,&depth,.25f,.125f,.0625f);
    sky_nvn_command_buffer_begin_recording(&setup);offset_abi(&setup,2,0,.03125f);
    setup_handle=sky_nvn_command_buffer_end_recording(&setup);sky_nvn_queue_submit_commands(queue,1,&setup_handle);
    begin_polygon_depth_case(&command,&program,memory,color,&depth,4);sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    handle=sky_nvn_command_buffer_end_recording(&command);submit_polygon_depth(queue,handle,&depth,.25f,.125f,.03125f);
    begin_polygon_depth_case(&command,&program,memory,color,&depth,0);sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    handle=sky_nvn_command_buffer_end_recording(&command);submit_polygon_depth(queue,handle,&depth,.25f,.125f,0);
    sky_nvn_command_buffer_finalize(&setup);sky_nvn_command_buffer_finalize(&command);sky_nvn_texture_finalize(&depth);sky_mtl_program_remove(serial);
    puts("NVN_POLYGON_OFFSET_GPU_OK float-abi-units-slope-clamp-enable-immutable-nested-and-split-state");
}

static void test_shader_draws(sky_nvn_device_t *device,sky_nvn_memory_pool_t *memory,
    sky_nvn_queue_t *queue,sky_nvn_texture_t *target) {
    const uint64_t uniform_serial=0x100000001ull,textured_serial=0x100000002ull;
    install_fixture(uniform_serial,0,"vertex_id_quad");
    install_fixture(uniform_serial,1,"uniform_color");
    install_fixture(textured_serial,0,"position_uv");
    install_fixture(textured_serial,1,"sample_texture");
    sky_nvn_program_t uniform={.magic=0x474f5250,.dev=device,.stage_count=2,.stage_mask=3,.serial=uniform_serial};
    sky_nvn_program_t textured=uniform;textured.serial=textured_serial;
    uint8_t *cpu=sky_nvn_memory_pool_map(memory);
    const float green[4]={0,1,0,1};memcpy(cpu+0x110000,green,sizeof green);
    sky_nvn_command_buffer_t command={0};assert(sky_nvn_command_buffer_initialize(&command,device));
    void *targets[]={target};
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x200000,65536);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&uniform,3);
    void (*set_viewport_abi)(void *,int,int,int,int)=sky_nvn_bootstrap_loader("nvnCommandBufferSetViewport");
    set_viewport_abi(&command,0,0,TEST_WIDTH,TEST_HEIGHT);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,1,0,memory->buffer_address+0x110000,16);
    /* The source handle is called and copied below: its explicit barrier must
     * survive immutable nested recording and replay with both entry paths. */
    sky_nvn_command_buffer_barrier(&command,0x1f);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    uint64_t original=sky_nvn_command_buffer_end_recording(&command);assert(original);
    sky_mtl_program_remove(uniform_serial);
    /* Re-recording the command object must not overwrite a previous handle. */
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    const float red[4]={1,0,0,1};sky_nvn_command_buffer_clear_color(&command,0,red,15);
    uint64_t clear=sky_nvn_command_buffer_end_recording(&command);assert(clear&&clear!=original);
    sky_nvn_queue_submit_commands(queue,1,&original);sky_nvn_queue_finish(queue);
    const uint8_t green_bytes[4]={0,255,0,255};expect_color(target,green_bytes);
    sky_nvn_queue_submit_commands(queue,1,&clear);sky_nvn_queue_finish(queue);
    const uint8_t red_bytes[4]={255,0,0,255};expect_color(target,red_bytes);
    sky_nvn_command_buffer_t called={0};assert(sky_nvn_command_buffer_initialize(&called,device));
    sky_nvn_command_buffer_begin_recording(&called);
    sky_nvn_command_buffer_call_commands(&called,1,&original);
    sky_nvn_command_buffer_copy_commands(&called,1,&clear);
    uint64_t copied=sky_nvn_command_buffer_end_recording(&called);assert(copied);
    /* Reusing command memory invalidates its old records, while commands copied
     * into an independent recording retain every shader/resource they need. */
    sky_nvn_command_buffer_add_command_memory(&command,memory,0x200000,65536);
    expect_rejected(queue,original);sky_nvn_command_buffer_finalize(&command);
    sky_nvn_queue_submit_commands(queue,1,&copied);sky_nvn_queue_finish(queue);expect_color(target,red_bytes);
    sky_nvn_command_buffer_finalize(&called);assert(sky_nvn_command_buffer_initialize(&command,device));

    sky_nvn_texture_t sampled;make_texture(&sampled,device,memory,0x25,0x40000);
    uint8_t pixels[TEST_PIXELS*4];for(int i=0;i<TEST_PIXELS;i++)memcpy(pixels+i*4,(uint8_t[]){255,0,255,255},4);
    const sky_nvn_copy_region_t region={0,0,0,TEST_WIDTH,TEST_HEIGHT,1};
    sky_nvn_texture_write_texels(&sampled,NULL,&region,pixels);
    sky_nvn_texture_pool_t texture_pool;assert(sky_nvn_texture_pool_initialize(&texture_pool,memory,0,512));
    sky_nvn_texture_pool_register_texture(&texture_pool,256,&sampled,NULL);
    sky_nvn_sampler_pool_t sampler_pool;assert(sky_nvn_sampler_pool_initialize(&sampler_pool,memory,0x5000,512));
    sky_nvn_sampler_builder_t sampler={0};sky_nvn_sampler_builder_set_device(&sampler,device);
    sky_nvn_sampler_builder_set_defaults(&sampler);sky_nvn_sampler_pool_register_sampler_builder(&sampler_pool,256,&sampler);
    const float vertices[][8]={
        {0,0,0,0,0,0,0,0},
        {-1,-1,0,1,.5,.5,0,0},{1,-1,0,1,.5,.5,0,0},
        {-1,1,0,1,.5,.5,0,0},{1,1,0,1,.5,.5,0,0}};
    memcpy(cpu+0x100000,vertices,sizeof vertices);
    const uint16_t indices[]={0,1,2,3};memcpy(cpu+0x120000,indices,sizeof indices);
    uint32_t attributes[2]={0};
    for(int i=0;i<2;i++) {
        sky_nvn_vertex_attrib_state_set_defaults(&attributes[i]);
        sky_nvn_vertex_attrib_state_set_format(&attributes[i],0x2e,i*16);
        sky_nvn_vertex_attrib_state_set_stream_index(&attributes[i],0);
    }
    uint32_t stream[2]={32,0};
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&textured,3);
    sky_nvn_command_buffer_bind_vertex_attrib_state(&command,2,attributes);
    sky_nvn_command_buffer_bind_vertex_stream_state(&command,1,stream);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,0,memory->buffer_address+0x100000,sizeof vertices);
    sky_nvn_command_buffer_set_texture_pool(&command,&texture_pool);
    sky_nvn_command_buffer_set_sampler_pool(&command,&sampler_pool);
    sky_nvn_command_buffer_bind_texture(&command,1,0,sky_nvn_device_get_texture_handle(device,256,256));
    /* Test clipping a scissor that extends beyond the render target. */
    sky_nvn_command_buffer_set_scissor(&command,-2,-2,TEST_WIDTH+4,TEST_HEIGHT+4);
    sky_nvn_command_buffer_draw_elements_base_vertex(&command,5,1,4,memory->buffer_address+0x120000,1);
    uint64_t handle=sky_nvn_command_buffer_end_recording(&command);assert(handle);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    const uint8_t magenta[4]={255,0,255,255};expect_color(target,magenta);
    /* Each texel differs so the compute check detects wrong source coordinates. */
    for(int y=0;y<TEST_HEIGHT;y++)for(int x=0;x<TEST_WIDTH;x++) {
        const uint8_t rgba[4]={(uint8_t)(x*40),(uint8_t)(y*50),100,255};
        memcpy(pixels+4*(y*TEST_WIDTH+x),rgba,4);
    }
    sky_nvn_texture_write_texels(&sampled,NULL,&region,pixels);
    const uint64_t compute_serial=0x100000003ull;
    install_fixture(compute_serial,5,"copy_texture");
    sky_nvn_program_t compute=uniform;compute.serial=compute_serial;compute.stage_count=1;compute.stage_mask=1u<<5;
    sky_nvn_texture_builder_t writable_builder={0};sky_nvn_texture_builder_set_device(&writable_builder,device);
    sky_nvn_texture_builder_set_defaults(&writable_builder);sky_nvn_texture_builder_set_format(&writable_builder,0x25);
    sky_nvn_texture_builder_set_size_2d(&writable_builder,TEST_WIDTH,TEST_HEIGHT);
    sky_nvn_texture_builder_set_flags(&writable_builder,0xd);sky_nvn_texture_builder_set_storage(&writable_builder,memory,0x60000);
    sky_nvn_texture_t writable;assert(sky_nvn_texture_initialize(&writable,&writable_builder));
    sky_nvn_texture_pool_register_image(&texture_pool,0,&writable,NULL);
    sky_nvn_sampler_pool_register_sampler_builder(&sampler_pool,0,&sampler);
    const uint32_t copy_parameters[]={0,0,TEST_WIDTH,TEST_HEIGHT,0,0,0,0};memcpy(cpu+0x110100,copy_parameters,sizeof copy_parameters);
    sky_nvn_sync_t before_compute,after_compute;
    assert(sky_nvn_sync_initialize(&before_compute,device));assert(sky_nvn_sync_initialize(&after_compute,device));
    assert(sky_nvn_queue_submit_empty_for_testing(queue,20000000));
    sky_nvn_queue_fence_sync(queue,&before_compute,0,0);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_wait_sync(&command,&before_compute);
    sky_nvn_command_buffer_bind_program(&command,&compute,1u<<5);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,5,0,memory->buffer_address+0x110100,sizeof copy_parameters);
    sky_nvn_command_buffer_set_texture_pool(&command,&texture_pool);sky_nvn_command_buffer_set_sampler_pool(&command,&sampler_pool);
    sky_nvn_command_buffer_bind_texture(&command,5,0,sky_nvn_device_get_texture_handle(device,256,0));
    sky_nvn_command_buffer_bind_image(&command,5,0,sky_nvn_device_get_image_handle(device,0));
    sky_nvn_command_buffer_dispatch_compute(&command,1,1,1);
    sky_nvn_command_buffer_fence_sync(&command,&after_compute,0,0);
    sky_nvn_command_buffer_wait_sync(&command,&after_compute);
    handle=sky_nvn_command_buffer_end_recording(&command);assert(handle);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    assert(sky_nvn_sync_wait(&after_compute,0)==0);
    uint8_t copied_pixels[sizeof pixels];
    assert(sky_mtl_texture_readback(writable.metal,0,0,SKY_MTL_READ_COLOR,copied_pixels,sizeof copied_pixels,NULL));
    assert(memcmp(copied_pixels,pixels,sizeof pixels)==0);
    sky_nvn_sync_finalize(&before_compute);sky_nvn_sync_finalize(&after_compute);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_clear_texture(&command,target,NULL,&region,red,15);
    const sky_nvn_copy_region_t middle={1,1,0,2,2,1};
    sky_nvn_command_buffer_clear_texture(&command,target,NULL,&middle,green,2);
    handle=sky_nvn_command_buffer_end_recording(&command);assert(handle);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    uint8_t cleared[TEST_PIXELS*4];assert(sky_mtl_texture_readback(target->metal,0,0,SKY_MTL_READ_COLOR,cleared,sizeof cleared,NULL));
    for(int y=0;y<TEST_HEIGHT;y++)for(int x=0;x<TEST_WIDTH;x++) {
        const uint8_t expected[4]={255,(x>=1&&x<3&&y>=1&&y<3)?255:0,0,255};
        assert(memcmp(cleared+4*(y*TEST_WIDTH+x),expected,4)==0);
    }
    install_fixture(uniform_serial,0,"vertex_id_quad");
    install_fixture(uniform_serial,1,"uniform_color");
    /* A setup-only nested handle must change bindings for its caller's draw. */
    sky_nvn_command_buffer_t setup={0};assert(sky_nvn_command_buffer_initialize(&setup,device));
    sky_nvn_command_buffer_begin_recording(&setup);
    sky_nvn_command_buffer_set_render_targets(&setup,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&setup,&uniform,3);
    sky_nvn_command_buffer_bind_uniform_buffer(&setup,1,0,memory->buffer_address+0x110000,16);
    uint64_t setup_handle=sky_nvn_command_buffer_end_recording(&setup);assert(setup_handle);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_call_commands(&command,1,&setup_handle);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    handle=sky_nvn_command_buffer_end_recording(&command);assert(handle);
    sky_nvn_command_buffer_finalize(&setup);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);expect_color(target,green_bytes);
    /* Queue state must also carry across separately submitted command handles. */
    const float blue_color[4]={0,0,1,1};memcpy(cpu+0x110200,blue_color,16);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,1,0,memory->buffer_address+0x110200,16);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);
    sky_nvn_command_buffer_begin_recording(&command);sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    const uint8_t blue_bytes[4]={0,0,255,255};expect_color(target,blue_bytes);
    sky_nvn_command_buffer_begin_recording(&command);
    *(uint64_t *)(cpu+0x130000)=1;
    sky_nvn_command_buffer_reset_counter(&command,2);sky_nvn_command_buffer_reset_counter(&command,3);
    sky_nvn_command_buffer_report_counter(&command,0,memory->buffer_address+0x130000);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    sky_nvn_command_buffer_report_counter(&command,2,memory->buffer_address+0x130010);
    sky_nvn_command_buffer_report_counter(&command,3,memory->buffer_address+0x130020);
    sky_nvn_command_buffer_report_value(&command,0xabcddcba,memory->buffer_address+0x130030);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    const uint64_t *timestamp=(const uint64_t *)(cpu+0x130000);
    assert(timestamp[0]==0&&timestamp[1]>0);assert(sky_nvn_device_get_timestamp_in_nanoseconds(device,timestamp)>0);
    assert(*(const uint64_t *)(cpu+0x130010)==4&&*(const uint64_t *)(cpu+0x130020)==2);
    assert(*(const uint32_t *)(cpu+0x130030)==0xabcddcba);
    sky_nvn_command_buffer_t draw_only={0};assert(sky_nvn_command_buffer_initialize(&draw_only,device));
    sky_nvn_command_buffer_begin_recording(&draw_only);sky_nvn_command_buffer_draw_arrays(&draw_only,5,0,4);
    uint64_t draw_only_handle=sky_nvn_command_buffer_end_recording(&draw_only);assert(draw_only_handle);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&uniform,3);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,1,0,memory->buffer_address+0x110000,16);
    sky_nvn_command_buffer_call_commands(&command,1,&draw_only_handle);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);expect_color(target,green_bytes);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,1,0,memory->buffer_address+0x110200,16);
    sky_nvn_command_buffer_call_commands(&command,1,&draw_only_handle);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);expect_color(target,blue_bytes);
    sky_nvn_command_buffer_finalize(&draw_only);
    sky_nvn_texture_t test_depth;make_texture(&test_depth,device,memory,0x34,0x70000);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,&test_depth,NULL);
    sky_nvn_command_buffer_clear_depth_stencil(&command,.25f,1,0,0);
    uint64_t depth_state=0;sky_nvn_depth_stencil_state_set_defaults(&depth_state);
    sky_nvn_depth_stencil_state_set_depth_test_enable(&depth_state,0);sky_nvn_depth_stencil_state_set_depth_write_enable(&depth_state,1);
    sky_nvn_command_buffer_bind_depth_stencil_state(&command,&depth_state);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    float depth_values[TEST_PIXELS];assert(sky_mtl_texture_readback(test_depth.metal,0,0,SKY_MTL_READ_DEPTH,depth_values,sizeof depth_values,NULL));
    for(int i=0;i<TEST_PIXELS;i++)assert(depth_values[i]==.25f);
    test_cross_queue_wait(device,memory,queue);
    uint8_t *upload=cpu+0x140000,*download=cpu+0x150000;memset(upload,0xee,128);memset(download,0xcd,192);
    for(int y=0;y<TEST_HEIGHT;y++)for(int x=0;x<TEST_WIDTH;x++) {
        const uint8_t rgba[4]={(uint8_t)(x*40),(uint8_t)(y*50),100,255};memcpy(upload+y*32+x*4,rgba,4);
    }
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_copy_row_stride(&command,32);sky_nvn_command_buffer_set_copy_image_stride(&command,128);
    sky_nvn_command_buffer_copy_buffer_to_texture(&command,memory->buffer_address+0x140000,&sampled,NULL,&region,0);
    sky_nvn_command_buffer_copy_texture_to_texture(&command,&sampled,NULL,&region,&writable,NULL,&region,0);
    sky_nvn_command_buffer_set_copy_row_stride(&command,48);sky_nvn_command_buffer_set_copy_image_stride(&command,192);
    sky_nvn_command_buffer_copy_texture_to_buffer(&command,&writable,NULL,&region,memory->buffer_address+0x150000,0);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    for(int y=0;y<TEST_HEIGHT;y++) {
        assert(memcmp(upload+y*32,download+y*48,TEST_WIDTH*4)==0);
        for(int padding=TEST_WIDTH*4;padding<48;padding++)assert(download[y*48+padding]==0xcd);
    }
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_bind_program(&command,&uniform,3);
    sky_nvn_command_buffer_reset_counter(&command,1);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    sky_nvn_command_buffer_report_counter(&command,1,memory->buffer_address+0x130060);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    sky_nvn_command_buffer_report_counter(&command,1,memory->buffer_address+0x130070);
    sky_nvn_command_buffer_reset_counter(&command,1);
    sky_nvn_command_buffer_report_counter(&command,1,memory->buffer_address+0x130080);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    assert(*(uint64_t *)(cpu+0x130060)==TEST_PIXELS);
    assert(*(uint64_t *)(cpu+0x130070)==TEST_PIXELS*2);
    assert(*(uint64_t *)(cpu+0x130080)==0);
    memset(cpu+0x180000,0xa5,64);float updated_color[4]={.25f,.5f,.75f,1};
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_update_uniform_buffer(&command,memory->buffer_address+0x180000,64,16,16,updated_color);
    memset(updated_color,0,sizeof updated_color);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&uniform,3);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,1,0,memory->buffer_address+0x180010,16);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);
    uint64_t update_handle=sky_nvn_command_buffer_end_recording(&command);assert(update_handle);
    sky_nvn_command_buffer_t update_copy={0};assert(sky_nvn_command_buffer_initialize(&update_copy,device));
    sky_nvn_command_buffer_begin_recording(&update_copy);sky_nvn_command_buffer_copy_commands(&update_copy,1,&update_handle);
    handle=sky_nvn_command_buffer_end_recording(&update_copy);sky_nvn_device_finalize_command_handle(device,update_handle);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    const uint8_t updated_rgba[4]={64,128,191,255};expect_color(target,updated_rgba);
    const float expected_update[4]={.25f,.5f,.75f,1};assert(memcmp(cpu+0x180010,expected_update,16)==0);
    for(int i=0;i<16;i++)assert(cpu[0x180000+i]==0xa5);for(int i=32;i<64;i++)assert(cpu[0x180000+i]==0xa5);
    sky_nvn_command_buffer_finalize(&update_copy);
    const int16_t short_positions[4][2]={{-1,-1},{1,-1},{-1,1},{1,1}};memcpy(cpu+0x190000,short_positions,sizeof short_positions);
    const uint8_t constant_uv[4]={128,128,0,255};memcpy(cpu+0x190100,constant_uv,sizeof constant_uv);
    uint32_t short_attributes[2]={0};sky_nvn_vertex_attrib_state_set_format(&short_attributes[0],0x5a,0);
    sky_nvn_vertex_attrib_state_set_format(&short_attributes[1],0x25,0);sky_nvn_vertex_attrib_state_set_stream_index(&short_attributes[1],1);
    const uint32_t short_streams[4]={4,0,0,0};
    sky_nvn_command_buffer_begin_recording(&command);sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&textured,3);sky_nvn_command_buffer_bind_vertex_attrib_state(&command,2,short_attributes);
    sky_nvn_command_buffer_bind_vertex_stream_state(&command,2,short_streams);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,0,memory->buffer_address+0x190000,sizeof short_positions);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,1,memory->buffer_address+0x190100,sizeof constant_uv);
    sky_nvn_command_buffer_set_texture_pool(&command,&texture_pool);sky_nvn_command_buffer_set_sampler_pool(&command,&sampler_pool);
    sky_nvn_command_buffer_bind_texture(&command,1,0,sky_nvn_device_get_texture_handle(device,256,256));
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);handle=sky_nvn_command_buffer_end_recording(&command);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    const uint8_t sampled_constant[4]={80,100,100,255};expect_color(target,sampled_constant);
    const uint64_t ui_serial=0x100000004ull;
    install_fixture(ui_serial,0,"vertex_color_transform");
    install_fixture(ui_serial,1,"adjust_vertex_color");
    sky_nvn_program_t ui_program=uniform;ui_program.serial=ui_serial;
    const struct {int16_t xy[2];uint8_t rgba[4];} ui_vertices[4]={
        {{0,0},{51,102,153,255}},{{4,0},{51,102,153,255}},{{0,4},{51,102,153,255}},{{4,4},{51,102,153,255}}};
    const float ui_vertex_constants[2][4]={{.5f,0,0,-1},{0,-.5f,0,1}};
    const float ui_fragment_constants[2][4]={{.1f,0,.05f,0},{.5f,.5f,.5f,1}};
    memcpy(cpu+0x1a0000,ui_vertices,sizeof ui_vertices);memcpy(cpu+0x1a0100,ui_vertex_constants,sizeof ui_vertex_constants);
    memcpy(cpu+0x1a0200,ui_fragment_constants,sizeof ui_fragment_constants);memcpy(cpu+0x1a0300,(uint8_t[]){204,153,102,255},4);
    uint32_t ui_attributes[3]={0};sky_nvn_vertex_attrib_state_set_format(&ui_attributes[0],0x5a,0);
    sky_nvn_vertex_attrib_state_set_format(&ui_attributes[1],0x25,4);sky_nvn_vertex_attrib_state_set_format(&ui_attributes[2],0x25,0);
    sky_nvn_vertex_attrib_state_set_stream_index(&ui_attributes[2],1);const uint32_t ui_streams[4]={8,0,0,0};
    sky_nvn_command_buffer_begin_recording(&command);sky_nvn_command_buffer_bind_program(&command,&ui_program,3);
    sky_nvn_command_buffer_bind_vertex_attrib_state(&command,3,ui_attributes);sky_nvn_command_buffer_bind_vertex_stream_state(&command,2,ui_streams);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,0,memory->buffer_address+0x1a0000,sizeof ui_vertices);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,1,memory->buffer_address+0x1a0300,4);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,0,0,memory->buffer_address+0x1a0100,sizeof ui_vertex_constants);
    sky_nvn_command_buffer_bind_uniform_buffer(&command,1,0,memory->buffer_address+0x1a0200,sizeof ui_fragment_constants);
    sky_nvn_command_buffer_draw_arrays(&command,5,0,4);handle=sky_nvn_command_buffer_end_recording(&command);
    sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    /* The normalized mask in the constant second stream affects all RGB lanes. */
    const uint8_t ui_expected[4]={46,31,43,255};expect_color(target,ui_expected);
    sky_mtl_program_remove(ui_serial);
    float (*textured_vertices)[8]=(void *)(cpu+0x100000);
    const float uv[4][2]={{0,1},{1,1},{0,0},{1,0}};
    for(int i=0;i<4;i++){textured_vertices[i+1][4]=uv[i][0];textured_vertices[i+1][5]=uv[i][1];}
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&textured,3);
    sky_nvn_command_buffer_bind_vertex_attrib_state(&command,2,attributes);sky_nvn_command_buffer_bind_vertex_stream_state(&command,1,stream);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,0,memory->buffer_address+0x100000,sizeof vertices);
    sky_nvn_command_buffer_set_texture_pool(&command,&texture_pool);sky_nvn_command_buffer_set_sampler_pool(&command,&sampler_pool);
    sky_nvn_command_buffer_bind_texture(&command,1,0,sky_nvn_device_get_texture_handle(device,256,256));
    sky_nvn_command_buffer_draw_arrays(&command,5,1,4);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    uint8_t sampled_pattern[TEST_PIXELS*4];assert(sky_mtl_texture_readback(target->metal,0,0,SKY_MTL_READ_COLOR,sampled_pattern,sizeof sampled_pattern,NULL));
    for(int y=0;y<TEST_HEIGHT;y++)assert(memcmp(sampled_pattern+y*TEST_WIDTH*4,upload+y*32,TEST_WIDTH*4)==0);
    sky_nvn_texture_t compressed;make_texture(&compressed,device,memory,0x43,0x80000);
    const uint8_t bc2_block[16]={255,255,255,255,255,255,255,255,0,248,0,0,0,0,0,0};
    sky_nvn_texture_write_texels(&compressed,NULL,&region,bc2_block);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_copy_row_stride(&command,0);sky_nvn_command_buffer_set_copy_image_stride(&command,0);
    sky_nvn_command_buffer_copy_texture_to_buffer(&command,&compressed,NULL,&region,memory->buffer_address+0x160000,0);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    assert(memcmp(cpu+0x160000,bc2_block,sizeof bc2_block)==0);
    test_direct_texture_uploads();
    test_frame_capture(device,queue,target);
    sky_nvn_texture_builder_t linear_builder={0};sky_nvn_texture_builder_set_device(&linear_builder,device);
    sky_nvn_texture_builder_set_defaults(&linear_builder);sky_nvn_texture_builder_set_format(&linear_builder,1);
    sky_nvn_texture_builder_set_flags(&linear_builder,0x10);sky_nvn_texture_builder_set_size_2d(&linear_builder,64,4);
    sky_nvn_texture_builder_set_stride(&linear_builder,64);sky_nvn_texture_builder_set_storage(&linear_builder,memory,0x170000);
    memset(cpu+0x170000,0x33,256);sky_nvn_texture_t linear_video;assert(sky_nvn_texture_initialize(&linear_video,&linear_builder));
    sky_mtl_texture_properties_t linear_properties;assert(sky_mtl_texture_get_properties(linear_video.metal,&linear_properties));
    assert(linear_properties.storage_mode==SKY_MTL_STORAGE_SHARED&&linear_properties.usage==SKY_MTL_TEXTURE_USAGE_SHADER_READ);
    assert(sky_nvn_texture_get_stride(&linear_video)==64);uint8_t video_pixels[256];
    assert(sky_mtl_texture_readback(linear_video.metal,0,0,SKY_MTL_READ_COLOR,video_pixels,sizeof video_pixels,NULL));
    for(int i=0;i<256;i++)assert(video_pixels[i]==0x33);
    memset(cpu+0x170000,0x7b,256);
    assert(sky_mtl_texture_readback(linear_video.metal,0,0,SKY_MTL_READ_COLOR,video_pixels,sizeof video_pixels,NULL));
    for(int i=0;i<256;i++)assert(video_pixels[i]==0x7b);
    /* Force the Simulator's private-copy path on this native GPU. A queued
     * buffer update must execute before the mirror refresh and shader sample.
     * A later CPU frame must refresh the same retained descriptor too. */
    sky_mtl_texture_destroy(linear_video.metal);
    linear_video.metal=sky_mtl_texture_create_linear_copy_from_buffer(
        memory->buffer_address+0x170000,10,64,4,64);
    assert(linear_video.metal&&sky_mtl_linear_texture_needs_sync(sky_mtl_texture_native_handle(linear_video.metal)));
    sky_nvn_texture_pool_register_texture(&texture_pool,258,&linear_video,NULL);
    uint8_t next_video_frame[256];memset(next_video_frame,0x66,sizeof next_video_frame);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command,1,targets,NULL,NULL,NULL);
    sky_nvn_command_buffer_bind_program(&command,&textured,3);
    sky_nvn_command_buffer_bind_vertex_attrib_state(&command,2,attributes);
    sky_nvn_command_buffer_bind_vertex_stream_state(&command,1,stream);
    sky_nvn_command_buffer_bind_vertex_buffer(&command,0,memory->buffer_address+0x100000,sizeof vertices);
    sky_nvn_command_buffer_set_texture_pool(&command,&texture_pool);
    sky_nvn_command_buffer_set_sampler_pool(&command,&sampler_pool);
    sky_nvn_command_buffer_bind_texture(&command,1,0,sky_nvn_device_get_texture_handle(device,258,256));
    sky_nvn_command_buffer_update_uniform_buffer(&command,memory->buffer_address+0x170000,
        sizeof next_video_frame,0,sizeof next_video_frame,next_video_frame);
    sky_nvn_command_buffer_draw_arrays(&command,5,1,4);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    expect_color(target,(uint8_t[]){0x66,0,0,255});
    memset(cpu+0x170000,0x9b,256);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_draw_arrays(&command,5,1,4);
    handle=sky_nvn_command_buffer_end_recording(&command);sky_nvn_queue_submit_commands(queue,1,&handle);sky_nvn_queue_finish(queue);
    expect_color(target,(uint8_t[]){0x9b,0,0,255});
    sky_nvn_texture_finalize(&linear_video);
    sky_mtl_graphics_stats_t stats=sky_mtl_graphics_stats();assert(stats.draws==17&&stats.dispatches==1&&stats.command_errors==0);
    sky_nvn_command_buffer_finalize(&command);
    sky_nvn_texture_pool_finalize(&texture_pool);sky_nvn_sampler_pool_finalize(&sampler_pool);
    sky_nvn_texture_finalize(&sampled);sky_nvn_texture_finalize(&writable);sky_nvn_texture_finalize(&test_depth);sky_nvn_texture_finalize(&compressed);
    sky_mtl_program_remove(uniform_serial);sky_mtl_program_remove(textured_serial);sky_mtl_program_remove(compute_serial);
    puts("NVN_SHADER_GPU_OK authored-quad-uniform-texture-sampler-index-base-vertex-scissor-immutable-handles-compute-image-sync-region-masked-clear-call-copy-retirement-state-inheritance-counter-depth-cross-queue-transfer");
}

int main(void) {
    ASSERT_LOOKUP("nvnCommandBufferBarrier",sky_nvn_command_buffer_barrier);
    ASSERT_LOOKUP("nvnCommandBufferInitialize", sky_nvn_command_buffer_initialize);
    ASSERT_LOOKUP("nvnCommandBufferFinalize", sky_nvn_command_buffer_finalize);
    ASSERT_LOOKUP("nvnCommandBufferSetMemoryCallback", sky_nvn_command_buffer_set_memory_callback);
    ASSERT_LOOKUP("nvnCommandBufferSetMemoryCallbackData", sky_nvn_command_buffer_set_memory_callback_data);
    ASSERT_LOOKUP("nvnCommandBufferAddCommandMemory", sky_nvn_command_buffer_add_command_memory);
    ASSERT_LOOKUP("nvnCommandBufferAddControlMemory", sky_nvn_command_buffer_add_control_memory);
    ASSERT_LOOKUP("nvnCommandBufferBeginRecording", sky_nvn_command_buffer_begin_recording);
    ASSERT_LOOKUP("nvnCommandBufferEndRecording", sky_nvn_command_buffer_end_recording);
    ASSERT_LOOKUP("nvnCommandBufferIsRecording", sky_nvn_command_buffer_is_recording);
    ASSERT_LOOKUP("nvnCommandBufferSetRenderTargets", sky_nvn_command_buffer_set_render_targets);
    ASSERT_LOOKUP("nvnCommandBufferClearColor", sky_nvn_command_buffer_clear_color);
    ASSERT_LOOKUP("nvnCommandBufferClearDepthStencil", sky_nvn_command_buffer_clear_depth_stencil);
    ASSERT_LOOKUP("nvnCommandBufferSetViewports", sky_nvn_command_buffer_set_viewports);
    ASSERT_LOOKUP("nvnCommandBufferSetScissor", sky_nvn_command_buffer_set_scissor);
    ASSERT_LOOKUP("nvnCommandBufferSetDepthRange", sky_nvn_command_buffer_set_depth_range);
    ASSERT_LOOKUP("nvnQueueSubmitCommands", sky_nvn_queue_submit_commands);
    ASSERT_LOOKUP("nvnCommandBufferBindUniformBuffer", sky_nvn_command_buffer_bind_uniform_buffer);
    ASSERT_LOOKUP("nvnCommandBufferBindProgram", sky_nvn_command_buffer_bind_program);
    ASSERT_LOOKUP("nvnProgramInitialize", sky_nvn_program_initialize);
    ASSERT_LOOKUP("nvnProgramSetShaders", sky_nvn_program_set_shaders);

    sky_nvn_device_t device;
    sky_nvn_memory_pool_t memory;
    void *allocation = NULL;
    setup_memory(&device, &memory, &allocation);
    sky_nvn_queue_t queue;
    void *queue_allocation = NULL;
    setup_queue(&device, &queue, &queue_allocation);
    sky_nvn_sync_t sync;
    assert(sky_nvn_sync_initialize(&sync, &device) == 1);

    sky_nvn_texture_t color[2], depth;
    make_texture(&color[0], &device, &memory, 0x25, 0x10000);
    make_texture(&color[1], &device, &memory, 0x25, 0x20000);
    make_texture(&depth, &device, &memory, 0x35, 0x30000);
    assert(sky_mtl_texture_active_count() >= 3);

    sky_nvn_command_buffer_t command = {0};
    assert(sky_nvn_command_buffer_initialize(NULL, &device) == 0);
    assert(sky_nvn_command_buffer_initialize(&command, NULL) == 0);
    assert(sky_nvn_command_buffer_initialize(&command, &device) == 1);
    assert(sky_nvn_command_buffer_is_recording(NULL) == 0);
    assert(sky_nvn_command_buffer_is_recording(&command) == 0);
    assert(sky_nvn_command_buffer_end_recording(&command) == 0);
    sky_nvn_command_buffer_begin_recording(&command);
    assert(sky_nvn_command_buffer_is_recording(&command) == 1);
    sky_nvn_command_buffer_begin_recording(&command);
    assert(sky_nvn_command_buffer_is_recording(&command) == 1);
    assert(sky_nvn_command_buffer_end_recording(&command) != 0);
    assert(sky_nvn_command_buffer_is_recording(&command) == 0);
    assert(sky_nvn_command_buffer_end_recording(&command) == 0);

    sky_nvn_command_buffer_set_memory_callback(&command, memory_callback);
    sky_nvn_command_buffer_set_memory_callback_data(&command, &command);
    sky_nvn_command_buffer_add_command_memory(&command, &memory, 0, 65536);
    uint8_t control_memory[4096] = {0};
    sky_nvn_command_buffer_add_control_memory(&command, control_memory, sizeof control_memory);

    void *targets[2] = {&color[0], &color[1]};
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command, 2, targets, NULL, NULL, NULL);
    sky_nvn_command_buffer_clear_color(&command, 0, red, 0xe);
    expect_rejected(&queue, sky_nvn_command_buffer_end_recording(&command));

    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_add_command_memory(&command, &memory, -1, 4096);
    sky_nvn_command_buffer_set_render_targets(&command, 2, targets, NULL, NULL, NULL);
    sky_nvn_command_buffer_clear_color(&command, 0, red, 0xf);
    expect_rejected(&queue, sky_nvn_command_buffer_end_recording(&command));

    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command, 2, targets, NULL, NULL, NULL);
    sky_nvn_command_buffer_clear_color(&command, 0, red, 0xf);
    expect_rejected(&queue, (uint64_t)&command);
    assert(sky_nvn_command_buffer_end_recording(&command) != 0);

    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command, 0, NULL, NULL, &depth, NULL);
    sky_nvn_command_buffer_clear_depth_stencil(&command,0.25f,1,0xa5,0x1ff);
    expect_rejected(&queue, sky_nvn_command_buffer_end_recording(&command));

    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command, 2, targets, NULL, NULL, NULL);
    sky_nvn_command_buffer_clear_color(&command, 0, red, 0xf);
    sky_nvn_command_buffer_clear_color(&command, 1, blue, 0xf);
    const float viewport[4] = {0.0f, 0.0f, TEST_WIDTH, TEST_HEIGHT};
    sky_nvn_command_buffer_set_viewports(&command, 0, 1, viewport);
    sky_nvn_command_buffer_set_scissor(&command, 0, 0, TEST_WIDTH, TEST_HEIGHT);
    sky_nvn_command_buffer_set_depth_range(&command, 0.0f, 1.0f);
    uint64_t handle = sky_nvn_command_buffer_end_recording(&command);
    assert(handle != 0 && handle != (uint64_t)&command);
    sky_nvn_queue_submit_commands(&queue, 1, &handle);
    assert(queue.submits == 1 && queue.completed <= 1);
    sky_nvn_queue_finish(&queue);
    assert(queue.submits == 1 && queue.completed == 1);
    const uint8_t red_bytes[4] = {255, 0, 0, 255};
    expect_color(&color[0], red_bytes);
    const uint8_t first_blue_bytes[4]={0,0,255,255};
    expect_color(&color[1], first_blue_bytes);

    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command, 2, targets, NULL, NULL, NULL);
    sky_nvn_command_buffer_clear_color(&command, 1, blue, 0xf);
    handle = sky_nvn_command_buffer_end_recording(&command);
    assert(handle != 0 && handle != (uint64_t)&command);
    sky_nvn_queue_submit_commands(&queue, 1, &handle);
    assert(queue.submits == 2 && queue.completed >= 1 && queue.completed <= 2);
    sky_nvn_queue_fence_sync(&queue, &sync, 0, 0);
    assert(sync.fence == 2);
    sky_nvn_command_buffer_finalize(&command);
    assert(command.magic == 0 && sky_nvn_command_buffer_is_recording(&command) == 0);
    int wait_result = sky_nvn_sync_wait(&sync, UINT64_MAX);
    assert((wait_result == 0 || wait_result == 1) && queue.completed == 2);
    assert(sky_mtl_texture_active_count() >= 3);
    const uint8_t blue_bytes[4] = {0, 0, 255, 255};
    expect_color(&color[0], red_bytes);
    expect_color(&color[1], blue_bytes);

    assert(sky_nvn_command_buffer_initialize(&command, &device) == 1);
    sky_nvn_command_buffer_begin_recording(&command);
    sky_nvn_command_buffer_set_render_targets(&command, 0, NULL, NULL, &depth, NULL);
    sky_nvn_command_buffer_clear_depth_stencil(&command, 0.25f, 1, 0xa5, 0xff);
    handle = sky_nvn_command_buffer_end_recording(&command);
    assert(handle != 0 && handle != (uint64_t)&command);
    sky_nvn_queue_submit_commands(&queue, 1, &handle);
    assert(queue.submits == 3);
    sky_nvn_command_buffer_finalize(&command);
    sky_nvn_queue_finish(&queue);
    assert(queue.completed == 3);
    assert(sky_mtl_texture_active_count() >= 3);

    float depth_pixels[TEST_PIXELS];
    uint8_t stencil_pixels[TEST_PIXELS];
    size_t depth_size = 0, stencil_size = 0;
    assert(sky_mtl_texture_readback(depth.metal, 0, 0, SKY_MTL_READ_DEPTH,
                                    depth_pixels, sizeof depth_pixels, &depth_size) == 1);
    assert(depth_size == sizeof depth_pixels);
    assert(sky_mtl_texture_readback(depth.metal, 0, 0, SKY_MTL_READ_STENCIL,
                                    stencil_pixels, sizeof stencil_pixels, &stencil_size) == 1);
    assert(stencil_size == sizeof stencil_pixels);
    printf("NVN_COMMAND_READBACK depth={%g,%g,%g,%g} stencil={%#x,%#x,%#x,%#x}\n",
           depth_pixels[0], depth_pixels[1], depth_pixels[2], depth_pixels[3],
           stencil_pixels[0], stencil_pixels[1], stencil_pixels[2], stencil_pixels[3]);
    fflush(stdout);
    for (int index = 0; index < TEST_PIXELS; index++) {
        assert(depth_pixels[index] == 0.25f);
        assert(stencil_pixels[index] == 0xa5);
    }
    assert(sky_mtl_texture_readback(depth.metal, 0, 0, SKY_MTL_READ_COLOR,
                                    depth_pixels, sizeof depth_pixels, NULL) == 0);
    assert(sky_mtl_texture_readback(color[0].metal, 0, 0, SKY_MTL_READ_DEPTH,
                                    depth_pixels, sizeof depth_pixels, NULL) == 0);
    assert(sky_mtl_texture_readback(color[0].metal, 0, 0, SKY_MTL_READ_COLOR,
                                    depth_pixels, 1, NULL) == 0);

    test_boolean_abi(&device,&queue,&depth);
    test_shader_draws(&device,&memory,&queue,&color[0]);
    test_polygon_offset(&device,&memory,&queue,&color[0]);
    test_command_memory(&device,&memory,&queue,&color[0]);
    test_invalid_command_diagnostics(&device,&memory,&queue);
    test_depth_only_clear(&device,&memory,&queue,&depth);
    test_called_handle_refill_retirement(&device,&memory,&queue);
    test_headless_present_pacing(&device,&queue,&color[0]);
    test_compute_device_limits(&queue);
    assert(sky_mtl_texture_active_count()>=3);

    sky_nvn_sync_finalize(&sync);
    sky_nvn_texture_finalize(&color[0]);
    sky_nvn_texture_finalize(&color[1]);
    sky_nvn_texture_finalize(&depth);
    sky_nvn_queue_finalize(&queue);
    assert(sky_mtl_command_queue_active_count() == 0);
    assert(sky_mtl_texture_active_count()==0);
    assert(sky_mtl_memory_pool_active_count() == 1);
    sky_nvn_memory_pool_finalize(&memory);
    assert(sky_mtl_memory_pool_active_count() == 0);
    free(queue_allocation);
    free(allocation);
    sky_nvn_device_finalize(&device);
    puts("T_NVN_COMMANDS_OK lifecycle-indexed-color-depth-stencil-fence-inflight-readback-cleanup");
    return 0;
}
