#include "nv.h"
#include "skyhost.h"

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

void sky_nv_initialize_graphics(void *memory, size_t size) {
    (void)memory;
    (void)size;
    sky_checkpoint("post_nnMain_nv_initialize");
    LOG("NV_INITIALIZE_GRAPHICS mode=offscreen no-nvdrv");
}

SKY_FORCE("_ZN2nv18InitializeGraphicsEPvm", "Graphics are initialized later by the offscreen host NVN backend",
          void, sky_nv_initialize_entry, void *memory, size_t size) { sky_nv_initialize_graphics(memory, size); }
