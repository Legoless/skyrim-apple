#pragma once

#include <stdint.h>

#define SKY_UPDATE_MAIN_BUILD_ID "4f7995092faa5dc083bca44d87dca93e1f77a4ea000000000000000000000000"

typedef struct {
    int64_t data_size, journal_size, data_max, journal_max;
} sky_save_config_t;

void sky_profile_configure_main(const char *build_id, const sky_save_config_t *save);
int sky_profile_has_update_ui(void);
int sky_profile_save_config(sky_save_config_t *out);
int sky_profile_valid_save_config(const sky_save_config_t *save);

typedef struct {
    const char *title_id;
    const char *name;
    const char *modules[2];
    unsigned module_count;
    const char *arguments[2];
    unsigned argument_count;
} sky_profile_t;

extern const sky_profile_t sky_profile;
