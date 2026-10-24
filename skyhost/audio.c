#include "audio.h"
#include "event.h"
#include "skyhost.h"
#include <stdatomic.h>

#define RES_OK 0u
#define RES_FAIL 0x2A2u

typedef struct { void *address; size_t size; } audio_span_t;
static uint32_t audio_service_update(sky_audio_renderer_t *renderer, audio_span_t *output,
    audio_span_t *performance, const audio_span_t *input) {
    if (!renderer || !renderer->backend || !output || !performance || !input) return RES_FAIL;
    return sky_audio_backend_update(renderer->backend, output->address, output->size,
        performance->address, performance->size, input->address, input->size);
}

static __attribute__((noreturn)) void audio_service_unimplemented(void) {
    LOG("RUN_STOP checkpoint=%s reason=UNIMPLEMENTED_AUDIO_RENDERER_SERVICE_METHOD", sky_checkpoint_name());
    abort();
}

static void *g_audio_service_vtable[16];
static pthread_once_t g_audio_service_once = PTHREAD_ONCE_INIT;
static void audio_service_initialize(void) {
    for (int index = 0; index < 16; index++) g_audio_service_vtable[index] = audio_service_unimplemented;
    /* Verified SDK RequestUpdateAudioRenderer dispatch: vtable + 0x40 receives
     * output span, performance span, input span. The original SDK retains its
     * config serialization and response/status updates around this boundary. */
    g_audio_service_vtable[8] = audio_service_update;
}

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)


int64_t sky_audio_get_work_buffer_size(const void *parameter) {
    if (!parameter) return 0;
    typedef int64_t (*config_size_fn)(const void *);
    config_size_fn config_size = (config_size_fn)guest_sym("_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE");
    if (!config_size) die("Skyrim config work-buffer-size function is missing");
    int64_t size = config_size(parameter);
    sky_checkpoint("post_nnMain_audio_work_buffer_size");
    LOG("AUDIO_WORK_BUFFER_SIZE value=%lld source=skyrim-config", (long long)size);
    return size;
}

uint32_t sky_audio_open_renderer(sky_audio_renderer_t **out, void *event, const void *parameter, void *work_buffer) {
    if (!out || !parameter || !work_buffer) return RES_FAIL;
    sky_audio_renderer_t *renderer = calloc(1, sizeof *renderer);
    if (!renderer) return RES_FAIL;
    if (sky_event_create(event, 1) != RES_OK) {
        free(renderer);
        return RES_FAIL;
    }
    renderer->event = event;
    pthread_once(&g_audio_service_once, audio_service_initialize);
    renderer->service_vtable = g_audio_service_vtable;
    renderer->parameter = parameter;
    renderer->work_buffer = work_buffer;
    renderer->backend = sky_audio_backend_create(parameter, event);
    if (!renderer->backend) {
        LOG("AUDIO_OPEN_ERROR reason=actual-renderer-backend-required");
        (void)sky_event_destroy(event);
        free(renderer);
        return RES_FAIL;
    }
    *out = renderer;
    sky_checkpoint("post_nnMain_audio_open");
    LOG("AUDIO_OPEN handle=%p mode=ryujinx-dsp-coreaudio no-hipc", (void *)renderer);
    return RES_OK;
}

SKY_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "Use Skyrim's pure config work-buffer calculation without creating a manager",
          int64_t, sky_audio_work_buffer_size_entry, const void *parameter) {
    return sky_audio_get_work_buffer_size(parameter);
}
SKY_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "Open a host-owned renderer without a HIP C manager",
          uint32_t, sky_audio_open_entry, sky_audio_renderer_handle_t *out, void *event,
          const void *parameter, void *work_buffer, size_t work_buffer_size) {
    if (!out || !work_buffer_size) return RES_FAIL;
    sky_audio_renderer_t *renderer = NULL;
    uint32_t result = sky_audio_open_renderer(&renderer, event, parameter, work_buffer);
    if (result == RES_OK) *out = (sky_audio_renderer_handle_t){renderer, event};
    return result;
}

