#include "audio_backend.h"
#include "event.h"
#include "skyhost.h"

#include <AudioUnit/AudioUnit.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#endif
#include <stdatomic.h>

#define AUDIO_RING_FRAMES 16384u

typedef struct {
    void *(*create)(const void *, void (*)(void *, const int16_t *, uint32_t, uint32_t),
                    void (*)(void *, int), void *);
    uint32_t (*update)(void *, void *, size_t, void *, size_t, const void *, size_t);
    uint32_t (*start)(void *);
    uint32_t (*stop)(void *);
    void (*destroy)(void *);
} renderer_api_t;

static renderer_api_t g_api;
static pthread_once_t g_api_once = PTHREAD_ONCE_INIT;

#if TARGET_OS_IPHONE
/* Device code and the managed DSP/runtime are signed and linked into the app.
   iOS must never try to open the macOS NativeAOT dynamic library. */
extern void *sky_audren_create(const void *,
    void (*)(void *, const int16_t *, uint32_t, uint32_t), void (*)(void *, int), void *);
extern uint32_t sky_audren_update(void *, void *, size_t, void *, size_t, const void *, size_t);
extern uint32_t sky_audren_start(void *);
extern uint32_t sky_audren_stop(void *);
extern void sky_audren_destroy(void *);
#endif

struct sky_audio_backend {
    void *renderer, *event;
    AudioUnit unit;
    int16_t samples[AUDIO_RING_FRAMES * 2];
    _Atomic uint64_t written, read;
    _Atomic uint64_t submitted, consumed, nonzero, callbacks;
    _Atomic uint64_t consumed_nonzero;
    int output_logged;
#if TARGET_OS_IPHONE
    struct sky_audio_backend *next;
    _Atomic int output_active, discard_pending;
#endif
};

#if TARGET_OS_IPHONE
static pthread_mutex_t g_outputs_lock = PTHREAD_MUTEX_INITIALIZER;
static sky_audio_backend_t *g_outputs;
static int g_host_active;
#endif

void sky_audio_backend_set_host_active(int active) {
#if TARGET_OS_IPHONE
    pthread_mutex_lock(&g_outputs_lock);
    g_host_active = active != 0;
    for (sky_audio_backend_t *backend = g_outputs; backend; backend = backend->next) {
        if (g_host_active == atomic_load(&backend->output_active)) continue;
        atomic_store(&backend->discard_pending, 1);
        atomic_store(&backend->output_active, g_host_active);
        OSStatus status = g_host_active ? AudioOutputUnitStart(backend->unit) : AudioOutputUnitStop(backend->unit);
        if (status) {
            atomic_store(&backend->output_active, 0);
            LOG("AUDIO_BACKEND_ERROR reason=remoteio-session-%s status=%d",
                g_host_active ? "resume" : "pause", (int)status);
        }
    }
    pthread_mutex_unlock(&g_outputs_lock);
#else
    (void)active;
#endif
}

static void renderer_load(void) {
#if TARGET_OS_IPHONE
    g_api = (renderer_api_t){sky_audren_create, sky_audren_update,
        sky_audren_start, sky_audren_stop, sky_audren_destroy};
#else
    const char *configured = getenv("SKY_AUDIO_RENDERER_LIB");
    void *library = configured ? dlopen(configured, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!configured) {
        char executable[PATH_MAX];
        uint32_t size = sizeof executable;
        if (!_NSGetExecutablePath(executable, &size)) {
            char *slash = strrchr(executable, '/');
            if (slash) *slash = 0;
            char path[PATH_MAX];
            int count = snprintf(path, sizeof path, "%s/audio-renderer/SkyAudioRenderer.dylib", executable);
            if (count > 0 && (size_t)count < sizeof path) library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!library) {
                count = snprintf(path, sizeof path, "%s/build/audio-renderer/SkyAudioRenderer.dylib", executable);
                if (count > 0 && (size_t)count < sizeof path) library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            }
        }
    }
    if (!library) {
        LOG("AUDIO_BACKEND_ERROR reason=renderer-library-missing detail=%s", dlerror());
        return;
    }
