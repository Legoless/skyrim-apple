#pragma once

#include <pthread.h>
#include <stdint.h>

typedef struct { uint32_t flags; } sky_nvn_device_builder_t;
typedef struct {
    uint32_t magic, flags;
    const void *debug_label;
    int intermediate_shader_cache, origin_mode, depth_mode;
    uint8_t automatic_checkpoints, checkpoint_call_stacks;
    uint64_t automatic_checkpoints_mask;
    int automatic_checkpoints_interval, automatic_checkpoints_by_debug_group, checkpoint_max_call_stacks_depth;
} sky_nvn_device_t;

typedef struct {
    uint32_t magic, fenced;
    void *dev, *queue;
    uint64_t fence;
} sky_nvn_sync_t;

typedef struct {
    void *dev, *mem;
    uint64_t size;
    uint32_t flags;
} sky_nvn_memory_pool_builder_t;
typedef struct {
    uint32_t magic, flags;
    void *dev, *mem;
    uint64_t size;
    void *metal;
    const char *debug_label;
    uint64_t buffer_address;
    uint64_t flush_count, invalidate_count;
    int64_t last_range_offset;
    uint64_t last_range_size;
} sky_nvn_memory_pool_t;
enum {
    SKY_NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS = 0x1,
    SKY_NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS = 0x8,
    SKY_NVN_MEMORY_POOL_FLAGS_VIRTUAL = 0x200,
};
typedef struct {
    void *dev;
    const void *pool;
    int64_t offset;
    uint64_t size;
} sky_nvn_buffer_builder_t;
typedef struct {
    uint32_t magic;
    void *dev;
    const void *pool;
    int64_t offset;
    uint64_t size;
    void *mem;
} sky_nvn_buffer_t;
typedef struct { uint64_t address, size; } sky_nvn_buffer_range_t;
typedef struct {
    uint64_t data;
    const void *control;
} sky_nvn_shader_data_t;
typedef struct {
    uint32_t magic;
    void *dev;
    const char *debug_label;
    uint32_t stage_count, stage_mask;
    uint64_t serial;
} sky_nvn_program_t;

typedef struct {
    uint32_t magic;
    const void *pool;
    int64_t offset;
    int32_t count;
} sky_nvn_texture_pool_t;
typedef struct {
    uint32_t magic;
    const void *pool;
    int64_t offset;
    int32_t count;
} sky_nvn_sampler_pool_t;
typedef struct {
    void *dev;
    int min_filter, mag_filter;
    int wrap[3];
    int compare_mode, compare_func;
    float lod_min, lod_max, max_anisotropy, lod_bias;
    int reduction_filter;
} sky_nvn_sampler_builder_t;
typedef struct {
    uint32_t magic;
    void *dev;
    void *metal;
} sky_nvn_sampler_t;

typedef struct {
    void *dev;
    int target, format, flags, samples;
    uint32_t width, height, depth, levels;
    int64_t stride;
    int pkg_block_h;
    const void *pool;
    int64_t offset;
    uint8_t has_layout, layout[8];
    const void *packaged_data;
    int depth_stencil_mode;
    uint8_t swizzle[4];
} sky_nvn_texture_builder_t;
typedef struct {
    uint32_t magic;
    void *dev;
    int target, format, flags, samples;
    uint32_t width, height, depth, levels;
    int64_t stride;
    int pkg_block_h;
    const void *pool;
    int64_t offset;
    uint64_t storage_size, storage_address;
    uint32_t storage_alignment, layers;
    void *metal;
    uint8_t has_layout, layout[8];
    uint8_t depth_stencil_mode, swizzle[4];
} sky_nvn_texture_t;
typedef struct { uint32_t magic; uint64_t state; } sky_nvn_texture_view_t;
typedef struct { int xoffset, yoffset, zoffset, width, height, depth; } sky_nvn_copy_region_t;

