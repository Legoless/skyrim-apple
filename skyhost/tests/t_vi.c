#include "vi.h"
#include "skyhost.h"
#include <assert.h>

int main(void) {
    sky_vi_initialize();
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_vi_initialize"));
    sky_vi_display_t *display = NULL;
    assert(sky_vi_open_default_display(&display) == 0 && display);
    sky_vi_layer_t *layer = NULL;
    assert(sky_vi_create_layer(&layer, display) == 0 && layer);
    sky_vi_native_window_t *native = NULL;
    assert(sky_vi_get_native_window(&native, layer) == 0 && native && native->layer == layer);
    assert(sky_vi_create_layer(&layer, (sky_vi_display_t *)1) != 0);
    assert(sky_vi_get_native_window(&native, (sky_vi_layer_t *)1) != 0);
    puts("T_VI OK display-layer-native-offscreen");
    return 0;
}
