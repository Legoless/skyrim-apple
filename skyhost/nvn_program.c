#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include <CommonCrypto/CommonCrypto.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SKY_NVN_PROGRAM_MAGIC(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define SKY_NVN_PROGRAM_MAX_STAGES 6
#define SKY_NVN_CONTROL_BYTES 0x880
#define SKY_NVN_MAX_CODE_BYTES (64u << 20)
#define SKY_NVN_CAPTURE_PATH_BYTES 4096

typedef struct sky_nvn_buffer_link {
    struct sky_nvn_buffer_link *next;
    const void *owner;
    uint64_t address, size;
    uint8_t *mem;
    int exact;
} sky_nvn_buffer_link_t;

static sky_nvn_buffer_link_t *g_buffer_links, *g_pool_links;
static pthread_mutex_t g_buffer_link_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_capture_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_capture_sequence;

enum {
    SKY_SHADER_FORMAT_UNKNOWN,
    SKY_SHADER_FORMAT_MAXWELL_NVN_SPH,
    SKY_SHADER_FORMAT_MAXWELL_NVN_COMPUTE,
    SKY_SHADER_FORMAT_MAXWELL_RAW_COMPUTE,
    SKY_SHADER_FORMAT_DXBC,
    SKY_SHADER_FORMAT_SPIRV,
    SKY_SHADER_FORMAT_BTCD_SIGNATURE,
    SKY_SHADER_FORMAT_SHAD_SIGNATURE,
};

typedef struct {
    int stage, code_format;
    uint64_t data_address;
    size_t code_size, control_size;
    uint32_t code_magic, control_magic;
    uint32_t program_start, program_length, constant_offset, constant_size, workgroup[3];
    uint8_t *code, *control;
    char code_sha256[65], control_sha256[65], shader_key[65];
} sky_nvn_shader_capture_t;

typedef struct {
    sky_nvn_shader_capture_t entries[SKY_NVN_PROGRAM_MAX_STAGES];
} sky_nvn_program_state_t;

typedef struct sky_nvn_program_link {
    struct sky_nvn_program_link *next;
    uint64_t serial;
    sky_nvn_program_state_t *state;
} sky_nvn_program_link_t;

static sky_nvn_program_link_t *g_program_links;
static pthread_mutex_t g_program_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_next_program_serial = 1;
static uint64_t g_missing_shader_count;

static sky_nvn_program_state_t *sky_nvn_program_state_take(uint64_t serial) {
    if (!serial) return NULL;
    sky_nvn_program_state_t *state = NULL;
    pthread_mutex_lock(&g_program_lock);
    sky_nvn_program_link_t **link = &g_program_links;
    while (*link) {
        if ((*link)->serial != serial) {
            link = &(*link)->next;
            continue;
        }
        sky_nvn_program_link_t *entry = *link;
        *link = entry->next;
        state = entry->state;
        free(entry);
        break;
    }
    pthread_mutex_unlock(&g_program_lock);
    return state;
}

static int sky_nvn_program_state_put(uint64_t serial, sky_nvn_program_state_t *state) {
    sky_nvn_program_link_t *entry = calloc(1, sizeof *entry);
    if (!entry) return 0;
    entry->serial = serial;
    entry->state = state;
    pthread_mutex_lock(&g_program_lock);
    entry->next = g_program_links;
    g_program_links = entry;
    pthread_mutex_unlock(&g_program_lock);
    return 1;
}

static void sky_nvn_link_untrack_locked(sky_nvn_buffer_link_t **list, const void *owner) {
    sky_nvn_buffer_link_t **link = list;
    while (*link) {
        if ((*link)->owner != owner) {
            link = &(*link)->next;
            continue;
        }
        sky_nvn_buffer_link_t *entry = *link;
        *link = entry->next;
        free(entry);
    }
}

static int sky_nvn_link_track(sky_nvn_buffer_link_t **list, const void *owner,
                              uint64_t address, uint64_t size, uint8_t *mem, int exact) {
    if (!owner || !mem || !size || address > UINT64_MAX - size) return 0;
    sky_nvn_buffer_link_t *entry = calloc(1, sizeof *entry);
    if (!entry) return 0;
    entry->owner = owner;
    entry->address = address;
    entry->size = size;
    entry->mem = mem;
    entry->exact = exact;
    pthread_mutex_lock(&g_buffer_link_lock);
    sky_nvn_link_untrack_locked(list, owner);
    entry->next = *list;
    *list = entry;
    pthread_mutex_unlock(&g_buffer_link_lock);
    return 1;
}

