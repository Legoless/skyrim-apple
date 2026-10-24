#include "nvn.h"
#include "nvn_metal.h"
#include "skyhost.h"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

_Static_assert(sizeof(sky_nvn_texture_pool_t) <= 0x20, "texture pool exceeds NVN storage");
_Static_assert(sizeof(sky_nvn_sampler_pool_t) <= 0x20, "sampler pool exceeds NVN storage");

#define ASSERT_LOOKUP(name, function) \
    assert(sky_nvn_bootstrap_loader(name) == (void *)(function))

static void setup(sky_nvn_device_t *device, sky_nvn_memory_pool_t *memory, void **allocation) {
    sky_nvn_device_builder_t device_builder = {0};
    assert(sky_nvn_device_initialize(device, &device_builder) == 1);
    assert(posix_memalign(allocation, (size_t)getpagesize(), 1 << 20) == 0);
    memset(*allocation, 0xa5, 1 << 20);
    sky_nvn_memory_pool_builder_t builder = {0};
    sky_nvn_memory_pool_builder_set_device(&builder, device);
    sky_nvn_memory_pool_builder_set_storage(&builder, *allocation, 1 << 20);
    sky_nvn_memory_pool_builder_set_flags(&builder, 0x22);
    assert(sky_nvn_memory_pool_initialize(memory, &builder) == 1);
}

static void release_resolved(sky_mtl_render_state_t *state) {
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 128; slot++) {
            sky_mtl_texture_destroy(state->textures[stage][slot]);
            sky_mtl_sampler_destroy(state->samplers[stage][slot]);
        }
        for (int slot = 0; slot < 32; slot++) sky_mtl_texture_destroy(state->images[stage][slot]);
    }
    memset(state, 0, sizeof *state);
}

static void make_registered_source(sky_nvn_texture_t *texture, sky_nvn_device_t *device,
    sky_nvn_memory_pool_t *memory, int width, int levels, int64_t offset) {
    sky_nvn_texture_builder_t builder = {0};
    sky_nvn_texture_builder_set_device(&builder, device);
    sky_nvn_texture_builder_set_defaults(&builder);
    sky_nvn_texture_builder_set_format(&builder, 0x25);
    sky_nvn_texture_builder_set_size_2d(&builder, width, width);
    sky_nvn_texture_builder_set_levels(&builder, levels);
    sky_nvn_texture_builder_set_flags(&builder, 4);
    sky_nvn_texture_builder_set_storage(&builder, memory, offset);
    assert(sky_nvn_texture_initialize(texture, &builder));
}

typedef struct {
    sky_nvn_texture_pool_t *pool;
    sky_nvn_texture_t *first, *second;
    _Atomic int done;
} registration_race_t;

static void *replace_descriptors(void *context) {
    registration_race_t *race = context;
    for (int i = 0; i < 64; i++)
        sky_nvn_texture_pool_register_texture(race->pool, 0, i & 1 ? race->first : race->second, NULL);
    atomic_store(&race->done, 1);
    return NULL;
}