typedef struct {
    void *dev;
    uint32_t flags;
    uint64_t command_size, compute_size, control_size, flush_threshold;
    void *memory;
    uint64_t memory_size;
} sky_nvn_queue_builder_t;
typedef struct {
    uint32_t magic;
    void *dev;
    uint32_t flags, error;
    uint64_t submits, completed;
    uint64_t command_size, compute_size, control_size, flush_threshold;
    void *memory;
    uint64_t memory_size;
    void *metal, *metal_memory;
    pthread_mutex_t lock;
    pthread_cond_t cv;
} sky_nvn_queue_t;
typedef struct {
    uint32_t magic, recording;
    void *dev, *state;
} sky_nvn_command_buffer_t;
typedef void (*sky_nvn_command_memory_callback_fn)(void *, int, uint64_t, void *);

typedef struct {
    void *dev, *native;
    int32_t texture_count, present_interval, active_texture_count;
    void *textures[4];
} sky_nvn_window_builder_t;
typedef struct {
    uint32_t magic;
    void *dev, *native;
    int32_t texture_count, present_interval, active_texture_count;
    void *textures[4];
    void *metal_textures[4];
    int32_t crop[4];
    int32_t next_texture;
    uint64_t presents;
    void *present_state;
} sky_nvn_window_t;
typedef struct { int x, y, width, height; } sky_nvn_rectangle_t;

void *sky_nvn_bootstrap_loader(const char *name);
void *sky_nvn_device_get_proc_address(const void *device, const char *name);
void sky_nvn_device_builder_set_defaults(void *builder);
void sky_nvn_device_builder_set_flags(void *builder, int flags);
int sky_nvn_device_builder_get_flags(const void *builder);
uint32_t sky_nvn_device_initialize(void *device, const void *builder);
void sky_nvn_device_finalize(void *device);
void sky_nvn_device_set_debug_label(void *device, const void *label);
void sky_nvn_device_set_intermediate_shader_cache(void *device, int value);
void sky_nvn_device_set_window_origin_mode(void *device, int mode);
void sky_nvn_device_set_depth_mode(void *device, int mode);
int sky_nvn_device_get_window_origin_mode(const void *device);
int sky_nvn_device_get_depth_mode(const void *device);
void sky_nvn_device_set_automatic_checkpoints_enable(void *device, uint8_t enabled);
void sky_nvn_device_set_automatic_checkpoints_mask(void *device, uint64_t mask);
void sky_nvn_device_set_automatic_checkpoints_sampling_interval(void *device, int interval);
void sky_nvn_device_set_automatic_checkpoints_by_debug_group(void *device, int group);
void sky_nvn_device_set_checkpoint_call_stacks_enable(void *device, uint8_t enabled);
void sky_nvn_device_set_checkpoint_max_call_stacks_depth(void *device, int depth);
void sky_nvn_device_get_integer(const void *device, int what, int *out);

uint32_t sky_nvn_sync_initialize(void *sync, void *device);
void sky_nvn_sync_finalize(void *sync);
int sky_nvn_sync_wait(const void *sync, uint64_t timeout_ns);

