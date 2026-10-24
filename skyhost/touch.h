#pragma once

#include <stddef.h>
#include <stdint.h>

/* UIKit coordinates are points, with y increasing down. Sticks use Switch's
 * positive-up y axis. Layout and touch ownership contain no UIKit or HID code. */
typedef enum {
    SKY_TOUCH_MOVE, SKY_TOUCH_LOOK,
    SKY_TOUCH_USE, SKY_TOUCH_BACK, SKY_TOUCH_JUMP, SKY_TOUCH_SNEAK,
    SKY_TOUCH_ATTACK, SKY_TOUCH_PAUSE,
    SKY_TOUCH_CONTROL_COUNT
} sky_touch_control_t;

typedef struct { float top, left, bottom, right; } sky_touch_insets_t;
typedef struct {
    float x, y, radius;
    uint64_t button;
    const char *label;
} sky_touch_region_t;
typedef struct {
    uint64_t id;
    float x, y;
    unsigned used, inside;
    sky_touch_control_t control;
} sky_touch_finger_t;

#define SKY_TOUCH_MAX_FINGERS 16
#define SKY_TOUCH_DEAD_ZONE 0.16f
/* Held whenever Move is deflected, except while Sneak is toggled on:
 * sprinting would make the player stand up. */
#define SKY_TOUCH_SPRINT_BUTTON (UINT64_C(1) << 6)

typedef struct {
    sky_touch_region_t regions[SKY_TOUCH_CONTROL_COUNT];
    sky_touch_finger_t fingers[SKY_TOUCH_MAX_FINGERS];
    struct { float x, y; } stick_home[SKY_TOUCH_LOOK + 1];
    float width, height;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    unsigned enabled, menu, sneaking;
} sky_touch_t;

/* A resize, including a safe-area change, cancels every touch. Invalid geometry
 * disables hit testing until the next valid resize. Zero initialization is valid.
 * Buttons win hit testing; any other touch on the left half drives Move and on
 * the right half drives Look, with the stick centered where the finger landed.
 * An idle stick's region is its resting place. */
void sky_touch_resize(sky_touch_t *touch, float width, float height, sky_touch_insets_t safe);
/* Mode 0 exposes gameplay, 1 only Back, and 2 all controller controls. Back
 * shares Pause's position in modes 0/1 and sits to its right in mode 2. */
void sky_touch_set_menu(sky_touch_t *touch, int menu);
int sky_touch_begin(sky_touch_t *touch, uint64_t finger, float x, float y);
void sky_touch_move(sky_touch_t *touch, uint64_t finger, float x, float y);
void sky_touch_end(sky_touch_t *touch, uint64_t finger);
void sky_touch_cancel(sky_touch_t *touch);
void sky_touch_state(const sky_touch_t *touch, uint64_t *buttons,
    int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
int sky_touch_hit_test(const sky_touch_t *touch, float x, float y);
const sky_touch_region_t *sky_touch_region(const sky_touch_t *touch, sky_touch_control_t control);
int sky_touch_control_active(const sky_touch_t *touch, sky_touch_control_t control);
