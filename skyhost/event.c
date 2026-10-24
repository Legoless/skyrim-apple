#include "event.h"
#include "skyhost.h"

#include <errno.h>

#define RES_OK 0u
#define RES_FAIL 0x2A2u
#define MAX_EVENTS 64
#define RES_BAD_HANDLE 0xE401u
#define RES_TIMEOUT 0xEA01u

typedef struct {
    void *guest;
    int autoclear, signaled;
    uint64_t generation, period_ns, next_signal_ns, poll_backoff_ns;
} sky_event_t;
static sky_event_t g_events[MAX_EVENTS];
static uint64_t g_generation;
static pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_event_cond = PTHREAD_COND_INITIALIZER;

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

uint32_t sky_event_create(void *guest, int autoclear) {
    if (!guest) return RES_FAIL;
    pthread_mutex_lock(&g_event_lock);
    for (int index = 0; index < MAX_EVENTS; index++)
        if (g_events[index].guest == guest) {
            pthread_mutex_unlock(&g_event_lock);
            return RES_OK;
        }
    for (int index = 0; index < MAX_EVENTS; index++) {
        if (g_events[index].guest) continue;
        g_events[index] = (sky_event_t){.guest = guest, .autoclear = !!autoclear,
                                      .generation = ++g_generation};
        pthread_mutex_unlock(&g_event_lock);
        LOG("EVENT_CREATE handle=%p autoclear=%d", guest, autoclear);
        return RES_OK;
    }
    pthread_mutex_unlock(&g_event_lock);
    return RES_FAIL;
}

uint32_t sky_event_destroy(void *guest) {
    if (!guest) return RES_FAIL;
    pthread_mutex_lock(&g_event_lock);
    for (int index = 0; index < MAX_EVENTS; index++) {
        if (g_events[index].guest != guest) continue;
        memset(&g_events[index], 0, sizeof g_events[index]);
        pthread_cond_broadcast(&g_event_cond);
        pthread_mutex_unlock(&g_event_lock);
        LOG("EVENT_DESTROY handle=%p", guest);
        return RES_OK;
    }
    pthread_mutex_unlock(&g_event_lock);
    return RES_FAIL;
}

static sky_event_t *event_find(void *guest) {
    for (int index = 0; index < MAX_EVENTS; index++)
        if (g_events[index].guest == guest && guest) return &g_events[index];
    return NULL;
}

static int event_is_host(void *guest) {
    pthread_mutex_lock(&g_event_lock);
    int result = event_find(guest) != NULL;
    pthread_mutex_unlock(&g_event_lock);
    return result;
}

uint32_t sky_event_signal(void *guest) {
    pthread_mutex_lock(&g_event_lock);
    sky_event_t *event = event_find(guest);
    if (event) {
        event->signaled = 1;
        pthread_cond_broadcast(&g_event_cond);
    }
    pthread_mutex_unlock(&g_event_lock);
    return event ? RES_OK : RES_BAD_HANDLE;
}

uint32_t sky_event_clear(void *guest) {
    pthread_mutex_lock(&g_event_lock);
    sky_event_t *event = event_find(guest);
    if (event) event->signaled = 0;
    pthread_mutex_unlock(&g_event_lock);
    return event ? RES_OK : RES_BAD_HANDLE;
}

uint32_t sky_event_set_periodic(void *guest, uint64_t period_ns) {
    pthread_mutex_lock(&g_event_lock);
    sky_event_t *event = event_find(guest);
    if (event) {
        event->period_ns = period_ns;
        event->next_signal_ns = period_ns ? now_ns() + period_ns : 0;
        pthread_cond_broadcast(&g_event_cond);
    }
    pthread_mutex_unlock(&g_event_lock);
    return event ? RES_OK : RES_BAD_HANDLE;
}

uint32_t sky_event_set_poll_backoff(void *guest, uint64_t backoff_ns) {
    pthread_mutex_lock(&g_event_lock);
    sky_event_t *event = event_find(guest);
    if (event) {
        event->poll_backoff_ns = backoff_ns;
        pthread_cond_broadcast(&g_event_cond);
    }
    pthread_mutex_unlock(&g_event_lock);
    return event ? RES_OK : RES_BAD_HANDLE;
}

void sky_aoc_get_add_on_content_list_changed_event(void *event) {
    if (!event) die("nn::aoc::GetAddOnContentListChangedEvent called with null event");
    if (sky_event_create(event, 1) != RES_OK)
        die("nn::aoc::GetAddOnContentListChangedEvent failed to create host event");
    /* Skyrim's AoCChangeThread spins on TryWaitSystemEvent in a tight loop.
     * Throttle unsignaled polls on the condvar while waking immediately on signal/destroy. */
    (void)sky_event_set_poll_backoff(event, 10000000ull);
    ((uint8_t *)event)[0x28] = 1;
    sky_checkpoint("post_nnMain_aoc_change_event");
    LOG("AOC_CHANGE_EVENT handle=%p source=host-offline", event);
}

