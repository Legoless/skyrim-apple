#pragma once

#include <stdint.h>

uint32_t sky_nifm_initialize(void);
int sky_nifm_is_network_available(void);
int sky_nifm_is_network_request_on_hold(void);
void sky_nifm_submit_network_request(void);
uint32_t sky_nifm_submit_network_request_and_wait(void);
void sky_nifm_cancel_network_request(void);
uint32_t sky_nifm_handle_network_request_error_result(void);
uint32_t sky_nifm_get_current_primary_ip_address(uint32_t *out);
uint32_t sky_nifm_get_current_proxy_setting(void *out);
uint32_t sky_socket_initialize(const void *config);