int sky_nvn_buffer_track(const sky_nvn_buffer_t *buffer) {
    if (!buffer || !buffer->pool || !buffer->mem || !buffer->size || buffer->offset < 0) return 0;
    const sky_nvn_memory_pool_t *pool = buffer->pool;
    if ((uint64_t)buffer->offset > UINT64_MAX - pool->buffer_address) return 0;
    return sky_nvn_link_track(&g_buffer_links, buffer, pool->buffer_address + (uint64_t)buffer->offset,
                              buffer->size, buffer->mem, 1);
}

void sky_nvn_buffer_untrack(const void *buffer) {
    pthread_mutex_lock(&g_buffer_link_lock);
    sky_nvn_link_untrack_locked(&g_buffer_links, buffer);
    pthread_mutex_unlock(&g_buffer_link_lock);
}

int sky_nvn_memory_pool_track(const sky_nvn_memory_pool_t *pool) {
    if (!pool || pool->magic != SKY_NVN_PROGRAM_MAGIC('P', 'O', 'O', 'L') || !pool->mem ||
        !pool->buffer_address || !pool->size) return 0;
    return sky_nvn_link_track(&g_pool_links, pool, pool->buffer_address, pool->size, pool->mem, 0);
}

void sky_nvn_memory_pool_untrack(const void *pool) {
    pthread_mutex_lock(&g_buffer_link_lock);
    sky_nvn_link_untrack_locked(&g_pool_links, pool);
    pthread_mutex_unlock(&g_buffer_link_lock);
}

static int sky_nvn_resolve_list(sky_nvn_buffer_link_t *list, uint64_t address,
                                uint8_t **mem, size_t *size, int *exact) {
    for (sky_nvn_buffer_link_t *entry = list; entry; entry = entry->next) {
        if (address < entry->address) continue;
        uint64_t offset = address - entry->address;
        if (offset >= entry->size || entry->size - offset > SIZE_MAX) continue;
        *mem = entry->mem + offset;
        *size = (size_t)(entry->size - offset);
        *exact = entry->exact;
        return 1;
    }
    return 0;
}

static int sky_nvn_buffer_resolve(uint64_t address, uint8_t **mem, size_t *size, int *exact) {
    int found = 0;
    pthread_mutex_lock(&g_buffer_link_lock);
    found = sky_nvn_resolve_list(g_buffer_links, address, mem, size, exact) ||
            sky_nvn_resolve_list(g_pool_links, address, mem, size, exact);
    pthread_mutex_unlock(&g_buffer_link_lock);
    return found;
}

static int sky_nvn_guest_read(const void *source, void *destination, size_t size) {
    if (!source || !destination || !size || size > UINT32_MAX) return 0;
    vm_size_t copied = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(), (vm_address_t)source, (vm_size_t)size,
                                             (vm_address_t)destination, &copied);
    return result == KERN_SUCCESS && copied == size;
}

static uint32_t sky_nvn_read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void sky_nvn_sha256(const uint8_t *bytes, size_t size, char hexadecimal[65]) {
    CC_SHA256_CTX context;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, bytes, (CC_LONG)size);
    CC_SHA256_Final(digest, &context);
    for (int index = 0; index < CC_SHA256_DIGEST_LENGTH; index++)
        snprintf(hexadecimal + index * 2, 3, "%02x", digest[index]);
}