static void test_descriptor_index(sky_nvn_device_t *device, sky_nvn_memory_pool_t *memory) {
    size_t pools_before = sky_mtl_memory_pool_active_count(), textures_before = sky_mtl_texture_active_count();
    sky_nvn_texture_pool_t first_pool, second_pool;
    assert(sky_nvn_texture_pool_initialize(&first_pool, memory, 0, 16385));
    assert(sky_nvn_texture_pool_initialize(&second_pool, memory, 0, 16385));
    sky_nvn_texture_t first, second;
    make_registered_source(&first, device, memory, 8, 2, 0xc0000);
    make_registered_source(&second, device, memory, 4, 1, 0xc4000);
    uint64_t first_id = sky_mtl_texture_resource_id(first.metal), second_id = sky_mtl_texture_resource_id(second.metal);
    assert(first_id && second_id && first_id != second_id);
    sky_nvn_texture_view_t view;
    sky_nvn_texture_view_set_defaults(&view);
    sky_nvn_texture_view_set_levels(&view, 1, 1);
    /* 0 and 16384 collide in the descriptor index. Replacement must preserve
     * the other chain member; a second pool's identical IDs remain separate. */
    sky_nvn_texture_pool_register_texture(&first_pool, 0, &first, &view);
    sky_nvn_texture_pool_register_image(&first_pool, 16384, &second, NULL);
    sky_nvn_texture_pool_register_texture(&second_pool, 0, &second, NULL);
    const sky_nvn_texture_t *registered = sky_nvn_texture_pool_get_registered_texture(&first_pool, 16384);
    assert(registered && sky_mtl_texture_resource_id(registered->metal) == second_id);
    sky_mtl_texture_t *retained_view = sky_mtl_texture_retain(
        (sky_mtl_texture_t *)sky_nvn_texture_pool_get_registered_metal_view(&first_pool, 0));
    assert(retained_view);
    sky_nvn_texture_pool_register_image(&first_pool, 0, &second, NULL);
    assert(!sky_nvn_texture_pool_get_registered_metal_view(&first_pool, 0));
    registered = sky_nvn_texture_pool_get_registered_texture(&first_pool, 16384);
    assert(registered && sky_mtl_texture_resource_id(registered->metal) == second_id);
    sky_nvn_texture_pool_register_texture(&first_pool, 0, &first, &view);
    sky_nvn_texture_pool_register_texture(&first_pool, 16384, &first, NULL);
    registered = sky_nvn_texture_pool_get_registered_texture(&second_pool, 0);
    assert(registered && sky_mtl_texture_resource_id(registered->metal) == second_id);
    assert(!sky_nvn_texture_pool_get_registered_texture(&first_pool, 1));

    sky_nvn_sampler_pool_t first_samplers, second_samplers;
    assert(sky_nvn_sampler_pool_initialize(&first_samplers, memory, 0, 4096));
    assert(sky_nvn_sampler_pool_initialize(&second_samplers, memory, 0, 4096));
    sky_nvn_sampler_builder_t builder = {0};
    sky_nvn_sampler_builder_set_device(&builder, device);
    sky_nvn_sampler_builder_set_defaults(&builder);
    sky_nvn_sampler_t sampler;
    assert(sky_nvn_sampler_initialize(&sampler, &builder));
    sky_nvn_sampler_pool_register_sampler(&first_samplers, 0, &sampler);
    sky_nvn_sampler_pool_register_sampler(&first_samplers, 4095, &sampler);
    sky_nvn_sampler_pool_register_sampler(&second_samplers, 0, &sampler);
    sky_nvn_sampler_builder_set_min_mag_filter(&builder, 1, 1);
    sky_nvn_sampler_pool_register_sampler_builder(&first_samplers, 0, &builder);
    sky_nvn_sampler_finalize(&sampler);

    sky_nvn_texture_pool_register_texture(&first_pool, 0, &first, NULL);
    registration_race_t race = {.pool = &first_pool, .first = &first, .second = &second};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, replace_descriptors, &race));
    do {
        sky_mtl_render_state_t state = {.texture_pool = &first_pool, .sampler_pool = &first_samplers};
        state.texture_bound[0][0] = 1;
        sky_nvn_graphics_resources_resolve(&state);
        uint64_t resource = sky_mtl_texture_resource_id(state.textures[0][0]);
        assert(resource == first_id || resource == second_id);
        assert(state.samplers[0][0]);
        release_resolved(&state);
    } while (!atomic_load(&race.done));
    assert(!pthread_join(thread, NULL));
    sky_nvn_texture_pool_register_texture(&first_pool, 0, &first, &view);

    sky_mtl_render_state_t retained = {.texture_pool = &first_pool, .sampler_pool = &first_samplers};
    retained.texture_bound[0][0] = retained.texture_bound[0][1] = retained.image_bound[1][0] = 1;
    retained.texture_handles[0][1] = 16384 | ((uint64_t)4095 << 20);
    retained.image_handles[1][0] = 16384;
    retained.sampler_bound[1][4] = 1;
    retained.sampler_handles[1][4] = (uint64_t)4095 << 20;
    sky_nvn_graphics_resources_resolve(&retained);
    assert(retained.textures[0][0] && retained.samplers[0][0] && retained.samplers[0][1] && retained.samplers[1][4]);
    assert(sky_mtl_texture_resource_id(retained.textures[0][1]) == first_id);
    assert(sky_mtl_texture_resource_id(retained.images[1][0]) == first_id);
    sky_nvn_texture_finalize(&first);
    sky_nvn_texture_finalize(&second);
    sky_nvn_texture_pool_finalize(&first_pool);
    sky_nvn_sampler_pool_finalize(&first_samplers);
    sky_mtl_texture_properties_t properties;
    assert(sky_mtl_texture_get_properties(retained.textures[0][0], &properties) && properties.width == 4);
    assert(sky_mtl_texture_get_properties(retained_view, &properties) && properties.width == 4);
    sky_mtl_sampler_t *held_sampler = sky_mtl_sampler_retain(retained.samplers[0][0]);
    assert(held_sampler);
    sky_mtl_sampler_destroy(held_sampler);
    release_resolved(&retained);
    sky_mtl_texture_destroy(retained_view);
    assert(!sky_nvn_texture_pool_get_registered_texture(&first_pool, 0));

    /* Reusing the exact pool object address must not resurrect retired IDs. */
    assert(sky_nvn_texture_pool_initialize(&first_pool, memory, 0, 16385));
    assert(sky_nvn_sampler_pool_initialize(&first_samplers, memory, 0, 4096));
    sky_mtl_render_state_t empty = {.texture_pool = &first_pool, .sampler_pool = &first_samplers};
    empty.texture_bound[0][0] = empty.image_bound[1][0] = 1;
    empty.image_handles[1][0] = 16384;
    sky_nvn_graphics_resources_resolve(&empty);
    assert(!empty.textures[0][0] && !empty.images[1][0] && !empty.samplers[0][0]);
    release_resolved(&empty);
    sky_mtl_render_state_t other = {.texture_pool = &second_pool, .sampler_pool = &second_samplers};
    other.texture_bound[0][0] = 1;
    sky_nvn_graphics_resources_resolve(&other);
    assert(sky_mtl_texture_resource_id(other.textures[0][0]) == second_id && other.samplers[0][0]);
    release_resolved(&other);
    sky_nvn_texture_pool_finalize(&first_pool);
    sky_nvn_texture_pool_finalize(&second_pool);
    sky_nvn_sampler_pool_finalize(&first_samplers);
    sky_nvn_sampler_pool_finalize(&second_samplers);
    assert(sky_mtl_texture_active_count() == textures_before && sky_mtl_memory_pool_active_count() == pools_before);
}

