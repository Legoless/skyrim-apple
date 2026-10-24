#include "hid.h"
#include "skyhost.h"
#include <assert.h>
#include <stddef.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

static void test_unverified_build(void) {
    size_t size = 0x3400000;
    void *inaccessible = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(inaccessible != MAP_FAILED);
    sky_profile_configure_main("0000000000000000000000000000000000000000000000000000000000000000", NULL);
    assert(!sky_profile_has_update_ui());
    sky_set_main_module_base((uintptr_t)inaccessible);
    sky_set_guest_main_thread(pthread_self());
    setenv("SKY_TOUCH_UI", "1", 1);
    assert(sky_hid_initialize_npad() == 0); /* Must not install an update vtable hook. */
    sky_hid_menu_touch(0, 11, 0.2f, 0.7f);
    sky_hid_menu_touch(2, 11, 0.2f, 0.7f);
    sky_hid_set_host_state(4, 123, -456, 789, -234);
    uint32_t id = 0;
    sky_hid_npad_state_t state;
    for (unsigned sample = 0; sample < 3; sample++) {
        sky_hid_get_full_key_state(&state, &id);
        assert(state.buttons == (sample < 2 ? 5 : 4));
        assert(state.lx == 123 && state.ly == -456 && state.rx == 789 && state.ry == -234);
        assert(sky_ui_is_in_game());
    }
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    /* The exact update still performs its proven strap-screen integration. */
    assert(!mprotect(inaccessible, size, PROT_READ | PROT_WRITE));
    sky_profile_configure_main(SKY_UPDATE_MAIN_BUILD_ID, NULL);
    assert(sky_profile_has_update_ui());
    unsetenv("SKY_NO_FIRSTEXIT_FORCE");
    float *strap_timer = (float *)((uintptr_t)inaccessible + 0x32a7c04);
    *strap_timer = 1.f;
    sky_hid_get_full_key_state(&state, &id);
    assert(*(uint8_t *)((uintptr_t)inaccessible + 0x32a7c02) == 1);
    assert(*strap_timer > 0.f && *strap_timer < 0.05f && !sky_ui_is_in_game());
    sky_profile_configure_main(NULL, NULL);
    sky_set_main_module_base(0);
    sky_set_guest_main_thread((pthread_t)0);
    sky_set_ui_in_game(2); /* No guest: preserve UIKit controls-test state. */
    sky_hid_get_full_key_state(&state, &id);
    assert(sky_ui_is_in_game() == 2);
    sky_set_ui_in_game(0);
    assert(!munmap(inaccessible, size));
    puts("T_HID_BUILD_PROFILE OK inaccessible-unknown-main native-controller visible-controls tap-expires verified-update-retained");
}

static void *write_host_state(void *unused) {
    (void)unused;
    for (int32_t index = 1; index <= 100000; index++)
        sky_hid_set_host_state((uint64_t)index, index, -index, index + 1, -index - 1);
    return NULL;
}

typedef struct {
    _Atomic unsigned published, consumed;
    uint64_t deadline;
} tap_test_t;

static void *write_host_taps(void *argument) {
    tap_test_t *test = argument;
    for (unsigned index = 1; index <= 10000; index++) {
        atomic_store(&test->published, index);
        sky_hid_queue_host_tap(0, 0, 0, 0, 0, 1);
        while (atomic_load(&test->consumed) < index) {
            assert(now_ns() < test->deadline);
            sched_yield();
        }
    }
    return NULL;
}

static void test_atomic_taps(void) {
    uint32_t id = 0;
    sky_hid_npad_state_t state;
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    sky_hid_queue_host_state(4, 12, -34, 56, -78);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_queue_host_tap(2, 0, 0, 0, 0, 1);
    id = 1;
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.attributes && !state.buttons);
    id = 0;
    sky_hid_get_joy_dual_state(&state, &id);
    assert(!state.attributes && !state.buttons);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 7 && state.lx == 12 && state.ly == -34 && state.rx == 56 && state.ry == -78);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 2 && !state.lx && !state.ly && !state.rx && !state.ry);
    sky_hid_queue_host_tap(2, 0, 0, 0, 0, 1);
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons);

    tap_test_t test = {.deadline = now_ns() + 15000000000ull};
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, write_host_taps, &test));
    unsigned consumed = 0;
    while (consumed < 10000) {
        assert(now_ns() < test.deadline);
        sky_hid_get_full_key_state(&state, &id);
        if (state.buttons) {
            assert(state.buttons == 1);
            assert(++consumed <= atomic_load(&test.published));
            atomic_store(&test.consumed, consumed);
        }
    }
    assert(!pthread_join(writer, NULL));
    for (unsigned index = 0; index < 100; index++) {
        sky_hid_get_full_key_state(&state, &id);
        assert(!state.buttons);
    }
    puts("T_HID_ATOMIC_TAPS OK pending-preserved held-snapshot connected-only reset 10000-concurrent-single-sample-pulses");
}