void sky_nvn_memory_pool_builder_set_defaults(void *builder);
void sky_nvn_memory_pool_builder_set_device(void *builder, void *device);
void sky_nvn_memory_pool_builder_set_storage(void *builder, void *memory, uint64_t size);
void sky_nvn_memory_pool_builder_set_flags(void *builder, int flags);
void *sky_nvn_memory_pool_builder_get_device(const void *builder);
void *sky_nvn_memory_pool_builder_get_memory(const void *builder);
uint64_t sky_nvn_memory_pool_builder_get_size(const void *builder);
int sky_nvn_memory_pool_builder_get_flags(const void *builder);
uint32_t sky_nvn_memory_pool_initialize(void *pool, const void *builder);
void sky_nvn_memory_pool_set_debug_label(void *pool, const char *label);
void sky_nvn_memory_pool_finalize(void *pool);
void *sky_nvn_memory_pool_map(const void *pool);
void sky_nvn_memory_pool_flush_mapped_range(void *pool, int64_t offset, uint64_t size);
void sky_nvn_memory_pool_invalidate_mapped_range(void *pool, int64_t offset, uint64_t size);
uint64_t sky_nvn_memory_pool_get_buffer_address(const void *pool);
uint64_t sky_nvn_memory_pool_get_size(const void *pool);
int sky_nvn_memory_pool_get_flags(const void *pool);
int sky_nvn_memory_pool_track(const sky_nvn_memory_pool_t *pool);
void sky_nvn_memory_pool_untrack(const void *pool);
void sky_nvn_buffer_builder_set_device(void *builder, void *device);
void sky_nvn_buffer_builder_set_defaults(void *builder);
void sky_nvn_buffer_builder_set_storage(void *builder, const void *pool, int64_t offset, uint64_t size);
uint64_t sky_nvn_buffer_builder_get_size(const void *builder);
uint32_t sky_nvn_buffer_initialize(void *buffer, const void *builder);
void sky_nvn_buffer_finalize(void *buffer);
void *sky_nvn_buffer_map(const void *buffer);
uint64_t sky_nvn_buffer_get_address(const void *buffer);
int sky_nvn_buffer_track(const sky_nvn_buffer_t *buffer);
void sky_nvn_buffer_untrack(const void *buffer);
uint32_t sky_nvn_program_initialize(void *program, void *device);
void sky_nvn_program_finalize(void *program);
void sky_nvn_program_set_debug_label(void *program, const char *label);
uint32_t sky_nvn_program_set_shaders(void *program, int count, const void *data);
uint32_t sky_nvn_texture_pool_initialize(void *texture_pool, const void *pool, int64_t offset, int count);
void sky_nvn_texture_pool_finalize(void *texture_pool);
void sky_nvn_texture_pool_register_texture(void *texture_pool, int id, const void *texture, const void *view);
void sky_nvn_texture_pool_register_image(void *texture_pool, int id, const void *texture, const void *view);
const void *sky_nvn_texture_pool_get_memory_pool(const void *texture_pool);
int64_t sky_nvn_texture_pool_get_memory_offset(const void *texture_pool);
int sky_nvn_texture_pool_get_size(const void *texture_pool);
uint32_t sky_nvn_sampler_pool_initialize(void *sampler_pool, const void *pool, int64_t offset, int count);
void sky_nvn_sampler_pool_finalize(void *sampler_pool);
void sky_nvn_sampler_pool_register_sampler(void *sampler_pool, int id, const void *sampler);
void sky_nvn_sampler_pool_register_sampler_builder(void *sampler_pool, int id, const void *builder);
const void *sky_nvn_sampler_pool_get_memory_pool(const void *sampler_pool);
int64_t sky_nvn_sampler_pool_get_memory_offset(const void *sampler_pool);
int sky_nvn_sampler_pool_get_size(const void *sampler_pool);
void sky_nvn_sampler_builder_set_device(void *builder, void *device);
void sky_nvn_sampler_builder_set_defaults(void *builder);
void sky_nvn_sampler_builder_set_min_mag_filter(void *builder, int min_filter, int mag_filter);
void sky_nvn_sampler_builder_set_wrap_mode(void *builder, int s, int t, int r);
void sky_nvn_sampler_builder_set_lod_clamp(void *builder, float minimum, float maximum);
void sky_nvn_sampler_builder_set_compare(void *builder, int mode, int function);
void sky_nvn_sampler_builder_set_max_anisotropy(void *builder, float value);
void sky_nvn_sampler_builder_set_lod_bias(void *builder, float value);
void sky_nvn_sampler_builder_set_reduction_filter(void *builder, int filter);
uint32_t sky_nvn_sampler_initialize(void *sampler, const void *builder);
void sky_nvn_sampler_finalize(void *sampler);
void sky_nvn_channel_mask_state_set_defaults(void *state);
void sky_nvn_channel_mask_state_set_channel_mask(void *state, int index,
    uint32_t r, uint32_t g, uint32_t b, uint32_t a);
