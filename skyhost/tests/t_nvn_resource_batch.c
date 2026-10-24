#include "../skyhost.h"
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

/* Include production exactly once, interposing only its C-level mutex calls.
 * The ordinary pthread declaration above remains unchanged, and production
 * builds acquire no test counters or hooks. */
static int test_mutex_lock(pthread_mutex_t *mutex);
#define pthread_mutex_lock test_mutex_lock
#include "../nvn.c"
#undef pthread_mutex_lock

typedef struct { unsigned textures, samplers; } lock_counts_t;
static _Thread_local lock_counts_t g_test_locks;
static _Thread_local int g_count_locks;

static int test_mutex_lock(pthread_mutex_t *mutex) {
    if (g_count_locks) {
        if (mutex == &g_texture_registration_lock) g_test_locks.textures++;
        if (mutex == &g_sampler_registration_lock) g_test_locks.samplers++;
    }
    return pthread_mutex_lock(mutex);
}

/* Baseline oracle: the resolver before registry-lock batching. Every occupied
 * output slot independently finds and retains its resource under one lock.
 * In particular, images/textures share a namespace, explicit samplers override
 * combined handles, and sampler IDs are not additionally masked to 12 bits. */
static sky_mtl_texture_t *baseline_texture(const void *pool, int id) {
    assert(!test_mutex_lock(&g_texture_registration_lock));
    sky_texture_registration_t *entry = sky_nvn_texture_registration_find(pool, id);
    sky_mtl_texture_t *result = entry ? sky_mtl_texture_retain(
        entry->metal_view ? entry->metal_view : entry->texture.metal) : NULL;
    assert(!pthread_mutex_unlock(&g_texture_registration_lock));
    return result;
}

static sky_mtl_sampler_t *baseline_sampler(const void *pool, int id) {
    assert(!test_mutex_lock(&g_sampler_registration_lock));
    sky_sampler_registration_t *entry = sky_nvn_sampler_registration_find(pool, id);
    sky_mtl_sampler_t *result = entry ? sky_mtl_sampler_retain(entry->metal) : NULL;
    assert(!pthread_mutex_unlock(&g_sampler_registration_lock));
    return result;
}

static void baseline_resolve(sky_mtl_render_state_t *state) {
    memset(state->textures, 0, sizeof state->textures);
    memset(state->samplers, 0, sizeof state->samplers);
    memset(state->images, 0, sizeof state->images);
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 32; slot++)
            if (state->image_bound[stage][slot]) state->images[stage][slot] = baseline_texture(
                state->texture_pool, state->image_handles[stage][slot] & 0xfffff);
        for (int slot = 0; slot < 128; slot++) {
            uint64_t texture = state->texture_handles[stage][slot];
            uint64_t sampler = state->sampler_handles[stage][slot];
            if (state->texture_bound[stage][slot]) state->textures[stage][slot] = baseline_texture(
                state->texture_pool, texture & 0xfffff);
            if (!state->sampler_bound[stage][slot]) sampler = texture;
            if (state->sampler_bound[stage][slot] || state->texture_bound[stage][slot])
                state->samplers[stage][slot] = baseline_sampler(state->sampler_pool, (int)(sampler >> 20));
        }
    }
}

static void release_outputs(sky_mtl_render_state_t *state) {
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 128; slot++) {
            sky_mtl_texture_destroy(state->textures[stage][slot]);
            sky_mtl_sampler_destroy(state->samplers[stage][slot]);
        }
        for (int slot = 0; slot < 32; slot++) sky_mtl_texture_destroy(state->images[stage][slot]);
    }
    memset(state->textures, 0, sizeof state->textures);
    memset(state->samplers, 0, sizeof state->samplers);
    memset(state->images, 0, sizeof state->images);
}

/* The current private Metal sampler wrapper contains one retained native
 * object pointer (nvn_metal.m, struct sky_mtl_sampler). Read it with memcpy so
 * the fixture compares retained objects without a production testing API or
 * incorrectly requiring independently allocated wrappers to share addresses. */
static const void *sampler_identity(const sky_mtl_sampler_t *sampler) {
    const void *native = NULL;
    if (sampler) memcpy(&native, sampler, sizeof native);
    return native;
}

