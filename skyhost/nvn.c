#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include "nvn/nvn_tex.h"
#include "nvn/nvn_deswizzle.h"
#include "nvn/nvn_fmt_mtl.h"
#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "ios/frontend.h"
#endif
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

static const char *g_last_name;

#define SKY_NVN_VIEW_FLAG_BIT 0x1
#define SKY_NVN_VIEW_FLAG_LAYER 0x2
#define SKY_NVN_VIEW_FLAG_FORMAT 0x4
#define SKY_NVN_VIEW_FLAG_TARGET 0x8
#define SKY_NVN_VIEW_FLAG_SWIZZLE 0x10
#define SKY_NVN_VIEW_FLAG_DSM 0x20
#define SKY_NVN_VIEW_FIELD(value, shift, bits) (((uint64_t)(value) & ((1ull << (bits)) - 1)) << (shift))
#define SKY_NVN_VIEW_GET(state, shift, bits) (((state) >> (shift)) & ((1ull << (bits)) - 1))

#if TARGET_OS_IPHONE
/* Unknown procedures keep lazy failure semantics without writable executable
 * storage. The lookup logs the name; the precompiled trap reports its caller. */
__attribute__((noreturn)) static void sky_nvn_unimplemented_ios(void) {
    extern void crash_backtrace(uint64_t frame_pointer);
    sky_checkpoint("post_nnMain_nvn_unimplemented");
    LOG("NVN_UNIMPL platform=ios diagnostic=static-fatal caller=%p", __builtin_return_address(0));
    crash_backtrace((uint64_t)__builtin_frame_address(0));
    LOG("RUN_STOP checkpoint=post_nnMain_nvn_unimplemented reason=UNIMPL_NVN");
    abort();
}

static void *sky_nvn_missing_proc(const char *name) {
    (void)name;
    return sky_nvn_unimplemented_ios;
}
#else
typedef struct sky_nvn_missing_proc { struct sky_nvn_missing_proc *next; char *name; void *code; } sky_nvn_missing_proc_t;
static sky_nvn_missing_proc_t *g_missing_procs;
static pthread_mutex_t g_missing_proc_lock=PTHREAD_MUTEX_INITIALIZER;
__attribute__((noreturn)) static void sky_nvn_unimplemented_named(const char *name) {
    extern void crash_backtrace(uint64_t frame_pointer);
    sky_checkpoint("post_nnMain_nvn_unimplemented");
    LOG("NVN_UNIMPL name=%s caller=%p",name,__builtin_return_address(0));
    crash_backtrace((uint64_t)__builtin_frame_address(0));
    LOG("RUN_STOP checkpoint=post_nnMain_nvn_unimplemented reason=UNIMPL_NVN");abort();
}
static void *sky_nvn_missing_proc(const char *name) {
    pthread_mutex_lock(&g_missing_proc_lock);
    for(sky_nvn_missing_proc_t *p=g_missing_procs;p;p=p->next)if(!strcmp(p->name,name)) {
        void *code=p->code;pthread_mutex_unlock(&g_missing_proc_lock);return code;
    }
    sky_nvn_missing_proc_t *p=calloc(1,sizeof *p);
    if(!p){pthread_mutex_unlock(&g_missing_proc_lock);return sky_nvn_unimplemented;}
    p->name=strdup(name);
    size_t page=(size_t)getpagesize();
    p->code=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(!p->name||p->code==MAP_FAILED){if(p->code!=MAP_FAILED)munmap(p->code,page);free(p->name);free(p);pthread_mutex_unlock(&g_missing_proc_lock);return sky_nvn_unimplemented;}
    /* ldr x0,name; ldr x16,handler; br x16. Preserve the guest link register,
     * so an unsupported call reports its own procedure and original caller. */
    uint32_t words[]={0x58000080,0x580000b0,0xd61f0200,0xd503201f};
    memcpy(p->code,words,sizeof words);
    uintptr_t literal=(uintptr_t)p->name;memcpy((uint8_t *)p->code+16,&literal,8);
    literal=(uintptr_t)sky_nvn_unimplemented_named;memcpy((uint8_t *)p->code+24,&literal,8);
    __builtin___clear_cache(p->code,(char *)p->code+32);
    if(mprotect(p->code,page,PROT_READ|PROT_EXEC)!=0){munmap(p->code,page);free(p->name);free(p);pthread_mutex_unlock(&g_missing_proc_lock);return sky_nvn_unimplemented;}
    p->next=g_missing_procs;g_missing_procs=p;
    void *code=p->code;pthread_mutex_unlock(&g_missing_proc_lock);return code;
}
#endif

void *sky_nvn_bootstrap_loader(const char *name) {
    if (!name) return sky_nvn_unimplemented;
    g_last_name = name;
    if (!strcmp(name,"nvnBootstrapLoader")) return sky_nvn_bootstrap_loader;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return sky_nvn_device_get_proc_address;
    if (!strcmp(name, "nvnDeviceGetInteger")) return sky_nvn_device_get_integer;
    if (!strcmp(name, "nvnDeviceBuilderSetDefaults")) return sky_nvn_device_builder_set_defaults;
    if (!strcmp(name, "nvnDeviceBuilderSetFlags")) return sky_nvn_device_builder_set_flags;
    if (!strcmp(name, "nvnDeviceBuilderGetFlags")) return sky_nvn_device_builder_get_flags;
    if (!strcmp(name, "nvnDeviceInitialize")) return sky_nvn_device_initialize;
    if (!strcmp(name, "nvnDeviceFinalize")) return sky_nvn_device_finalize;
    if (!strcmp(name, "nvnDeviceSetDebugLabel")) return sky_nvn_device_set_debug_label;
    if (!strcmp(name, "nvnDeviceSetIntermediateShaderCache")) return sky_nvn_device_set_intermediate_shader_cache;
    if (!strcmp(name, "nvnDeviceSetWindowOriginMode")) return sky_nvn_device_set_window_origin_mode;
    if (!strcmp(name, "nvnDeviceSetDepthMode")) return sky_nvn_device_set_depth_mode;
    if (!strcmp(name, "nvnDeviceGetWindowOriginMode")) return sky_nvn_device_get_window_origin_mode;
    if (!strcmp(name, "nvnDeviceGetDepthMode")) return sky_nvn_device_get_depth_mode;
    if (!strcmp(name, "nvnDeviceSetAutomaticCheckpointsEnable")) return sky_nvn_device_set_automatic_checkpoints_enable;
    if (!strcmp(name, "nvnDeviceSetAutomaticCheckpointsMask")) return sky_nvn_device_set_automatic_checkpoints_mask;
    if (!strcmp(name, "nvnDeviceSetAutomaticCheckpointsSamplingInterval")) return sky_nvn_device_set_automatic_checkpoints_sampling_interval;
    if (!strcmp(name, "nvnDeviceSetAutomaticCheckpointsByDebugGroup")) return sky_nvn_device_set_automatic_checkpoints_by_debug_group;
    if (!strcmp(name, "nvnDeviceSetCheckpointCallStacksEnable")) return sky_nvn_device_set_checkpoint_call_stacks_enable;
    if (!strcmp(name, "nvnDeviceSetCheckpointMaxCallStacksDepth")) return sky_nvn_device_set_checkpoint_max_call_stacks_depth;
    if (!strcmp(name, "nvnSyncInitialize")) return sky_nvn_sync_initialize;
    if (!strcmp(name, "nvnSyncFinalize")) return sky_nvn_sync_finalize;
    if (!strcmp(name, "nvnSyncWait")) return sky_nvn_sync_wait;
    if (!strcmp(name, "nvnMemoryPoolBuilderSetDefaults")) return sky_nvn_memory_pool_builder_set_defaults;
    if (!strcmp(name, "nvnMemoryPoolBuilderSetDevice")) return sky_nvn_memory_pool_builder_set_device;
    if (!strcmp(name, "nvnMemoryPoolBuilderSetStorage")) return sky_nvn_memory_pool_builder_set_storage;
    if (!strcmp(name, "nvnMemoryPoolBuilderSetFlags")) return sky_nvn_memory_pool_builder_set_flags;
    if (!strcmp(name, "nvnMemoryPoolBuilderGetDevice")) return sky_nvn_memory_pool_builder_get_device;
    if (!strcmp(name, "nvnMemoryPoolBuilderGetMemory")) return sky_nvn_memory_pool_builder_get_memory;
    if (!strcmp(name, "nvnMemoryPoolBuilderGetSize")) return sky_nvn_memory_pool_builder_get_size;
    if (!strcmp(name, "nvnMemoryPoolBuilderGetFlags")) return sky_nvn_memory_pool_builder_get_flags;
    if (!strcmp(name, "nvnMemoryPoolInitialize")) return sky_nvn_memory_pool_initialize;
    if (!strcmp(name, "nvnMemoryPoolSetDebugLabel")) return sky_nvn_memory_pool_set_debug_label;
    if (!strcmp(name, "nvnMemoryPoolFinalize")) return sky_nvn_memory_pool_finalize;
    if (!strcmp(name, "nvnMemoryPoolMap")) return sky_nvn_memory_pool_map;
    if (!strcmp(name, "nvnMemoryPoolFlushMappedRange")) return sky_nvn_memory_pool_flush_mapped_range;
    if (!strcmp(name, "nvnMemoryPoolInvalidateMappedRange")) return sky_nvn_memory_pool_invalidate_mapped_range;
    if (!strcmp(name, "nvnMemoryPoolGetBufferAddress")) return sky_nvn_memory_pool_get_buffer_address;
    if (!strcmp(name, "nvnMemoryPoolGetSize")) return sky_nvn_memory_pool_get_size;
    if (!strcmp(name, "nvnMemoryPoolGetFlags")) return sky_nvn_memory_pool_get_flags;
    if (!strcmp(name, "nvnBufferBuilderSetDevice")) return sky_nvn_buffer_builder_set_device;
    if (!strcmp(name, "nvnBufferBuilderSetDefaults")) return sky_nvn_buffer_builder_set_defaults;
    if (!strcmp(name, "nvnBufferBuilderSetStorage")) return sky_nvn_buffer_builder_set_storage;
    if (!strcmp(name, "nvnBufferBuilderGetSize")) return sky_nvn_buffer_builder_get_size;
    if (!strcmp(name, "nvnBufferInitialize")) return sky_nvn_buffer_initialize;
    if (!strcmp(name, "nvnBufferFinalize")) return sky_nvn_buffer_finalize;
    if (!strcmp(name, "nvnBufferMap")) return sky_nvn_buffer_map;
    if (!strcmp(name, "nvnBufferGetAddress")) return sky_nvn_buffer_get_address;
    if (!strcmp(name, "nvnProgramInitialize")) return sky_nvn_program_initialize;
    if (!strcmp(name, "nvnProgramFinalize")) return sky_nvn_program_finalize;
    if (!strcmp(name, "nvnProgramSetDebugLabel")) return sky_nvn_program_set_debug_label;
    if (!strcmp(name, "nvnProgramSetShaders")) return sky_nvn_program_set_shaders;
    if (!strcmp(name, "nvnTexturePoolInitialize")) return sky_nvn_texture_pool_initialize;
    if (!strcmp(name, "nvnTexturePoolFinalize")) return sky_nvn_texture_pool_finalize;
    if (!strcmp(name, "nvnTexturePoolRegisterTexture")) return sky_nvn_texture_pool_register_texture;
    if (!strcmp(name, "nvnTexturePoolRegisterImage")) return sky_nvn_texture_pool_register_image;
    if (!strcmp(name, "nvnTexturePoolGetMemoryPool")) return sky_nvn_texture_pool_get_memory_pool;
    if (!strcmp(name, "nvnTexturePoolGetMemoryOffset")) return sky_nvn_texture_pool_get_memory_offset;
    if (!strcmp(name, "nvnTexturePoolGetSize")) return sky_nvn_texture_pool_get_size;
    if (!strcmp(name, "nvnSamplerPoolInitialize")) return sky_nvn_sampler_pool_initialize;
    if (!strcmp(name, "nvnSamplerPoolFinalize")) return sky_nvn_sampler_pool_finalize;
    if (!strcmp(name, "nvnSamplerPoolRegisterSampler")) return sky_nvn_sampler_pool_register_sampler;
    if (!strcmp(name, "nvnSamplerPoolRegisterSamplerBuilder")) return sky_nvn_sampler_pool_register_sampler_builder;
    if (!strcmp(name, "nvnSamplerPoolGetMemoryPool")) return sky_nvn_sampler_pool_get_memory_pool;
    if (!strcmp(name, "nvnSamplerPoolGetMemoryOffset")) return sky_nvn_sampler_pool_get_memory_offset;
    if (!strcmp(name, "nvnSamplerPoolGetSize")) return sky_nvn_sampler_pool_get_size;
    if (!strcmp(name, "nvnSamplerBuilderSetDevice")) return sky_nvn_sampler_builder_set_device;
    if (!strcmp(name, "nvnSamplerBuilderSetDefaults")) return sky_nvn_sampler_builder_set_defaults;
    if (!strcmp(name, "nvnSamplerBuilderSetMinMagFilter")) return sky_nvn_sampler_builder_set_min_mag_filter;
    if (!strcmp(name, "nvnSamplerBuilderSetWrapMode")) return sky_nvn_sampler_builder_set_wrap_mode;
    if (!strcmp(name, "nvnSamplerBuilderSetLodClamp")) return sky_nvn_sampler_builder_set_lod_clamp;
    if (!strcmp(name, "nvnSamplerBuilderSetCompare")) return sky_nvn_sampler_builder_set_compare;
    if (!strcmp(name, "nvnSamplerBuilderSetMaxAnisotropy")) return sky_nvn_sampler_builder_set_max_anisotropy;
    if (!strcmp(name, "nvnSamplerBuilderSetLodBias")) return sky_nvn_sampler_builder_set_lod_bias;
    if (!strcmp(name, "nvnSamplerBuilderSetReductionFilter")) return sky_nvn_sampler_builder_set_reduction_filter;
    if (!strcmp(name, "nvnSamplerInitialize")) return sky_nvn_sampler_initialize;
    if (!strcmp(name, "nvnSamplerFinalize")) return sky_nvn_sampler_finalize;
    if (!strcmp(name, "nvnChannelMaskStateSetDefaults")) return sky_nvn_channel_mask_state_set_defaults;
    if (!strcmp(name, "nvnChannelMaskStateSetChannelMask")) return sky_nvn_channel_mask_state_set_channel_mask;
    if (!strcmp(name, "nvnBlendStateSetDefaults")) return sky_nvn_blend_state_set_defaults;
    if (!strcmp(name, "nvnBlendStateSetBlendTarget")) return sky_nvn_blend_state_set_blend_target;
    if (!strcmp(name, "nvnBlendStateSetBlendEquation")) return sky_nvn_blend_state_set_blend_equation;
    if (!strcmp(name, "nvnBlendStateSetBlendFunc")) return sky_nvn_blend_state_set_blend_func;
    if (!strcmp(name, "nvnColorStateSetDefaults")) return sky_nvn_color_state_set_defaults;
    if (!strcmp(name, "nvnColorStateSetBlendEnable")) return sky_nvn_color_state_set_blend_enable;
    if (!strcmp(name, "nvnColorStateSetAlphaTest")) return sky_nvn_color_state_set_alpha_test;
    if (!strcmp(name, "nvnPolygonStateSetDefaults")) return sky_nvn_polygon_state_set_defaults;
    if (!strcmp(name, "nvnPolygonStateSetFrontFace")) return sky_nvn_polygon_state_set_front_face;
    if (!strcmp(name, "nvnPolygonStateSetPolygonMode")) return sky_nvn_polygon_state_set_polygon_mode;
    if (!strcmp(name, "nvnPolygonStateSetCullFace")) return sky_nvn_polygon_state_set_cull_face;
    if (!strcmp(name, "nvnPolygonStateSetPolygonOffsetEnables")) return sky_nvn_polygon_state_set_polygon_offset_enables;
    if (!strcmp(name, "nvnDepthStencilStateSetDefaults")) return sky_nvn_depth_stencil_state_set_defaults;
    if (!strcmp(name, "nvnDepthStencilStateSetDepthTestEnable")) return sky_nvn_depth_stencil_state_set_depth_test_enable;
    if (!strcmp(name, "nvnDepthStencilStateSetStencilTestEnable")) return sky_nvn_depth_stencil_state_set_stencil_test_enable;
    if (!strcmp(name, "nvnDepthStencilStateSetStencilFunc")) return sky_nvn_depth_stencil_state_set_stencil_func;
    if (!strcmp(name, "nvnDepthStencilStateSetDepthWriteEnable")) return sky_nvn_depth_stencil_state_set_depth_write_enable;
    if (!strcmp(name, "nvnDepthStencilStateSetDepthFunc")) return sky_nvn_depth_stencil_state_set_depth_func;
    if (!strcmp(name, "nvnDepthStencilStateSetStencilOp")) return sky_nvn_depth_stencil_state_set_stencil_op;
    if (!strcmp(name, "nvnVertexAttribStateSetDefaults")) return sky_nvn_vertex_attrib_state_set_defaults;
    if (!strcmp(name, "nvnVertexAttribStateSetFormat")) return sky_nvn_vertex_attrib_state_set_format;
    if (!strcmp(name, "nvnVertexAttribStateSetStreamIndex")) return sky_nvn_vertex_attrib_state_set_stream_index;
    if (!strcmp(name, "nvnVertexStreamStateSetDefaults")) return sky_nvn_vertex_stream_state_set_defaults;
    if (!strcmp(name, "nvnVertexStreamStateSetStride")) return sky_nvn_vertex_stream_state_set_stride;
    if (!strcmp(name, "nvnVertexStreamStateSetDivisor")) return sky_nvn_vertex_stream_state_set_divisor;
    if (!strcmp(name, "nvnCommandBufferBindStorageBuffer")) return sky_nvn_command_buffer_bind_storage_buffer;
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffer")) return sky_nvn_command_buffer_bind_vertex_buffer;
    if (!strcmp(name, "nvnCommandBufferSetTexturePool")) return sky_nvn_command_buffer_set_texture_pool;
    if (!strcmp(name, "nvnCommandBufferSetSamplerPool")) return sky_nvn_command_buffer_set_sampler_pool;
    if (!strcmp(name, "nvnCommandBufferBindTexture")) return sky_nvn_command_buffer_bind_texture;
    if (!strcmp(name, "nvnCommandBufferBindSeparateTexture")) return sky_nvn_command_buffer_bind_separate_texture;
    if (!strcmp(name, "nvnCommandBufferBindSeparateSampler")) return sky_nvn_command_buffer_bind_separate_sampler;
    if (!strcmp(name, "nvnCommandBufferBindVertexAttribState")) return sky_nvn_command_buffer_bind_vertex_attrib_state;
    if (!strcmp(name, "nvnCommandBufferBindVertexStreamState")) return sky_nvn_command_buffer_bind_vertex_stream_state;
    if (!strcmp(name, "nvnCommandBufferBindBlendState")) return sky_nvn_command_buffer_bind_blend_state;
    if (!strcmp(name, "nvnCommandBufferBindChannelMaskState")) return sky_nvn_command_buffer_bind_channel_mask_state;
    if (!strcmp(name, "nvnCommandBufferBindColorState")) return sky_nvn_command_buffer_bind_color_state;
    if (!strcmp(name, "nvnCommandBufferBindPolygonState")) return sky_nvn_command_buffer_bind_polygon_state;
    if (!strcmp(name,"nvnCommandBufferSetPolygonOffsetClamp")) return sky_nvn_command_buffer_set_polygon_offset_clamp;
    if (!strcmp(name, "nvnCommandBufferBindDepthStencilState")) return sky_nvn_command_buffer_bind_depth_stencil_state;
    if (!strcmp(name, "nvnCommandBufferSetBlendColor")) return sky_nvn_command_buffer_set_blend_color;
    if (!strcmp(name, "nvnCommandBufferSetStencilValueMask")) return sky_nvn_command_buffer_set_stencil_value_mask;
    if (!strcmp(name, "nvnCommandBufferSetStencilMask")) return sky_nvn_command_buffer_set_stencil_mask;
    if (!strcmp(name, "nvnCommandBufferSetStencilRef")) return sky_nvn_command_buffer_set_stencil_ref;
    if (!strcmp(name, "nvnCommandBufferDrawArraysInstanced")) return sky_nvn_command_buffer_draw_arrays_instanced;
    if (!strcmp(name, "nvnCommandBufferDrawArrays")) return sky_nvn_command_buffer_draw_arrays;
    if (!strcmp(name, "nvnCommandBufferDrawElementsInstanced")) return sky_nvn_command_buffer_draw_elements_instanced;
    if (!strcmp(name, "nvnCommandBufferDrawElementsBaseVertex")) return sky_nvn_command_buffer_draw_elements_base_vertex;
    if (!strcmp(name, "nvnCommandBufferDrawElements")) return sky_nvn_command_buffer_draw_elements;
    if (!strcmp(name, "nvnCommandBufferDispatchCompute")) return sky_nvn_command_buffer_dispatch_compute;
    if (!strcmp(name, "nvnDeviceGetTextureHandle")) return sky_nvn_device_get_texture_handle;
    if (!strcmp(name, "nvnDeviceGetSeparateTextureHandle")) return sky_nvn_device_get_separate_texture_handle;
    if (!strcmp(name, "nvnDeviceGetSeparateSamplerHandle")) return sky_nvn_device_get_separate_sampler_handle;
    if (!strcmp(name, "nvnDeviceGetImageHandle")) return sky_nvn_device_get_image_handle;
    if (!strcmp(name, "nvnQueueAcquireTexture")) return sky_nvn_queue_acquire_texture;
    if (!strcmp(name, "nvnWindowAcquireTexture")) return sky_nvn_window_acquire_texture;
    if (!strcmp(name, "nvnQueuePresentTexture")) return sky_nvn_queue_present_texture;
    if (!strcmp(name, "nvnCommandBufferBindImage")) return sky_nvn_command_buffer_bind_image;
    if (!strcmp(name, "nvnCommandBufferBindImages")) return sky_nvn_command_buffer_bind_images;
    if (!strcmp(name, "nvnCommandBufferBindTextures")) return sky_nvn_command_buffer_bind_textures;
    if (!strcmp(name, "nvnCommandBufferBindSeparateTextures")) return sky_nvn_command_buffer_bind_separate_textures;
    if (!strcmp(name, "nvnCommandBufferBindSeparateSamplers")) return sky_nvn_command_buffer_bind_separate_samplers;
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffers")) return sky_nvn_command_buffer_bind_uniform_buffers;
    if (!strcmp(name, "nvnCommandBufferBindStorageBuffers")) return sky_nvn_command_buffer_bind_storage_buffers;
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffers")) return sky_nvn_command_buffer_bind_vertex_buffers;
    if (!strcmp(name, "nvnCommandBufferSetViewport")) return sky_nvn_command_buffer_set_viewport;
    if (!strcmp(name, "nvnCommandBufferSetScissors")) return sky_nvn_command_buffer_set_scissors;
    if (!strcmp(name, "nvnCommandBufferSetDepthRanges")) return sky_nvn_command_buffer_set_depth_ranges;
    if (!strcmp(name, "nvnCommandBufferBarrier")) return sky_nvn_command_buffer_barrier;
    if (!strcmp(name,"nvnCommandBufferWaitSync")) return sky_nvn_command_buffer_wait_sync;
    if (!strcmp(name,"nvnCommandBufferFenceSync")) return sky_nvn_command_buffer_fence_sync;
    if (!strcmp(name,"nvnCommandBufferClearTexture")) return sky_nvn_command_buffer_clear_texture;
    if (!strcmp(name,"nvnCommandBufferClearTexturei")) return sky_nvn_command_buffer_clear_texture_i;
    if (!strcmp(name,"nvnCommandBufferClearTextureui")) return sky_nvn_command_buffer_clear_texture_ui;
    if (!strcmp(name,"nvnCommandBufferCallCommands")) return sky_nvn_command_buffer_call_commands;
    if (!strcmp(name,"nvnCommandBufferCopyCommands")) return sky_nvn_command_buffer_copy_commands;
    if (!strcmp(name,"nvnCommandBufferReportCounter")) return sky_nvn_command_buffer_report_counter;
    if (!strcmp(name,"nvnCommandBufferResetCounter")) return sky_nvn_command_buffer_reset_counter;
    if (!strcmp(name,"nvnCommandBufferReportValue")) return sky_nvn_command_buffer_report_value;
    if (!strcmp(name,"nvnDeviceGetTimestampInNanoseconds")) return sky_nvn_device_get_timestamp_in_nanoseconds;
    if (!strcmp(name,"nvnDeviceFinalizeCommandHandle")) return sky_nvn_device_finalize_command_handle;
    if (!strcmp(name,"nvnCommandBufferSetShaderScratchMemory")) return sky_nvn_command_buffer_set_shader_scratch_memory;
    if (!strcmp(name,"nvnCommandBufferSetDepthBounds")) return sky_nvn_command_buffer_set_depth_bounds;
    if (!strcmp(name,"nvnQueueGetError")) return sky_nvn_queue_get_error;
    if (!strcmp(name,"nvnCommandBufferSetCopyRowStride")) return sky_nvn_command_buffer_set_copy_row_stride;
    if (!strcmp(name,"nvnCommandBufferSetCopyImageStride")) return sky_nvn_command_buffer_set_copy_image_stride;
    if (!strcmp(name,"nvnCommandBufferCopyBufferToTexture")) return sky_nvn_command_buffer_copy_buffer_to_texture;
    if (!strcmp(name,"nvnCommandBufferCopyTextureToBuffer")) return sky_nvn_command_buffer_copy_texture_to_buffer;
    if (!strcmp(name,"nvnCommandBufferCopyTextureToTexture")) return sky_nvn_command_buffer_copy_texture_to_texture;
    if (!strcmp(name,"nvnCommandBufferUpdateUniformBuffer")) return sky_nvn_command_buffer_update_uniform_buffer;
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffer")) return sky_nvn_command_buffer_bind_uniform_buffer;
    if (!strcmp(name, "nvnCommandBufferBindProgram")) return sky_nvn_command_buffer_bind_program;
    if (!strcmp(name, "nvnTextureBuilderSetDefaults")) return sky_nvn_texture_builder_set_defaults;
    if (!strcmp(name, "nvnTextureBuilderSetDevice")) return sky_nvn_texture_builder_set_device;
    if (!strcmp(name, "nvnTextureBuilderSetFlags")) return sky_nvn_texture_builder_set_flags;
    if (!strcmp(name, "nvnTextureBuilderSetTarget")) return sky_nvn_texture_builder_set_target;
    if (!strcmp(name, "nvnTextureBuilderSetWidth")) return sky_nvn_texture_builder_set_width;
    if (!strcmp(name, "nvnTextureBuilderSetHeight")) return sky_nvn_texture_builder_set_height;
    if (!strcmp(name, "nvnTextureBuilderSetDepth")) return sky_nvn_texture_builder_set_depth;
    if (!strcmp(name, "nvnTextureBuilderSetSize1D")) return sky_nvn_texture_builder_set_size_1d;
    if (!strcmp(name, "nvnTextureBuilderSetSize2D")) return sky_nvn_texture_builder_set_size_2d;
    if (!strcmp(name, "nvnTextureBuilderSetSize3D")) return sky_nvn_texture_builder_set_size_3d;
    if (!strcmp(name, "nvnTextureBuilderSetLevels")) return sky_nvn_texture_builder_set_levels;
    if (!strcmp(name, "nvnTextureBuilderSetFormat")) return sky_nvn_texture_builder_set_format;
    if (!strcmp(name, "nvnTextureBuilderSetSamples")) return sky_nvn_texture_builder_set_samples;
    if (!strcmp(name, "nvnTextureBuilderSetSwizzle")) return sky_nvn_texture_builder_set_swizzle;
    if (!strcmp(name, "nvnTextureBuilderSetDepthStencilMode")) return sky_nvn_texture_builder_set_depth_stencil_mode;
    if (!strcmp(name, "nvnTextureBuilderSetStride")) return sky_nvn_texture_builder_set_stride;
    if (!strcmp(name, "nvnTextureBuilderSetStorage")) return sky_nvn_texture_builder_set_storage;
    if (!strcmp(name, "nvnTextureBuilderSetPackagedTextureData")) return sky_nvn_texture_builder_set_packaged_texture_data;
    if (!strcmp(name, "nvnTextureBuilderSetPackagedTextureLayout")) return sky_nvn_texture_builder_set_packaged_texture_layout;
    if (!strcmp(name, "nvnTextureBuilderGetDevice")) return sky_nvn_texture_builder_get_device;
    if (!strcmp(name, "nvnTextureBuilderGetFlags")) return sky_nvn_texture_builder_get_flags;
    if (!strcmp(name, "nvnTextureBuilderGetTarget")) return sky_nvn_texture_builder_get_target;
    if (!strcmp(name, "nvnTextureBuilderGetWidth")) return sky_nvn_texture_builder_get_width;
    if (!strcmp(name, "nvnTextureBuilderGetHeight")) return sky_nvn_texture_builder_get_height;
    if (!strcmp(name, "nvnTextureBuilderGetDepth")) return sky_nvn_texture_builder_get_depth;
    if (!strcmp(name, "nvnTextureBuilderGetLevels")) return sky_nvn_texture_builder_get_levels;
    if (!strcmp(name, "nvnTextureBuilderGetFormat")) return sky_nvn_texture_builder_get_format;
    if (!strcmp(name, "nvnTextureBuilderGetSamples")) return sky_nvn_texture_builder_get_samples;
    if (!strcmp(name, "nvnTextureBuilderGetSwizzle")) return sky_nvn_texture_builder_get_swizzle;
    if (!strcmp(name, "nvnTextureBuilderGetDepthStencilMode")) return sky_nvn_texture_builder_get_depth_stencil_mode;
    if (!strcmp(name, "nvnTextureBuilderGetStride")) return sky_nvn_texture_builder_get_stride;
    if (!strcmp(name, "nvnTextureBuilderGetMemoryPool")) return sky_nvn_texture_builder_get_memory_pool;
    if (!strcmp(name, "nvnTextureBuilderGetMemoryOffset")) return sky_nvn_texture_builder_get_memory_offset;
    if (!strcmp(name, "nvnTextureBuilderGetStorageSize")) return sky_nvn_texture_builder_get_storage_size;
    if (!strcmp(name, "nvnTextureBuilderGetStorageAlignment")) return sky_nvn_texture_builder_get_storage_alignment;
    if (!strcmp(name, "nvnTextureInitialize")) return sky_nvn_texture_initialize;
    if (!strcmp(name, "nvnTextureFinalize")) return sky_nvn_texture_finalize;
    if (!strcmp(name, "nvnTextureWriteTexels")) return sky_nvn_texture_write_texels;
    if (!strcmp(name, "nvnTextureSetDebugLabel")) return sky_nvn_texture_set_debug_label;
    if (!strcmp(name, "nvnTextureGetFlags")) return sky_nvn_texture_get_flags;
    if (!strcmp(name, "nvnTextureGetTarget")) return sky_nvn_texture_get_target;
    if (!strcmp(name, "nvnTextureGetWidth")) return sky_nvn_texture_get_width;
    if (!strcmp(name, "nvnTextureGetHeight")) return sky_nvn_texture_get_height;
    if (!strcmp(name, "nvnTextureGetDepth")) return sky_nvn_texture_get_depth;
    if (!strcmp(name, "nvnTextureGetLevels")) return sky_nvn_texture_get_levels;
    if (!strcmp(name, "nvnTextureGetFormat")) return sky_nvn_texture_get_format;
    if (!strcmp(name, "nvnTextureGetSamples")) return sky_nvn_texture_get_samples;
    if (!strcmp(name, "nvnTextureGetSwizzle")) return sky_nvn_texture_get_swizzle;
    if (!strcmp(name, "nvnTextureGetDepthStencilMode")) return sky_nvn_texture_get_depth_stencil_mode;
    if (!strcmp(name, "nvnTextureGetStride")) return sky_nvn_texture_get_stride;
    if (!strcmp(name, "nvnTextureGetMemoryPool")) return sky_nvn_texture_get_memory_pool;
    if (!strcmp(name, "nvnTextureGetMemoryOffset")) return sky_nvn_texture_get_memory_offset;
    if (!strcmp(name, "nvnTextureGetStorageSize")) return sky_nvn_texture_get_storage_size;
    if (!strcmp(name, "nvnTextureGetTextureAddress")) return sky_nvn_texture_get_texture_address;
    if (!strcmp(name, "nvnTextureViewSetDefaults")) return sky_nvn_texture_view_set_defaults;
    if (!strcmp(name, "nvnTextureViewSetLevels")) return sky_nvn_texture_view_set_levels;
    if (!strcmp(name, "nvnTextureViewSetLayers")) return sky_nvn_texture_view_set_layers;
    if (!strcmp(name, "nvnTextureViewSetFormat")) return sky_nvn_texture_view_set_format;
    if (!strcmp(name, "nvnTextureViewSetSwizzle")) return sky_nvn_texture_view_set_swizzle;
    if (!strcmp(name, "nvnTextureViewSetDepthStencilMode")) return sky_nvn_texture_view_set_depth_stencil_mode;
    if (!strcmp(name, "nvnTextureViewSetTarget")) return sky_nvn_texture_view_set_target;
    if (!strcmp(name, "nvnTextureViewGetLevels")) return sky_nvn_texture_view_get_levels;
    if (!strcmp(name, "nvnTextureViewGetLayers")) return sky_nvn_texture_view_get_layers;
    if (!strcmp(name, "nvnTextureViewGetFormat")) return sky_nvn_texture_view_get_format;
    if (!strcmp(name, "nvnTextureViewGetSwizzle")) return sky_nvn_texture_view_get_swizzle;
    if (!strcmp(name, "nvnTextureViewGetDepthStencilMode")) return sky_nvn_texture_view_get_depth_stencil_mode;
    if (!strcmp(name, "nvnTextureViewGetTarget")) return sky_nvn_texture_view_get_target;
    if (!strcmp(name, "nvnTextureViewCompare")) return sky_nvn_texture_view_compare;
    if (!strcmp(name, "nvnTextureGetViewOffset")) return sky_nvn_texture_get_view_offset;
    if (!strcmp(name, "nvnQueueBuilderSetDefaults")) return sky_nvn_queue_builder_set_defaults;
    if (!strcmp(name, "nvnQueueBuilderSetDevice")) return sky_nvn_queue_builder_set_device;
    if (!strcmp(name, "nvnQueueBuilderSetFlags")) return sky_nvn_queue_builder_set_flags;
    if (!strcmp(name, "nvnQueueBuilderSetCommandMemorySize")) return sky_nvn_queue_builder_set_command_memory_size;
    if (!strcmp(name, "nvnQueueBuilderSetComputeMemorySize")) return sky_nvn_queue_builder_set_compute_memory_size;
    if (!strcmp(name, "nvnQueueBuilderSetControlMemorySize")) return sky_nvn_queue_builder_set_control_memory_size;
    if (!strcmp(name, "nvnQueueBuilderSetCommandFlushThreshold")) return sky_nvn_queue_builder_set_command_flush_threshold;
    if (!strcmp(name, "nvnQueueBuilderGetQueueMemorySize")) return sky_nvn_queue_builder_get_queue_memory_size;
    if (!strcmp(name, "nvnQueueBuilderSetQueueMemory")) return sky_nvn_queue_builder_set_queue_memory;
    if (!strcmp(name, "nvnQueueInitialize")) return sky_nvn_queue_initialize;
    if (!strcmp(name, "nvnQueueFinalize")) return sky_nvn_queue_finalize;
    if (!strcmp(name, "nvnQueueFenceSync")) return sky_nvn_queue_fence_sync;
    if (!strcmp(name, "nvnQueueFlush")) return sky_nvn_queue_flush;
    if (!strcmp(name, "nvnQueueFinish")) return sky_nvn_queue_finish;
    if (!strcmp(name, "nvnCommandBufferInitialize")) return sky_nvn_command_buffer_initialize;
    if (!strcmp(name, "nvnCommandBufferFinalize")) return sky_nvn_command_buffer_finalize;
    if (!strcmp(name, "nvnCommandBufferSetMemoryCallback")) return sky_nvn_command_buffer_set_memory_callback;
    if (!strcmp(name, "nvnCommandBufferSetMemoryCallbackData")) return sky_nvn_command_buffer_set_memory_callback_data;
    if (!strcmp(name, "nvnCommandBufferAddCommandMemory")) return sky_nvn_command_buffer_add_command_memory;
    if (!strcmp(name, "nvnCommandBufferAddControlMemory")) return sky_nvn_command_buffer_add_control_memory;
    if (!strcmp(name, "nvnCommandBufferGetCommandMemoryFree")) return sky_nvn_command_buffer_get_command_memory_free;
    if (!strcmp(name, "nvnCommandBufferGetControlMemoryFree")) return sky_nvn_command_buffer_get_control_memory_free;
    if (!strcmp(name, "nvnCommandBufferBeginRecording")) return sky_nvn_command_buffer_begin_recording;
    if (!strcmp(name, "nvnCommandBufferEndRecording")) return sky_nvn_command_buffer_end_recording;
    if (!strcmp(name, "nvnCommandBufferIsRecording")) return sky_nvn_command_buffer_is_recording;
    if (!strcmp(name, "nvnCommandBufferSetRenderTargets")) return sky_nvn_command_buffer_set_render_targets;
    if (!strcmp(name, "nvnCommandBufferClearColor")) return sky_nvn_command_buffer_clear_color;
    if (!strcmp(name, "nvnCommandBufferClearDepthStencil")) return sky_nvn_command_buffer_clear_depth_stencil;
    if (!strcmp(name, "nvnCommandBufferSetViewports")) return sky_nvn_command_buffer_set_viewports;
    if (!strcmp(name, "nvnCommandBufferSetScissor")) return sky_nvn_command_buffer_set_scissor;
    if (!strcmp(name, "nvnCommandBufferSetDepthRange")) return sky_nvn_command_buffer_set_depth_range;
    if (!strcmp(name, "nvnQueueSubmitCommands")) return sky_nvn_queue_submit_commands;
    if (!strcmp(name, "nvnWindowBuilderSetDefaults")) return sky_nvn_window_builder_set_defaults;
    if (!strcmp(name, "nvnWindowBuilderSetDevice")) return sky_nvn_window_builder_set_device;
    if (!strcmp(name, "nvnWindowBuilderSetNativeWindow")) return sky_nvn_window_builder_set_native_window;
    if (!strcmp(name, "nvnWindowBuilderSetTextures")) return sky_nvn_window_builder_set_textures;
    if (!strcmp(name, "nvnWindowBuilderSetPresentInterval")) return sky_nvn_window_builder_set_present_interval;
    if (!strcmp(name, "nvnWindowBuilderSetNumActiveTextures")) return sky_nvn_window_builder_set_num_active_textures;
    if (!strcmp(name, "nvnWindowBuilderGetDevice")) return sky_nvn_window_builder_get_device;
    if (!strcmp(name, "nvnWindowBuilderGetNumTextures")) return sky_nvn_window_builder_get_num_textures;
    if (!strcmp(name, "nvnWindowBuilderGetTexture")) return sky_nvn_window_builder_get_texture;
    if (!strcmp(name, "nvnWindowBuilderGetNativeWindow")) return sky_nvn_window_builder_get_native_window;
    if (!strcmp(name, "nvnWindowBuilderGetPresentInterval")) return sky_nvn_window_builder_get_present_interval;
    if (!strcmp(name, "nvnWindowBuilderGetNumActiveTextures")) return sky_nvn_window_builder_get_num_active_textures;
    if (!strcmp(name, "nvnWindowInitialize")) return sky_nvn_window_initialize;
    if (!strcmp(name, "nvnWindowFinalize")) return sky_nvn_window_finalize;
    if (!strcmp(name, "nvnWindowSetPresentInterval")) return sky_nvn_window_set_present_interval;
    if (!strcmp(name, "nvnWindowSetNumActiveTextures")) return sky_nvn_window_set_num_active_textures;
    if (!strcmp(name, "nvnWindowSetCrop")) return sky_nvn_window_set_crop;
    if (!strcmp(name, "nvnWindowGetCrop")) return sky_nvn_window_get_crop;
    if (!strcmp(name, "nvnWindowGetNativeWindow")) return sky_nvn_window_get_native_window;
    if (!strcmp(name, "nvnWindowGetNumTextures")) return sky_nvn_window_get_num_textures;
    if (!strcmp(name, "nvnWindowGetNumActiveTextures")) return sky_nvn_window_get_num_active_textures;
    if (!strcmp(name, "nvnWindowGetPresentInterval")) return sky_nvn_window_get_present_interval;
    LOG("NVN_LOOKUP_UNKNOWN name=%s", name);
    return sky_nvn_missing_proc(name);
}