void sky_nvn_blend_state_set_defaults(void *state);
void sky_nvn_blend_state_set_blend_target(void *state, int target);
void sky_nvn_blend_state_set_blend_equation(void *state, int rgb, int alpha);
void sky_nvn_blend_state_set_blend_func(void *state, int src_rgb, int dst_rgb, int src_alpha, int dst_alpha);
void sky_nvn_color_state_set_defaults(void *state);
void sky_nvn_color_state_set_blend_enable(void *state, int index, uint32_t enable);
void sky_nvn_color_state_set_alpha_test(void *state, int function);
void sky_nvn_polygon_state_set_defaults(void *state);
void sky_nvn_polygon_state_set_front_face(void *state, int face);
void sky_nvn_polygon_state_set_polygon_mode(void *state, int mode);
void sky_nvn_polygon_state_set_cull_face(void *state, int face);
void sky_nvn_polygon_state_set_polygon_offset_enables(void *state, int enables);
void sky_nvn_depth_stencil_state_set_defaults(void *state);
void sky_nvn_depth_stencil_state_set_depth_test_enable(void *state, uint32_t enable);
void sky_nvn_depth_stencil_state_set_stencil_test_enable(void *state, uint32_t enable);
void sky_nvn_depth_stencil_state_set_stencil_func(void *state, int face, int function);
void sky_nvn_depth_stencil_state_set_depth_write_enable(void *state, uint32_t enable);
void sky_nvn_depth_stencil_state_set_depth_func(void *state, int function);
void sky_nvn_depth_stencil_state_set_stencil_op(void *state, int face, int sfail, int dpfail, int dppass);
void sky_nvn_vertex_attrib_state_set_defaults(void *state);
void sky_nvn_vertex_attrib_state_set_format(void *state, int format, int64_t offset);
void sky_nvn_vertex_attrib_state_set_stream_index(void *state, int index);
void sky_nvn_vertex_stream_state_set_defaults(void *state);
void sky_nvn_vertex_stream_state_set_stride(void *state, int64_t stride);
void sky_nvn_vertex_stream_state_set_divisor(void *state, int divisor);
void sky_nvn_command_buffer_bind_uniform_buffer(void *command_buffer, int stage, int index,
    uint64_t address, uint64_t size);
void sky_nvn_command_buffer_bind_program(void *command_buffer, const void *program, int stages);
void sky_nvn_texture_builder_set_defaults(void *builder);
void sky_nvn_texture_builder_set_device(void *builder, void *device);
void sky_nvn_texture_builder_set_flags(void *builder, int flags);
void sky_nvn_texture_builder_set_target(void *builder, int target);
void sky_nvn_texture_builder_set_width(void *builder, int width);
void sky_nvn_texture_builder_set_height(void *builder, int height);
void sky_nvn_texture_builder_set_depth(void *builder, int depth);
void sky_nvn_texture_builder_set_size_1d(void *builder, int width);
void sky_nvn_texture_builder_set_size_2d(void *builder, int width, int height);
void sky_nvn_texture_builder_set_size_3d(void *builder, int width, int height, int depth);
void sky_nvn_texture_builder_set_levels(void *builder, int levels);
void sky_nvn_texture_builder_set_format(void *builder, int format);
void sky_nvn_texture_builder_set_samples(void *builder, int samples);
void sky_nvn_texture_builder_set_swizzle(void *builder, int r, int g, int b, int a);
void sky_nvn_texture_builder_set_depth_stencil_mode(void *builder, int mode);
void sky_nvn_texture_builder_set_stride(void *builder, int64_t stride);
void sky_nvn_texture_builder_set_storage(void *builder, const void *pool, int64_t offset);
void sky_nvn_texture_builder_set_packaged_texture_data(void *builder, const void *data);
void sky_nvn_texture_builder_set_packaged_texture_layout(void *builder, const void *layout);
void *sky_nvn_texture_builder_get_device(const void *builder);
int sky_nvn_texture_builder_get_flags(const void *builder);
int sky_nvn_texture_builder_get_target(const void *builder);
int sky_nvn_texture_builder_get_width(const void *builder);
int sky_nvn_texture_builder_get_height(const void *builder);
int sky_nvn_texture_builder_get_depth(const void *builder);
int sky_nvn_texture_builder_get_levels(const void *builder);
int sky_nvn_texture_builder_get_format(const void *builder);
int sky_nvn_texture_builder_get_samples(const void *builder);
void sky_nvn_texture_builder_get_swizzle(const void *builder, int *r, int *g, int *b, int *a);
int sky_nvn_texture_builder_get_depth_stencil_mode(const void *builder);
int64_t sky_nvn_texture_builder_get_stride(const void *builder);
const void *sky_nvn_texture_builder_get_memory_pool(const void *builder);
int64_t sky_nvn_texture_builder_get_memory_offset(const void *builder);
uint64_t sky_nvn_texture_builder_get_storage_size(const void *builder);
uint64_t sky_nvn_texture_builder_get_storage_alignment(const void *builder);
uint32_t sky_nvn_texture_initialize(void *texture, const void *builder);
void sky_nvn_texture_finalize(void *texture);
void sky_nvn_texture_write_texels(const void *texture, const void *view,
    const sky_nvn_copy_region_t *region, const void *data);