SKY_FORCE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE",
          "Bind the offline add-on-content change notification event to a host-owned SystemEvent",
          void, sky_aoc_get_add_on_content_list_changed_event_entry, void *event) {
    sky_aoc_get_add_on_content_list_changed_event(event);
}

uint32_t sky_event_wait(void *guest, int64_t timeout_ns) {
    uint64_t started = now_ns();
    pthread_mutex_lock(&g_event_lock);
    sky_event_t *event = event_find(guest);
    if (!event) {
        pthread_mutex_unlock(&g_event_lock);
        return RES_BAD_HANDLE;
    }
    uint64_t generation = event->generation;
    uint32_t result;
    for (;;) {
        if (event->guest != guest || event->generation != generation) {
            result = RES_BAD_HANDLE;
            break;
        }
        uint64_t now = now_ns();
        if (event->period_ns && now >= event->next_signal_ns) {
            event->signaled = 1;
            event->next_signal_ns += ((now - event->next_signal_ns) / event->period_ns + 1) * event->period_ns;
        }
        if (event->signaled) {
            if (event->autoclear) event->signaled = 0;
            result = RES_OK;
            break;
        }
        uint64_t elapsed = now - started;
        if (timeout_ns >= 0 && elapsed >= (uint64_t)timeout_ns) {
            result = RES_TIMEOUT;
            break;
        }
        uint64_t remaining = timeout_ns < 0 ? UINT64_MAX : (uint64_t)timeout_ns - elapsed;
        if (event->period_ns && event->next_signal_ns - now < remaining)
            remaining = event->next_signal_ns - now;
        int error;
        if (remaining == UINT64_MAX) {
            error = pthread_cond_wait(&g_event_cond, &g_event_lock);
        } else {
            struct timespec duration = {(time_t)(remaining / 1000000000), (long)(remaining % 1000000000)};
            error = pthread_cond_timedwait_relative_np(&g_event_cond, &g_event_lock, &duration);
        }
        if (error && error != ETIMEDOUT) die("host SystemEvent wait failed: %d", error);
    }
    pthread_mutex_unlock(&g_event_lock);
    return result;
}

static void *original_event_function(const char *name) {
    void *function = guest_sym(name);
    if (!function) die("original SystemEvent function is missing: %s", name);
    return function;
}

SKY_FORCE("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE", "Destroy the host-owned event associated with the guest pointer",
          void, sky_event_destroy_entry, void *guest) {
    if (event_is_host(guest)) (void)sky_event_destroy(guest);
    else ((void (*)(void *))original_event_function("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))(guest);
}

SKY_FORCE("_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE", "Wait for a host-owned event or dispatch guest-owned events to the original SDK",
          void, sky_event_wait_entry, void *guest) {
    if (!event_is_host(guest)) {
        ((void (*)(void *))original_event_function("_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE"))(guest);
        return;
    }
    uint32_t result = sky_event_wait(guest, -1);
    if (result != RES_OK) die("host SystemEvent wait returned %#x", result);
}

SKY_FORCE("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE", "Poll a host-owned event with its configured clear mode",
          int, sky_event_try_wait_entry, void *guest) {
    if (!event_is_host(guest))
        return ((int (*)(void *))original_event_function("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE"))(guest);
    if (sky_event_wait(guest, 0) == RES_OK) return 1;
    uint64_t backoff_ns = 0;
    pthread_mutex_lock(&g_event_lock);
    sky_event_t *event = event_find(guest);
    if (event) backoff_ns = event->poll_backoff_ns;
    pthread_mutex_unlock(&g_event_lock);
    if (!backoff_ns) return 0;
    return sky_event_wait(guest, (int64_t)backoff_ns) == RES_OK;
}

SKY_FORCE("_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE", "Wait for a host-owned event until its deadline",
          int, sky_event_timed_wait_entry, void *guest, int64_t timeout_ns) {
    if (!event_is_host(guest))
        return ((int (*)(void *, int64_t))original_event_function("_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"))(guest, timeout_ns);
    return sky_event_wait(guest, timeout_ns) == RES_OK;
}

SKY_FORCE("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE", "Clear a host-owned event without touching opaque guest event storage",
          void, sky_event_clear_entry, void *guest) {
    if (event_is_host(guest)) (void)sky_event_clear(guest);
    else ((void (*)(void *))original_event_function("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE"))(guest);
}
