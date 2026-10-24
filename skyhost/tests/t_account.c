#include "account.h"
#include "skyhost.h"
#include <assert.h>
#include <stddef.h>
#include <mach-o/getsect.h>

extern const struct mach_header_64 _mh_execute_header;
static void *force_function(const char *name) {
    typedef struct { const char *name, *why; void *function; } force_entry_t;
    unsigned long size = 0;
    const force_entry_t *entries = (const force_entry_t *)getsectiondata(
        &_mh_execute_header, "__DATA", "swforce", &size);
    assert(entries && size % sizeof *entries == 0);
    for (unsigned long index = 0; index < size / sizeof *entries; index++)
        if (!strcmp(entries[index].name, name)) return entries[index].function;
    assert(!"missing forced account entry point");
    return NULL;
}

int main(void) {
    sky_account_initialize();
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_account_initialize"));

    sky_account_user_handle_t first, second;
    memset(&first, 0xa5, sizeof first);
    assert(sky_account_open_preselected_user(&first) == 0);
    assert(sizeof first == 24);
    assert(first.words[0] == SKY_ACCOUNT_OFFLINE_UID_LO);
    assert(first.words[1] == SKY_ACCOUNT_OFFLINE_UID_HI);
    assert(first.words[2] == SKY_ACCOUNT_HANDLE_MAGIC);
    assert(sky_account_open_preselected_user(&second) == 0);
    assert(!memcmp(&first, &second, sizeof first));

    sky_account_uid_t uid = {{0xffffffffffffffffull, 0xffffffffffffffffull}};
    assert(sky_account_get_user_id(&uid, &first) == 0);
    assert(sizeof uid == 16);
    assert(uid.words[0] == SKY_ACCOUNT_OFFLINE_UID_LO);
    assert(uid.words[1] == SKY_ACCOUNT_OFFLINE_UID_HI);

    uint8_t available = 0xff;
    assert(sky_account_is_network_service_account_available(&available, &first) == 0);
    assert(available == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_account_offline_query"));

    second.words[2] ^= 1;
    available = 0xff;
    assert(sky_account_is_network_service_account_available(&available, &second) == 0x2A2u);
    assert(available == 0xff);
    assert(sky_account_get_user_id(&uid, &second) == 0x2A2u);
    assert(sky_account_open_preselected_user(NULL) == 0x2A2u);
    assert(sky_account_get_user_id(NULL, &first) == 0x2A2u);
    assert(sky_account_is_network_service_account_available(NULL, &first) == 0x2A2u);

    typedef uint32_t (*selector_fn)(sky_account_uid_t *, const sky_account_user_selection_settings_t *);
    typedef uint32_t (*registration_fn)(uint8_t *);
    selector_fn select_user = (selector_fn)force_function("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE");
    registration_fn registration = (registration_fn)force_function("_ZN2nn7account34IsUserRegistrationRequestPermittedEPb");
    struct { uint8_t before, permitted, after; } permission = {0xa5, 0xff, 0x5a};
    assert(registration(&permission.permitted) == 0);
    assert(permission.permitted == 0 && permission.before == 0xa5 && permission.after == 0x5a);
    assert(registration(NULL) == 0x2a2);
    struct { uint64_t before; sky_account_uid_t selected; uint64_t after; } guarded = {
        0x1122334455667788, {{UINT64_MAX, UINT64_MAX}}, 0x8877665544332211,
    };
    sky_account_user_selection_settings_t settings = {0}, original_settings;
    assert(sizeof settings == 0x88 && offsetof(sky_account_user_selection_settings_t, is_skip_enabled) == 0x80);
    original_settings = settings;
    assert(select_user(&guarded.selected, &settings) == 0);
    assert(guarded.before == 0x1122334455667788 && guarded.after == 0x8877665544332211);
    assert(!memcmp(&settings, &original_settings, sizeof settings));
    /* The reachable title ChangeUser request must select the same UID already
     * used for saves, so the game's changed-user branch stays inactive. */
    assert(sky_account_get_user_id(&uid, &first) == 0);
    assert(!memcmp(&guarded.selected, &uid, sizeof uid));
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_account_user_selector"));
    settings.invalid_uid_list[7] = uid;
    guarded.selected = (sky_account_uid_t){{UINT64_MAX, UINT64_MAX}};
    assert(select_user(&guarded.selected, &settings) == 0x2a2);
    assert(guarded.selected.words[0] == UINT64_MAX && guarded.selected.words[1] == UINT64_MAX);
    settings.show_skip_button = 1;
    assert(select_user(&guarded.selected, &settings) == 0);
    assert(!guarded.selected.words[0] && !guarded.selected.words[1]);
    settings = (sky_account_user_selection_settings_t){0};
    settings.invalid_uid_list[0].words[0] = 2;
    assert(select_user(&guarded.selected, &settings) == 0 && !memcmp(&guarded.selected, &uid, sizeof uid));
    settings.is_skip_enabled = 1;
    assert(select_user(&guarded.selected, &settings) == 0x2a2);
    settings.invalid_uid_list[0].words[0] = 0;
    settings.additional_select = 1;
    assert(select_user(&guarded.selected, &settings) == 0x2a2);
    settings.additional_select = 0;
    assert(select_user(&guarded.selected, &settings) == 0);
    settings = (sky_account_user_selection_settings_t){0};
    settings.is_network_service_account_required = 1;
    settings.is_unqualified_user_selectable = 1;
    guarded.selected = (sky_account_uid_t){{UINT64_MAX, UINT64_MAX}};
    assert(select_user(&guarded.selected, &settings) == 0x2a2);
    assert(guarded.selected.words[0] == UINT64_MAX && guarded.selected.words[1] == UINT64_MAX);
    settings.show_skip_button = 1;
    assert(select_user(&guarded.selected, &settings) == 0);
    assert(!guarded.selected.words[0] && !guarded.selected.words[1]);
    assert(select_user(NULL, &settings) == 0x2a2);
    assert(select_user(&guarded.selected, NULL) == 0x2a2);
    assert(guarded.before == 0x1122334455667788 && guarded.after == 0x8877665544332211);

    puts("T_ACCOUNT OK init stable-uid-16 handle-24 offline-query");
    puts("T_ACCOUNT_SELECTOR OK forced-abi one-local-user excluded-uid network-required skip unchanged-profile out-widths registration-disabled");
    return 0;
}
