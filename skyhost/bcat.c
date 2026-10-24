#include "bcat.h"
#include "skyhost.h"

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

void sky_bcat_initialize(void) {
    sky_checkpoint("post_nnMain_bcat_initialize");
    LOG("BCAT_INITIALIZE mode=offline no-ipc");
}

SKY_FORCE("_ZN2nn4bcat10InitializeEv", "Initialize BCAT in the host-managed offline state without IPC or downloads",
          void, sky_bcat_initialize_entry, void) { sky_bcat_initialize(); }