static lock_counts_t expected_baseline_locks(const sky_mtl_render_state_t *state) {
    lock_counts_t counts = {0};
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 32; slot++) counts.textures += !!state->image_bound[stage][slot];
        for (int slot = 0; slot < 128; slot++) {
            counts.textures += !!state->texture_bound[stage][slot];
            counts.samplers += !!(state->texture_bound[stage][slot] || state->sampler_bound[stage][slot]);
        }
    }
    return counts;
}

static lock_counts_t counted_resolve(void (*resolve)(sky_mtl_render_state_t *), sky_mtl_render_state_t *state) {
    g_test_locks = (lock_counts_t){0};
    g_count_locks = 1;
    resolve(state);
    g_count_locks = 0;
    return g_test_locks;
}

static void compare_resolvers(const char *name, const sky_mtl_render_state_t *input,
    sky_mtl_render_state_t *baseline, sky_mtl_render_state_t *batch) {
    *baseline = *batch = *input;
    /* Poison only borrowed output fields; a resolver must overwrite every one,
     * including unbound slots whose otherwise-valid handles are stale. */
    memset(baseline->textures, 0xa5, sizeof baseline->textures);
    memset(baseline->samplers, 0xa5, sizeof baseline->samplers);
    memset(baseline->images, 0xa5, sizeof baseline->images);
    *batch = *baseline;
    lock_counts_t old = counted_resolve(baseline_resolve, baseline);
    lock_counts_t current = counted_resolve(sky_nvn_graphics_resources_resolve, batch);
    lock_counts_t expected = expected_baseline_locks(input);
    assert(old.textures == expected.textures && old.samplers == expected.samplers);
    assert(current.textures == !!expected.textures && current.samplers == !!expected.samplers);
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 128; slot++) {
            assert(sky_mtl_texture_resource_id(baseline->textures[stage][slot]) ==
                sky_mtl_texture_resource_id(batch->textures[stage][slot]));
            assert(sampler_identity(baseline->samplers[stage][slot]) == sampler_identity(batch->samplers[stage][slot]));
        }
        for (int slot = 0; slot < 32; slot++)
            assert(sky_mtl_texture_resource_id(baseline->images[stage][slot]) ==
                sky_mtl_texture_resource_id(batch->images[stage][slot]));
    }
    assert(!memcmp(input->texture_bound, batch->texture_bound, sizeof batch->texture_bound));
    assert(!memcmp(input->sampler_bound, batch->sampler_bound, sizeof batch->sampler_bound));
    assert(!memcmp(input->image_bound, batch->image_bound, sizeof batch->image_bound));
    assert(!memcmp(input->texture_handles, batch->texture_handles, sizeof batch->texture_handles));
    assert(!memcmp(input->sampler_handles, batch->sampler_handles, sizeof batch->sampler_handles));
    assert(!memcmp(input->image_handles, batch->image_handles, sizeof batch->image_handles));
    printf("NVN_RESOURCE_BATCH_EQUIVALENT case=%s baseline_locks=%u/%u batch_locks=%u/%u\n",
        name, old.textures, old.samplers, current.textures, current.samplers);
}

typedef struct {
    sky_nvn_device_t device;
    sky_nvn_memory_pool_t memory;
    sky_nvn_texture_pool_t textures[2];
    sky_nvn_sampler_pool_t samplers[2];
    sky_nvn_texture_t first, second;
    sky_nvn_sampler_t nearest, linear;
    void *allocation;
} fixture_t;

static void make_source(fixture_t *fixture, sky_nvn_texture_t *texture, int size, int levels, int64_t offset) {
    sky_nvn_texture_builder_t builder = {0};
    sky_nvn_texture_builder_set_device(&builder, &fixture->device);
    sky_nvn_texture_builder_set_defaults(&builder);
    sky_nvn_texture_builder_set_format(&builder, 0x25);
    sky_nvn_texture_builder_set_size_2d(&builder, size, size);
    sky_nvn_texture_builder_set_levels(&builder, levels);
    sky_nvn_texture_builder_set_flags(&builder, 4);
    sky_nvn_texture_builder_set_storage(&builder, &fixture->memory, offset);
    assert(sky_nvn_texture_initialize(texture, &builder));
}