void *sky_nvn_device_get_proc_address(const void *device, const char *name) {
    (void)device;
    return sky_nvn_bootstrap_loader(name);
}

void sky_nvn_device_builder_set_defaults(void *builder) {
    if (builder) ((sky_nvn_device_builder_t *)builder)->flags = 0;
}

void sky_nvn_device_builder_set_flags(void *builder, int flags) {
    if (builder) ((sky_nvn_device_builder_t *)builder)->flags = (uint32_t)flags;
}

int sky_nvn_device_builder_get_flags(const void *builder) {
    return builder ? (int)((const sky_nvn_device_builder_t *)builder)->flags : 0;
}

uint32_t sky_nvn_device_initialize(void *device, const void *builder) {
    if (!device || !builder) return 0;
    sky_nvn_device_t *out = device;
    memset(out, 0, sizeof *out);
    out->magic = 0x44455630;
    out->flags = ((const sky_nvn_device_builder_t *)builder)->flags;
    sky_checkpoint("post_nnMain_nvn_device_initialize");
    LOG("NVN_DEVICE_INITIALIZED device=%p flags=%#x", device, out->flags);
    return 1;
}

void sky_nvn_device_finalize(void *device) {
    if (device) ((sky_nvn_device_t *)device)->magic = 0;
}

void sky_nvn_device_set_debug_label(void *device, const void *label) {
    if (device) ((sky_nvn_device_t *)device)->debug_label = label;
}

void sky_nvn_device_set_intermediate_shader_cache(void *device, int value) {
    if (device) ((sky_nvn_device_t *)device)->intermediate_shader_cache = value;
}

void sky_nvn_device_set_window_origin_mode(void *device, int mode) {
    if (device) ((sky_nvn_device_t *)device)->origin_mode = mode;
}

void sky_nvn_device_set_depth_mode(void *device, int mode) {
    if (device) ((sky_nvn_device_t *)device)->depth_mode = mode;
}

int sky_nvn_device_get_window_origin_mode(const void *device) {
    return device ? ((const sky_nvn_device_t *)device)->origin_mode : 0;
}

int sky_nvn_device_get_depth_mode(const void *device) {
    return device ? ((const sky_nvn_device_t *)device)->depth_mode : 0;
}

void sky_nvn_device_set_automatic_checkpoints_enable(void *device, uint8_t enabled) {
    if (device) ((sky_nvn_device_t *)device)->automatic_checkpoints = enabled;
}

void sky_nvn_device_set_automatic_checkpoints_mask(void *device, uint64_t mask) {
    if (device) ((sky_nvn_device_t *)device)->automatic_checkpoints_mask = mask;
}

void sky_nvn_device_set_automatic_checkpoints_sampling_interval(void *device, int interval) {
    if (device) ((sky_nvn_device_t *)device)->automatic_checkpoints_interval = interval;
}

void sky_nvn_device_set_automatic_checkpoints_by_debug_group(void *device, int group) {
    if (device) ((sky_nvn_device_t *)device)->automatic_checkpoints_by_debug_group = group;
}

void sky_nvn_device_set_checkpoint_call_stacks_enable(void *device, uint8_t enabled) {
    if (device) ((sky_nvn_device_t *)device)->checkpoint_call_stacks = enabled;
}

void sky_nvn_device_set_checkpoint_max_call_stacks_depth(void *device, int depth) {
    if (device) ((sky_nvn_device_t *)device)->checkpoint_max_call_stacks_depth = depth;
}

void sky_nvn_device_get_integer(const void *device, int what, int *out) {
    (void)device;
    if (!out) {
        LOG("NVN_DEVICE_INFO_ERROR item=%#x reason=null-output", what);
        return;
    }
    // Public NVN ABI values also used by D2R's generic implementation. Queue minima fit
    // this host's queue rounding and shared pages; descriptor values do not claim texture support.
    static const struct { int item, value; const char *name; } entries[] = {
        {0x0a, 16, "COUNTER_ALIGNMENT"},
        {0x54, 16384, "QUEUE_COMPUTE_MEMORY_MIN_SIZE"},
        {0x51, 65536, "QUEUE_COMMAND_MEMORY_MIN_SIZE"},
        {0x0f, 32, "TEXTURE_DESCRIPTOR_SIZE"},
        {0x24, 1048576, "MAX_TEXTURE_POOL_SIZE"},
        {0x10, 32, "SAMPLER_DESCRIPTOR_SIZE"},
        {0x12, 256, "RESERVED_SAMPLER_DESCRIPTORS"},
        {0x25, 4096, "MAX_SAMPLER_POOL_SIZE"},
        {0x38, 32, "LINEAR_TEXTURE_STRIDE_ALIGNMENT"},
        {0x39, 128, "LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT"},
    };
    for (unsigned index = 0; index < sizeof entries / sizeof entries[0]; index++) {
        if (entries[index].item != what) continue;
        *out = entries[index].value;
        LOG("NVN_DEVICE_INFO item=%#x name=%s value=%d source=public-nvn-abi+d2r-generic",
            what, entries[index].name, entries[index].value);
        return;
    }
    g_last_name = "nvnDeviceGetInteger";
    LOG("NVN_RESOURCE_BOUNDARY name=nvnDeviceGetInteger item=%#x reason=DEVICE_INFO_REQUIRED", what);
    sky_nvn_unimplemented();
}

#define SKY_NVN_MAGIC(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

uint32_t sky_nvn_sync_initialize(void *sync, void *device) {
    if (!sync || !device) return 0;
    sky_nvn_sync_t *out = sync;
    memset(out, 0, sizeof *out);
    out->magic = SKY_NVN_MAGIC('S', 'Y', 'N', 'C');
    out->dev = device;
    sky_checkpoint("post_nnMain_nvn_sync_initialize");
    LOG("NVN_SYNC_INITIALIZED sync=%p device=%p", sync, device);
    return 1;
}

void sky_nvn_sync_finalize(void *sync) {
    if (sync) ((sky_nvn_sync_t *)sync)->magic = 0;
}

void sky_nvn_memory_pool_builder_set_defaults(void *builder) {
    if (builder) memset(builder, 0, sizeof(sky_nvn_memory_pool_builder_t));
}

void sky_nvn_memory_pool_builder_set_device(void *builder, void *device) {
    if (builder) ((sky_nvn_memory_pool_builder_t *)builder)->dev = device;
}

void sky_nvn_memory_pool_builder_set_storage(void *builder, void *memory, uint64_t size) {
    if (!builder) return;
    sky_nvn_memory_pool_builder_t *out = builder;
    out->mem = memory;
    out->size = size;
}

void sky_nvn_memory_pool_builder_set_flags(void *builder, int flags) {
    if (builder) ((sky_nvn_memory_pool_builder_t *)builder)->flags = (uint32_t)flags;
}

void *sky_nvn_memory_pool_builder_get_device(const void *builder) {
    return builder ? ((const sky_nvn_memory_pool_builder_t *)builder)->dev : NULL;
}

void *sky_nvn_memory_pool_builder_get_memory(const void *builder) {
    return builder ? ((const sky_nvn_memory_pool_builder_t *)builder)->mem : NULL;
}

uint64_t sky_nvn_memory_pool_builder_get_size(const void *builder) {
    return builder ? ((const sky_nvn_memory_pool_builder_t *)builder)->size : 0;
}

int sky_nvn_memory_pool_builder_get_flags(const void *builder) {
    return builder ? (int)((const sky_nvn_memory_pool_builder_t *)builder)->flags : 0;
}

uint32_t sky_nvn_memory_pool_initialize(void *pool, const void *builder) {
    if (!pool || !builder) return 0;
    sky_nvn_memory_pool_t *out = pool;
    const sky_nvn_memory_pool_builder_t *in = builder;
    memset(out, 0, sizeof *out);
    if (!in->dev || ((const sky_nvn_device_t *)in->dev)->magic != 0x44455630) {
        LOG("NVN_MEMORY_POOL_INIT_ERROR reason=invalid-device device=%p", in->dev);
        return 0;
    }
    if (!in->mem || !in->size) {
        LOG("NVN_MEMORY_POOL_INIT_ERROR reason=invalid-storage memory=%p size=%#llx",
            in->mem, (unsigned long long)in->size);
        return 0;
    }
    if (in->flags & SKY_NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        LOG("NVN_MEMORY_POOL_INIT_ERROR reason=virtual-unsupported memory=%p size=%#llx",
            in->mem, (unsigned long long)in->size);
        return 0;
    }
    void *metal = sky_mtl_memory_pool_create(in->mem, in->size);
    if (!metal) {
        LOG("NVN_MEMORY_POOL_INIT_ERROR reason=metal-zero-copy-failed memory=%p size=%#llx",
            in->mem, (unsigned long long)in->size);
        return 0;
    }
    out->magic = SKY_NVN_MAGIC('P', 'O', 'O', 'L');
    out->flags = in->flags;
    out->dev = in->dev;
    out->mem = in->mem;
    out->size = in->size;
    out->metal = metal;
    if (!(out->flags & SKY_NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS))
        out->buffer_address = sky_mtl_memory_pool_gpu_address(metal);
    if (out->buffer_address && !sky_nvn_memory_pool_track(out)) {
        sky_mtl_memory_pool_destroy(metal);
        memset(out, 0, sizeof *out);
        LOG("NVN_MEMORY_POOL_INIT_ERROR reason=tracking-allocation-failed");
        return 0;
    }
    sky_checkpoint("post_nnMain_nvn_memory_pool_initialize");
    LOG("NVN_MEMORY_POOL_INITIALIZED pool=%p device=%p memory=%p size=%#llx flags=%#x metal=shared zero_copy=1 gpu_address=%#llx wrapped_size=%llu",
        pool, out->dev, out->mem, (unsigned long long)out->size, out->flags,
        (unsigned long long)out->buffer_address,
        (unsigned long long)sky_mtl_memory_pool_wrapped_size(metal));
    return 1;
}

