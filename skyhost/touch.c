#include "touch.h"
#include <math.h>
#include <string.h>

/* Positions are on a 12x6 grid; each control keeps its grid distance from the
 * safe-area edges it is anchored to, so spare space opens up in the middle. */
enum { RIGHT = 1, BOTTOM = 2 };
static const struct {
    float x, y, radius;
    int bit;
    unsigned anchor;
    const char *label;
} layout[SKY_TOUCH_CONTROL_COUNT] = {
    [SKY_TOUCH_MOVE]   = {2.00f, 4.00f, 1.10f, -1, BOTTOM, "Move"},
    [SKY_TOUCH_LOOK]   = {10.20f, 4.85f, 1.10f, -1, RIGHT | BOTTOM, "Look"},
    [SKY_TOUCH_USE]    = {11.30f, 2.24f, .36f, 0, RIGHT | BOTTOM, "Use"},
    [SKY_TOUCH_BACK]   = {.60f, .50f, .37f, 1, 0, "Back"},
    [SKY_TOUCH_JUMP]   = {11.30f, 1.36f, .36f, 2, RIGHT | BOTTOM, "Jump"},
    [SKY_TOUCH_SNEAK]  = {11.30f, .48f, .36f, 4, RIGHT | BOTTOM, "Sneak"},
    [SKY_TOUCH_ATTACK] = {11.30f, 3.31f, .55f, 9, RIGHT | BOTTOM, "Attack"},
    [SKY_TOUCH_PAUSE]  = {.60f, .50f, .37f, 10, 0, "Pause"},
};

static int contains(const sky_touch_region_t *r, float x, float y) {
    double dx = (double)x - r->x, dy = (double)y - r->y;
    return dx * dx + dy * dy <= (double)r->radius * r->radius;
}

static int reachable(const sky_touch_t *t, int control) {
    return t->menu == 2 || (t->menu == 1 ? control == SKY_TOUCH_BACK : control != SKY_TOUCH_BACK);
}

static void position_back(sky_touch_t *t) {
    const sky_touch_region_t *pause = &t->regions[SKY_TOUCH_PAUSE];
    t->regions[SKY_TOUCH_BACK].x = pause->x + (t->menu == 2 ? 3.25f * pause->radius : 0.f);
    t->regions[SKY_TOUCH_BACK].y = pause->y;
}

static void rest_stick(sky_touch_t *t, sky_touch_control_t control) {
    t->regions[control].x = t->stick_home[control].x;
    t->regions[control].y = t->stick_home[control].y;
}

static void stick(const sky_touch_region_t *r, float x, float y, int32_t *sx, int32_t *sy) {
    double dx = ((double)x - r->x) / r->radius;
    double dy = ((double)r->y - y) / r->radius;
    double length = hypot(dx, dy);
    if (length <= SKY_TOUCH_DEAD_ZONE) { *sx = *sy = 0; return; }
    double magnitude = (fmin(length, 1.0) - SKY_TOUCH_DEAD_ZONE) / (1.0 - SKY_TOUCH_DEAD_ZONE);
    *sx = (int32_t)lround(dx / length * magnitude * 32767.0);
    *sy = (int32_t)lround(dy / length * magnitude * 32767.0);
}

static void update(sky_touch_t *t) {
    t->buttons = 0;
    t->lx = t->ly = t->rx = t->ry = 0;
    for (size_t i = 0; i < SKY_TOUCH_MAX_FINGERS; ++i) {
        sky_touch_finger_t *f = &t->fingers[i];
        if (!f->used) continue;
        const sky_touch_region_t *r = &t->regions[f->control];
        if (f->control == SKY_TOUCH_MOVE) stick(r, f->x, f->y, &t->lx, &t->ly);
        else if (f->control == SKY_TOUCH_LOOK) stick(r, f->x, f->y, &t->rx, &t->ry);
        else if (f->inside) t->buttons |= r->button;
    }
    if ((t->lx || t->ly) && !t->sneaking) t->buttons |= SKY_TOUCH_SPRINT_BUTTON;
}

void sky_touch_cancel(sky_touch_t *t) {
    if (!t) return;
    memset(t->fingers, 0, sizeof t->fingers);
    rest_stick(t, SKY_TOUCH_MOVE);
    rest_stick(t, SKY_TOUCH_LOOK);
    t->buttons = 0;
    t->lx = t->ly = t->rx = t->ry = 0;
}