static void setup_fixture(fixture_t *fixture) {
    memset(fixture, 0, sizeof *fixture);
    sky_nvn_device_builder_t device_builder = {0};
    assert(sky_nvn_device_initialize(&fixture->device, &device_builder));
    assert(!posix_memalign(&fixture->allocation, (size_t)getpagesize(), 1 << 20));
    memset(fixture->allocation, 0, 1 << 20);
    sky_nvn_memory_pool_builder_t builder = {0};
    sky_nvn_memory_pool_builder_set_device(&builder, &fixture->device);
    sky_nvn_memory_pool_builder_set_storage(&builder, fixture->allocation, 1 << 20);
    sky_nvn_memory_pool_builder_set_flags(&builder, 0x22);
    assert(sky_nvn_memory_pool_initialize(&fixture->memory, &builder));
    for (int i = 0; i < 2; i++) {
        assert(sky_nvn_texture_pool_initialize(&fixture->textures[i], &fixture->memory, 0, 16385));
        assert(sky_nvn_sampler_pool_initialize(&fixture->samplers[i], &fixture->memory, 0, 4096));
    }
    make_source(fixture, &fixture->first, 8, 2, 0xa0000);
    make_source(fixture, &fixture->second, 4, 1, 0xa4000);
    sky_nvn_texture_view_t view;
    sky_nvn_texture_view_set_defaults(&view);
    sky_nvn_texture_view_set_levels(&view, 1, 1);
    sky_nvn_texture_pool_register_texture(&fixture->textures[0], 0, &fixture->first, &view);
    sky_nvn_texture_pool_register_image(&fixture->textures[0], 16384, &fixture->first, NULL);
    sky_nvn_texture_pool_register_texture(&fixture->textures[1], 0, &fixture->second, NULL);
    sky_nvn_sampler_builder_t sampler = {0};
    sky_nvn_sampler_builder_set_device(&sampler, &fixture->device);
    sky_nvn_sampler_builder_set_defaults(&sampler);
    assert(sky_nvn_sampler_initialize(&fixture->nearest, &sampler));
    sky_nvn_sampler_builder_set_min_mag_filter(&sampler, 1, 1);
    assert(sky_nvn_sampler_initialize(&fixture->linear, &sampler));
    sky_nvn_sampler_pool_register_sampler(&fixture->samplers[0], 0, &fixture->nearest);
    sky_nvn_sampler_pool_register_sampler(&fixture->samplers[0], 4095, &fixture->linear);
    sky_nvn_sampler_pool_register_sampler(&fixture->samplers[1], 0, &fixture->linear);
}

static sky_mtl_render_state_t mixed_state(fixture_t *fixture) {
    sky_mtl_render_state_t state = {.texture_pool = &fixture->textures[0], .sampler_pool = &fixture->samplers[0]};
    state.image_bound[0][31] = state.image_bound[2][0] = 1;
    state.image_handles[0][31] = 16384 | (UINT64_C(0xabc) << 20);
    state.image_handles[2][0] = 1234;
    state.texture_bound[0][0] = state.texture_bound[0][127] = state.texture_bound[5][127] = 1;
    state.texture_handles[0][127] = 16384 | (UINT64_C(4095) << 20);
    state.sampler_bound[0][0] = 1;
    state.sampler_handles[0][0] = UINT64_C(4095) << 20;
    state.sampler_bound[3][16] = state.sampler_bound[3][17] = 1;
    state.sampler_handles[3][17] = UINT64_C(4096) << 20;
    state.texture_bound[4][80] = state.texture_bound[2][90] = 1;
    state.texture_handles[4][80] = 1234;
    state.texture_handles[2][90] = UINT64_C(123) << 20;
    state.texture_handles[1][3] = 16384;
    state.sampler_handles[1][3] = UINT64_C(4095) << 20;
    return state;
}

