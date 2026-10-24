#include "hid.h"
#include "skyhost.h"

#include <stdatomic.h>
#include <math.h>

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

_Static_assert(sizeof(sky_hid_npad_state_t) == 0x28, "Skyrim Npad state size");
_Static_assert(offsetof(sky_hid_npad_state_t, attributes) == 0x20, "Skyrim Npad attributes offset");
_Static_assert(sizeof(sky_hid_six_axis_state_t) == 0x60, "Skyrim six-axis state size");
_Static_assert(offsetof(sky_hid_six_axis_state_t, attributes) == 0x58, "Skyrim six-axis attributes offset");
_Static_assert(sizeof(sky_hid_vibration_info_t) == 8, "Skyrim vibration info size");
_Static_assert(sizeof(sky_hid_vibration_value_t) == 16, "Skyrim vibration value size");
_Static_assert(sizeof(sky_hid_keyboard_state_t) == 0x30, "Skyrim keyboard state size");

static _Atomic int64_t g_sampling;
static _Atomic int g_state_logged;
static pthread_mutex_t g_host_state_lock = PTHREAD_MUTEX_INITIALIZER;
enum { HOST_INPUT_SOURCES = 3 };
typedef struct {
    sky_hid_npad_state_t held;
    uint64_t pending_buttons;
    int32_t pending_lx, pending_ly, pending_rx, pending_ry;
} hid_source_state_t;
static hid_source_state_t g_sources[HOST_INPUT_SOURCES];
static uint64_t g_host_state_transitions;
typedef struct {
    sky_hid_npad_state_t state;
    uint32_t id;
    uint64_t reads, transitions;
    int seen;
} hid_read_diagnostic_t;
static hid_read_diagnostic_t g_full_key_diagnostics[10];
static _Atomic int g_six_axis_started;
static _Atomic uint64_t g_six_axis_sampling;
static _Atomic uint64_t g_keyboard_sampling;
static _Atomic unsigned g_vibration_initialized;
#define HOST_FULLKEY_SIX_AXIS_HANDLE 0x00020003u

static int valid_six_axis_handle(const uint32_t *handle) {
    return handle && *handle == HOST_FULLKEY_SIX_AXIS_HANDLE;
}

int sky_hid_get_six_axis_handles(uint32_t *out, int maximum, const uint32_t *id, uint32_t styles) {
    if (!out || maximum <= 0 || !id || *id != 0 || !(styles & 1u)) return 0;
    /* Canonical FullKey style index 3, player 0, sensor device index 2. */
    *out = HOST_FULLKEY_SIX_AXIS_HANDLE;
    return 1;
}

uint32_t sky_hid_start_six_axis_sensor(const uint32_t *handle) {
    if (!valid_six_axis_handle(handle)) return 0x2a2;
    atomic_store(&g_six_axis_started, 1);
    LOG("HID_SIX_AXIS_START handle=%#x mode=stationary-host-controller no-ipc", *handle);
    return 0;
}

uint32_t sky_hid_stop_six_axis_sensor(const uint32_t *handle) {
    if (!valid_six_axis_handle(handle)) return 0x2a2;
    atomic_store(&g_six_axis_started, 0);
    LOG("HID_SIX_AXIS_STOP handle=%#x", *handle);
    return 0;
}

int sky_hid_is_six_axis_sensor_at_rest(const uint32_t *handle) {
    return valid_six_axis_handle(handle);
}

int sky_hid_get_six_axis_states(sky_hid_six_axis_state_t *out, int maximum, const uint32_t *handle) {
    if (!out || maximum <= 0 || !valid_six_axis_handle(handle) || !atomic_load(&g_six_axis_started)) return 0;
    memset(out, 0, sizeof *out);
    out->delta_time = 5000000;
    out->sampling = atomic_fetch_add(&g_six_axis_sampling, 1) + 1;
    out->acceleration[2] = -1.f;
    out->direction[0] = out->direction[4] = out->direction[8] = 1.f;
    out->attributes = 1;
    return 1;
}

void sky_hid_get_six_axis_state(sky_hid_six_axis_state_t *out, const uint32_t *handle) {
    if (out && !sky_hid_get_six_axis_states(out, 1, handle)) memset(out, 0, sizeof *out);
}

static int vibration_index(const uint32_t *handle) {
    if (!handle || (*handle != 0x00000003u && *handle != 0x00010003u)) return -1;
    return (int)(*handle >> 16);
}

int sky_hid_get_vibration_handles(uint32_t *out, int maximum, const uint32_t *id, uint32_t styles) {
    if (!out || maximum <= 0 || !id || *id != 0 || !(styles & 1u)) return 0;
    out[0] = 3;
    if (maximum > 1) out[1] = 0x00010003;
    return maximum > 1 ? 2 : 1;
}

uint32_t sky_hid_initialize_vibration_device(const uint32_t *handle) {
    int index = vibration_index(handle);
    if (index < 0) return 0x2a2;
    atomic_fetch_or(&g_vibration_initialized, 1u << index);
    LOG("HID_VIBRATION_INITIALIZE handle=%#x hardware=unavailable no-ipc", *handle);
    return 0;
}

uint32_t sky_hid_get_vibration_info(sky_hid_vibration_info_t *out, const uint32_t *handle) {
    if (!out || vibration_index(handle) < 0) return 0x2a2;
    /* The keyboard-backed FullKey controller has no physical actuator. */
    *out = (sky_hid_vibration_info_t){0, 0};
    return 0;
}

uint32_t sky_hid_send_vibration_value(const uint32_t *handle, const sky_hid_vibration_value_t *value) {
    int index = vibration_index(handle);
    if (index < 0 || !value || !(atomic_load(&g_vibration_initialized) & (1u << index))) return 0x2a2;
    if (!isfinite(value->amplitude_low) || !isfinite(value->amplitude_high) ||
        !isfinite(value->frequency_low) || !isfinite(value->frequency_high)) return 0x2a2;
    /* Accept the request for the virtual controller. Actual amplitudes remain
     * zero because no motor is attached; do not claim physical actuation. */
    return 0;
}

uint32_t sky_hid_get_actual_vibration_value(sky_hid_vibration_value_t *out, const uint32_t *handle) {
    if (!out || vibration_index(handle) < 0) return 0x2a2;
    *out = (sky_hid_vibration_value_t){0.f, 160.f, 0.f, 320.f};
    return 0;
}

void sky_hid_get_keyboard_state(sky_hid_keyboard_state_t *out) {
    if (!out) return;
    /* Host keyboard input is exposed as the connected FullKey controller.
     * There is no separate Switch USB keyboard in this host profile. */
    memset(out, 0, sizeof *out);
    out->sampling = atomic_fetch_add(&g_keyboard_sampling, 1) + 1;
}