void sky_touch_resize(sky_touch_t *t, float width, float height, sky_touch_insets_t safe) {
    if (!t) return;
    sky_touch_cancel(t);
    t->enabled = 0;
    memset(t->regions, 0, sizeof t->regions);
    memset(t->stick_home, 0, sizeof t->stick_home);
    if (!isfinite(width) || !isfinite(height) || !isfinite(safe.top) ||
        !isfinite(safe.left) || !isfinite(safe.bottom) || !isfinite(safe.right) ||
        width <= 0 || height <= 0 || safe.top < 0 || safe.left < 0 ||
        safe.bottom < 0 || safe.right < 0) return;
    float w = width - safe.left - safe.right, h = height - safe.top - safe.bottom;
    if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0) return;
    float unit = fminf(w / 12.f, h / 6.f);
    if (unit <= 0) return;
    float right = width - safe.right, bottom = height - safe.bottom;
    for (size_t i = 0; i < SKY_TOUCH_CONTROL_COUNT; ++i) {
        t->regions[i] = (sky_touch_region_t){
            layout[i].anchor & RIGHT ? right - (12.f - layout[i].x) * unit : safe.left + layout[i].x * unit,
            layout[i].anchor & BOTTOM ? bottom - (6.f - layout[i].y) * unit : safe.top + layout[i].y * unit,
            layout[i].radius * unit,
            layout[i].bit < 0 ? 0 : UINT64_C(1) << layout[i].bit,
            layout[i].label,
        };
    }
    for (size_t i = 0; i <= SKY_TOUCH_LOOK; ++i) {
        t->stick_home[i].x = t->regions[i].x;
        t->stick_home[i].y = t->regions[i].y;
    }
    position_back(t);
    t->width = width;
    t->height = height;
    t->enabled = 1;
}

void sky_touch_set_menu(sky_touch_t *t, int menu) {
    if (!t) return;
    t->menu = menu == 2 ? 2 : menu != 0;
    position_back(t);
}

int sky_touch_hit_test(const sky_touch_t *t, float x, float y) {
    if (!t || !t->enabled || !isfinite(x) || !isfinite(y) ||
        x < 0 || y < 0 || x > t->width || y > t->height) return -1;
    for (int i = SKY_TOUCH_LOOK + 1; i < SKY_TOUCH_CONTROL_COUNT; ++i)
        if (reachable(t, i) && contains(&t->regions[i], x, y)) return i;
    if (t->menu == 1) return -1;
    return x < t->width * .5f ? SKY_TOUCH_MOVE : SKY_TOUCH_LOOK;
}

int sky_touch_begin(sky_touch_t *t, uint64_t id, float x, float y) {
    if (!t) return 0;
    /* A reused UIKit identity must never retain its old button or stick. */
    sky_touch_end(t, id);
    int control = sky_touch_hit_test(t, x, y);
    if (control < 0) return 0;
    sky_touch_finger_t *available = NULL;
    for (size_t i = 0; i < SKY_TOUCH_MAX_FINGERS; ++i) {
        sky_touch_finger_t *f = &t->fingers[i];
        if (!f->used) { if (!available) available = f; }
        else if (control <= SKY_TOUCH_LOOK && (int)f->control == control) return 0;
    }
    if (!available) return 0;
    if (control <= SKY_TOUCH_LOOK) {
        t->regions[control].x = x;
        t->regions[control].y = y;
    } else if (control == SKY_TOUCH_SNEAK) {
        t->sneaking = !t->sneaking;
    }
    *available = (sky_touch_finger_t){id, x, y, 1, 1, (sky_touch_control_t)control};
    update(t);
    return 1;
}

void sky_touch_move(sky_touch_t *t, uint64_t id, float x, float y) {
    if (!t) return;
    if (!isfinite(x) || !isfinite(y)) { sky_touch_end(t, id); return; }
    for (size_t i = 0; i < SKY_TOUCH_MAX_FINGERS; ++i) {
        sky_touch_finger_t *f = &t->fingers[i];
        if (f->used && f->id == id) {
            f->x = x; f->y = y;
            f->inside = contains(&t->regions[f->control], x, y);
            update(t);
            return;
        }
    }
}

void sky_touch_end(sky_touch_t *t, uint64_t id) {
    if (!t) return;
    for (size_t i = 0; i < SKY_TOUCH_MAX_FINGERS; ++i) {
        sky_touch_finger_t *f = &t->fingers[i];
        if (!f->used || f->id != id) continue;
        f->used = 0;
        if (f->control <= SKY_TOUCH_LOOK) rest_stick(t, f->control);
    }
    update(t);
}

void sky_touch_state(const sky_touch_t *t, uint64_t *buttons,
    int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (buttons) *buttons = t ? t->buttons : 0;
    if (lx) *lx = t ? t->lx : 0;
    if (ly) *ly = t ? t->ly : 0;
    if (rx) *rx = t ? t->rx : 0;
    if (ry) *ry = t ? t->ry : 0;
}

const sky_touch_region_t *sky_touch_region(const sky_touch_t *t, sky_touch_control_t control) {
    return t && (unsigned)control < SKY_TOUCH_CONTROL_COUNT ? &t->regions[control] : NULL;
}

int sky_touch_control_active(const sky_touch_t *t, sky_touch_control_t control) {
    if (!t || (unsigned)control >= SKY_TOUCH_CONTROL_COUNT) return 0;
    for (size_t i = 0; i < SKY_TOUCH_MAX_FINGERS; ++i)
        if (t->fingers[i].used && t->fingers[i].control == control &&
            (control <= SKY_TOUCH_LOOK || t->fingers[i].inside)) return 1;
    return 0;
}
