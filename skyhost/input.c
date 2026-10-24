#include "input.h"
#include <math.h>
#include <string.h>

#define MOUSE_DELTA_LIMIT 4096.0
#define INPUT_INTERVAL_NS (1000000000.0 / 60.0)

static double sensitivity(const sky_input_t *input) {
    double value = input->mouse_sensitivity;
    return isfinite(value) && value > 0 ? value : SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT;
}

static void clear_mouse_motion(sky_input_t *input) {
    input->mouse_dx = input->mouse_dy = 0;
    input->mouse_rx = input->mouse_ry = 0;
    input->have_mouse_time = input->have_tick_time = 0;
}

static void clear_scroll(sky_input_t *input) {
    input->scroll_button = input->have_scroll_time = 0;
    input->scroll_lines = 0;
    input->scroll_time_ns = input->scroll_event_time_ns = 0;
}

void sky_input_reset(sky_input_t *input) {
    double value = sensitivity(input);
    memset(input, 0, sizeof *input);
    input->mouse_sensitivity = value;
}

void sky_input_key(sky_input_t *input, unsigned keycode, int down) {
    if (keycode >= sizeof input->keys) return;
    if (down && (input->modifiers & SKY_INPUT_MOD_COMMAND)) return;
    if (keycode == 53 && down && !input->keys[keycode])
        input->escape_button = input->mouse_captured ? 10 : 1;
    input->keys[keycode] = down != 0;
}

void sky_input_set_modifiers(sky_input_t *input, unsigned modifiers) {
    const unsigned known = SKY_INPUT_MOD_SHIFT | SKY_INPUT_MOD_CTRL |
        SKY_INPUT_MOD_ALT | SKY_INPUT_MOD_COMMAND;
    input->modifiers = modifiers & known;
    if (input->modifiers & SKY_INPUT_MOD_COMMAND) {
        /* Command shortcuts belong to AppKit. Discard held controls as well as
           new key presses so a shortcut cannot leave a game action latched. */
        memset(input->keys, 0, sizeof input->keys);
        memset(input->mouse_button_down, 0, sizeof input->mouse_button_down);
        memset(input->mouse_button_bindings, 0, sizeof input->mouse_button_bindings);
        input->mouse_buttons = 0;
        clear_mouse_motion(input);
        clear_scroll(input);
    }
}

void sky_input_set_mouse_captured(sky_input_t *input, int captured) {
    captured = captured != 0;
    if (input->mouse_captured != captured) clear_mouse_motion(input);
    input->mouse_captured = captured;
}

void sky_input_mouse_button(sky_input_t *input, unsigned button, int down) {
    if (button >= sizeof input->mouse_button_down) return;
    if (down && (input->modifiers & SKY_INPUT_MOD_COMMAND)) return;
    if (down && !input->mouse_button_down[button]) {
        static const uint8_t free_buttons[] = {0, 1, 5};
        static const uint8_t captured_buttons[] = {9, 8, 5};
        unsigned action = input->mouse_captured ? captured_buttons[button] : free_buttons[button];
        input->mouse_button_bindings[button] = UINT64_C(1) << action;
    }
    input->mouse_button_down[button] = down != 0;
    input->mouse_buttons = 0;
    for (unsigned i = 0; i < sizeof input->mouse_button_down; i++)
        if (input->mouse_button_down[i]) input->mouse_buttons |= input->mouse_button_bindings[i];
}

void sky_input_set_mouse_sensitivity(sky_input_t *input, double value) {
    input->mouse_sensitivity = isfinite(value) && value > 0 ? value : SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT;
}

static double bounded_delta(double value) {
    if (value > MOUSE_DELTA_LIMIT) return MOUSE_DELTA_LIMIT;
    if (value < -MOUSE_DELTA_LIMIT) return -MOUSE_DELTA_LIMIT;
    return value;
}

void sky_input_mouse_motion(sky_input_t *input, double dx, double dy, uint64_t now_ns) {
    if (!input->mouse_captured || (input->modifiers & SKY_INPUT_MOD_COMMAND) ||
        !isfinite(dx) || !isfinite(dy)) return;
    if (input->have_mouse_time && now_ns < input->mouse_time_ns) return;
    if (input->have_mouse_time && now_ns - input->mouse_time_ns > SKY_INPUT_MOUSE_IDLE_NS)
        input->mouse_dx = input->mouse_dy = 0;
    input->mouse_dx = bounded_delta(input->mouse_dx + bounded_delta(dx));
    input->mouse_dy = bounded_delta(input->mouse_dy + bounded_delta(dy));
    input->mouse_time_ns = now_ns;
    input->have_mouse_time = 1;
}

static int32_t stick_value(double value) {
    if (value >= 32767) return 32767;
    if (value <= -32767) return -32767;
    return (int32_t)(value + (value < 0 ? -0.5 : 0.5));
}