static void set_source_state(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry, int queue_edges, uint64_t tap_buttons) {
    if ((unsigned)source >= HOST_INPUT_SOURCES) return;
    pthread_mutex_lock(&g_host_state_lock);
    hid_source_state_t *state = &g_sources[source];
    uint64_t previous_pending = state->pending_buttons;
    int pending_sticks_changed = 0;
    if (queue_edges) {
        state->pending_buttons |= (buttons | tap_buttons) & ~state->held.buttons;
        /* Preserve a short keyboard deflection until one sample. Unchanged
         * key repeats must not queue extra motion after a held key releases. */
        if ((lx || ly) && (lx != state->held.lx || ly != state->held.ly)) {
            pending_sticks_changed = state->pending_lx != lx || state->pending_ly != ly;
            state->pending_lx = lx;
            state->pending_ly = ly;
        }
        if ((rx || ry) && (rx != state->held.rx || ry != state->held.ry)) {
            pending_sticks_changed |= state->pending_rx != rx || state->pending_ry != ry;
            state->pending_rx = rx;
            state->pending_ry = ry;
        }
    } else {
        state->pending_buttons = 0;
        pending_sticks_changed = !!(state->pending_lx || state->pending_ly || state->pending_rx || state->pending_ry);
        state->pending_lx = state->pending_ly = state->pending_rx = state->pending_ry = 0;
    }
    int changed = state->held.buttons != buttons || state->held.lx != lx ||
        state->held.ly != ly || state->held.rx != rx || state->held.ry != ry ||
        previous_pending != state->pending_buttons || pending_sticks_changed;
    state->held.buttons = buttons;
    state->held.lx = lx;
    state->held.ly = ly;
    state->held.rx = rx;
    state->held.ry = ry;
    if (changed) {
        uint64_t count = ++g_host_state_transitions;
        if (count <= 32 || !(count & (count - 1)))
            LOG("HID_HOST_STATE transition=%llu at_ns=%llu buttons=%#llx sticks=%d,%d,%d,%d mode=%s pending=%#llx source=%u",
                (unsigned long long)count, (unsigned long long)now_ns(),
                (unsigned long long)buttons, lx, ly, rx, ry, queue_edges ? "queued" : "raw",
                (unsigned long long)state->pending_buttons, (unsigned)source);
    }
    pthread_mutex_unlock(&g_host_state_lock);
}

void sky_hid_set_source_state(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    set_source_state(source, buttons, lx, ly, rx, ry, 0, 0);
}

void sky_hid_queue_source_state(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    set_source_state(source, buttons, lx, ly, rx, ry, 1, 0);
}

void sky_hid_queue_source_tap(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry, uint64_t tap_buttons) {
    /* Publish this source's held snapshot and pending pulse atomically. */
    set_source_state(source, buttons, lx, ly, rx, ry, 1, tap_buttons);
}

void sky_hid_set_host_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, buttons, lx, ly, rx, ry);
}

void sky_hid_queue_host_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    sky_hid_queue_source_state(SKY_HID_SOURCE_EXTERNAL, buttons, lx, ly, rx, ry);
}

void sky_hid_queue_host_tap(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry,
                            uint64_t tap_buttons) {
    sky_hid_queue_source_tap(SKY_HID_SOURCE_EXTERNAL, buttons, lx, ly, rx, ry, tap_buttons);
}

static void ensure_scaleform_translator_hook(void);

uint32_t sky_hid_initialize_npad(void) {
    ensure_scaleform_translator_hook();
    sky_checkpoint("post_nnMain_hid_initialize");
    LOG("HID_INITIALIZE mode=neutral no-ipc");
    return 0;
}

void sky_hid_set_supported_npad_style_set(uint32_t styles) {
    LOG("HID_SUPPORTED_STYLES value=%#x host=neutral-fullkey", styles);
}

void sky_hid_set_supported_npad_id_type(const uint32_t *ids, uint64_t count) {
    LOG("HID_SUPPORTED_IDS count=%llu host=neutral-fullkey", (unsigned long long)count);
    for (uint64_t index = 0; ids && index < count && index < 8; index++)
        LOG("HID_SUPPORTED_ID index=%llu id=%#x", (unsigned long long)index, ids[index]);
}

void sky_hid_set_npad_joy_hold_type(int type) {
    LOG("HID_JOY_HOLD_TYPE value=%d host=neutral-fullkey", type);
}

uint32_t sky_hid_get_npad_style_set(const uint32_t *id) {
    return id && *id == 0 ? 1u : 0u;
}

static void fill_state(sky_hid_npad_state_t *out, const uint32_t *id, uint32_t style);

enum { MENU_TOUCH_QUEUE_CAP = 64 };
typedef struct {
    unsigned phase;
    uint64_t finger;
    float nx, ny;
} menu_touch_event_t;

static menu_touch_event_t g_menu_touches[MENU_TOUCH_QUEUE_CAP];
static unsigned g_menu_touch_head, g_menu_touch_tail;
static uint64_t g_menu_primary_finger;
static int g_menu_has_primary;
static float g_menu_start_x, g_menu_start_y;
static float g_menu_step_x, g_menu_step_y;
static int g_menu_dragged;
static unsigned g_menu_active_fingers;
static uint64_t g_menu_pulse_buttons;
static unsigned g_menu_pulse_frames;
static uint64_t g_menu_pulse_until_ns;
static uint64_t g_last_main_pulse_tick_ns;
static int g_last_logged_in_game = -1;
static int g_translator_hooked;
/* StartMenu's own platform before touch input switched it to mouse mode, and a
 * pending restore after a confirm-dialog click (the click is processed on the
 * movie's next Advance, so the restore is deferred). */
static double g_native_menu_platform = NAN;
static uint64_t g_restore_platform_at_ns;
static void *g_restore_platform_movie;

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

typedef struct {
    void *obj_iface;
    uint32_t type;
    uint32_t pad;
    union {
        double num;
        uint8_t boolean;
        const char *str;
        void *ptr;
    } value;
} gfx_value_t;
_Static_assert(sizeof(gfx_value_t) == 0x18, "Scaleform GFxValue size");

typedef struct {
    uint32_t type;
    float x, y, scroll;
    uint32_t button, mouse_idx;
} gfx_mouse_event_t;
_Static_assert(sizeof(gfx_mouse_event_t) == 0x18, "Scaleform GFxMouseEvent size");

/* main+0xf83450: x1 = method path, x2 = argument array, w3 = argument count. */
typedef uint8_t (*gfx_invoke_fn)(void *movie, const char *method, const gfx_value_t *args, uint32_t num_args);
typedef uint8_t (*gfx_get_var_fn)(void *movie, gfx_value_t *out, const char *path);
typedef uint32_t (*gfx_handle_event_fn)(void *movie, const gfx_mouse_event_t *event);
typedef void (*scaleform_translate_fn)(void *translator, void *translate_info);
typedef void (*scaleform_set_result_fn)(void *translate_info, const uint32_t *wstr, int64_t len);

static int is_touch_ui_enabled(void) {
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    return 1;
#else
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("SKY_TOUCH_UI");
        cached = (env && !strcmp(env, "1")) ? 1 : 0;
    }
    return cached;
#endif
}

static void sky_scaleform_translate(void *translator, void *translate_info) {
    if (!sky_profile_has_update_ui()) return;
    uintptr_t main_base = sky_main_module_base();
    if (!main_base) return;
    scaleform_translate_fn orig = (scaleform_translate_fn)(main_base + 0xc7651c);
    orig(translator, translate_info);
    if (!translate_info || !is_touch_ui_enabled()) return;
    const uint32_t *key = *(const uint32_t **)translate_info;
    if (!key) return;
    static const char k_target[] = "$Press [START]";
    for (size_t i = 0; i < sizeof(k_target); i++) {
        if (key[i] != (uint32_t)(uint8_t)k_target[i]) return;
    }
    static const uint32_t k_touch_to_begin[] = {
        'T','o','u','c','h',' ','t','o',' ','b','e','g','i','n', 0
    };
    scaleform_set_result_fn set_result = (scaleform_set_result_fn)(main_base + 0xf59430);
    set_result(translate_info, k_touch_to_begin, -1);
    LOG("SCALEFORM_TRANSLATE key=$Press [START] -> Touch to begin");
}