void sky_nvn_memory_pool_set_debug_label(void *pool, const char *label) {
    if (!pool || ((sky_nvn_memory_pool_t *)pool)->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L')) return;
    sky_nvn_memory_pool_t *out = pool;
    out->debug_label = label;
    sky_mtl_memory_pool_set_debug_label(out->metal, label);
}

void sky_nvn_memory_pool_finalize(void *pool) {
    if (!pool || ((sky_nvn_memory_pool_t *)pool)->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L')) return;
    sky_nvn_memory_pool_t *out = pool;
    LOG("NVN_MEMORY_POOL_FINALIZED pool=%p", pool);
    sky_nvn_memory_pool_untrack(out);
    sky_mtl_memory_pool_destroy(out->metal);
    memset(out, 0, sizeof *out);
}

void *sky_nvn_memory_pool_map(const void *pool) {
    if (!pool) return NULL;
    const sky_nvn_memory_pool_t *in = pool;
    if (in->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') ||
        (in->flags & SKY_NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return NULL;
    return in->mem;
}

static int sky_nvn_memory_pool_range_valid(const sky_nvn_memory_pool_t *pool, int64_t offset, uint64_t size) {
    if (!pool || pool->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') || offset < 0) return 0;
    uint64_t start = (uint64_t)offset;
    return start <= pool->size && size <= pool->size - start;
}

static void sky_nvn_memory_pool_range(void *pool, int64_t offset, uint64_t size, int invalidate) {
    sky_nvn_memory_pool_t *out = pool;
    const char *operation = invalidate ? "InvalidateMappedRange" : "FlushMappedRange";
    if (!sky_nvn_memory_pool_range_valid(out, offset, size) ||
        (out->flags & SKY_NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) {
        LOG("NVN_MEMORY_POOL_RANGE_ERROR operation=%s offset=%lld size=%#llx",
            operation, (long long)offset, (unsigned long long)size);
        return;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (invalidate) out->invalidate_count++;
    else out->flush_count++;
    out->last_range_offset = offset;
    out->last_range_size = size;
    LOG("NVN_MEMORY_POOL_RANGE operation=%s offset=%lld size=%#llx mode=shared-coherent no-gpu-wait",
        operation, (long long)offset, (unsigned long long)size);
}

void sky_nvn_memory_pool_flush_mapped_range(void *pool, int64_t offset, uint64_t size) {
    sky_nvn_memory_pool_range(pool, offset, size, 0);
}

void sky_nvn_memory_pool_invalidate_mapped_range(void *pool, int64_t offset, uint64_t size) {
    sky_nvn_memory_pool_range(pool, offset, size, 1);
}

uint64_t sky_nvn_memory_pool_get_buffer_address(const void *pool) {
    if (!pool || ((const sky_nvn_memory_pool_t *)pool)->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L')) return 0;
    return ((const sky_nvn_memory_pool_t *)pool)->buffer_address;
}

uint64_t sky_nvn_memory_pool_get_size(const void *pool) {
    if (!pool || ((const sky_nvn_memory_pool_t *)pool)->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L')) return 0;
    return ((const sky_nvn_memory_pool_t *)pool)->size;
}

int sky_nvn_memory_pool_get_flags(const void *pool) {
    if (!pool || ((const sky_nvn_memory_pool_t *)pool)->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L')) return 0;
    return (int)((const sky_nvn_memory_pool_t *)pool)->flags;
}

void sky_nvn_buffer_builder_set_device(void *builder, void *device) {
    if (builder) ((sky_nvn_buffer_builder_t *)builder)->dev = device;
}

void sky_nvn_buffer_builder_set_defaults(void *builder) {
    if (!builder) return;
    sky_nvn_buffer_builder_t *out = builder;
    void *device = out->dev;
    memset(out, 0, sizeof *out);
    out->dev = device;
}

void sky_nvn_buffer_builder_set_storage(void *builder, const void *pool, int64_t offset, uint64_t size) {
    if (!builder) return;
    sky_nvn_buffer_builder_t *out = builder;
    out->pool = pool;
    out->offset = offset;
    out->size = size;
}

uint64_t sky_nvn_buffer_builder_get_size(const void *builder) {
    return builder ? ((const sky_nvn_buffer_builder_t *)builder)->size : 0;
}

uint32_t sky_nvn_buffer_initialize(void *buffer, const void *builder) {
    if (!buffer || !builder) return 0;
    sky_nvn_buffer_t *out = buffer;
    memset(out, 0, sizeof *out);
    const sky_nvn_buffer_builder_t *in = builder;
    const sky_nvn_memory_pool_t *pool = in->pool;
    if (!in->dev || ((const sky_nvn_device_t *)in->dev)->magic != 0x44455630) {
        LOG("NVN_BUFFER_INIT_ERROR reason=invalid-device");
        return 0;
    }
    if (!pool || pool->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') || !pool->mem ||
        in->offset < 0 || (uint64_t)in->offset > pool->size || !in->size ||
        in->size > pool->size - (uint64_t)in->offset) {
        LOG("NVN_BUFFER_INIT_ERROR reason=invalid-storage");
        return 0;
    }
    out->magic = SKY_NVN_MAGIC('B', 'U', 'F', 0);
    out->dev = in->dev;
    out->pool = in->pool;
    out->offset = in->offset;
    out->size = in->size;
    out->mem = (uint8_t *)pool->mem + in->offset;
    if (!sky_nvn_buffer_track(out)) {
        LOG("NVN_BUFFER_INIT_ERROR reason=tracking-allocation-failed");
        memset(out, 0, sizeof *out);
        return 0;
    }
    sky_checkpoint("post_nnMain_nvn_buffer_initialize");
    LOG("NVN_BUFFER_INITIALIZED buffer=%p device=%p pool=%p offset=%#llx size=%#llx map=%p",
        buffer, out->dev, out->pool, (unsigned long long)out->offset,
        (unsigned long long)out->size, out->mem);
    return 1;
}

void sky_nvn_buffer_finalize(void *buffer) {
    if (!buffer || ((sky_nvn_buffer_t *)buffer)->magic != SKY_NVN_MAGIC('B', 'U', 'F', 0)) return;
    sky_nvn_buffer_untrack(buffer);
    memset(buffer, 0, sizeof(sky_nvn_buffer_t));
}

void *sky_nvn_buffer_map(const void *buffer) {
    if (!buffer || ((const sky_nvn_buffer_t *)buffer)->magic != SKY_NVN_MAGIC('B', 'U', 'F', 0)) return NULL;
    return ((const sky_nvn_buffer_t *)buffer)->mem;
}

uint64_t sky_nvn_buffer_get_address(const void *buffer) {
    if (!buffer || ((const sky_nvn_buffer_t *)buffer)->magic != SKY_NVN_MAGIC('B', 'U', 'F', 0)) return 0;
    const sky_nvn_buffer_t *out = buffer;
    const sky_nvn_memory_pool_t *pool = out->pool;
    if (!pool || pool->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') || out->offset < 0 ||
        (uint64_t)out->offset > UINT64_MAX - pool->buffer_address) return 0;
    uint64_t address = pool->buffer_address + (uint64_t)out->offset;
    LOG("NVN_BUFFER_ADDRESS buffer=%p address=%#llx", buffer, (unsigned long long)address);
    return address;
}

typedef struct {
    uint32_t magic;
    const void *pool;
    int64_t offset;
    int32_t count;
} sky_nvn_descriptor_pool_t;

static uint32_t sky_nvn_descriptor_pool_initialize(void *object, const void *pool, int64_t offset, int count,
                                                    uint32_t magic, int maximum, const char *name,
                                                    const char *checkpoint) {
    if (!object) return 0;
    sky_nvn_descriptor_pool_t *out = object;
    memset(out, 0, sizeof *out);
    const sky_nvn_memory_pool_t *memory = pool;
    if (!memory || memory->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') ||
        !memory->metal || !memory->mem || !memory->buffer_address) {
        LOG("NVN_DESCRIPTOR_POOL_INIT_ERROR name=%s reason=invalid-memory-pool", name);
        return 0;
    }
    if (offset < 0) {
        LOG("NVN_DESCRIPTOR_POOL_INIT_ERROR name=%s reason=invalid-offset offset=%#llx",
            name, (unsigned long long)offset);
        return 0;
    }
    if (offset & 31) {
        LOG("NVN_DESCRIPTOR_POOL_INIT_ERROR name=%s reason=invalid-alignment offset=%#llx",
            name, (unsigned long long)offset);
        return 0;
    }
    if (count <= 0 || count > maximum) {
        LOG("NVN_DESCRIPTOR_POOL_INIT_ERROR name=%s reason=invalid-capacity count=%d maximum=%d",
            name, count, maximum);
        return 0;
    }
    uint64_t bytes = (uint64_t)count * 32;
    uint64_t start = (uint64_t)offset;
    if (start > memory->size || bytes > memory->size - start ||
        start > UINT64_MAX - memory->buffer_address) {
        LOG("NVN_DESCRIPTOR_POOL_INIT_ERROR name=%s reason=invalid-range offset=%#llx count=%d pool_size=%#llx",
            name, (unsigned long long)offset, count, (unsigned long long)memory->size);
        return 0;
    }
    out->magic = magic;
    out->pool = pool;
    out->offset = offset;
    out->count = count;
    sky_checkpoint(checkpoint);
    LOG("%s object=%p memory_pool=%p offset=%#llx count=%d descriptor_size=32 storage=%p gpu_address=%#llx descriptors=uninitialized",
        name, object, pool, (unsigned long long)offset, count,
        (uint8_t *)memory->mem + start, (unsigned long long)(memory->buffer_address + start));
    return 1;
}

static const sky_nvn_descriptor_pool_t *sky_nvn_descriptor_pool_valid(const void *object, uint32_t magic) {
    if (!object) return NULL;
    const sky_nvn_descriptor_pool_t *pool = object;
    return pool->magic == magic ? pool : NULL;
}

static void sky_nvn_descriptor_pool_finalize(void *object, uint32_t magic, const char *name) {
    if (!sky_nvn_descriptor_pool_valid(object, magic)) return;
    LOG("%s object=%p", name, object);
    memset(object, 0, sizeof(sky_nvn_descriptor_pool_t));
}

uint32_t sky_nvn_texture_pool_initialize(void *texture_pool, const void *pool, int64_t offset, int count) {
    return sky_nvn_descriptor_pool_initialize(texture_pool, pool, offset, count,
        SKY_NVN_MAGIC('T', 'X', 'P', 'L'), 1048576, "NVN_TEXTURE_POOL_INITIALIZED",
        "post_nnMain_nvn_texture_pool_initialize");
}

typedef struct sky_texture_registration sky_texture_registration_t;
struct sky_texture_registration {
    sky_texture_registration_t *next;
    sky_texture_registration_t *hash_next;
    const void *pool;
    int id, image, has_view;
    sky_nvn_memory_pool_t memory;
    sky_nvn_texture_t texture;
    sky_nvn_texture_view_t view;
    void *metal_view;
};

static sky_texture_registration_t *g_texture_registrations;
/* Descriptor IDs below 16384 have distinct buckets within one pool. Keep the
 * ownership list for retirement, and index hot draw lookups by pool and ID. */
enum { SKY_NVN_DESCRIPTOR_BUCKETS = 16384 };
static sky_texture_registration_t *g_texture_registration_index[SKY_NVN_DESCRIPTOR_BUCKETS];
static pthread_mutex_t g_texture_registration_lock = PTHREAD_MUTEX_INITIALIZER;
static const sky_nvn_texture_t *sky_nvn_texture_valid(const void *texture);
static const sky_nvn_texture_view_t *sky_nvn_texture_view_valid(const void *view);

static size_t sky_nvn_descriptor_bucket(const void *pool, int id) {
    uintptr_t key = (uintptr_t)pool;
    key ^= key >> 16;
    key ^= key >> 32;
    return (key ^ (uint32_t)id * UINT32_C(2654435761)) & (SKY_NVN_DESCRIPTOR_BUCKETS - 1);
}

/* Caller holds g_texture_registration_lock. */
static sky_texture_registration_t *sky_nvn_texture_registration_find(const void *pool, int id) {
    for (sky_texture_registration_t *entry = g_texture_registration_index[sky_nvn_descriptor_bucket(pool, id)];
         entry; entry = entry->hash_next)
        if (entry->pool == pool && entry->id == id) return entry;
    return NULL;
}

static void sky_nvn_texture_registration_index_remove(sky_texture_registration_t *entry) {
    sky_texture_registration_t **link = &g_texture_registration_index[sky_nvn_descriptor_bucket(entry->pool, entry->id)];
    while (*link && *link != entry) link = &(*link)->hash_next;
    if (*link) *link = entry->hash_next;
}

static void sky_nvn_texture_registration_index_add(sky_texture_registration_t *entry) {
    size_t bucket = sky_nvn_descriptor_bucket(entry->pool, entry->id);
    entry->hash_next = g_texture_registration_index[bucket];
    g_texture_registration_index[bucket] = entry;
}

static void sky_nvn_texture_registration_release(sky_texture_registration_t *entry) {
    sky_mtl_texture_destroy(entry->metal_view);
    sky_mtl_texture_destroy(entry->texture.metal);
    sky_mtl_memory_pool_destroy(entry->memory.metal);
    entry->metal_view = NULL;
    entry->texture.metal = NULL;
    entry->memory.metal = NULL;
}

static void sky_nvn_texture_pool_release_registrations(const void *texture_pool) {
    pthread_mutex_lock(&g_texture_registration_lock);
    sky_texture_registration_t **link = &g_texture_registrations;
    while (*link) {
        sky_texture_registration_t *entry = *link;
        if (entry->pool != texture_pool) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        sky_nvn_texture_registration_index_remove(entry);
        sky_nvn_texture_registration_release(entry);
        free(entry);
    }
    pthread_mutex_unlock(&g_texture_registration_lock);
}

void sky_nvn_texture_pool_finalize(void *texture_pool) {
    if (!sky_nvn_descriptor_pool_valid(texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'))) return;
    sky_nvn_texture_pool_release_registrations(texture_pool);
    sky_nvn_descriptor_pool_finalize(texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'),
        "NVN_TEXTURE_POOL_FINALIZED");
}

static uint32_t sky_nvn_texture_view_metal_type(int target) {
    switch (target) {
        case 0: return SKY_MTL_TEXTURE_TYPE_1D;
        case 1: case 7: return SKY_MTL_TEXTURE_TYPE_2D;
        case 2: return SKY_MTL_TEXTURE_TYPE_3D;
        case 3: return SKY_MTL_TEXTURE_TYPE_1D_ARRAY;
        case 4: return SKY_MTL_TEXTURE_TYPE_2D_ARRAY;
        case 5: return SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE;
        case 6: return SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE_ARRAY;
        case 8: return SKY_MTL_TEXTURE_TYPE_CUBE;
        case 9: return SKY_MTL_TEXTURE_TYPE_CUBE_ARRAY;
        default: return UINT32_MAX;
    }
}

static uint64_t sky_nvn_texture_view_layer_count(const sky_nvn_texture_t *texture) {
    switch (texture->target) {
        case 3: return texture->height;
        case 4: case 6: return texture->depth;
        case 8: return 6;
        case 9: return (uint64_t)texture->depth * 6;
        default: return 1;
    }
}

static int sky_nvn_texture_view_target_compatible(const sky_nvn_texture_t *source, int target,
                                                   uint64_t sample_count) {
    if (target == source->target && target != 10) return 1;
    if (target != 1 && target != 7) return 0;
    if (sample_count > 1) return 0;
    return source->target == 1 || source->target == 4 || source->target == 7 ||
           source->target == 8 || source->target == 9;
}

static void *sky_nvn_texture_view_create_metal(const sky_nvn_texture_t *source,
                                                const sky_nvn_texture_view_t *config) {
    sky_mtl_texture_properties_t base;
    if (!sky_mtl_texture_get_properties(source->metal, &base)) return NULL;
    int target = (config->state & SKY_NVN_VIEW_FLAG_TARGET) ?
        (int)SKY_NVN_VIEW_GET(config->state, 46, 4) : source->target;
    uint32_t texture_type = sky_nvn_texture_view_metal_type(target);
    if (texture_type == UINT32_MAX ||
        !sky_nvn_texture_view_target_compatible(source, target, base.sample_count) ||
        (base.sample_count > 1 && target != 5 && target != 6) ||
        (base.sample_count <= 1 && (target == 5 || target == 6))) return NULL;
    uint64_t level_start = (config->state & SKY_NVN_VIEW_FLAG_BIT) ?
        SKY_NVN_VIEW_GET(config->state, 6, 4) : 0;
    uint64_t level_count = source->levels;
    if (config->state & SKY_NVN_VIEW_FLAG_BIT) {
        uint64_t configured = SKY_NVN_VIEW_GET(config->state, 10, 4);
        if (configured) level_count = configured;
        else if (level_start < level_count) level_count -= level_start;
    }
    if (level_start >= source->levels || !level_count || level_start + level_count > source->levels)
        return NULL;
    uint64_t slice_start = (config->state & SKY_NVN_VIEW_FLAG_LAYER) ?
        SKY_NVN_VIEW_GET(config->state, 14, 12) : 0;
    uint64_t total_slices = sky_nvn_texture_view_layer_count(source);
    uint64_t slice_count = total_slices;
    if (config->state & SKY_NVN_VIEW_FLAG_LAYER) {
        uint64_t configured = SKY_NVN_VIEW_GET(config->state, 26, 12);
        if (configured) slice_count = configured;
        else if (slice_start < slice_count) slice_count -= slice_start;
    }
    if (slice_start >= total_slices || !slice_count || slice_count > total_slices - slice_start)
        return NULL;
    /* A single-layer view is indexed relative to its cube/array parent. Its
     * native 2D view starts at that parent slice, which need not be slice zero. */
    if ((target == 0 || target == 1 || target == 2 || target == 5 || target == 7) && slice_count != 1)
        return NULL;
    int format = (config->state & SKY_NVN_VIEW_FLAG_FORMAT) ?
        (int)SKY_NVN_VIEW_GET(config->state, 38, 8) : source->format;
    uint32_t pixel_format = 0;
    int format_mapping = nvn_fmt_to_mtl(format, &pixel_format, NULL);
    if (format_mapping != 1 && !(format_mapping == 2 && (format == 0x33 || format == 0x35)))
        return NULL;
    if (pixel_format != base.pixel_format && !(base.usage & SKY_MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW))
        return NULL;
    uint8_t swizzle[4] = {
        (uint8_t)SKY_NVN_VIEW_GET(config->state, 51, 3),
        (uint8_t)SKY_NVN_VIEW_GET(config->state, 54, 3),
        (uint8_t)SKY_NVN_VIEW_GET(config->state, 57, 3),
        (uint8_t)SKY_NVN_VIEW_GET(config->state, 60, 3),
    };
    return sky_mtl_texture_view_create(source->metal, pixel_format, texture_type,
        level_start, level_count, slice_start, slice_count, swizzle);
}

static void sky_nvn_texture_pool_register(void *texture_pool, int id, const void *texture,
                                          const void *view, int image, const char *name) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'));
    if (!pool) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=invalid-pool", name, id);
        return;
    }
    if (id < 0 || id >= pool->count) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=invalid-id", name, id);
        return;
    }
    const sky_nvn_texture_t *source = sky_nvn_texture_valid(texture);
    if (!source || !source->metal) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=invalid-texture", name, id);
        return;
    }
    const sky_nvn_texture_view_t *config = sky_nvn_texture_view_valid(view);
    if (view && !config) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=invalid-view", name, id);
        return;
    }
    void *metal_view = config ? sky_nvn_texture_view_create_metal(source, config) : NULL;
    if (config && !metal_view) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=unsupported-view", name, id);
        return;
    }
    const sky_nvn_memory_pool_t *memory = source->pool;
    if (!memory || memory->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') || !memory->metal) {
        sky_mtl_texture_destroy(metal_view);
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=invalid-texture-pool", name, id);
        return;
    }
    sky_texture_registration_t *entry = calloc(1, sizeof *entry);
    if (entry) {
        entry->memory = *memory;
        entry->memory.metal = sky_mtl_memory_pool_retain(memory->metal);
        entry->texture = *source;
        entry->texture.metal = sky_mtl_texture_retain(source->metal);
        entry->metal_view = metal_view;
        if (!entry->memory.metal || !entry->texture.metal) {
            sky_nvn_texture_registration_release(entry);
            free(entry);
            entry = NULL;
        }
    }
    if (!entry) {
        sky_mtl_texture_destroy(metal_view);
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=%s id=%d reason=resource-retain-failed", name, id);
        return;
    }
    if (config) entry->view = *config;
    entry->has_view = config != NULL;
    entry->pool = texture_pool;
    entry->id = id;
    entry->image = image;
    entry->texture.pool = &entry->memory;
    pthread_mutex_lock(&g_texture_registration_lock);
    sky_texture_registration_t **link = &g_texture_registrations;
    while (*link) {
        sky_texture_registration_t *old = *link;
        if (old->pool == texture_pool && old->id == id) {
            entry->next = old->next;
            *link = entry;
            sky_nvn_texture_registration_index_remove(old);
            sky_nvn_texture_registration_index_add(entry);
            sky_nvn_texture_registration_release(old);
            free(old);
            pthread_mutex_unlock(&g_texture_registration_lock);
            LOG("NVN_TEXTURE_POOL_REGISTERED pool=%p id=%d texture=%p resource_id=%#llx image=%d has_view=%d",
                texture_pool, id, texture, (unsigned long long)sky_mtl_texture_resource_id(source->metal),
                image, entry->has_view);
            return;
        }
        link = &old->next;
    }
    entry->next = g_texture_registrations;
    g_texture_registrations = entry;
    sky_nvn_texture_registration_index_add(entry);
    pthread_mutex_unlock(&g_texture_registration_lock);
    LOG("NVN_TEXTURE_POOL_REGISTERED pool=%p id=%d texture=%p resource_id=%#llx image=%d has_view=%d",
        texture_pool, id, texture, (unsigned long long)sky_mtl_texture_resource_id(source->metal),
        image, entry->has_view);
}

void sky_nvn_texture_pool_register_texture(void *texture_pool, int id, const void *texture, const void *view) {
    sky_nvn_texture_pool_register(texture_pool, id, texture, view, 0, "nvnTexturePoolRegisterTexture");
}

void sky_nvn_texture_pool_register_image(void *texture_pool, int id, const void *texture, const void *view) {
    sky_nvn_texture_pool_register(texture_pool, id, texture, view, 1, "nvnTexturePoolRegisterImage");
}

const void *sky_nvn_texture_pool_get_registered_texture(const void *texture_pool, int id) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'));
    if (!pool || id < 0 || id >= pool->count) return NULL;
    pthread_mutex_lock(&g_texture_registration_lock);
    sky_texture_registration_t *entry = sky_nvn_texture_registration_find(texture_pool, id);
    const sky_nvn_texture_t *texture = entry ? &entry->texture : NULL;
    pthread_mutex_unlock(&g_texture_registration_lock);
    return texture;
}

const void *sky_nvn_texture_pool_get_registered_metal_view(const void *texture_pool, int id) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'));
    if (!pool || id < 0 || id >= pool->count) return NULL;
    pthread_mutex_lock(&g_texture_registration_lock);
    sky_texture_registration_t *entry = sky_nvn_texture_registration_find(texture_pool, id);
    const void *view = entry ? entry->metal_view : NULL;
    pthread_mutex_unlock(&g_texture_registration_lock);
    return view;
}

const void *sky_nvn_texture_pool_get_memory_pool(const void *texture_pool) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'));
    return pool ? pool->pool : NULL;
}

int64_t sky_nvn_texture_pool_get_memory_offset(const void *texture_pool) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'));
    return pool ? pool->offset : 0;
}

int sky_nvn_texture_pool_get_size(const void *texture_pool) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        texture_pool, SKY_NVN_MAGIC('T', 'X', 'P', 'L'));
    return pool ? pool->count : 0;
}

typedef struct sky_sampler_registration {
    struct sky_sampler_registration *next;
    struct sky_sampler_registration *hash_next;
    const void *pool;
    int id;
    sky_mtl_sampler_t *metal;
} sky_sampler_registration_t;
static sky_sampler_registration_t *g_sampler_registrations;
static sky_sampler_registration_t *g_sampler_registration_index[SKY_NVN_DESCRIPTOR_BUCKETS];
static pthread_mutex_t g_sampler_registration_lock = PTHREAD_MUTEX_INITIALIZER;
/* Caller holds g_sampler_registration_lock. */
static sky_sampler_registration_t *sky_nvn_sampler_registration_find(const void *pool, int id) {
    for (sky_sampler_registration_t *entry = g_sampler_registration_index[sky_nvn_descriptor_bucket(pool, id)];
         entry; entry = entry->hash_next)
        if (entry->pool == pool && entry->id == id) return entry;
    return NULL;
}
static void sky_nvn_sampler_registration_index_remove(sky_sampler_registration_t *entry) {
    sky_sampler_registration_t **link = &g_sampler_registration_index[sky_nvn_descriptor_bucket(entry->pool, entry->id)];
    while (*link && *link != entry) link = &(*link)->hash_next;
    if (*link) *link = entry->hash_next;
}
static void sky_nvn_register_metal_sampler(const void *pool, int id, sky_mtl_sampler_t *metal) {
    pthread_mutex_lock(&g_sampler_registration_lock);
    sky_sampler_registration_t *entry = sky_nvn_sampler_registration_find(pool, id);
    if (!entry) {
        entry = calloc(1, sizeof *entry);
        if (entry) {
            entry->pool=pool; entry->id=id; entry->next=g_sampler_registrations; g_sampler_registrations=entry;
            size_t bucket=sky_nvn_descriptor_bucket(pool,id);
            entry->hash_next=g_sampler_registration_index[bucket];g_sampler_registration_index[bucket]=entry;
        }
    }
    if (entry) { sky_mtl_sampler_destroy(entry->metal); entry->metal=sky_mtl_sampler_retain(metal); }
    pthread_mutex_unlock(&g_sampler_registration_lock);
}
/* Caller holds g_sampler_registration_lock through the retain. */
static sky_mtl_sampler_t *sky_nvn_sampler_registered_locked(const void *pool, int id) {
    sky_mtl_sampler_t *result = NULL;
    sky_sampler_registration_t *entry=sky_nvn_sampler_registration_find(pool,id);
    if(entry)result=sky_mtl_sampler_retain(entry->metal);
    return result;
}
static sky_mtl_sampler_t *sky_nvn_sampler_from_builder(const sky_nvn_sampler_builder_t *in) {
    sky_mtl_sampler_properties_t p = {.min_filter=in->min_filter,.mag_filter=in->mag_filter,
        .wrap={in->wrap[0],in->wrap[1],in->wrap[2]},.compare_mode=in->compare_mode,.compare_func=in->compare_func,
        .lod_min=in->lod_min,.lod_max=in->lod_max,.max_anisotropy=in->max_anisotropy,.lod_bias=in->lod_bias,.reduction_filter=in->reduction_filter};
    return sky_mtl_sampler_create(&p);
}

uint32_t sky_nvn_sampler_pool_initialize(void *sampler_pool, const void *pool, int64_t offset, int count) {
    return sky_nvn_descriptor_pool_initialize(sampler_pool, pool, offset, count,
        SKY_NVN_MAGIC('S', 'M', 'P', 'L'), 4096, "NVN_SAMPLER_POOL_INITIALIZED",
        "post_nnMain_nvn_sampler_pool_initialize");
}

void sky_nvn_sampler_pool_finalize(void *sampler_pool) {
    pthread_mutex_lock(&g_sampler_registration_lock);
    sky_sampler_registration_t **link=&g_sampler_registrations;
    while (*link) {
        sky_sampler_registration_t *entry=*link;
        if (entry->pool==sampler_pool) {
            *link=entry->next;sky_nvn_sampler_registration_index_remove(entry);
            sky_mtl_sampler_destroy(entry->metal);free(entry);
        }
        else link=&entry->next;
    }
    pthread_mutex_unlock(&g_sampler_registration_lock);
    sky_nvn_descriptor_pool_finalize(sampler_pool, SKY_NVN_MAGIC('S', 'M', 'P', 'L'),
        "NVN_SAMPLER_POOL_FINALIZED");
}

void sky_nvn_sampler_pool_register_sampler(void *sampler_pool, int id, const void *sampler) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        sampler_pool, SKY_NVN_MAGIC('S', 'M', 'P', 'L'));
    if (!pool) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=nvnSamplerPoolRegisterSampler id=%d reason=invalid-pool", id);
        return;
    }
    if (id < 0 || id >= pool->count) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=nvnSamplerPoolRegisterSampler id=%d reason=invalid-id", id);
        return;
    }
    const sky_nvn_sampler_t *object = sampler;
    if (!object || object->magic != SKY_NVN_MAGIC('S', 'A', 'M', 'P')) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=nvnSamplerPoolRegisterSampler id=%d reason=invalid-sampler", id);
        return;
    }
    sky_nvn_register_metal_sampler(sampler_pool, id, object->metal);
    sky_checkpoint("post_nnMain_nvn_sampler_pool_register_sampler");
    LOG("NVN_SAMPLER_POOL_REGISTERED pool=%p id=%d sampler=%p device=%p",
        sampler_pool, id, sampler, object->dev);
}

void sky_nvn_sampler_pool_register_sampler_builder(void *sampler_pool, int id, const void *builder) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        sampler_pool, SKY_NVN_MAGIC('S', 'M', 'P', 'L'));
    if (!pool) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=nvnSamplerPoolRegisterSamplerBuilder id=%d reason=invalid-pool", id);
        return;
    }
    if (id < 0 || id >= pool->count) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=nvnSamplerPoolRegisterSamplerBuilder id=%d reason=invalid-id", id);
        return;
    }
    const sky_nvn_sampler_builder_t *object = builder;
    if (!object || !object->dev || ((const sky_nvn_device_t *)object->dev)->magic != 0x44455630) {
        LOG("NVN_DESCRIPTOR_REGISTER_ERROR name=nvnSamplerPoolRegisterSamplerBuilder id=%d reason=invalid-sampler-builder", id);
        return;
    }
    sky_mtl_sampler_t *metal=sky_nvn_sampler_from_builder(object);
    if (!metal) return;
    sky_nvn_register_metal_sampler(sampler_pool,id,metal);
    sky_mtl_sampler_destroy(metal);
    sky_checkpoint("post_nnMain_nvn_sampler_pool_register_sampler_builder");
    LOG("NVN_SAMPLER_POOL_REGISTERED pool=%p id=%d sampler_builder=%p device=%p",
        sampler_pool, id, builder, object->dev);
}

const void *sky_nvn_sampler_pool_get_memory_pool(const void *sampler_pool) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        sampler_pool, SKY_NVN_MAGIC('S', 'M', 'P', 'L'));
    return pool ? pool->pool : NULL;
}

int64_t sky_nvn_sampler_pool_get_memory_offset(const void *sampler_pool) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        sampler_pool, SKY_NVN_MAGIC('S', 'M', 'P', 'L'));
    return pool ? pool->offset : 0;
}

int sky_nvn_sampler_pool_get_size(const void *sampler_pool) {
    const sky_nvn_descriptor_pool_t *pool = sky_nvn_descriptor_pool_valid(
        sampler_pool, SKY_NVN_MAGIC('S', 'M', 'P', 'L'));
    return pool ? pool->count : 0;
}

void sky_nvn_sampler_builder_set_device(void *builder, void *device) {
    if (builder) ((sky_nvn_sampler_builder_t *)builder)->dev = device;
}