void sky_nvn_texture_set_debug_label(void *texture, const char *label);
int sky_nvn_texture_get_flags(const void *texture);
int sky_nvn_texture_get_target(const void *texture);
int sky_nvn_texture_get_width(const void *texture);
int sky_nvn_texture_get_height(const void *texture);
int sky_nvn_texture_get_depth(const void *texture);
int sky_nvn_texture_get_levels(const void *texture);
int sky_nvn_texture_get_format(const void *texture);
int sky_nvn_texture_get_samples(const void *texture);
void sky_nvn_texture_get_swizzle(const void *texture, int *r, int *g, int *b, int *a);
int sky_nvn_texture_get_depth_stencil_mode(const void *texture);
int64_t sky_nvn_texture_get_stride(const void *texture);
const void *sky_nvn_texture_get_memory_pool(const void *texture);
int64_t sky_nvn_texture_get_memory_offset(const void *texture);
int sky_nvn_texture_get_storage_size(const void *texture);
uint64_t sky_nvn_texture_get_texture_address(const void *texture);
void sky_nvn_texture_view_set_defaults(void *view);
void sky_nvn_texture_view_set_levels(void *view, int base, int count);
void sky_nvn_texture_view_set_layers(void *view, int minimum, int count);
void sky_nvn_texture_view_set_format(void *view, int format);
void sky_nvn_texture_view_set_swizzle(void *view, int r, int g, int b, int a);
void sky_nvn_texture_view_set_depth_stencil_mode(void *view, int mode);
void sky_nvn_texture_view_set_target(void *view, int target);
uint32_t sky_nvn_texture_view_get_levels(const void *view, int *base, int *count);
uint32_t sky_nvn_texture_view_get_layers(const void *view, int *minimum, int *count);
uint32_t sky_nvn_texture_view_get_format(const void *view, int *format);
uint32_t sky_nvn_texture_view_get_swizzle(const void *view, int *r, int *g, int *b, int *a);
uint32_t sky_nvn_texture_view_get_depth_stencil_mode(const void *view, int *mode);
uint32_t sky_nvn_texture_view_get_target(const void *view, int *target);
uint32_t sky_nvn_texture_view_compare(const void *view, const void *other);
int64_t sky_nvn_texture_get_view_offset(const void *texture, const void *view);
const void *sky_nvn_texture_pool_get_registered_texture(const void *texture_pool, int id);
const void *sky_nvn_texture_pool_get_registered_metal_view(const void *texture_pool, int id);

