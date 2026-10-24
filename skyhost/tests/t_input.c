#include "input.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

#define BIT(n) (UINT64_C(1) << (n))
#define MS(n) (UINT64_C(1000000) * (n))

static void expect(const sky_input_t *input, uint64_t want_buttons,
                   int32_t want_lx, int32_t want_ly, int32_t want_rx, int32_t want_ry) {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    sky_input_state(input, &buttons, &lx, &ly, &rx, &ry);
    assert(buttons == want_buttons);
    assert(lx == want_lx && ly == want_ly && rx == want_rx && ry == want_ry);
}

static void test_bindings(void) {
    sky_input_t input = {0};
    static const struct { unsigned key, button; } bindings[] = {
        {36, 0}, {14, 0}, {53, 1}, {48, 1}, {49, 2}, {15, 3},
        {8, 4}, {3, 5}, {6, 7}, {20, 8}, {21, 9}, {24, 10},
        {27, 11}, {17, 11}, {18, 12}, {12, 13}, {19, 14},
        {123, 12}, {126, 13}, {124, 14}, {125, 15},
    };
    for (unsigned i = 0; i < sizeof bindings / sizeof bindings[0]; i++) {
        sky_input_key(&input, bindings[i].key, 1);
        sky_input_key(&input, bindings[i].key, 1); /* OS key repeat is idempotent. */
        expect(&input, BIT(bindings[i].button), 0, 0, 0, 0);
        sky_input_key(&input, bindings[i].key, 0);
        expect(&input, 0, 0, 0, 0, 0);
    }
    sky_input_key(&input, 36, 1);
    sky_input_key(&input, 14, 1);
    sky_input_key(&input, 36, 0);
    expect(&input, BIT(0), 0, 0, 0, 0); /* Releasing one binding retains the other. */
    sky_input_key(&input, 128, 1);
    sky_input_key(&input, 9999, 1);
    expect(&input, BIT(0), 0, 0, 0, 0);
}

static void test_movement_and_look(void) {
    sky_input_t input = {0};
    sky_input_key(&input, 13, 1); /* W */
    sky_input_key(&input, 0, 1); /* A */
    sky_input_key(&input, 38, 1); /* J */
    sky_input_key(&input, 34, 1); /* I */
    expect(&input, 0, -32767, 32767, -32767, 32767);
    sky_input_key(&input, 2, 1); /* D */
    sky_input_key(&input, 1, 1); /* S */
    sky_input_key(&input, 37, 1); /* L */
    sky_input_key(&input, 40, 1); /* K */
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_key(&input, 0, 0);
    sky_input_key(&input, 13, 0);
    sky_input_key(&input, 38, 0);
    sky_input_key(&input, 34, 0);
    expect(&input, 0, 32767, -32767, 32767, -32767);
    sky_input_reset(&input);
    expect(&input, 0, 0, 0, 0, 0);
}

static void test_modifiers(void) {
    sky_input_t input = {0};
    sky_input_set_modifiers(&input, SKY_INPUT_MOD_SHIFT | SKY_INPUT_MOD_CTRL | SKY_INPUT_MOD_ALT);
    expect(&input, BIT(6) | BIT(4), 0, 0, 0, 0);
    sky_input_key(&input, 8, 1); /* C duplicates Control; release is independent. */
    sky_input_set_modifiers(&input, SKY_INPUT_MOD_ALT);
    expect(&input, BIT(4), 0, 0, 0, 0);
    sky_input_key(&input, 8, 0);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_key(&input, 13, 1);
    sky_input_mouse_button(&input, 0, 1);
    sky_input_set_modifiers(&input, SKY_INPUT_MOD_COMMAND | SKY_INPUT_MOD_SHIFT);
    sky_input_key(&input, 12, 1); /* Command-Q must never become favorites. */
    sky_input_key(&input, 12, 1);
    sky_input_mouse_button(&input, 1, 1);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_set_modifiers(&input, 0);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_key(&input, 12, 0);
    sky_input_mouse_button(&input, 1, 0);
    sky_input_key(&input, 12, 1);
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_reset(&input);
    sky_input_set_modifiers(&input, ~0u);
    expect(&input, 0, 0, 0, 0, 0); /* Unknown modifier bits never become controls. */
}