void sky_audio_flush_data_cache(const void *data, size_t size) {
    (void)data;
    (void)size;
    sky_checkpoint("post_nnMain_audio_cache_flush");
    static _Atomic uint64_t flushes;
    uint64_t count = atomic_fetch_add(&flushes, 1) + 1;
    if (count <= 4 || !(count & (count - 1)))
        LOG("AUDIO_CACHE_FLUSH mode=host-coherent count=%llu", (unsigned long long)count);
}

SKY_FORCE("_ZN2nn2os14FlushDataCacheEPKvm", "Host memory is coherent without ctr_el0 or dc civac",
          void, sky_audio_cache_flush_entry, const void *data, size_t size) {
    sky_audio_flush_data_cache(data, size);
}

void sky_audio_close_renderer(sky_audio_renderer_t *renderer) {
    if (!renderer) return;
    if (renderer->backend) sky_audio_backend_destroy(renderer->backend);
    (void)sky_event_clear(renderer->event);
    LOG("AUDIO_CLOSE handle=%p", (void *)renderer);
    sky_checkpoint("post_nnMain_audio_close");
    free(renderer);
}

SKY_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "Close and free the host renderer",
          void, sky_audio_close_entry, sky_audio_renderer_t *renderer) {
    sky_audio_close_renderer(renderer);
}

uint32_t sky_audio_request_update_renderer(sky_audio_renderer_t *renderer, const void *config) {
    if (!renderer || !renderer->backend || !config) return RES_FAIL;
    renderer->config = config;
    renderer->update_sequence++;
    sky_checkpoint("post_nnMain_audio_update");
    if (renderer->update_sequence <= 4 || !(renderer->update_sequence & (renderer->update_sequence - 1)))
        LOG("AUDIO_UPDATE handle=%p sequence=%llu mode=ryujinx-dsp", (void *)renderer,
            (unsigned long long)renderer->update_sequence);
    typedef uint32_t (*update_fn)(sky_audio_renderer_handle_t, const void *);
    update_fn update = (update_fn)guest_sym("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE");
    if (!update) die("original audio config update function is missing");
    return update((sky_audio_renderer_handle_t){renderer, renderer->event}, config);
}

uint32_t sky_audio_start_renderer(sky_audio_renderer_t *renderer) {
    if (!renderer || !renderer->backend) return RES_FAIL;
    uint32_t result = sky_audio_backend_start(renderer->backend);
    if (result != RES_OK) return result;
    renderer->started = 1;
    sky_checkpoint("post_nnMain_audio_start");
    LOG("AUDIO_START handle=%p state=started mode=ryujinx-dsp-coreaudio", (void *)renderer);
    return RES_OK;
}

uint32_t sky_audio_stop_renderer(sky_audio_renderer_t *renderer) {
    if (!renderer || !renderer->backend) return RES_FAIL;
    uint32_t result = sky_audio_backend_stop(renderer->backend);
    if (result != RES_OK) return result;
    (void)sky_event_clear(renderer->event);
    renderer->started = 0;
    sky_checkpoint("post_nnMain_audio_stop");
    LOG("AUDIO_STOP handle=%p state=stopped", (void *)renderer);
    return RES_OK;
}

SKY_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "Use SDK config serialization with the native DSP renderer service",
          uint32_t, sky_audio_update_entry, sky_audio_renderer_handle_t handle, const void *config) {
    return sky_audio_request_update_renderer(handle.renderer, config);
}
SKY_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "Start the host renderer state machine",
          uint32_t, sky_audio_start_entry, sky_audio_renderer_t *renderer) {
    return sky_audio_start_renderer(renderer);
}
SKY_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "Stop the host renderer state machine",
          uint32_t, sky_audio_stop_entry, sky_audio_renderer_t *renderer) {
    return sky_audio_stop_renderer(renderer);
}
