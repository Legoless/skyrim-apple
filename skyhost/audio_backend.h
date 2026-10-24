#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct sky_audio_backend sky_audio_backend_t;
sky_audio_backend_t *sky_audio_backend_create(const void *parameter, void *event);
uint32_t sky_audio_backend_update(sky_audio_backend_t *, void *output, size_t output_size,
    void *performance, size_t performance_size, const void *input, size_t input_size);
uint32_t sky_audio_backend_start(sky_audio_backend_t *);
uint32_t sky_audio_backend_stop(sky_audio_backend_t *);
void sky_audio_backend_destroy(sky_audio_backend_t *);
/* iOS: activate after AVAudioSession, deactivate before background/interruption.
   Stops/resumes existing RemoteIO outputs; macOS keeps its normal sink policy. */
void sky_audio_backend_set_host_active(int active);