static void ensure_scaleform_translator_hook(void) {
    if (!sky_profile_has_update_ui()) return;
    if (g_translator_hooked || !is_touch_ui_enabled()) return;
    uintptr_t main_base = sky_main_module_base();
    if (!main_base) return;
    uintptr_t *vt_slot = (uintptr_t *)(main_base + 0x20df7e8);
    if (*vt_slot == main_base + 0xc7651c) {
        *vt_slot = (uintptr_t)sky_scaleform_translate;
        g_translator_hooked = 1;
    }
}

static void queue_menu_pulse_locked(uint64_t buttons) {
    if (!buttons) return;
    g_menu_pulse_buttons |= buttons;
    if (g_menu_pulse_frames < 2) g_menu_pulse_frames = 2;
    g_menu_pulse_until_ns = now_ns() + 90000000ull;
}

void sky_hid_menu_touch(unsigned phase, uint64_t finger, float nx, float ny) {
    if (nx < 0.f) nx = 0.f;
    if (nx > 1.f) nx = 1.f;
    if (ny < 0.f) ny = 0.f;
    if (ny > 1.f) ny = 1.f;
    pthread_mutex_lock(&g_host_state_lock);
    unsigned next_tail = (g_menu_touch_tail + 1) % MENU_TOUCH_QUEUE_CAP;
    if (next_tail == g_menu_touch_head)
        g_menu_touch_head = (g_menu_touch_head + 1) % MENU_TOUCH_QUEUE_CAP;
    g_menu_touches[g_menu_touch_tail] = (menu_touch_event_t){phase, finger, nx, ny};
    g_menu_touch_tail = next_tail;
    pthread_mutex_unlock(&g_host_state_lock);
}

static void gfx_set_platform(uintptr_t main_base, void *movie, double platform_id) {
    if (!main_base || !movie) return;
    gfx_invoke_fn invoke = (gfx_invoke_fn)(main_base + 0xf83450);
    gfx_value_t args[2];
    memset(args, 0, sizeof args);
    args[0].type = 3;
    args[0].value.num = platform_id;
    args[1].type = 2;
    args[1].value.boolean = 0;
    (void)invoke(movie, "_root.SetPlatform", args, 2);
}

static void gfx_invoke_attempt_equip(uintptr_t main_base, void *movie, double slot) {
    if (!main_base || !movie) return;
    gfx_invoke_fn invoke = (gfx_invoke_fn)(main_base + 0xf83450);
    gfx_value_t arg;
    memset(&arg, 0, sizeof arg);
    arg.type = 3;
    arg.value.num = slot;
    (void)invoke(movie, "_root.Menu_mc.AttemptEquip", &arg, 1);
}

static void gfx_invoke_noarg(uintptr_t main_base, void *movie, const char *method) {
    if (!main_base || !movie || !method) return;
    gfx_invoke_fn invoke = (gfx_invoke_fn)(main_base + 0xf83450);
    (void)invoke(movie, method, NULL, 0);
}

static void gfx_process_input(uintptr_t main_base, void *movie) {
    if (!main_base || !movie) return;
    /* GFxMovieRoot::ProcessInput (main+0xf7492c) drains [movie + 0x190],
     * updates Mouse.getTopMostEntity(), and queues button/mouse handlers;
     * GFxMovieRoot::DoActions (main+0xf6d5e0) executes queued AS actions. */
    typedef void (*gfx_movie_fn)(void *movie);
    ((gfx_movie_fn)(main_base + 0xf7492c))(movie);
    ((gfx_movie_fn)(main_base + 0xf6d5e0))(movie);
}

static void gfx_adjust_menu_coords(uintptr_t top_vt, float *nx, float *ny) {
    /* TweenMenu (0x1a91fa0) has narrow directional InputRects:
     * Skills (top, x=[0.42..0.58], y=[0..0.43]), Map (bottom, x=[0.42..0.58], y=[0.57..1.0]),
     * Magic (left, x=[0..0.47], y=[0.43..0.57]), Items (right, x=[0.53..1.0], y=[0.43..0.57]).
     * Snap any tap outside the center deadzone into the arm's InputRect center. */
    if (top_vt == 0x1a91fa0) {
        float dx = *nx - 0.5f;
        float dy = *ny - 0.5f;
        if (fabsf(dx) > 0.06f || fabsf(dy) > 0.06f) {
            if (fabsf(dx) >= fabsf(dy)) {
                *nx = (dx < 0.f) ? 0.25f : 0.75f;
                *ny = 0.50f;
            } else {
                *nx = 0.50f;
                *ny = (dy < 0.f) ? 0.25f : 0.75f;
            }
        }
    }
}

static void gfx_release_value(uintptr_t main_base, gfx_value_t *val) {
    if ((val->type & 0x40u) && val->obj_iface) {
        typedef void (*gfx_release_fn)(void *iface, gfx_value_t *value, void *data);
        ((gfx_release_fn)(main_base + 0xfce138))(val->obj_iface, val, val->value.ptr);
    }
}

static int gfx_get_value(void *movie, const char *path, gfx_value_t *val, uintptr_t *main_base) {
    *main_base = sky_main_module_base();
    memset(val, 0, sizeof *val);
    if (!movie || !path || !*main_base) return 0;
    /* GFxMovieRoot vtable (main+0x2108858) slot 0x90 = GetVariable(out, path). */
    uintptr_t *vtable = *(uintptr_t **)movie;
    if (!vtable || !vtable[0x90 / 8]) return 0;
    return ((gfx_get_var_fn)vtable[0x90 / 8])(movie, val, path) != 0;
}

/* Copies a Scaleform string variable into `out`. AS2 strings come back as
 * managed values (type bit 0x40) holding a pointer to a string node whose
 * first field is the character data; the reference is released afterwards
 * via ObjectInterface::ObjectRelease (main+0xfce138), as the game does. */
static const char *gfx_get_string_var(void *movie, const char *path, char *out, size_t out_size) {
    if (!out || !out_size) return NULL;
    out[0] = 0;
    uintptr_t main_base;
    gfx_value_t val;
    if (!gfx_get_value(movie, path, &val, &main_base)) return NULL;
    const char *s = NULL;
    if ((val.type & 0x3fu) == 4 && val.value.ptr)
        s = (val.type & 0x40u) ? *(const char *const *)val.value.ptr : val.value.str;
    if (s) snprintf(out, out_size, "%s", s);
    gfx_release_value(main_base, &val);
    return s ? out : NULL;
}

/* Reads a number or boolean variable; returns NAN when absent. */
static double gfx_get_number_var(void *movie, const char *path) {
    uintptr_t main_base;
    gfx_value_t val;
    if (!gfx_get_value(movie, path, &val, &main_base)) return NAN;
    double result = NAN;
    if ((val.type & 0x3fu) == 3) result = val.value.num;
    else if ((val.type & 0x3fu) == 2) result = val.value.boolean ? 1.0 : 0.0;
    gfx_release_value(main_base, &val);
    return result;
}