#define LOAD(name, symbol) do { *(void **)(&g_api.name) = dlsym(library, symbol); \
    if (!g_api.name) die("audio renderer export missing: %s", symbol); } while (0)
    LOAD(create, "sky_audren_create");
    LOAD(update, "sky_audren_update");
    LOAD(start, "sky_audren_start");
    LOAD(stop, "sky_audren_stop");
    LOAD(destroy, "sky_audren_destroy");
#undef LOAD
#endif
}

static void renderer_event(void *context, int signaled) {
    sky_audio_backend_t *backend = context;
    if (signaled) (void)sky_event_signal(backend->event);
    else (void)sky_event_clear(backend->event);
}

static void renderer_samples(void *context, const int16_t *samples, uint32_t count, uint32_t channels) {
    sky_audio_backend_t *backend = context;
#if TARGET_OS_IPHONE
    /* The DSP still maintains guest completion/status while audio is inactive.
       Do not accumulate stale sound or overflow its ring during an interruption. */
    if (!atomic_load(&backend->output_active)) return;
#endif
    if (!channels || count % channels) die("invalid renderer PCM sample layout");
    uint32_t frames = count / channels;
    uint64_t written = atomic_load_explicit(&backend->written, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&backend->read, memory_order_acquire);
    uint64_t available = AUDIO_RING_FRAMES - (written - read);
    if (frames > available) {
        LOG("AUDIO_BACKEND_ERROR reason=pcm-ring-overrun frames=%u available=%llu", frames,
            (unsigned long long)available);
        return;
    }
    uint64_t nonzero = 0;
    for (uint32_t frame = 0; frame < frames; frame++) {
        size_t destination = ((written + frame) % AUDIO_RING_FRAMES) * 2;
        int16_t left = samples[frame * channels];
        int16_t right = samples[frame * channels + (channels > 1)];
        backend->samples[destination] = left;
        backend->samples[destination + 1] = right;
        nonzero += !!left + !!right;
    }
    atomic_store_explicit(&backend->written, written + frames, memory_order_release);
    uint64_t submitted = atomic_fetch_add(&backend->submitted, frames) + frames;
    uint64_t previous_nonzero = atomic_fetch_add(&backend->nonzero, nonzero);
    if (nonzero && !previous_nonzero)
        LOG("AUDIO_PCM_NONZERO frames=%u submitted=%llu source=ryujinx-dsp", frames,
            (unsigned long long)submitted);
    uint64_t consumed_nonzero = atomic_load(&backend->consumed_nonzero);
    if (consumed_nonzero && !backend->output_logged) {
        backend->output_logged = 1;
        LOG("AUDIO_COREAUDIO_NONZERO consumed_samples=%llu callbacks=%llu",
            (unsigned long long)consumed_nonzero, (unsigned long long)atomic_load(&backend->callbacks));
    }
}

static OSStatus output_render(void *context, AudioUnitRenderActionFlags *flags,
    const AudioTimeStamp *time, UInt32 bus, UInt32 frames, AudioBufferList *buffers) {
    (void)flags; (void)time; (void)bus;
    sky_audio_backend_t *backend = context;
    if (!buffers || buffers->mNumberBuffers != 1 || !buffers->mBuffers[0].mData) return noErr;
    int16_t *output = buffers->mBuffers[0].mData;
    uint64_t read = atomic_load_explicit(&backend->read, memory_order_relaxed);
    uint64_t written = atomic_load_explicit(&backend->written, memory_order_acquire);
#if TARGET_OS_IPHONE
    if (atomic_exchange(&backend->discard_pending, 0)) read = written;
#endif
    UInt32 copy = written - read < frames ? (UInt32)(written - read) : frames;
    uint64_t nonzero = 0;
    for (UInt32 frame = 0; frame < copy; frame++) {
        size_t source = ((read + frame) % AUDIO_RING_FRAMES) * 2;
        output[frame * 2] = backend->samples[source];
        output[frame * 2 + 1] = backend->samples[source + 1];
        nonzero += !!output[frame * 2] + !!output[frame * 2 + 1];
    }
    memset(output + copy * 2, 0, (frames - copy) * 2 * sizeof *output);
    atomic_store_explicit(&backend->read, read + copy, memory_order_release);
    atomic_fetch_add(&backend->consumed, copy);
    atomic_fetch_add(&backend->consumed_nonzero, nonzero);
    atomic_fetch_add(&backend->callbacks, 1);
    return noErr;
}

