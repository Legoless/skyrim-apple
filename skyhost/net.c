#include "net.h"
#include "skyhost.h"

#define RES_OK 0u
#define RES_FAIL 0x2A2u

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

static uint32_t nifm_fail(const char *operation) {
    LOG("NIFM_ERROR operation=%s result=%#x mode=offline", operation, RES_FAIL);
    return RES_FAIL;
}

uint32_t sky_nifm_initialize(void) {
    sky_checkpoint("post_nnMain_nifm_initialize");
    LOG("NIFM_INITIALIZE mode=offline no-ipc");
    return RES_OK;
}

int sky_nifm_is_network_available(void) {
    sky_checkpoint("post_nnMain_nifm_availability");
    LOG("NIFM_NETWORK_AVAILABLE value=0");
    return 0;
}

int sky_nifm_is_network_request_on_hold(void) {
    return 0;
}

void sky_nifm_submit_network_request(void) {
}

uint32_t sky_nifm_submit_network_request_and_wait(void) {
    sky_checkpoint("post_nnMain_nifm_submit_wait");
    return nifm_fail("SubmitNetworkRequestAndWait");
}

void sky_nifm_cancel_network_request(void) {
}

uint32_t sky_nifm_handle_network_request_error_result(void) {
    return nifm_fail("HandleNetworkRequestErrorResult");
}

uint32_t sky_nifm_get_current_primary_ip_address(uint32_t *out) {
    if (out) *out = 0;
    return nifm_fail("GetCurrentPrimaryIpAddress");
}

uint32_t sky_nifm_get_current_proxy_setting(void *out) {
    (void)out;
    return nifm_fail("GetCurrentProxySetting");
}

uint32_t sky_socket_initialize(const void *config) {
    (void)config;
    sky_checkpoint("post_nnMain_socket_initialize");
    LOG("SOCKET_INITIALIZE mode=offline no-session no-ipc");
    return RES_OK;
}

SKY_FORCE("_ZN2nn4nifm10InitializeEv", "Initialize the explicit host-managed offline NIFM state without IPC",
          uint32_t, sky_nifm_initialize_entry, void) { return sky_nifm_initialize(); }
SKY_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "The host-managed NIFM state is always offline",
          int, sky_nifm_available_entry, void) { return sky_nifm_is_network_available(); }
SKY_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "Offline network requests cannot be submitted or awaited",
          uint32_t, sky_nifm_submit_wait_entry, void) { return sky_nifm_submit_network_request_and_wait(); }
SKY_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "The offline host has no network request to cancel",
          void, sky_nifm_cancel_entry, void) { sky_nifm_cancel_network_request(); }
SKY_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "Offline NIFM has no proxy setting",
          uint32_t, sky_nifm_get_proxy_entry, void *out) { return sky_nifm_get_current_proxy_setting(out); }
SKY_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "Initialize offline socket state without IPC or a session",
          uint32_t, sky_socket_initialize_entry, const void *config) { return sky_socket_initialize(config); }
