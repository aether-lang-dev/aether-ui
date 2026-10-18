/* aether_ui_scrollbar.h -- the scrollbar's (windowing slider's) state and
 * rules, shared by every backend: range, window, value, steps and the
 * callback live here, keyed by handle; a backend makes the native control,
 * shows the state it is given (aeui_sb_render) and reports what a person
 * did through aether_ui_scrollbar_user_set. Include it once per backend,
 * after AeClosure is defined, and define aeui_sb_render. */
#ifndef AETHER_UI_SCROLLBAR_H
#define AETHER_UI_SCROLLBAR_H

#include <stdlib.h>

/* Integer resolution for a backend whose native control counts in ints. */
#define AEUI_SB_UNITS 10000

typedef struct {
    int handle;
    int vertical;
    double min, max, page, value, step, page_step;
    AeClosure* cb;
} AeuiScrollbar;

static AeuiScrollbar* aeui_sbs = NULL;
static int aeui_sb_count = 0, aeui_sb_cap = 0;

static AeuiScrollbar* aeui_sb_get(int handle) {
    for (int i = 0; i < aeui_sb_count; i++) if (aeui_sbs[i].handle == handle) return &aeui_sbs[i];
    return NULL;
}

/* The highest value: the window's start when it shows the end. */
static double aeui_sb_top(const AeuiScrollbar* s) {
    double t = s->max - s->page;
    return t < s->min ? s->min : t;
}

static double aeui_sb_clamp(const AeuiScrollbar* s, double v) {
    if (v > aeui_sb_top(s)) v = aeui_sb_top(s);
    if (v < s->min) v = s->min;
    return v;
}

/* A new record for a control just made (its handle given after it is
 * registered, aeui_sb_bind). */
static AeuiScrollbar* aeui_sb_new(int vertical, double min, double max, double page, double value, void* boxed) {
    if (aeui_sb_count >= aeui_sb_cap) {
        int nc = aeui_sb_cap ? aeui_sb_cap * 2 : 8;
        AeuiScrollbar* n = (AeuiScrollbar*)realloc(aeui_sbs, sizeof(AeuiScrollbar) * (size_t)nc);
        if (!n) return NULL;
        aeui_sbs = n; aeui_sb_cap = nc;
    }
    AeuiScrollbar* s = &aeui_sbs[aeui_sb_count++];
    s->handle = 0;
    s->vertical = vertical ? 1 : 0;
    s->min = min;
    s->max = max < min ? min : max;
    s->page = page < 0 ? 0 : page;
    s->step = 1;
    s->page_step = 0;
    s->cb = (AeClosure*)boxed;
    s->value = aeui_sb_clamp(s, value);
    return s;
}

static void aeui_sb_render(AeuiScrollbar* s);   /* the backend's */

/* The share of the track the thumb takes, 0..1, and where its start sits,
 * 0..1 of the room it moves in: what a drawn or a native thumb shows. */
static double aeui_sb_proportion(const AeuiScrollbar* s) {
    double span = s->max - s->min;
    if (span <= 0) return 1.0;
    double p = s->page / span;
    return p > 1 ? 1 : (p < 0 ? 0 : p);
}
static double aeui_sb_fraction(const AeuiScrollbar* s) {
    double room = aeui_sb_top(s) - s->min;
    return room <= 0 ? 0 : (s->value - s->min) / room;
}
static double aeui_sb_page_step(const AeuiScrollbar* s) {
    return s->page_step > 0 ? s->page_step : (s->page > 0 ? s->page : 1);
}

void aether_ui_scrollbar_configure(int handle, double min, double max, double page, double value) {
    AeuiScrollbar* s = aeui_sb_get(handle);
    if (!s) return;
    s->min = min;
    s->max = max < min ? min : max;
    s->page = page < 0 ? 0 : page;
    s->value = aeui_sb_clamp(s, value);
    aeui_sb_render(s);
}

void aether_ui_scrollbar_set_steps(int handle, double step, double page_step) {
    AeuiScrollbar* s = aeui_sb_get(handle);
    if (!s) return;
    s->step = step > 0 ? step : 1;
    s->page_step = page_step > 0 ? page_step : 0;
    aeui_sb_render(s);
}

double aether_ui_scrollbar_info(int handle, int what) {
    AeuiScrollbar* s = aeui_sb_get(handle);
    if (!s) return 0;
    switch (what) {
        case 0: return s->value;
        case 1: return s->min;
        case 2: return s->max;
        case 3: return s->page;
        case 4: return s->step;
        case 5: return aeui_sb_page_step(s);
        default: return 0;
    }
}

void aether_ui_scrollbar_user_set(int handle, double value) {
    AeuiScrollbar* s = aeui_sb_get(handle);
    if (!s) return;
    double v = aeui_sb_clamp(s, value);
    int moved = v != s->value;
    s->value = v;
    aeui_sb_render(s);
    if (moved && s->cb && s->cb->fn) ((void(*)(void*, double))s->cb->fn)(s->cb->env, v);
}

#endif
