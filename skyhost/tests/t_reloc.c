#include "../loader.c"
#include <assert.h>
#include <unistd.h>

static _Alignas(16) uint8_t importer[8192], exporter[8192];
static const char names[] = "\0optional\0provided\0required\0host_only";
static Elf64_Sym imports[5] = {
    {0},
    {.st_name = 1, .st_info = 0x20},
    {.st_name = 10, .st_info = 0x20},
    {.st_name = 19, .st_info = 0x10},
    {.st_name = 28, .st_info = 0x10},
};
static Elf64_Sym exports[2] = {
    {0},
    {.st_name = 10, .st_info = 0x10, .st_shndx = 1, .st_value = 0x100},
};
static Elf64_Dyn dynamic_table[3];
static hle_force_ent forced = {
    .name = "host_only",
    .why = "synthetic forced-entry test",
    .function = (void *)(uintptr_t)0x12345000,
};

static void setup(unsigned relocation_count) {
    sky_profile_configure_main(SKY_UPDATE_MAIN_BUILD_ID, NULL);
    memset(importer, 0, sizeof importer);
    memset(exporter, 0, sizeof exporter);
    memcpy(importer + 0x100, names, sizeof names);
    memcpy(exporter + 0x100, names, sizeof names);
    memset(g_mod, 0, sizeof g_mod);
    g_nmod = 2;
    dynamic_table[0] = (Elf64_Dyn){.d_tag = DT_RELA, .d_un.d_ptr = 0x300};
    dynamic_table[1] = (Elf64_Dyn){.d_tag = DT_RELASZ, .d_un.d_val = relocation_count * sizeof(Elf64_Rela)};
    g_mod[0] = (module_t){
        .base = importer, .size = sizeof importer, .file_size = sizeof importer,
        .symtab = imports, .strtab = (char *)importer + 0x100, .nsym = 5, .dyn = dynamic_table,
    };
    strcpy(g_mod[0].name, "main");
    g_mod[1] = (module_t){
        .base = exporter, .size = sizeof exporter, .file_size = sizeof exporter,
        .symtab = exports, .strtab = (char *)exporter + 0x100, .nsym = 2,
    };
    strcpy(g_mod[1].name, "sdk");
    g_force = NULL;
    g_nforce = 0;
    g_force_hits = NULL;
}

static void success(void) {
    setup(5);
    Elf64_Rela *relocations = (Elf64_Rela *)(importer + 0x300);
    relocations[0] = (Elf64_Rela){.r_offset = 0x800, .r_info = R_AARCH64_RELATIVE, .r_addend = 0x40};
    relocations[1] = (Elf64_Rela){.r_offset = 0x808, .r_info = (2ull << 32) | R_AARCH64_ABS64, .r_addend = 8};
    relocations[2] = (Elf64_Rela){.r_offset = 0x810, .r_info = (1ull << 32) | R_AARCH64_GLOB_DAT};
    relocations[3] = (Elf64_Rela){.r_offset = 0x818, .r_info = (2ull << 32) | R_AARCH64_JUMP_SLOT, .r_addend = 0xdead};
    relocations[4] = (Elf64_Rela){.r_offset = 0x820, .r_info = (4ull << 32) | R_AARCH64_GLOB_DAT};
    g_force = &forced;
    g_nforce = 1;
    g_force_hits = checked_calloc(1, sizeof *g_force_hits);

    relocate(&g_mod[0]);
    force_check();
    uint64_t *values = (uint64_t *)(importer + 0x800);
    assert(values[0] == (uintptr_t)(importer + 0x40));
    assert(values[1] == (uintptr_t)(exporter + 0x108));
    assert(values[2] == 0);
    assert(values[3] == (uintptr_t)(exporter + 0x100));
    assert(values[4] == (uintptr_t)0x12345000);
    assert(g_mod[0].weak_null == 1);
    assert(g_mod[0].relocations == 5);
    assert(g_mod[0].reloc_count[RELOC_RELATIVE] == 1);
    assert(g_mod[0].reloc_count[RELOC_ABS64] == 1);
    assert(g_mod[0].reloc_count[RELOC_GLOB_DAT] == 2);
    assert(g_mod[0].reloc_count[RELOC_JUMP_SLOT] == 1);
    puts("T_RELOC OK relative/abs64/glob_dat/jump_slot weak-null forced-entry");
}