static void sky_nvn_shader_key(sky_nvn_shader_capture_t *capture) {
    /* Match tools/shaders.py: mutable control pointers are not shader identity. */
    uint32_t layout[8] = {(uint32_t)capture->stage, capture->program_start,
        capture->program_length, capture->constant_offset, capture->constant_size, 0, 0, 0};
    if (capture->stage == 5)
        memcpy(layout + 5, capture->workgroup, sizeof capture->workgroup);
    uint8_t encoded[sizeof layout];
    for (size_t index = 0; index < sizeof layout / sizeof layout[0]; index++)
        for (size_t byte = 0; byte < 4; byte++) encoded[index * 4 + byte] = (uint8_t)(layout[index] >> (byte * 8));
    CC_SHA256_CTX context;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, capture->code, (CC_LONG)capture->code_size);
    CC_SHA256_Update(&context, encoded, sizeof encoded);
    CC_SHA256_Final(digest, &context);
    for (int index = 0; index < CC_SHA256_DIGEST_LENGTH; index++)
        snprintf(capture->shader_key + index * 2, 3, "%02x", digest[index]);
}

static int sky_nvn_sph_stage(uint32_t sph_word) {
    static const int stages[6] = {-1, 0, 3, 4, 2, 1};
    unsigned value = (sph_word >> 10) & 0xf;
    return value < 6 ? stages[value] : -1;
}

static const char *sky_nvn_stage_name(int stage) {
    static const char *const names[SKY_NVN_PROGRAM_MAX_STAGES] = {
        "vertex", "fragment", "geometry", "tess-control", "tess-evaluation", "compute",
    };
    return stage >= 0 && stage < SKY_NVN_PROGRAM_MAX_STAGES ? names[stage] : "unknown";
}

static const char *sky_nvn_code_format_name(int format) {
    switch (format) {
        case SKY_SHADER_FORMAT_MAXWELL_NVN_SPH: return "maxwell-nvn-sph";
        case SKY_SHADER_FORMAT_MAXWELL_NVN_COMPUTE: return "maxwell-nvn-compute";
        case SKY_SHADER_FORMAT_MAXWELL_RAW_COMPUTE: return "maxwell-raw-compute";
        case SKY_SHADER_FORMAT_DXBC: return "dxbc-intermediate";
        case SKY_SHADER_FORMAT_SPIRV: return "spirv-intermediate";
        case SKY_SHADER_FORMAT_BTCD_SIGNATURE: return "btcd-signature";
        case SKY_SHADER_FORMAT_SHAD_SIGNATURE: return "shad-signature";
        default: return "unknown";
    }
}

static void sky_nvn_program_state_free(sky_nvn_program_state_t *state) {
    if (!state) return;
    for (int index = 0; index < SKY_NVN_PROGRAM_MAX_STAGES; index++) {
        free(state->entries[index].code);
        free(state->entries[index].control);
    }
    free(state);
}

static int sky_nvn_capture_layout(sky_nvn_shader_capture_t *capture, const char **reason) {
    const uint8_t *control = capture->control;
    capture->program_length = sky_nvn_read_u32(control + 0x6f8);
    capture->constant_size = sky_nvn_read_u32(control + 0x6fc);
    capture->constant_offset = sky_nvn_read_u32(control + 0x700);
    capture->program_start = sky_nvn_read_u32(control + 0x708);
    for (int index = 0; index < 3; index++)
        capture->workgroup[index] = sky_nvn_read_u32(control + 0x748 + index * 4);
    if (sky_nvn_read_u32(control + 0x704) != capture->code_size) {
        *reason = "code-size-mismatch";
        return 0;
    }
    if (!capture->program_length || (capture->program_length & 7) ||
        capture->program_start > capture->code_size ||
        capture->program_length > capture->code_size - capture->program_start) {
        *reason = "invalid-program-range";
        return 0;
    }
    size_t program_end = (size_t)capture->program_start + capture->program_length;
    if (program_end > SIZE_MAX - 0xff) {
        *reason = "invalid-program-range";
        return 0;
    }
    size_t expected_constant = (program_end + 0xff) & ~(size_t)0xff;
    if (capture->constant_offset != expected_constant) {
        *reason = "constant-offset-mismatch";
        return 0;
    }
    if (expected_constant > SIZE_MAX - capture->constant_size - 0xff) {
        *reason = "invalid-constant-range";
        return 0;
    }
    size_t expected_size = (expected_constant + capture->constant_size + 0xff) & ~(size_t)0xff;
    if (expected_size != capture->code_size) {
        *reason = "code-block-layout-mismatch";
        return 0;
    }
    return 1;
}

