#pragma once

#include <stdint.h>

typedef struct { uint32_t magic; } sky_vi_display_t;
typedef struct { uint32_t magic; } sky_vi_layer_t;
typedef struct { uint32_t magic; sky_vi_layer_t *layer; } sky_vi_native_window_t;

void sky_vi_initialize(void);
uint32_t sky_vi_open_default_display(sky_vi_display_t **out);
uint32_t sky_vi_create_layer(sky_vi_layer_t **out, const sky_vi_display_t *display);
uint32_t sky_vi_get_native_window(sky_vi_native_window_t **out, const sky_vi_layer_t *layer);