static void reset_sources(void) {
    for (unsigned source = 0; source < 3; source++)
        sky_hid_set_source_state((sky_hid_source_t)source, 0, 0, 0, 0, 0);
}

static void expect_source_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint32_t id = 0;
    sky_hid_npad_state_t state;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 1 && state.buttons == buttons);
    assert(state.lx == lx && state.ly == ly && state.rx == rx && state.ry == ry);
}

static void test_source_isolation(void) {
    reset_sources();
    sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, 1, 11, -12, 13, -14);
    sky_hid_set_source_state(SKY_HID_SOURCE_TOUCH, 2, 21, -22, 23, -24);
    sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, 4, 31, -32, 33, -34);
    expect_source_state(7, 31, -32, 33, -34);
    sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, 0, 0, 0, 0, 0);
    expect_source_state(3, 21, -22, 23, -24);
    sky_hid_set_host_state(0, 0, 0, 0, 0); /* Legacy reset belongs to External. */
    expect_source_state(2, 21, -22, 23, -24);

    reset_sources();
    for (unsigned source = 0; source < 3; source++)
        sky_hid_set_source_state((sky_hid_source_t)source, 1, 0, 0, 0, 0);
    sky_hid_set_source_state(SKY_HID_SOURCE_DESKTOP, 0, 0, 0, 0, 0);
    expect_source_state(1, 0, 0, 0, 0);
    sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, 0, 0, 0, 0, 0);
    expect_source_state(1, 0, 0, 0, 0);
    sky_hid_set_source_state(SKY_HID_SOURCE_TOUCH, 0, 0, 0, 0, 0);
    expect_source_state(0, 0, 0, 0, 0);

    const sky_hid_source_t priority[] = {
        SKY_HID_SOURCE_DESKTOP, SKY_HID_SOURCE_TOUCH, SKY_HID_SOURCE_EXTERNAL,
    };
    for (unsigned cleared = 0; cleared < 3; cleared++) {
        reset_sources();
        for (unsigned source = 0; source < 3; source++) {
            int32_t value = 10 + source;
            sky_hid_queue_source_state((sky_hid_source_t)source, 1ull << source,
                value, -value, value + 20, -value - 20);
            sky_hid_queue_source_state((sky_hid_source_t)source, 0, 0, 0, 0, 0);
        }
        sky_hid_set_source_state((sky_hid_source_t)cleared, 0, 0, 0, 0, 0);
        sky_hid_source_t winner = priority[priority[0] == cleared ? 1 : 0];
        int32_t value = 10 + winner;
        expect_source_state(7 & ~(1ull << cleared), value, -value, value + 20, -value - 20);
        expect_source_state(0, 0, 0, 0, 0); /* Hidden lower-priority taps were drained too. */
    }

    reset_sources();
    sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, 1, 11, -12, 13, -14);
    sky_hid_queue_source_state(SKY_HID_SOURCE_TOUCH, 2, 21, -22, 23, -24);
    sky_hid_queue_source_state(SKY_HID_SOURCE_TOUCH, 0, 0, 0, 0, 0);
    const sky_hid_source_t invalid[] = {(sky_hid_source_t)-1, (sky_hid_source_t)3, (sky_hid_source_t)INT32_MAX};
    for (unsigned index = 0; index < sizeof invalid / sizeof invalid[0]; index++) {
        sky_hid_set_source_state(invalid[index], UINT64_MAX, 101, 102, 103, 104);
        sky_hid_queue_source_state(invalid[index], UINT64_MAX, 201, 202, 203, 204);
        sky_hid_queue_source_tap(invalid[index], UINT64_MAX, 301, 302, 303, 304, UINT64_MAX);
    }
    expect_source_state(3, 21, -22, 23, -24);
    expect_source_state(1, 11, -12, 13, -14);
    reset_sources();
    puts("T_HID_SOURCES OK independent-reset overlapping-held pending-isolation invalid-source legacy-external");
}

