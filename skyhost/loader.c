#include "skyhost.h"
#include <TargetConditionals.h>

#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_KRES0, KEY_KRES1, KEY_KASPECT, KEY_KASPECTUI, KEY_N };
static const char *const g_key_names[KEY_N] = {
    "K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI",
};

static void module_range(const module_t *module, uint64_t offset, uint64_t size, const char *what) {
    if (offset > module->file_size || size > module->file_size - offset)
        die("%s: %s range %#llx+%#llx exceeds file %#zx", module->name, what,
            (unsigned long long)offset, (unsigned long long)size, module->file_size);
}

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static int32_t read_i32(const uint8_t *bytes) {
    int32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static void write_u64(uint8_t *bytes, uint64_t value) {
    memcpy(bytes, &value, sizeof value);
}

static void *checked_malloc(size_t size) {
    void *result = malloc(size ? size : 1);
    if (!result) die("malloc %#zx failed", size);
    return result;
}

static void *checked_calloc(size_t count, size_t size) {
    void *result = calloc(count ? count : 1, size ? size : 1);
    if (!result) die("calloc %#zx x %#zx failed", count, size);
    return result;
}

static FILE *open_module_file(const char *dir, const char *name, const char *extension, const char *mode) {
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/%s.%s", dir, name, extension);
    if (length < 0 || (size_t)length >= sizeof path) die("%s.%s path is too long", name, extension);
    FILE *file = fopen(path, mode);
    if (!file) die("open %s", path);
    return file;
}

static pthread_key_t key_value(unsigned index) {
    switch (index) {
    case KEY_K18: return g_key18;
    case KEY_KTLS: return g_keytls;
    case KEY_KTLR: return g_keytlr;
    case KEY_KGATE: return g_keygate;
    case KEY_KRES0: return g_keyres[0];
    case KEY_KRES1: return g_keyres[1];
    case KEY_KASPECT: return g_keyaspect;
    case KEY_KASPECTUI: return g_keyaspectui;
    default: die("invalid key index %u", index);
    }
}

static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL) ||
        pthread_key_create(&g_keyres[0], NULL) || pthread_key_create(&g_keyres[1], NULL) ||
        pthread_key_create(&g_keyaspect, NULL) || pthread_key_create(&g_keyaspectui, NULL))
        die("pthread_key_create failed");
}