static void gfx_send_mouse_event(void *movie, uint32_t type, float nx, float ny) {
    if (!movie) return;
    uintptr_t *vtable = *(uintptr_t **)movie;
    if (!vtable || !vtable[0x170 / 8]) return;
    /* GFx viewport: left, top, width, height at 0x78..0x84 (see main+0xf737b8). */
    int32_t vp_x = *(int32_t *)((uintptr_t)movie + 0x78);
    int32_t vp_y = *(int32_t *)((uintptr_t)movie + 0x7c);
    int32_t vp_w = *(int32_t *)((uintptr_t)movie + 0x80);
    int32_t vp_h = *(int32_t *)((uintptr_t)movie + 0x84);
    if (vp_w <= 0 || vp_w > 7680 || vp_h <= 0 || vp_h > 4320) {
        vp_x = vp_y = 0;
        vp_w = 1280;
        vp_h = 720;
    }
    *(uint32_t *)((uintptr_t)movie + 0xa60) = 1;
    gfx_mouse_event_t ev = {
        .type = type,
        .x = (float)vp_x + nx * (float)vp_w,
        .y = (float)vp_y + ny * (float)vp_h,
        .scroll = 0.f,
        .button = 0,
        .mouse_idx = 0,
    };
    gfx_handle_event_fn handle_event = (gfx_handle_event_fn)vtable[0x170 / 8];
    if (type == 1) {
        /* Enqueue a zero-button entry in [movie + 0x190] so ProcessInput runs
         * onRollOver (setting iSelectedIndex) before any subsequent MouseDown. */
        gfx_mouse_event_t move_q = ev;
        move_q.type = 2;
        move_q.button = 16;
        (void)handle_event(movie, &move_q);
    }
    (void)handle_event(movie, &ev);
}

/* Unattended device testing: SKY_AUTO_TAPS="secs,x,y;secs,x,y" replays taps
 * (seconds since the first HID poll, normalized view coordinates) through the
 * same path as real touches. Does nothing unless the variable is set. */
static void run_scripted_taps(void) {
    enum { MAX_TAPS = 32 };
    static int parsed;
    static unsigned count, next_down, next_up;
    static double at[MAX_TAPS];
    static float tx[MAX_TAPS], ty[MAX_TAPS];
    static uint64_t start_ns;
    if (!parsed) {
        parsed = 1;
        start_ns = now_ns();
        const char *spec = getenv("SKY_AUTO_TAPS");
        while (spec && *spec && count < MAX_TAPS) {
            double s;
            float x, y;
            int used = 0;
            if (sscanf(spec, "%lf,%f,%f%n", &s, &x, &y, &used) != 3 || used <= 0) break;
            at[count] = s; tx[count] = x; ty[count] = y; count++;
            spec += used;
            if (*spec == ';') spec++;
        }
        if (count) LOG("AUTO_TAPS count=%u", count);
    }
    if (next_up >= count) return;
    double elapsed = (double)(now_ns() - start_ns) / 1e9;
    if (next_down < count && next_up == next_down && elapsed >= at[next_down]) {
        LOG("AUTO_TAP index=%u t=%.1f x=%.3f y=%.3f", next_down, elapsed, tx[next_down], ty[next_down]);
        if (tx[next_down] < 0.f) {
            /* x < 0: press the controller button mask given in y. */
            pthread_mutex_lock(&g_host_state_lock);
            queue_menu_pulse_locked((uint64_t)ty[next_down]);
            g_last_main_pulse_tick_ns = now_ns();
            pthread_mutex_unlock(&g_host_state_lock);
            next_down++;
            next_up = next_down;
            return;
        }
        sky_hid_menu_touch(0, 0x5ca1ab1e, tx[next_down], ty[next_down]);
        next_down++;
    } else if (next_up < next_down && elapsed >= at[next_up] + 0.15) {
        sky_hid_menu_touch(2, 0x5ca1ab1e, tx[next_up], ty[next_up]);
        next_up++;
    }
}