static void unsupported(void) {
    setup(1);
    Elf64_Rela *relocation = (Elf64_Rela *)(importer + 0x300);
    *relocation = (Elf64_Rela){.r_offset = 0x800, .r_info = 261};
    relocate(&g_mod[0]);
}

static void nonweak(void) {
    setup(1);
    Elf64_Rela *relocation = (Elf64_Rela *)(importer + 0x300);
    *relocation = (Elf64_Rela){.r_offset = 0x800, .r_info = (3ull << 32) | R_AARCH64_GLOB_DAT};
    relocate(&g_mod[0]);
}

static void force_unused(int known_build) {
    setup(0);
    if (!known_build) sky_profile_configure_main(NULL, NULL);
    g_force = &forced;
    g_nforce = 1;
    g_force_hits = checked_calloc(1, sizeof *g_force_hits);
    force_check();
}

static void metadata(void) {
    char directory[] = "/tmp/sky-layout-test-XXXXXX", path[1024];
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof path, "%s/main.fixups.tsv", directory) > 0);
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "# PATCH_OFF 0x10000\n# KEY_PAGE_OFF 0x4000\n# KEY_PAGE_SIZE 0x4000\n"
                  "# TEXT_END 0x1000\n# RO_OFF 0x1000\n# DATA_OFF 0x2000\n");
    for (unsigned index = 0; index < KEY_N; index++) fprintf(file, "# KEY_%s_IDX %u\n", g_key_names[index], index);
    assert(!fclose(file));
    module_t module = {.name = "main"};
    read_layout(&module, directory); /* Pre-metadata unit fixtures remain valid. */
    assert(!module.build_id[0] && !module.has_save_config);
    file = fopen(path, "a");
    assert(file);
    fprintf(file, "# NSO_BUILD_ID %s\n# SAVE_DATA_SIZE 0x20000\n# SAVE_JOURNAL_SIZE 0x10000\n"
                  "# SAVE_DATA_MAX 0x40000\n# SAVE_JOURNAL_MAX 0x20000\n", SKY_UPDATE_MAIN_BUILD_ID);
    assert(!fclose(file));
    read_layout(&module, directory);
    assert(strlen(module.build_id) == 64 && !strcmp(module.build_id, SKY_UPDATE_MAIN_BUILD_ID));
    assert(module.has_save_config && module.save_config.data_size == 0x20000);
    sky_profile_configure_main(module.build_id, &module.save_config);
    assert(sky_profile_has_update_ui());
    sky_save_config_t save;
    assert(sky_profile_save_config(&save) && save.data_max == 0x40000 && save.journal_max == 0x20000);
    module.build_id[63] = '1';
    sky_profile_configure_main(module.build_id, NULL);
    assert(!sky_profile_has_update_ui() && !sky_profile_save_config(&save));
    sky_profile_configure_main(module.build_id, &module.save_config);
    assert(!sky_profile_has_update_ui() && sky_profile_save_config(&save));
    assert(save.data_size == 0x20000 && save.journal_size == 0x10000);
    assert(!unlink(path) && !rmdir(directory));
    puts("T_METADATA OK source-build-id source-save-config exact-update-only legacy-layout");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "success";
    if (!strcmp(mode, "success")) {
        success();
        metadata();
        force_unused(0);
    }
    else if (!strcmp(mode, "unsupported")) unsupported();
    else if (!strcmp(mode, "nonweak")) nonweak();
    else if (!strcmp(mode, "nonweak-unknown")) {
        setup(1);
        sky_profile_configure_main(NULL, NULL);
        Elf64_Rela *relocation = (Elf64_Rela *)(importer + 0x300);
        *relocation = (Elf64_Rela){.r_offset = 0x800, .r_info = (3ull << 32) | R_AARCH64_GLOB_DAT};
        relocate(&g_mod[0]);
    }
    else if (!strcmp(mode, "force-unused")) force_unused(1);
    else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    return 0;
}