static uint64_t batch_now_ns(void) {
    struct timespec value;
    assert(!clock_gettime(CLOCK_MONOTONIC, &value));
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static double time_resolver(void (*resolve)(sky_mtl_render_state_t *),
    const sky_mtl_render_state_t *input, unsigned iterations) {
    sky_mtl_render_state_t state = *input;
    uint64_t begin = batch_now_ns();
    for (unsigned i = 0; i < iterations; i++) {
        resolve(&state);
        release_outputs(&state);
    }
    return (double)(batch_now_ns() - begin) / iterations;
}

static int compare_double(const void *a, const void *b) {
    double first = *(const double *)a, second = *(const double *)b;
    return (first > second) - (first < second);
}

static void benchmark_case(const char *name, const sky_mtl_render_state_t *state, unsigned iterations) {
    enum { TRIALS = 9 };
    double baseline[TRIALS], batch[TRIALS];
    (void)time_resolver(baseline_resolve, state, 64);
    (void)time_resolver(sky_nvn_graphics_resources_resolve, state, 64);
    for (int trial = 0; trial < TRIALS; trial++) {
        if (trial & 1) {
            batch[trial] = time_resolver(sky_nvn_graphics_resources_resolve, state, iterations);
            baseline[trial] = time_resolver(baseline_resolve, state, iterations);
        } else {
            baseline[trial] = time_resolver(baseline_resolve, state, iterations);
            batch[trial] = time_resolver(sky_nvn_graphics_resources_resolve, state, iterations);
        }
    }
    qsort(baseline, TRIALS, sizeof *baseline, compare_double);
    qsort(batch, TRIALS, sizeof *batch, compare_double);
    /* Per-lock TLS/interposition overhead differs with the acquisition count;
     * this fixture timing is not a production CPU or game-FPS measurement. */
    printf("NVN_RESOURCE_BATCH_BENCH instrumentation=mutex-lock-interposition-tls case=%s iterations=%u trials=%d baseline_resolve_release_ns=%.1f batch_resolve_release_ns=%.1f reduction_percent=%.2f\n",
        name, iterations, TRIALS, baseline[TRIALS / 2], batch[TRIALS / 2],
        100.0 * (baseline[TRIALS / 2] - batch[TRIALS / 2]) / baseline[TRIALS / 2]);
}

static void benchmark(fixture_t *fixture) {
    sky_mtl_render_state_t sparse = {.texture_pool = &fixture->textures[0], .sampler_pool = &fixture->samplers[0]};
    sparse.texture_bound[1][0] = 1;
    benchmark_case("sparse-one-combined-binding", &sparse, 2048);
    sky_mtl_render_state_t dense = sparse;
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 32; slot++) {
            dense.image_bound[stage][slot] = 1;
            dense.image_handles[stage][slot] = slot & 1 ? 16384 : 0;
        }
        for (int slot = 0; slot < 128; slot++) {
            dense.texture_bound[stage][slot] = 1;
            dense.texture_handles[stage][slot] = slot & 1 ? 16384 | (UINT64_C(4095) << 20) : 0;
            dense.sampler_bound[stage][slot] = (slot % 3) == 0;
            dense.sampler_handles[stage][slot] = slot & 1 ? 0 : UINT64_C(4095) << 20;
        }
    }
    sky_mtl_render_state_t old, current;
    compare_resolvers("dense", &dense, &old, &current);
    release_outputs(&old); release_outputs(&current);
    benchmark_case("dense-all-stages-slots", &dense, 256);
}

typedef struct {
    fixture_t *fixture;
    int samplers;
    _Atomic int start, ready, done;
} replacement_race_t;

static void *replace_resources(void *context) {
    replacement_race_t *race = context;
    while (!atomic_load(&race->start)) sched_yield();
    for (int i = 0; i < 64; i++) {
        if (race->samplers) sky_nvn_sampler_pool_register_sampler(&race->fixture->samplers[0], 0,
            i & 1 ? &race->fixture->nearest : &race->fixture->linear);
        else sky_nvn_texture_pool_register_texture(&race->fixture->textures[0], 0,
            i & 1 ? &race->fixture->first : &race->fixture->second, NULL);
        atomic_store(&race->ready, 1);
        sched_yield();
    }
    atomic_store(&race->done, 1);
    return NULL;
}