static void read_layout(module_t *module, const char *dir) {
    module->build_id[0] = 0;
    module->has_save_config = 0;
    module->save_config = (sky_save_config_t){0};
    enum { L_PATCH, L_KEY_PAGE, L_KEY_SIZE, L_TEXT, L_RO, L_DATA, L_LAYOUT_N };
    static const char *const layout_names[L_LAYOUT_N] = {
        "PATCH_OFF", "KEY_PAGE_OFF", "KEY_PAGE_SIZE", "TEXT_END", "RO_OFF", "DATA_OFF",
    };
    uint64_t *layout_targets[L_LAYOUT_N] = {
        &module->patch_off, &module->key_page_off, &module->key_page_size,
        &module->text_end, &module->ro_off, &module->data_off,
    };
    FILE *file = open_module_file(dir, module->name, "fixups.tsv", "r");
    unsigned seen = 0, metadata_seen = 0;
    static const char *const save_names[] = {
        "SAVE_DATA_SIZE", "SAVE_JOURNAL_SIZE", "SAVE_DATA_MAX", "SAVE_JOURNAL_MAX",
    };
    int64_t *save_targets[] = {
        &module->save_config.data_size, &module->save_config.journal_size,
        &module->save_config.data_max, &module->save_config.journal_max,
    };
    char line[256];
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') && !feof(file)) die("%s: fixup line too long", module->name);
        if (line[0] == '\n' || line[0] == '\0') continue;
        if (line[0] != '#') die("%s: non-header row in fixups.tsv", module->name);
        char key[64], value[65], extra[2];
        if (sscanf(line, "# %63s %64s %1s", key, value, extra) != 2)
            die("%s: malformed fixup line: %s", module->name, line);
        if (!strcmp(key, "NSO_BUILD_ID")) {
            if ((metadata_seen & 1) || strlen(value) != 64) die("%s: invalid or duplicate NSO_BUILD_ID", module->name);
            for (unsigned index = 0; index < 64; index++) {
                if (!isxdigit((unsigned char)value[index])) die("%s: invalid NSO_BUILD_ID", module->name);
                module->build_id[index] = (char)tolower((unsigned char)value[index]);
            }
            module->build_id[64] = 0;
            metadata_seen |= 1;
            continue;
        }
        errno = 0;
        char *end = NULL;
        unsigned long long parsed = strtoull(value, &end, 0);
        if (errno || !end || *end) die("%s: invalid fixup value %s", module->name, value);

        int save_key = 0;
        for (unsigned index = 0; index < 4; index++) {
            if (strcmp(key, save_names[index])) continue;
            unsigned bit = 2u << index;
            if (strcmp(module->name, "main") || (metadata_seen & bit) || value[0] == '-' || parsed > INT64_MAX)
                die("%s: invalid or duplicate save metadata %s", module->name, key);
            *save_targets[index] = (int64_t)parsed;
            metadata_seen |= bit;
            save_key = 1;
            break;
        }
        if (save_key) continue;

        unsigned bit = 0;
        int matched = 0;
        for (int index = 0; index < L_LAYOUT_N; index++) {
            if (!strcmp(key, layout_names[index])) {
                *layout_targets[index] = parsed;
                bit = 1u << index;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            for (int index = 0; index < KEY_N; index++) {
                char expected[64];
                snprintf(expected, sizeof expected, "KEY_%s_IDX", g_key_names[index]);
                if (!strcmp(key, expected)) {
                    if (parsed >= SKY_PAGE_SIZE / 8) die("%s: key index %llu out of range", module->name, parsed);
                    module->key_idx[index] = (unsigned)parsed;
                    bit = 1u << (L_LAYOUT_N + index);
                    matched = 1;
                    break;
                }
            }
        }
        if (!matched || !bit || (seen & bit)) die("%s: unknown or duplicate fixup key %s", module->name, key);
        seen |= bit;
    }
    if (ferror(file) || fclose(file)) die("%s: reading fixups.tsv failed", module->name);
    if (seen != (1u << (L_LAYOUT_N + KEY_N)) - 1) die("%s: incomplete fixups.tsv", module->name);
    if (metadata_seen & 30) {
        if ((metadata_seen & 30) != 30 || !sky_profile_valid_save_config(&module->save_config))
            die("%s: incomplete or invalid Control NACP save metadata", module->name);
        module->has_save_config = 1;
    }
    for (int first = 0; first < KEY_N; first++)
        for (int second = first + 1; second < KEY_N; second++)
            if (module->key_idx[first] == module->key_idx[second]) die("%s: duplicate key-page slot", module->name);
}

static void validate_layout(const module_t *module) {
    if (!module->text_end || module->text_end % 4 || module->text_end > module->ro_off ||
        module->ro_off > module->data_off || module->data_off >= module->key_page_off)
        die("%s: invalid segment layout", module->name);
    if (module->key_page_size != SKY_PAGE_SIZE || module->key_page_off % SKY_PAGE_SIZE ||
        module->key_page_off > module->patch_off ||
        module->key_page_size > module->patch_off - module->key_page_off ||
        module->patch_off % SKY_PATCH_ALIGN || module->patch_off > module->file_size)
        die("%s: invalid key-page/patch layout", module->name);
    if (module->file_size > SIZE_MAX - (SKY_PAGE_SIZE - 1)) die("%s: image too large", module->name);
}

