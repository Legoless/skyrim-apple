#include "vi.h"
#include "skyhost.h"

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

static sky_vi_display_t g_display = {0x50534944};
static sky_vi_layer_t g_layer = {0x5259414c};
static sky_vi_native_window_t g_native = {0x4e49574e, &g_layer};

void sky_vi_initialize(void) {
    sky_checkpoint("post_nnMain_vi_initialize");
    LOG("VI_INITIALIZE mode=offscreen no-window");
}

uint32_t sky_vi_open_default_display(sky_vi_display_t **out) {
    if (!out) return 0x2A2;
    *out = &g_display;
    sky_checkpoint("post_nnMain_vi_display");
    LOG("VI_OPEN_DEFAULT_DISPLAY display=%p", (void *)*out);
    return 0;
}

uint32_t sky_vi_create_layer(sky_vi_layer_t **out, const sky_vi_display_t *display) {
    if (!out || display != &g_display) return 0x2A2;
    *out = &g_layer;
    sky_checkpoint("post_nnMain_vi_layer");
    LOG("VI_CREATE_LAYER layer=%p", (void *)*out);
    return 0;
}

uint32_t sky_vi_get_native_window(sky_vi_native_window_t **out, const sky_vi_layer_t *layer) {
    if (!out || layer != &g_layer) return 0x2A2;
    *out = &g_native;
    sky_checkpoint("post_nnMain_vi_native_window");
    LOG("VI_NATIVE_WINDOW native=%p", (void *)*out);
    return 0;
}

SKY_FORCE("_ZN2nn2vi10InitializeEv", "Initialize VI with an offscreen host display",
          void, sky_vi_initialize_entry, void) { sky_vi_initialize(); }
SKY_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "Open the host-owned default display",
          uint32_t, sky_vi_open_display_entry, sky_vi_display_t **out) { return sky_vi_open_default_display(out); }
SKY_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "Create a layer on the host default display",
          uint32_t, sky_vi_create_layer_entry, sky_vi_layer_t **out, const sky_vi_display_t *display) {
    return sky_vi_create_layer(out, display);
}
SKY_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "Return the offscreen native window associated with the layer",
          uint32_t, sky_vi_native_window_entry, sky_vi_native_window_t **out, const sky_vi_layer_t *layer) {
    return sky_vi_get_native_window(out, layer);
}