void sky_nvn_queue_builder_set_defaults(void *builder);
void sky_nvn_queue_builder_set_device(void *builder, void *device);
void sky_nvn_queue_builder_set_flags(void *builder, int flags);
void sky_nvn_queue_builder_set_command_memory_size(void *builder, uint64_t size);
void sky_nvn_queue_builder_set_compute_memory_size(void *builder, uint64_t size);
void sky_nvn_queue_builder_set_control_memory_size(void *builder, uint64_t size);
void sky_nvn_queue_builder_set_command_flush_threshold(void *builder, uint64_t size);
uint64_t sky_nvn_queue_builder_get_queue_memory_size(const void *builder);
void sky_nvn_queue_builder_set_queue_memory(void *builder, void *memory, uint64_t size);
uint32_t sky_nvn_queue_initialize(void *queue, const void *builder);
void sky_nvn_queue_finalize(void *queue);
void sky_nvn_queue_fence_sync(void *queue, void *sync, int condition, int flags);
void sky_nvn_queue_flush(void *queue);
void sky_nvn_queue_finish(void *queue);
uint32_t sky_nvn_command_buffer_initialize(void *command_buffer, void *device);
void sky_nvn_command_buffer_finalize(void *command_buffer);
void sky_nvn_command_buffer_set_memory_callback(void *command_buffer, sky_nvn_command_memory_callback_fn callback);
void sky_nvn_command_buffer_set_memory_callback_data(void *command_buffer, void *data);
void sky_nvn_command_buffer_add_command_memory(void *command_buffer, const void *pool, int64_t offset, uint64_t size);
void sky_nvn_command_buffer_add_control_memory(void *command_buffer, void *memory, uint64_t size);
uint64_t sky_nvn_command_buffer_get_command_memory_free(const void *command_buffer);
uint64_t sky_nvn_command_buffer_get_control_memory_free(const void *command_buffer);
void sky_nvn_command_buffer_begin_recording(void *command_buffer);
uint64_t sky_nvn_command_buffer_end_recording(void *command_buffer);
uint32_t sky_nvn_command_buffer_is_recording(const void *command_buffer);
void sky_nvn_command_buffer_set_render_targets(void *command_buffer, int count, void *const *textures,
    void *const *views, const void *depth_texture, const void *depth_view);
void sky_nvn_command_buffer_clear_color(void *command_buffer, int index, const float *rgba, int mask);
void sky_nvn_command_buffer_clear_depth_stencil(void *command_buffer, float depth,
    uint32_t depth_mask, int stencil, uint32_t stencil_mask);
void sky_nvn_command_buffer_set_viewports(void *command_buffer, int first, int count, const float *values);
void sky_nvn_command_buffer_set_scissor(void *command_buffer, int x, int y, int width, int height);
void sky_nvn_command_buffer_set_depth_range(void *command_buffer, float near_value, float far_value);
void sky_nvn_queue_submit_commands(void *queue, int count, const uint64_t *handles);
uint32_t sky_nvn_queue_submit_empty_for_testing(void *queue, uint64_t delay_ns);

void sky_nvn_window_builder_set_defaults(void *builder);
void sky_nvn_window_builder_set_device(void *builder, void *device);
void sky_nvn_window_builder_set_native_window(void *builder, void *native);
void sky_nvn_window_builder_set_textures(void *builder, int count, void *const *textures);
void sky_nvn_window_builder_set_present_interval(void *builder, int interval);
void sky_nvn_window_builder_set_num_active_textures(void *builder, int count);
void *sky_nvn_window_builder_get_device(const void *builder);
int sky_nvn_window_builder_get_num_textures(const void *builder);
void *sky_nvn_window_builder_get_texture(const void *builder, int index);
void *sky_nvn_window_builder_get_native_window(const void *builder);
int sky_nvn_window_builder_get_present_interval(const void *builder);
int sky_nvn_window_builder_get_num_active_textures(const void *builder);
uint32_t sky_nvn_window_initialize(void *window, const void *builder);
void sky_nvn_window_finalize(void *window);
void sky_nvn_window_set_present_interval(void *window, int interval);
void sky_nvn_window_set_num_active_textures(void *window, int count);
void sky_nvn_window_set_crop(void *window, int x, int y, int width, int height);
void sky_nvn_window_get_crop(const void *window, sky_nvn_rectangle_t *crop);
void *sky_nvn_window_get_native_window(const void *window);
int sky_nvn_window_get_num_textures(const void *window);
int sky_nvn_window_get_num_active_textures(const void *window);
int sky_nvn_window_get_present_interval(const void *window);
/* Test observer; not exposed through the NVN bootstrap table. */
uint32_t sky_nvn_window_present_waiters_for_testing(const void *window);
uint64_t sky_nvn_window_last_present_ns_for_testing(const void *window);
void sky_nvn_unimplemented(void);

