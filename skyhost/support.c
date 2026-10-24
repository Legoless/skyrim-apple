#include "skyhost.h"
#include <stdatomic.h>
#include <time.h>

static _Atomic(const char *) g_checkpoint = "map-only";
static void (*g_gate_context)(const uint64_t *regs);
static _Atomic int g_post_entry;
static pthread_once_t g_service_names_once = PTHREAD_ONCE_INIT;
static char g_service_names[256][32];
static _Atomic uint64_t g_service_calls[256];
static int g_trace_services;

static void initialize_service_names(void) {
    for (unsigned index = 0; index < 256; index++)
        snprintf(g_service_names[index], sizeof g_service_names[index], "post_nnMain_svc_%#x", index);
    const char *trace = getenv("SKY_TRACE_SVC");
    g_trace_services = trace && !strcmp(trace, "1");
}

/* Weak stub: loader.c provides the real module_of in the full host;
 * unit tests that link svc.c without loader.o fall back to NULL. */
__attribute__((weak)) module_t *module_of(const void *pointer) { (void)pointer; return NULL; }

void sky_checkpoint(const char *name) { g_checkpoint = name; }
const char *sky_checkpoint_name(void) { return g_checkpoint; }
void sky_gate_context_register(void (*callback)(const uint64_t *regs)) { g_gate_context = callback; }
void sky_gate_context(const uint64_t *regs) { if (g_gate_context) g_gate_context(regs); }
void sky_post_entry_enable(void) { g_post_entry = 1; }
void sky_post_entry_service(int number) {
    if (!g_post_entry || number < 0 || number >= 256) return;
    pthread_once(&g_service_names_once, initialize_service_names);
    sky_checkpoint(g_service_names[number]);
    uint64_t count = atomic_fetch_add(&g_service_calls[number], 1) + 1;
    /* Busy waits hit these gates millions of times. Preserve checkpoints on
       every call, and bounded diagnostics unless an explicit trace is wanted. */
    if (g_trace_services || count <= 4 || !(count & (count - 1)))
        LOG("POST_NNMAIN svc=%#x count=%llu", number, (unsigned long long)count);
}

uint64_t now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) die("clock_gettime failed");
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static _Atomic uintptr_t g_main_module_base;
static _Atomic uintptr_t g_guest_main_thread;
static _Atomic int g_startup_logo_done;
static _Atomic int g_ui_in_game;
static _Atomic uint32_t g_last_frame_draws;
static pthread_once_t g_verbose_once = PTHREAD_ONCE_INIT;
static int g_verbose_log;

static void init_verbose_log(void) {
    const char *env = getenv("SKY_VERBOSE_LOG");
    g_verbose_log = env && !strcmp(env, "1");
}

void sky_set_main_module_base(uintptr_t base) {
    atomic_store_explicit(&g_main_module_base, base, memory_order_release);
}

uintptr_t sky_main_module_base(void) {
    return atomic_load_explicit(&g_main_module_base, memory_order_acquire);
}

void sky_set_guest_main_thread(pthread_t thread) {
    atomic_store_explicit(&g_guest_main_thread, (uintptr_t)thread, memory_order_release);
}

int sky_is_guest_main_thread(void) {
    uintptr_t thread = atomic_load_explicit(&g_guest_main_thread, memory_order_acquire);
    return thread != 0 && pthread_equal((pthread_t)thread, pthread_self());
}

pthread_t sky_guest_main_thread(void) {
    return (pthread_t)atomic_load_explicit(&g_guest_main_thread, memory_order_acquire);
}

void sky_set_startup_logo_finished(int finished) {
    atomic_store_explicit(&g_startup_logo_done, finished ? 1 : 0, memory_order_release);
}

int sky_is_startup_logo_finished(void) {
    return atomic_load_explicit(&g_startup_logo_done, memory_order_acquire);
}

void sky_set_ui_in_game(int in_game) {
    atomic_store_explicit(&g_ui_in_game, in_game > 0 ? in_game : 0, memory_order_release);
}

int sky_ui_is_in_game(void) {
    return atomic_load_explicit(&g_ui_in_game, memory_order_acquire);
}

void sky_set_last_frame_draw_count(uint32_t draws) {
    atomic_store_explicit(&g_last_frame_draws, draws, memory_order_release);
}

uint32_t sky_last_frame_draw_count(void) {
    return atomic_load_explicit(&g_last_frame_draws, memory_order_acquire);
}

int sky_verbose_logging(void) {
    pthread_once(&g_verbose_once, init_verbose_log);
    return g_verbose_log;
}
