#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(sky_nvn_program_t) <= 0xc0, "program exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_buffer_builder_t) <= 0x40, "buffer builder exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_buffer_t) <= 0x30, "buffer exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_shader_data_t) == 0x10, "shader data ABI mismatch");

#define ASSERT_LOOKUP(name, function) \
    assert(sky_nvn_bootstrap_loader(name) == (void *)(function))
#define CODE_SIZE 0x200
#define CONTROL_SIZE 0x880

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (int index = 0; index < 4; index++) bytes[index] = (uint8_t)(value >> index * 8);
}

static void make_code(uint8_t *code, size_t size, int seed, int sph_stage) {
    for (size_t index = 0; index < size; index++) code[index] = (uint8_t)(seed + index * 29);
    write_u32(code, 0x12345678);
    if (size > 4) memset(code + 4, 0, size - 4 < 0x2c ? size - 4 : 0x2c);
    if (size >= 0x34) write_u32(code + 0x30, (uint32_t)sph_stage << 10);
}

static void make_control_layout(uint8_t *control, int seed, uint32_t code_size,
                                uint32_t program_start, uint32_t program_length, int workgroup) {
    for (size_t index = 0; index < CONTROL_SIZE; index++) control[index] = (uint8_t)(seed + index * 17);
    write_u32(control, 0x98761234);
    write_u32(control + 0x6f8, program_length);
    write_u32(control + 0x6fc, 0);
    write_u32(control + 0x700, code_size);
    write_u32(control + 0x704, code_size);
    write_u32(control + 0x708, program_start);
    write_u32(control + 0x748, (uint32_t)workgroup);
    write_u32(control + 0x74c, 1);
    write_u32(control + 0x750, 1);
}

static void make_control(uint8_t *control, int seed) {
    make_control_layout(control, seed, CODE_SIZE, 0x30, 0x100, 0);
}

static void make_buffer(sky_nvn_buffer_t *buffer, sky_nvn_device_t *device,
                        sky_nvn_memory_pool_t *pool, int64_t offset, uint64_t size) {
    sky_nvn_buffer_builder_t builder = {0};
    sky_nvn_buffer_builder_set_device(&builder, device);
    sky_nvn_buffer_builder_set_defaults(&builder);
    sky_nvn_buffer_builder_set_storage(&builder, pool, offset, size);
    assert(sky_nvn_buffer_initialize(buffer, &builder) == 1);
    assert(sky_nvn_buffer_map(buffer) == (uint8_t *)pool->mem + offset);
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length >= 0);
    assert(!fseek(file, 0, SEEK_SET));
    uint8_t *bytes = malloc((size_t)length + 1);
    assert(bytes);
    assert(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    assert(!fclose(file));
    bytes[length] = 0;
    *size = (size_t)length;
    return bytes;
}

static void expect_binary(const char *path, const uint8_t *expected, size_t expected_size) {
    size_t size = 0;
    uint8_t *bytes = read_file(path, &size);
    assert(size == expected_size);
    assert(!memcmp(bytes, expected, size));
    free(bytes);
}

static void expect_artifact(const char *json_path, const char *stage,
                            const uint8_t *code, size_t code_size, const uint8_t *control,
                            unsigned code_magic, const char *code_format) {
    size_t json_size = 0;
    char *json = (char *)read_file(json_path, &json_size);
    char expected[256];
    assert(strstr(json, "\"accepted\": true"));
    snprintf(expected, sizeof expected, "\"stage\": \"%s\"", stage);
    assert(strstr(json, expected));
    snprintf(expected, sizeof expected, "\"code_size\": %zu", code_size);
    assert(strstr(json, expected));
    assert(strstr(json, "\"control_size\": 2176"));
    snprintf(expected, sizeof expected, "\"code_magic\": \"0x%08x\"", code_magic);
    assert(strstr(json, expected));
    assert(strstr(json, "\"control_magic\": \"0x98761234\""));
    snprintf(expected, sizeof expected, "\"code_format\": \"%s\"", code_format);
    assert(strstr(json, expected));
    assert(strstr(json, "\"d2r_shad_btcd\": false"));
    free(json);
    char base[4096];
    assert(strlen(json_path) < sizeof base);
    strcpy(base, json_path);
    char *suffix = strrchr(base, '.');
    assert(suffix && !strcmp(suffix, ".json"));
    *suffix = 0;
    char path[4096];
    assert(snprintf(path, sizeof path, "%s-code.bin", base) < (int)sizeof path);
    expect_binary(path, code, code_size);
    assert(snprintf(path, sizeof path, "%s-control.bin", base) < (int)sizeof path);
    expect_binary(path, control, CONTROL_SIZE);
}