static int sky_nvn_capture_one(const sky_nvn_shader_data_t *input, sky_nvn_shader_capture_t *capture,
                               const char **reason) {
    uint8_t *code = NULL;
    size_t available = 0;
    int exact = 0;
    capture->stage = -1;
    capture->code_format = SKY_SHADER_FORMAT_UNKNOWN;
    if (!input->control) {
        *reason = "null-control";
        return 0;
    }
    if (!sky_nvn_buffer_resolve(input->data, &code, &available, &exact)) {
        *reason = "unregistered-code-address";
        return 0;
    }
    uint8_t control_header[4];
    if (!sky_nvn_guest_read(input->control, control_header, sizeof control_header)) {
        *reason = "unreadable-control-header";
        return 0;
    }
    capture->control_magic = sky_nvn_read_u32(control_header);
    if (capture->control_magic != 0x98761234) {
        *reason = "unsupported-control-magic";
        return 0;
    }
    capture->control = malloc(SKY_NVN_CONTROL_BYTES);
    if (!capture->control) {
        *reason = "control-allocation-failed";
        return 0;
    }
    if (!sky_nvn_guest_read(input->control, capture->control, SKY_NVN_CONTROL_BYTES)) {
        *reason = "truncated-or-unreadable-control";
        return 0;
    }
    capture->control_size = SKY_NVN_CONTROL_BYTES;
    size_t code_size = sky_nvn_read_u32(capture->control + 0x704);
    if (code_size < 4 || code_size > SKY_NVN_MAX_CODE_BYTES) {
        *reason = "invalid-code-size";
        return 0;
    }
    uint8_t code_header[4];
    if (!sky_nvn_guest_read(code, code_header, sizeof code_header)) {
        *reason = "unreadable-code";
        return 0;
    }
    capture->code_magic = sky_nvn_read_u32(code_header);
    if (code_size > available) {
        *reason = capture->code_magic == 0x12345678 && available < 0x80
            ? "truncated-maxwell-header" : "truncated-code";
        return 0;
    }
    if (exact && code_size != available) {
        *reason = "code-size-mismatch";
        return 0;
    }
    capture->code = malloc(code_size);
    if (!capture->code) {
        *reason = "code-allocation-failed";
        return 0;
    }
    if (!sky_nvn_guest_read(code, capture->code, code_size)) {
        *reason = "unreadable-code";
        return 0;
    }
    capture->data_address = input->data;
    capture->code_size = code_size;
    if (capture->code_magic == 0x12345678) capture->code_format = SKY_SHADER_FORMAT_MAXWELL_NVN_SPH;
    else if (capture->code_magic == 0x12345679) capture->code_format = SKY_SHADER_FORMAT_MAXWELL_NVN_COMPUTE;
    else if (capture->code_magic == 0x07230203) capture->code_format = SKY_SHADER_FORMAT_SPIRV;
    else if (capture->code_magic == 0x43425844) capture->code_format = SKY_SHADER_FORMAT_DXBC;
    else if (capture->code_magic == 0x44435442) capture->code_format = SKY_SHADER_FORMAT_BTCD_SIGNATURE;
    else if (capture->code_magic == 0x44414853) capture->code_format = SKY_SHADER_FORMAT_SHAD_SIGNATURE;
    if (capture->code_format == SKY_SHADER_FORMAT_MAXWELL_NVN_SPH && code_size < 0x80) {
        *reason = "truncated-maxwell-header";
        return 0;
    }
    sky_nvn_sha256(capture->code, capture->code_size, capture->code_sha256);
    sky_nvn_sha256(capture->control, capture->control_size, capture->control_sha256);
    if (capture->code_format != SKY_SHADER_FORMAT_UNKNOWN &&
        capture->code_format != SKY_SHADER_FORMAT_MAXWELL_NVN_SPH &&
        capture->code_format != SKY_SHADER_FORMAT_MAXWELL_NVN_COMPUTE) {
        *reason = "unsupported-code-format";
        return -1;
    }
    if (!sky_nvn_capture_layout(capture, reason)) return 0;
    if (capture->code_format == SKY_SHADER_FORMAT_MAXWELL_NVN_SPH) {
        if (capture->program_start != 0x30) {
            *reason = "invalid-graphics-start";
            return 0;
        }
        for (size_t index = 4; index < 0x30; index++) {
            if (capture->code[index]) {
                *reason = "invalid-graphics-header";
                return 0;
            }
        }
        capture->stage = sky_nvn_sph_stage(sky_nvn_read_u32(capture->code + 0x30));
        if (capture->stage < 0) {
            *reason = "invalid-sph-stage";
            return 0;
        }
    } else if (capture->code_format == SKY_SHADER_FORMAT_MAXWELL_NVN_COMPUTE) {
        if (capture->program_start != 0x100) {
            *reason = "invalid-compute-start";
            return 0;
        }
        for (size_t index = 4; index < 0x100; index++) {
            if (capture->code[index]) {
                *reason = "invalid-compute-header";
                return 0;
            }
        }
        capture->stage = 5;
    } else if (capture->program_start == 0 && capture->workgroup[0] && capture->workgroup[1] &&
               capture->workgroup[2]) {
        capture->code_format = SKY_SHADER_FORMAT_MAXWELL_RAW_COMPUTE;
        capture->stage = 5;
    } else {
        *reason = "unsupported-code-magic";
        return -1;
    }
    if (capture->stage == 5 &&
        (!capture->workgroup[0] || !capture->workgroup[1] || !capture->workgroup[2])) {
        *reason = "invalid-workgroup";
        return 0;
    }
    sky_nvn_shader_key(capture);
    return 1;
}

