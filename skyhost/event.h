#pragma once

#include <stdint.h>

uint32_t sky_event_create(void *guest, int autoclear);
uint32_t sky_event_destroy(void *guest);
uint32_t sky_event_wait(void *guest, int64_t timeout_ns);
uint32_t sky_event_signal(void *guest);
uint32_t sky_event_clear(void *guest);
uint32_t sky_event_set_periodic(void *guest, uint64_t period_ns);
uint32_t sky_event_set_poll_backoff(void *guest, uint64_t backoff_ns);
void sky_aoc_get_add_on_content_list_changed_event(void *event);