void sky_nvn_sampler_builder_set_defaults(void *builder) {
    if (!builder) return;
    sky_nvn_sampler_builder_t *out = builder;
    void *device = out->dev;
    memset(out, 0, sizeof *out);
    out->dev = device;
    out->lod_max = 1000.0f;
    out->max_anisotropy = 1.0f;
    out->wrap[0]=out->wrap[1]=out->wrap[2]=1;
    out->compare_func=2;
}

void sky_nvn_sampler_builder_set_min_mag_filter(void *builder, int min_filter, int mag_filter) {
    if (!builder) return;
    sky_nvn_sampler_builder_t *out = builder;
    out->min_filter = min_filter;
    out->mag_filter = mag_filter;
}

void sky_nvn_sampler_builder_set_wrap_mode(void *builder, int s, int t, int r) {
    if (!builder) return;
    sky_nvn_sampler_builder_t *out = builder;
    out->wrap[0] = s;
    out->wrap[1] = t;
    out->wrap[2] = r;
}

void sky_nvn_sampler_builder_set_lod_clamp(void *builder, float minimum, float maximum) {
    if (!builder) return;
    sky_nvn_sampler_builder_t *out = builder;
    out->lod_min = minimum;
    out->lod_max = maximum;
}

void sky_nvn_sampler_builder_set_compare(void *builder, int mode, int function) {
    if (!builder) return;
    sky_nvn_sampler_builder_t *out = builder;
    out->compare_mode = mode;
    out->compare_func = function;
}

void sky_nvn_sampler_builder_set_max_anisotropy(void *builder, float value) {
    if (builder) ((sky_nvn_sampler_builder_t *)builder)->max_anisotropy = value;
}

void sky_nvn_sampler_builder_set_lod_bias(void *builder, float value) {
    if (builder) ((sky_nvn_sampler_builder_t *)builder)->lod_bias = value;
}

void sky_nvn_sampler_builder_set_reduction_filter(void *builder, int filter) {
    if (builder) ((sky_nvn_sampler_builder_t *)builder)->reduction_filter = filter;
}

uint32_t sky_nvn_sampler_initialize(void *sampler, const void *builder) {
    if (!sampler || !builder) return 0;
    const sky_nvn_sampler_builder_t *in = builder;
    if (!in->dev || ((const sky_nvn_device_t *)in->dev)->magic != 0x44455630) {
        LOG("NVN_SAMPLER_INIT_ERROR reason=invalid-device");
        return 0;
    }
    sky_nvn_sampler_t *out = sampler;
    out->metal=sky_nvn_sampler_from_builder(in);
    if (!out->metal) return 0;
    out->magic = SKY_NVN_MAGIC('S', 'A', 'M', 'P');
    out->dev = in->dev;
    sky_checkpoint("post_nnMain_nvn_sampler_initialize");
    LOG("NVN_SAMPLER_INITIALIZED sampler=%p device=%p", sampler, in->dev);
    return 1;
}

void sky_nvn_sampler_finalize(void *sampler) {
    if (!sampler || ((sky_nvn_sampler_t *)sampler)->magic != SKY_NVN_MAGIC('S', 'A', 'M', 'P')) return;
    sky_mtl_sampler_destroy(((sky_nvn_sampler_t *)sampler)->metal);
    memset(sampler, 0, sizeof(sky_nvn_sampler_t));
}

typedef struct { uint64_t target:3, src_rgb:7, dst_rgb:7, src_a:7, dst_a:7, eq_rgb:3, eq_a:3; } sky_blend_state_t;
typedef struct { uint32_t cull:2, front:1, mode:2, offset_en:3; } sky_polygon_state_t;
typedef struct { uint64_t dt:1, dw:1, df:4, st:1, sf_f:4, sf_b:4, fail_f:4, zfail_f:4, zpass_f:4, fail_b:4, zfail_b:4, zpass_b:4; } sky_depth_state_t;
typedef struct { uint32_t format:8, offset:19, stream:5; } sky_vertex_attrib_t;

void sky_nvn_channel_mask_state_set_defaults(void *state) {
    if (state) *(uint32_t *)state = UINT32_MAX;
}

void sky_nvn_channel_mask_state_set_channel_mask(void *state, int index,
    uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (!state || index < 0 || index >= 8) return;
    /* NVNboolean is an ELF uint8 argument. The caller may leave bits 8..31
     * undefined; Darwin's narrow-argument ABI assumes caller extension, so keep
     * a full register carrier here and normalize its low byte explicitly. */
    r=!!(r&0xff);g=!!(g&0xff);b=!!(b&0xff);a=!!(a&0xff);
    uint32_t mask = r | g << 1 | b << 2 | a << 3;
    uint32_t shift = (uint32_t)index * 4;
    uint32_t *out = state;
    *out = (*out & ~(0xfu << shift)) | mask << shift;
}

void sky_nvn_blend_state_set_defaults(void *state) {
    if (state) *(sky_blend_state_t *)state = (sky_blend_state_t){.src_rgb=2, .dst_rgb=1, .src_a=2, .dst_a=1, .eq_rgb=1, .eq_a=1};
}

void sky_nvn_blend_state_set_blend_target(void *state, int target) {
    if (state) ((sky_blend_state_t *)state)->target = target;
}

void sky_nvn_blend_state_set_blend_equation(void *state, int rgb, int alpha) {
    if (state) { ((sky_blend_state_t *)state)->eq_rgb = rgb; ((sky_blend_state_t *)state)->eq_a = alpha; }
}

void sky_nvn_blend_state_set_blend_func(void *state, int src_rgb, int dst_rgb, int src_alpha, int dst_alpha) {
    if (state) { sky_blend_state_t *s = state; s->src_rgb=src_rgb; s->dst_rgb=dst_rgb; s->src_a=src_alpha; s->dst_a=dst_alpha; }
}

void sky_nvn_color_state_set_defaults(void *state) {
    if (state) memset(state, 0, 4);
}

void sky_nvn_color_state_set_blend_enable(void *state, int index, uint32_t enable) {
    if (!state || index < 0 || index >= 8) return;
    uint32_t *out = state;
    if (enable&0xff) *out |= 1u << index;
    else *out &= ~(1u << index);
}

void sky_nvn_color_state_set_alpha_test(void *state, int function) {
    (void)state;
    (void)function;
}

void sky_nvn_polygon_state_set_defaults(void *state) {
    if(state)*(sky_polygon_state_t *)state=(sky_polygon_state_t){.front=1,.mode=2};
}

void sky_nvn_polygon_state_set_front_face(void *state, int face) {
    if (state) ((sky_polygon_state_t *)state)->front = face;
}

void sky_nvn_polygon_state_set_polygon_mode(void *state, int mode) {
    if (state) ((sky_polygon_state_t *)state)->mode = mode;
}

void sky_nvn_polygon_state_set_cull_face(void *state, int face) {
    if (state) ((sky_polygon_state_t *)state)->cull = face;
}

void sky_nvn_polygon_state_set_polygon_offset_enables(void *state, int enables) {
    if (state) ((sky_polygon_state_t *)state)->offset_en = enables;
}

void sky_nvn_depth_stencil_state_set_defaults(void *state) {
    if (state) *(sky_depth_state_t *)state = (sky_depth_state_t){.dw=1,.df=2,.sf_f=8,.sf_b=8,.fail_f=1,.zfail_f=1,.zpass_f=1,.fail_b=1,.zfail_b=1,.zpass_b=1};
}

void sky_nvn_depth_stencil_state_set_depth_test_enable(void *state, uint32_t enable) {
    if (state) ((sky_depth_state_t *)state)->dt = !!(enable&0xff);
}

void sky_nvn_depth_stencil_state_set_stencil_test_enable(void *state, uint32_t enable) {
    if (state) ((sky_depth_state_t *)state)->st = !!(enable&0xff);
}

void sky_nvn_depth_stencil_state_set_stencil_func(void *state, int face, int function) {
    if (state) { sky_depth_state_t *s=state; if (face & 1) s->sf_f=function; if (face & 2) s->sf_b=function; }
}

void sky_nvn_depth_stencil_state_set_depth_write_enable(void *state, uint32_t enable) {
    if (state) ((sky_depth_state_t *)state)->dw = !!(enable&0xff);
}

void sky_nvn_depth_stencil_state_set_depth_func(void *state, int function) {
    if (state) ((sky_depth_state_t *)state)->df = function;
}

void sky_nvn_depth_stencil_state_set_stencil_op(void *state, int face, int sfail, int dpfail, int dppass) {
    if (state) { sky_depth_state_t *s=state; if (face & 1) {s->fail_f=sfail;s->zfail_f=dpfail;s->zpass_f=dppass;} if (face & 2) {s->fail_b=sfail;s->zfail_b=dpfail;s->zpass_b=dppass;} }
}

void sky_nvn_vertex_attrib_state_set_defaults(void *state) {
    if (state) memset(state, 0, 4);
}

void sky_nvn_vertex_attrib_state_set_format(void *state, int format, int64_t offset) {
    if (state && offset >= 0 && offset < (1 << 19)) { ((sky_vertex_attrib_t *)state)->format = format; ((sky_vertex_attrib_t *)state)->offset = (uint32_t)offset; }
}

void sky_nvn_vertex_attrib_state_set_stream_index(void *state, int index) {
    if (state && index >= 0 && index < 16) ((sky_vertex_attrib_t *)state)->stream = (uint32_t)index;
}

void sky_nvn_vertex_stream_state_set_defaults(void *state) {
    if (state) memset(state, 0, 8);
}

void sky_nvn_vertex_stream_state_set_stride(void *state, int64_t stride) {
    if (state) *(int32_t *)state = (int32_t)stride;
}

void sky_nvn_vertex_stream_state_set_divisor(void *state, int divisor) {
    if (state) ((int32_t *)state)[1] = divisor;
}

void sky_nvn_texture_builder_set_defaults(void *builder) {
    if (!builder) return;
    sky_nvn_texture_builder_t *out = builder;
    void *device = out->dev;
    memset(out, 0, sizeof *out);
    out->dev = device;
    out->target = 1;
    out->width = out->height = out->depth = out->levels = 1;
    out->depth_stencil_mode = 0;
    out->swizzle[0] = 2;
    out->swizzle[1] = 3;
    out->swizzle[2] = 4;
    out->swizzle[3] = 5;
}

void sky_nvn_texture_builder_set_device(void *builder, void *device) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->dev = device;
}

void sky_nvn_texture_builder_set_flags(void *builder, int flags) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->flags = flags;
}

void sky_nvn_texture_builder_set_target(void *builder, int target) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->target = target;
}

static uint32_t sky_nvn_texture_dimension(int value) {
    return value > 0 ? (uint32_t)value : 0;
}

void sky_nvn_texture_builder_set_width(void *builder, int width) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->width = sky_nvn_texture_dimension(width);
}

void sky_nvn_texture_builder_set_height(void *builder, int height) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->height = sky_nvn_texture_dimension(height);
}

void sky_nvn_texture_builder_set_depth(void *builder, int depth) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->depth = sky_nvn_texture_dimension(depth);
}

void sky_nvn_texture_builder_set_size_1d(void *builder, int width) {
    if (!builder) return;
    sky_nvn_texture_builder_t *out = builder;
    out->width = sky_nvn_texture_dimension(width);
    out->height = out->depth = 1;
}

void sky_nvn_texture_builder_set_size_2d(void *builder, int width, int height) {
    if (!builder) return;
    sky_nvn_texture_builder_t *out = builder;
    out->width = sky_nvn_texture_dimension(width);
    out->height = sky_nvn_texture_dimension(height);
    out->depth = 1;
}

void sky_nvn_texture_builder_set_size_3d(void *builder, int width, int height, int depth) {
    if (!builder) return;
    sky_nvn_texture_builder_t *out = builder;
    out->width = sky_nvn_texture_dimension(width);
    out->height = sky_nvn_texture_dimension(height);
    out->depth = sky_nvn_texture_dimension(depth);
}

void sky_nvn_texture_builder_set_levels(void *builder, int levels) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->levels = sky_nvn_texture_dimension(levels);
}

void sky_nvn_texture_builder_set_format(void *builder, int format) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->format = format;
}

void sky_nvn_texture_builder_set_samples(void *builder, int samples) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->samples = samples;
}

void sky_nvn_texture_builder_set_swizzle(void *builder, int r, int g, int b, int a) {
    if ((r | g | b | a) < 0 || r > 5 || g > 5 || b > 5 || a > 5) {
        LOG("NVN_TEXTURE_BUILDER_ERROR reason=invalid-swizzle");
        return;
    }
    sky_nvn_texture_builder_t *out = builder;
    out->swizzle[0] = (uint8_t)r;
    out->swizzle[1] = (uint8_t)g;
    out->swizzle[2] = (uint8_t)b;
    out->swizzle[3] = (uint8_t)a;
}

void sky_nvn_texture_builder_set_depth_stencil_mode(void *builder, int mode) {
    if (mode < 0 || mode > 1) {
        LOG("NVN_TEXTURE_BUILDER_ERROR reason=invalid-depth-stencil-mode mode=%d", mode);
        return;
    }
    if (builder) ((sky_nvn_texture_builder_t *)builder)->depth_stencil_mode = mode;
}

void sky_nvn_texture_builder_set_stride(void *builder, int64_t stride) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->stride = stride;
}

void sky_nvn_texture_builder_set_storage(void *builder, const void *pool, int64_t offset) {
    if (!builder) return;
    sky_nvn_texture_builder_t *out = builder;
    out->pool = pool;
    out->offset = offset;
}

void sky_nvn_texture_builder_set_packaged_texture_data(void *builder, const void *data) {
    if (builder) ((sky_nvn_texture_builder_t *)builder)->packaged_data = data;
}

void sky_nvn_texture_builder_set_packaged_texture_layout(void *builder, const void *layout) {
    if (!builder) return;
    sky_nvn_texture_builder_t *out = builder;
    out->has_layout = layout != NULL;
    if (layout) {
        memcpy(out->layout, layout, 8);
        out->pkg_block_h = out->layout[0] + 1;
    } else {
        memset(out->layout, 0, 8);
        out->pkg_block_h = 0;
    }
}

void *sky_nvn_texture_builder_get_device(const void *builder) {
    return builder ? ((const sky_nvn_texture_builder_t *)builder)->dev : NULL;
}

#define SKY_TEXTURE_BUILDER_GETTER(name, field) \
    int sky_nvn_texture_builder_get_##name(const void *builder) { \
        return builder ? ((const sky_nvn_texture_builder_t *)builder)->field : 0; \
    }
SKY_TEXTURE_BUILDER_GETTER(flags, flags)
SKY_TEXTURE_BUILDER_GETTER(target, target)
SKY_TEXTURE_BUILDER_GETTER(width, width)
SKY_TEXTURE_BUILDER_GETTER(height, height)
SKY_TEXTURE_BUILDER_GETTER(depth, depth)
SKY_TEXTURE_BUILDER_GETTER(levels, levels)
SKY_TEXTURE_BUILDER_GETTER(format, format)
SKY_TEXTURE_BUILDER_GETTER(samples, samples)

void sky_nvn_texture_builder_get_swizzle(const void *builder, int *r, int *g, int *b, int *a) {
    const sky_nvn_texture_builder_t *out = builder;
    if (!out) return;
    if (r) *r = out->swizzle[0];
    if (g) *g = out->swizzle[1];
    if (b) *b = out->swizzle[2];
    if (a) *a = out->swizzle[3];
}

int sky_nvn_texture_builder_get_depth_stencil_mode(const void *builder) {
    return builder ? ((const sky_nvn_texture_builder_t *)builder)->depth_stencil_mode : 0;
}

int64_t sky_nvn_texture_builder_get_stride(const void *builder) {
    return builder ? ((const sky_nvn_texture_builder_t *)builder)->stride : 0;
}

const void *sky_nvn_texture_builder_get_memory_pool(const void *builder) {
    return builder ? ((const sky_nvn_texture_builder_t *)builder)->pool : NULL;
}

int64_t sky_nvn_texture_builder_get_memory_offset(const void *builder) {
    return builder ? ((const sky_nvn_texture_builder_t *)builder)->offset : 0;
}

static nvn_texdesc sky_nvn_texture_make_desc(const sky_nvn_texture_builder_t *builder) {
    nvn_texdesc desc = {
        builder->target, builder->format, builder->flags, builder->samples,
        builder->width, builder->height, builder->depth, builder->levels,
        (uint32_t)builder->stride, builder->pkg_block_h,
    };
    return desc;
}

uint64_t sky_nvn_texture_builder_get_storage_size(const void *builder) {
    if (!builder) return 0;
    const sky_nvn_texture_builder_t *in = builder;
    if (in->stride < 0 || in->stride > UINT32_MAX) return 0;
    nvn_texdesc desc = sky_nvn_texture_make_desc(in);
    nvn_texlayout layout;
    nvn_tex_layout(&desc, &layout);
    if (!layout.bpb) LOG("NVN_TEXTURE_LAYOUT_ERROR reason=unsupported-format format=%#x", in->format);
    return layout.size;
}

uint64_t sky_nvn_texture_builder_get_storage_alignment(const void *builder) {
    if (!builder) return 0;
    const sky_nvn_texture_builder_t *in = builder;
    if (in->stride < 0 || in->stride > UINT32_MAX) return 0;
    nvn_texdesc desc = sky_nvn_texture_make_desc(in);
    nvn_texlayout layout;
    nvn_tex_layout(&desc, &layout);
    return layout.align;
}

static int sky_nvn_texture_upload_packaged(const sky_nvn_texture_builder_t *in,
    const nvn_texdesc *desc, const nvn_texlayout *layout, void *metal) {
    if (!layout->size || layout->size > UINT32_MAX) return 0;
    uint8_t *packed = malloc(layout->size);
    if (!packed) return 0;
    vm_size_t copied = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(), (vm_address_t)in->packaged_data,
                                             (vm_size_t)layout->size, (vm_address_t)packed, &copied);
    if (result != KERN_SUCCESS || copied != layout->size) {
        free(packed);
        return 0;
    }
    int valid = 0;
    for (unsigned level = 0; level < in->levels; level++) {
        for (unsigned layer = 0; layer < layout->layers; layer++) {
            uint32_t row_bytes, rows, slices, bpb;
            uint64_t base;
            unsigned tile_h, tile_d;
            if (nvn_deswizzle_geom(desc, level, layer, &row_bytes, &rows, &slices,
                                   &base, &tile_h, &tile_d, &bpb)) goto done;
            if ((row_bytes && rows > SIZE_MAX / row_bytes) ||
                slices > SIZE_MAX / (row_bytes * rows)) goto done;
            size_t linear_size = (size_t)row_bytes * rows * slices;
            uint8_t *linear = malloc(linear_size);
            if (!linear) goto done;
            int region[6] = {0, 0, 0, 0, 0, (int)slices};
            uint32_t width = in->width >> level;
            uint32_t height = in->height >> level;
            region[3] = (int)(width ? width : 1);
            region[4] = (int)(height ? height : 1);
            int uploaded = linear_size &&
                !nvn_deswizzle(desc, level, layer, packed, linear, row_bytes) &&
                sky_mtl_texture_upload_packaged(metal, in->format, level, layer, region, linear);
            free(linear);
            if (!uploaded) goto done;
        }
    }
    valid = 1;
done:
    free(packed);
    return valid;
}

static const sky_nvn_texture_t *sky_nvn_texture_valid(const void *texture) {
    if (!texture) return NULL;
    const sky_nvn_texture_t *out = texture;
    return out->magic == SKY_NVN_MAGIC('T', 'E', 'X', 0) ? out : NULL;
}

uint32_t sky_nvn_texture_initialize(void *texture, const void *builder) {
    if (!texture || !builder) return 0;
    const sky_nvn_texture_builder_t *in = builder;
    sky_nvn_texture_t *out = texture;
    memset(out, 0, sizeof *out);
    if (!in->dev || ((const sky_nvn_device_t *)in->dev)->magic != 0x44455630) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-device device=%p", in->dev);
        return 0;
    }
    if (!in->width || !in->height || !in->depth || !in->levels || in->samples < 0) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-dimensions");
        return 0;
    }
    if (in->target < 0 || in->target > 10 || in->target == 10) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=unsupported-target target=%d", in->target);
        return 0;
    }
    if ((in->target == 5 || in->target == 6) && (in->samples < 2 || in->levels != 1)) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-multisample samples=%d levels=%u", in->samples, in->levels);
        return 0;
    }
    if (in->target != 5 && in->target != 6 && in->samples > 1) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-samples target=%d samples=%d", in->target, in->samples);
        return 0;
    }
    if (in->flags & (0x2 | 0x20 | 0x80 | 0x100 | 0x400)) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=unsupported-flags flags=%#x", in->flags);
        return 0;
    }
    if (in->stride < 0 || in->stride > UINT32_MAX) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-stride stride=%lld", (long long)in->stride);
        return 0;
    }
    nvn_texdesc desc = sky_nvn_texture_make_desc(in);
    nvn_texlayout layout;
    nvn_tex_layout(&desc, &layout);
    if (!layout.bpb || !layout.size) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=unsupported-format format=%#x", in->format);
        return 0;
    }
    const sky_nvn_memory_pool_t *memory = in->pool;
    if (!memory || memory->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') ||
        memory->dev != in->dev || !memory->metal || !memory->mem || !memory->buffer_address) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-memory-pool");
        return 0;
    }
    if (in->offset < 0 || (uint64_t)in->offset % layout.align) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-alignment offset=%#llx alignment=%u",
            (unsigned long long)in->offset, layout.align);
        return 0;
    }
    uint64_t offset = (uint64_t)in->offset;
    if (offset > memory->size || layout.size > memory->size - offset ||
        offset > UINT64_MAX - memory->buffer_address) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=invalid-storage offset=%#llx storage_size=%#llx pool_size=%#llx",
            (unsigned long long)offset, (unsigned long long)layout.size,
            (unsigned long long)memory->size);
        return 0;
    }
    uint32_t pixel_format = 0;
    int format_mapping = nvn_fmt_to_mtl(in->format, &pixel_format, NULL);
    if (format_mapping != 1 && !(format_mapping == 2 && (in->format == 0x33 || in->format == 0x35))) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=unsupported-format format=%#x", in->format);
        return 0;
    }
    if (format_mapping == 2)
        LOG("NVN_TEXTURE_FORMAT_CONVERSION format=%#x pixel_format=%u reason=render-target-depth-upgrade-no-upload",
            in->format, pixel_format);
    sky_mtl_texture_properties_t properties = {
        .pixel_format = pixel_format,
        .texture_type = SKY_MTL_TEXTURE_TYPE_2D,
        .storage_mode = SKY_MTL_STORAGE_PRIVATE,
        .width = in->width,
        .height = in->height,
        .depth = 1,
        .array_length = 1,
        .mipmap_level_count = in->levels,
        .sample_count = in->samples ? (uint64_t)in->samples : 1,
        .usage = SKY_MTL_TEXTURE_USAGE_SHADER_READ,
    };
    memcpy(properties.swizzle, in->swizzle, sizeof properties.swizzle);
    switch (in->target) {
        case 0: properties.texture_type = SKY_MTL_TEXTURE_TYPE_1D; properties.height = 1; break;
        case 1: case 7: break;
        case 2: properties.texture_type = SKY_MTL_TEXTURE_TYPE_3D; properties.depth = in->depth; break;
        case 3:
            properties.texture_type = SKY_MTL_TEXTURE_TYPE_1D_ARRAY;
            properties.array_length = in->height;
            properties.height = 1;
            break;
        case 4: properties.texture_type = SKY_MTL_TEXTURE_TYPE_2D_ARRAY; properties.array_length = in->depth; break;
        case 5:
            properties.texture_type = SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE;
            properties.mipmap_level_count = 1;
            break;
        case 6:
            properties.texture_type = SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE_ARRAY;
            properties.array_length = in->depth;
            properties.mipmap_level_count = 1;
            break;
        case 8: properties.texture_type = SKY_MTL_TEXTURE_TYPE_CUBE; break;
        case 9: properties.texture_type = SKY_MTL_TEXTURE_TYPE_CUBE_ARRAY; properties.array_length = in->depth; break;
    }
    int compressed = (in->format >= 0x41 && in->format <= 0x50) ||
                     (in->format >= 0x79 && in->format <= 0x94);
    int shared_linear=(in->flags&0x10)&&!(in->flags&(0x40|0x4))&&in->format==1&&in->target==1&&
        in->levels==1&&in->samples<=1&&!in->has_layout&&!in->packaged_data;
    if (compressed) properties.usage |= SKY_MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW;
    if (!compressed && !in->has_layout && !shared_linear) properties.usage |= SKY_MTL_TEXTURE_USAGE_RENDER_TARGET;
    if (in->flags & 0x4) properties.usage |= SKY_MTL_TEXTURE_USAGE_SHADER_WRITE;
    int custom_swizzle = in->swizzle[0] != 2 || in->swizzle[1] != 3 ||
                         in->swizzle[2] != 4 || in->swizzle[3] != 5;
    if (custom_swizzle && (properties.usage & SKY_MTL_TEXTURE_USAGE_RENDER_TARGET)) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=swizzle-render-target-incompatible");
        return 0;
    }
    uint64_t linear_stride=in->stride?(uint64_t)in->stride:((uint64_t)in->width+31)&~31ull;
    void *metal=shared_linear?sky_mtl_texture_create_linear_from_buffer(memory->buffer_address+offset,pixel_format,
        in->width,in->height,linear_stride):sky_mtl_texture_create(&properties);
    if(shared_linear&&metal&&custom_swizzle) {
        void *swizzled=sky_mtl_texture_view_create(metal,pixel_format,SKY_MTL_TEXTURE_TYPE_2D,0,1,0,1,in->swizzle);
        sky_mtl_texture_destroy(metal);metal=swizzled;
    }
    if (!metal) {
        LOG("NVN_TEXTURE_INIT_ERROR reason=metal-create-failed format=%#x target=%d", in->format, in->target);
        return 0;
    }
    if(shared_linear) {
        sky_mtl_texture_get_properties(metal,&properties);
        int mirror=sky_mtl_linear_texture_needs_sync(sky_mtl_texture_native_handle(metal));
        LOG("NVN_TEXTURE_SHARED_LINEAR texture=%p memory=%p offset=%#llx row_stride=%llu cpu_gpu_alias=%d private_mirror=%d",texture,memory->mem,
            (unsigned long long)offset,(unsigned long long)linear_stride,!mirror,mirror);
    }
    if (in->packaged_data && !sky_nvn_texture_upload_packaged(in, &desc, &layout, metal)) {
        sky_mtl_texture_destroy(metal);
        LOG("NVN_TEXTURE_INIT_ERROR reason=packaged-upload-failed format=%#x target=%d", in->format, in->target);
        return 0;
    }
    if (in->packaged_data)
        LOG("NVN_TEXTURE_PACKAGED_UPLOAD texture=%p bytes=%#llx levels=%u layers=%u",
            texture, (unsigned long long)layout.size, in->levels, layout.layers);
    out->magic = SKY_NVN_MAGIC('T', 'E', 'X', 0);
    out->dev = in->dev;
    out->target = in->target;
    out->format = in->format;
    out->flags = in->flags;
    out->samples = in->samples;
    out->width = in->width;
    out->height = in->height;
    out->depth = in->depth;
    out->levels = in->levels;
    out->stride = shared_linear?(int64_t)linear_stride:in->stride;
    out->pkg_block_h = in->pkg_block_h;
    out->pool = in->pool;
    out->offset = in->offset;
    out->storage_size = layout.size;
    out->storage_address = memory->buffer_address + offset;
    out->storage_alignment = layout.align;
    out->layers = layout.layers;
    out->metal = metal;
    out->has_layout = in->has_layout;
    out->depth_stencil_mode = (uint8_t)in->depth_stencil_mode;
    memcpy(out->swizzle, in->swizzle, sizeof out->swizzle);
    memcpy(out->layout, in->layout, 8);
    sky_checkpoint("post_nnMain_nvn_texture_initialize");
    LOG("NVN_TEXTURE_INITIALIZED texture=%p target=%d dimensions=%ux%ux%u levels=%u format=%#x flags=%#x storage_size=%#llx stride=%lld metal_texture=1 pixel_format=%u texture_type=%u usage=%#llx storage=%s resource_id=%#llx has_layout=%d packaged_data=%d",
        texture, out->target, out->width, out->height, out->depth,
        out->levels, out->format, out->flags, (unsigned long long)out->storage_size, (long long)out->stride,
        properties.pixel_format, properties.texture_type, (unsigned long long)properties.usage,shared_linear?"shared-linear":"private",
        (unsigned long long)sky_mtl_texture_resource_id(metal),in->has_layout,in->packaged_data!=NULL);
    return 1;
}

