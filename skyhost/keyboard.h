#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *header, *subtext, *guide, *initial_text;
    /* UTF-16 code units, including two for a surrogate pair. max_length is
     * normalized to the configured limit and output capacity; zero allows
     * only empty text and never means unlimited. */
    size_t min_length, max_length;
} sky_keyboard_request_t;

/* Called synchronously on the guest thread. Strings are UTF-8 and valid only
 * during the call. Return 1 with NUL-terminated output to accept, 0 to cancel.
 * Frontends marshal their UI to the main thread and wait only on the guest. */
typedef int (*sky_keyboard_presenter_t)(const sky_keyboard_request_t *request,
    char *out_utf8, size_t capacity);
void sky_keyboard_set_presenter(sky_keyboard_presenter_t presenter);

/* nn::swkbd::ShowKeyboard(String *, const ShowKeyboardArg &). */
uint32_t sky_keyboard_show(void *out_string, const void *show_arg);