void sky_input_scroll(sky_input_t *input, double lines, uint64_t now_ns) {
    if (!isfinite(lines) || lines == 0 || (input->modifiers & SKY_INPUT_MOD_COMMAND)) return;
    if (input->have_scroll_time && now_ns < input->scroll_event_time_ns) return;
    input->scroll_event_time_ns = now_ns;
    input->have_scroll_time = 1;
    unsigned button = lines > 0 ? 13 : 15;
    if (input->scroll_button == button) input->scroll_time_ns = now_ns;
    if ((lines > 0 && input->scroll_lines < 0) || (lines < 0 && input->scroll_lines > 0))
        input->scroll_lines = 0;
    if (lines > 64) lines = 64;
    if (lines < -64) lines = -64;
    input->scroll_lines += lines;
    if (input->scroll_lines >= 1 || input->scroll_lines <= -1) {
        input->scroll_button = button;
        input->scroll_time_ns = now_ns;
        /* At most one direction is held, even for a large wheel event. Retain
           only the fraction, so a burst cannot queue minutes of menu repeats. */
        input->scroll_lines -= (int)input->scroll_lines;
    }
}

void sky_input_tick(sky_input_t *input, uint64_t now_ns) {
    if (input->scroll_button && now_ns >= input->scroll_time_ns &&
        now_ns - input->scroll_time_ns >= SKY_INPUT_SCROLL_HOLD_NS)
        input->scroll_button = 0;
    if (input->have_tick_time && now_ns <= input->tick_time_ns) {
        if (now_ns < input->tick_time_ns) {
            clear_mouse_motion(input);
            clear_scroll(input);
        }
        return;
    }
    double interval = input->have_tick_time ? (double)(now_ns - input->tick_time_ns) : INPUT_INTERVAL_NS;
    input->tick_time_ns = now_ns;
    input->have_tick_time = 1;
    input->mouse_rx = input->mouse_ry = 0;
    if (input->mouse_captured && !(input->modifiers & SKY_INPUT_MOD_COMMAND) &&
        input->have_mouse_time && now_ns >= input->mouse_time_ns &&
        now_ns - input->mouse_time_ns <= SKY_INPUT_MOUSE_IDLE_NS) {
        double rate = INPUT_INTERVAL_NS / interval;
        input->mouse_rx = stick_value((input->mouse_dx * rate) * sensitivity(input));
        input->mouse_ry = stick_value((-input->mouse_dy * rate) * sensitivity(input));
    }
    input->mouse_dx = input->mouse_dy = 0;
}

void sky_input_state(const sky_input_t *input, uint64_t *buttons,
                     int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    static const struct { uint8_t key, button; } bindings[] = {
        {36, 0}, {14, 0},              /* Return / E: A (confirm, interact) */
        {48, 1},                       /* Tab: B (character menu, back) */
        {49, 2},                       /* Space: X (jump) */
        {15, 3},                       /* R: Y (ready weapon) */
        {8, 4}, {3, 5},                /* C / F: stick clicks (sneak / POV) */
        {6, 7},                        /* Z: R (shout) */
        {20, 8}, {21, 9},              /* 3 / 4: triggers */
        {24, 10}, {27, 11}, {17, 11},  /* = / - / T: journal / wait */
        {18, 12}, {12, 13}, {19, 14},  /* 1 / Q / 2: favorites */
        {123, 12}, {126, 13}, {124, 14}, {125, 15}, /* D-pad */
    };
    *buttons = input->mouse_buttons;
    if (input->modifiers & SKY_INPUT_MOD_COMMAND) {
        *buttons = 0;
        *lx = *ly = *rx = *ry = 0;
        return;
    }
    for (unsigned i = 0; i < sizeof bindings / sizeof bindings[0]; i++)
        if (input->keys[bindings[i].key]) *buttons |= UINT64_C(1) << bindings[i].button;
    if (input->scroll_button) *buttons |= UINT64_C(1) << input->scroll_button;
    if (input->keys[53]) *buttons |= UINT64_C(1) << input->escape_button;
    if (input->modifiers & SKY_INPUT_MOD_CTRL) *buttons |= UINT64_C(1) << 4;
    if (input->modifiers & SKY_INPUT_MOD_SHIFT) *buttons |= UINT64_C(1) << 6;
    *lx = (input->keys[2] - input->keys[0]) * 32767; /* D - A */
    *ly = (input->keys[13] - input->keys[1]) * 32767; /* W - S */
    if (input->keys[37] || input->keys[38] || input->keys[34] || input->keys[40]) {
        *rx = (input->keys[37] - input->keys[38]) * 32767; /* L - J */
        *ry = (input->keys[34] - input->keys[40]) * 32767; /* I - K */
    } else {
        *rx = input->mouse_rx;
        *ry = input->mouse_ry;
    }
}