static int sky_nvn_mkdir_p(const char *path) {
    char copy[SKY_NVN_CAPTURE_PATH_BYTES];
    size_t length = strlen(path);
    if (!length || length >= sizeof copy) return 0;
    memcpy(copy, path, length + 1);
    for (char *cursor = copy + 1; ; cursor++) {
        if (*cursor != '/' && *cursor) continue;
        char saved = *cursor;
        *cursor = 0;
        if (*copy && mkdir(copy, 0700) && errno != EEXIST) return 0;
        struct stat status;
        if (stat(copy, &status) || !S_ISDIR(status.st_mode)) return 0;
        if (saved) *cursor = saved;
        if (!saved) break;
    }
    return 1;
}

static int sky_nvn_write_file(const char *path, const void *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int valid = !size || fwrite(bytes, 1, size, file) == size;
    if (fclose(file)) valid = 0;
    return valid;
}

static int sky_nvn_capture_write(const void *program, const sky_nvn_program_state_t *state, int count,
                                 int accepted) {
    const char *enabled = getenv("SKY_SHADER_CAPTURE");
    const char *missing_enabled = getenv("SKY_SHADER_CAPTURE_MISSING");
    int missing_only = (!enabled || strcmp(enabled, "1")) && missing_enabled && !strcmp(missing_enabled, "1");
    if ((!enabled || strcmp(enabled, "1")) && !missing_only) {
        pthread_mutex_lock(&g_capture_lock);
        g_capture_sequence += (uint64_t)count;
        pthread_mutex_unlock(&g_capture_lock);
        return 1;
    }
    const char *directory = getenv("SKY_SHADER_CAPTURE_DIR");
    if (!directory) directory = "skyrim-work/build/shader-captures";
    pthread_mutex_lock(&g_capture_lock);
    int valid = *directory && sky_nvn_mkdir_p(directory);
    char manifest[SKY_NVN_CAPTURE_PATH_BYTES];
    if (valid && snprintf(manifest, sizeof manifest, "%s/manifest.tsv", directory) >= (int)sizeof manifest)
        valid = 0;
    for (int index = 0; valid && index < count; index++) {
        g_capture_sequence++;
        const sky_nvn_shader_capture_t *capture = &state->entries[index];
        if (missing_only && accepted && capture->shader_key[0]) {
            const char *cache = getenv("SKY_SHADER_CACHE_DIR");
            if (!cache) cache = "skyrim-work/build/shaders";
            char source[SKY_NVN_CAPTURE_PATH_BYTES], reflection[SKY_NVN_CAPTURE_PATH_BYTES];
            if (snprintf(source, sizeof source, "%s/%s.metal", cache, capture->shader_key) < (int)sizeof source &&
                snprintf(reflection, sizeof reflection, "%s/%s.json", cache, capture->shader_key) < (int)sizeof reflection &&
                !access(source, R_OK) && !access(reflection, R_OK))
                continue;
        }
        const char *stage = sky_nvn_stage_name(capture->stage);
        char base[160];
        int base_length = capture->shader_key[0]
            ? snprintf(base, sizeof base, "%s-%s", stage, capture->shader_key)
            : snprintf(base, sizeof base, "%s-%.16s-%.16s", stage,
                       capture->code_sha256, capture->control_sha256);
        char code_path[SKY_NVN_CAPTURE_PATH_BYTES];
        char control_path[SKY_NVN_CAPTURE_PATH_BYTES];
        char json_path[SKY_NVN_CAPTURE_PATH_BYTES];
        if (base_length <= 0 || (size_t)base_length >= sizeof base ||
            snprintf(code_path, sizeof code_path, "%s/%s-code.bin", directory, base) >= (int)sizeof code_path ||
            snprintf(control_path, sizeof control_path, "%s/%s-control.bin", directory, base) >= (int)sizeof control_path ||
            snprintf(json_path, sizeof json_path, "%s/%s.json", directory, base) >= (int)sizeof json_path) {
            valid = 0;
            break;
        }
        struct stat code_status, control_status, json_status;
        int exists = !stat(code_path, &code_status) && !stat(control_path, &control_status) &&
                     !stat(json_path, &json_status) &&
                     code_status.st_size == (off_t)capture->code_size &&
                     control_status.st_size == (off_t)capture->control_size && json_status.st_size > 0;
        if (exists) continue;
        if (!sky_nvn_write_file(code_path, capture->code, capture->code_size) ||
                        !sky_nvn_write_file(control_path, capture->control, capture->control_size)) {
            valid = 0;
            break;
        }
        FILE *json = NULL;
        if (!exists && !(json = fopen(json_path, "wb"))) {
            valid = 0;
            break;
        }
        int json_result = exists ? 0 : fprintf(json,
            "{\n"
            "  \"schema\": 1,\n"
            "  \"accepted\": %s,\n"
            "  \"stage\": \"%s\",\n"
            "  \"stage_ordinal\": %d,\n"
            "  \"data_address\": \"0x%llx\",\n"
            "  \"code_size\": %zu,\n"
            "  \"control_size\": %zu,\n"
            "  \"code_magic\": \"0x%08x\",\n"
            "  \"control_magic\": \"0x%08x\",\n"
            "  \"program_start\": %u,\n"
            "  \"program_length\": %u,\n"
            "  \"constant_offset\": %u,\n"
            "  \"constant_size\": %u,\n"
            "  \"workgroup\": [%u, %u, %u],\n"
            "  \"code_sha256\": \"%s\",\n"
            "  \"control_sha256\": \"%s\",\n"
            "  \"shader_key\": \"%s\",\n"
            "  \"container\": \"nvn-shader-data\",\n"
            "  \"code_format\": \"%s\",\n"
            "  \"control_format\": \"nvn-control-0x880\",\n"
            "  \"d2r_shad_btcd\": %s,\n"
            "  \"code_file\": \"%s-code.bin\",\n"
            "  \"control_file\": \"%s-control.bin\"\n"
            "}\n",
            accepted ? "true" : "false", stage, capture->stage,
            (unsigned long long)capture->data_address, capture->code_size, capture->control_size,
            capture->code_magic, capture->control_magic, capture->program_start, capture->program_length,
            capture->constant_offset, capture->constant_size, capture->workgroup[0], capture->workgroup[1],
            capture->workgroup[2], capture->code_sha256, capture->control_sha256, capture->shader_key,
            sky_nvn_code_format_name(capture->code_format),
            capture->code_format == SKY_SHADER_FORMAT_BTCD_SIGNATURE ||
            capture->code_format == SKY_SHADER_FORMAT_SHAD_SIGNATURE ? "true" : "false", base, base);
        int json_close = exists ? 0 : fclose(json);
        if (json_result < 0 || json_close) {
            valid = 0;
            break;
        }
        struct stat status;
        FILE *table = fopen(manifest, "a");
        if (!table) {
            valid = 0;
            break;
        }
        if (!stat(manifest, &status) && status.st_size == 0)
            fprintf(table, "stage\tcode_size\tcontrol_size\tcode_magic\tcontrol_magic\tcode_sha256\tcontrol_sha256\tjson\n");
        int row_result = fprintf(table, "%s\t%zu\t%zu\t0x%08x\t0x%08x\t%s\t%s\t%s\n",
            stage, capture->code_size, capture->control_size, capture->code_magic, capture->control_magic,
            capture->code_sha256, capture->control_sha256, json_path);
        int table_close = fclose(table);
        if (row_result < 0 || table_close) {
            valid = 0;
            break;
        }
        if (g_capture_sequence <= 8 || g_capture_sequence % 1024 == 0 || !accepted || capture->stage == 5)
            LOG("NVN_SHADER_CAPTURE program=%p stage=%s code_size=%#zx control_size=%#zx code_magic=%#x control_magic=%#x format=%s accepted=%d json=%s",
                program, stage, capture->code_size, capture->control_size,
                capture->code_magic, capture->control_magic, sky_nvn_code_format_name(capture->code_format),
                accepted, json_path);
    }
    pthread_mutex_unlock(&g_capture_lock);
    return valid;
}

