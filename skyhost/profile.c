#include "profile.h"
#include <stddef.h>
#include <string.h>

static int g_update_ui, g_has_save_config;
static sky_save_config_t g_save_config;

int sky_profile_valid_save_config(const sky_save_config_t *save) {
    return save && save->data_size > 0 && save->journal_size >= 0 &&
        save->data_max >= save->data_size && save->journal_max >= save->journal_size &&
        save->data_max <= INT64_MAX - save->journal_max;
}

void sky_profile_configure_main(const char *build_id, const sky_save_config_t *save) {
    g_update_ui = build_id && !strcmp(build_id, SKY_UPDATE_MAIN_BUILD_ID);
    g_has_save_config = sky_profile_valid_save_config(save);
    if (g_has_save_config) g_save_config = *save;
    else if (g_update_ui) {
        /* Verified v327680 Control NACP: 6004b944711530a9e0d484c72510d56b
         * 561c33b154e6b731eac4ba6fcab54343. Older rewrites omit NACP metadata. */
        g_save_config = (sky_save_config_t){0x2800000, 0x1800000, 0x1e000000, 0x1800000};
        g_has_save_config = 1;
    }
}

int sky_profile_has_update_ui(void) { return g_update_ui; }

int sky_profile_save_config(sky_save_config_t *out) {
    if (!g_has_save_config) return 0;
    *out = g_save_config;
    return 1;
}

const sky_profile_t sky_profile = {
    .title_id = "01000a10041ea000",
    .name = "The Elder Scrolls V: Skyrim",
    .modules = {"main", "sdk"},
    .module_count = 2,
    .arguments = {"Skyrim", NULL},
    .argument_count = 1,
};