static void expect_artifact_row(const char *directory, int row, const char *expected_stage,
                                const uint8_t *code, size_t expected_code_size, const uint8_t *control,
                                unsigned expected_magic, const char *expected_format) {
    char path[4096];
    assert(snprintf(path, sizeof path, "%s/manifest.tsv", directory) < (int)sizeof path);
    FILE *manifest = fopen(path, "rb");
    assert(manifest);
    char line[8192];
    assert(fgets(line, sizeof line, manifest));
    assert(!strncmp(line, "stage\tcode_size\tcontrol_size", 28));
    for (int index = 0; index <= row; index++) assert(fgets(line, sizeof line, manifest));
    assert(!fclose(manifest));
    char stage[32], code_hash[65], control_hash[65], json[4096];
    size_t code_size = 0, control_size = 0;
    unsigned code_magic = 0, control_magic = 0;
    int fields = sscanf(line, "%31s %zu %zu %x %x %64s %64s %4095s",
                        stage, &code_size, &control_size, &code_magic, &control_magic,
                        code_hash, control_hash, json);
    assert(fields == 8);
    assert(!strcmp(stage, expected_stage));
    assert(code_size == expected_code_size && control_size == CONTROL_SIZE);
    assert(code_magic == expected_magic && control_magic == 0x98761234);
    assert(strlen(code_hash) == 64 && strlen(control_hash) == 64);
    expect_artifact(json, stage, code, expected_code_size, control, expected_magic, expected_format);
}

static void expect_two_artifacts(const char *directory, const uint8_t *code_a, const uint8_t *control_a,
                                 const uint8_t *code_b, const uint8_t *control_b) {
    expect_artifact_row(directory, 0, "vertex", code_a, CODE_SIZE, control_a,
                        0x12345678, "maxwell-nvn-sph");
    expect_artifact_row(directory, 1, "fragment", code_b, CODE_SIZE, control_b,
                        0x12345678, "maxwell-nvn-sph");
}