static void test_source_priority(void) {
    const sky_hid_source_t pairs[][2] = {
        {SKY_HID_SOURCE_DESKTOP, SKY_HID_SOURCE_TOUCH},
        {SKY_HID_SOURCE_DESKTOP, SKY_HID_SOURCE_EXTERNAL},
        {SKY_HID_SOURCE_TOUCH, SKY_HID_SOURCE_EXTERNAL},
    };
    for (unsigned index = 0; index < sizeof pairs / sizeof pairs[0]; index++) {
        sky_hid_source_t high = pairs[index][0], low = pairs[index][1];
        reset_sources();
        sky_hid_set_source_state(low, 1, 11, -12, 13, -14);
        sky_hid_set_source_state(high, 4, 21, -22, 23, -24);
        expect_source_state(5, 21, -22, 23, -24);
        sky_hid_set_source_state(high, 4, 0, 0, 31, -32);
        expect_source_state(5, 11, -12, 31, -32);
        sky_hid_set_source_state(high, 4, 41, -42, 0, 0);
        expect_source_state(5, 41, -42, 13, -14);

        sky_hid_set_source_state(high, 0, 0, 0, 0, 0);
        sky_hid_queue_source_state(high, 4, 51, -52, 53, -54);
        sky_hid_queue_source_state(high, 0, 0, 0, 0, 0);
        expect_source_state(5, 51, -52, 53, -54); /* Pending Desktop beats held Touch. */
        expect_source_state(1, 11, -12, 13, -14);

        reset_sources();
        sky_hid_queue_source_state(low, 1, 61, -62, 63, -64);
        sky_hid_queue_source_state(low, 0, 0, 0, 0, 0);
        sky_hid_set_source_state(high, 4, 71, -72, 73, -74);
        expect_source_state(5, 71, -72, 73, -74);
        expect_source_state(4, 71, -72, 73, -74);
        sky_hid_set_source_state(high, 0, 0, 0, 0, 0);
        expect_source_state(0, 0, 0, 0, 0);

        sky_hid_queue_source_state(high, 4, 81, -82, 83, -84);
        sky_hid_queue_source_state(high, 0, 0, 0, 0, 0);
        sky_hid_queue_source_state(high, 8, 91, -92, 0, 0);
        expect_source_state(12, 91, -92, 83, -84); /* Held wins within each stick. */
        expect_source_state(8, 91, -92, 0, 0);
    }
    reset_sources();
    puts("T_HID_SOURCE_PRIORITY OK all-pairs independent-sticks held-pending hidden-pending-drained");
}

static void *write_source_taps(void *argument) {
    tap_test_t *test = argument;
    for (unsigned index = 1; index <= 10000; index++) {
        atomic_store(&test->published, index);
        sky_hid_queue_source_tap(SKY_HID_SOURCE_DESKTOP, 4, 31, -32, 33, -34, 8);
        while (atomic_load(&test->consumed) < index) {
            assert(now_ns() < test->deadline);
            sched_yield();
        }
    }
    return NULL;
}