static void test_press_time_modes(void) {
    sky_input_t input = {0};
    sky_input_key(&input, 53, 1);
    sky_input_set_mouse_captured(&input, 1);
    sky_input_key(&input, 53, 1);
    expect(&input, BIT(1), 0, 0, 0, 0);
    sky_input_key(&input, 53, 0);
    sky_input_key(&input, 53, 1);
    sky_input_set_mouse_captured(&input, 0);
    sky_input_key(&input, 53, 1);
    expect(&input, BIT(10), 0, 0, 0, 0);
    sky_input_key(&input, 53, 0);
    expect(&input, 0, 0, 0, 0, 0);
    for (unsigned captured = 0; captured < 2; captured++) {
        static const unsigned free_buttons[] = {0, 1, 5};
        static const unsigned captured_buttons[] = {9, 8, 5};
        for (unsigned button = 0; button < 3; button++) {
            unsigned action = captured ? captured_buttons[button] : free_buttons[button];
            sky_input_set_mouse_captured(&input, captured);
            sky_input_mouse_button(&input, button, 1);
            sky_input_set_mouse_captured(&input, !captured);
            sky_input_mouse_button(&input, button, 1);
            expect(&input, BIT(action), 0, 0, 0, 0);
            sky_input_mouse_button(&input, button, 0);
            expect(&input, 0, 0, 0, 0, 0);
        }
    }
    sky_input_mouse_button(&input, 3, 1);
    sky_input_mouse_button(&input, 9999, 1);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_set_mouse_captured(&input, 1);
    sky_input_mouse_button(&input, 0, 1);
    sky_input_mouse_button(&input, 1, 1);
    sky_input_mouse_button(&input, 2, 1);
    expect(&input, BIT(9) | BIT(8) | BIT(5), 0, 0, 0, 0);
    sky_input_mouse_button(&input, 0, 0);
    expect(&input, BIT(8) | BIT(5), 0, 0, 0, 0);
}

static void test_mouse_resampling(void) {
    sky_input_t input = {0};
    sky_input_mouse_motion(&input, 10, 10, 0); /* Free cursor never turns the camera. */
    sky_input_tick(&input, 0);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_set_mouse_captured(&input, 1);
    sky_input_tick(&input, 0);
    sky_input_mouse_motion(&input, 1, -2, MS(5));
    sky_input_mouse_motion(&input, 2, -1, MS(10));
    sky_input_tick(&input, MS(20));
    expect(&input, 0, 0, 0, 4000, 4000); /* Sum all events, then normalize elapsed time. */
    sky_input_tick(&input, MS(20));
    expect(&input, 0, 0, 0, 4000, 4000); /* Duplicate ticks do not drain a sample twice. */
    sky_input_tick(&input, MS(40));
    expect(&input, 0, 0, 0, 0, 0); /* An idle tick always releases look. */
    sky_input_set_mouse_sensitivity(&input, 600);
    sky_input_mouse_motion(&input, 2, 2, MS(50));
    sky_input_tick(&input, MS(60));
    expect(&input, 0, 0, 0, 1000, -1000);
    sky_input_mouse_motion(&input, -1, 1, MS(65));
    sky_input_mouse_motion(&input, 1, -1, MS(70));
    sky_input_tick(&input, MS(80));
    expect(&input, 0, 0, 0, 0, 0); /* Opposite events cancel within an interval. */
    sky_input_mouse_motion(&input, 1e300, -1e300, MS(90));
    sky_input_mouse_motion(&input, 1e300, -1e300, MS(90));
    assert(input.mouse_dx == 4096 && input.mouse_dy == -4096);
    sky_input_tick(&input, MS(100));
    expect(&input, 0, 0, 0, 32767, 32767);
    sky_input_mouse_motion(&input, NAN, 1, MS(101));
    sky_input_mouse_motion(&input, 1, INFINITY, MS(102));
    sky_input_tick(&input, MS(120));
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_mouse_motion(&input, 2, 0, MS(130));
    sky_input_tick(&input, MS(200));
    expect(&input, 0, 0, 0, 0, 0); /* Stale queued motion expires. */
    sky_input_mouse_motion(&input, 9, 0, MS(201));
    sky_input_mouse_motion(&input, 1, 0, MS(299)); /* Does not resurrect the old 9 pixels. */
    sky_input_tick(&input, MS(300));
    expect(&input, 0, 0, 0, 100, 0); /* 100 ms polling scales rather than amplifies. */
    sky_input_mouse_motion(&input, 1, 0, MS(301));
    sky_input_mouse_motion(&input, 100, 100, MS(300)); /* Out-of-order event is ignored. */
    sky_input_tick(&input, MS(320));
    expect(&input, 0, 0, 0, 500, 0);
    sky_input_tick(&input, MS(310)); /* Regressed clocks safely reset motion. */
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_set_mouse_sensitivity(&input, DBL_MAX);
    sky_input_mouse_motion(&input, 1, 0, MS(321));
    sky_input_tick(&input, MS(321));
    expect(&input, 0, 0, 0, 32767, 0); /* Finite extreme gain cannot poison a zero axis. */
    sky_input_set_mouse_captured(&input, 0);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_set_mouse_sensitivity(&input, NAN);
    assert(input.mouse_sensitivity == SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT);
    sky_input_set_mouse_sensitivity(&input, INFINITY);
    assert(input.mouse_sensitivity == SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT);
    sky_input_set_mouse_sensitivity(&input, 0);
    assert(input.mouse_sensitivity == SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT);
    sky_input_set_mouse_sensitivity(&input, -1);
    assert(input.mouse_sensitivity == SKY_INPUT_MOUSE_SENSITIVITY_DEFAULT);
}