static void test_replacement_races(fixture_t *fixture) {
    for (int samplers = 0; samplers < 2; samplers++) {
        replacement_race_t race = {.fixture = fixture, .samplers = samplers};
        pthread_t writer;
        assert(!pthread_create(&writer, NULL, replace_resources, &race));
        atomic_store(&race.start, 1);
        while (!atomic_load(&race.ready)) sched_yield();
        unsigned samples = 0;
        do {
            sky_mtl_render_state_t state = {.texture_pool = &fixture->textures[0], .sampler_pool = &fixture->samplers[0]};
            state.texture_bound[0][0] = state.texture_bound[5][127] = state.image_bound[3][31] = 1;
            state.sampler_bound[2][13] = 1;
            lock_counts_t locks = counted_resolve(sky_nvn_graphics_resources_resolve, &state);
            assert(locks.textures == 1 && locks.samplers == 1);
            uint64_t texture = sky_mtl_texture_resource_id(state.textures[0][0]);
            assert(texture == sky_mtl_texture_resource_id(fixture->first.metal) ||
                texture == sky_mtl_texture_resource_id(fixture->second.metal));
            assert(texture == sky_mtl_texture_resource_id(state.textures[5][127]) &&
                texture == sky_mtl_texture_resource_id(state.images[3][31]));
            const void *sampler = sampler_identity(state.samplers[0][0]);
            assert(sampler && (sampler == sampler_identity(fixture->nearest.metal) ||
                sampler == sampler_identity(fixture->linear.metal)));
            assert(sampler == sampler_identity(state.samplers[5][127]) && sampler == sampler_identity(state.samplers[2][13]));
            /* Each registry is coherent; the two independent registries need
             * not expose matching texture/sampler replacement generations. */
            release_outputs(&state);
            samples++;
        } while (samples < 1024 && (!atomic_load(&race.done) || samples < 64));
        assert(!pthread_join(writer, NULL));
        printf("NVN_RESOURCE_BATCH_REPLACEMENT_OK registry=%s replacements=64 snapshots=%u\n",
            samplers ? "samplers" : "textures-images", samples);
    }
}

