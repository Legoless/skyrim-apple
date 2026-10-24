#pragma once

#include <stdint.h>

enum {
    SKY_INPUT_MOD_SHIFT = 1u << 0,
    SKY_INPUT_MOD_CTRL = 1u << 1,
    SKY_INPUT_MOD_ALT = 1u << 2,
    SKY_INPUT_MOD_COMMAND = 1u << 3,
};

/* Stick units per pixel moved in one 60 Hz input interval. */
#define SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT 1600.0
#define SKY_INPUT_MOUSE_IDLE_NS UINT64_C(50000000)
#define SKY_INPUT_SCROLL_HOLD_NS UINT64_C(100000000)

typedef struct {
    uint8_t keys[128];
    uint64_t mouse_buttons;
    uint64_t mouse_button_bindings[3];
    uint8_t mouse_button_down[3];
    uint8_t escape_button;
    uint8_t mouse_captured;
    uint8_t have_mouse_time;
    uint8_t have_tick_time;
    uint8_t scroll_button;
    uint8_t have_scroll_time;
    unsigned modifiers;
    double mouse_sensitivity;
    double mouse_dx, mouse_dy;
    int32_t mouse_rx, mouse_ry;
    uint64_t mouse_time_ns, tick_time_ns;
    double scroll_lines;
    uint64_t scroll_time_ns, scroll_event_time_ns;
} sky_input_t;

/* All-zero initialization selects the defaults. Reset releases capture and all
   controls, retaining only a valid configured mouse sensitivity. */
void sky_input_reset(sky_input_t *input);
void sky_input_key(sky_input_t *input, unsigned keycode, int down);
void sky_input_set_modifiers(sky_input_t *input, unsigned modifiers);
void sky_input_set_mouse_captured(sky_input_t *input, int captured);
/* Native button numbers: 0 left, 1 right, 2 middle. Capture is latched on press
   so releasing a button after a capture change always releases the same action. */
void sky_input_mouse_button(sky_input_t *input, unsigned button, int down);
void sky_input_set_mouse_sensitivity(sky_input_t *input, double sensitivity);
void sky_input_mouse_motion(sky_input_t *input, double dx, double dy,
                            uint64_t now_ns);
/* Positive logical lines navigate up, negative lines down. A full accumulated
   line presses D-pad Up/Down for 100 ms; same-direction motion extends that
   hold. Opposite motion starts a fresh fractional count before taking over. */
void sky_input_scroll(sky_input_t *input, double logical_lines, uint64_t now_ns);
/* Call on a monotonic clock at approximately 60 Hz, including while idle.
   Accumulated relative motion is converted to an interval-normalized right
   stick sample. No motion on the following tick releases the stick; queued
   motion older than 50 ms is discarded. */
void sky_input_tick(sky_input_t *input, uint64_t now_ns);
void sky_input_state(const sky_input_t *input, uint64_t *buttons,
                     int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
