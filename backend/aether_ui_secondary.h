/* aether_ui_secondary.h -- a widget's secondary click (on_secondary_click):
 * a right click under a mouse, a long press on touch, the menu key. The
 * closure is kept here, by handle, for every backend; a backend detects
 * the gesture natively and calls aeui_secondary_fire, or the driver does
 * (AETHER_DRV_SECONDARY). Include it once per backend, after AeClosure is
 * defined, and define aeui_secondary_attach (the native hook for one
 * widget, made the first time it has a closure).
 *
 * A secondary click usually pops a menu (menu_popup). A menu the driver's
 * click pops is recorded, not shown -- AppKit's and Win32's popups track in
 * a modal loop of their own, which would hold the driver's reply -- and the
 * driver lists and picks it through /menus and GET /popup. */
#ifndef AETHER_UI_SECONDARY_H
#define AETHER_UI_SECONDARY_H

#include <stdlib.h>

typedef struct { int handle; AeClosure* cb; } AeuiSecondary;
static AeuiSecondary* aeui_secs = NULL;
static int aeui_sec_count = 0, aeui_sec_cap = 0;

/* Set while a driver-made secondary click runs: menu_popup records. */
int aether_ui_secondary_from_driver = 0;
/* The last menu popped, and the widget it was popped for (GET /popup). */
int aether_ui_last_popup_menu = 0, aether_ui_last_popup_anchor = 0;

static void aeui_secondary_attach(int handle);   /* the backend's */

static AeClosure* aeui_secondary_of(int handle) {
    for (int i = 0; i < aeui_sec_count; i++)
        if (aeui_secs[i].handle == handle) return aeui_secs[i].cb;
    return NULL;
}

void aether_ui_on_secondary_click_impl(int handle, void* boxed_closure) {
    if (handle <= 0 || !boxed_closure) return;
    for (int i = 0; i < aeui_sec_count; i++)
        if (aeui_secs[i].handle == handle) { aeui_secs[i].cb = (AeClosure*)boxed_closure; return; }
    if (aeui_sec_count >= aeui_sec_cap) {
        int nc = aeui_sec_cap ? aeui_sec_cap * 2 : 16;
        AeuiSecondary* n = (AeuiSecondary*)realloc(aeui_secs, sizeof(AeuiSecondary) * (size_t)nc);
        if (!n) return;
        aeui_secs = n; aeui_sec_cap = nc;
    }
    aeui_secs[aeui_sec_count].handle = handle;
    aeui_secs[aeui_sec_count].cb = (AeClosure*)boxed_closure;
    aeui_sec_count++;
    aeui_secondary_attach(handle);
}

/* Fire a widget's secondary click at (x, y), widget-local px. 1 if it has one. */
static int aeui_secondary_fire(int handle, double x, double y) {
    AeClosure* c = aeui_secondary_of(handle);
    if (!c || !c->fn) return 0;
    ((void(*)(void*, double, double))c->fn)(c->env, x, y);
    return 1;
}

/* The driver's secondary click: the closure with menu_popup recording. */
int aether_ui_secondary_drive(int handle, double x, double y) {
    aether_ui_last_popup_menu = 0;
    aether_ui_last_popup_anchor = 0;
    aether_ui_secondary_from_driver = 1;
    int r = aeui_secondary_fire(handle, x, y);
    aether_ui_secondary_from_driver = 0;
    return r;
}

/* Called first by each backend's menu_popup: note it; 1 = do not show it. */
static int aeui_popup_note(int menu, int anchor) {
    aether_ui_last_popup_menu = menu;
    aether_ui_last_popup_anchor = anchor;
    return aether_ui_secondary_from_driver;
}

#endif
