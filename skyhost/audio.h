#pragma once

#include "event.h"
#include "audio_backend.h"
#include <stddef.h>
#include <stdint.h>

typedef struct sky_audio_renderer_t {
    void **service_vtable;
    void *event;
    const void *parameter;
    void *work_buffer;
    const void *config;
    uint64_t update_sequence;
    int started;
    sky_audio_backend_t *backend;
} sky_audio_renderer_t;

/* The guest passes this 16-byte value in two AArch64 argument registers. */
typedef struct {
    sky_audio_renderer_t *renderer;
    void *event;
} sky_audio_renderer_handle_t;

int64_t sky_audio_get_work_buffer_size(const void *parameter);
uint32_t sky_audio_open_renderer(sky_audio_renderer_t **out, void *event, const void *parameter, void *work_buffer);
uint32_t sky_audio_request_update_renderer(sky_audio_renderer_t *renderer, const void *config);
uint32_t sky_audio_start_renderer(sky_audio_renderer_t *renderer);
uint32_t sky_audio_stop_renderer(sky_audio_renderer_t *renderer);
void sky_audio_close_renderer(sky_audio_renderer_t *renderer);
void sky_audio_flush_data_cache(const void *data, size_t size);