void sky_nvn_texture_finalize(void *texture) {
    const sky_nvn_texture_t *valid = sky_nvn_texture_valid(texture);
    if (!valid) return;
    sky_nvn_texture_t *out = texture;
    LOG("NVN_TEXTURE_FINALIZED texture=%p resource_id=%#llx", texture,
        (unsigned long long)sky_mtl_texture_resource_id(out->metal));
    sky_mtl_texture_destroy(out->metal);
    memset(out, 0, sizeof *out);
}

void sky_nvn_texture_write_texels(const void *texture, const void *view,
    const sky_nvn_copy_region_t *region, const void *data) {
    const sky_nvn_texture_t *source = sky_nvn_texture_valid(texture);
    const sky_nvn_texture_view_t *config = sky_nvn_texture_view_valid(view);
    void *metal = source ? source->metal : NULL;
    int view_owned = 0;
    int format=source?source->format:0,view_format=0;
    if(config&&sky_nvn_texture_view_get_format(config,&view_format))format=view_format;
    if (config && source) {
        metal = sky_nvn_texture_view_create_metal(source, config);
        view_owned = 1;
    }
    if (!source || !metal || !region || !data || (view && !config) ||
        !sky_mtl_texture_upload_packaged(metal,format,0,0,(const int *)region,data)) {
        if (view_owned) sky_mtl_texture_destroy(metal);
        g_last_name = "nvnTextureWriteTexels";
        LOG("NVN_RESOURCE_BOUNDARY name=nvnTextureWriteTexels reason=TEXTURE_UPLOAD_REQUIRED texture=%p view=%p region=%p data=%p",
            texture, view, region, data);
        sky_nvn_unimplemented();
        return;
    }
    if (view_owned) sky_mtl_texture_destroy(metal);
    sky_checkpoint("post_nnMain_nvn_texture_write_texels");
    LOG("NVN_TEXTURE_WRITE texture=%p view=%p origin=%d,%d,%d extent=%dx%dx%d",
        texture, view, region->xoffset, region->yoffset, region->zoffset,
        region->width, region->height, region->depth);
}

void sky_nvn_texture_set_debug_label(void *texture, const char *label) {
    sky_nvn_texture_t *out = (sky_nvn_texture_t *)texture;
    if (!sky_nvn_texture_valid(out)) return;
    sky_mtl_texture_set_debug_label(out->metal, label);
}

#define SKY_TEXTURE_GETTER(name, field) \
    int sky_nvn_texture_get_##name(const void *texture) { \
        const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture); \
        return out ? out->field : 0; \
    }
SKY_TEXTURE_GETTER(flags, flags)
SKY_TEXTURE_GETTER(target, target)
SKY_TEXTURE_GETTER(width, width)
SKY_TEXTURE_GETTER(height, height)
SKY_TEXTURE_GETTER(depth, depth)
SKY_TEXTURE_GETTER(levels, levels)
SKY_TEXTURE_GETTER(format, format)
SKY_TEXTURE_GETTER(samples, samples)

void sky_nvn_texture_get_swizzle(const void *texture, int *r, int *g, int *b, int *a) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    if (!out) return;
    if (r) *r = out->swizzle[0];
    if (g) *g = out->swizzle[1];
    if (b) *b = out->swizzle[2];
    if (a) *a = out->swizzle[3];
}

int sky_nvn_texture_get_depth_stencil_mode(const void *texture) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    return out ? out->depth_stencil_mode : 0;
}

int64_t sky_nvn_texture_get_stride(const void *texture) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    return out ? out->stride : 0;
}

const void *sky_nvn_texture_get_memory_pool(const void *texture) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    return out ? out->pool : NULL;
}

int64_t sky_nvn_texture_get_memory_offset(const void *texture) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    return out ? out->offset : 0;
}

int sky_nvn_texture_get_storage_size(const void *texture) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    return out ? (int)out->storage_size : 0;
}

uint64_t sky_nvn_texture_get_texture_address(const void *texture) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    return out ? out->storage_address : 0;
}

static const sky_nvn_texture_view_t *sky_nvn_texture_view_valid(const void *view) {
    if (!view) return NULL;
    const sky_nvn_texture_view_t *out = view;
    return out->magic == SKY_NVN_MAGIC('V', 'I', 'E', 'W') ? out : NULL;
}

static void sky_nvn_texture_view_store(sky_nvn_texture_view_t *out, uint64_t flag,
                                       unsigned shift, unsigned bits, int value) {
    if (!out || value < 0 || (uint64_t)value >= (1ull << bits)) {
        LOG("NVN_TEXTURE_VIEW_ERROR reason=invalid-field value=%d", value);
        return;
    }
    out->state |= flag;
    uint64_t mask = ((1ull << bits) - 1) << shift;
    out->state = (out->state & ~mask) | SKY_NVN_VIEW_FIELD(value, shift, bits);
}

void sky_nvn_texture_view_set_defaults(void *view) {
    if (!view) return;
    sky_nvn_texture_view_t *out = view;
    out->magic = SKY_NVN_MAGIC('V', 'I', 'E', 'W');
    out->state = SKY_NVN_VIEW_FIELD(2, 51, 3) | SKY_NVN_VIEW_FIELD(3, 54, 3) |
                 SKY_NVN_VIEW_FIELD(4, 57, 3) | SKY_NVN_VIEW_FIELD(5, 60, 3);
}

void sky_nvn_texture_view_set_levels(void *view, int base, int count) {
    sky_nvn_texture_view_store(view, SKY_NVN_VIEW_FLAG_BIT, 6, 4, base);
    if (base >= 0 && base < 16) sky_nvn_texture_view_store(view, 0, 10, 4, count);
}

void sky_nvn_texture_view_set_layers(void *view, int minimum, int count) {
    sky_nvn_texture_view_store(view, SKY_NVN_VIEW_FLAG_LAYER, 14, 12, minimum);
    if (minimum >= 0 && minimum < 4096) sky_nvn_texture_view_store(view, 0, 26, 12, count);
}

void sky_nvn_texture_view_set_format(void *view, int format) {
    if (format < 0 || format > 0x9c) {
        LOG("NVN_TEXTURE_VIEW_ERROR reason=invalid-format format=%#x", format);
        return;
    }
    sky_nvn_texture_view_store(view, SKY_NVN_VIEW_FLAG_FORMAT, 38, 8, format);
}

void sky_nvn_texture_view_set_swizzle(void *view, int r, int g, int b, int a) {
    if ((r | g | b | a) < 0 || r > 5 || g > 5 || b > 5 || a > 5) {
        LOG("NVN_TEXTURE_VIEW_ERROR reason=invalid-swizzle");
        return;
    }
    sky_nvn_texture_view_store(view, SKY_NVN_VIEW_FLAG_SWIZZLE, 51, 3, r);
    sky_nvn_texture_view_store(view, 0, 54, 3, g);
    sky_nvn_texture_view_store(view, 0, 57, 3, b);
    sky_nvn_texture_view_store(view, 0, 60, 3, a);
}

void sky_nvn_texture_view_set_depth_stencil_mode(void *view, int mode) {
    sky_nvn_texture_view_store(view, SKY_NVN_VIEW_FLAG_DSM, 50, 1, mode);
}

void sky_nvn_texture_view_set_target(void *view, int target) {
    if (target < 0 || target > 10) {
        LOG("NVN_TEXTURE_VIEW_ERROR reason=invalid-target target=%d", target);
        return;
    }
    sky_nvn_texture_view_store(view, SKY_NVN_VIEW_FLAG_TARGET, 46, 4, target);
}

uint32_t sky_nvn_texture_view_get_levels(const void *view, int *base, int *count) {
    const sky_nvn_texture_view_t *out = sky_nvn_texture_view_valid(view);
    if (!out) return 0;
    if (base) *base = (int)SKY_NVN_VIEW_GET(out->state, 6, 4);
    if (count) *count = (int)SKY_NVN_VIEW_GET(out->state, 10, 4);
    return (out->state & SKY_NVN_VIEW_FLAG_BIT) != 0;
}

uint32_t sky_nvn_texture_view_get_layers(const void *view, int *minimum, int *count) {
    const sky_nvn_texture_view_t *out = sky_nvn_texture_view_valid(view);
    if (!out) return 0;
    if (minimum) *minimum = (int)SKY_NVN_VIEW_GET(out->state, 14, 12);
    if (count) *count = (int)SKY_NVN_VIEW_GET(out->state, 26, 12);
    return (out->state & SKY_NVN_VIEW_FLAG_LAYER) != 0;
}

uint32_t sky_nvn_texture_view_get_format(const void *view, int *format) {
    const sky_nvn_texture_view_t *out = sky_nvn_texture_view_valid(view);
    if (!out) return 0;
    if (format) *format = (int)SKY_NVN_VIEW_GET(out->state, 38, 8);
    return (out->state & SKY_NVN_VIEW_FLAG_FORMAT) != 0;
}

uint32_t sky_nvn_texture_view_get_swizzle(const void *view, int *r, int *g, int *b, int *a) {
    const sky_nvn_texture_view_t *out = sky_nvn_texture_view_valid(view);
    if (!out) return 0;
    if (r) *r = (int)SKY_NVN_VIEW_GET(out->state, 51, 3);
    if (g) *g = (int)SKY_NVN_VIEW_GET(out->state, 54, 3);
    if (b) *b = (int)SKY_NVN_VIEW_GET(out->state, 57, 3);
    if (a) *a = (int)SKY_NVN_VIEW_GET(out->state, 60, 3);
    return (out->state & SKY_NVN_VIEW_FLAG_SWIZZLE) != 0;
}

uint32_t sky_nvn_texture_view_get_depth_stencil_mode(const void *view, int *mode) {
    const sky_nvn_texture_view_t *out = sky_nvn_texture_view_valid(view);
    if (!out) return 0;
    if (mode) *mode = (int)SKY_NVN_VIEW_GET(out->state, 50, 1);
    return (out->state & SKY_NVN_VIEW_FLAG_DSM) != 0;
}

uint32_t sky_nvn_texture_view_get_target(const void *view, int *target) {
    const sky_nvn_texture_view_t *out = sky_nvn_texture_view_valid(view);
    if (!out) return 0;
    if (target) *target = (int)SKY_NVN_VIEW_GET(out->state, 46, 4);
    return (out->state & SKY_NVN_VIEW_FLAG_TARGET) != 0;
}

uint32_t sky_nvn_texture_view_compare(const void *view, const void *other) {
    const sky_nvn_texture_view_t *a = sky_nvn_texture_view_valid(view);
    const sky_nvn_texture_view_t *b = sky_nvn_texture_view_valid(other);
    return a && b && a->state == b->state;
}

int64_t sky_nvn_texture_get_view_offset(const void *texture, const void *view) {
    const sky_nvn_texture_t *out = sky_nvn_texture_valid(texture);
    if (!out) return 0;
    const sky_nvn_texture_view_t *config = sky_nvn_texture_view_valid(view);
    if (!config) return 0;
    nvn_texdesc desc = {
        out->target, out->format, out->flags, out->samples, out->width, out->height,
        out->depth, out->levels, (uint32_t)out->stride, out->pkg_block_h,
    };
    uint64_t offset = 0;
    if (config->state & SKY_NVN_VIEW_FLAG_BIT)
        offset = nvn_tex_level_offset(&desc, (unsigned)SKY_NVN_VIEW_GET(config->state, 6, 4));
    if (config->state & SKY_NVN_VIEW_FLAG_LAYER) {
        nvn_texlayout layout;
        nvn_tex_layout(&desc, &layout);
        offset += SKY_NVN_VIEW_GET(config->state, 14, 12) * layout.layer_size;
    }
    return (int64_t)offset;
}

void sky_nvn_queue_builder_set_defaults(void *builder) {
    if (!builder) return;
    sky_nvn_queue_builder_t *out = builder;
    void *device = out->dev;
    memset(out, 0, sizeof *out);
    out->dev = device;
    out->command_size = 65536;
    out->compute_size = 262144;
    out->control_size = 16384;
    out->flush_threshold = 4096;
    sky_checkpoint("post_nnMain_nvn_queue_builder_defaults");
    LOG("NVN_QUEUE_BUILDER_DEFAULTS device=%p", device);
}

void sky_nvn_queue_builder_set_device(void *builder, void *device) {
    if (builder) ((sky_nvn_queue_builder_t *)builder)->dev = device;
}

void sky_nvn_queue_builder_set_flags(void *builder, int flags) {
    if (builder) ((sky_nvn_queue_builder_t *)builder)->flags = (uint32_t)flags;
}

void sky_nvn_queue_builder_set_command_memory_size(void *builder, uint64_t size) {
    if (builder) ((sky_nvn_queue_builder_t *)builder)->command_size = size;
}

void sky_nvn_queue_builder_set_compute_memory_size(void *builder, uint64_t size) {
    if (builder) ((sky_nvn_queue_builder_t *)builder)->compute_size = size;
}

void sky_nvn_queue_builder_set_control_memory_size(void *builder, uint64_t size) {
    if (builder) ((sky_nvn_queue_builder_t *)builder)->control_size = size;
}

void sky_nvn_queue_builder_set_command_flush_threshold(void *builder, uint64_t size) {
    if (builder) ((sky_nvn_queue_builder_t *)builder)->flush_threshold = size;
}

uint64_t sky_nvn_queue_builder_get_queue_memory_size(const void *builder) {
    if (!builder) return 0;
    const sky_nvn_queue_builder_t *in = builder;
    return (in->command_size + in->compute_size + in->control_size + 4095) & ~(uint64_t)4095;
}

void sky_nvn_queue_builder_set_queue_memory(void *builder, void *memory, uint64_t size) {
    if (!builder) return;
    sky_nvn_queue_builder_t *out = builder;
    out->memory = memory;
    out->memory_size = size;
}

uint32_t sky_nvn_queue_initialize(void *queue, const void *builder) {
    if (!queue || !builder) return 0;
    const sky_nvn_queue_builder_t *in = builder;
    sky_nvn_queue_t *out = queue;
    memset(out, 0, sizeof *out);
    if (!in->dev || ((const sky_nvn_device_t *)in->dev)->magic != 0x44455630) {
        LOG("NVN_QUEUE_INIT_ERROR reason=invalid-device device=%p", in->dev);
        return 0;
    }
    uint64_t required = sky_nvn_queue_builder_get_queue_memory_size(in);
    if (!in->memory || in->memory_size < required) {
        LOG("NVN_QUEUE_INIT_ERROR reason=invalid-memory memory=%p size=%#llx required=%#llx",
            in->memory, (unsigned long long)in->memory_size, (unsigned long long)required);
        return 0;
    }
    void *metal = sky_mtl_command_queue_create();
    if (!metal) {
        LOG("NVN_QUEUE_INIT_ERROR reason=metal-command-queue-failed");
        return 0;
    }
    void *metal_memory = sky_mtl_memory_pool_create(in->memory, in->memory_size);
    if (!metal_memory) {
        sky_mtl_command_queue_destroy(metal);
        LOG("NVN_QUEUE_INIT_ERROR reason=metal-memory-failed memory=%p size=%#llx",
            in->memory, (unsigned long long)in->memory_size);
        return 0;
    }
    out->magic = SKY_NVN_MAGIC('Q', 'U', 'E', 'U');
    out->dev = in->dev;
    out->flags = in->flags;
    out->command_size = in->command_size;
    out->compute_size = in->compute_size;
    out->control_size = in->control_size;
    out->flush_threshold = in->flush_threshold;
    out->memory = in->memory;
    out->memory_size = in->memory_size;
    out->metal = metal;
    out->metal_memory = metal_memory;
    int mutex_error = pthread_mutex_init(&out->lock, NULL);
    int cond_error = pthread_cond_init(&out->cv, NULL);
    if (mutex_error || cond_error) {
        if (!mutex_error) pthread_mutex_destroy(&out->lock);
        sky_mtl_memory_pool_destroy(metal_memory);
        sky_mtl_command_queue_destroy(metal);
        LOG("NVN_QUEUE_INIT_ERROR reason=sync-primitive-failed");
        return 0;
    }
    sky_checkpoint("post_nnMain_nvn_queue_initialize");
    LOG("NVN_QUEUE_INITIALIZED queue=%p device=%p flags=%#x command=%llu compute=%llu control=%llu flush=%llu memory=%p memory_size=%llu metal_command_queue=1 memory_metal=shared memory_gpu_address=%#llx",
        queue, out->dev, out->flags,
        (unsigned long long)out->command_size, (unsigned long long)out->compute_size,
        (unsigned long long)out->control_size, (unsigned long long)out->flush_threshold,
        out->memory, (unsigned long long)out->memory_size,
        (unsigned long long)sky_mtl_memory_pool_gpu_address(metal_memory));
    return 1;
}

static sky_nvn_queue_t *sky_nvn_queue_valid(void *queue) {
    if (!queue) return NULL;
    sky_nvn_queue_t *out = queue;
    return out->magic == SKY_NVN_MAGIC('Q', 'U', 'E', 'U') ? out : NULL;
}

static int sky_nvn_queue_wait(sky_nvn_queue_t *queue, uint64_t fence, uint64_t timeout_ns,
                              int *already_signaled) {
    int already = 0;
    pthread_mutex_lock(&queue->lock);
    if (queue->completed >= fence) already = 1;
    if (!already && timeout_ns) {
        struct timespec deadline;
        if (timeout_ns == UINT64_MAX) {
            while (queue->completed < fence) pthread_cond_wait(&queue->cv, &queue->lock);
        } else {
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += (time_t)(timeout_ns / 1000000000ull);
            deadline.tv_nsec += (long)(timeout_ns % 1000000000ull);
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            while (queue->completed < fence) {
                int error = pthread_cond_timedwait(&queue->cv, &queue->lock, &deadline);
                if (error) break;
            }
        }
    }
    int satisfied = queue->completed >= fence;
    pthread_mutex_unlock(&queue->lock);
    if (already_signaled) *already_signaled = already;
    return satisfied;
}

static void sky_nvn_queue_complete(void *context) {
    sky_nvn_queue_t *queue = context;
    pthread_mutex_lock(&queue->lock);
    queue->completed++;
    pthread_cond_broadcast(&queue->cv);
    pthread_mutex_unlock(&queue->lock);
}

uint32_t sky_nvn_queue_submit_empty_for_testing(void *queue, uint64_t delay_ns) {
    sky_nvn_queue_t *out = sky_nvn_queue_valid(queue);
    if (!out) return 0;
    pthread_mutex_lock(&out->lock);
    out->submits++;
    pthread_mutex_unlock(&out->lock);
    if (sky_mtl_command_queue_submit_empty(out->metal, delay_ns, sky_nvn_queue_complete, out))
        return 1;
    pthread_mutex_lock(&out->lock);
    out->submits--;
    pthread_mutex_unlock(&out->lock);
    return 0;
}

void sky_nvn_queue_finalize(void *queue) {
    sky_nvn_queue_t *out = sky_nvn_queue_valid(queue);
    if (!out) return;
    sky_nvn_queue_wait(out, out->submits, UINT64_MAX, NULL);
    LOG("NVN_QUEUE_FINALIZED queue=%p submits=%llu completed=%llu", queue,
        (unsigned long long)out->submits, (unsigned long long)out->completed);
    sky_mtl_memory_pool_destroy(out->metal_memory);
    sky_mtl_command_queue_destroy(out->metal);
    pthread_cond_destroy(&out->cv);
    pthread_mutex_destroy(&out->lock);
    memset(out, 0, sizeof *out);
}

void sky_nvn_queue_fence_sync(void *queue, void *sync, int condition, int flags) {
    sky_nvn_queue_t *out = sky_nvn_queue_valid(queue);
    sky_nvn_sync_t *target = sync;
    if (!out || !target || target->magic != SKY_NVN_MAGIC('S', 'Y', 'N', 'C') || condition < 0 || condition > 1 || (flags & ~1)) {
        LOG("NVN_QUEUE_FENCE_ERROR condition=%d flags=%#x", condition, flags);
        return;
    }
    pthread_mutex_lock(&out->lock);
    target->queue = out;
    target->fence = out->submits;
    target->fenced = 1;
    pthread_mutex_unlock(&out->lock);
    sky_checkpoint("post_nnMain_nvn_queue_fence_sync");
    LOG("NVN_QUEUE_FENCE queue=%p sync=%p fence=%llu submits=%llu completed=%llu",
        queue, sync, (unsigned long long)target->fence,
        (unsigned long long)out->submits, (unsigned long long)out->completed);
}

int sky_nvn_queue_get_error(void *queue,void *error_info) {
    (void)error_info;
    sky_nvn_queue_t *out=sky_nvn_queue_valid(queue);if(!out)return 1;
    int error=__atomic_load_n(&out->error,__ATOMIC_RELAXED)||sky_mtl_command_queue_get_error(out->metal);
    if(error)LOG("NVN_QUEUE_ERROR queue=%p result=GPU_ERROR_UNKNOWN",queue);
    return error?1:0;
}

void sky_nvn_queue_flush(void *queue) {
    sky_nvn_queue_t *out = sky_nvn_queue_valid(queue);
    if (!out) return;
    LOG("NVN_QUEUE_FLUSH queue=%p submits=%llu completed=%llu", queue,
        (unsigned long long)out->submits, (unsigned long long)out->completed);
}

void sky_nvn_queue_finish(void *queue) {
    sky_nvn_queue_t *out = sky_nvn_queue_valid(queue);
    if (!out) return;
    sky_nvn_queue_wait(out, out->submits, UINT64_MAX, NULL);
    LOG("NVN_QUEUE_FINISH queue=%p submits=%llu completed=%llu", queue,
        (unsigned long long)out->submits, (unsigned long long)out->completed);
}

int sky_nvn_sync_wait(const void *sync, uint64_t timeout_ns) {
    const sky_nvn_sync_t *target = sync;
    if (!target || target->magic != SKY_NVN_MAGIC('S', 'Y', 'N', 'C') || !target->fenced)
        return 3;
    if (target->fenced==2) return 0;
    sky_nvn_queue_t *queue = sky_nvn_queue_valid(target->queue);
    if (!queue) return 3;
    int already = 0;
    int satisfied = sky_nvn_queue_wait(queue, target->fence, timeout_ns, &already);
    if (!satisfied) return 2;
    int result = already ? 0 : 1;
    LOG("NVN_SYNC_WAIT sync=%p fence=%llu result=%d", sync, (unsigned long long)target->fence, result);
    return result;
}

typedef struct sky_nvn_command_region {
    struct sky_nvn_command_region *next;
    const void *pool;
    uint64_t offset,size;
} sky_nvn_command_region_t;

static void sky_nvn_command_regions_free(sky_nvn_command_region_t *region) {
    while(region){sky_nvn_command_region_t *next=region->next;free(region);region=next;}
}

typedef struct {
    const char *api,*reason;
    unsigned line;
    uint64_t arguments[3];
} sky_nvn_command_failure_t;

typedef struct sky_nvn_command_record {
    struct sky_nvn_command_record *next;
    uint32_t magic;
    int invalid;
    sky_nvn_command_failure_t failure;
    void *command_buffer;
    sky_mtl_operation_t *operations;
    sky_nvn_command_region_t *regions;
} sky_nvn_command_record_t;

static void sky_nvn_operations_free(sky_mtl_operation_t *op) {
    while (op) {
        sky_mtl_operation_t *next=op->next;
        for (int i=0;i<8;i++) if (op->targets.textures[i]) sky_mtl_texture_destroy(op->targets.textures[i]);
        if (op->targets.depth_texture) sky_mtl_texture_destroy(op->targets.depth_texture);
        for (int stage=0;stage<6;stage++) {
            if (op->state.programs[stage]) sky_mtl_program_release(op->state.programs[stage]);
            for (int slot=0;slot<128;slot++) {
                if (op->state.textures[stage][slot]) sky_mtl_texture_destroy(op->state.textures[stage][slot]);
                if (op->state.samplers[stage][slot]) sky_mtl_sampler_destroy(op->state.samplers[stage][slot]);
            }
            for (int slot=0;slot<32;slot++) if (op->state.images[stage][slot]) sky_mtl_texture_destroy(op->state.images[stage][slot]);
        }
        if (op->transfer.source) sky_mtl_texture_destroy(op->transfer.source);
        if (op->transfer.destination) sky_mtl_texture_destroy(op->transfer.destination);
        free(op->payload);
        free(op->sync_context);
        free(op);
        op=next;
    }
}
typedef struct {
    void *command_buffer;
    void *memory_callback, *callback_data;
    const void *command_pool;
    int64_t command_offset;
    uint64_t command_size,command_used;
    void *control_memory;
    uint64_t control_size,control_used;
    int command_provided,control_provided,in_memory_callback;
    sky_nvn_command_region_t *regions;
    int color_count;
    const void *color[8];
    void *color_metal[8];
    const void *depth;
    void *depth_metal;
    float clear_color[4];
    float clear_depth;
    uint32_t depth_mask, stencil_mask;
    int clear_stencil, has_clear_color, clear_index;
    int32_t scissor[4];
    float depth_range[2];
    int viewport_first, viewport_count;
    float *viewports;
    int invalid, targets_bound;
    sky_nvn_command_failure_t failure;
    sky_mtl_render_state_t render;
    const void *texture_pool, *sampler_pool;
    sky_mtl_operation_t *operations, *last_operation;
    sky_nvn_command_record_t *records;
} sky_nvn_command_state_t;

static void sky_nvn_command_invalid(sky_nvn_command_state_t *state,const char *api,unsigned line,
                                    const char *reason,uint64_t a,uint64_t b,uint64_t c) {
    if(!state)return;
    if(!state->invalid) {
        state->failure=(sky_nvn_command_failure_t){api,reason,line,{a,b,c}};
        static unsigned diagnostics;
        if(__atomic_fetch_add(&diagnostics,1,__ATOMIC_RELAXED)<128)
            LOG("NVN_COMMAND_BUFFER_INVALID command_buffer=%p api=%s line=%u reason=%s arguments={%#llx,%#llx,%#llx}",
                state->command_buffer,api,line,reason,(unsigned long long)a,(unsigned long long)b,(unsigned long long)c);
    }
    state->invalid=1;
}
#define SKY_NVN_COMMAND_INVALID(state,reason) \
    sky_nvn_command_invalid((state),__func__,__LINE__,(reason),0,0,0)

