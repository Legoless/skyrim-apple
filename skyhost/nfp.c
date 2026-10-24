#include "nfp.h"
#include "skyhost.h"

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

#define RES_FAIL 0x2A2u

static uint32_t nfp_fail(const char *operation) {
    LOG("NFP_ERROR operation=%s result=%#x mode=offline", operation, RES_FAIL);
    return RES_FAIL;
}

uint32_t sky_nfp_initialize(void) {
    sky_checkpoint("post_nnMain_nfp_initialize");
    LOG("NFP_INITIALIZE mode=offline devices=0 no-ipc");
    return 0;
}

uint32_t sky_nfp_attach_availability_change_event(void *event) {
    (void)event;
    sky_checkpoint("post_nnMain_nfp_availability_event");
    return nfp_fail("AttachAvailabilityChangeEvent");
}

SKY_FORCE("_ZN2nn3nfp10InitializeEv", "Initialize NFP with no local NFC devices and no IPC",
          uint32_t, sky_nfp_initialize_entry, void) { return sky_nfp_initialize(); }
SKY_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "No NFP availability events exist without an NFP service",
          uint32_t, sky_nfp_attach_availability_entry, void *event) {
    return sky_nfp_attach_availability_change_event(event);
}
