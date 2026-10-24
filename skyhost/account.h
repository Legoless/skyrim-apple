#pragma once

#include <stdint.h>

#define SKY_ACCOUNT_OFFLINE_UID_LO 1ull
#define SKY_ACCOUNT_OFFLINE_UID_HI 0ull
#define SKY_ACCOUNT_HANDLE_MAGIC 0x0054434341594B53ull

typedef struct { uint64_t words[2]; } sky_account_uid_t;
typedef struct { uint64_t words[3]; } sky_account_user_handle_t;
typedef struct {
    sky_account_uid_t invalid_uid_list[8];
    uint8_t is_skip_enabled;
    uint8_t is_network_service_account_required;
    uint8_t show_skip_button;
    uint8_t additional_select;
    uint8_t is_unqualified_user_selectable;
    uint8_t reserved[3];
} sky_account_user_selection_settings_t;

void sky_account_initialize(void);
uint32_t sky_account_open_preselected_user(sky_account_user_handle_t *out);
uint32_t sky_account_get_user_id(sky_account_uid_t *out, const sky_account_user_handle_t *handle);
uint32_t sky_account_is_network_service_account_available(uint8_t *out, const sky_account_user_handle_t *handle);
uint32_t sky_account_show_user_selector(sky_account_uid_t *out, const sky_account_user_selection_settings_t *settings);
uint32_t sky_account_is_user_registration_request_permitted(uint8_t *out);
