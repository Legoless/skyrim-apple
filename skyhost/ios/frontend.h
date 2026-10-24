#pragma once
#include "../keyboard.h"

/* UIKit owns the main thread. The game entry runs once on an 8 MiB pthread;
 * call this only from the app's main entry point. UIApplicationMain normally
 * owns the process until iOS terminates the app. */
int sky_ios_run(int (*run_guest)(void));
void *sky_ios_metal_layer(void);
/* Call before sky_ios_run to show setup instructions instead of starting the guest. */
void sky_ios_set_setup_message(const char *message);
/* A setup check returns NULL when files are ready, otherwise a UTF-8 message.
 * The returned message must remain valid until the frontend copies it immediately. */
void sky_ios_set_setup_check(const char *(*check)(void));
int sky_ios_retry_setup(void);
int sky_ios_present_keyboard(const sky_keyboard_request_t *request, char *out_utf8, size_t capacity);
/* Main-thread diagnostic hooks; they use the real native dialog and validation. */
int sky_ios_keyboard_is_presented(void);
int sky_ios_keyboard_respond(const char *text_utf8, int accepted);

/* Optional in-process simulator/UI-test hooks. Main thread only. Coordinates
 * are overlay points, not Metal pixels. phase: begin=0, move=1, end=2, cancel=3. */
int sky_ios_touch_inject(unsigned phase, unsigned long long finger, float x, float y);
struct sky_ios_touch_snapshot {
    unsigned long long buttons;
    int lx, ly, rx, ry;
};
int sky_ios_touch_snapshot(struct sky_ios_touch_snapshot *out);