static void update_guest_ui_and_menu_touch(void) {
    uintptr_t main_base = sky_main_module_base();
    if (main_base && sky_is_guest_main_thread()) {
        run_scripted_taps();
    }
    if (!main_base || !sky_profile_has_update_ui()) {
        /* Unverified builds use the game's controller menus. Keep controls
         * visible and translate a menu tap to A without inspecting guest UI. */
        if (main_base) sky_set_ui_in_game(1);
        pthread_mutex_lock(&g_host_state_lock);
        while (g_menu_touch_head != g_menu_touch_tail) {
            menu_touch_event_t ev = g_menu_touches[g_menu_touch_head];
            g_menu_touch_head = (g_menu_touch_head + 1) % MENU_TOUCH_QUEUE_CAP;
            if (ev.phase == 0) queue_menu_pulse_locked(1ull << 0);
        }
        pthread_mutex_unlock(&g_host_state_lock);
        return;
    }
    static pthread_mutex_t ui_state_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&ui_state_lock);

    if (!g_translator_hooked && is_touch_ui_enabled()) {
        uintptr_t *vt_slot = (uintptr_t *)(main_base + 0x20df7e8);
        if (*vt_slot == main_base + 0xc7651c) {
            *vt_slot = (uintptr_t)sky_scaleform_translate;
            g_translator_hooked = 1;
        }
    }

    pthread_mutex_lock(&g_host_state_lock);
    if (g_menu_pulse_frames > 0) {
        uint64_t t_ns = now_ns();
        if (t_ns - g_last_main_pulse_tick_ns >= 12000000ull) {
            g_last_main_pulse_tick_ns = t_ns;
            if (g_menu_pulse_frames > 1) {
                g_menu_pulse_frames--;
            } else if (t_ns >= g_menu_pulse_until_ns) {
                g_menu_pulse_frames = 0;
                g_menu_pulse_buttons = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_host_state_lock);

    /* Skip the Switch Joy-Con strap instruction screen after New Game/Continue:
     * firstExitedMenu (passed to StartMenu.SetFirstExitedMenu) makes the menu
     * bypass ShowJoyConStrapInstructionScreen. The strap countdown at +0x32a7c04
     * must stay untouched: MainMenu only calls ExitJoyConStrapInstructionScreen
     * (and continues starting the game) when it crosses zero. */
    static int no_firstexit_force = -1;
    if (no_firstexit_force < 0) no_firstexit_force = getenv("SKY_NO_FIRSTEXIT_FORCE") != NULL;
    if (!no_firstexit_force) *(uint8_t *)(main_base + 0x32a7c02) = 1;
    static int strap_active;
    float strap_timer = *(float *)(main_base + 0x32a7c04);
    if (!no_firstexit_force && strap_timer > 0.05f) {
        strap_timer = 0.01f;
        *(float *)(main_base + 0x32a7c04) = strap_timer;
    }
    if ((strap_timer > 0.0f) != strap_active) {
        strap_active = strap_timer > 0.0f;
        LOG("UI_JOYCON_STRAP active=%d timer=%.2f", strap_active, strap_timer);
    }

    void *main_menu = *(void **)(main_base + 0x32a7cf8);
    void *ui = *(void **)(main_base + 0x21ac858);
    int in_game = 0;
    void *top_menu = NULL;
    uintptr_t top_vt = 0;

    if (ui) {
        void **menus = *(void ***)((uintptr_t)ui + 0xf0);
        uint32_t count = *(uint32_t *)((uintptr_t)ui + 0xfc);
        uint32_t pauses = *(uint32_t *)((uintptr_t)ui + 0x138);
        uint32_t modals = *(uint32_t *)((uintptr_t)ui + 0x140);
        int has_hud = 0;
        int has_loading_menu = 0;
        int has_in_game_menu = 0;
        if (menus && count <= 64) {
            for (uint32_t i = 0; i < count; i++) {
                void *m = menus[i];
                if (!m) continue;
                uintptr_t vt = *(uintptr_t *)m - main_base;
                if (vt == 0x1a8fd90) {
                    has_hud = 1;
                } else if (vt == 0x1a906f8 || vt == 0x1a90bd8 || vt == 0x1a8f7c8) {
                    has_loading_menu = 1;
                } else if (vt != 0x1a8fa90 && vt != 0x1a91318 && vt != 0x1a8f8a0 &&
                           vt != 0x1a91660 && vt != 0x1a91778 && vt != 0x1a91da8 &&
                           vt != 0x1a91db0) {
                    has_in_game_menu = 1;
                }
                /* Track topmost interactive Scaleform menu for touch dispatch. */
                if (vt != 0x1a8fa90 && vt != 0x1a91318 && vt != 0x1a8fd90 &&
                    vt != 0x1a8f8a0 && vt != 0x1a906f8 && vt != 0x1a91db0 &&
                    vt != 0x1a91660 && vt != 0x1a91778 && vt != 0x1a91da8) {
                    top_menu = m;
                    top_vt = vt;
                }
            }
        }
        if (!top_menu && main_menu) {
            top_menu = main_menu;
            top_vt = 0x1a90bd8;
        }
        uint32_t draws = sky_last_frame_draw_count();
        if (sky_is_startup_logo_finished() && !main_menu && has_hud && !has_loading_menu) {
            in_game = (has_in_game_menu || pauses > 0 || modals > 0) ? 2 : 1;
        }
        if (in_game != g_last_logged_in_game) {
            g_last_logged_in_game = in_game;
            LOG("UI_IN_GAME state=%d draws=%u menus=%u pauses=%u modals=%u top_vt=%#llx",
                in_game, draws, count, pauses, modals, (unsigned long long)top_vt);
        }
        /* Log the menu stack whenever it changes (diagnoses blocking menus). */
        static uint64_t last_stack_sig;
        uint64_t sig = (uint64_t)count * 1000003u ^ (uint64_t)pauses << 40 ^ (uint64_t)modals << 48 ^
                       (main_menu ? 1ull << 63 : 0);
        char stack[64 * 11 + 1];
        size_t used = 0;
        stack[0] = 0;
        if (menus && count <= 64) {
            for (uint32_t i = 0; i < count; i++) {
                uintptr_t vt = menus[i] ? *(uintptr_t *)menus[i] - main_base : 0;
                sig = sig * 31 + vt;
                int n = snprintf(stack + used, sizeof stack - used, "%s%#lx", i ? "," : "", (unsigned long)vt);
                if (n > 0 && (size_t)n < sizeof stack - used) used += (size_t)n;
            }
        }
        if (sig != last_stack_sig) {
            static uint64_t first_ns;
            if (!first_ns) first_ns = now_ns();
            last_stack_sig = sig;
            LOG("UI_MENU_STACK t=%.1f main_menu=%d pauses=%u modals=%u menus=%u [%s]",
                (double)(now_ns() - first_ns) / 1e9, main_menu != NULL, pauses, modals, count, stack);
        }
    }
    sky_set_ui_in_game(in_game);
    pthread_mutex_unlock(&ui_state_lock);

    static int auto_continue_done;
    if (in_game == 1) {
        auto_continue_done = 1;
        static int no_force_controls = -1;
        if (no_force_controls < 0)
            no_force_controls = getenv("SKY_NO_FORCE_PLAYER_CONTROLS") != NULL;
        /* JournalMenu::bTabsDisabled (set by DisablePlayerControls with
         * abJournalTabs, e.g. during the Helgen intro) is only read when the
         * Journal opens, so it can be cleared from the worker thread that
         * polls input during gameplay. */
        uint8_t *journal_disabled = (uint8_t *)(main_base + 0x32aa190);
        if (!no_force_controls && *journal_disabled) {
            *journal_disabled = 0;
            LOG("JOURNAL_TABS_ENABLED");
        }
        if (sky_is_guest_main_thread()) {
            if (!no_force_controls) {
                void *control_map = *(void **)(main_base + 0x31b5f58);
                void *player_controls = *(void **)(main_base + 0x31b5f50);
                void *player_char = *(void **)(main_base + 0x3228940);
                uint32_t enabled = control_map ? *(uint32_t *)((uintptr_t)control_map + 0xf8) : 0x5c3u;
                uint8_t chargen = player_char ? *(uint8_t *)((uintptr_t)player_char + 0xab2) : 0;
                uint32_t pc_flags = player_char ? *(uint32_t *)((uintptr_t)player_char + 0xb78) : 0;
                if ((enabled & 0x5c3u) != 0x5c3u || *journal_disabled != 0 ||
                    (chargen & 0x7u) != 0 || (pc_flags & 0x80000u) != 0) {
                    typedef void (*toggle_controls_fn)(void *, uint32_t, uint32_t, uint32_t);
                    typedef void (*set_activate_fn)(void *, uint32_t);
                    typedef void (*set_ai_driven_fn)(void *, uint32_t);
                    if (control_map) {
                        toggle_controls_fn toggle = (toggle_controls_fn)(main_base + 0xad80ac);
                        toggle(control_map, 0xffffffffu, 1u, 1u);
                    }
                    if (player_controls) {
                        *(uint16_t *)((uintptr_t)player_controls + 0x48) = 0;
                        set_activate_fn set_act = (set_activate_fn)(main_base + 0x767100);
                        set_act(player_controls, 1u);
                    }
                    *journal_disabled = 0;
                    if (player_char) {
                        *(uint8_t *)((uintptr_t)player_char + 0xab2) = (uint8_t)(chargen & ~0x7u);
                        if (pc_flags & 0x80000u) {
                            set_ai_driven_fn set_ai = (set_ai_driven_fn)(main_base + 0x712820);
                            set_ai(player_char, 0u);
                        }
                    }
                    LOG("PLAYER_CONTROLS_ENABLED prev_controls=%#x prev_journal_dis=%u prev_chargen=%#x prev_pc_flags=%#x",
                        enabled, *journal_disabled, chargen, pc_flags);
                }
            }
        }
        pthread_mutex_lock(&g_host_state_lock);
        g_menu_touch_head = g_menu_touch_tail;
        g_menu_has_primary = 0;
        g_menu_active_fingers = 0;
        g_menu_dragged = 0;
        pthread_mutex_unlock(&g_host_state_lock);
        return;
    }
    if (!sky_is_guest_main_thread()) return;

    menu_touch_event_t events[MENU_TOUCH_QUEUE_CAP];
    unsigned event_count = 0;
    pthread_mutex_lock(&g_host_state_lock);
    while (g_menu_touch_head != g_menu_touch_tail && event_count < MENU_TOUCH_QUEUE_CAP) {
        events[event_count++] = g_menu_touches[g_menu_touch_head];
        g_menu_touch_head = (g_menu_touch_head + 1) % MENU_TOUCH_QUEUE_CAP;
    }
    pthread_mutex_unlock(&g_host_state_lock);

    void *movie = top_menu ? *(void **)((uintptr_t)top_menu + 0x10) : NULL;
    if (g_restore_platform_at_ns && now_ns() >= g_restore_platform_at_ns) {
        g_restore_platform_at_ns = 0;
        if (movie && movie == g_restore_platform_movie) {
            gfx_set_platform(main_base, movie, g_native_menu_platform);
            LOG("MENU_PLATFORM_RESTORED value=%.0f", g_native_menu_platform);
        }
        g_restore_platform_movie = NULL;
    }
    if (main_menu) {
        static int last_flags = -1;
        int flags = *(uint8_t *)((uintptr_t)main_menu + 0x65) | *(uint8_t *)((uintptr_t)main_menu + 0x66) << 8;
        if (flags != last_flags) {
            last_flags = flags;
            LOG("MAINMENU_FLAGS start_requested=%d press_start=%d", flags & 0xff, flags >> 8);
        }
        /* Trace StartMenu AS state transitions (diagnoses load fallbacks). */
        if (movie && top_vt == 0x1a90bd8) {
            static char last_as_state[64];
            char state_buf[64];
            const char *state_str = gfx_get_string_var(movie, "_root.MenuHolder.Menu_mc.strCurrentState",
                                                       state_buf, sizeof state_buf);
            if (state_str && strcmp(state_str, last_as_state)) {
                snprintf(last_as_state, sizeof last_as_state, "%s", state_str);
                LOG("MAINMENU_STATE state=%s", last_as_state);
            }
            static int no_auto_continue = -1;
            if (no_auto_continue < 0)
                no_auto_continue = getenv("SKY_NO_AUTO_CONTINUE") != NULL || getenv("SKY_AUTO_TAPS") != NULL;
            if (!no_auto_continue && !auto_continue_done && state_str && strap_timer <= 0.0f &&
                (!strcmp(state_str, "PressStart") || !strcmp(state_str, "Main") ||
                 !strcmp(state_str, "MainConfirm"))) {
                static uint64_t last_auto_ns;
                uint64_t t_ns = now_ns();
                pthread_mutex_lock(&g_host_state_lock);
                int can_pulse = (g_menu_pulse_frames == 0 && t_ns - last_auto_ns >= 350000000ull);
                if (can_pulse) {
                    last_auto_ns = t_ns;
                    queue_menu_pulse_locked(1ull << 0);
                    g_last_main_pulse_tick_ns = t_ns;
                    if (!strcmp(state_str, "MainConfirm")) auto_continue_done = 1;
                }
                pthread_mutex_unlock(&g_host_state_lock);
                if (can_pulse) LOG("MAINMENU_AUTO_CONTINUE state=%s", state_str);
            }
        }
    }

    if (!event_count) return;

    int press_start = 0;
    if (main_menu && top_vt == 0x1a90bd8) {
        uint8_t in_press_start = *(uint8_t *)((uintptr_t)main_menu + 0x66);
        char state_buf[64];
        const char *state_str = movie ? gfx_get_string_var(movie, "_root.MenuHolder.Menu_mc.strCurrentState",
                                                           state_buf, sizeof state_buf) : NULL;
        if (in_press_start != 0 || (state_str && !strcmp(state_str, "PressStart"))) {
            press_start = 1;
        }
    }

    for (unsigned i = 0; i < event_count; i++) {
        menu_touch_event_t ev = events[i];
        float tx = ev.nx, ty = ev.ny;
        gfx_adjust_menu_coords(top_vt, &tx, &ty);
        if (ev.phase == 0) {
            g_menu_active_fingers++;
            if (press_start) {
                pthread_mutex_lock(&g_host_state_lock);
                queue_menu_pulse_locked(1ull << 0);
                g_last_main_pulse_tick_ns = now_ns();
                pthread_mutex_unlock(&g_host_state_lock);
                LOG("MENU_TOUCH_PRESS_START nx=%.3f ny=%.3f", ev.nx, ev.ny);
                continue;
            }
            if (g_menu_active_fingers >= 2) {
                g_menu_dragged = 1;
                pthread_mutex_lock(&g_host_state_lock);
                queue_menu_pulse_locked(1ull << 1);
                g_last_main_pulse_tick_ns = now_ns();
                pthread_mutex_unlock(&g_host_state_lock);
                LOG("MENU_TOUCH_BACK two-finger-tap");
                continue;
            }
            g_menu_has_primary = 1;
            g_menu_primary_finger = ev.finger;
            g_menu_start_x = g_menu_step_x = ev.nx;
            g_menu_start_y = g_menu_step_y = ev.ny;
            g_menu_dragged = 0;
            if (movie && top_vt != 0x1a91f18) {
                if (isnan(g_native_menu_platform) && top_vt == 0x1a90bd8) {
                    g_native_menu_platform = gfx_get_number_var(movie, "_root.MenuHolder.Menu_mc.iPlatform");
                    LOG("MENU_NATIVE_PLATFORM value=%.0f", g_native_menu_platform);
                }
                gfx_set_platform(main_base, movie, 0.0);
                gfx_send_mouse_event(movie, 1, tx, ty);
            }
        } else if (ev.phase == 1) {
            if (!g_menu_has_primary || ev.finger != g_menu_primary_finger) continue;
            float dx = ev.nx - g_menu_step_x;
            float dy = ev.ny - g_menu_step_y;
            const float step = 0.055f;
            if (fabsf(dx) >= step || fabsf(dy) >= step) {
                g_menu_dragged = 1;
                if (movie)
                    gfx_set_platform(main_base, movie,
                                     isnan(g_native_menu_platform) ? 4.0 : g_native_menu_platform);
                uint64_t dpad = 0;
                if (fabsf(dy) >= fabsf(dx)) {
                    dpad = (dy < 0.f) ? (1ull << 13) : (1ull << 15);
                    g_menu_step_y = ev.ny;
                } else {
                    dpad = (dx < 0.f) ? (1ull << 12) : (1ull << 14);
                    g_menu_step_x = ev.nx;
                }
                pthread_mutex_lock(&g_host_state_lock);
                queue_menu_pulse_locked(dpad);
                g_last_main_pulse_tick_ns = now_ns();
                pthread_mutex_unlock(&g_host_state_lock);
            } else if (movie && !g_menu_dragged && top_vt != 0x1a91f18) {
                gfx_send_mouse_event(movie, 1, tx, ty);
            }
        } else if (ev.phase == 2 || ev.phase == 3) {
            if (g_menu_active_fingers > 0) g_menu_active_fingers--;
            if (!g_menu_has_primary || ev.finger != g_menu_primary_finger) continue;
            g_menu_has_primary = 0;
            if (ev.phase == 2 && !g_menu_dragged && !press_start) {
                char state_buf[64];
                const char *state_str = (top_vt == 0x1a90bd8 && movie) ?
                    gfx_get_string_var(movie, "_root.MenuHolder.Menu_mc.strCurrentState",
                                       state_buf, sizeof state_buf) : NULL;
                int was_confirming = (state_str && strstr(state_str, "Confirm") != NULL);
                uint64_t pulse = 0;
                if (movie && top_vt != 0x1a91f18) {
                    int was_two_panels =
                        (gfx_get_number_var(movie, "_root.Menu_mc.InventoryLists_mc.iCurrentState") == 2.0);
                    /* Confirm dialogs expose mouse Accept/Cancel buttons in mouse
                     * mode, so a click reaches the game's own handlers. */
                    gfx_set_platform(main_base, movie, 0.0);
                    gfx_send_mouse_event(movie, 1, tx, ty);
                    gfx_send_mouse_event(movie, 2, tx, ty);
                    gfx_send_mouse_event(movie, 3, tx, ty);
                    gfx_process_input(main_base, movie);
                    if (was_two_panels) {
                        /* ItemMenu subclasses (Inventory/Magic/Container) ignore
                         * mouse clicks in onItemSelect; invoke AttemptEquip(0)
                         * after ProcessInput updates Mouse.getTopMostEntity(). */
                        gfx_invoke_attempt_equip(main_base, movie, 0.0);
                    } else if (top_vt == 0x1a8fb18) {
                        /* FavoritesMenu ignores event.keyboardOrMouse == 0 in
                         * onItemSelect; invoking with no args passes undefined != 0. */
                        gfx_invoke_noarg(main_base, movie, "_root.MenuHolder.Menu_mc.onItemSelect");
                    } else if (top_vt == 0x1a8f9b8) {
                        /* DialogueMenu.onMouseDown skips every 2nd click
                         * (iMouseDownExecutionCount % 2 == 0); ensure selection runs. */
                        gfx_invoke_noarg(main_base, movie, "_root.DialogueMenu_mc.onItemSelect");
                    }
                    if (was_confirming && !isnan(g_native_menu_platform)) {
                        g_restore_platform_movie = movie;
                        g_restore_platform_at_ns = now_ns() + 150000000ull;
                    }
                }
                if (top_vt == 0x1a91508 && ev.ny > 0.86f && ev.nx > 0.65f) {
                    /* RaceSexMenu bottom-bar Done (X) prompt area. */
                    pulse = (1ull << 2);
                } else if (top_vt == 0x1a91f18) {
                    /* TutorialMenu is a modal popup over other menus (e.g. the
                     * Journal on first open); any tap dismisses it like B. */
                    pulse = (1ull << 1);
                } else if (top_vt == 0x1a918a0) {
                    /* StatsMenu uses 3D constellation rendering; tap selects skill/perk. */
                    pulse = (1ull << 0);
                } else if (top_vt == 0x1a90bd8 && was_confirming && ev.ny < 0.82f) {
                    pulse = (1ull << 0);
                } else if (top_vt == 0x1a90bd8 && !was_confirming && ev.nx < 0.50f) {
                    pulse = (1ull << 0);
                } else if (!movie) {
                    pulse = (1ull << 0);
                }
                if (pulse) {
                    pthread_mutex_lock(&g_host_state_lock);
                    queue_menu_pulse_locked(pulse);
                    g_last_main_pulse_tick_ns = now_ns();
                    pthread_mutex_unlock(&g_host_state_lock);
                }
                LOG("MENU_TOUCH_TAP vt=%#llx state=%s nx=%.3f ny=%.3f confirm=%d pulse=%#llx",
                    (unsigned long long)top_vt, state_str ? state_str : "?", ev.nx, ev.ny,
                    was_confirming, (unsigned long long)pulse);
            }
        }
    }
}

void sky_hid_controller_support_arg_set_default(void *arg) {
    if (arg) memset(arg, 0, 0x210);
    LOG("HID_CONTROLLER_SUPPORT_SET_DEFAULT host=single-player-fullkey");
}

uint32_t sky_hid_show_controller_support(sky_hid_controller_support_result_t *result, const void *arg) {
    (void)arg;
    if (result) {
        memset(result, 0, sizeof *result);
        result->player_count = 1;
        result->selected_id = 0;
    }
    sky_checkpoint("post_nnMain_hid_controller_support");
    LOG("HID_SHOW_CONTROLLER_SUPPORT action=skipped player_count=1 selected_id=0");
    return 0;
}

static void fill_state(sky_hid_npad_state_t *out, const uint32_t *id, uint32_t style) {
    if (!out) return;
    uintptr_t main_base = sky_main_module_base();
    int guest_ui = main_base && sky_profile_has_update_ui();
    if (style == 1u && id && *id == 0) update_guest_ui_and_menu_touch();
    memset(out, 0, sizeof *out);
    out->sampling = atomic_fetch_add(&g_sampling, 1) + 1;
    out->attributes = sky_hid_get_npad_style_set(id) == style ? 1u : 0u;
    if (out->attributes && style == 1u) {
        pthread_mutex_lock(&g_host_state_lock);
        const sky_hid_source_t priority[] = {SKY_HID_SOURCE_DESKTOP, SKY_HID_SOURCE_TOUCH, SKY_HID_SOURCE_EXTERNAL};
        int left_chosen = 0, right_chosen = 0;
        for (unsigned index = 0; index < HOST_INPUT_SOURCES; index++) {
            hid_source_state_t *state = &g_sources[priority[index]];
            const sky_hid_npad_state_t *held = &state->held;
            out->buttons |= held->buttons | state->pending_buttons;
            int left_held = held->lx || held->ly, right_held = held->rx || held->ry;
            int32_t lx = left_held ? held->lx : state->pending_lx;
            int32_t ly = left_held ? held->ly : state->pending_ly;
            int32_t rx = right_held ? held->rx : state->pending_rx;
            int32_t ry = right_held ? held->ry : state->pending_ry;
            if (!left_chosen && (lx || ly)) { out->lx = lx; out->ly = ly; left_chosen = 1; }
            if (!right_chosen && (rx || ry)) { out->rx = rx; out->ry = ry; right_chosen = 1; }
            /* Connected sampling consumes even impulses hidden by a higher
             * priority source, so they cannot replay after it releases. */
            state->pending_buttons = 0;
            state->pending_lx = state->pending_ly = state->pending_rx = state->pending_ry = 0;
        }
        if (g_menu_pulse_frames > 0) {
            if (guest_ui && now_ns() >= g_menu_pulse_until_ns + 100000000ull) {
                g_menu_pulse_frames = 0;
                g_menu_pulse_buttons = 0;
            } else {
                out->buttons |= g_menu_pulse_buttons;
                if (!guest_ui && --g_menu_pulse_frames == 0) g_menu_pulse_buttons = 0;
            }
        }
        pthread_mutex_unlock(&g_host_state_lock);
    }
    sky_checkpoint("post_nnMain_hid_state");
    if (atomic_exchange(&g_state_logged, 1) == 0) {
        LOG("HID_STATE sampling=%lld buttons=%#llx sticks=%d attributes=%u",
            (long long)out->sampling, (unsigned long long)out->buttons,
            !!(out->lx || out->ly || out->rx || out->ry), out->attributes);
    }
}

void sky_hid_get_full_key_state(sky_hid_npad_state_t *out, const uint32_t *id) {
    fill_state(out, id, 1u);
    if (!out) return;
    uint32_t player = id ? *id : UINT32_MAX;
    unsigned index = player < 8 ? player : player == 32 ? 8 : 9;
    sky_hid_npad_state_t snapshot = *out;
    snapshot.sampling = 0;
    pthread_mutex_lock(&g_host_state_lock);
    hid_read_diagnostic_t *diagnostic = &g_full_key_diagnostics[index];
    uint64_t reads = ++diagnostic->reads;
    int changed = !diagnostic->seen || diagnostic->id != player ||
        memcmp(&diagnostic->state, &snapshot, sizeof snapshot);
    uint64_t transitions = diagnostic->transitions;
    if (changed) {
        transitions = ++diagnostic->transitions;
        diagnostic->state = snapshot;
        diagnostic->id = player;
        diagnostic->seen = 1;
    }
    int log_transition = changed && (transitions <= 32 || !(transitions & (transitions - 1)));
    int log_poll = !changed && !snapshot.buttons && !(reads & (reads - 1));
    if (log_transition || log_poll)
        LOG("HID_FULLKEY_STATE reason=%s transition=%llu reads=%llu at_ns=%llu id=%#x style=%#x connected=%u buttons=%#llx sticks=%d,%d,%d,%d sampling=%lld",
            changed ? "transition" : "poll", (unsigned long long)transitions,
            (unsigned long long)reads, (unsigned long long)now_ns(), player,
            sky_hid_get_npad_style_set(id), snapshot.attributes,
            (unsigned long long)snapshot.buttons, snapshot.lx, snapshot.ly,
            snapshot.rx, snapshot.ry, (long long)out->sampling);
    pthread_mutex_unlock(&g_host_state_lock);
}

void sky_hid_get_joy_dual_state(sky_hid_npad_state_t *out, const uint32_t *id) {
    fill_state(out, id, 1u << 2);
}

void sky_hid_get_handheld_state(sky_hid_npad_state_t *out, const uint32_t *id) {
    fill_state(out, id, 1u << 1);
}

SKY_FORCE("_ZN2nn3hid14InitializeNpadEv", "Initialize Npad with a host-owned neutral state and no shared-memory IPC",
          uint32_t, sky_hid_initialize_entry, void) { return sky_hid_initialize_npad(); }
SKY_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "The host supports one neutral full-key controller",
          void, sky_hid_set_styles_entry, uint32_t styles) { sky_hid_set_supported_npad_style_set(styles); }
