#pragma once

#include <stdint.h>

uint32_t sky_svc_main_thread_init(void);
uint32_t sky_svc_main_thread_handle(void);
uint32_t sky_svc_current_thread_handle(void);
void sky_svc_thread_diagnose(void);