static void test_source_taps(void) {
    reset_sources();
    sky_hid_set_source_state(SKY_HID_SOURCE_EXTERNAL, 1, 11, -12, 13, -14);
    sky_hid_set_source_state(SKY_HID_SOURCE_TOUCH, 2, 21, -22, 23, -24);
    sky_hid_queue_source_tap(SKY_HID_SOURCE_DESKTOP, 4, 31, -32, 33, -34, 8);
    sky_hid_npad_state_t state;
    uint32_t id = 1;
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.attributes && !state.buttons && !state.lx && !state.ly && !state.rx && !state.ry);
    id = 0;
    sky_hid_get_joy_dual_state(&state, &id);
    assert(!state.attributes && !state.buttons);
    sky_hid_get_handheld_state(&state, &id);
    assert(!state.attributes && !state.buttons);
    sky_hid_get_full_key_state(&state, NULL);
    assert(!state.attributes && !state.buttons);
    sky_hid_get_full_key_state(NULL, &id);
    expect_source_state(15, 31, -32, 33, -34);
    expect_source_state(7, 31, -32, 33, -34);

    tap_test_t test = {.deadline = now_ns() + 15000000000ull};
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, write_source_taps, &test));
    unsigned consumed = 0;
    while (consumed < 10000) {
        assert(now_ns() < test.deadline);
        sky_hid_get_full_key_state(&state, &id);
        assert(state.buttons == 7 || state.buttons == 15);
        assert(state.lx == 31 && state.ly == -32 && state.rx == 33 && state.ry == -34);
        if (state.buttons & 8) {
            assert(++consumed <= atomic_load(&test.published));
            atomic_store(&test.consumed, consumed);
        }
    }
    assert(!pthread_join(writer, NULL));
    for (unsigned index = 0; index < 100; index++) expect_source_state(7, 31, -32, 33, -34);
    reset_sources();
    puts("T_HID_SOURCE_TAPS OK atomic-held-snapshot unsupported-null-nondrain 10000-concurrent-single-sample-pulses");
}

typedef struct {
    _Atomic unsigned ready, start, finished;
    uint64_t deadline;
} source_snapshot_test_t;

typedef struct {
    source_snapshot_test_t *test;
    sky_hid_source_t source;
    unsigned sticks;
} source_snapshot_writer_t;

enum { SOURCE_SNAPSHOT_ITERATIONS = 20000 };

static void publish_source_marker(const source_snapshot_writer_t *writer, unsigned marker) {
    int32_t value = 100000 + marker;
    sky_hid_set_source_state(writer->source, (uint64_t)marker << (16 * writer->source),
        writer->sticks & 1 ? value : 0, writer->sticks & 1 ? -2 * value : 0,
        writer->sticks & 2 ? value + 11 : 0, writer->sticks & 2 ? -3 * value : 0);
}

static void *write_source_snapshots(void *argument) {
    source_snapshot_writer_t *writer = argument;
    atomic_fetch_add(&writer->test->ready, 1);
    while (!atomic_load(&writer->test->start)) {
        assert(now_ns() < writer->test->deadline);
        sched_yield();
    }
    for (unsigned marker = 1; marker <= SOURCE_SNAPSHOT_ITERATIONS; marker++) {
        assert(now_ns() < writer->test->deadline);
        publish_source_marker(writer, marker);
        if (!(marker & 31)) sched_yield();
    }
    atomic_fetch_add(&writer->test->finished, 1);
    return NULL;
}

static void check_source_snapshots(const unsigned sticks[3], unsigned count) {
    reset_sources();
    source_snapshot_test_t test = {.deadline = now_ns() + 15000000000ull};
    source_snapshot_writer_t writers[3];
    pthread_t threads[3];
    uint64_t button_mask = 0;
    for (unsigned source = 0; source < count; source++) {
        writers[source] = (source_snapshot_writer_t){&test, (sky_hid_source_t)source, sticks[source]};
        publish_source_marker(&writers[source], 1);
        button_mask |= 0xffffull << (16 * source);
        assert(!pthread_create(&threads[source], NULL, write_source_snapshots, &writers[source]));
    }
    while (atomic_load(&test.ready) < count) {
        assert(now_ns() < test.deadline);
        sched_yield();
    }
    atomic_store(&test.start, 1);
    const sky_hid_source_t priority[] = {
        SKY_HID_SOURCE_DESKTOP, SKY_HID_SOURCE_TOUCH, SKY_HID_SOURCE_EXTERNAL,
    };
    uint32_t id = 0;
    for (unsigned reads = 0; reads < 40000 || atomic_load(&test.finished) < count; reads++) {
        assert(now_ns() < test.deadline);
        sky_hid_npad_state_t state;
        sky_hid_get_full_key_state(&state, &id);
        assert(state.attributes == 1 && !(state.buttons & ~button_mask));
        unsigned markers[3] = {0};
        for (unsigned source = 0; source < count; source++) {
            markers[source] = (state.buttons >> (16 * source)) & 0xffff;
            assert(markers[source] >= 1 && markers[source] <= SOURCE_SNAPSHOT_ITERATIONS);
        }
        int32_t expected[4] = {0};
        for (unsigned stick = 0; stick < 2; stick++) {
            for (unsigned index = 0; index < 3; index++) {
                sky_hid_source_t source = priority[index];
                if ((unsigned)source >= count || !(sticks[source] & (1u << stick))) continue;
                int32_t value = 100000 + markers[source];
                expected[2 * stick] = value + (stick ? 11 : 0);
                expected[2 * stick + 1] = -(stick ? 3 : 2) * value;
                break;
            }
        }
        assert(state.lx == expected[0] && state.ly == expected[1]);
        assert(state.rx == expected[2] && state.ry == expected[3]);
        if (!(reads & 63)) sched_yield();
    }
    for (unsigned source = 0; source < count; source++) assert(!pthread_join(threads[source], NULL));
    reset_sources();
}