uint32_t sky_nvn_program_initialize(void *program, void *device) {
    if (!program || !device || ((const sky_nvn_device_t *)device)->magic != 0x44455630) {
        LOG("NVN_PROGRAM_INIT_ERROR reason=invalid-device program=%p device=%p", program, device);
        return 0;
    }
    sky_nvn_program_t *out = program;
    if (out->magic == SKY_NVN_PROGRAM_MAGIC('P', 'R', 'O', 'G')) sky_nvn_program_finalize(out);
    memset(out, 0, sizeof *out);
    out->magic = SKY_NVN_PROGRAM_MAGIC('P', 'R', 'O', 'G');
    out->dev = device;
    sky_checkpoint("post_nnMain_nvn_program_initialize");
    LOG("NVN_PROGRAM_INITIALIZED program=%p device=%p origin_mode=%d depth_mode=%d", program, device,
        ((const sky_nvn_device_t *)device)->origin_mode, ((const sky_nvn_device_t *)device)->depth_mode);
    return 1;
}

void sky_nvn_program_finalize(void *program) {
    if (!program || ((sky_nvn_program_t *)program)->magic != SKY_NVN_PROGRAM_MAGIC('P', 'R', 'O', 'G')) return;
    sky_nvn_program_t *out = program;
    sky_mtl_program_remove(out->serial);
    sky_nvn_program_state_free(sky_nvn_program_state_take(out->serial));
    LOG("NVN_PROGRAM_FINALIZED program=%p captured_stages=%u", program, out->stage_count);
    memset(out, 0, sizeof *out);
}

