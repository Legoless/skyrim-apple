#pragma once

#include <stddef.h>
#include <stdint.h>

#define SKY_INPUT_CONTROL_LINE_MAX 256

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
} sky_input_control_t;

enum {
    SKY_INPUT_CONTROL_INVALID = -1,
    SKY_INPUT_CONTROL_APPLIED = 0,
    SKY_INPUT_CONTROL_QUIT = 1,
};

/* Masks are decimal or 0x-prefixed; stick values are decimal. A malformed
 * command resets held input and pending edges. The caller owns this state. */
int sky_input_control_apply(sky_input_control_t *control, const char *line, size_t length);
/* Read newline commands until EOF or quit. The fd remains owned by the caller.
 * EOF clears input but never invokes on_quit. */
void sky_input_control_read(int fd, void (*on_quit)(void *), void *context);
/* Start a detached stdin reader only when SKY_INPUT_STDIN=1.
 * Returns 0 when disabled, 1 when started, or -1 on startup failure. */
int sky_input_control_start(void (*on_quit)(void *), void *context);
