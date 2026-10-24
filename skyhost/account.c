#include "account.h"
#include "skyhost.h"
#include <stddef.h>

#define RES_OK 0u
#define RES_FAIL 0x2A2u

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

_Static_assert(sizeof(sky_account_uid_t) == 16, "Skyrim account UID layout");
_Static_assert(sizeof(sky_account_user_handle_t) == 24, "Skyrim account UserHandle layout");
_Static_assert(sizeof(sky_account_user_selection_settings_t) == 0x88, "Skyrim account UserSelectionSettings layout");
_Static_assert(offsetof(sky_account_user_selection_settings_t, is_skip_enabled) == 0x80, "Skyrim account selection flags offset");

static uint32_t account_error(const char *operation) {
    LOG("ACCOUNT_ERROR operation=%s result=%#x mode=offline", operation, RES_FAIL);
    return RES_FAIL;
}

static int valid_handle(const sky_account_user_handle_t *handle) {
    return handle && handle->words[0] == SKY_ACCOUNT_OFFLINE_UID_LO &&
           handle->words[1] == SKY_ACCOUNT_OFFLINE_UID_HI && handle->words[2] == SKY_ACCOUNT_HANDLE_MAGIC;
}

void sky_account_initialize(void) {
    sky_checkpoint("post_nnMain_account_initialize");
    LOG("ACCOUNT_INITIALIZE mode=offline no-ipc");
}

uint32_t sky_account_open_preselected_user(sky_account_user_handle_t *out) {
    if (!out) return account_error("OpenPreselectedUser");
    out->words[0] = SKY_ACCOUNT_OFFLINE_UID_LO;
    out->words[1] = SKY_ACCOUNT_OFFLINE_UID_HI;
    out->words[2] = SKY_ACCOUNT_HANDLE_MAGIC;
    sky_checkpoint("post_nnMain_account_open_preselected_user");
    LOG("ACCOUNT_OPEN_PRESELECTED uid_lo=%#llx uid_hi=%#llx mode=local-offline",
        (unsigned long long)out->words[0], (unsigned long long)out->words[1]);
    return RES_OK;
}

uint32_t sky_account_get_user_id(sky_account_uid_t *out, const sky_account_user_handle_t *handle) {
    if (!out || !valid_handle(handle)) return account_error("GetUserId");
    out->words[0] = handle->words[0];
    out->words[1] = handle->words[1];
    return RES_OK;
}

uint32_t sky_account_is_network_service_account_available(uint8_t *out, const sky_account_user_handle_t *handle) {
    if (!out || !valid_handle(handle)) return account_error("IsNetworkServiceAccountAvailable");
    *out = 0;
    sky_checkpoint("post_nnMain_account_offline_query");
    LOG("ACCOUNT_READY uid_lo=%#llx uid_hi=%#llx network_service=0 mode=offline",
        (unsigned long long)handle->words[0], (unsigned long long)handle->words[1]);
    return RES_OK;
}

uint32_t sky_account_is_user_registration_request_permitted(uint8_t *out) {
    if (!out) return account_error("IsUserRegistrationRequestPermitted");
    *out = 0;
    LOG("ACCOUNT_REGISTRATION permitted=0 mode=local-offline");
    return RES_OK;
}

uint32_t sky_account_show_user_selector(sky_account_uid_t *out, const sky_account_user_selection_settings_t *settings) {
    if (!out || !settings) return account_error("ShowUserSelector");
    /* Match sdk+0x192b70..0x192b88: implicit selection cannot exclude the
     * first user or request an additional user. Other flags use nonzero=true. */
    if (settings->is_skip_enabled && (settings->invalid_uid_list[0].words[0] ||
        settings->invalid_uid_list[0].words[1] || settings->additional_select))
        return account_error("ShowUserSelector invalid-skip-settings");
    int eligible = !settings->is_network_service_account_required;
    for (unsigned index = 0; index < 8; index++)
        if (settings->invalid_uid_list[index].words[0] == SKY_ACCOUNT_OFFLINE_UID_LO &&
            settings->invalid_uid_list[index].words[1] == SKY_ACCOUNT_OFFLINE_UID_HI) eligible = 0;
    if (!eligible) {
        if (!settings->show_skip_button) return account_error("ShowUserSelector no-eligible-local-user");
        /* The SDK permits successful selection with a zero UID when skipped.
         * Keep the current account rather than inventing an eligible user. */
        *out = (sky_account_uid_t){{0, 0}};
        LOG("ACCOUNT_USER_SELECTOR selected=none reason=skip no-eligible-local-user mode=local-offline");
    } else {
        *out = (sky_account_uid_t){{SKY_ACCOUNT_OFFLINE_UID_LO, SKY_ACCOUNT_OFFLINE_UID_HI}};
        LOG("ACCOUNT_USER_SELECTOR uid_lo=%#llx uid_hi=%#llx mode=single-local-offline",
            (unsigned long long)out->words[0], (unsigned long long)out->words[1]);
    }
    sky_checkpoint("post_nnMain_account_user_selector");
    return RES_OK;
}

SKY_FORCE("_ZN2nn7account10InitializeEv", "Initialize the host-managed local offline account state without IPC",
          void, sky_account_initialize_entry, void) { sky_account_initialize(); }
SKY_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "Open the stable local offline UID in a host-tagged 24-byte handle",
          uint32_t, sky_account_open_preselected_entry, sky_account_user_handle_t *out) {
    return sky_account_open_preselected_user(out);
}
SKY_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "Read the 16-byte UID from a host-issued offline handle",
          uint32_t, sky_account_get_user_id_entry, sky_account_uid_t *out, const sky_account_user_handle_t *handle) {
    return sky_account_get_user_id(out, handle);
}
SKY_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "The local offline UID has no network service account",
          uint32_t, sky_account_network_available_entry, uint8_t *out, const sky_account_user_handle_t *handle) {
    return sky_account_is_network_service_account_available(out, handle);
}
SKY_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "Select the eligible stable local UID without an IPC-backed Switch account applet",
          uint32_t, sky_account_show_user_selector_entry, sky_account_uid_t *out,
          const sky_account_user_selection_settings_t *settings) {
    return sky_account_show_user_selector(out, settings);
}
SKY_FORCE("_ZN2nn7account34IsUserRegistrationRequestPermittedEPb", "The host supports its existing local account and does not offer account creation",
          uint32_t, sky_account_registration_permitted_entry, uint8_t *out) {
    return sky_account_is_user_registration_request_permitted(out);
}
