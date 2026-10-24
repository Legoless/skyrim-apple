#pragma once

#include <stdint.h>

void crash_install(void);
void crash_backtrace(uint64_t frame_pointer);