static uint32_t sky_nvn_float_bits(float value) {uint32_t bits;memcpy(&bits,&value,sizeof bits);return bits;}

static void sky_nvn_command_state_reset(sky_nvn_command_state_t *state) {
    sky_nvn_operations_free(state->operations);
    state->operations=state->last_operation=NULL;
    sky_nvn_command_regions_free(state->regions);state->regions=NULL;
    memset(&state->render,0,sizeof state->render);
    state->render.channel_mask=UINT32_MAX;
    state->render.depth_func=2;
    state->render.front_face=1;state->render.polygon_mode=2;
    state->render.viewport[5]=1;
    for (int i=0;i<8;i++) {
        state->render.blend[i].rgb_op=state->render.blend[i].alpha_op=1;
        state->render.blend[i].src_rgb=state->render.blend[i].src_alpha=2;
        state->render.blend[i].dst_rgb=state->render.blend[i].dst_alpha=1;
    }
    for (int i=0;i<2;i++) {
        state->render.stencil[i].function=8;
        state->render.stencil[i].fail=state->render.stencil[i].depth_fail=state->render.stencil[i].pass=1;
        state->render.stencil[i].read_mask=state->render.stencil[i].write_mask=255;
    }
    state->texture_pool=state->sampler_pool=NULL;
    state->targets_bound=0;
    for (int index = 0; index < 8; index++) sky_mtl_texture_destroy(state->color_metal[index]);
    sky_mtl_texture_destroy(state->depth_metal);
    free(state->viewports);
    state->color_count = 0;
    memset(state->color, 0, sizeof state->color);
    memset(state->color_metal, 0, sizeof state->color_metal);
    state->depth = NULL;
    state->depth_metal = NULL;
    memset(state->clear_color, 0, sizeof state->clear_color);
    state->clear_depth = 0;
    state->depth_mask = state->stencil_mask = 0;
    state->clear_stencil = 0;
    state->has_clear_color = 0;
    memset(state->scissor, 0, sizeof state->scissor);
    memset(state->depth_range, 0, sizeof state->depth_range);
    state->viewport_first = state->viewport_count = 0;
    state->viewports = NULL;
    state->invalid = 0;
    memset(&state->failure,0,sizeof state->failure);
}

static sky_nvn_command_state_t *sky_nvn_recording(void *command_buffer) {
    sky_nvn_command_buffer_t *out=command_buffer;
    return out && out->magic==SKY_NVN_MAGIC('C','M','D','B') && out->recording ? out->state : NULL;
}

/* Metal owns the full resource snapshots. The supplied NVN memory stores a
 * compact stream of references to those snapshots, plus inline update data.
 * Account its actual bytes, not the much larger host-only render-state copies.
 * Cursors survive BeginRecording: multiple handles can occupy one grant. */
typedef struct { uint32_t kind,size; uint64_t value; } sky_nvn_command_reference_t;

static void *sky_nvn_command_memory_allocate(sky_nvn_command_state_t *state,int control,uint64_t size) {
    if(state->invalid)return NULL;
    if(size>UINT64_MAX-7){SKY_NVN_COMMAND_INVALID(state,"allocation-size-overflow");return NULL;}
    size=(size+7)&~UINT64_C(7);
    uint64_t *capacity=control?&state->control_size:&state->command_size;
    uint64_t *used=control?&state->control_used:&state->command_used;
    int provided=control?state->control_provided:state->command_provided;
    /* Direct host clients may let Metal allocate all command storage. Once a
     * guest grant or refill callback is supplied, its finite budget is binding. */
    if(!provided&&!state->memory_callback)return NULL;
    if(size>*capacity-*used&&state->memory_callback&&!state->in_memory_callback) {
        state->in_memory_callback=1;
        ((sky_nvn_command_memory_callback_fn)state->memory_callback)(state->command_buffer,control,size,state->callback_data);
        state->in_memory_callback=0;
    }
    if(state->invalid)return NULL;
    if(size>*capacity-*used) {
        sky_nvn_command_invalid(state,__func__,__LINE__,control?"control-memory-exhausted":"command-memory-exhausted",size,*capacity,*used);
        LOG("NVN_COMMAND_BUFFER_MEMORY_EXHAUSTED command_buffer=%p event=%d required=%llu free=%llu",
            state->command_buffer,control,(unsigned long long)size,(unsigned long long)(*capacity-*used));
        return NULL;
    }
    void *result;
    if(control)result=(uint8_t *)state->control_memory+*used;
    else {
        const sky_nvn_memory_pool_t *pool=state->command_pool;
        uint64_t offset=(uint64_t)state->command_offset+*used;
        result=(uint8_t *)pool->mem+offset;
        sky_nvn_command_region_t *region=state->regions;
        if(region&&region->pool==pool&&region->offset+region->size==offset)region->size+=size;
        else {
            region=malloc(sizeof *region);
            if(!region){SKY_NVN_COMMAND_INVALID(state,"region-allocation-failed");return NULL;}
            *region=(sky_nvn_command_region_t){state->regions,pool,offset,size};state->regions=region;
        }
    }
    *used+=size;
    return result;
}

static int sky_nvn_command_reference_append(sky_nvn_command_state_t *state,uint32_t kind,const void *value) {
    sky_nvn_command_reference_t *slot=sky_nvn_command_memory_allocate(state,0,sizeof *slot);
    if(slot){sky_nvn_command_reference_t reference={kind,sizeof *slot,(uint64_t)(uintptr_t)value};memcpy(slot,&reference,sizeof reference);}
    return !state->invalid;
}
/* Caller holds g_texture_registration_lock through the retain. */
static sky_mtl_texture_t *sky_nvn_registered_texture_locked(const void *pool, int id) {
    sky_mtl_texture_t *result=NULL;
    sky_texture_registration_t *entry=sky_nvn_texture_registration_find(pool,id);
    if(entry)result=sky_mtl_texture_retain(entry->metal_view ? entry->metal_view : entry->texture.metal);
    return result;
}
void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *render) {
    memset(render->textures,0,sizeof render->textures);memset(render->samplers,0,sizeof render->samplers);memset(render->images,0,sizeof render->images);
    /* Snapshot each registry under one lock, retaining every occupied slot
     * before replacement/finalization may release its descriptor. Keep the
     * registries separate so no texture/sampler lock nesting is introduced. */
    int texture_locked=0;
    for(int stage=0;stage<6;stage++) {
        for(int slot=0;slot<32;slot++)if(render->image_bound[stage][slot]) {
            if(!texture_locked){pthread_mutex_lock(&g_texture_registration_lock);texture_locked=1;}
            render->images[stage][slot]=sky_nvn_registered_texture_locked(render->texture_pool,render->image_handles[stage][slot]&0xfffff);
        }
        for(int slot=0;slot<128;slot++)if(render->texture_bound[stage][slot]) {
            if(!texture_locked){pthread_mutex_lock(&g_texture_registration_lock);texture_locked=1;}
            render->textures[stage][slot]=sky_nvn_registered_texture_locked(render->texture_pool,render->texture_handles[stage][slot]&0xfffff);
        }
    }
    if(texture_locked)pthread_mutex_unlock(&g_texture_registration_lock);
    int sampler_locked=0;
    for(int stage=0;stage<6;stage++) {
        for(int slot=0;slot<128;slot++)if(render->sampler_bound[stage][slot]||render->texture_bound[stage][slot]) {
            if(!sampler_locked){pthread_mutex_lock(&g_sampler_registration_lock);sampler_locked=1;}
            uint64_t sh=render->sampler_handles[stage][slot],th=render->texture_handles[stage][slot];
            if(!render->sampler_bound[stage][slot])sh=th;
            render->samplers[stage][slot]=sky_nvn_sampler_registered_locked(render->sampler_pool,(int)(sh>>20));
        }
    }
    if(sampler_locked)pthread_mutex_unlock(&g_sampler_registration_lock);
}
static sky_mtl_operation_t *sky_nvn_operation_append(sky_nvn_command_state_t *state, uint32_t kind) {
    if (!state) return NULL;
    sky_mtl_operation_t *op=calloc(1,sizeof *op);
    if (!op) { SKY_NVN_COMMAND_INVALID(state,"operation-allocation-failed"); return NULL; }
    if(!sky_nvn_command_reference_append(state,kind,op)){free(op);return NULL;}
    op->kind=kind;op->targets_bound=state->targets_bound;
    op->targets.color_count=state->color_count;
    for(int i=0;i<state->color_count;i++)op->targets.textures[i]=sky_mtl_texture_retain(state->color_metal[i]);
    op->targets.depth_texture=sky_mtl_texture_retain(state->depth_metal);
    if(kind==SKY_MTL_OP_DRAW||kind==SKY_MTL_OP_DISPATCH||kind==SKY_MTL_OP_STATE||kind==SKY_MTL_OP_TRANSFER) {
        op->state=state->render;
        op->state.texture_pool=(void *)state->texture_pool;op->state.sampler_pool=(void *)state->sampler_pool;
        for(int stage=0;stage<6;stage++)sky_mtl_program_retain(op->state.programs[stage]);
    }
    if(state->last_operation)state->last_operation->next=op;else state->operations=op;
    state->last_operation=op;
    return op;
}

void sky_nvn_command_buffer_bind_uniform_buffer(void *command_buffer,int stage,int index,uint64_t address,uint64_t size) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if (!s) return;
    if (stage<0 || stage>=6 || index<0 || index>=16) {SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.uniform[stage][index]=(sky_mtl_buffer_binding_t){address,size};s->render.uniform_bound[stage][index]=1;
}
void sky_nvn_command_buffer_bind_storage_buffer(void *command_buffer,int stage,int index,uint64_t address,uint64_t size) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if (!s) return;
    if (stage<0 || stage>=6 || index<0 || index>=16) {SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.storage[stage][index]=(sky_mtl_buffer_binding_t){address,size};s->render.storage_bound[stage][index]=1;
}
void sky_nvn_command_buffer_bind_vertex_buffer(void *command_buffer,int index,uint64_t address,uint64_t size) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if (!s) return;
    if (index<0 || index>=16) {SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.vertex[index]=(sky_mtl_buffer_binding_t){address,size};s->render.vertex_bound[index]=1;
}
void sky_nvn_command_buffer_bind_program(void *command_buffer,const void *program,int stages) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    const sky_nvn_program_t *p=program;
    if (!s) return;
    if (p && p->magic!=SKY_NVN_MAGIC('P','R','O','G')) {SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.program_mask|=(uint32_t)stages;
    for (int stage=0;stage<6;stage++) if (stages & (1<<stage)) s->render.programs[stage]=p && (p->stage_mask & (1u<<stage)) ? p->serial : 0;
}
void sky_nvn_command_buffer_set_texture_pool(void *command_buffer,const void *pool) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s){s->texture_pool=pool;s->render.pool_mask|=1;s->render.texture_pool=(void *)pool;}
}
void sky_nvn_command_buffer_set_sampler_pool(void *command_buffer,const void *pool) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s){s->sampler_pool=pool;s->render.pool_mask|=2;s->render.sampler_pool=(void *)pool;}
}
void sky_nvn_command_buffer_bind_texture(void *command_buffer,int stage,int index,uint64_t handle) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if (!s) return;
    if(stage<0||stage>=6||index<0||index>=128){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.texture_handles[stage][index]=handle;s->render.texture_bound[stage][index]=1;
}
void sky_nvn_command_buffer_bind_separate_texture(void *command_buffer,int stage,int index,uint64_t handle) {
    sky_nvn_command_buffer_bind_texture(command_buffer,stage,index,handle);
}
void sky_nvn_command_buffer_bind_separate_sampler(void *command_buffer,int stage,int index,uint64_t handle) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if(!s)return;
    if(stage<0||stage>=6||index<0||index>=128){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.sampler_handles[stage][index]=handle;s->render.sampler_bound[stage][index]=1;
}
void sky_nvn_command_buffer_bind_vertex_attrib_state(void *command_buffer,int count,const void *attributes) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if(!s)return;
    if(count<0||count>16||(count&&!attributes)){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    const sky_vertex_attrib_t *a=attributes;
    memset(s->render.attributes,0,sizeof s->render.attributes);
    for(int i=0;i<count;i++){s->render.attributes[i].format=a[i].format;s->render.attributes[i].offset=a[i].offset;s->render.attributes[i].stream=a[i].stream;}
    s->render.attribute_count=count;s->render.changed|=SKY_MTL_STATE_ATTRIBUTES;
}
void sky_nvn_command_buffer_bind_vertex_stream_state(void *command_buffer,int count,const void *streams) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);
    if(!s)return;
    if(count<0||count>16||(count&&!streams)){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    memset(s->render.streams,0,sizeof s->render.streams);
    if(count)memcpy(s->render.streams,streams,count*8u);
    s->render.stream_count=count;s->render.changed|=SKY_MTL_STATE_STREAMS;
}
void sky_nvn_command_buffer_bind_blend_state(void *command_buffer,const void *state) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s||!state)return;
    const sky_blend_state_t *b=state;unsigned i=b->target;s->render.blend_mask|=1u<<i;
    s->render.blend[i].rgb_op=b->eq_rgb;s->render.blend[i].alpha_op=b->eq_a;
    s->render.blend[i].src_rgb=b->src_rgb;s->render.blend[i].dst_rgb=b->dst_rgb;
    s->render.blend[i].src_alpha=b->src_a;s->render.blend[i].dst_alpha=b->dst_a;
}
void sky_nvn_command_buffer_bind_channel_mask_state(void *command_buffer,const void *state) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s&&state){s->render.channel_mask=*(const uint32_t *)state;s->render.changed|=SKY_MTL_STATE_CHANNEL;}
}
void sky_nvn_command_buffer_bind_color_state(void *command_buffer,const void *state) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s&&state){s->render.blend_enable=*(const uint32_t *)state & 255;s->render.changed|=SKY_MTL_STATE_COLOR;}
}
void sky_nvn_command_buffer_bind_polygon_state(void *command_buffer,const void *state) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s||!state)return;
    s->render.changed|=SKY_MTL_STATE_POLYGON;
    const sky_polygon_state_t *p=state;s->render.cull_face=p->cull;s->render.front_face=p->front;s->render.polygon_mode=p->mode;s->render.polygon_offset_enables=p->offset_en;
}
void sky_nvn_command_buffer_set_polygon_offset_clamp(void *command_buffer,float factor,float units,float clamp) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s)return;
    s->render.polygon_offset_factor=factor;s->render.polygon_offset_units=units;s->render.polygon_offset_clamp=clamp;
    s->render.changed|=SKY_MTL_STATE_POLYGON_OFFSET;
    static unsigned diagnostics;
    if(__atomic_fetch_add(&diagnostics,1,__ATOMIC_RELAXED)<16)
        LOG("NVN_POLYGON_OFFSET factor=%g units=%g clamp=%g",factor,units,clamp);
}
void sky_nvn_command_buffer_bind_depth_stencil_state(void *command_buffer,const void *state) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s||!state)return;
    s->render.changed|=SKY_MTL_STATE_DEPTH;
    const sky_depth_state_t *d=state;s->render.depth_test=d->dt;s->render.depth_write=d->dw;s->render.depth_func=d->df;s->render.stencil_test=d->st;
    s->render.stencil[0].function=d->sf_f;s->render.stencil[1].function=d->sf_b;
    s->render.stencil[0].fail=d->fail_f;s->render.stencil[1].fail=d->fail_b;
    s->render.stencil[0].depth_fail=d->zfail_f;s->render.stencil[1].depth_fail=d->zfail_b;
    s->render.stencil[0].pass=d->zpass_f;s->render.stencil[1].pass=d->zpass_b;
}
void sky_nvn_command_buffer_set_blend_color(void *command_buffer,const float *rgba) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s&&rgba){memcpy(s->render.blend_color,rgba,16);s->render.changed|=SKY_MTL_STATE_BLEND_COLOR;}
}
void sky_nvn_command_buffer_set_stencil_value_mask(void *command_buffer,int face,int mask) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s)for(int i=0;i<2;i++)if(face&(1<<i)){s->render.stencil[i].read_mask=mask;s->render.changed|=SKY_MTL_STATE_STENCIL_READ_FRONT<<i;}
}
void sky_nvn_command_buffer_set_stencil_mask(void *command_buffer,int face,int mask) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s)for(int i=0;i<2;i++)if(face&(1<<i)){s->render.stencil[i].write_mask=mask;s->render.changed|=SKY_MTL_STATE_STENCIL_WRITE_FRONT<<i;}
}
void sky_nvn_command_buffer_set_stencil_ref(void *command_buffer,int face,int reference) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(s)for(int i=0;i<2;i++)if(face&(1<<i)){s->render.stencil[i].reference=reference;s->render.changed|=SKY_MTL_STATE_STENCIL_REF_FRONT<<i;}
}
void sky_nvn_command_buffer_draw_arrays_instanced(void *command_buffer,int primitive,int first,int count,int base_instance,int instances) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s)return;
    if(first<0||count<0||base_instance<0||instances<0){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(s,SKY_MTL_OP_DRAW);if(!op)return;
    op->primitive=primitive;op->first=first;op->count=count;op->base_instance=base_instance;op->instances=instances;
}
void sky_nvn_command_buffer_draw_arrays(void *command_buffer,int primitive,int first,int count) {
    sky_nvn_command_buffer_draw_arrays_instanced(command_buffer,primitive,first,count,0,1);
}
void sky_nvn_command_buffer_draw_elements_instanced(void *command_buffer,int primitive,int index_type,int count,uint64_t address,int base_vertex,int base_instance,int instances) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s)return;
    if(index_type<0||index_type>2||count<0||base_instance<0||instances<0){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(s,SKY_MTL_OP_DRAW);if(!op)return;
    op->primitive=primitive;op->index_type=index_type;op->count=count;op->index_address=address;
    op->base_vertex=base_vertex;op->base_instance=base_instance;op->instances=instances;
}
void sky_nvn_command_buffer_draw_elements_base_vertex(void *command_buffer,int primitive,int type,int count,uint64_t address,int base) {
    sky_nvn_command_buffer_draw_elements_instanced(command_buffer,primitive,type,count,address,base,0,1);
}
void sky_nvn_command_buffer_draw_elements(void *command_buffer,int primitive,int type,int count,uint64_t address) {
    sky_nvn_command_buffer_draw_elements_instanced(command_buffer,primitive,type,count,address,0,0,1);
}
void sky_nvn_command_buffer_dispatch_compute(void *command_buffer,int x,int y,int z) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s)return;
    if(x<0||y<0||z<0){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(s,SKY_MTL_OP_DISPATCH);if(op){op->groups[0]=x;op->groups[1]=y;op->groups[2]=z;}
}

void sky_nvn_command_buffer_bind_image(void *command_buffer,int stage,int index,uint64_t handle) {
    sky_nvn_command_state_t *s=sky_nvn_recording(command_buffer);if(!s)return;
    if(stage<0||stage>=6||index<0||index>=32){SKY_NVN_COMMAND_INVALID(s,"invalid-arguments");return;}
    s->render.image_handles[stage][index]=handle;s->render.image_bound[stage][index]=1;
}
void sky_nvn_command_buffer_bind_images(void *command_buffer,int stage,int first,int count,const uint64_t *handles) {
    for(int i=0;i<count;i++)sky_nvn_command_buffer_bind_image(command_buffer,stage,first+i,handles[i]);
}
void sky_nvn_command_buffer_bind_textures(void *command_buffer,int stage,int first,int count,const uint64_t *handles) {
    for(int i=0;i<count;i++)sky_nvn_command_buffer_bind_texture(command_buffer,stage,first+i,handles[i]);
}
void sky_nvn_command_buffer_bind_separate_textures(void *command_buffer,int stage,int first,int count,const uint64_t *handles) {
    sky_nvn_command_buffer_bind_textures(command_buffer,stage,first,count,handles);
}
void sky_nvn_command_buffer_bind_separate_samplers(void *command_buffer,int stage,int first,int count,const uint64_t *handles) {
    for(int i=0;i<count;i++)sky_nvn_command_buffer_bind_separate_sampler(command_buffer,stage,first+i,handles[i]);
}
void sky_nvn_command_buffer_bind_uniform_buffers(void *command_buffer,int stage,int first,int count,const sky_nvn_buffer_range_t *ranges) {
    for(int i=0;i<count;i++)sky_nvn_command_buffer_bind_uniform_buffer(command_buffer,stage,first+i,ranges[i].address,ranges[i].size);
}
void sky_nvn_command_buffer_bind_storage_buffers(void *command_buffer,int stage,int first,int count,const sky_nvn_buffer_range_t *ranges) {
    for(int i=0;i<count;i++)sky_nvn_command_buffer_bind_storage_buffer(command_buffer,stage,first+i,ranges[i].address,ranges[i].size);
}
void sky_nvn_command_buffer_bind_vertex_buffers(void *command_buffer,int first,int count,const sky_nvn_buffer_range_t *ranges) {
    for(int i=0;i<count;i++)sky_nvn_command_buffer_bind_vertex_buffer(command_buffer,first+i,ranges[i].address,ranges[i].size);
}
void sky_nvn_command_buffer_set_viewport(void *command_buffer,int x,int y,int width,int height) {
    const float values[]={x,y,width,height};sky_nvn_command_buffer_set_viewports(command_buffer,0,1,values);
}
void sky_nvn_command_buffer_set_scissors(void *command_buffer,int first,int count,const int *values) {
    if(first==0&&count>0&&values)sky_nvn_command_buffer_set_scissor(command_buffer,values[0],values[1],values[2],values[3]);
}
void sky_nvn_command_buffer_set_depth_ranges(void *command_buffer,int first,int count,const float *values) {
    if(first==0&&count>0&&values)sky_nvn_command_buffer_set_depth_range(command_buffer,values[0],values[1]);
}
void sky_nvn_command_buffer_barrier(void *command_buffer,int barriers) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    /* Preserve the boundary when consecutive draws share a Metal encoder. */
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_BARRIER);
    if(op)op->barriers=(uint32_t)barriers;
}

static void sky_nvn_record_clear_texture(void *command_buffer,const void *texture,const void *view,
    const sky_nvn_copy_region_t *region,const void *values,int mask,int type) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    const sky_nvn_texture_t *t=sky_nvn_texture_valid(texture);
    const sky_nvn_texture_view_t *v=sky_nvn_texture_view_valid(view);
    if(!t||!region||!values||(view&&!v)||mask<0||mask>15){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_CLEAR_TEXTURE);if(!op)return;
    for(int i=0;i<8;i++){sky_mtl_texture_destroy(op->targets.textures[i]);op->targets.textures[i]=NULL;}
    sky_mtl_texture_destroy(op->targets.depth_texture);op->targets.depth_texture=NULL;
    op->targets.color_count=1;op->targets.textures[0]=v?sky_nvn_texture_view_create_metal(t,v):sky_mtl_texture_retain(t->metal);
    if(!op->targets.textures[0]){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    memcpy(op->region,region,sizeof op->region);op->clear_mask=mask;op->clear_value_type=type;
    for(int i=0;i<4;i++) {
        if(type==1)op->clear_value[i]=((const int32_t *)values)[i];
        else if(type==2)op->clear_value[i]=((const uint32_t *)values)[i];
        else op->clear_value[i]=((const float *)values)[i];
    }
    LOG("NVN_CLEAR_TEXTURE texture=%p region=%d,%d,%d,%d,%d,%d mask=%#x type=%d",texture,region->xoffset,region->yoffset,region->zoffset,region->width,region->height,region->depth,mask,type);
}
void sky_nvn_command_buffer_clear_texture(void *command_buffer,const void *texture,const void *view,const sky_nvn_copy_region_t *region,const float *values,int mask) {
    sky_nvn_record_clear_texture(command_buffer,texture,view,region,values,mask,0);
}
void sky_nvn_command_buffer_clear_texture_i(void *command_buffer,const void *texture,const void *view,const sky_nvn_copy_region_t *region,const int32_t *values,int mask) {
    sky_nvn_record_clear_texture(command_buffer,texture,view,region,values,mask,1);
}
void sky_nvn_command_buffer_clear_texture_ui(void *command_buffer,const void *texture,const void *view,const sky_nvn_copy_region_t *region,const uint32_t *values,int mask) {
    sky_nvn_record_clear_texture(command_buffer,texture,view,region,values,mask,2);
}

typedef struct { sky_nvn_sync_t value; sky_nvn_sync_t *target; } sky_nvn_recorded_sync_t;
static int sky_nvn_recorded_sync_wait(void *context) {
    sky_nvn_recorded_sync_t *s=context;
    int result=sky_nvn_sync_wait(s->target?s->target:&s->value,UINT64_MAX);
    return result==0||result==1;
}
static int sky_nvn_recorded_sync_fence(void *context) {
    sky_nvn_recorded_sync_t *s=context;
    if(!s->target||s->target->magic!=SKY_NVN_MAGIC('S','Y','N','C'))return 0;
    s->target->fenced=2;s->target->queue=NULL;s->target->fence=0;return 1;
}
void sky_nvn_command_buffer_wait_sync(void *command_buffer,const void *sync) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);
    const sky_nvn_sync_t *s=sync;if(!state)return;
    if(!s||s->magic!=SKY_NVN_MAGIC('S','Y','N','C')){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_SYNC);if(!op)return;
    sky_nvn_recorded_sync_t *context=calloc(1,sizeof *context);if(!context){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    context->value=*s;context->target=(void *)s;op->sync_context=context;op->sync_context_size=sizeof *context;op->sync_action=sky_nvn_recorded_sync_wait;
}
void sky_nvn_command_buffer_fence_sync(void *command_buffer,void *sync,int condition,int flags) {
    (void)condition;(void)flags;
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);
    sky_nvn_sync_t *s=sync;if(!state)return;
    if(!s||s->magic!=SKY_NVN_MAGIC('S','Y','N','C')){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_SYNC);if(!op)return;
    sky_nvn_recorded_sync_t *context=calloc(1,sizeof *context);if(!context){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    context->target=s;op->sync_context=context;op->sync_context_size=sizeof *context;op->sync_action=sky_nvn_recorded_sync_fence;
}

