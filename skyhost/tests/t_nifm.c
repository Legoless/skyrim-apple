#include "net.h"
#include "skyhost.h"
#include <assert.h>

int main(void) {
    assert(sky_nifm_initialize() == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nifm_initialize"));
    assert(!sky_nifm_is_network_available());
    assert(!sky_nifm_is_network_request_on_hold());

    sky_nifm_submit_network_request();
    assert(sky_nifm_submit_network_request_and_wait() == 0x2A2u);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nifm_submit_wait"));
    assert(!sky_nifm_is_network_available());
    assert(!sky_nifm_is_network_request_on_hold());
    sky_nifm_cancel_network_request();
    assert(!sky_nifm_is_network_available());
    assert(!sky_nifm_is_network_request_on_hold());

    assert(sky_nifm_handle_network_request_error_result() == 0x2A2u);
    uint32_t address = 0xffffffffu;
    assert(sky_nifm_get_current_primary_ip_address(&address) == 0x2A2u);
    assert(address == 0);
    assert(sky_nifm_get_current_primary_ip_address(NULL) == 0x2A2u);

    uint8_t proxy[64], original[64];
    memset(proxy, 0xa5, sizeof proxy);
    memcpy(original, proxy, sizeof proxy);
    assert(sky_nifm_get_current_proxy_setting(proxy) == 0x2A2u);
    assert(!memcmp(proxy, original, sizeof proxy));
    assert(sky_nifm_get_current_proxy_setting(NULL) == 0x2A2u);

    assert(sky_socket_initialize(NULL) == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_socket_initialize"));

    puts("T_NIFM OK offline-init unavailable request-noop submit-wait-failure failure-results socket-init");
    return 0;
}
