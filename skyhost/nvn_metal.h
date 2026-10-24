#pragma once

#include <stddef.h>
#include <stdint.h>
#include "nvn_transfer.h"

typedef struct sky_mtl_memory_pool sky_mtl_memory_pool_t;

sky_mtl_memory_pool_t *sky_mtl_memory_pool_create(void *memory, uint64_t size);
sky_mtl_memory_pool_t *sky_mtl_memory_pool_retain(sky_mtl_memory_pool_t *pool);
void sky_mtl_memory_pool_destroy(sky_mtl_memory_pool_t *pool);
void *sky_mtl_memory_pool_contents(const sky_mtl_memory_pool_t *pool);
uint64_t sky_mtl_memory_pool_gpu_address(const sky_mtl_memory_pool_t *pool);
uint64_t sky_mtl_memory_pool_wrapped_size(const sky_mtl_memory_pool_t *pool);
int sky_mtl_memory_pool_is_shared(const sky_mtl_memory_pool_t *pool);
void sky_mtl_memory_pool_set_debug_label(sky_mtl_memory_pool_t *pool, const char *label);
size_t sky_mtl_memory_pool_active_count(void);

typedef struct sky_mtl_command_queue sky_mtl_command_queue_t;

sky_mtl_command_queue_t *sky_mtl_command_queue_create(void);
void sky_mtl_command_queue_destroy(sky_mtl_command_queue_t *queue);
size_t sky_mtl_command_queue_active_count(void);
typedef void (*sky_mtl_completion_fn)(void *context);
int sky_mtl_command_queue_submit_empty(sky_mtl_command_queue_t *queue, uint64_t delay_ns,
                                       sky_mtl_completion_fn completion, void *context);

enum {
    SKY_MTL_TEXTURE_TYPE_1D = 0,
    SKY_MTL_TEXTURE_TYPE_1D_ARRAY = 1,
    SKY_MTL_TEXTURE_TYPE_2D = 2,
    SKY_MTL_TEXTURE_TYPE_2D_ARRAY = 3,
    SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE = 4,
    SKY_MTL_TEXTURE_TYPE_CUBE = 5,
    SKY_MTL_TEXTURE_TYPE_CUBE_ARRAY = 6,
    SKY_MTL_TEXTURE_TYPE_3D = 7,
    SKY_MTL_TEXTURE_TYPE_2D_MULTISAMPLE_ARRAY = 8,
};
enum {
    SKY_MTL_TEXTURE_USAGE_SHADER_READ = 1,
    SKY_MTL_TEXTURE_USAGE_SHADER_WRITE = 2,
    SKY_MTL_TEXTURE_USAGE_RENDER_TARGET = 4,
    SKY_MTL_TEXTURE_USAGE_PIXEL_FORMAT_VIEW = 0x10,
};
enum { SKY_MTL_STORAGE_SHARED=0, SKY_MTL_STORAGE_PRIVATE=2 };

typedef struct {
    uint32_t pixel_format, texture_type, storage_mode;
    uint64_t width, height, depth, array_length, mipmap_level_count, sample_count, usage;
    uint8_t swizzle[4];
} sky_mtl_texture_properties_t;
typedef struct sky_mtl_texture sky_mtl_texture_t;

sky_mtl_texture_t *sky_mtl_texture_create(const sky_mtl_texture_properties_t *properties);
sky_mtl_texture_t *sky_mtl_texture_retain(sky_mtl_texture_t *texture);
sky_mtl_texture_t *sky_mtl_texture_view_create(sky_mtl_texture_t *base, uint32_t pixel_format,
    uint32_t texture_type, uint64_t level_start, uint64_t level_count,
    uint64_t slice_start, uint64_t slice_count, const uint8_t swizzle[4]);
void sky_mtl_texture_destroy(sky_mtl_texture_t *texture);
int sky_mtl_texture_get_properties(const sky_mtl_texture_t *texture, sky_mtl_texture_properties_t *properties);
/* Logical compressed properties remain BC when an unsupported device uses a
 * decoded native texture. This flag is stable across retained copies/views. */
int sky_mtl_texture_is_bc_fallback(const sky_mtl_texture_t *texture);
/* BC fallback transfers require a completed GPU prefix before CPU block
 * conversion/readback. Only the queue executor owns that synchronization. */
