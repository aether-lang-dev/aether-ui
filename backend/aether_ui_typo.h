/* aether_ui_typo.h -- a label's typography beyond family, size, weight,
 * italic, single underline and strike (which have calls of their own):
 * a double underline, super- or subscript, vertical placement in its box,
 * an indent, and a rotation. The values are kept here, by handle, for every
 * backend (and the driver's /widgets); a backend defines aeui_typo_apply,
 * which shows them its own way. Include it once per backend, after the
 * widget registry. */
#ifndef AETHER_UI_TYPO_H
#define AETHER_UI_TYPO_H

#include <stdlib.h>

typedef struct {
    int handle;
    int underline;   // 0 as set otherwise, 2 double (a single one is set_text_decoration's)
    int script;      // 0 none, 1 superscript, 2 subscript
    int valign;      // 0 the platform's (centred), 1 top, 2 middle, 3 bottom
    int indent;      // px from the leading edge
    double rotation; // degrees, counter-clockwise, -90 to 90
} AeuiTypo;

static AeuiTypo* aeui_typos = NULL;
static int aeui_typo_count = 0, aeui_typo_cap = 0;

static void aeui_typo_apply(int handle, const AeuiTypo* t);   /* the backend's */

static AeuiTypo* aeui_typo_of(int handle) {
    for (int i = 0; i < aeui_typo_count; i++) if (aeui_typos[i].handle == handle) return &aeui_typos[i];
    return NULL;
}

void aether_ui_text_set_typography(int handle, int underline, int script, int valign,
                                   int indent, double rotation) {
    if (handle <= 0) return;
    AeuiTypo* t = aeui_typo_of(handle);
    if (!t) {
        // A label that never had any keeps none (a plain grid sets nothing).
        if (underline == 0 && script == 0 && valign == 0 && indent == 0 && rotation == 0.0) return;
        if (aeui_typo_count >= aeui_typo_cap) {
            int nc = aeui_typo_cap ? aeui_typo_cap * 2 : 32;
            AeuiTypo* n = (AeuiTypo*)realloc(aeui_typos, sizeof(AeuiTypo) * (size_t)nc);
            if (!n) return;
            aeui_typos = n; aeui_typo_cap = nc;
        }
        t = &aeui_typos[aeui_typo_count++];
        t->handle = handle;
    }
    if (rotation > 90.0) rotation = 90.0;
    if (rotation < -90.0) rotation = -90.0;
    if (indent < 0) indent = 0;
    t->underline = underline == 2 ? 2 : 0;
    t->script = script == 1 || script == 2 ? script : 0;
    t->valign = valign >= 0 && valign <= 3 ? valign : 0;
    t->indent = indent;
    t->rotation = rotation;
    aeui_typo_apply(handle, t);
}

/* For /widgets: "u2 s1 v3 i8 r90", or "" for a label with none. */
const char* aether_ui_text_typography_impl(int handle) {
    static char buf[64];
    AeuiTypo* t = aeui_typo_of(handle);
    if (!t || (t->underline == 0 && t->script == 0 && t->valign == 0 && t->indent == 0 && t->rotation == 0.0)) return "";
    snprintf(buf, sizeof(buf), "u%d s%d v%d i%d r%d", t->underline, t->script, t->valign, t->indent, (int)(t->rotation + (t->rotation < 0 ? -0.5 : 0.5)));
    return buf;
}

#endif