void sky_nvn_command_buffer_call_commands(void *command_buffer,int count,const uint64_t *handles) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(count<0||(count&&!handles)){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    if(count&&!sky_nvn_operation_append(state,SKY_MTL_OP_STATE))return;
    for(int index=0;index<count;index++) {
        const sky_nvn_command_record_t *record=(const void *)(uintptr_t)handles[index];
        if(!record||record->magic!=SKY_NVN_MAGIC('C','M','D','S')||record->invalid){
            sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-called-handle",handles[index],index,0);return;
        }
        for(const sky_mtl_operation_t *source=record->operations;source;source=source->next) {
            sky_mtl_operation_t *copy=malloc(sizeof *copy);if(!copy){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
            if(!sky_nvn_command_reference_append(state,source->kind,copy)){free(copy);return;}
            if(record->invalid){
                free(copy);sky_nvn_command_invalid(state,__func__,__LINE__,"called-handle-retired-during-refill",handles[index],index,0);return;
            }
            *copy=*source;copy->next=NULL;copy->sync_context=NULL;copy->payload=NULL;
            if (source->transfer.source) copy->transfer.source=sky_mtl_texture_retain(source->transfer.source);
            if (source->transfer.destination) copy->transfer.destination=sky_mtl_texture_retain(source->transfer.destination);
            for(int i=0;i<8;i++) if (source->targets.textures[i]) copy->targets.textures[i]=sky_mtl_texture_retain(source->targets.textures[i]);
            if (source->targets.depth_texture) copy->targets.depth_texture=sky_mtl_texture_retain(source->targets.depth_texture);
            for(int stage=0;stage<6;stage++) {
                if (copy->state.programs[stage]) sky_mtl_program_retain(copy->state.programs[stage]);
                for(int slot=0;slot<128;slot++) {
                    if (source->state.textures[stage][slot]) copy->state.textures[stage][slot]=sky_mtl_texture_retain(source->state.textures[stage][slot]);
                    if (source->state.samplers[stage][slot]) copy->state.samplers[stage][slot]=sky_mtl_sampler_retain(source->state.samplers[stage][slot]);
                }
                for(int slot=0;slot<32;slot++) if (source->state.images[stage][slot]) copy->state.images[stage][slot]=sky_mtl_texture_retain(source->state.images[stage][slot]);
            }
            if(source->payload_size){
                void *inline_data=sky_nvn_command_memory_allocate(state,0,source->payload_size);
                if(state->invalid){sky_nvn_operations_free(copy);return;}
                if(record->invalid){
                    sky_nvn_operations_free(copy);sky_nvn_command_invalid(state,__func__,__LINE__,"called-handle-retired-during-refill",handles[index],index,0);return;
                }
                if(inline_data)memcpy(inline_data,source->payload,source->payload_size);
                copy->payload=malloc(source->payload_size);
                if(!copy->payload){sky_nvn_operations_free(copy);SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
                memcpy(copy->payload,source->payload,source->payload_size);
            }
            if(source->sync_context_size){
                copy->sync_context=malloc(source->sync_context_size);
                if(!copy->sync_context){sky_nvn_operations_free(copy);SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
                memcpy(copy->sync_context,source->sync_context,source->sync_context_size);
            }
            if(state->last_operation)state->last_operation->next=copy;else state->operations=copy;
            state->last_operation=copy;
            if(source->kind==SKY_MTL_OP_STATE) {
                sky_mtl_render_state_merge(&state->render,&source->state);
                state->texture_pool=state->render.texture_pool;state->sampler_pool=state->render.sampler_pool;
                if(source->targets_bound){
                    for(int i=0;i<8;i++){sky_mtl_texture_destroy(state->color_metal[i]);state->color_metal[i]=sky_mtl_texture_retain(source->targets.textures[i]);state->color[i]=state->color_metal[i];}
                    sky_mtl_texture_destroy(state->depth_metal);state->depth_metal=sky_mtl_texture_retain(source->targets.depth_texture);state->depth=state->depth_metal;
                    state->color_count=source->targets.color_count;state->targets_bound=1;
                }
            }
        }
    }
}
void sky_nvn_command_buffer_copy_commands(void *command_buffer,int count,const uint64_t *handles) {
    sky_nvn_command_buffer_call_commands(command_buffer,count,handles);
}

void sky_nvn_command_buffer_report_counter(void *command_buffer,int type,uint64_t address) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_REPORT_COUNTER);if(!op)return;
    op->counter_type=type;op->destination_address=address;
    LOG("NVN_REPORT_COUNTER type=%d destination=%#llx",type,(unsigned long long)address);
}
void sky_nvn_command_buffer_reset_counter(void *command_buffer,int type) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_RESET_COUNTER);if(op)op->counter_type=type;
}
void sky_nvn_command_buffer_report_value(void *command_buffer,uint32_t value,uint64_t address) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_REPORT_VALUE);if(op){op->report_value=value;op->destination_address=address;}
}
uint64_t sky_nvn_device_get_timestamp_in_nanoseconds(const void *device,const uint64_t *counter_data) {
    (void)device;return counter_data?(uint64_t)((__uint128_t)counter_data[1]*625/12):0;
}
void sky_nvn_device_finalize_command_handle(void *device,uint64_t handle) {
    (void)device;sky_nvn_command_record_t *record=(void *)(uintptr_t)handle;
    if(!record||record->magic!=SKY_NVN_MAGIC('C','M','D','S'))return;
    sky_nvn_operations_free(record->operations);record->operations=NULL;
    if(!record->invalid)record->failure=(sky_nvn_command_failure_t){__func__,"handle-finalized",__LINE__,{handle,0,0}};
    record->invalid=1;
}

void sky_nvn_command_buffer_set_shader_scratch_memory(void *command_buffer,const void *pool,int64_t offset,uint64_t size) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    const sky_nvn_memory_pool_t *memory=pool;
    if(!sky_nvn_memory_pool_range_valid(memory,offset,size)||!memory->metal||
        memory->dev!=((sky_nvn_command_buffer_t *)command_buffer)->dev||
        (memory->flags&SKY_NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS)||((uint64_t)offset&4095)||(size&4095)) {
        SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");LOG("NVN_SHADER_SCRATCH_ERROR pool=%p offset=%#llx size=%#llx",pool,(unsigned long long)offset,(unsigned long long)size);return;
    }
    /* Metal compiles thread-private local/scratch storage into each pipeline;
     * the guest reservation is validated but is not a shader-visible buffer. */
    LOG("NVN_SHADER_SCRATCH pool=%p offset=%#llx size=%#llx storage=metal-managed",pool,(unsigned long long)offset,(unsigned long long)size);
}

void sky_nvn_command_buffer_set_depth_bounds(void *command_buffer,uint32_t enabled,float near_value,float far_value) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    enabled=!!(enabled&0xff);
    if(enabled&&(near_value>0.f||far_value<1.f)) {
        SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");LOG("NVN_DEPTH_BOUNDS_ERROR reason=restricted-depth-bounds-unsupported enabled=%u near=%g far=%g",enabled,near_value,far_value);return;
    }
    LOG("NVN_DEPTH_BOUNDS enabled=%u near=%g far=%g mode=unrestricted",enabled,near_value,far_value);
}

static sky_mtl_texture_t *sky_nvn_transfer_texture(const void *object,const void *view,int32_t *format) {
    const sky_nvn_texture_t *texture=sky_nvn_texture_valid(object);if(!texture)return NULL;
    const sky_nvn_texture_view_t *config=sky_nvn_texture_view_valid(view);
    if(view&&!config)return NULL;
    int effective=texture->format,view_format=0;if(config&&sky_nvn_texture_view_get_format(config,&view_format))effective=view_format;*format=effective;
    return config?sky_nvn_texture_view_create_metal(texture,config):sky_mtl_texture_retain(texture->metal);
}
void sky_nvn_command_buffer_set_copy_row_stride(void *command_buffer,int64_t stride) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(stride<0){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}state->render.copy_row_stride=stride;state->render.changed|=SKY_MTL_STATE_COPY_ROW;
}
void sky_nvn_command_buffer_set_copy_image_stride(void *command_buffer,int64_t stride) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(stride<0){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}state->render.copy_image_stride=stride;state->render.changed|=SKY_MTL_STATE_COPY_IMAGE;
}
void sky_nvn_command_buffer_copy_buffer_to_texture(void *command_buffer,uint64_t source,const void *destination,const void *view,const sky_nvn_copy_region_t *region,int flags) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(!region){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_TRANSFER);if(!op)return;
    op->transfer.kind=SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE;op->transfer.flags=flags;op->transfer.buffer_address=source;
    op->transfer.destination=sky_nvn_transfer_texture(destination,view,&op->transfer.destination_format);
    memcpy(op->transfer.destination_region,region,sizeof op->transfer.destination_region);
    if(!op->transfer.destination)SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");
    LOG("NVN_COPY_BUFFER_TO_TEXTURE source=%#llx destination=%p extent=%d,%d,%d flags=%#x",(unsigned long long)source,destination,region->width,region->height,region->depth,flags);
}
void sky_nvn_command_buffer_copy_texture_to_buffer(void *command_buffer,const void *source,const void *view,const sky_nvn_copy_region_t *region,uint64_t destination,int flags) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(!region){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_TRANSFER);if(!op)return;
    op->transfer.kind=SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER;op->transfer.flags=flags;op->transfer.buffer_address=destination;
    op->transfer.source=sky_nvn_transfer_texture(source,view,&op->transfer.source_format);
    memcpy(op->transfer.source_region,region,sizeof op->transfer.source_region);
    if(!op->transfer.source)SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");
    LOG("NVN_COPY_TEXTURE_TO_BUFFER source=%p destination=%#llx extent=%d,%d,%d flags=%#x",source,(unsigned long long)destination,region->width,region->height,region->depth,flags);
}
void sky_nvn_command_buffer_copy_texture_to_texture(void *command_buffer,const void *source,const void *source_view,const sky_nvn_copy_region_t *source_region,const void *destination,const void *destination_view,const sky_nvn_copy_region_t *destination_region,int flags) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(!source_region||!destination_region){SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");return;}
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_TRANSFER);if(!op)return;
    op->transfer.kind=SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE;op->transfer.flags=flags;
    op->transfer.source=sky_nvn_transfer_texture(source,source_view,&op->transfer.source_format);
    op->transfer.destination=sky_nvn_transfer_texture(destination,destination_view,&op->transfer.destination_format);
    memcpy(op->transfer.source_region,source_region,sizeof op->transfer.source_region);
    memcpy(op->transfer.destination_region,destination_region,sizeof op->transfer.destination_region);
    if(!op->transfer.source||!op->transfer.destination)SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");
    LOG("NVN_COPY_TEXTURE_TO_TEXTURE source=%p destination=%p extent=%d,%d,%d flags=%#x",source,destination,source_region->width,source_region->height,source_region->depth,flags);
}

void sky_nvn_command_buffer_update_uniform_buffer(void *command_buffer,uint64_t address,uint64_t buffer_size,int64_t offset,uint64_t size,const void *data) {
    sky_nvn_command_state_t *state=sky_nvn_recording(command_buffer);if(!state)return;
    if(offset<0||(uint64_t)offset>buffer_size||size>buffer_size-(uint64_t)offset||address>UINT64_MAX-(uint64_t)offset||
        ((address+(uint64_t)offset)&3)||(size&3)||size>(512u<<20)||(size&&!data)) {
        SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");LOG("NVN_UPDATE_UNIFORM_ERROR address=%#llx buffer_size=%llu offset=%lld size=%llu",(unsigned long long)address,(unsigned long long)buffer_size,(long long)offset,(unsigned long long)size);return;
    }
    if(!size)return;
    void *inline_data=sky_nvn_command_memory_allocate(state,0,size);
    if(state->invalid)return;
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_UPDATE_BUFFER);if(!op)return;
    op->destination_address=address+(uint64_t)offset;op->payload_size=size;op->payload=malloc(size);
    vm_size_t copied=0;
    if(!op->payload||vm_read_overwrite(mach_task_self(),(vm_address_t)data,size,(vm_address_t)op->payload,&copied)!=KERN_SUCCESS||copied!=size) {
        SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");LOG("NVN_UPDATE_UNIFORM_ERROR reason=unreadable-payload");return;
    }
    if(inline_data)memcpy(inline_data,op->payload,size);
    LOG("NVN_UPDATE_UNIFORM address=%#llx offset=%lld size=%llu",(unsigned long long)address,(long long)offset,(unsigned long long)size);
}

uint32_t sky_nvn_command_buffer_initialize(void *command_buffer, void *device) {
    if (!command_buffer || !device || ((const sky_nvn_device_t *)device)->magic != 0x44455630) {
        LOG("NVN_COMMAND_BUFFER_INIT_ERROR reason=invalid-device");
        return 0;
    }
    sky_nvn_command_buffer_t *out = command_buffer;
    memset(out, 0, sizeof *out);
    sky_nvn_command_state_t *state = calloc(1, sizeof *state);
    if (!state) {
        LOG("NVN_COMMAND_BUFFER_INIT_ERROR reason=allocation-failed");
        return 0;
    }
    out->magic = SKY_NVN_MAGIC('C', 'M', 'D', 'B');
    out->dev = device;
    out->state = state;
    state->command_buffer=command_buffer;
    sky_checkpoint("post_nnMain_nvn_command_buffer_initialize");
    LOG("NVN_COMMAND_BUFFER_INITIALIZED command_buffer=%p device=%p", command_buffer, device);
    return 1;
}

void sky_nvn_command_buffer_finalize(void *command_buffer) {
    if (!command_buffer || ((sky_nvn_command_buffer_t *)command_buffer)->magic != SKY_NVN_MAGIC('C', 'M', 'D', 'B'))
        return;
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out->state;
    sky_nvn_command_state_reset(state);
    while (state->records) {
        sky_nvn_command_record_t *record=state->records;
        state->records=record->next;
        sky_nvn_operations_free(record->operations);
        sky_nvn_command_regions_free(record->regions);
        free(record);
    }
    free(state);
    memset(out, 0, sizeof *out);
}

void sky_nvn_command_buffer_set_memory_callback(void *command_buffer, sky_nvn_command_memory_callback_fn callback) {
    sky_nvn_command_buffer_t *out = command_buffer;
    if (out&&out->magic==SKY_NVN_MAGIC('C','M','D','B')) ((sky_nvn_command_state_t *)out->state)->memory_callback = (void *)callback;
    LOG("NVN_COMMAND_BUFFER_MEMORY_CALLBACK command_buffer=%p callback=%p", command_buffer, callback);
}

void sky_nvn_command_buffer_set_memory_callback_data(void *command_buffer, void *data) {
    sky_nvn_command_buffer_t *out = command_buffer;
    if (out&&out->magic==SKY_NVN_MAGIC('C','M','D','B')) ((sky_nvn_command_state_t *)out->state)->callback_data = data;
    LOG("NVN_COMMAND_BUFFER_MEMORY_CALLBACK_DATA command_buffer=%p data=%p", command_buffer, data);
}

void sky_nvn_command_buffer_add_command_memory(void *command_buffer, const void *pool, int64_t offset, uint64_t size) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out&&out->magic==SKY_NVN_MAGIC('C','M','D','B') ? out->state : NULL;
    const sky_nvn_memory_pool_t *memory = pool;
    if (!state || !memory || memory->magic != SKY_NVN_MAGIC('P', 'O', 'O', 'L') || !memory->mem || !size || offset < 0 ||
        (uint64_t)offset > memory->size || size > memory->size - (uint64_t)offset) {
        if (state) sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-command-memory",(uint64_t)(uintptr_t)pool,(uint64_t)offset,size);
        LOG("NVN_COMMAND_BUFFER_MEMORY_ERROR command_buffer=%p pool=%p offset=%#llx size=%#llx",
            command_buffer, pool, (unsigned long long)offset, (unsigned long long)size);
        return;
    }
    /* Re-supplying a command memory region grants the driver permission to
     * overwrite its old commands. Retire host snapshots for that region too. */
    for(sky_nvn_command_record_t *record=state->records;record;record=record->next) {
        for(sky_nvn_command_region_t *region=record->regions;region;region=region->next) {
            if(region->pool==pool&&(uint64_t)offset<region->offset+region->size&&region->offset<(uint64_t)offset+size) {
                sky_nvn_operations_free(record->operations);record->operations=NULL;
                if(!record->invalid)record->failure=(sky_nvn_command_failure_t){__func__,"command-memory-reused",__LINE__,{(uint64_t)offset,size,region->offset}};
                record->invalid=1;break;
            }
        }
    }
    for(sky_nvn_command_region_t *region=state->regions;region;region=region->next) {
        if(region->pool==pool&&(uint64_t)offset<region->offset+region->size&&region->offset<(uint64_t)offset+size) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"overlaps-current-recording",(uint64_t)offset,size,region->offset);LOG("NVN_COMMAND_BUFFER_MEMORY_ERROR reason=overlaps-current-recording");return;
        }
    }
    state->command_pool = pool;
    state->command_offset = offset;
    state->command_size = size;
    state->command_used=0;state->command_provided=1;
    LOG("NVN_COMMAND_BUFFER_MEMORY command_buffer=%p pool=%p offset=%#llx size=%#llx",
        command_buffer, pool, (unsigned long long)offset, (unsigned long long)size);
}

void sky_nvn_command_buffer_add_control_memory(void *command_buffer, void *memory, uint64_t size) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out&&out->magic==SKY_NVN_MAGIC('C','M','D','B') ? out->state : NULL;
    if(state&&!memory&&size) {
        state->control_memory=NULL;state->control_size=state->control_used=0;state->control_provided=1;
        if(out->recording)SKY_NVN_COMMAND_INVALID(state,"control-memory-revoked-during-recording");
        static unsigned diagnostics;
        if(__atomic_fetch_add(&diagnostics,1,__ATOMIC_RELAXED)<16)
            LOG("NVN_COMMAND_BUFFER_CONTROL_REVOKED command_buffer=%p recording=%u",command_buffer,out->recording);
        return;
    }
    if (!state || !memory || !size) {
        if (state) {
            SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");
            /* Skyrim revokes its old control grant with NULL during teardown.
             * Do not retain a pointer the owner is about to free. */
            state->control_memory=NULL;state->control_size=state->control_used=0;state->control_provided=1;
        }
        LOG("NVN_COMMAND_BUFFER_CONTROL_ERROR command_buffer=%p memory=%p size=%#llx",
            command_buffer, memory, (unsigned long long)size);
        return;
    }
    state->control_memory = memory;
    state->control_size = size;
    state->control_used=0;state->control_provided=1;
    LOG("NVN_COMMAND_BUFFER_CONTROL command_buffer=%p memory=%p size=%#llx",
        command_buffer, memory, (unsigned long long)size);
}

static const sky_nvn_command_state_t *sky_nvn_command_state_valid(const void *command_buffer) {
    const sky_nvn_command_buffer_t *out=command_buffer;
    return out&&out->magic==SKY_NVN_MAGIC('C','M','D','B')?out->state:NULL;
}
uint64_t sky_nvn_command_buffer_get_command_memory_free(const void *command_buffer) {
    const sky_nvn_command_state_t *state=sky_nvn_command_state_valid(command_buffer);return state?state->command_size-state->command_used:0;
}
uint64_t sky_nvn_command_buffer_get_control_memory_free(const void *command_buffer) {
    const sky_nvn_command_state_t *state=sky_nvn_command_state_valid(command_buffer);return state?state->control_size-state->control_used:0;
}

void sky_nvn_command_buffer_begin_recording(void *command_buffer) {
    sky_nvn_command_buffer_t *out = command_buffer;
    if (!out || out->magic != SKY_NVN_MAGIC('C', 'M', 'D', 'B') || out->recording) {
        if (out) LOG("NVN_COMMAND_BUFFER_ERROR reason=begin-recording-state");
        return;
    }
    sky_nvn_command_state_reset(out->state);
    out->recording = 1;
    sky_nvn_command_reference_append(out->state,0,NULL);
    LOG("NVN_COMMAND_BUFFER_BEGIN command_buffer=%p", command_buffer);
}

uint64_t sky_nvn_command_buffer_end_recording(void *command_buffer) {
    sky_nvn_command_buffer_t *out = command_buffer;
    if (!out || out->magic != SKY_NVN_MAGIC('C', 'M', 'D', 'B') || !out->recording) {
        if (out) LOG("NVN_COMMAND_BUFFER_ERROR reason=end-recording-state");
        return 0;
    }
    sky_nvn_command_state_t *state=out->state;
    sky_nvn_operation_append(state,SKY_MTL_OP_STATE);
    sky_nvn_command_record_t *record=calloc(1,sizeof *record);
    if (!record) { SKY_NVN_COMMAND_INVALID(state,"record-allocation-failed"); return 0; }
    record->magic=SKY_NVN_MAGIC('C','M','D','S');
    record->invalid=state->invalid;
    record->command_buffer=command_buffer;
    record->regions=state->regions;state->regions=NULL;
    record->operations=state->operations;
    record->next=state->records;
    state->records=record;
    sky_nvn_command_reference_t *control=sky_nvn_command_memory_allocate(state,1,sizeof *control);
    if(control){sky_nvn_command_reference_t reference={SKY_NVN_MAGIC('C','M','D','S'),sizeof *control,(uint64_t)(uintptr_t)record};memcpy(control,&reference,sizeof reference);}
    record->invalid=state->invalid;
    record->failure=state->failure;
    state->operations=state->last_operation=NULL;
    out->recording = 0;
    LOG("NVN_COMMAND_BUFFER_END command_buffer=%p handle=%#llx", command_buffer, (unsigned long long)(uintptr_t)record);
    return (uint64_t)(uintptr_t)record;
}

uint32_t sky_nvn_command_buffer_is_recording(const void *command_buffer) {
    if (!command_buffer || ((const sky_nvn_command_buffer_t *)command_buffer)->magic != SKY_NVN_MAGIC('C', 'M', 'D', 'B'))
        return 0;
    return ((const sky_nvn_command_buffer_t *)command_buffer)->recording != 0;
}

void sky_nvn_command_buffer_set_render_targets(void *command_buffer, int count, void *const *textures,
    void *const *views, const void *depth_texture, const void *depth_view) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out ? out->state : NULL;
    if (!state || !out->recording) return;
    for (int index=0;index<8;index++) sky_mtl_texture_destroy(state->color_metal[index]);
    sky_mtl_texture_destroy(state->depth_metal);
    memset(state->color,0,sizeof state->color);
    memset(state->color_metal,0,sizeof state->color_metal);
    state->depth=NULL;
    state->depth_metal=NULL;
    if (count < 0 || count > 8 || (count > 0 && !textures)) {
        sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-color-target-count",count,(uint64_t)(uintptr_t)textures,0);
        return;
    }
    state->color_count = count;state->targets_bound=1;
    for (int index = 0; index < count; index++) {
        const sky_nvn_texture_t *texture = sky_nvn_texture_valid(textures[index]);
        if (!texture || !texture->metal) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-color-target",index,(uint64_t)(uintptr_t)textures[index],0);
            continue;
        }
        state->color[index] = textures[index];
        const sky_nvn_texture_view_t *view = views ? sky_nvn_texture_view_valid(views[index]) : NULL;
        if (views && views[index] && !view) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-color-view",index,(uint64_t)(uintptr_t)views[index],0);
            continue;
        }
        state->color_metal[index] = view ? sky_nvn_texture_view_create_metal(texture, view)
                                         : sky_mtl_texture_retain(texture->metal);
        if (!state->color_metal[index])sky_nvn_command_invalid(state,__func__,__LINE__,"unsupported-color-view",index,texture->target,view?view->state:0);
    }
    if (depth_texture) {
        const sky_nvn_texture_t *texture = sky_nvn_texture_valid(depth_texture);
        if (!texture || !texture->metal) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-depth-target",(uint64_t)(uintptr_t)depth_texture,0,0);
            return;
        }
        state->depth = depth_texture;
        const sky_nvn_texture_view_t *view = sky_nvn_texture_view_valid(depth_view);
        if (depth_view && !view) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-depth-view",(uint64_t)(uintptr_t)depth_view,0,0);
            return;
        }
        state->depth_metal = view ? sky_nvn_texture_view_create_metal(texture, view)
                                  : sky_mtl_texture_retain(texture->metal);
        if (!state->depth_metal)sky_nvn_command_invalid(state,__func__,__LINE__,"unsupported-depth-view",texture->target,view?view->state:0,0);
    }
    LOG("NVN_COMMAND_BUFFER_RENDER_TARGETS command_buffer=%p colors=%d depth=%p views=%p invalid=%d",
        command_buffer, count, depth_texture, views, state->invalid);
}

void sky_nvn_command_buffer_clear_color(void *command_buffer, int index, const float *rgba, int mask) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out ? out->state : NULL;
    if (!state || !out->recording) return;
    if (index < 0 || index >= 8 || !rgba || mask != 0xf ||
        (state->targets_bound&&(index>=state->color_count||!state->color[index]))) {
        SKY_NVN_COMMAND_INVALID(state,"unsupported-color-clear");
        return;
    }
    memcpy(state->clear_color, rgba, sizeof state->clear_color);
    state->has_clear_color = 1;
    state->clear_index = index;
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_CLEAR);
    if(op){op->targets.has_color_clear=1;op->targets.clear_index=index;memcpy(op->targets.colors[index],rgba,16);}
    LOG("NVN_COMMAND_BUFFER_CLEAR_COLOR command_buffer=%p index=%d mask=%#x", command_buffer, index, mask);
}

void sky_nvn_command_buffer_clear_depth_stencil(void *command_buffer, float depth,
    uint32_t depth_mask, int stencil, uint32_t stencil_mask) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out ? out->state : NULL;
    if (!state || !out->recording) return;
    depth_mask=!!(depth_mask&0xff);
    if ((depth_mask || stencil_mask) && state->targets_bound && !state->depth) {
        sky_nvn_command_invalid(state,__func__,__LINE__,"unsupported-depth-stencil-clear",depth_mask,stencil_mask,(uint64_t)(uintptr_t)state->depth);
        return;
    }
    if ((depth_mask || stencil_mask) && state->targets_bound) {
        sky_mtl_texture_properties_t properties={0};
        if (!sky_mtl_texture_get_properties(state->depth_metal, &properties) ||
            (depth_mask && properties.pixel_format != 250 && properties.pixel_format != 252 &&
             properties.pixel_format != 260)) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"unsupported-depth-stencil-format",properties.pixel_format,depth_mask,stencil_mask);
            return;
        }
        /* NVN uses one clear call for both aspects. Recognized depth-only
         * attachments have no stencil plane, so their stencil clear is inert. */
        if(properties.pixel_format==250||properties.pixel_format==252)stencil_mask=0;
        if(stencil_mask&&properties.pixel_format!=253&&properties.pixel_format!=260) {
            sky_nvn_command_invalid(state,__func__,__LINE__,"unsupported-depth-stencil-format",properties.pixel_format,depth_mask,stencil_mask);return;
        }
    }
    if(stencil_mask&&stencil_mask!=0xff) {
        sky_nvn_command_invalid(state,__func__,__LINE__,"unsupported-stencil-mask",depth_mask,stencil_mask,0);return;
    }
    state->clear_depth = depth;
    state->depth_mask = depth_mask;
    state->clear_stencil = stencil;
    state->stencil_mask = stencil_mask;
    sky_mtl_operation_t *op=sky_nvn_operation_append(state,SKY_MTL_OP_CLEAR);
    if(op){op->targets.clear_depth=depth;op->targets.depth_mask=depth_mask;op->targets.clear_stencil=stencil;op->targets.stencil_mask=stencil_mask;}
    LOG("NVN_COMMAND_BUFFER_CLEAR_DEPTH_STENCIL command_buffer=%p depth=%g depth_mask=%#x stencil=%#x stencil_mask=%#x",
        command_buffer, depth, depth_mask, stencil, stencil_mask);
}

void sky_nvn_command_buffer_set_viewports(void *command_buffer, int first, int count, const float *values) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out ? out->state : NULL;
    if (!state || !out->recording) return;
    if (first < 0 || count < 0 || first + count > 16 || (count > 0 && !values)) {
        sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-viewport-range",first,count,(uint64_t)(uintptr_t)values);
        return;
    }
    free(state->viewports);
    state->viewports = NULL;
    if (count) {
        state->viewports = malloc((size_t)count * 4 * sizeof(float));
        if (!state->viewports) {
            SKY_NVN_COMMAND_INVALID(state,"invalid-arguments");
            return;
        }
        memcpy(state->viewports, values, (size_t)count * 4 * sizeof(float));
    }
    state->viewport_first = first;
    state->viewport_count = count;
    if(first==0&&count){state->render.changed|=SKY_MTL_STATE_VIEWPORT;state->render.has_viewport=1;for(int i=0;i<4;i++)state->render.viewport[i]=values[i];}
}