int main(int argc, char **argv) {
    assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--bench")));
    size_t pools_before = sky_mtl_memory_pool_active_count(), textures_before = sky_mtl_texture_active_count();
    fixture_t fixture;
    setup_fixture(&fixture);
    sky_mtl_render_state_t input = {.texture_pool = &fixture.textures[0], .sampler_pool = &fixture.samplers[0]};
    sky_mtl_render_state_t old, current;
    compare_resolvers("empty", &input, &old, &current);
    release_outputs(&old); release_outputs(&current);
    input.image_bound[5][31] = 1;
    compare_resolvers("image-only", &input, &old, &current);
    release_outputs(&old); release_outputs(&current);
    memset(input.image_bound, 0, sizeof input.image_bound);
    input.sampler_bound[5][127] = 1;
    compare_resolvers("sampler-only", &input, &old, &current);
    release_outputs(&old); release_outputs(&current);
    input = mixed_state(&fixture);
    lock_counts_t expected = expected_baseline_locks(&input);
    assert(expected.textures == 7 && expected.samplers == 7);
    compare_resolvers("mixed-view-collision-overrides-missing", &input, &old, &current);
    assert(sky_mtl_texture_resource_id(current.textures[0][0]) == sky_mtl_texture_resource_id(current.textures[5][127]));
    assert(sampler_identity(current.samplers[0][0]) == sampler_identity(fixture.linear.metal) &&
        sampler_identity(current.samplers[0][127]) == sampler_identity(fixture.linear.metal));
    assert(sampler_identity(current.samplers[3][16]) == sampler_identity(fixture.nearest.metal) &&
        sampler_identity(current.samplers[5][127]) == sampler_identity(fixture.nearest.metal));
    assert(!current.samplers[3][17] && !current.samplers[2][90]);
    assert(!current.textures[4][80] && sampler_identity(current.samplers[4][80]) == sampler_identity(fixture.nearest.metal));
    assert(!current.images[2][0] && !current.textures[1][3] && !current.samplers[1][3]);
    sky_mtl_texture_properties_t properties;
    assert(sky_mtl_texture_get_properties(current.textures[0][0], &properties) && properties.width == 4);
    assert(sky_mtl_texture_get_properties(current.images[0][31], &properties) && properties.width == 8);
    if (argc == 2) benchmark(&fixture);
    test_replacement_races(&fixture);

    /* Retained duplicate slots keep their exact view/sampler alive across
     * replacement, source finalization, and retirement of the owning pools. */
    sky_nvn_texture_pool_register_image(&fixture.textures[0], 0, &fixture.second, NULL);
    sky_nvn_sampler_pool_register_sampler(&fixture.samplers[0], 4095, &fixture.nearest);
    sky_mtl_render_state_t replaced_old, replaced;
    compare_resolvers("replaced-same-ids", &input, &replaced_old, &replaced);
    assert(sky_mtl_texture_resource_id(replaced.textures[0][0]) == sky_mtl_texture_resource_id(fixture.second.metal) &&
        sky_mtl_texture_resource_id(replaced.images[0][31]) == sky_mtl_texture_resource_id(fixture.first.metal));
    assert(sampler_identity(replaced.samplers[0][0]) == sampler_identity(fixture.nearest.metal) &&
        sampler_identity(current.samplers[0][0]) == sampler_identity(fixture.linear.metal));
    release_outputs(&replaced_old); release_outputs(&replaced);
    sky_nvn_texture_finalize(&fixture.first); sky_nvn_texture_finalize(&fixture.second);
    sky_nvn_sampler_finalize(&fixture.nearest); sky_nvn_sampler_finalize(&fixture.linear);
    sky_nvn_texture_pool_finalize(&fixture.textures[0]); sky_nvn_sampler_pool_finalize(&fixture.samplers[0]);
    assert(sky_mtl_texture_get_properties(current.textures[0][0], &properties) && properties.width == 4);
    assert(sky_mtl_texture_get_properties(current.images[0][31], &properties) && properties.width == 8);
    sky_mtl_sampler_t *held = sky_mtl_sampler_retain(current.samplers[0][0]);
    assert(held); sky_mtl_sampler_destroy(held);
    assert(sky_nvn_texture_pool_initialize(&fixture.textures[0], &fixture.memory, 0, 16385));
    assert(sky_nvn_sampler_pool_initialize(&fixture.samplers[0], &fixture.memory, 0, 4096));
    compare_resolvers("retired-pool-address-reused", &input, &replaced_old, &replaced);
    for (int stage = 0; stage < 6; stage++) {
        for (int slot = 0; slot < 128; slot++) assert(!replaced.textures[stage][slot] && !replaced.samplers[stage][slot]);
        for (int slot = 0; slot < 32; slot++) assert(!replaced.images[stage][slot]);
    }
    release_outputs(&replaced_old); release_outputs(&replaced);
    input.texture_pool = &fixture.textures[1]; input.sampler_pool = &fixture.samplers[1];
    compare_resolvers("other-pool-identical-id", &input, &replaced_old, &replaced);
    assert(sky_mtl_texture_get_properties(replaced.textures[0][0], &properties) && properties.width == 4);
    assert(!replaced.images[0][31] && !replaced.samplers[0][0] && replaced.samplers[5][127]);
    release_outputs(&replaced_old); release_outputs(&replaced);
    release_outputs(&old); release_outputs(&current);
    for (int i = 0; i < 2; i++) {
        sky_nvn_texture_pool_finalize(&fixture.textures[i]);
        sky_nvn_sampler_pool_finalize(&fixture.samplers[i]);
    }
    sky_nvn_memory_pool_finalize(&fixture.memory);
    free(fixture.allocation);
    sky_nvn_device_finalize(&fixture.device);
    assert(sky_mtl_memory_pool_active_count() == pools_before && sky_mtl_texture_active_count() == textures_before);
    puts("T_NVN_RESOURCE_BATCH_OK baseline-equivalence-exact-lock-counts-views-duplicate-retains-replacement-retirement-pool-reuse-no-GPU-submissions");
    return 0;
}