static int manifest_rows(const char *directory) {
    char path[4096], line[8192];
    assert(snprintf(path, sizeof path, "%s/manifest.tsv", directory) < (int)sizeof path);
    FILE *manifest = fopen(path, "rb");
    assert(manifest && fgets(line, sizeof line, manifest));
    int count = 0;
    while (fgets(line, sizeof line, manifest)) count++;
    assert(!fclose(manifest));
    return count;
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

int main(void) {
    ASSERT_LOOKUP("nvnProgramInitialize", sky_nvn_program_initialize);
    ASSERT_LOOKUP("nvnProgramFinalize", sky_nvn_program_finalize);
    ASSERT_LOOKUP("nvnProgramSetDebugLabel", sky_nvn_program_set_debug_label);
    ASSERT_LOOKUP("nvnProgramSetShaders", sky_nvn_program_set_shaders);
    ASSERT_LOOKUP("nvnCommandBufferBindProgram", sky_nvn_command_buffer_bind_program);

    char directory[256] = "/tmp/skyrim-nvn-program-XXXXXX";
    assert(mkdtemp(directory));
    assert(strlen(directory) + sizeof "/capture" < sizeof directory);
    strcat(directory, "/capture");
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", directory, 1));
    assert(!setenv("SKY_SHADER_CAPTURE", "1", 1));

    sky_nvn_device_builder_t device_builder = {0};
    sky_nvn_device_t device;
    assert(sky_nvn_device_initialize(&device, &device_builder) == 1);
    size_t page = (size_t)getpagesize();
    void *allocation = NULL;
    assert(posix_memalign(&allocation, page, 1 << 20) == 0);
    memset(allocation, 0, 1 << 20);
    sky_nvn_memory_pool_builder_t pool_builder = {0};
    sky_nvn_memory_pool_builder_set_device(&pool_builder, &device);
    sky_nvn_memory_pool_builder_set_storage(&pool_builder, allocation, 1 << 20);
    sky_nvn_memory_pool_builder_set_flags(&pool_builder, 0x22);
    sky_nvn_memory_pool_t pool;
    assert(sky_nvn_memory_pool_initialize(&pool, &pool_builder) == 1);

    sky_nvn_program_t program = {0};
    assert(sky_nvn_program_initialize(NULL, &device) == 0);
    assert(sky_nvn_program_initialize(&program, NULL) == 0);
    assert(sky_nvn_program_initialize(&program, &device) == 1);
    assert(program.magic == 0x474f5250 && program.dev == &device);
    assert(program.stage_count == 0 && program.stage_mask == 0 && program.serial == 0);
    sky_nvn_program_set_debug_label(&program, "program-test");
    assert(!strcmp(program.debug_label, "program-test"));
    assert(sky_nvn_program_set_shaders(&program, -1, NULL) == 0);
    assert(sky_nvn_program_set_shaders(&program, 7, NULL) == 0);
    assert(sky_nvn_program_set_shaders(&program, 1, NULL) == 0);
    assert(sky_nvn_program_set_shaders(&program, 1, (const void *)1) == 0);

    uint8_t code_a[CODE_SIZE], code_b[CODE_SIZE], short_code[0x40], invalid_stage_code[CODE_SIZE];
    uint8_t compute_code[0x300], raw_code[CODE_SIZE];
    uint8_t original_a[CODE_SIZE], original_b[CODE_SIZE];
    uint8_t control_a[CONTROL_SIZE], control_b[CONTROL_SIZE], compute_control[CONTROL_SIZE], raw_control[CONTROL_SIZE];
    uint8_t original_control_a[CONTROL_SIZE], original_control_b[CONTROL_SIZE];
    make_code(code_a, sizeof code_a, 3, 1);
    make_code(code_b, sizeof code_b, 71, 5);
    make_code(short_code, sizeof short_code, 17, 1);
    make_code(invalid_stage_code, sizeof invalid_stage_code, 37, 0);
    make_code(compute_code, sizeof compute_code, 101, 0);
    write_u32(compute_code, 0x12345679);
    memset(compute_code + 4, 0, 0xfc);
    make_code(raw_code, sizeof raw_code, 131, 0);
    write_u32(raw_code, 0xe2200711);
    make_control(control_a, 5);
    make_control(control_b, 89);
    make_control_layout(compute_control, 101, sizeof compute_code, 0x100, 0x180, 8);
    make_control_layout(raw_control, 131, sizeof raw_code, 0, 0x140, 8);
    memcpy(original_a, code_a, sizeof code_a);
    memcpy(original_b, code_b, sizeof code_b);
    memcpy(original_control_a, control_a, sizeof control_a);
    memcpy(original_control_b, control_b, sizeof control_b);
    memcpy(allocation, code_a, sizeof code_a);
    memcpy((uint8_t *)allocation + 0x1000, code_b, sizeof code_b);
    memcpy((uint8_t *)allocation + 0x2000, short_code, sizeof short_code);
    memcpy((uint8_t *)allocation + 0x3000, invalid_stage_code, sizeof invalid_stage_code);
    memcpy((uint8_t *)allocation + 0x4000, compute_code, sizeof compute_code);
    memcpy((uint8_t *)allocation + 0x5000, raw_code, sizeof raw_code);

    sky_nvn_buffer_t buffer_a, buffer_b, short_buffer, invalid_stage_buffer, compute_buffer, raw_buffer;
    make_buffer(&buffer_a, &device, &pool, 0, sizeof code_a);
    make_buffer(&buffer_b, &device, &pool, 0x1000, sizeof code_b);
    make_buffer(&short_buffer, &device, &pool, 0x2000, sizeof short_code);
    make_buffer(&invalid_stage_buffer, &device, &pool, 0x3000, sizeof invalid_stage_code);
    make_buffer(&compute_buffer, &device, &pool, 0x4000, sizeof compute_code);
    make_buffer(&raw_buffer, &device, &pool, 0x5000, sizeof raw_code);
    uint64_t address_a = sky_nvn_buffer_get_address(&buffer_a);
    uint64_t address_b = sky_nvn_buffer_get_address(&buffer_b);
    uint64_t short_address = sky_nvn_buffer_get_address(&short_buffer);
    uint64_t invalid_stage_address = sky_nvn_buffer_get_address(&invalid_stage_buffer);
    uint64_t compute_address = sky_nvn_buffer_get_address(&compute_buffer);
    uint64_t raw_address = sky_nvn_buffer_get_address(&raw_buffer);
    assert(address_a && address_b && short_address && invalid_stage_address && compute_address && raw_address);

    sky_nvn_shader_data_t input = {address_a, control_a};
    sky_nvn_shader_data_t pair[2] = {{address_a, control_a}, {address_b, control_b}};
    sky_nvn_shader_data_t malformed = {0, control_a};
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);
    malformed.data = address_a;
    malformed.control = NULL;
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);
    malformed.data = short_address;
    malformed.control = control_a;
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);
    malformed.data = invalid_stage_address;
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);
    malformed.data = address_a;
    malformed.control = (const void *)1;
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);

    uint8_t *guard = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guard != MAP_FAILED);
    assert(!mprotect(guard + page, page, PROT_NONE));
    uint8_t *edge_control = guard + page - 4;
    write_u32(edge_control, 0x98761234);
    malformed.control = edge_control;
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);
    assert(!munmap(guard, page * 2));
    assert(program.stage_count == 0 && program.serial == 0);

    char disabled_directory[320];
    assert(snprintf(disabled_directory, sizeof disabled_directory, "%s-disabled", directory) > 0);
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", disabled_directory, 1));
    assert(!unsetenv("SKY_SHADER_CAPTURE"));
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    struct stat disabled_status;
    assert(stat(disabled_directory, &disabled_status) != 0);
    assert(program.stage_count == 1 && program.serial != 0);
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", directory, 1));
    assert(!setenv("SKY_SHADER_CAPTURE", "1", 1));

    assert(sky_nvn_program_set_shaders(&program, 2, pair) == 1);
    assert(program.stage_count == 2 && program.stage_mask == 3 && program.serial != 0);
    expect_two_artifacts(directory, original_a, original_control_a, original_b, original_control_b);
    char canonical_path[4096];
    assert(snprintf(canonical_path, sizeof canonical_path,
        "%s/vertex-5e25bef65df57276ed2fa247998c6ac2002f1c17f73cf9d3e7634e7c6d4afe91.json", directory) < (int)sizeof canonical_path);
    assert(!access(canonical_path, R_OK));

    /* Control contains application metadata: it must not multiply shader cache entries. */
    control_a[0x40] ^= 0x5a;
    write_u32(control_a + 0x748, 64); /* Unused compute size in a graphics control. */
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    assert(manifest_rows(directory) == 2);
    memcpy(control_a, original_control_a, sizeof control_a);
    assert(sky_nvn_program_set_shaders(&program, 2, pair) == 1);
    expect_two_artifacts(directory, original_a, original_control_a, original_b, original_control_b);

    memset(code_a, 0, sizeof code_a);
    memset(code_b, 0, sizeof code_b);
    memset(control_a, 0, sizeof control_a);
    memset(control_b, 0, sizeof control_b);
    memset(allocation, 0, sizeof code_a);
    memset((uint8_t *)allocation + 0x1000, 0, sizeof code_b);
    assert(sky_nvn_program_set_shaders(&program, 2, pair) == 0);
    assert(program.stage_count == 2 && program.stage_mask == 3 && program.serial != 0);
    memcpy(code_a, original_a, sizeof code_a);
    memcpy(code_b, original_b, sizeof code_b);
    memcpy(control_a, original_control_a, sizeof control_a);
    memcpy(control_b, original_control_b, sizeof control_b);
    memcpy(allocation, original_a, sizeof original_a);
    memcpy((uint8_t *)allocation + 0x1000, original_b, sizeof original_b);

    sky_nvn_shader_data_t duplicate[2] = {{address_a, control_a}, {address_a, control_a}};
    assert(sky_nvn_program_set_shaders(&program, 2, duplicate) == 0);
    assert(program.stage_count == 2 && program.serial != 0);
    control_a[0] = 0;
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 0);
    memcpy(control_a, original_control_a, sizeof control_a);
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", "/dev/null/not-a-directory", 1));
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 0);
    assert(program.stage_count == 2 && program.serial != 0);
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", directory, 1));

    sky_nvn_buffer_finalize(&short_buffer);
    assert(sky_nvn_program_set_shaders(&program, 1, &malformed) == 0);

    sky_nvn_shader_data_t compute_input = {compute_address, compute_control};
    assert(sky_nvn_program_set_shaders(&program, 1, &compute_input) == 1);
    assert(program.stage_count == 1 && program.stage_mask == 0x20 && program.serial != 0);
    expect_artifact_row(directory, 2, "compute", compute_code, sizeof compute_code, compute_control,
                        0x12345679, "maxwell-nvn-compute");
    sky_nvn_shader_data_t raw_input = {raw_address, raw_control};
    assert(sky_nvn_program_set_shaders(&program, 1, &raw_input) == 1);
    assert(program.stage_count == 1 && program.stage_mask == 0x20 && program.serial != 0);
    expect_artifact_row(directory, 3, "compute", raw_code, sizeof raw_code, raw_control,
                        0xe2200711, "maxwell-raw-compute");
    write_u32(compute_control + 0x704, CODE_SIZE);
    assert(sky_nvn_program_set_shaders(&program, 1, &compute_input) == 0);
    write_u32(compute_control + 0x704, sizeof compute_code);
    write_u32(raw_control + 0x748, 0);
    assert(sky_nvn_program_set_shaders(&program, 1, &raw_input) == 0);
    write_u32(raw_control + 0x748, 8);
    assert(program.stage_count == 1 && program.stage_mask == 0x20 && program.serial != 0);
    int rows_before_specialization = manifest_rows(directory);
    write_u32(raw_control + 0x748, 16);
    assert(sky_nvn_program_set_shaders(&program, 1, &raw_input) == 1);
    assert(manifest_rows(directory) == rows_before_specialization + 1);
    expect_artifact_row(directory, rows_before_specialization, "compute", raw_code, sizeof raw_code, raw_control,
                        0xe2200711, "maxwell-raw-compute");

    assert(sky_nvn_program_set_shaders(&program, 0, NULL) == 1);
    assert(program.stage_count == 0 && program.stage_mask == 0 && program.serial == 0);
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    assert(program.stage_count == 1 && program.stage_mask == 1 && program.serial != 0);
    sky_nvn_program_t alias = program;
    sky_nvn_program_finalize(&program);
    assert(program.magic == 0 && program.serial == 0 && program.stage_count == 0);
    assert(alias.serial != 0);
    sky_nvn_program_finalize(&alias);

    /* Selective capture must skip a complete compiled cache entry, but capture
     * it when only its MSL file exists and reflection is missing. */
    char missing_directory[256], cache_directory[256], cache_source[512], cache_reflection[512];
    assert(snprintf(missing_directory, sizeof missing_directory, "%s-missing", directory) < (int)sizeof missing_directory);
    assert(snprintf(cache_directory, sizeof cache_directory, "%s-cache", directory) < (int)sizeof cache_directory);
    assert(!mkdir(cache_directory, 0700));
    assert(snprintf(cache_source, sizeof cache_source,
        "%s/5e25bef65df57276ed2fa247998c6ac2002f1c17f73cf9d3e7634e7c6d4afe91.metal", cache_directory) < (int)sizeof cache_source);
    assert(snprintf(cache_reflection, sizeof cache_reflection,
        "%s/5e25bef65df57276ed2fa247998c6ac2002f1c17f73cf9d3e7634e7c6d4afe91.json", cache_directory) < (int)sizeof cache_reflection);
    write_text(cache_source, "#include <metal_stdlib>\nusing namespace metal;\nvertex float4 main0(){return float4(0,0,0,1);}\n");
    write_text(cache_reflection, "{\"route\":\"B\",\"entry\":\"main0\",\"bindings\":[]}\n");
    assert(!setenv("SKY_SHADER_CACHE_DIR", cache_directory, 1));
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", missing_directory, 1));
    assert(!unsetenv("SKY_SHADER_CAPTURE"));
    assert(!setenv("SKY_SHADER_CAPTURE_MISSING", "1", 1));
    assert(sky_nvn_program_initialize(&program, &device) == 1);
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    char missing_manifest[512];
    assert(snprintf(missing_manifest, sizeof missing_manifest, "%s/manifest.tsv", missing_directory) < (int)sizeof missing_manifest);
    assert(access(missing_manifest, F_OK) != 0);
    assert(!unlink(cache_reflection));
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    assert(manifest_rows(missing_directory) == 1);
    expect_artifact_row(missing_directory, 0, "vertex", original_a, sizeof original_a,
                        original_control_a, 0x12345678, "maxwell-nvn-sph");
    sky_nvn_program_finalize(&program);
    assert(!unsetenv("SKY_SHADER_CAPTURE_MISSING"));
    assert(!unsetenv("SKY_SHADER_CACHE_DIR"));
    assert(!setenv("SKY_SHADER_CAPTURE", "1", 1));
    assert(!setenv("SKY_SHADER_CAPTURE_DIR", directory, 1));

    sky_nvn_buffer_finalize(&buffer_a);
    assert(sky_nvn_program_initialize(&program, &device) == 1);
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    assert(program.stage_count == 1 && program.stage_mask == 1 && program.serial != 0);
    sky_nvn_program_finalize(&program);

    /* The fallback capture directory must work in an unrelated checkout. */
    char original_cwd[4096];
    assert(getcwd(original_cwd, sizeof original_cwd));
    assert(!chdir(directory));
    assert(!unsetenv("SKY_SHADER_CAPTURE_DIR"));
    assert(sky_nvn_program_initialize(&program, &device) == 1);
    assert(sky_nvn_program_set_shaders(&program, 1, &input) == 1);
    assert(manifest_rows("skyrim-work/build/shader-captures") == 1);
    sky_nvn_program_finalize(&program);
    assert(!chdir(original_cwd));
    sky_nvn_buffer_finalize(&buffer_b);
    sky_nvn_buffer_finalize(&invalid_stage_buffer);
    sky_nvn_buffer_finalize(&compute_buffer);
    sky_nvn_buffer_finalize(&raw_buffer);
    sky_nvn_memory_pool_finalize(&pool);
    free(allocation);
    sky_nvn_device_finalize(&device);
    assert(!unsetenv("SKY_SHADER_CAPTURE_DIR"));
    assert(!unsetenv("SKY_SHADER_CAPTURE"));
    printf("NVN_PROGRAM_TEST_ARTIFACTS dir=%s\n", directory);
    puts("T_NVN_PROGRAM_OK lifecycle-validation-exact-capture-malformed-cleanup");
    return 0;
}