void sky_nvn_command_buffer_bind_storage_buffer(void *command_buffer,int stage,int index,uint64_t address,uint64_t size);
void sky_nvn_command_buffer_bind_vertex_buffer(void *command_buffer,int index,uint64_t address,uint64_t size);
void sky_nvn_command_buffer_set_texture_pool(void *command_buffer,const void *pool);
void sky_nvn_command_buffer_set_sampler_pool(void *command_buffer,const void *pool);
void sky_nvn_command_buffer_bind_texture(void *command_buffer,int stage,int index,uint64_t handle);
void sky_nvn_command_buffer_bind_separate_texture(void *command_buffer,int stage,int index,uint64_t handle);
void sky_nvn_command_buffer_bind_separate_sampler(void *command_buffer,int stage,int index,uint64_t handle);
void sky_nvn_command_buffer_bind_vertex_attrib_state(void *command_buffer,int count,const void *attributes);
void sky_nvn_command_buffer_bind_vertex_stream_state(void *command_buffer,int count,const void *streams);
void sky_nvn_command_buffer_bind_blend_state(void *command_buffer,const void *state);
void sky_nvn_command_buffer_bind_channel_mask_state(void *command_buffer,const void *state);
void sky_nvn_command_buffer_bind_color_state(void *command_buffer,const void *state);
void sky_nvn_command_buffer_bind_polygon_state(void *command_buffer,const void *state);
void sky_nvn_command_buffer_bind_depth_stencil_state(void *command_buffer,const void *state);
void sky_nvn_command_buffer_set_blend_color(void *command_buffer,const float *rgba);
void sky_nvn_command_buffer_set_stencil_value_mask(void *command_buffer,int face,int mask);
void sky_nvn_command_buffer_set_stencil_mask(void *command_buffer,int face,int mask);
void sky_nvn_command_buffer_set_stencil_ref(void *command_buffer,int face,int reference);
void sky_nvn_command_buffer_draw_arrays_instanced(void *command_buffer,int primitive,int first,int count,int base_instance,int instances);
void sky_nvn_command_buffer_draw_arrays(void *command_buffer,int primitive,int first,int count);
void sky_nvn_command_buffer_draw_elements_instanced(void *command_buffer,int primitive,int index_type,int count,uint64_t address,int base_vertex,int base_instance,int instances);
void sky_nvn_command_buffer_draw_elements_base_vertex(void *command_buffer,int primitive,int type,int count,uint64_t address,int base);
void sky_nvn_command_buffer_draw_elements(void *command_buffer,int primitive,int type,int count,uint64_t address);
void sky_nvn_command_buffer_dispatch_compute(void *command_buffer,int x,int y,int z);

uint64_t sky_nvn_device_get_texture_handle(const void *device,int texture,int sampler);
uint64_t sky_nvn_device_get_separate_texture_handle(const void *device,int texture);
uint64_t sky_nvn_device_get_separate_sampler_handle(const void *device,int sampler);
uint64_t sky_nvn_device_get_image_handle(const void *device,int image);
int sky_nvn_queue_acquire_texture(void *queue,void *window,int *index);
int sky_nvn_window_acquire_texture(void *window,void *sync,int *index);
void sky_nvn_queue_present_texture(void *queue,void *window,int index);