static void test_keyboard_precedence_and_reset(void) {
    sky_input_t input = {0};
    sky_input_set_mouse_captured(&input, 1);
    sky_input_set_mouse_sensitivity(&input, 600);
    sky_input_tick(&input, 0);
    sky_input_mouse_motion(&input, 2, 2, MS(10));
    sky_input_tick(&input, MS(20));
    sky_input_key(&input, 38, 1);
    expect(&input, 0, 0, 0, -32767, 0);
    sky_input_key(&input, 37, 1);
    expect(&input, 0, 0, 0, 0, 0); /* Opposing keyboard look still overrides mouse. */
    sky_input_key(&input, 38, 0);
    sky_input_key(&input, 37, 0);
    expect(&input, 0, 0, 0, 1000, -1000);
    sky_input_set_modifiers(&input, SKY_INPUT_MOD_SHIFT | SKY_INPUT_MOD_CTRL);
    sky_input_mouse_button(&input, 0, 1);
    sky_input_key(&input, 13, 1);
    sky_input_key(&input, 53, 1);
    sky_input_mouse_motion(&input, 8, 8, MS(21));
    sky_input_reset(&input);
    expect(&input, 0, 0, 0, 0, 0);
    assert(!input.mouse_captured && !input.modifiers);
    assert(input.mouse_sensitivity == 600);
    sky_input_tick(&input, MS(40));
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_key(&input, 53, 1);
    expect(&input, BIT(1), 0, 0, 0, 0); /* Reset also resets Escape's mode latch. */
}

static void test_scroll(void) {
    sky_input_t input = {0};
    sky_input_scroll(&input, .25, 0);
    sky_input_scroll(&input, .25, MS(10));
    sky_input_scroll(&input, .25, MS(20));
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_scroll(&input, .25, MS(30));
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_tick(&input, MS(129));
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_tick(&input, MS(130));
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_scroll(&input, 1, MS(140));
    sky_input_scroll(&input, .25, MS(230)); /* Fractional motion extends the hold. */
    sky_input_tick(&input, MS(240));
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_tick(&input, MS(329));
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_tick(&input, MS(330));
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_scroll(&input, -.5, MS(340)); /* Sign change discards the old +.25 fraction. */
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_scroll(&input, -.5, MS(350));
    expect(&input, BIT(15), 0, 0, 0, 0);
    sky_input_scroll(&input, 1, MS(360)); /* Opposing whole line changes direction immediately. */
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_key(&input, 125, 1); /* Wheel and physical keys combine independently. */
    expect(&input, BIT(13) | BIT(15), 0, 0, 0, 0);
    sky_input_tick(&input, MS(460));
    expect(&input, BIT(15), 0, 0, 0, 0);
    sky_input_key(&input, 125, 0);
    sky_input_scroll(&input, -1e300, MS(470));
    assert(input.scroll_lines == 0);
    expect(&input, BIT(15), 0, 0, 0, 0);
    sky_input_scroll(&input, NAN, MS(500));
    sky_input_scroll(&input, INFINITY, MS(500));
    sky_input_scroll(&input, 0, MS(500));
    sky_input_scroll(&input, 1, MS(469)); /* An old event cannot reverse a newer event. */
    sky_input_tick(&input, MS(570));
    expect(&input, 0, 0, 0, 0, 0); /* Invalid/zero/old events never extend the hold. */
    sky_input_scroll(&input, .5, MS(580));
    sky_input_reset(&input);
    sky_input_scroll(&input, .5, MS(590));
    expect(&input, 0, 0, 0, 0, 0); /* Reset clears partial lines as well as held buttons. */
    sky_input_scroll(&input, .5, MS(600));
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_set_modifiers(&input, SKY_INPUT_MOD_COMMAND);
    sky_input_scroll(&input, 1, MS(610));
    sky_input_set_modifiers(&input, 0);
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_scroll(&input, .5, MS(620));
    expect(&input, 0, 0, 0, 0, 0);
    sky_input_tick(&input, MS(625));
    sky_input_scroll(&input, .5, MS(630));
    expect(&input, BIT(13), 0, 0, 0, 0);
    sky_input_tick(&input, MS(624)); /* Clock regression cannot leave wheel input latched. */
    expect(&input, 0, 0, 0, 0, 0);
}

int main(void) {
    test_bindings();
    test_movement_and_look();
    test_modifiers();
    test_press_time_modes();
    test_mouse_resampling();
    test_keyboard_precedence_and_reset();
    test_scroll();
    puts("T_INPUT OK bindings-modifiers-command-shortcuts-press-latches-mouse-resampling-scroll-idle-reset");
    return 0;
}