void sky_nvn_program_set_debug_label(void *program, const char *label) {
    if (!program || ((sky_nvn_program_t *)program)->magic != SKY_NVN_PROGRAM_MAGIC('P', 'R', 'O', 'G')) return;
    ((sky_nvn_program_t *)program)->debug_label = label;
}

uint32_t sky_nvn_program_set_shaders(void *program, int count, const void *data) {
    if (!program || ((sky_nvn_program_t *)program)->magic != SKY_NVN_PROGRAM_MAGIC('P', 'R', 'O', 'G') ||
        count < 0 || count > SKY_NVN_PROGRAM_MAX_STAGES || (count > 0 && !data)) {
        LOG("NVN_PROGRAM_SET_SHADERS_ERROR program=%p count=%d reason=invalid-abi", program, count);
        return 0;
    }
    sky_nvn_program_t *out = program;
    if (!count) {
        sky_mtl_program_remove(out->serial);
        sky_nvn_program_state_free(sky_nvn_program_state_take(out->serial));
        out->serial = 0;
        out->stage_count = 0;
        out->stage_mask = 0;
        LOG("NVN_PROGRAM_SHADERS_SET program=%p count=0 stage_mask=0", program);
        return 1;
    }
    sky_nvn_shader_data_t inputs[SKY_NVN_PROGRAM_MAX_STAGES];
    if (!sky_nvn_guest_read(data, inputs, (size_t)count * sizeof inputs[0])) {
        LOG("NVN_PROGRAM_SET_SHADERS_ERROR program=%p count=%d reason=unreadable-shader-data", program, count);
        return 0;
    }
    sky_nvn_program_state_t *state = calloc(1, sizeof *state);
    if (!state) {
        LOG("NVN_PROGRAM_SET_SHADERS_ERROR program=%p count=%d reason=allocation-failed", program, count);
        return 0;
    }
    uint32_t stage_mask = 0;
    const char *reason = NULL;
    int failed = -1, diagnostic = 0;
    for (int index = 0; index < count; index++) {
        int status = sky_nvn_capture_one(&inputs[index], &state->entries[index], &reason);
        if (status <= 0) {
            failed = index;
            diagnostic = status < 0;
            break;
        }
        int stage = state->entries[index].stage;
        if (stage_mask & 1u << stage) {
            reason = "duplicate-stage";
            failed = index;
            break;
        }
        stage_mask |= 1u << stage;
    }
    if (failed >= 0) {
        if (diagnostic && !sky_nvn_capture_write(program, state, failed + 1, 0)) reason = "capture-io-failed";
        LOG("NVN_PROGRAM_SET_SHADERS_ERROR program=%p index=%d reason=%s data=%#llx control=%p",
            program, failed, reason ? reason : "capture-failed",
            (unsigned long long)inputs[failed].data, inputs[failed].control);
        sky_nvn_program_state_free(state);
        return 0;
    }
    if (!sky_nvn_capture_write(program, state, count, 1)) {
        LOG("NVN_PROGRAM_SET_SHADERS_ERROR program=%p count=%d reason=capture-io-failed", program, count);
        sky_nvn_program_state_free(state);
        return 0;
    }
    pthread_mutex_lock(&g_program_lock);
    uint64_t serial = g_next_program_serial++;
    pthread_mutex_unlock(&g_program_lock);
    if (!sky_nvn_program_state_put(serial, state)) {
        LOG("NVN_PROGRAM_SET_SHADERS_ERROR program=%p count=%d reason=tracking-allocation-failed", program, count);
        sky_nvn_program_state_free(state);
        return 0;
    }
    const char *cache = getenv("SKY_SHADER_CACHE_DIR");
    if (!cache) cache = "skyrim-work/build/shaders";
    for (int index = 0; index < count; index++) {
        const sky_nvn_shader_capture_t *capture = &state->entries[index];
        char source[SKY_NVN_CAPTURE_PATH_BYTES], reflection[SKY_NVN_CAPTURE_PATH_BYTES];
        if (snprintf(source, sizeof source, "%s/%s.metal", cache, capture->shader_key) >= (int)sizeof source ||
            snprintf(reflection, sizeof reflection, "%s/%s.json", cache, capture->shader_key) >= (int)sizeof reflection)
            continue;
        if (!access(source, R_OK) && !access(reflection, R_OK)) {
            if (!sky_mtl_program_set_shader(serial, capture->stage, source, reflection))
                LOG("NVN_PROGRAM_TRANSLATION_ERROR serial=%llu stage=%d key=%s", (unsigned long long)serial,
                    capture->stage, capture->shader_key);
        } else {
            pthread_mutex_lock(&g_program_lock);
            uint64_t missing = ++g_missing_shader_count;
            pthread_mutex_unlock(&g_program_lock);
            if (missing <= 8 || missing % 1024 == 0)
                LOG("NVN_PROGRAM_SHADER_CACHE_MISSING serial=%llu stage=%d key=%s source=%s",
                    (unsigned long long)serial, capture->stage, capture->shader_key, source);
        }
    }
    sky_mtl_program_remove(out->serial);
    sky_nvn_program_state_free(sky_nvn_program_state_take(out->serial));
    out->serial = serial;
    out->stage_count = (uint32_t)count;
    out->stage_mask = stage_mask;
    sky_checkpoint("post_nnMain_nvn_program_set_shaders");
    if (count != 1 || g_capture_sequence <= (uint64_t)count || g_capture_sequence % 1024 == 0 || stage_mask == 0x20)
        LOG("NVN_PROGRAM_SHADERS_SET program=%p count=%d stage_mask=%#x", program, count, stage_mask);
    return 1;
}