void sky_nvn_command_buffer_bind_image(void *command_buffer,int stage,int index,uint64_t handle);
void sky_nvn_command_buffer_bind_images(void *command_buffer,int stage,int first,int count,const uint64_t *handles);
void sky_nvn_command_buffer_bind_textures(void *command_buffer,int stage,int first,int count,const uint64_t *handles);
void sky_nvn_command_buffer_bind_separate_textures(void *command_buffer,int stage,int first,int count,const uint64_t *handles);
void sky_nvn_command_buffer_bind_separate_samplers(void *command_buffer,int stage,int first,int count,const uint64_t *handles);
void sky_nvn_command_buffer_bind_uniform_buffers(void *command_buffer,int stage,int first,int count,const sky_nvn_buffer_range_t *ranges);
void sky_nvn_command_buffer_bind_storage_buffers(void *command_buffer,int stage,int first,int count,const sky_nvn_buffer_range_t *ranges);
void sky_nvn_command_buffer_bind_vertex_buffers(void *command_buffer,int first,int count,const sky_nvn_buffer_range_t *ranges);
void sky_nvn_command_buffer_set_viewport(void *command_buffer,int x,int y,int width,int height);
void sky_nvn_command_buffer_set_scissors(void *command_buffer,int first,int count,const int *values);
void sky_nvn_command_buffer_set_depth_ranges(void *command_buffer,int first,int count,const float *values);
void sky_nvn_command_buffer_barrier(void *command_buffer,int barriers);

void sky_nvn_command_buffer_wait_sync(void *command_buffer,const void *sync);
void sky_nvn_command_buffer_fence_sync(void *command_buffer,void *sync,int condition,int flags);

void sky_nvn_command_buffer_clear_texture(void *command_buffer,const void *texture,const void *view,const sky_nvn_copy_region_t *region,const float *values,int mask);
void sky_nvn_command_buffer_clear_texture_i(void *command_buffer,const void *texture,const void *view,const sky_nvn_copy_region_t *region,const int32_t *values,int mask);
void sky_nvn_command_buffer_clear_texture_ui(void *command_buffer,const void *texture,const void *view,const sky_nvn_copy_region_t *region,const uint32_t *values,int mask);

void sky_nvn_command_buffer_call_commands(void *command_buffer,int count,const uint64_t *handles);
void sky_nvn_command_buffer_copy_commands(void *command_buffer,int count,const uint64_t *handles);

void sky_nvn_command_buffer_report_counter(void *command_buffer,int type,uint64_t address);
void sky_nvn_command_buffer_reset_counter(void *command_buffer,int type);
void sky_nvn_command_buffer_report_value(void *command_buffer,uint32_t value,uint64_t address);
uint64_t sky_nvn_device_get_timestamp_in_nanoseconds(const void *device,const uint64_t *counter_data);
void sky_nvn_device_finalize_command_handle(void *device,uint64_t handle);

void sky_nvn_command_buffer_set_shader_scratch_memory(void *command_buffer,const void *pool,int64_t offset,uint64_t size);

void sky_nvn_command_buffer_set_depth_bounds(void *command_buffer,uint32_t enabled,float near_value,float far_value);

int sky_nvn_queue_get_error(void *queue,void *error_info);

void sky_nvn_command_buffer_set_copy_row_stride(void *command_buffer,int64_t stride);
void sky_nvn_command_buffer_set_copy_image_stride(void *command_buffer,int64_t stride);
void sky_nvn_command_buffer_copy_buffer_to_texture(void *command_buffer,uint64_t source,const void *destination,const void *view,const sky_nvn_copy_region_t *region,int flags);
void sky_nvn_command_buffer_copy_texture_to_buffer(void *command_buffer,const void *source,const void *view,const sky_nvn_copy_region_t *region,uint64_t destination,int flags);
void sky_nvn_command_buffer_copy_texture_to_texture(void *command_buffer,const void *source,const void *source_view,const sky_nvn_copy_region_t *source_region,const void *destination,const void *destination_view,const sky_nvn_copy_region_t *destination_region,int flags);

void sky_nvn_command_buffer_update_uniform_buffer(void *command_buffer,uint64_t address,uint64_t buffer_size,int64_t offset,uint64_t size,const void *data);

void sky_nvn_command_buffer_set_polygon_offset_clamp(void *command_buffer,float factor,float units,float clamp);
