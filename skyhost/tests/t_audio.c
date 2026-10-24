#include "audio.h"
#include "skyhost.h"
#include <assert.h>
#include <mach-o/getsect.h>
#include <sched.h>

extern const struct mach_header_64 _mh_execute_header;

static void *force_function(const char *name) {
    typedef struct { const char *name, *why; void *function; } force_entry_t;
    unsigned long size = 0;
    const force_entry_t *entries = (const force_entry_t *)getsectiondata(
        &_mh_execute_header, "__DATA", "swforce", &size);
    assert(entries && size % sizeof *entries == 0);
    for (unsigned long index = 0; index < size / sizeof *entries; index++)
        if (!strcmp(entries[index].name, name)) return entries[index].function;
    assert(!"missing forced audio entry point");
    return NULL;
}

static int64_t test_config_size(const void *parameter) {
    assert(parameter);
    assert(*(const uint32_t *)parameter == 48000);
    return 0x5000;
}

static void *g_original_event;
static uint32_t test_original_renderer_update(sky_audio_renderer_handle_t handle, const void *config) {
    assert(handle.renderer && handle.event == handle.renderer->event && config);
    return 0;
}
static int test_original_try_wait(void *guest) {
    assert(guest == g_original_event);
    return 1;
}

void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE"))
        return test_original_renderer_update;
    if (!strcmp(name, "_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE"))
        return test_original_try_wait;
    assert(!strcmp(name, "_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE"));
    return test_config_size;
}

typedef struct { void *event; uint32_t ready, result; } event_wait_job_t;

static void *wait_for_event(void *argument) {
    event_wait_job_t *job = argument;
    __atomic_store_n(&job->ready, 1, __ATOMIC_SEQ_CST);
    job->result = sky_event_wait(job->event, 1000000000);
    return NULL;
}

static void test_host_events(void) {
    uint8_t manual[48] = {0}, automatic[48] = {0}, unregistered[48] = {0};
    assert(sky_event_create(manual, 0) == 0);
    assert(sky_event_wait(manual, 0) == 0xEA01);
    assert(sky_event_signal(manual) == 0);
    assert(sky_event_wait(manual, 0) == 0);
    assert(sky_event_wait(manual, 0) == 0);
    assert(sky_event_clear(manual) == 0);
    assert(sky_event_wait(manual, 1000000) == 0xEA01);
    assert(sky_event_create(automatic, 1) == 0);
    event_wait_job_t job = {automatic, 0, UINT32_MAX};
    pthread_t waiter;
    assert(!pthread_create(&waiter, NULL, wait_for_event, &job));
    uint64_t deadline = now_ns() + 1000000000;
    while (!__atomic_load_n(&job.ready, __ATOMIC_SEQ_CST) && now_ns() < deadline) sched_yield();
    assert(job.ready);
    assert(sky_event_signal(automatic) == 0);
    assert(!pthread_join(waiter, NULL));
    assert(job.result == 0);
    assert(sky_event_wait(automatic, 0) == 0xEA01);
    assert(sky_event_set_periodic(automatic, 5000000) == 0);
    assert(sky_event_wait(automatic, 100000000) == 0);
    assert(sky_event_set_periodic(automatic, 0) == 0);
    assert(sky_event_wait(automatic, 1000000) == 0xEA01);

    typedef int (*try_wait_fn)(void *);
    try_wait_fn try_wait = (try_wait_fn)force_function("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    assert(!try_wait(automatic));
    assert(sky_event_signal(automatic) == 0);
    assert(try_wait(automatic) && !try_wait(automatic));
    g_original_event = unregistered;
    assert(try_wait(unregistered));
    assert(sky_event_destroy(manual) == 0);
    assert(sky_event_destroy(automatic) == 0);
    assert(sky_event_wait(automatic, 0) == 0xE401);
}

int main(void) {
    uint8_t parameter[0x40] = {0};
    *(uint32_t *)parameter = 48000;
    ((uint32_t *)parameter)[1] = 240;
    ((uint32_t *)parameter)[2] = 2;
    ((uint32_t *)parameter)[4] = 1;
    ((uint32_t *)parameter)[5] = 1;
    ((uint32_t *)parameter)[13] = 0x36564552;
    assert(sky_audio_get_work_buffer_size(parameter) == 0x5000);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_audio_work_buffer_size"));
    assert(sky_audio_get_work_buffer_size(NULL) == 0);

    sky_audio_renderer_t *renderer = NULL;
    uint8_t work_buffer[0x5000];
    uint8_t event[32], original[32];
    memset(event, 0xa5, sizeof event);
    memcpy(original, event, sizeof event);
    assert(sky_audio_open_renderer(&renderer, event, parameter, work_buffer) == 0);
    assert(renderer);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_audio_open"));
    assert(!memcmp(event, original, sizeof event));
    sky_audio_flush_data_cache(work_buffer, sizeof work_buffer);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_audio_cache_flush"));
    const uint8_t config[16] = {0};
    assert(sky_audio_request_update_renderer(renderer, config) == 0);
    assert(renderer->config == config && renderer->update_sequence == 1);
    assert(sky_audio_start_renderer(renderer) == 0 && renderer->started == 1);
    assert(sky_event_wait(event, 100000000) == 0);
    uint64_t start = now_ns();
    assert(sky_event_wait(event, 100000000) == 0);
    assert(now_ns() - start >= 1000000);
    assert(sky_audio_stop_renderer(renderer) == 0 && renderer->started == 0);
    assert(sky_event_wait(event, 1000000) == 0xEA01);
    sky_audio_close_renderer(renderer);
    assert(sky_event_destroy(event) == 0);

    /* Exercise the actual guest ABI. The handle fills x0/x1, so config is x2. */
    typedef uint32_t (*open_fn)(sky_audio_renderer_handle_t *, void *, const void *, void *, size_t);
    typedef uint32_t (*update_fn)(sky_audio_renderer_handle_t, const void *);
    open_fn open_entry = (open_fn)force_function("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    update_fn update_entry = (update_fn)force_function("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE");
    struct { sky_audio_renderer_handle_t handle; uint64_t canary; } guest = {{0}, 0x123456789abcdef0};
    assert(open_entry(&guest.handle, event, parameter, work_buffer, sizeof work_buffer) == 0);
    assert(guest.handle.renderer && guest.handle.event == event);
    assert(guest.canary == 0x123456789abcdef0);
    assert(update_entry(guest.handle, config) == 0);
    assert(guest.handle.renderer->config == config);
    assert(guest.handle.renderer->update_sequence == 1);
    sky_audio_close_renderer(guest.handle.renderer);
    assert(sky_event_destroy(event) == 0);
    test_host_events();
    puts("T_AUDIO OK config-size host-open-no-hipc coherent-cache update-start-stop-close-event-destroy");
    return 0;
}
