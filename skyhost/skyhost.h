#pragma once

#include "profile.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MOD 2
#define SKY_PAGE_SIZE 0x4000
#define SKY_PATCH_ALIGN 0x10000
#define LOG(...) do { fprintf(stderr, "[skyhost] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

typedef struct {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;

typedef struct {
    int64_t d_tag;
    union { uint64_t d_val, d_ptr; } d_un;
} Elf64_Dyn;

typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffu)

enum {
    DT_PLTRELSZ = 2,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_SYMENT = 11,
    DT_INIT = 12,
    DT_PLTREL = 20,
    DT_JMPREL = 23,
    DT_INIT_ARRAY = 25,
    DT_INIT_ARRAYSZ = 27,
};

enum {
    R_AARCH64_ABS64 = 257,
    R_AARCH64_GLOB_DAT = 1025,
    R_AARCH64_JUMP_SLOT = 1026,
    R_AARCH64_RELATIVE = 1027,
};

enum { RELOC_RELATIVE, RELOC_ABS64, RELOC_GLOB_DAT, RELOC_JUMP_SLOT, RELOC_KIND_N };

typedef struct {
    char name[16];
    char build_id[65];
    sky_save_config_t save_config;
    int has_save_config;
    uint8_t *base;
    size_t size, file_size;
    uint64_t text_end, ro_off, data_off, patch_off, key_page_off, key_page_size;
    unsigned key_idx[8];
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh, symh_mask;
    unsigned gates;
    uint64_t relocations, weak_null;
    uint64_t reloc_count[RELOC_KIND_N];
    uint64_t init, init_array, init_arraysz;
} module_t;

extern module_t g_mod[MAX_MOD];
extern int g_nmod;
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
extern pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

void load_modules(const char *dir, const char *const *names, int count);
void loader_hash_log(void);
void *module_sym(module_t *module, const char *name);
void *guest_sym(const char *name);
module_t *module_of(const void *pointer);
uint64_t resolve_import(module_t *module, const char *name, int weak);

void gate_register(uint64_t return_address, const char *kind, int argument, uint64_t original);
uint64_t gate_dispatch(uint64_t return_address, uint64_t *regs);
uint64_t svc_dispatch(int number, uint64_t *regs, uint64_t original);
uint64_t now_ns(void);
void sky_checkpoint(const char *name);
const char *sky_checkpoint_name(void);
void sky_gate_context_register(void (*callback)(const uint64_t *regs));
void sky_gate_context(const uint64_t *regs);
void sky_post_entry_enable(void);
void sky_post_entry_service(int number);
void sky_set_main_module_base(uintptr_t base);
uintptr_t sky_main_module_base(void);
void sky_set_guest_main_thread(pthread_t thread);
int sky_is_guest_main_thread(void);
pthread_t sky_guest_main_thread(void);
void sky_set_startup_logo_finished(int finished);
int sky_is_startup_logo_finished(void);
void sky_set_ui_in_game(int in_game);
int sky_ui_is_in_game(void);
void sky_set_last_frame_draw_count(uint32_t draws);
uint32_t sky_last_frame_draw_count(void);
int sky_verbose_logging(void);