SKY_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "The host has one fixed neutral controller ID",
          void, sky_hid_set_ids_entry, const uint32_t *ids, uint64_t count) { sky_hid_set_supported_npad_id_type(ids, count); }
SKY_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "The neutral full-key controller has no hold-type-dependent input",
          void, sky_hid_set_hold_type_entry, int type) { sky_hid_set_npad_joy_hold_type(type); }
SKY_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "ID zero is the host's neutral full-key controller",
          uint32_t, sky_hid_get_style_entry, const uint32_t *id) {
    update_guest_ui_and_menu_touch();
    uint32_t style = sky_hid_get_npad_style_set(id);
    static _Atomic uint64_t queries;
    uint64_t count = atomic_fetch_add(&queries, 1) + 1;
    if (count <= 16 || !(count & (count - 1)))
        LOG("HID_STYLE_QUERY count=%llu id=%#x style=%#x at_ns=%llu",
            (unsigned long long)count, id ? *id : UINT32_MAX, style, (unsigned long long)now_ns());
    return style;
}
SKY_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "Return the host full-key input state in Skyrim's 0x28-byte layout",
          void, sky_hid_get_full_key_entry, sky_hid_npad_state_t *out, const uint32_t *id) {
    sky_hid_get_full_key_state(out, id);
}
SKY_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", "No Joy-Dual controller is connected",
          void, sky_hid_get_joy_dual_entry, sky_hid_npad_state_t *out, const uint32_t *id) {
    sky_hid_get_joy_dual_state(out, id);
}
SKY_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "No handheld controller is connected",
          void, sky_hid_get_handheld_entry, sky_hid_npad_state_t *out, const uint32_t *id) {
    sky_hid_get_handheld_state(out, id);
}

