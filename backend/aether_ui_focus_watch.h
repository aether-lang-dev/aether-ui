/* aether_ui_focus_watch.h -- on_focus_change, shared by every backend (each
 * includes it once). A watcher hears when keyboard focus comes into, or
 * leaves, a widget or anything inside it ("focus within"). Each backend
 * calls aeui_focus_watch_check() wherever focus may have moved (GTK's
 * window focus-widget, AppKit's first responder, Win32's focused window
 * after a message, UIKit's text editing begin/end, Android's global focus
 * listener); the check compares every watcher with aether_ui_focused_widget()
 * and the parent chain, and fires those whose state changed.
 *
 * Focus leaving the APP (another application activated) is not a change:
 * each backend only calls the check for focus moving within it.
 */
#ifndef AETHER_UI_FOCUS_WATCH_H
#define AETHER_UI_FOCUS_WATCH_H

#include <stdint.h>
#include <stdlib.h>

typedef struct { int handle; void* closure; int inside; } AeuiFocusWatch;
/* The boxed closure's layout (each backend's AeClosure, Aether's _AeClosure),
   named here because this header is included before a backend defines it. */
typedef struct { void (*fn)(void); void* env; } AeuiFwClosure;

static AeuiFocusWatch* aeui_fw = NULL;
static int aeui_fw_n = 0, aeui_fw_cap = 0;

static int aeui_fw_within(int focused, int ancestor) {
    for (int h = focused, guard = 0; h > 0 && guard < 4096; h = aether_ui_widget_parent_impl(h), guard++)
        if (h == ancestor) return 1;
    return 0;
}

static void aeui_focus_watch_check(void) {
    if (aeui_fw_n == 0) return;
    int f = aether_ui_focused_widget();
    int n = aeui_fw_n;   /* a handler may add a watcher; it is checked next time */
    for (int i = 0; i < n; i++) {
        int now = (f > 0 && aeui_fw_within(f, aeui_fw[i].handle)) ? 1 : 0;
        if (now == aeui_fw[i].inside) continue;
        aeui_fw[i].inside = now;
        AeuiFwClosure* c = (AeuiFwClosure*)aeui_fw[i].closure;
        if (c && c->fn) ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)now);
    }
}

/* Record a watcher; 1 when it is the first (a backend arms its signal). */
static int aeui_focus_watch_add(int handle, void* closure) {
    if (aeui_fw_n == aeui_fw_cap) {
        int cap = aeui_fw_cap ? aeui_fw_cap * 2 : 16;
        AeuiFocusWatch* g = (AeuiFocusWatch*)realloc(aeui_fw, sizeof(AeuiFocusWatch) * (size_t)cap);
        if (!g) return 0;
        aeui_fw = g;
        aeui_fw_cap = cap;
    }
    int f = aether_ui_focused_widget();
    aeui_fw[aeui_fw_n].handle = handle;
    aeui_fw[aeui_fw_n].closure = closure;
    aeui_fw[aeui_fw_n].inside = (f > 0 && aeui_fw_within(f, handle)) ? 1 : 0;
    aeui_fw_n++;
    return aeui_fw_n == 1;
}

#endif