static void check_protection(module_t *module, uint64_t offset, int expected, const char *what) {
    vm_address_t region = (vm_address_t)module->base + offset;
    vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = vm_region_64(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                                        (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    int permissions = PROT_READ | PROT_WRITE | PROT_EXEC;
    if (result != KERN_SUCCESS || (info.protection & permissions) != expected)
        die("%s: %s protection validation failed", module->name, what);
}

static void install_key_page(module_t *module) {
    if (module->key_page_off > module->size || module->key_page_size > module->size - module->key_page_off)
        die("%s: key page out of bounds", module->name);
    uint64_t *page = (uint64_t *)(module->base + module->key_page_off);
    memset(page, 0, module->key_page_size);
    for (unsigned index = 0; index < KEY_N; index++) {
        page[module->key_idx[index]] = (uint64_t)key_value(index) * 8;
        if (!page[module->key_idx[index]]) die("%s: zero key-page entry %u", module->name, index);
    }
    if (mprotect(page, module->key_page_size, PROT_READ)) die("%s: key-page mprotect", module->name);
}

static void tsd_check(void) {
    if (pthread_setspecific(g_key18, (void *)0x1234)) die("pthread_setspecific TSD check");
    uint64_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    module_t *module = &g_mod[0];
    uint64_t offset = *(uint64_t *)(module->base + module->key_page_off + module->key_idx[KEY_K18] * 8);
    uint64_t actual = *(uint64_t *)((uintptr_t)(base & ~7ull) + offset);
    if (actual != 0x1234) die("TSD layout mismatch");
    if (pthread_setspecific(g_key18, NULL)) die("restore TSD check value");
}

#if TARGET_OS_IPHONE || defined(SKY_IOS_EMBEDDED_LOADER_TEST)
#include "sky_ios_embed_sizes.h"

/* gen_embed.py places signed code in __TEXT and mutable data in separate RW
 * segments. Reassemble their original relative layout without copying code to
 * anonymous executable pages. The test define exercises this mapper on macOS. */
static void remap_signed_section(module_t *module, vm_address_t target,
    const uint8_t *source, vm_size_t size, boolean_t copy, vm_prot_t expected) {
    vm_address_t address = target;
    vm_prot_t current = 0, maximum = 0;
    kern_return_t result = vm_remap(mach_task_self(), &address, size, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)source,
        copy, &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS || address != target ||
        (current & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) != expected)
        die("%s: signed section remap failed result=%d protection=%#x", module->name, result, current);
}

static void map_image(module_t *module, const char *dir) {
    (void)dir;
    if (!strcmp(module->name, "main")) module->file_size = SKY_IOS_MAIN_FILE_SIZE;
    else if (!strcmp(module->name, "sdk")) module->file_size = SKY_IOS_SDK_FILE_SIZE;
    else die("%s: no signed embedded image metadata", module->name);
    validate_layout(module);
    module->size = (module->file_size + SKY_PAGE_SIZE - 1) & ~(size_t)(SKY_PAGE_SIZE - 1);
    size_t text_size = ((size_t)module->text_end + SKY_PAGE_SIZE - 1) & ~(size_t)(SKY_PAGE_SIZE - 1);
    char text_name[17], patch_name[17], data_segment[17];
    if (snprintf(text_name, sizeof text_name, "__dm%sx0", module->name) >= (int)sizeof text_name ||
        snprintf(patch_name, sizeof patch_name, "__dm%sx1", module->name) >= (int)sizeof patch_name ||
        snprintf(data_segment, sizeof data_segment, "__DM%sW", module->name) >= (int)sizeof data_segment)
        die("%s: embedded section name too long", module->name);
    unsigned long text_bytes = 0, data_bytes = 0, patch_bytes = 0;
    const uint8_t *text = getsectiondata(&_mh_execute_header, "__TEXT", text_name, &text_bytes);
    const uint8_t *data = getsectiondata(&_mh_execute_header, data_segment, "__img", &data_bytes);
    const uint8_t *patch = getsectiondata(&_mh_execute_header, "__TEXT", patch_name, &patch_bytes);
    if (!text || !data || !patch || text_bytes != text_size || text_size >= module->patch_off ||
        data_bytes != module->patch_off - text_size || patch_bytes != module->size - module->patch_off ||
        ((uintptr_t)text | (uintptr_t)data | (uintptr_t)patch | text_bytes | data_bytes | patch_bytes) % SKY_PAGE_SIZE)
        die("%s: signed embedded section layout mismatch", module->name);
    vm_address_t base = 0;
    kern_return_t result = vm_allocate(mach_task_self(), &base, module->size, VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS) die("%s: reserve signed image address space result=%d", module->name, result);
    remap_signed_section(module, base, text, text_bytes, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_signed_section(module, base + text_size, data, data_bytes, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    remap_signed_section(module, base + module->patch_off, patch, patch_bytes, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    module->base = (uint8_t *)base;
    if (!strcmp(module->name, "main")) sky_set_main_module_base((uintptr_t)module->base);
    install_key_page(module);
    check_protection(module, 0, PROT_READ | PROT_EXEC, "text");
    check_protection(module, module->data_off, PROT_READ | PROT_WRITE, "data");
    check_protection(module, module->key_page_off, PROT_READ, "key page");
    check_protection(module, module->patch_off, PROT_READ | PROT_EXEC, "patch");
    LOG("SIGNED_IMAGE module=%s code=embedded-shared-rx data=copy-on-write-rw", module->name);
}
#else
static uint8_t *slurp(const char *dir, const char *name, size_t *size) {
    char path[1024];
    int length = snprintf(path, sizeof path, "%s/%s.patched.bin", dir, name);
    if (length < 0 || (size_t)length >= sizeof path) die("%s: patched image path too long", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat status;
    if (fstat(fd, &status) || status.st_size <= 0) die("stat %s", path);
    size_t wanted = (size_t)status.st_size;
    uint8_t *bytes = checked_malloc(wanted);
    size_t done = 0;
    while (done < wanted) {
        ssize_t got = read(fd, bytes + done, wanted - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) die("read %s", path);
        done += (size_t)got;
    }
    if (close(fd)) die("close %s", path);
    *size = wanted;
    return bytes;
}

static void map_image(module_t *module, const char *dir) {
    size_t file_size = 0;
    uint8_t *raw = slurp(dir, module->name, &file_size);
    module->file_size = file_size;
    module->size = (file_size + SKY_PAGE_SIZE - 1) & ~(size_t)(SKY_PAGE_SIZE - 1);
    validate_layout(module);
    module->base = mmap(NULL, module->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (module->base == MAP_FAILED) die("%s: mmap %#zx", module->name, module->size);
    if (!strcmp(module->name, "main")) sky_set_main_module_base((uintptr_t)module->base);
    memcpy(module->base, raw, file_size);
    free(raw);

    install_key_page(module);
    size_t text_size = ((size_t)module->text_end + SKY_PAGE_SIZE - 1) & ~(size_t)(SKY_PAGE_SIZE - 1);
    if (mprotect(module->base, text_size, PROT_READ | PROT_EXEC)) die("%s: text mprotect", module->name);
    if (module->size > module->patch_off &&
        mprotect(module->base + module->patch_off, module->size - module->patch_off, PROT_READ | PROT_EXEC))
        die("%s: patch mprotect", module->name);

    check_protection(module, 0, PROT_READ | PROT_EXEC, "text");
    check_protection(module, module->data_off, PROT_READ | PROT_WRITE, "data");
    check_protection(module, module->key_page_off, PROT_READ, "key page");
    if (module->size > module->patch_off)
        check_protection(module, module->patch_off, PROT_READ | PROT_EXEC, "patch");
}
#endif

static uint32_t symbol_hash(const char *name) {
    uint32_t hash = 2166136261u;
    while (*name) hash = (hash ^ (uint8_t)*name++) * 16777619u;
    return hash;
}

static const char *module_string(const module_t *module, uint32_t offset) {
    uintptr_t base = (uintptr_t)module->base, table = (uintptr_t)module->strtab;
    if (table < base || table > base + module->file_size) die("%s: invalid string table", module->name);
    size_t table_offset = table - base;
    if (table_offset > module->file_size || offset >= module->file_size - table_offset)
        die("%s: string offset out of bounds", module->name);
    const char *string = module->strtab + offset;
    size_t remaining = module->file_size - table_offset - (size_t)offset;
    if (!memchr(string, '\0', remaining)) die("%s: unterminated dynamic string", module->name);
    return string;
}

static void build_symhash(module_t *module) {
    unsigned capacity = 16;
    if (module->nsym > UINT32_MAX / 2) die("%s: too many dynamic symbols", module->name);
    while (capacity < module->nsym * 2) {
        if (capacity > UINT32_MAX / 2) die("%s: symbol hash overflow", module->name);
        capacity <<= 1;
    }
    module->symh = checked_calloc(capacity, sizeof *module->symh);
    module->symh_mask = capacity - 1;
    for (unsigned index = 1; index < module->nsym; index++) {
        const Elf64_Sym *symbol = &module->symtab[index];
        if (!symbol->st_shndx) continue;
        const char *name = module_string(module, symbol->st_name);
        for (unsigned slot = symbol_hash(name) & module->symh_mask;; slot = (slot + 1) & module->symh_mask) {
            if (!module->symh[slot]) {
                module->symh[slot] = index;
                break;
            }
            const char *existing = module_string(module, module->symtab[module->symh[slot]].st_name);
            if (!strcmp(name, existing)) break;
        }
    }
}

static void parse_dynamic(module_t *module) {
    module_range(module, 4, 4, "MOD0 pointer");
    uint32_t mod0 = read_u32(module->base + 4);
    module_range(module, mod0, 24, "MOD0");
    if (memcmp(module->base + mod0, "MOD0", 4)) die("%s: missing MOD0", module->name);
    int64_t dynamic_offset = (int64_t)mod0 + read_i32(module->base + mod0 + 4);
    if (dynamic_offset < 0) die("%s: negative dynamic offset", module->name);
    module_range(module, (uint64_t)dynamic_offset, 16, "dynamic table");
    module->dyn = (const Elf64_Dyn *)(module->base + dynamic_offset);

    const Elf64_Dyn *entry = module->dyn;
    for (;; entry++) {
        uint64_t offset = (uint64_t)((const uint8_t *)entry - module->base);
        module_range(module, offset, 16, "dynamic entry");
        if (entry->d_tag == 0) break;
        if (entry->d_tag == DT_SYMENT && entry->d_un.d_val != sizeof(Elf64_Sym)) die("%s: unsupported SYMENT", module->name);
        if (entry->d_tag == DT_RELAENT && entry->d_un.d_val != sizeof(Elf64_Rela)) die("%s: unsupported RELAENT", module->name);
        if (entry->d_tag == DT_PLTREL && entry->d_un.d_val != DT_RELA) die("%s: JMPREL is not RELA", module->name);
    }
    uint64_t symtab_offset = 0, strtab_offset = 0;
    for (entry = module->dyn; entry->d_tag; entry++) {
        if (entry->d_tag == DT_SYMTAB) symtab_offset = entry->d_un.d_ptr;
        if (entry->d_tag == DT_STRTAB) strtab_offset = entry->d_un.d_ptr;
        if (entry->d_tag == DT_INIT) module->init = entry->d_un.d_ptr;
        if (entry->d_tag == DT_INIT_ARRAY) module->init_array = entry->d_un.d_ptr;
        if (entry->d_tag == DT_INIT_ARRAYSZ) module->init_arraysz = entry->d_un.d_val;
    }
    if (module->init) module_range(module, module->init, 4, "DT_INIT");
    if (module->init_arraysz % 8) die("%s: invalid DT_INIT_ARRAYSZ", module->name);
    if (module->init_array || module->init_arraysz)
        module_range(module, module->init_array, module->init_arraysz, "DT_INIT_ARRAY");
    if (!symtab_offset || !strtab_offset || strtab_offset <= symtab_offset ||
        (strtab_offset - symtab_offset) % sizeof(Elf64_Sym))
        die("%s: invalid dynamic symbol layout", module->name);
    module_range(module, symtab_offset, strtab_offset - symtab_offset, "dynamic symbols");
    module_range(module, strtab_offset, 1, "dynamic strings");
    module->symtab = (const Elf64_Sym *)(module->base + symtab_offset);
    module->strtab = (const char *)module->base + strtab_offset;
    module->nsym = (unsigned)((strtab_offset - symtab_offset) / sizeof(Elf64_Sym));
    if (!module->nsym) die("%s: empty dynamic symbol table", module->name);
    build_symhash(module);
}

static void *module_sym_linear(module_t *module, const char *name) {
    for (unsigned index = 1; index < module->nsym; index++) {
        const Elf64_Sym *symbol = &module->symtab[index];
        if (symbol->st_shndx && !strcmp(module_string(module, symbol->st_name), name))
            return module->base + symbol->st_value;
    }
    return NULL;
}

void *module_sym(module_t *module, const char *name) {
    if (!module->symh) return module_sym_linear(module, name);
    for (unsigned slot = symbol_hash(name) & module->symh_mask; module->symh[slot]; slot = (slot + 1) & module->symh_mask) {
        const Elf64_Sym *symbol = &module->symtab[module->symh[slot]];
        if (!strcmp(module_string(module, symbol->st_name), name)) return module->base + symbol->st_value;
    }
    return NULL;
}

void *guest_sym(const char *name) {
    for (int index = 0; index < g_nmod; index++) {
        void *result = module_sym(&g_mod[index], name);
        if (result) return result;
    }
    return NULL;
}

module_t *module_of(const void *pointer) {
    for (int index = 0; index < g_nmod; index++) {
        const uint8_t *address = pointer;
        if (address >= g_mod[index].base && address < g_mod[index].base + g_mod[index].size) return &g_mod[index];
    }
    return NULL;
}

typedef struct { const char *name, *why; void *function; } hle_force_ent;
static const hle_force_ent *g_force;
static unsigned g_nforce, *g_force_hits;

static void force_init(void) {
    unsigned long size = 0;
    g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &size);
    g_nforce = g_force ? (unsigned)(size / sizeof *g_force) : 0;
    if (g_force && size % sizeof *g_force) die("invalid swforce section size");
    g_force_hits = checked_calloc(g_nforce + 1, sizeof *g_force_hits);
    for (unsigned first = 0; first < g_nforce; first++) {
        if (!g_force[first].name || !*g_force[first].name || !g_force[first].function || !g_force[first].why)
            die("invalid forced entry %u", first);
        for (unsigned second = first + 1; second < g_nforce; second++)
            if (!strcmp(g_force[first].name, g_force[second].name)) die("duplicate forced entry %s", g_force[first].name);
    }
}

static void *force_lookup(const char *name) {
    for (unsigned index = 0; index < g_nforce; index++) {
        if (!strcmp(g_force[index].name, name)) {
            g_force_hits[index]++;
            return g_force[index].function;
        }
    }
    return NULL;
}

static void force_check(void) {
    unsigned unused = 0;
    for (unsigned index = 0; index < g_nforce; index++) {
        if (!g_force_hits[index]) {
            LOG("unused forced entry %s", g_force[index].name);
            unused++;
        }
    }
    LOG("FORCE entries=%u unused=%u", g_nforce, unused);
    if (unused && sky_profile_has_update_ui()) die("unused forced-entry check failed");
}

uint64_t resolve_import(module_t *module, const char *name, int weak) {
    if (weak) return 0;
    die("unresolved non-weak import %s required by %s", name, module->name);
}

static uint64_t resolve_sym(module_t *module, const char *name, int weak) {
    for (int index = 0; index < g_nmod; index++) {
        void *result = module_sym(&g_mod[index], name);
        if (result) return (uint64_t)result;
    }
    return resolve_import(module, name, weak);
}

static void register_gates(module_t *module, const char *dir) {
    FILE *file = open_module_file(dir, module->name, "gates.tsv", "r");
    char line[256];
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') && !feof(file)) die("%s: gate line too long", module->name);
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        unsigned long long return_address, original;
        char kind[16], extra[2];
        int argument;
        if (sscanf(line, "%llx %15s %d %llx %1s", &return_address, kind, &argument, &original, extra) != 4)
            die("%s: malformed gate row: %s", module->name, line);
        if (return_address < module->patch_off || return_address >= module->file_size || original >= module->text_end)
            die("%s: gate addresses out of range: %s", module->name, line);
        gate_register(module->base ? (uint64_t)module->base + return_address : return_address,
                      kind, argument, original);
        module->gates++;
    }
    if (ferror(file) || fclose(file)) die("%s: reading gates.tsv failed", module->name);
    LOG("GATES %s count=%u", module->name, module->gates);
}

static void relocation_tables(module_t *module, const Elf64_Rela **tables, uint64_t *sizes) {
    uint64_t pointers[2] = {0, 0};
    sizes[0] = sizes[1] = 0;
    for (const Elf64_Dyn *entry = module->dyn; entry->d_tag; entry++) {
        if (entry->d_tag == DT_RELA) pointers[0] = entry->d_un.d_ptr;
        if (entry->d_tag == DT_RELASZ) sizes[0] = entry->d_un.d_val;
        if (entry->d_tag == DT_JMPREL) pointers[1] = entry->d_un.d_ptr;
        if (entry->d_tag == DT_PLTRELSZ) sizes[1] = entry->d_un.d_val;
    }
    for (int table = 0; table < 2; table++) {
        if (sizes[table] && !pointers[table]) die("%s: relocation table %d lacks pointer", module->name, table);
        if (!sizes[table]) continue;
        if (sizes[table] % sizeof(Elf64_Rela)) die("%s: relocation table %d has invalid size", module->name, table);
        module_range(module, pointers[table], sizes[table], "relocation table");
        tables[table] = (const Elf64_Rela *)(module->base + pointers[table]);
    }
}

static void relocate(module_t *module) {
    const Elf64_Rela *tables[2] = {NULL, NULL};
    uint64_t sizes[2];
    relocation_tables(module, tables, sizes);
    for (int table = 0; table < 2; table++) {
        for (uint64_t index = 0; index < sizes[table] / sizeof(Elf64_Rela); index++) {
            const Elf64_Rela *relocation = &tables[table][index];
            unsigned type = (unsigned)ELF64_R_TYPE(relocation->r_info);
            int kind;
            switch (type) {
            case R_AARCH64_RELATIVE: kind = RELOC_RELATIVE; break;
            case R_AARCH64_ABS64: kind = RELOC_ABS64; break;
            case R_AARCH64_GLOB_DAT: kind = RELOC_GLOB_DAT; break;
            case R_AARCH64_JUMP_SLOT: kind = RELOC_JUMP_SLOT; break;
            default:
                die("unsupported relocation type %u in %s table %d index %llu offset %#llx", type,
                    module->name, table, (unsigned long long)index, (unsigned long long)relocation->r_offset);
            }
            if (relocation->r_offset % 8 || relocation->r_offset > module->size ||
                relocation->r_offset + 8 > module->size)
                die("%s: relocation target out of bounds %#llx", module->name,
                    (unsigned long long)relocation->r_offset);
#if TARGET_OS_IPHONE || defined(SKY_IOS_EMBEDDED_LOADER_TEST)
            if (relocation->r_offset < module->data_off ||
                relocation->r_offset >= module->key_page_off ||
                module->key_page_off - relocation->r_offset < 8)
                die("%s: relocation target is not writable guest data %#llx", module->name,
                    (unsigned long long)relocation->r_offset);
#endif
            uint8_t *where_bytes = module->base + relocation->r_offset;
            if (type == R_AARCH64_RELATIVE) {
                write_u64(where_bytes, (uint64_t)module->base + (uint64_t)relocation->r_addend);
                module->reloc_count[kind]++;
                module->relocations++;
                continue;
            }

            uint64_t symbol_index = ELF64_R_SYM(relocation->r_info);
            if (!symbol_index || symbol_index >= module->nsym) die("%s: invalid relocation symbol index %llu",
                module->name, (unsigned long long)symbol_index);
            const Elf64_Sym *symbol = &module->symtab[symbol_index];
            const char *name = module_string(module, symbol->st_name);
            int weak = (symbol->st_info >> 4) == 2;
            uint64_t value = (uint64_t)force_lookup(name);
            if (!value) value = symbol->st_shndx ? (uint64_t)module->base + symbol->st_value
                                                  : resolve_sym(module, name, weak);
            if (!value && !symbol->st_shndx && weak) module->weak_null++;
            if (type == R_AARCH64_JUMP_SLOT) write_u64(where_bytes, value);
            else write_u64(where_bytes, value + (uint64_t)relocation->r_addend);
            module->reloc_count[kind]++;
            module->relocations++;
        }
    }
    LOG("RELOC %s relative=%llu abs64=%llu glob_dat=%llu jump_slot=%llu total=%llu weak_null=%llu",
        module->name,
        (unsigned long long)module->reloc_count[RELOC_RELATIVE],
        (unsigned long long)module->reloc_count[RELOC_ABS64],
        (unsigned long long)module->reloc_count[RELOC_GLOB_DAT],
        (unsigned long long)module->reloc_count[RELOC_JUMP_SLOT],
        (unsigned long long)module->relocations, (unsigned long long)module->weak_null);
}

void load_modules(const char *dir, const char *const *names, int count) {
    if (count <= 0 || count > MAX_MOD) die("invalid module count %d", count);
    g_nmod = 0;
    for (int index = 0; index < count; index++) {
        if (snprintf(g_mod[index].name, sizeof g_mod[index].name, "%s", names[index]) >= (int)sizeof g_mod[index].name)
            die("module name too long: %s", names[index]);
        read_layout(&g_mod[index], dir);
    }
    sky_profile_configure_main(NULL, NULL);
    for (int index = 0; index < count; index++) {
        module_t *module = &g_mod[index];
        if (!strcmp(module->name, "main"))
            sky_profile_configure_main(module->build_id, module->has_save_config ? &module->save_config : NULL);
    }
    LOG("BUILD_PROFILE ui=%s unused-host-handlers=%s",
        sky_profile_has_update_ui() ? "verified-v327680" : "native-controller-fallback",
        sky_profile_has_update_ui() ? "fatal" : "allowed");
    create_keys();
    for (int index = 0; index < count; index++) {
        module_t *module = &g_mod[index];
        map_image(module, dir);
        if (index == 0) tsd_check();
        parse_dynamic(module);
        register_gates(module, dir);
        g_nmod++;
        LOG("MAP %s base=%p size=%#zx file=%#zx text=rx data=rw key_page=r patch=rx",
            module->name, module->base, module->size, module->file_size);
    }
    force_init();
    for (int index = 0; index < count; index++) relocate(&g_mod[index]);
    force_check();
    unsigned gates = 0;
    for (int index = 0; index < count; index++) gates += g_mod[index].gates;
    LOG("LOAD_OK modules=%d gates=%u", count, gates);
}

static uint64_t loadhash_name(const char *name) {
    uint64_t hash = 0xcbf29ce484222325ull;
    while (*name) hash = (hash ^ (uint8_t)*name++) * 0x100000001b3ull;
    return hash | 1ull << 63;
}

void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int module_index = 0; module_index < g_nmod; module_index++) {
        module_t *module = &g_mod[module_index];
        if (module->size > UINT32_MAX) die("%s: load hash image too large", module->name);
        uint8_t *copy = checked_malloc(module->size);
        memcpy(copy, module->base, module->size);
        const Elf64_Rela *tables[2] = {NULL, NULL};
        uint64_t sizes[2];
        relocation_tables(module, tables, sizes);
        for (int table = 0; table < 2; table++) {
            for (uint64_t index = 0; index < sizes[table] / sizeof(Elf64_Rela); index++) {
                const Elf64_Rela *relocation = &tables[table][index];
                unsigned type = (unsigned)ELF64_R_TYPE(relocation->r_info);
                if (type != R_AARCH64_RELATIVE && type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
                    type != R_AARCH64_JUMP_SLOT)
                    die("unsupported relocation type %u during load hash", type);
                uint64_t token = 0;
                if (type != R_AARCH64_RELATIVE) {
                    uint64_t value = read_u64(module->base + relocation->r_offset);
                    module_t *target = module_of((const void *)value);
                    if (!target) {
                        uint64_t symbol_index = ELF64_R_SYM(relocation->r_info);
                        const Elf64_Sym *symbol = &module->symtab[symbol_index];
                        token = loadhash_name(module_string(module, symbol->st_name));
                    }
                }
                memcpy(copy + relocation->r_offset, &token, sizeof token);
            }
        }

        CC_SHA256_CTX context;
        CC_SHA256_Init(&context);
        CC_SHA256_Update(&context, copy, (CC_LONG)module->size);
        for (int table = 0; table < 2; table++) {
            for (uint64_t index = 0; index < sizes[table] / sizeof(Elf64_Rela); index++) {
                const Elf64_Rela *relocation = &tables[table][index];
                uint64_t tail = (uint64_t)relocation->r_addend;
                if (ELF64_R_TYPE(relocation->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t value = read_u64(module->base + relocation->r_offset);
                    module_t *target = module_of((const void *)value);
                    if (target) tail = value - (uint64_t)target->base;
                }
                CC_SHA256_Update(&context, &tail, sizeof tail);
            }
        }
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(digest, &context);
        free(copy);
        char hexadecimal[CC_SHA256_DIGEST_LENGTH * 2 + 1];
        for (int index = 0; index < CC_SHA256_DIGEST_LENGTH; index++)
            snprintf(hexadecimal + index * 2, 3, "%02x", digest[index]);
        LOG("LOADHASH %s size=%#zx relocations=%llu sha256=%s", module->name, module->size,
            (unsigned long long)module->relocations, hexadecimal);
    }
}
