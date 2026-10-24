#pragma once

#include <stdint.h>

uint32_t sky_nfp_initialize(void);
uint32_t sky_nfp_attach_availability_change_event(void *event);