int sky_mtl_transfer_requires_cpu_boundary(const sky_mtl_transfer_t *transfer);
int sky_mtl_transfer_execute_cpu(const sky_mtl_transfer_t *transfer);
uint64_t sky_mtl_texture_resource_id(const sky_mtl_texture_t *texture);
void sky_mtl_texture_set_debug_label(sky_mtl_texture_t *texture, const char *label);
size_t sky_mtl_texture_active_count(void);

typedef struct {
    void *textures[8];
    float colors[8][4];
    uint32_t color_count, has_color_clear, clear_index;
    void *depth_texture;
    float clear_depth;
    uint32_t depth_mask, stencil_mask;
    int clear_stencil;
} sky_mtl_clear_pass_t;
int sky_mtl_command_queue_submit_clear(sky_mtl_command_queue_t *queue, const sky_mtl_clear_pass_t *pass,
                                       sky_mtl_completion_fn completion, void *context);

int sky_mtl_texture_upload(sky_mtl_texture_t *texture, uint64_t level, uint64_t slice,
                           const int region[6], const void *data);
int sky_mtl_texture_upload_packaged(sky_mtl_texture_t *texture, int nvn_format,
                                    uint64_t level, uint64_t slice, const int region[6], const void *data);

enum { SKY_MTL_READ_COLOR, SKY_MTL_READ_DEPTH, SKY_MTL_READ_STENCIL };
int sky_mtl_texture_readback(sky_mtl_texture_t *texture, uint64_t level, uint64_t slice, int aspect,
                             void *out, size_t out_size, size_t *read_size);
/* Diagnostic pixels in the physical decoded format; never guest BC readback. */
int sky_mtl_texture_readback_decoded(sky_mtl_texture_t *texture,uint64_t level,uint64_t slice,
                                    void *out,size_t out_size,size_t *read_size);

/* Translated Maxwell stages use the dmsh route B argument-buffer reflection. */
int sky_mtl_program_set_shader(uint64_t serial, int stage, const char *msl_path,
                               const char *reflection_path);
void sky_mtl_program_remove(uint64_t serial);
void sky_mtl_program_retain(uint64_t serial);
void sky_mtl_program_release(uint64_t serial);

typedef struct sky_mtl_sampler sky_mtl_sampler_t;
typedef struct {
    int min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction_filter;
    float lod_min, lod_max, max_anisotropy, lod_bias;
} sky_mtl_sampler_properties_t;
sky_mtl_sampler_t *sky_mtl_sampler_create(const sky_mtl_sampler_properties_t *properties);
sky_mtl_sampler_t *sky_mtl_sampler_retain(sky_mtl_sampler_t *sampler);
void sky_mtl_sampler_destroy(sky_mtl_sampler_t *sampler);