SKY_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "Expose the stationary host FullKey controller's canonical motion handle",
          int, sky_hid_six_axis_handles_entry, uint32_t *out, int maximum, const uint32_t *id, uint32_t styles) {
    return sky_hid_get_six_axis_handles(out, maximum, id, styles);
}
SKY_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "Start the host controller's stationary motion samples without a HID IPC session",
          void, sky_hid_six_axis_start_entry, const uint32_t *handle) {
    if (sky_hid_start_six_axis_sensor(handle)) die("invalid host six-axis sensor handle");
}
SKY_FORCE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", "Stop host motion samples without a HID IPC session",
          void, sky_hid_six_axis_stop_entry, const uint32_t *handle) {
    if (sky_hid_stop_six_axis_sensor(handle)) die("invalid host six-axis sensor handle");
}
SKY_FORCE("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", "The host keyboard controller has no motion input",
          int, sky_hid_six_axis_at_rest_entry, const uint32_t *handle) {
    return sky_hid_is_six_axis_sensor_at_rest(handle);
}
SKY_FORCE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", "Return stationary gravity and identity orientation in the 0x60-byte motion state",
          void, sky_hid_six_axis_state_entry, sky_hid_six_axis_state_t *out, const uint32_t *handle) {
    sky_hid_get_six_axis_state(out, handle);
}
SKY_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "Return the newest host motion sample, with no fabricated history",
          int, sky_hid_six_axis_states_entry, sky_hid_six_axis_state_t *out, int maximum, const uint32_t *handle) {
    return sky_hid_get_six_axis_states(out, maximum, handle);
}