void sky_nvn_command_buffer_set_scissor(void *command_buffer, int x, int y, int width, int height) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out ? out->state : NULL;
    if (!state || !out->recording) return;
    if (width < 0 || height < 0) {
        sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-scissor",width,height,0);
        return;
    }
    state->scissor[0] = x;
    state->scissor[1] = y;
    state->scissor[2] = width;
    state->scissor[3] = height;
    state->render.has_scissor=1;state->render.changed|=SKY_MTL_STATE_SCISSOR;
    memcpy(state->render.scissor,state->scissor,sizeof state->scissor);
}

void sky_nvn_command_buffer_set_depth_range(void *command_buffer, float near_value, float far_value) {
    sky_nvn_command_buffer_t *out = command_buffer;
    sky_nvn_command_state_t *state = out ? out->state : NULL;
    if (!state || !out->recording) return;
    if (near_value > far_value) {
        sky_nvn_command_invalid(state,__func__,__LINE__,"invalid-depth-range",sky_nvn_float_bits(near_value),sky_nvn_float_bits(far_value),0);
        return;
    }
    state->depth_range[0] = near_value;
    state->depth_range[1] = far_value;
    state->render.changed|=SKY_MTL_STATE_DEPTH_RANGE;
    state->render.viewport[4]=near_value;state->render.viewport[5]=far_value;
}

static uint32_t g_frame_submitted_draws;

void sky_nvn_queue_submit_commands(void *queue, int count, const uint64_t *handles) {
    sky_nvn_queue_t *out=sky_nvn_queue_valid(queue);
    if (!out || count<0 || (count && !handles)) return;
    for (int index=0;index<count;index++) {
        const sky_nvn_command_record_t *record=(const void *)(uintptr_t)handles[index];
        if (!record || record->magic!=SKY_NVN_MAGIC('C','M','D','S') || record->invalid) {
            const sky_nvn_command_failure_t *failure=record&&record->magic==SKY_NVN_MAGIC('C','M','D','S')?&record->failure:NULL;
            static unsigned diagnostics;unsigned rejected=__atomic_add_fetch(&diagnostics,1,__ATOMIC_RELAXED);
            if(rejected<=128||!(rejected&255))
                LOG("NVN_QUEUE_SUBMIT_ERROR reason=invalid-command-state index=%d handle=%#llx command_buffer=%p rejected=%u invalid_api=%s invalid_line=%u invalid_reason=%s arguments={%#llx,%#llx,%#llx}",
                    index,(unsigned long long)handles[index],failure?record->command_buffer:NULL,rejected,
                    failure&&failure->api?failure->api:"unknown",failure?failure->line:0,
                    failure&&failure->reason?failure->reason:"invalid-handle",
                    (unsigned long long)(failure?failure->arguments[0]:0),(unsigned long long)(failure?failure->arguments[1]:0),(unsigned long long)(failure?failure->arguments[2]:0));
            return;
        }
        uint32_t cmd_draws=0;
        for(const sky_mtl_operation_t *op=record->operations;op;op=op->next)if(op->kind==SKY_MTL_OP_DRAW)cmd_draws++;
        if(cmd_draws)__atomic_add_fetch(&g_frame_submitted_draws,cmd_draws,__ATOMIC_RELAXED);
        pthread_mutex_lock(&out->lock);
        uint64_t submits=++out->submits;
        pthread_mutex_unlock(&out->lock);
        if (!sky_mtl_command_queue_submit_operations(out->metal,record->operations,sky_nvn_queue_complete,out)) {
            pthread_mutex_lock(&out->lock);
            out->submits--;
            pthread_mutex_unlock(&out->lock);
            __atomic_store_n(&out->error,1,__ATOMIC_RELAXED);
            LOG("NVN_QUEUE_SUBMIT_ERROR reason=metal-submit-failed index=%d",index);
            return;
        }
        sky_checkpoint("post_nnMain_nvn_queue_submit_commands");
        if(submits<=64||submits%256==0||sky_verbose_logging())
            LOG("NVN_QUEUE_SUBMIT queue=%p handle=%#llx submits=%llu completed=%llu",queue,
                (unsigned long long)handles[index],(unsigned long long)submits,(unsigned long long)out->completed);
    }
}

void sky_nvn_window_builder_set_defaults(void *builder) {
    if (!builder) return;
    sky_nvn_window_builder_t *out = builder;
    void *device = out->dev;
    memset(out, 0, sizeof *out);
    out->dev = device;
    out->present_interval = 1;
}

void sky_nvn_window_builder_set_device(void *builder, void *device) {
    if (builder) ((sky_nvn_window_builder_t *)builder)->dev = device;
}

void sky_nvn_window_builder_set_native_window(void *builder, void *native) {
    if (builder) ((sky_nvn_window_builder_t *)builder)->native = native;
}

void sky_nvn_window_builder_set_textures(void *builder, int count, void *const *textures) {
    if (!builder) return;
    sky_nvn_window_builder_t *out = builder;
    if (count < 0 || count > 4 || (count > 0 && !textures)) {
        LOG("NVN_WINDOW_BUILDER_ERROR reason=invalid-textures count=%d", count);
        out->texture_count = 0;
        memset(out->textures, 0, sizeof out->textures);
        return;
    }
    out->texture_count = count;
    memset(out->textures, 0, sizeof out->textures);
    for (int index = 0; index < count; index++) out->textures[index] = textures[index];
}

void sky_nvn_window_builder_set_present_interval(void *builder, int interval) {
    if (builder) ((sky_nvn_window_builder_t *)builder)->present_interval = interval;
}

void sky_nvn_window_builder_set_num_active_textures(void *builder, int count) {
    if (builder) ((sky_nvn_window_builder_t *)builder)->active_texture_count = count;
}

void *sky_nvn_window_builder_get_device(const void *builder) {
    return builder ? ((const sky_nvn_window_builder_t *)builder)->dev : NULL;
}

int sky_nvn_window_builder_get_num_textures(const void *builder) {
    return builder ? ((const sky_nvn_window_builder_t *)builder)->texture_count : 0;
}

void *sky_nvn_window_builder_get_texture(const void *builder, int index) {
    if (!builder || index < 0 || index >= 4) return NULL;
    return ((const sky_nvn_window_builder_t *)builder)->textures[index];
}

void *sky_nvn_window_builder_get_native_window(const void *builder) {
    return builder ? ((const sky_nvn_window_builder_t *)builder)->native : NULL;
}

int sky_nvn_window_builder_get_present_interval(const void *builder) {
    return builder ? ((const sky_nvn_window_builder_t *)builder)->present_interval : 0;
}

int sky_nvn_window_builder_get_num_active_textures(const void *builder) {
    return builder ? ((const sky_nvn_window_builder_t *)builder)->active_texture_count : 0;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint64_t last_present_ns;
    unsigned active,waiting;
    int closing,presenting;
} sky_nvn_window_present_state_t;
static pthread_mutex_t g_window_present_lifetime_lock=PTHREAD_MUTEX_INITIALIZER;

static int sky_nvn_window_present_begin(sky_nvn_window_t *window,sky_nvn_window_present_state_t **state) {
    pthread_mutex_lock(&g_window_present_lifetime_lock);
    int valid=window&&window->magic==SKY_NVN_MAGIC('W','I','N','D')&&window->present_state;
    *state=valid?window->present_state:NULL;
    if(*state){pthread_mutex_lock(&(*state)->lock);(*state)->active++;pthread_mutex_unlock(&(*state)->lock);}
    pthread_mutex_unlock(&g_window_present_lifetime_lock);return valid;
}
static int sky_nvn_window_present_end(sky_nvn_window_present_state_t *state,int slot,int submitted) {
    if(!state)return 1;
    int valid=1;
    pthread_mutex_lock(&state->lock);
    if(slot) {
        if(submitted) {
            struct timespec clock;
            if(clock_gettime(CLOCK_MONOTONIC,&clock))valid=0;
            else state->last_present_ns=(uint64_t)clock.tv_sec*1000000000+(uint64_t)clock.tv_nsec;
        }
        state->presenting=0;pthread_cond_broadcast(&state->condition);
    }
    if(!--state->active)pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->lock);return valid;
}
uint32_t sky_nvn_window_present_waiters_for_testing(const void *window) {
    pthread_mutex_lock(&g_window_present_lifetime_lock);
    const sky_nvn_window_t *w=window;
    sky_nvn_window_present_state_t *state=w&&w->magic==SKY_NVN_MAGIC('W','I','N','D')?w->present_state:NULL;
    unsigned waiting=0;
    if(state){pthread_mutex_lock(&state->lock);waiting=state->waiting;pthread_mutex_unlock(&state->lock);}
    pthread_mutex_unlock(&g_window_present_lifetime_lock);return waiting;
}
uint64_t sky_nvn_window_last_present_ns_for_testing(const void *window) {
    pthread_mutex_lock(&g_window_present_lifetime_lock);
    const sky_nvn_window_t *w=window;
    sky_nvn_window_present_state_t *state=w&&w->magic==SKY_NVN_MAGIC('W','I','N','D')?w->present_state:NULL;
    uint64_t stamp=0;
    if(state){pthread_mutex_lock(&state->lock);stamp=state->last_present_ns;pthread_mutex_unlock(&state->lock);}
    pthread_mutex_unlock(&g_window_present_lifetime_lock);return stamp;
}

static int sky_nvn_window_pace_headless(sky_nvn_window_t *window,sky_nvn_window_present_state_t *state) {
    if(!state)return __atomic_load_n(&window->present_interval,__ATOMIC_RELAXED)<=0?1:-1;
    pthread_mutex_lock(&state->lock);
    for(;;) {
        if(state->closing){pthread_mutex_unlock(&state->lock);return 0;}
        if(state->presenting) {
            state->waiting++;int error=pthread_cond_wait(&state->condition,&state->lock);state->waiting--;
            if(error){pthread_mutex_unlock(&state->lock);LOG("NVN_WINDOW_PACING_ERROR reason=wait-failed error=%d",error);return -1;}
            continue;
        }
        struct timespec clock;
        if(clock_gettime(CLOCK_MONOTONIC,&clock)) {
            pthread_mutex_unlock(&state->lock);LOG("NVN_WINDOW_PACING_ERROR reason=monotonic-clock-failed");return -1;
        }
        uint64_t now=(uint64_t)clock.tv_sec*1000000000+(uint64_t)clock.tv_nsec;
        int interval=__atomic_load_n(&window->present_interval,__ATOMIC_RELAXED);
        uint64_t period=interval>0?(uint64_t)interval*1000000000/60:0;
        uint64_t deadline=state->last_present_ns+period;
        if(!period||!state->last_present_ns||now>=deadline) {
            /* Serialize only this window's submission. The next deadline is
             * anchored after enqueue, so a stalled enqueue cannot cause bursts. */
            state->presenting=1;pthread_mutex_unlock(&state->lock);return 1;
        }
        uint64_t left=deadline-now;
        struct timespec remaining={(time_t)(left/1000000000),(long)(left%1000000000)};
        state->waiting++;
        int error=pthread_cond_timedwait_relative_np(&state->condition,&state->lock,&remaining);
        state->waiting--;
        if(error&&error!=ETIMEDOUT) {
            pthread_mutex_unlock(&state->lock);LOG("NVN_WINDOW_PACING_ERROR reason=wait-failed error=%d",error);return -1;
        }
    }
}

uint32_t sky_nvn_window_initialize(void *window, const void *builder) {
    if (!window || !builder) return 0;
    const sky_nvn_window_builder_t *in = builder;
    sky_nvn_window_t *out = window;
    memset(out, 0, sizeof *out);
    if (!in->dev || ((const sky_nvn_device_t *)in->dev)->magic != 0x44455630) {
        LOG("NVN_WINDOW_INIT_ERROR reason=invalid-device device=%p", in->dev);
        return 0;
    }
    if (!in->native) {
        LOG("NVN_WINDOW_INIT_ERROR reason=invalid-native-window");
        return 0;
    }
    if (in->texture_count < 0 || in->texture_count > 4) {
        LOG("NVN_WINDOW_INIT_ERROR reason=invalid-texture-count count=%d", in->texture_count);
        return 0;
    }
    for (int index = 0; index < in->texture_count; index++) {
        const sky_nvn_texture_t *texture = sky_nvn_texture_valid(in->textures[index]);
        if (!texture || !texture->metal) {
            for (int cleanup = 0; cleanup < index; cleanup++) sky_mtl_texture_destroy(out->metal_textures[cleanup]);
            memset(out, 0, sizeof *out);
            const sky_nvn_texture_t *candidate = in->textures[index];
            LOG("NVN_WINDOW_INIT_ERROR reason=invalid-texture index=%d texture=%p magic=%#x metal=%p",
                index, (const void *)candidate, candidate ? candidate->magic : 0,
                candidate ? candidate->metal : NULL);
            return 0;
        }
        out->metal_textures[index] = sky_mtl_texture_retain(texture->metal);
        if (!out->metal_textures[index]) {
            for (int cleanup = 0; cleanup < index; cleanup++) sky_mtl_texture_destroy(out->metal_textures[cleanup]);
            memset(out, 0, sizeof *out);
            LOG("NVN_WINDOW_INIT_ERROR reason=metal-texture-retain-failed index=%d", index);
            return 0;
        }
    }
    sky_nvn_window_present_state_t *present=calloc(1,sizeof *present);
    int mutex_error=present?pthread_mutex_init(&present->lock,NULL):ENOMEM;
    int cond_error=mutex_error?mutex_error:pthread_cond_init(&present->condition,NULL);
    if(mutex_error||cond_error) {
        if(!mutex_error)pthread_mutex_destroy(&present->lock);
        free(present);
        for(int index=0;index<4;index++)sky_mtl_texture_destroy(out->metal_textures[index]);
        memset(out,0,sizeof *out);LOG("NVN_WINDOW_INIT_ERROR reason=pacing-state-failed");return 0;
    }
    out->present_state=present;
    out->magic = SKY_NVN_MAGIC('W', 'I', 'N', 'D');
    out->dev = in->dev;
    out->native = in->native;
    out->texture_count = in->texture_count;
    out->present_interval = in->present_interval;
    out->active_texture_count = in->active_texture_count;
    memcpy(out->textures, in->textures, sizeof out->textures);
    sky_checkpoint("post_nnMain_nvn_window_initialize");
    LOG("NVN_WINDOW_INITIALIZED window=%p device=%p native=%p textures=%d active_textures=%d present_interval=%d metal_textures=%d",
        window, out->dev, out->native, out->texture_count, out->active_texture_count, out->present_interval,
        out->texture_count);
    return 1;
}

void sky_nvn_window_finalize(void *window) {
    sky_nvn_window_t *out = window;
    pthread_mutex_lock(&g_window_present_lifetime_lock);
    if(!out||out->magic!=SKY_NVN_MAGIC('W','I','N','D')){pthread_mutex_unlock(&g_window_present_lifetime_lock);return;}
    out->magic=0;
    sky_nvn_window_present_state_t *present=out->present_state;out->present_state=NULL;
    if(present){pthread_mutex_lock(&present->lock);present->closing=1;pthread_cond_broadcast(&present->condition);}
    pthread_mutex_unlock(&g_window_present_lifetime_lock);
    if(present){
        while(present->active)pthread_cond_wait(&present->condition,&present->lock);
        pthread_mutex_unlock(&present->lock);
        pthread_cond_destroy(&present->condition);pthread_mutex_destroy(&present->lock);free(present);
    }
    pthread_mutex_lock(&g_window_present_lifetime_lock);
    LOG("NVN_WINDOW_FINALIZED window=%p", window);
    for (int index = 0; index < 4; index++) sky_mtl_texture_destroy(out->metal_textures[index]);
    memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&g_window_present_lifetime_lock);
}

void sky_nvn_window_set_present_interval(void *window, int interval) {
    sky_nvn_window_t *out=window;
    pthread_mutex_lock(&g_window_present_lifetime_lock);
    if(!out||out->magic!=SKY_NVN_MAGIC('W','I','N','D')){pthread_mutex_unlock(&g_window_present_lifetime_lock);return;}
    sky_nvn_window_present_state_t *state=out->present_state;
    if(state)pthread_mutex_lock(&state->lock);
    __atomic_store_n(&out->present_interval,interval,__ATOMIC_RELAXED);
    if(state){pthread_cond_broadcast(&state->condition);pthread_mutex_unlock(&state->lock);}
    pthread_mutex_unlock(&g_window_present_lifetime_lock);
}

void sky_nvn_window_set_num_active_textures(void *window, int count) {
    if (window) ((sky_nvn_window_t *)window)->active_texture_count = count;
}

void sky_nvn_window_set_crop(void *window, int x, int y, int width, int height) {
    if (!window) return;
    sky_nvn_window_t *out = window;
    out->crop[0] = x;
    out->crop[1] = y;
    out->crop[2] = width;
    out->crop[3] = height;
    LOG("NVN_WINDOW_CROP window=%p x=%d y=%d width=%d height=%d",window,x,y,width,height);
}

void sky_nvn_window_get_crop(const void *window, sky_nvn_rectangle_t *crop) {
    if (!window || !crop) return;
    const sky_nvn_window_t *in = window;
    crop->x = in->crop[0];
    crop->y = in->crop[1];
    crop->width = in->crop[2];
    crop->height = in->crop[3];
}

void *sky_nvn_window_get_native_window(const void *window) {
    return window ? ((const sky_nvn_window_t *)window)->native : NULL;
}

int sky_nvn_window_get_num_textures(const void *window) {
    return window ? ((const sky_nvn_window_t *)window)->texture_count : 0;
}

int sky_nvn_window_get_num_active_textures(const void *window) {
    return window ? ((const sky_nvn_window_t *)window)->active_texture_count : 0;
}

int sky_nvn_window_get_present_interval(const void *window) {
    return window ? __atomic_load_n(&((const sky_nvn_window_t *)window)->present_interval,__ATOMIC_RELAXED) : 0;
}

void sky_nvn_unimplemented(void) {
    extern void crash_backtrace(uint64_t frame_pointer);
    sky_checkpoint("post_nnMain_nvn_unimplemented");
    LOG("NVN_UNIMPL name=%s caller=%p", g_last_name ? g_last_name : "(null)", __builtin_return_address(0));
    crash_backtrace((uint64_t)__builtin_frame_address(0));
    LOG("RUN_STOP checkpoint=post_nnMain_nvn_unimplemented reason=UNIMPL_NVN");
    abort();
}

SKY_FORCE("nvnBootstrapLoader", "Return only audited host NVN functions and named unimplemented traps",
          void *, sky_nvn_bootstrap_entry, const char *name) { return sky_nvn_bootstrap_loader(name); }

uint64_t sky_nvn_device_get_texture_handle(const void *device,int texture,int sampler) {
    (void)device;return (uint64_t)(texture&0xfffff)|((uint64_t)(sampler&0xfff)<<20);
}
uint64_t sky_nvn_device_get_separate_texture_handle(const void *device,int texture) {
    (void)device;return (uint64_t)(texture&0xfffff);
}
uint64_t sky_nvn_device_get_separate_sampler_handle(const void *device,int sampler) {
    (void)device;return (uint64_t)(sampler&0xfff)<<20;
}
uint64_t sky_nvn_device_get_image_handle(const void *device,int image) {
    (void)device;return (uint64_t)(image&0xfffff);
}

#if TARGET_OS_IPHONE
__attribute__((weak)) void *sky_ios_metal_layer(void) { return NULL; }
#define sky_platform_metal_layer sky_ios_metal_layer
#else
__attribute__((weak)) void *sky_macos_metal_layer(void) { return NULL; }
#define sky_platform_metal_layer sky_macos_metal_layer
#endif
int sky_nvn_queue_acquire_texture(void *queue,void *window,int *index) {
    sky_nvn_queue_t *q=sky_nvn_queue_valid(queue);
    sky_nvn_window_t *w=window;
    if(!q||!w||w->magic!=SKY_NVN_MAGIC('W','I','N','D')||!index||w->texture_count<=0)return 1;
    *index=w->next_texture;
    w->next_texture=(w->next_texture+1)%(w->active_texture_count>0?w->active_texture_count:w->texture_count);
    if(w->presents==0)sky_nvn_queue_finish(queue);
    if(w->presents<64||w->presents%120==0||sky_verbose_logging())
        LOG("NVN_QUEUE_ACQUIRE queue=%p window=%p texture=%d",queue,window,*index);
    return 0;
}
int sky_nvn_window_acquire_texture(void *window,void *sync,int *index) {
    sky_nvn_window_t *w=window;
    sky_nvn_sync_t *s=sync;
    if(!w||w->magic!=SKY_NVN_MAGIC('W','I','N','D')||!index||w->texture_count<=0)return 1;
    *index=w->next_texture;w->next_texture=(w->next_texture+1)%(w->active_texture_count>0?w->active_texture_count:w->texture_count);
    if(s&&s->magic==SKY_NVN_MAGIC('S','Y','N','C')){s->fenced=2;s->queue=NULL;s->fence=0;}
    return 0;
}
typedef struct { sky_nvn_queue_t *queue; sky_mtl_texture_t *texture; uint64_t frame; } sky_nvn_present_completion_t;
static uint64_t g_present_completions;
static int sky_nvn_capture_frame(sky_mtl_texture_t *texture,const char *path) {
    sky_mtl_texture_properties_t p;
    if(!path||!sky_mtl_texture_get_properties(texture,&p)||(p.pixel_format!=70&&p.pixel_format!=71&&p.pixel_format!=80&&p.pixel_format!=81))return 0;
    if(p.width>16384||p.height>16384||!p.width||!p.height)return 0;
    size_t size=(size_t)p.width*(size_t)p.height*4;
    uint8_t *pixels=malloc(size);if(!pixels)return 0;
    if(!sky_mtl_texture_readback(texture,0,0,SKY_MTL_READ_COLOR,pixels,size,NULL)){free(pixels);return 0;}
    FILE *file=fopen(path,"wb");if(!file){free(pixels);return 0;}
    fprintf(file,"P6\n%llu %llu\n255\n",(unsigned long long)p.width,(unsigned long long)p.height);
    size_t pixel_count=(size_t)p.width*(size_t)p.height;
    for(size_t i=0;i<pixel_count;i++) {
        const uint8_t *rgba=pixels+i*4;
        uint8_t rgb[3]={rgba[p.pixel_format==80||p.pixel_format==81?2:0],rgba[1],rgba[p.pixel_format==80||p.pixel_format==81?0:2]};
        memcpy(pixels+i*3,rgb,3);
    }
    if(fwrite(pixels,1,pixel_count*3,file)!=pixel_count*3){fclose(file);free(pixels);return 0;}
    int valid=fclose(file)==0;free(pixels);return valid;
}
static void sky_nvn_present_complete(void *context) {
    sky_nvn_present_completion_t *p=context;
    uint64_t frames=__atomic_add_fetch(&g_present_completions,1,__ATOMIC_RELAXED);
    sky_mtl_graphics_stats_t stats=sky_mtl_graphics_stats();
    const char *limit_text=getenv("SKY_PRESENT_LIMIT");
    uint64_t limit=limit_text?strtoull(limit_text,NULL,10):0;
    const char *capture=getenv("SKY_FRAME_CAPTURE");
    const char *every_text=getenv("SKY_FRAME_CAPTURE_EVERY");
    uint64_t every=every_text?strtoull(every_text,NULL,10):0;
    if(capture&&(frames==1||(every&&frames%every==0)||(limit&&frames==limit))) {
        int valid=sky_nvn_capture_frame(p->texture,capture);
        LOG("NVN_FRAME_CAPTURE frame=%llu path=%s success=%d",(unsigned long long)frames,capture,valid);
    }
    if(frames<=64||frames%60==0||limit||sky_verbose_logging())
        LOG("NVN_PRESENT_COMPLETED frames=%llu draws=%llu dispatches=%llu gpu_errors=%llu",
            (unsigned long long)frames,(unsigned long long)stats.draws,(unsigned long long)stats.dispatches,(unsigned long long)stats.command_errors);
    int stop=limit&&frames>=limit&&(stats.draws||stats.dispatches)&&!stats.command_errors&&!sky_nvn_queue_get_error(p->queue,NULL);
    sky_nvn_queue_complete(p->queue);
    sky_mtl_texture_destroy(p->texture);free(p);
    if(stop) {
        LOG("PRESENT_LIMIT_REACHED frames=%llu draws=%llu dispatches=%llu",(unsigned long long)frames,
            (unsigned long long)stats.draws,(unsigned long long)stats.dispatches);
        fflush(NULL);_exit(0);
    }
}

void sky_nvn_queue_present_texture(void *queue,void *window,int index) {
    sky_nvn_queue_t *q=sky_nvn_queue_valid(queue);
    sky_nvn_window_t *w=window;
    sky_nvn_window_present_state_t *present=NULL;
    int slot=0,submitted=0;
    if(!q||!sky_nvn_window_present_begin(w,&present))return;
    if(index<0||index>=(w->active_texture_count>0?w->active_texture_count:w->texture_count))goto done;
    void *layer=sky_platform_metal_layer();
    /* Simulator drawables lack native minimum-duration presentation. */
    int host_pacing=!layer||TARGET_OS_SIMULATOR;
    int paced=host_pacing?sky_nvn_window_pace_headless(w,present):1;
    if(paced<=0) {
        if(paced<0){__atomic_store_n(&q->error,1,__ATOMIC_RELAXED);LOG("NVN_QUEUE_PRESENT_ERROR reason=headless-pacing-failed");}
        else LOG("NVN_QUEUE_PRESENT_CANCELLED reason=window-finalized");
        goto done;
    }
    slot=host_pacing;
    sky_nvn_present_completion_t *completion=calloc(1,sizeof *completion);if(!completion)goto done;
    completion->queue=q;completion->texture=sky_mtl_texture_retain(w->metal_textures[index]);completion->frame=w->presents+1;
    pthread_mutex_lock(&q->lock);q->submits++;pthread_mutex_unlock(&q->lock);
    submitted=layer ? sky_mtl_command_queue_present_texture(q->metal,w->metal_textures[index],layer,sky_nvn_window_get_present_interval(w),w->crop,sky_nvn_present_complete,completion)
        :sky_mtl_command_queue_submit_empty(q->metal,0,sky_nvn_present_complete,completion);
    if(!submitted){pthread_mutex_lock(&q->lock);q->submits--;pthread_mutex_unlock(&q->lock);sky_mtl_texture_destroy(completion->texture);free(completion);__atomic_store_n(&q->error,1,__ATOMIC_RELAXED);LOG("NVN_QUEUE_PRESENT_ERROR reason=metal-present-failed");goto done;}
    w->presents++;
    uint32_t frame_draws=__atomic_exchange_n(&g_frame_submitted_draws,0,__ATOMIC_RELAXED);
    sky_set_last_frame_draw_count(frame_draws);
    sky_checkpoint("post_nnMain_nvn_queue_present_texture");
    if(w->presents<=64||w->presents%60==0||sky_verbose_logging())
        LOG("NVN_QUEUE_PRESENT queue=%p window=%p texture=%d presents=%llu visible=%d",queue,window,index,(unsigned long long)w->presents,layer!=NULL);
done:
    if(!sky_nvn_window_present_end(present,slot,submitted)) {
        __atomic_store_n(&q->error,1,__ATOMIC_RELAXED);LOG("NVN_QUEUE_PRESENT_ERROR reason=monotonic-clock-failed");
    }
}