int main(int argc, char **argv) {
    ASSERT_LOOKUP("nvnTexturePoolInitialize", sky_nvn_texture_pool_initialize);
    ASSERT_LOOKUP("nvnTexturePoolFinalize", sky_nvn_texture_pool_finalize);
    ASSERT_LOOKUP("nvnTexturePoolRegisterTexture", sky_nvn_texture_pool_register_texture);
    ASSERT_LOOKUP("nvnTexturePoolRegisterImage", sky_nvn_texture_pool_register_image);
    ASSERT_LOOKUP("nvnTexturePoolGetMemoryPool", sky_nvn_texture_pool_get_memory_pool);
    ASSERT_LOOKUP("nvnTexturePoolGetMemoryOffset", sky_nvn_texture_pool_get_memory_offset);
    ASSERT_LOOKUP("nvnTexturePoolGetSize", sky_nvn_texture_pool_get_size);
    ASSERT_LOOKUP("nvnSamplerPoolInitialize", sky_nvn_sampler_pool_initialize);
    ASSERT_LOOKUP("nvnSamplerPoolFinalize", sky_nvn_sampler_pool_finalize);
    ASSERT_LOOKUP("nvnSamplerPoolRegisterSampler", sky_nvn_sampler_pool_register_sampler);
    ASSERT_LOOKUP("nvnSamplerPoolRegisterSamplerBuilder", sky_nvn_sampler_pool_register_sampler_builder);
    ASSERT_LOOKUP("nvnSamplerPoolGetMemoryPool", sky_nvn_sampler_pool_get_memory_pool);
    ASSERT_LOOKUP("nvnSamplerPoolGetMemoryOffset", sky_nvn_sampler_pool_get_memory_offset);
    ASSERT_LOOKUP("nvnSamplerPoolGetSize", sky_nvn_sampler_pool_get_size);
    ASSERT_LOOKUP("nvnSamplerBuilderSetDevice", sky_nvn_sampler_builder_set_device);
    ASSERT_LOOKUP("nvnSamplerBuilderSetDefaults", sky_nvn_sampler_builder_set_defaults);
    ASSERT_LOOKUP("nvnSamplerBuilderSetMinMagFilter", sky_nvn_sampler_builder_set_min_mag_filter);
    ASSERT_LOOKUP("nvnSamplerBuilderSetWrapMode", sky_nvn_sampler_builder_set_wrap_mode);
    ASSERT_LOOKUP("nvnSamplerBuilderSetLodClamp", sky_nvn_sampler_builder_set_lod_clamp);
    ASSERT_LOOKUP("nvnSamplerBuilderSetCompare", sky_nvn_sampler_builder_set_compare);
    ASSERT_LOOKUP("nvnSamplerBuilderSetMaxAnisotropy", sky_nvn_sampler_builder_set_max_anisotropy);
    ASSERT_LOOKUP("nvnSamplerBuilderSetLodBias", sky_nvn_sampler_builder_set_lod_bias);
    ASSERT_LOOKUP("nvnSamplerBuilderSetReductionFilter", sky_nvn_sampler_builder_set_reduction_filter);
    ASSERT_LOOKUP("nvnSamplerInitialize", sky_nvn_sampler_initialize);
    ASSERT_LOOKUP("nvnSamplerFinalize", sky_nvn_sampler_finalize);
    ASSERT_LOOKUP("nvnTextureBuilderSetDefaults", sky_nvn_texture_builder_set_defaults);

    sky_nvn_device_t device;
    sky_nvn_memory_pool_t memory;
    void *allocation = NULL;
    setup(&device, &memory, &allocation);

    if (argc > 1 && !strcmp(argv[1], "texture-builder")) {
        sky_nvn_texture_builder_set_defaults(NULL);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "register-texture")) {
        sky_nvn_texture_pool_t pool;
        assert(sky_nvn_texture_pool_initialize(&pool, &memory, 64, 2) == 1);
        sky_nvn_texture_pool_register_texture(&pool, 0, NULL, NULL);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "register-image")) {
        sky_nvn_texture_pool_t pool;
        assert(sky_nvn_texture_pool_initialize(&pool, &memory, 64, 2) == 1);
        sky_nvn_texture_pool_register_image(&pool, 0, NULL, NULL);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "register-sampler")) {
        sky_nvn_sampler_pool_t pool;
        assert(sky_nvn_sampler_pool_initialize(&pool, &memory, 128, 2) == 1);
        sky_nvn_sampler_pool_register_sampler(&pool, 0, NULL);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "register-sampler-builder")) {
        sky_nvn_sampler_pool_t pool;
        assert(sky_nvn_sampler_pool_initialize(&pool, &memory, 128, 2) == 1);
        sky_nvn_sampler_pool_register_sampler_builder(&pool, 0, NULL);
        return 1;
    }

    sky_nvn_texture_pool_t textures;
    assert(sky_nvn_texture_pool_initialize(&textures, &memory, 1, 2) == 0);
    assert(textures.magic == 0);
    assert(sky_nvn_texture_pool_initialize(&textures, &memory, -32, 2) == 0);
    assert(sky_nvn_texture_pool_initialize(&textures, &memory, 64, 0) == 0);
    assert(sky_nvn_texture_pool_initialize(&textures, &memory, 64, 1048577) == 0);
    assert(sky_nvn_texture_pool_initialize(&textures, &memory, (1 << 20) - 32, 2) == 0);
    assert(sky_nvn_texture_pool_initialize(&textures, NULL, 64, 2) == 0);
    assert(sky_nvn_texture_pool_initialize(&textures, &memory, 64, 2) == 1);
    assert(textures.magic == 0x4c505854 && textures.pool == &memory);
    assert(textures.offset == 64 && textures.count == 2);
    assert(sky_nvn_texture_pool_get_memory_pool(&textures) == &memory);
    assert(sky_nvn_texture_pool_get_memory_offset(&textures) == 64);
    assert(sky_nvn_texture_pool_get_size(&textures) == 2);
    uint8_t *texture_storage = (uint8_t *)memory.mem + 64;
    assert(texture_storage == (uint8_t *)sky_mtl_memory_pool_contents(memory.metal) + 64);
    assert(texture_storage[0] == 0xa5 && texture_storage[63] == 0xa5);
    sky_nvn_texture_pool_register_texture(&textures, -1, NULL, NULL);
    sky_nvn_texture_pool_register_texture(&textures, 2, NULL, NULL);
    sky_nvn_texture_pool_register_image(&textures, 2, NULL, NULL);
    sky_nvn_texture_pool_t invalid_texture_pool = {0};
    sky_nvn_texture_pool_register_texture(&invalid_texture_pool, 0, NULL, NULL);
    assert(texture_storage[0] == 0xa5 && texture_storage[63] == 0xa5);
    sky_nvn_texture_pool_finalize(&textures);
    assert(textures.magic == 0 && textures.pool == NULL && textures.count == 0);
    assert(sky_nvn_texture_pool_get_memory_pool(&textures) == NULL);
    assert(sky_nvn_texture_pool_get_memory_offset(&textures) == 0);
    assert(sky_nvn_texture_pool_get_size(&textures) == 0);
    assert(memory.magic == 0x4c4f4f50 && sky_mtl_memory_pool_active_count() == 1);
    sky_nvn_texture_pool_finalize(&textures);

    sky_nvn_sampler_pool_t samplers;
    assert(sky_nvn_sampler_pool_initialize(&samplers, &memory, 127, 2) == 0);
    assert(sky_nvn_sampler_pool_initialize(&samplers, &memory, 128, 4097) == 0);
    assert(sky_nvn_sampler_pool_initialize(&samplers, &memory, (1 << 20) - 32, 2) == 0);
    assert(sky_nvn_sampler_pool_initialize(&samplers, &memory, 128, 2) == 1);
    assert(samplers.magic == 0x4c504d53 && samplers.pool == &memory);
    assert(samplers.offset == 128 && samplers.count == 2);
    assert(sky_nvn_sampler_pool_get_memory_pool(&samplers) == &memory);
    assert(sky_nvn_sampler_pool_get_memory_offset(&samplers) == 128);
    assert(sky_nvn_sampler_pool_get_size(&samplers) == 2);
    uint8_t *sampler_storage = (uint8_t *)memory.mem + 128;
    assert(sampler_storage[0] == 0xa5 && sampler_storage[63] == 0xa5);
    sky_nvn_sampler_builder_t sampler_builder = {0};
    sky_nvn_sampler_builder_set_device(&sampler_builder, &device);
    sky_nvn_sampler_builder_set_defaults(&sampler_builder);
    assert(sampler_builder.dev == &device && sampler_builder.lod_max == 1000.0f);
    sky_nvn_sampler_builder_set_min_mag_filter(&sampler_builder, 4, 5);
    sky_nvn_sampler_builder_set_wrap_mode(&sampler_builder, 1, 2, 3);
    sky_nvn_sampler_builder_set_lod_clamp(&sampler_builder, 0.0f, 12.0f);
    sky_nvn_sampler_builder_set_compare(&sampler_builder, 6, 7);
    sky_nvn_sampler_builder_set_max_anisotropy(&sampler_builder, 8.0f);
    sky_nvn_sampler_builder_set_lod_bias(&sampler_builder, -1.5f);
    sky_nvn_sampler_builder_set_reduction_filter(&sampler_builder, 2);
    assert(sampler_builder.min_filter == 4 && sampler_builder.mag_filter == 5);
    assert(sampler_builder.wrap[0] == 1 && sampler_builder.wrap[1] == 2 && sampler_builder.wrap[2] == 3);
    assert(sampler_builder.lod_min == 0.0f && sampler_builder.lod_max == 12.0f);
    assert(sampler_builder.compare_mode == 6 && sampler_builder.compare_func == 7);
    assert(sampler_builder.max_anisotropy == 8.0f);
    assert(sampler_builder.lod_bias == -1.5f && sampler_builder.reduction_filter == 2);
    sky_nvn_sampler_t sampler;
    assert(sky_nvn_sampler_initialize(&sampler, &sampler_builder) == 1);
    assert(sampler.magic == 0x504d4153 && sampler.dev == &device);
    sky_nvn_sampler_pool_register_sampler(&samplers, 0, &sampler);
    sky_nvn_sampler_pool_register_sampler_builder(&samplers, 1, &sampler_builder);
    sky_nvn_sampler_finalize(&sampler);
    assert(sampler.magic == 0);
    assert(sampler_storage[0] == 0xa5 && sampler_storage[63] == 0xa5);
    sky_nvn_sampler_pool_register_sampler(&samplers, -1, NULL);
    sky_nvn_sampler_pool_register_sampler(&samplers, 2, NULL);
    sky_nvn_sampler_pool_register_sampler_builder(&samplers, 2, NULL);
    sky_nvn_sampler_pool_t invalid_sampler_pool = {0};
    sky_nvn_sampler_pool_register_sampler(&invalid_sampler_pool, 0, NULL);
    assert(sampler_storage[0] == 0xa5 && sampler_storage[63] == 0xa5);
    sky_nvn_sampler_pool_finalize(&samplers);
    assert(samplers.magic == 0 && samplers.pool == NULL && samplers.count == 0);
    assert(sky_nvn_sampler_pool_get_memory_pool(&samplers) == NULL);
    assert(sky_nvn_sampler_pool_get_memory_offset(&samplers) == 0);
    assert(sky_nvn_sampler_pool_get_size(&samplers) == 0);
    sky_nvn_sampler_pool_finalize(&samplers);

    test_descriptor_index(&device, &memory);
    assert(texture_storage[0] == 0xa5 && sampler_storage[0] == 0xa5);
    sky_nvn_memory_pool_finalize(&memory);
    assert(sky_mtl_memory_pool_active_count() == 0);
    free(allocation);
    sky_nvn_device_finalize(&device);
    puts("T_NVN_DESCRIPTORS_OK storage-capacity-hash-collision-replacement-concurrent-resolve-pool-reuse-retained-lifetimes");
    return 0;
}