SKY_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "Return the host FullKey controller's canonical haptic handles",
          int, sky_hid_vibration_handles_entry, uint32_t *out, int maximum, const uint32_t *id, uint32_t styles) {
    return sky_hid_get_vibration_handles(out, maximum, id, styles);
}
SKY_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "Initialize host virtual haptic state without opening HID IPC",
          void, sky_hid_vibration_initialize_entry, const uint32_t *handle) {
    if (sky_hid_initialize_vibration_device(handle)) die("invalid host vibration device handle");
}
SKY_FORCE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", "The host keyboard controller has no physical vibration device",
          void, sky_hid_vibration_info_entry, sky_hid_vibration_info_t *out, const uint32_t *handle) {
    if (sky_hid_get_vibration_info(out, handle)) die("invalid host vibration info request");
}
SKY_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "Accept virtual haptic requests without claiming physical motor output",
          void, sky_hid_vibration_send_entry, const uint32_t *handle, const sky_hid_vibration_value_t *value) {
    if (sky_hid_send_vibration_value(handle, value)) die("invalid host vibration value request");
}
SKY_FORCE("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", "No physical motor is attached, so actual vibration amplitudes are zero",
          void, sky_hid_vibration_actual_entry, sky_hid_vibration_value_t *out, const uint32_t *handle) {
    if (sky_hid_get_actual_vibration_value(out, handle)) die("invalid host vibration value request");
}
SKY_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "No separate Switch USB keyboard is attached to the FullKey host profile",
          void, sky_hid_keyboard_state_entry, sky_hid_keyboard_state_t *out) {
    sky_hid_get_keyboard_state(out);
}
SKY_FORCE("_ZN2nn3hid20ControllerSupportArg10SetDefaultEv", "Initialize controller support arguments without HID applet state",
          void, sky_hid_controller_support_set_default_entry, void *arg) {
    sky_hid_controller_support_arg_set_default(arg);
}
SKY_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "Skip the Switch controller support applet and keep player 0 attached",
          uint32_t, sky_hid_show_controller_support_entry, sky_hid_controller_support_result_t *result, const void *arg) {
    return sky_hid_show_controller_support(result, arg);
}
