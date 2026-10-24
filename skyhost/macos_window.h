#pragma once

/* AppKit owns the main thread; the original game runs on a guest thread. */
int sky_macos_run(int (*run_guest)(void));
void *sky_macos_metal_layer(void);