static void test_source_snapshots(void) {
    const unsigned external_sticks[3] = {3, 0, 0};
    const unsigned split_sticks[3] = {3, 1, 2};
    const unsigned desktop_sticks[3] = {3, 3, 3};
    const unsigned touch_sticks[3] = {3, 0, 3};
    check_source_snapshots(external_sticks, 2);
    check_source_snapshots(split_sticks, 3);
    check_source_snapshots(desktop_sticks, 3);
    check_source_snapshots(touch_sticks, 3);
    puts("T_HID_SOURCE_SNAPSHOTS OK two-three-writers source-button-or xy-coherence full-snapshots int32-range");
}

int main(void) {
    assert(sky_hid_initialize_npad() == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_hid_initialize"));
    sky_hid_set_supported_npad_style_set(7);
    const uint32_t ids[] = {0, 1, 2};
    sky_hid_set_supported_npad_id_type(ids, 3);
    sky_hid_set_npad_joy_hold_type(1);

    uint32_t id = 0;
    assert(sky_hid_get_npad_style_set(&id) == 1);
    id = 1;
    assert(sky_hid_get_npad_style_set(&id) == 0);
    assert(sizeof(sky_hid_npad_state_t) == 0x28);
    assert(offsetof(sky_hid_npad_state_t, attributes) == 0x20);

    sky_hid_npad_state_t state;
    id = 0;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.sampling == 1 && state.attributes == 1);
    assert(state.buttons == 0);
    assert(state.lx == 0 && state.ly == 0 && state.rx == 0 && state.ry == 0);
    sky_hid_get_joy_dual_state(&state, &id);
    assert(state.sampling == 2 && state.attributes == 0);
    sky_hid_get_handheld_state(&state, &id);
    assert(state.sampling == 3 && state.attributes == 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.sampling == 4 && state.attributes == 1);
    id = 1;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.sampling == 5 && state.attributes == 0);

    sky_hid_set_host_state((1ull << 0) | (1ull << 10), 32767, -32767, 123, -456);
    id = 0;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 1 && state.buttons == ((1ull << 0) | (1ull << 10)));
    assert(state.lx == 32767 && state.ly == -32767 && state.rx == 123 && state.ry == -456);
    id = 1;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 0 && !state.buttons && !state.lx && !state.ly && !state.rx && !state.ry);
    id = 0;
    sky_hid_get_handheld_state(&state, &id);
    assert(state.attributes == 0 && !state.buttons && !state.lx && !state.ly && !state.rx && !state.ry);
    sky_hid_get_joy_dual_state(&state, &id);
    assert(state.attributes == 0 && !state.buttons && !state.lx && !state.ly && !state.rx && !state.ry);

    sky_hid_set_host_state(1, 1, -1, 2, -2);
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, write_host_state, NULL));
    for (int index = 0; index < 100000; index++) {
        sky_hid_get_full_key_state(&state, &id);
        assert(state.buttons == (uint64_t)state.lx);
        assert(state.ly == -state.lx && state.rx == state.lx + 1 && state.ry == -state.lx - 1);
    }
    assert(!pthread_join(writer, NULL));
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 1 && !state.buttons && !state.lx && !state.ly && !state.rx && !state.ry);

    uint32_t motion_handles[2] = {0, 0xdeadbeef};
    id = 0;
    assert(sky_hid_get_six_axis_handles(motion_handles, 2, &id, 1) == 1);
    assert(motion_handles[0] == 0x00020003 && motion_handles[1] == 0xdeadbeef);
    assert(sky_hid_get_six_axis_handles(motion_handles, 0, &id, 1) == 0);
    assert(sky_hid_get_six_axis_handles(motion_handles, 1, &id, 4) == 0);
    id = 1;
    assert(sky_hid_get_six_axis_handles(motion_handles, 1, &id, 1) == 0);
    sky_hid_six_axis_state_t motion[2];
    memset(motion, 0xa5, sizeof motion);
    assert(sky_hid_get_six_axis_states(motion, 2, motion_handles) == 0);
    assert(sky_hid_start_six_axis_sensor(motion_handles) == 0);
    assert(sky_hid_is_six_axis_sensor_at_rest(motion_handles));
    assert(sky_hid_get_six_axis_states(motion, 2, motion_handles) == 1);
    assert(motion[0].sampling == 1 && motion[0].delta_time == 5000000 && motion[0].attributes == 1);
    assert(motion[0].acceleration[0] == 0 && motion[0].acceleration[1] == 0 && motion[0].acceleration[2] == -1);
    for (int index = 0; index < 3; index++) {
        assert(motion[0].angular_velocity[index] == 0 && motion[0].angle[index] == 0);
        for (int column = 0; column < 3; column++)
            assert(motion[0].direction[index * 3 + column] == (index == column ? 1.f : 0.f));
    }
    const uint8_t *untouched = (const uint8_t *)&motion[1];
    for (size_t index = 0; index < sizeof motion[1]; index++) assert(untouched[index] == 0xa5);
    sky_hid_get_six_axis_state(motion, motion_handles);
    assert(motion[0].sampling == 2);
    assert(sky_hid_stop_six_axis_sensor(motion_handles) == 0);
    assert(sky_hid_get_six_axis_states(motion, 2, motion_handles) == 0);
    sky_hid_get_six_axis_state(motion, motion_handles);
    assert(motion[0].sampling == 0 && motion[0].attributes == 0);
    uint32_t bad_handle = 0;
    assert(sky_hid_start_six_axis_sensor(&bad_handle) != 0);
    assert(!sky_hid_is_six_axis_sensor_at_rest(&bad_handle));

    uint32_t vibration_handles[3] = {0, 0, 0xdeadbeef};
    id = 0;
    assert(sky_hid_get_vibration_handles(vibration_handles, 3, &id, 1) == 2);
    assert(vibration_handles[0] == 3 && vibration_handles[1] == 0x10003 && vibration_handles[2] == 0xdeadbeef);
    assert(sky_hid_get_vibration_handles(vibration_handles, 1, &id, 1) == 1);
    assert(sky_hid_get_vibration_handles(vibration_handles, 0, &id, 1) == 0);
    id = 1;
    assert(sky_hid_get_vibration_handles(vibration_handles, 2, &id, 1) == 0);
    sky_hid_vibration_value_t requested = {1.f, 160.f, .5f, 320.f}, actual;
    assert(sky_hid_send_vibration_value(&vibration_handles[0], &requested) != 0);
    for (int index = 0; index < 2; index++) {
        assert(sky_hid_initialize_vibration_device(&vibration_handles[index]) == 0);
        sky_hid_vibration_info_t info = {0xffffffff, 0xffffffff};
        assert(sky_hid_get_vibration_info(&info, &vibration_handles[index]) == 0);
        assert(info.type == 0 && info.position == 0);
        assert(sky_hid_send_vibration_value(&vibration_handles[index], &requested) == 0);
        assert(sky_hid_get_actual_vibration_value(&actual, &vibration_handles[index]) == 0);
        assert(actual.amplitude_low == 0 && actual.amplitude_high == 0);
        assert(actual.frequency_low == 160 && actual.frequency_high == 320);
    }
    assert(sky_hid_initialize_vibration_device(&bad_handle) != 0);
    sky_hid_keyboard_state_t keyboard;
    memset(&keyboard, 0xa5, sizeof keyboard);
    sky_hid_get_keyboard_state(&keyboard);
    assert(keyboard.sampling == 1 && !keyboard.modifiers);
    for (int index = 0; index < 4; index++) assert(!keyboard.keys[index]);
    sky_hid_get_keyboard_state(&keyboard);
    assert(keyboard.sampling == 2);

    id = 0;
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    sky_hid_queue_host_state(1, 123, -456, 0, 0);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 1 && state.buttons == 1);
    assert(state.lx == 123 && state.ly == -456);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons && !state.lx && !state.ly);

    sky_hid_queue_host_state(1, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 1);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 1);  /* Held state persists after consuming the edge. */
    sky_hid_queue_host_state(1, 0, 0, 0, 0);  /* Repeat down is not a rising edge. */
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons);

    sky_hid_queue_host_state(1, 0, 0, 0, 0);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_joy_dual_state(&state, &id);
    assert(!state.buttons && !state.attributes);
    sky_hid_get_handheld_state(&state, &id);
    assert(!state.buttons && !state.attributes);
    id = 1;
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons && !state.attributes);
    id = 0;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 1 && state.attributes == 1);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons);

    sky_hid_queue_host_state(1, 0, 0, 0, 0);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_set_host_state(0, 0, 0, 0, 0);  /* Focus reset discards pending input. */
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons);

    sky_hid_queue_host_state(1, 0, 0, 0, 0);
    sky_hid_queue_host_state(2, 0, 0, 0, 0);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 3);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.buttons);
    puts("T_HID_EDGES OK fast-tap held-repeat disconnected-read focus-reset");

    test_atomic_taps();

    sky_hid_queue_host_state(0, 12, -34, 56, -78);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_joy_dual_state(&state, &id);
    assert(!state.lx && !state.ly && !state.rx && !state.ry);
    sky_hid_get_handheld_state(&state, &id);
    assert(!state.lx && !state.ly && !state.rx && !state.ry);
    id = 1;
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.lx && !state.ly && !state.rx && !state.ry);
    id = 0;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.lx == 12 && state.ly == -34 && state.rx == 56 && state.ry == -78);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.lx && !state.ly && !state.rx && !state.ry);

    sky_hid_queue_host_state(0, 0, 0, -32767, 32767);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.rx == -32767 && state.ry == 32767);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.rx == -32767 && state.ry == 32767);
    sky_hid_queue_host_state(0, 0, 0, -32767, 32767); /* Held repeat. */
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.rx && !state.ry);

    sky_hid_queue_host_state(0, 1, 2, 3, 4);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_queue_host_state(0, -10, 0, 0, -20); /* Newest vector wins. */
    sky_hid_get_full_key_state(&state, &id);
    assert(state.lx == -10 && !state.ly && !state.rx && state.ry == -20);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.lx && !state.ly && !state.rx && !state.ry);

    sky_hid_queue_host_state(0, 1, 2, 3, 4);
    sky_hid_queue_host_state(0, 0, 0, 0, 0);
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    sky_hid_get_full_key_state(&state, &id);
    assert(!state.lx && !state.ly && !state.rx && !state.ry);
    puts("T_HID_STICKS OK short-tap held-repeat replacement connected-only focus-reset");

    test_source_isolation();
    test_source_priority();
    test_source_taps();
    test_source_snapshots();

    uint8_t support_arg[0x210];
    memset(support_arg, 0xa5, sizeof support_arg);
    sky_hid_controller_support_arg_set_default(support_arg);
    for (size_t index = 0; index < sizeof support_arg; index++) assert(support_arg[index] == 0);
    sky_hid_controller_support_result_t support_result = {-1, {1, 2, 3}, -1};
    assert(sky_hid_show_controller_support(&support_result, support_arg) == 0);
    assert(support_result.player_count == 1 && support_result.selected_id == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_hid_controller_support"));

    sky_hid_menu_touch(0, 1, 0.5f, 0.5f);
    sky_hid_menu_touch(2, 1, 0.5f, 0.5f);
    id = 0;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 1);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 1);
    sky_hid_get_full_key_state(&state, &id);
    assert(state.buttons == 0);

    test_unverified_build();
    puts("T_HID OK init style neutral-state-0x28 sampling controller-support menu-touch");
    return 0;
}
