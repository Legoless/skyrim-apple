#include "touch.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void neutral(const sky_touch_t *t) {
    assert(!t->buttons && !t->lx && !t->ly && !t->rx && !t->ry);
}
static sky_touch_t phone(void) {
    sky_touch_t t = {0};
    sky_touch_resize(&t, 844, 390, (sky_touch_insets_t){0, 47, 21, 47});
    assert(t.enabled);
    return t;
}
static void begin(sky_touch_t *t, uint64_t id, sky_touch_control_t c) {
    const sky_touch_region_t *r = sky_touch_region(t, c);
    assert(r && sky_touch_begin(t, id, r->x, r->y));
}
static void layout_test(float w, float h, sky_touch_insets_t safe) {
    sky_touch_t t = {0};
    sky_touch_resize(&t, w, h, safe);
    assert(t.enabled);
    for (int i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i) {
        const sky_touch_region_t *r = sky_touch_region(&t, i);
        assert(r && r->label && r->radius > 0);
        assert(r->x - r->radius >= safe.left - .001f);
        assert(r->x + r->radius <= w - safe.right + .001f);
        assert(r->y - r->radius >= safe.top - .001f);
        assert(r->y + r->radius <= h - safe.bottom + .001f);
        sky_touch_set_menu(&t, i == SKY_TOUCH_BACK);
        assert(sky_touch_hit_test(&t, r->x, r->y) == i);
        sky_touch_set_menu(&t, 0);
        for (int j = i + 1; j < SKY_TOUCH_CONTROL_COUNT; ++j) {
            const sky_touch_region_t *b = sky_touch_region(&t, j);
            if (i == SKY_TOUCH_BACK && j == SKY_TOUCH_PAUSE) {
                assert(r->x == b->x && r->y == b->y && r->radius == b->radius);
                continue;
            }
            assert(hypotf(r->x - b->x, r->y - b->y) > r->radius + b->radius);
        }
    }
    /* The lower-right corner stays clear of buttons so Look starts under the right thumb. */
    const float unit = fminf((w - safe.left - safe.right) / 12, (h - safe.top - safe.bottom) / 6);
    for (float x = w - safe.right - 3 * unit; x <= w - safe.right; x += unit / 4)
        for (float y = h - safe.bottom - 2 * unit; y <= h - safe.bottom; y += unit / 4)
            assert(sky_touch_hit_test(&t, x, y) == SKY_TOUCH_LOOK);
    assert(sky_touch_hit_test(&t, -1, 0) == -1);
    assert(sky_touch_hit_test(&t, w + 1, h + 1) == -1);
    assert(sky_touch_region(&t, -1) == NULL);
    assert(!sky_touch_control_active(&t, SKY_TOUCH_CONTROL_COUNT));
}
static void mappings(void) {
    sky_touch_t t = phone();
    const struct { sky_touch_control_t control; unsigned bit; } map[] = {
        {SKY_TOUCH_USE, 0}, {SKY_TOUCH_JUMP, 2}, {SKY_TOUCH_SNEAK, 4},
        {SKY_TOUCH_ATTACK, 9}, {SKY_TOUCH_PAUSE, 10},
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; ++i) {
        begin(&t, i, map[i].control);
        assert(t.buttons == (UINT64_C(1) << map[i].bit));
        assert(sky_touch_control_active(&t, map[i].control));
        sky_touch_end(&t, i);
        neutral(&t);
    }
    sky_touch_set_menu(&t, 1);
    begin(&t, 99, SKY_TOUCH_BACK);
    assert(t.buttons == (UINT64_C(1) << 1));
    sky_touch_end(&t, 99);
    neutral(&t);
}
static void menu_mode(void) {
    sky_touch_t t = phone();
    const sky_touch_region_t *pause = sky_touch_region(&t, SKY_TOUCH_PAUSE);
    const sky_touch_region_t *use = sky_touch_region(&t, SKY_TOUCH_USE);
    assert(pause->x < 844 * .25f && pause->y < 390 * .25f); /* top-left */
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_PAUSE);
    sky_touch_set_menu(&t, 1);
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_BACK);
    assert(sky_touch_hit_test(&t, use->x, use->y) == -1);
    assert(sky_touch_hit_test(&t, 200, 200) == -1); /* no sticks in menus */
    assert(!sky_touch_begin(&t, 1, use->x, use->y));
    neutral(&t);
    sky_touch_resize(&t, 844, 390, (sky_touch_insets_t){0});
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_BACK);
    sky_touch_set_menu(&t, 0);
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_PAUSE);
}
static void controller_mode(void) {
    sky_touch_t t = phone();
    sky_touch_set_menu(&t, 2);
    const sky_touch_region_t *pause = sky_touch_region(&t, SKY_TOUCH_PAUSE);
    const sky_touch_region_t *back = sky_touch_region(&t, SKY_TOUCH_BACK);
    assert(back->x > pause->x && back->y == pause->y);
    assert(back->x - pause->x > back->radius + pause->radius);
    for (int control = 0; control < SKY_TOUCH_CONTROL_COUNT; control++) {
        const sky_touch_region_t *region = sky_touch_region(&t, control);
        assert(sky_touch_hit_test(&t, region->x, region->y) == control);
    }
    begin(&t, 1, SKY_TOUCH_PAUSE);
    assert(t.buttons == (UINT64_C(1) << 10));
    begin(&t, 2, SKY_TOUCH_BACK);
    begin(&t, 3, SKY_TOUCH_USE);
    assert(t.buttons == ((UINT64_C(1) << 10) | 3));
    sky_touch_end(&t, 2);
    assert(t.buttons == ((UINT64_C(1) << 10) | 1));
    sky_touch_end(&t, 3);
    assert(t.buttons == (UINT64_C(1) << 10));
    sky_touch_end(&t, 1);
    neutral(&t);
    sky_touch_resize(&t, 1194, 834, (sky_touch_insets_t){24, 0, 20, 0});
    assert(t.menu == 2 && back->x - pause->x > back->radius + pause->radius);
    assert(sky_touch_hit_test(&t, back->x, back->y) == SKY_TOUCH_BACK);
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_PAUSE);
    sky_touch_set_menu(&t, 1);
    assert(back->x == pause->x && back->y == pause->y);
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_BACK);
    sky_touch_set_menu(&t, 0);
    assert(sky_touch_hit_test(&t, pause->x, pause->y) == SKY_TOUCH_PAUSE);
}
static void corners(void) {
    sky_touch_t t = {0};
    sky_touch_resize(&t, 1194, 834, (sky_touch_insets_t){24, 0, 20, 0}); /* 4:3 leaves spare height */
    const sky_touch_region_t *pause = sky_touch_region(&t, SKY_TOUCH_PAUSE);
    assert(pause->y - pause->radius - 24 < pause->radius && pause->x < 1194 * .1f);
    /* One right-edge column, top to bottom, with Attack nearest the Look corner. */
    const sky_touch_control_t column[] = {SKY_TOUCH_SNEAK, SKY_TOUCH_JUMP, SKY_TOUCH_USE, SKY_TOUCH_ATTACK};
    for (unsigned i = 0; i < sizeof column / sizeof *column; ++i) {
        const sky_touch_region_t *r = sky_touch_region(&t, column[i]);
        assert(r->x > 1194 * .9f && r->x == sky_touch_region(&t, SKY_TOUCH_ATTACK)->x);
        if (i) assert(r->y > sky_touch_region(&t, column[i - 1])->y);
    }
}
static void floating(void) {
    sky_touch_t t = phone();
    const sky_touch_region_t *move = sky_touch_region(&t, SKY_TOUCH_MOVE);
    const sky_touch_region_t *look = sky_touch_region(&t, SKY_TOUCH_LOOK);
    const float move_x = move->x, move_y = move->y, look_x = look->x, look_y = look->y;
    assert(sky_touch_hit_test(&t, 100, 120) == SKY_TOUCH_MOVE);
    assert(sky_touch_hit_test(&t, 500, 80) == SKY_TOUCH_LOOK);
    assert(sky_touch_hit_test(&t, 1, 389) == SKY_TOUCH_MOVE); /* safe-area margins count */
    assert(sky_touch_begin(&t, 1, 100, 120));
    assert(move->x == 100 && move->y == 120);
    neutral(&t); /* centered under the finger, so landing never deflects */
    sky_touch_move(&t, 1, 100 + move->radius, 120);
    assert(t.lx == 32767 && !t.ly);
    assert(!sky_touch_begin(&t, 2, 300, 300)); /* Move half already owned */
    assert(move->x == 100 && move->y == 120);
    assert(sky_touch_begin(&t, 3, 500, 80));
    assert(look->x == 500 && look->y == 80);
    sky_touch_move(&t, 3, 500, 80 - look->radius);
    assert(t.ry == 32767 && !t.rx);
    sky_touch_end(&t, 1);
    assert(move->x == move_x && move->y == move_y);
    assert(!t.lx && !t.ly && t.ry == 32767);
    sky_touch_cancel(&t);
    assert(look->x == look_x && look->y == look_y);
    neutral(&t);
}
static void sprint(void) {
    sky_touch_t t = phone();
    const sky_touch_region_t *move = sky_touch_region(&t, SKY_TOUCH_MOVE);
    begin(&t, 1, SKY_TOUCH_MOVE);
    sky_touch_move(&t, 1, move->x + move->radius * .1f, move->y);
    neutral(&t); /* dead zone */
    sky_touch_move(&t, 1, move->x, move->y - move->radius * .5f);
    assert(t.ly > 0 && t.buttons == SKY_TOUCH_SPRINT_BUTTON);
    begin(&t, 2, SKY_TOUCH_SNEAK);
    assert(t.buttons == (UINT64_C(1) << 4));
    sky_touch_end(&t, 2);
    assert(t.ly > 0 && !t.buttons);
    sky_touch_cancel(&t); /* the game stays sneaking through interruptions */
    begin(&t, 3, SKY_TOUCH_MOVE);
    sky_touch_move(&t, 3, move->x + move->radius, move->y);
    assert(t.lx == 32767 && !t.buttons);
    begin(&t, 4, SKY_TOUCH_SNEAK);
    sky_touch_end(&t, 4);
    assert(t.buttons == SKY_TOUCH_SPRINT_BUTTON);
    sky_touch_end(&t, 3);
    neutral(&t);
}
static void simultaneous(void) {
    sky_touch_t t = phone();
    const sky_touch_region_t *l = sky_touch_region(&t, SKY_TOUCH_MOVE);
    const sky_touch_region_t *r = sky_touch_region(&t, SKY_TOUCH_LOOK);
    begin(&t, 100, SKY_TOUCH_MOVE);
    begin(&t, 200, SKY_TOUCH_LOOK);
    begin(&t, 300, SKY_TOUCH_ATTACK);
    begin(&t, 400, SKY_TOUCH_JUMP);
    sky_touch_move(&t, 100, l->x, l->y - l->radius);
    sky_touch_move(&t, 200, r->x + r->radius, r->y);
    assert(t.lx == 0 && t.ly == 32767 && t.rx == 32767 && t.ry == 0);
    assert(t.buttons == ((UINT64_C(1) << 2) | (UINT64_C(1) << 9) | SKY_TOUCH_SPRINT_BUTTON));
    assert(!sky_touch_begin(&t, 500, l->x, l->y)); /* single owner per stick */
    sky_touch_end(&t, 100);
    assert(!t.lx && !t.ly && t.rx == 32767);
    sky_touch_end(&t, 300);
    assert(t.buttons == (UINT64_C(1) << 2));
    sky_touch_end(&t, 200);
    sky_touch_end(&t, 400);
    neutral(&t);
    uint64_t buttons = 1;
    int32_t lx = 1, ly = 1, rx = 1, ry = 1;
    sky_touch_state(&t, &buttons, &lx, &ly, &rx, &ry);
    assert(!buttons && !lx && !ly && !rx && !ry);
}
static void owners(void) {
    sky_touch_t t = phone();
    const sky_touch_region_t *b = sky_touch_region(&t, SKY_TOUCH_USE);
    begin(&t, 0, SKY_TOUCH_USE); /* zero is a valid stable ID */
    begin(&t, 1, SKY_TOUCH_USE);
    sky_touch_end(&t, 0);
    assert(t.buttons == 1);
    sky_touch_move(&t, 1, -100, -100);
    neutral(&t);
    sky_touch_move(&t, 1, b->x, b->y);
    assert(t.buttons == 1);
    begin(&t, 1, SKY_TOUCH_JUMP); /* identity reuse releases old ownership */
    assert(t.buttons == 4);
    assert(!sky_touch_control_active(&t, SKY_TOUCH_USE));
    sky_touch_move(&t, UINT64_MAX, b->x, b->y);
    sky_touch_end(&t, UINT64_MAX);
    assert(t.buttons == 4);
    sky_touch_cancel(&t);
    for (uint64_t i = 0; i < SKY_TOUCH_MAX_FINGERS; ++i) begin(&t, i, SKY_TOUCH_ATTACK);
    assert(!sky_touch_begin(&t, 1000, b->x, b->y));
    sky_touch_end(&t, 0);
    begin(&t, 1000, SKY_TOUCH_USE);
    assert(t.buttons == (1 | (UINT64_C(1) << 9)));
    sky_touch_cancel(&t);
    neutral(&t);
}
static void analog(void) {
    sky_touch_t t = phone();
    const sky_touch_region_t *r = sky_touch_region(&t, SKY_TOUCH_MOVE);
    begin(&t, 8, SKY_TOUCH_MOVE);
    sky_touch_move(&t, 8, r->x + r->radius * .15f, r->y);
    neutral(&t);
    sky_touch_move(&t, 8, r->x + r->radius * .58f, r->y);
    assert(abs(t.lx - 16384) <= 1 && !t.ly);
    sky_touch_move(&t, 8, r->x, r->y - r->radius * .58f);
    assert(abs(t.ly - 16384) <= 1 && !t.lx); /* no isometric vertical skew */
    sky_touch_move(&t, 8, r->x + r->radius, r->y - r->radius);
    assert(abs(t.lx - 23170) <= 1 && t.lx == t.ly);
    assert(hypot(t.lx, t.ly) <= 32768);
    sky_touch_move(&t, 8, FLT_MAX, -FLT_MAX);
    assert(abs(t.lx - 23170) <= 1 && t.lx == t.ly);
    sky_touch_move(&t, 8, NAN, r->y);
    neutral(&t);
    assert(!sky_touch_control_active(&t, SKY_TOUCH_MOVE));
    assert(!sky_touch_begin(&t, 8, INFINITY, 3));
    assert(sky_touch_hit_test(&t, NAN, 0) == -1);
}
static void resize_cancel(void) {
    sky_touch_t t = phone();
    begin(&t, 1, SKY_TOUCH_ATTACK);
    sky_touch_resize(&t, 1194, 834, (sky_touch_insets_t){24, 0, 20, 0});
    neutral(&t);
    sky_touch_move(&t, 1, t.regions[SKY_TOUCH_ATTACK].x, t.regions[SKY_TOUCH_ATTACK].y);
    neutral(&t);
    begin(&t, 1, SKY_TOUCH_ATTACK);
    sky_touch_resize(&t, 0, 390, (sky_touch_insets_t){0});
    neutral(&t);
    assert(!t.enabled && sky_touch_hit_test(&t, 0, 0) == -1);
    sky_touch_resize(&t, NAN, 390, (sky_touch_insets_t){0});
    assert(!t.enabled);
    sky_touch_resize(&t, 844, 390, (sky_touch_insets_t){0, 900, 0, 0});
    assert(!t.enabled);
    sky_touch_resize(&t, 844, 390, (sky_touch_insets_t){-1, 0, 0, 0});
    assert(!t.enabled);
    sky_touch_resize(&t, 844, 390, (sky_touch_insets_t){0, INFINITY, 0, 0});
    assert(!t.enabled);
    sky_touch_resize(&t, FLT_MAX, FLT_MAX, (sky_touch_insets_t){0});
    assert(t.enabled);
    begin(&t, 0, SKY_TOUCH_MOVE);
    sky_touch_move(&t, 0, FLT_MAX, -FLT_MAX);
    assert(t.lx >= 0 && t.ly >= 0 && hypot(t.lx, t.ly) <= 32768);
    sky_touch_state(NULL, &t.buttons, &t.lx, &t.ly, &t.rx, &t.ry);
    neutral(&t);
    sky_touch_state(&t, NULL, NULL, NULL, NULL, NULL);
    sky_touch_cancel(NULL);
    sky_touch_resize(NULL, 0, 0, (sky_touch_insets_t){0});
    sky_touch_set_menu(NULL, 1);
}
int main(void) {
    layout_test(844, 390, (sky_touch_insets_t){0, 47, 21, 47});
    layout_test(1194, 834, (sky_touch_insets_t){24, 0, 20, 0});
    layout_test(320, 200, (sky_touch_insets_t){0});
    layout_test(390, 844, (sky_touch_insets_t){47, 0, 34, 0});
    mappings(); menu_mode(); controller_mode(); corners(); floating(); sprint();
    simultaneous(); owners(); analog(); resize_cancel();
    puts("touch tests passed");
    return 0;
}