typedef struct { uint64_t address, size; } sky_mtl_buffer_binding_t;
enum {
    SKY_MTL_STATE_ATTRIBUTES=1u<<0, SKY_MTL_STATE_STREAMS=1u<<1,
    SKY_MTL_STATE_CHANNEL=1u<<2, SKY_MTL_STATE_COLOR=1u<<3,
    SKY_MTL_STATE_DEPTH=1u<<4, SKY_MTL_STATE_POLYGON=1u<<5,
    SKY_MTL_STATE_VIEWPORT=1u<<6, SKY_MTL_STATE_SCISSOR=1u<<7,
    SKY_MTL_STATE_DEPTH_RANGE=1u<<8, SKY_MTL_STATE_BLEND_COLOR=1u<<9,
    SKY_MTL_STATE_STENCIL_READ_FRONT=1u<<10, SKY_MTL_STATE_STENCIL_READ_BACK=1u<<11,
    SKY_MTL_STATE_STENCIL_WRITE_FRONT=1u<<12, SKY_MTL_STATE_STENCIL_WRITE_BACK=1u<<13,
    SKY_MTL_STATE_STENCIL_REF_FRONT=1u<<14, SKY_MTL_STATE_STENCIL_REF_BACK=1u<<15,
    SKY_MTL_STATE_COPY_ROW=1u<<16, SKY_MTL_STATE_COPY_IMAGE=1u<<17,
    SKY_MTL_STATE_POLYGON_OFFSET=1u<<18,
};
typedef struct {
    int64_t copy_row_stride,copy_image_stride;
    uint64_t programs[6];
    uint32_t changed, program_mask, blend_mask, pool_mask;
    void *texture_pool, *sampler_pool;
    uint8_t uniform_bound[6][16], storage_bound[6][16], vertex_bound[16];
    sky_mtl_buffer_binding_t uniform[6][16], storage[6][16], vertex[16];
    void *textures[6][128], *samplers[6][128], *images[6][32];
    uint64_t texture_handles[6][128], sampler_handles[6][128], image_handles[6][32];
    uint8_t texture_bound[6][128], sampler_bound[6][128], image_bound[6][32];
    struct { uint32_t format, offset, stream; } attributes[16];
    struct { uint32_t stride, divisor; } streams[16];
    uint32_t attribute_count, stream_count;
    uint32_t channel_mask, blend_enable;
    struct { uint8_t rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha; } blend[8];
    uint8_t depth_test, depth_write, depth_func, stencil_test;
    struct { uint8_t function, fail, depth_fail, pass, read_mask, write_mask, reference; } stencil[2];
    uint8_t cull_face, front_face, polygon_mode, polygon_offset_enables;
    float polygon_offset_factor,polygon_offset_units,polygon_offset_clamp;
    uint8_t has_viewport, has_scissor, depth_mode;
    double viewport[6];
    int32_t scissor[4];
    float blend_color[4];
} sky_mtl_render_state_t;

enum { SKY_MTL_OP_CLEAR, SKY_MTL_OP_DRAW, SKY_MTL_OP_DISPATCH, SKY_MTL_OP_SYNC, SKY_MTL_OP_CLEAR_TEXTURE, SKY_MTL_OP_REPORT_COUNTER, SKY_MTL_OP_RESET_COUNTER, SKY_MTL_OP_REPORT_VALUE, SKY_MTL_OP_STATE, SKY_MTL_OP_TRANSFER, SKY_MTL_OP_UPDATE_BUFFER, SKY_MTL_OP_BARRIER };
typedef struct sky_mtl_operation {
    struct sky_mtl_operation *next;
    uint32_t kind, targets_bound;
    sky_mtl_clear_pass_t targets;
    sky_mtl_render_state_t state;
    uint32_t primitive, first, count, instances, base_instance, index_type;
    int32_t base_vertex;
    uint64_t index_address;
    uint32_t groups[3];
    int (*sync_action)(void *context);
    void *sync_context;
    uint32_t sync_context_size;
    int32_t region[6];
    double clear_value[4];
    uint32_t clear_mask, clear_value_type;
    uint64_t destination_address;
    uint32_t counter_type, report_value;
    uint32_t barriers;
    sky_mtl_transfer_t transfer;
    void *payload;
    uint64_t payload_size;
} sky_mtl_operation_t;
int sky_mtl_command_queue_submit_operations(sky_mtl_command_queue_t *queue,
    const sky_mtl_operation_t *operations, sky_mtl_completion_fn completion, void *context);
int sky_mtl_command_queue_present_texture(sky_mtl_command_queue_t *queue,
    sky_mtl_texture_t *texture,void *layer,int present_interval,const int32_t crop[4],sky_mtl_completion_fn completion,void *context);

typedef struct { uint64_t draws, dispatches, command_errors, sync_actions; } sky_mtl_graphics_stats_t;
sky_mtl_graphics_stats_t sky_mtl_graphics_stats(void);

void sky_mtl_render_state_merge(sky_mtl_render_state_t *destination,const sky_mtl_render_state_t *source);
void sky_nvn_graphics_resources_resolve(sky_mtl_render_state_t *state);
/* Borrowed native Metal objects remain valid while their wrapper/pool lives. */
void *sky_mtl_texture_native_handle(const sky_mtl_texture_t *texture);
void *sky_mtl_buffer_native_handle(uint64_t address,uint64_t size,uint64_t *offset);

int sky_mtl_command_queue_get_error(const sky_mtl_command_queue_t *queue);
sky_mtl_texture_t *sky_mtl_texture_wrap_native(void *native_texture);