sky_audio_backend_t *sky_audio_backend_create(const void *parameter, void *event) {
    pthread_once(&g_api_once, renderer_load);
    if (!g_api.create) return NULL;
    sky_audio_backend_t *backend = calloc(1, sizeof *backend);
    if (!backend) return NULL;
    backend->event = event;
    backend->renderer = g_api.create(parameter, renderer_samples, renderer_event, backend);
    if (!backend->renderer) { free(backend); return NULL; }
    AudioComponentDescription description = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
        .componentSubType = kAudioUnitSubType_RemoteIO, .componentManufacturer = kAudioUnitManufacturer_Apple};
#else
        .componentSubType = kAudioUnitSubType_DefaultOutput, .componentManufacturer = kAudioUnitManufacturer_Apple};
#endif
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    OSStatus status = component ? AudioComponentInstanceNew(component, &backend->unit) : -1;
    AudioStreamBasicDescription format = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AURenderCallbackStruct callback = {output_render, backend};
    if (!status) status = AudioUnitSetProperty(backend->unit, kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &format, sizeof format);
    if (!status) status = AudioUnitSetProperty(backend->unit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &callback, sizeof callback);
    if (!status) status = AudioUnitInitialize(backend->unit);
#if TARGET_OS_IPHONE
    if (!status) {
        pthread_mutex_lock(&g_outputs_lock);
        atomic_store(&backend->output_active, g_host_active);
        if (g_host_active) status = AudioOutputUnitStart(backend->unit);
        if (!status) { backend->next = g_outputs; g_outputs = backend; }
        pthread_mutex_unlock(&g_outputs_lock);
    }
#else
    if (!status) status = AudioOutputUnitStart(backend->unit);
#endif
    if (status) {
        LOG("AUDIO_BACKEND_ERROR reason=coreaudio-initialize status=%d", (int)status);
        sky_audio_backend_destroy(backend);
        return NULL;
    }
    LOG("AUDIO_BACKEND_READY renderer=ryujinx-dsp sink=%s sample_rate=48000 channels=2",
#if TARGET_OS_IPHONE
        "coreaudio-remoteio"
#else
        "coreaudio"
#endif
    );
    return backend;
}

uint32_t sky_audio_backend_update(sky_audio_backend_t *backend, void *output, size_t output_size,
    void *performance, size_t performance_size, const void *input, size_t input_size) {
    return g_api.update(backend->renderer, output, output_size, performance, performance_size, input, input_size);
}
uint32_t sky_audio_backend_start(sky_audio_backend_t *backend) { return g_api.start(backend->renderer); }
uint32_t sky_audio_backend_stop(sky_audio_backend_t *backend) { return g_api.stop(backend->renderer); }
void sky_audio_backend_destroy(sky_audio_backend_t *backend) {
    if (!backend) return;
#if TARGET_OS_IPHONE
    pthread_mutex_lock(&g_outputs_lock);
    sky_audio_backend_t **link = &g_outputs;
    while (*link && *link != backend) link = &(*link)->next;
    if (*link) *link = backend->next;
    atomic_store(&backend->output_active, 0);
#else
    if (backend->renderer) g_api.destroy(backend->renderer);
#endif
    if (backend->unit) {
        AudioOutputUnitStop(backend->unit);
        AudioUnitUninitialize(backend->unit);
        AudioComponentInstanceDispose(backend->unit);
    }
#if TARGET_OS_IPHONE
    pthread_mutex_unlock(&g_outputs_lock);
    if (backend->renderer) g_api.destroy(backend->renderer);
#endif
    LOG("AUDIO_BACKEND_CLOSED submitted_frames=%llu consumed_frames=%llu nonzero_samples=%llu callbacks=%llu consumed_nonzero=%llu",
        (unsigned long long)atomic_load(&backend->submitted), (unsigned long long)atomic_load(&backend->consumed),
        (unsigned long long)atomic_load(&backend->nonzero), (unsigned long long)atomic_load(&backend->callbacks),
        (unsigned long long)atomic_load(&backend->consumed_nonzero));
    free(backend);
}
