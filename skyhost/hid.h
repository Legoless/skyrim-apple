#pragma once

#include <stdint.h>

typedef struct {
    int64_t sampling;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes;
    uint32_t padding;
} sky_hid_npad_state_t;

typedef struct {
    uint64_t delta_time, sampling;
    float acceleration[3], angular_velocity[3], angle[3], direction[9];
    uint32_t attributes, reserved;
} sky_hid_six_axis_state_t;

typedef struct { uint32_t type, position; } sky_hid_vibration_info_t;
typedef struct { float amplitude_low, frequency_low, amplitude_high, frequency_high; } sky_hid_vibration_value_t;
typedef struct { uint64_t sampling, modifiers, keys[4]; } sky_hid_keyboard_state_t;

typedef enum {
    SKY_HID_SOURCE_EXTERNAL = 0,
    SKY_HID_SOURCE_DESKTOP = 1,
    SKY_HID_SOURCE_TOUCH = 2,
} sky_hid_source_t;

/* Raw updates clear only this source's pending input. Queued updates preserve
 * edges and short stick taps until a connected FullKey read. Buttons combine
 * across sources; each stick independently prefers Desktop, Touch, External.
 * Values remain int32_t here; input models perform any device-range clamping. */
void sky_hid_set_source_state(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void sky_hid_queue_source_state(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void sky_hid_queue_source_tap(sky_hid_source_t source, uint64_t buttons,
    int32_t lx, int32_t ly, int32_t rx, int32_t ry, uint64_t tap_buttons);

uint32_t sky_hid_initialize_npad(void);
void sky_hid_set_host_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void sky_hid_queue_host_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void sky_hid_queue_host_tap(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry,
                            uint64_t tap_buttons);
void sky_hid_set_supported_npad_style_set(uint32_t styles);
void sky_hid_set_supported_npad_id_type(const uint32_t *ids, uint64_t count);
void sky_hid_set_npad_joy_hold_type(int type);
uint32_t sky_hid_get_npad_style_set(const uint32_t *id);
void sky_hid_get_full_key_state(sky_hid_npad_state_t *out, const uint32_t *id);
void sky_hid_get_joy_dual_state(sky_hid_npad_state_t *out, const uint32_t *id);
void sky_hid_get_handheld_state(sky_hid_npad_state_t *out, const uint32_t *id);
int sky_hid_get_six_axis_handles(uint32_t *out, int maximum, const uint32_t *id, uint32_t styles);
uint32_t sky_hid_start_six_axis_sensor(const uint32_t *handle);
uint32_t sky_hid_stop_six_axis_sensor(const uint32_t *handle);
int sky_hid_is_six_axis_sensor_at_rest(const uint32_t *handle);
int sky_hid_get_six_axis_states(sky_hid_six_axis_state_t *out, int maximum, const uint32_t *handle);
void sky_hid_get_six_axis_state(sky_hid_six_axis_state_t *out, const uint32_t *handle);
int sky_hid_get_vibration_handles(uint32_t *out, int maximum, const uint32_t *id, uint32_t styles);
uint32_t sky_hid_initialize_vibration_device(const uint32_t *handle);
uint32_t sky_hid_get_vibration_info(sky_hid_vibration_info_t *out, const uint32_t *handle);
uint32_t sky_hid_send_vibration_value(const uint32_t *handle, const sky_hid_vibration_value_t *value);
uint32_t sky_hid_get_actual_vibration_value(sky_hid_vibration_value_t *out, const uint32_t *handle);
void sky_hid_get_keyboard_state(sky_hid_keyboard_state_t *out);

typedef struct {
    int8_t player_count;
    uint8_t padding[3];
    int32_t selected_id;
} sky_hid_controller_support_result_t;

void sky_hid_controller_support_arg_set_default(void *arg);
uint32_t sky_hid_show_controller_support(sky_hid_controller_support_result_t *result, const void *arg);
void sky_hid_menu_touch(unsigned phase, uint64_t finger, float nx, float ny);
