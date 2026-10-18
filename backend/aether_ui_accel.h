/* aether_ui_accel.h -- a menu item's accelerator (menu_item_accel): the
 * shared registry, by canonical key, of each accelerator a menu carries,
 * and the closure its item runs. Each backend binds the accelerator
 * natively for a person's key (AppKit's key equivalent, GTK's
 * set_accels_for_action, Win32's accelerator table, Android's MenuItem
 * shortcut; UIKit, which has no menu bar, its key handler) and shows it the
 * platform's way; the driver's window key (AETHER_DRV_WIN_KEY) asks this
 * registry first, so a spec's key and a menu's item end in the same
 * closure, as a person's do. Primary is Command on macOS and iOS and Ctrl
 * elsewhere, never Super: a backend defines AEUI_PRIMARY_IS_CMD (1 or 0)
 * before including this, once, after AeClosure is defined. */
#ifndef AETHER_UI_ACCEL_H
#define AETHER_UI_ACCEL_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AEUI_PRIMARY_IS_CMD
#define AEUI_PRIMARY_IS_CMD 0
#endif

enum { AEUI_ACC_SHIFT = 1, AEUI_ACC_CTRL = 2, AEUI_ACC_ALT = 4, AEUI_ACC_CMD = 8 };

/* Parse "Primary+Shift+S", "Ctrl+Alt+Delete", "F5": the modifiers
 * (AEUI_ACC_*) and the key, lower case ("s", "delete", "f5"). */
static int aeui_accel_parse(const char* accel, char* key, size_t ks) {
    int mods = 0;
    key[0] = 0;
    if (!accel) return 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", accel);
    char* save = NULL;
    char* parts[8];
    int np = 0;
    for (char* t = strtok_r(buf, "+", &save); t && np < 8; t = strtok_r(NULL, "+", &save)) parts[np++] = t;
    /* "Ctrl++" (the plus key): a trailing empty part. */
    size_t al = strlen(accel);
    int plus_key = al >= 2 && accel[al - 1] == '+' && accel[al - 2] == '+';
    for (int i = 0; i < np; i++) {
        char low[32];
        size_t k = 0;
        for (const char* c = parts[i]; *c && k + 1 < sizeof(low); c++)
            if (!isspace((unsigned char)*c)) low[k++] = (char)tolower((unsigned char)*c);
        low[k] = 0;
        int last = i == np - 1 && !plus_key;
        if (!last) {
            if (!strcmp(low, "shift")) mods |= AEUI_ACC_SHIFT;
            else if (!strcmp(low, "ctrl") || !strcmp(low, "control")) mods |= AEUI_ACC_CTRL;
            else if (!strcmp(low, "alt") || !strcmp(low, "option") || !strcmp(low, "opt")) mods |= AEUI_ACC_ALT;
            else if (!strcmp(low, "cmd") || !strcmp(low, "command") || !strcmp(low, "meta")) mods |= AEUI_ACC_CMD;
            else if (!strcmp(low, "primary") || !strcmp(low, "mod")) mods |= AEUI_PRIMARY_IS_CMD ? AEUI_ACC_CMD : AEUI_ACC_CTRL;
            continue;
        }
        if (!strcmp(low, "esc")) snprintf(low, sizeof(low), "escape");
        else if (!strcmp(low, "del")) snprintf(low, sizeof(low), "delete");
        else if (!strcmp(low, "enter")) snprintf(low, sizeof(low), "return");
        else if (!strcmp(low, "pgup") || !strcmp(low, "page_up")) snprintf(low, sizeof(low), "pageup");
        else if (!strcmp(low, "pgdn") || !strcmp(low, "page_down")) snprintf(low, sizeof(low), "pagedown");
        snprintf(key, ks, "%s", low);
    }
    if (plus_key) snprintf(key, ks, "+");
    return mods;
}

/* The canonical form two spellings of one key share: "10:s". */
static void aeui_accel_canon(const char* accel, char* out, size_t n) {
    char key[32];
    int mods = aeui_accel_parse(accel, key, sizeof(key));
    snprintf(out, n, "%d:%s", mods, key);
}

/* How the accelerator reads in a label where the platform draws none:
 * "Ctrl+Shift+S", or on macOS and iOS their symbols, "⇧⌘S". */
static void aeui_accel_text(const char* accel, char* out, size_t n) {
    char key[32];
    int mods = aeui_accel_parse(accel, key, sizeof(key));
    char k[32];
    snprintf(k, sizeof(k), "%s", key);
    if (strlen(k) == 1) k[0] = (char)toupper((unsigned char)k[0]);
    else if (k[0]) k[0] = (char)toupper((unsigned char)k[0]);
#if AEUI_PRIMARY_IS_CMD
    snprintf(out, n, "%s%s%s%s%s", (mods & AEUI_ACC_CTRL) ? "\xe2\x8c\x83" : "", (mods & AEUI_ACC_ALT) ? "\xe2\x8c\xa5" : "",
             (mods & AEUI_ACC_SHIFT) ? "\xe2\x87\xa7" : "", (mods & AEUI_ACC_CMD) ? "\xe2\x8c\x98" : "", k);
#else
    snprintf(out, n, "%s%s%s%s%s", (mods & AEUI_ACC_CTRL) ? "Ctrl+" : "", (mods & AEUI_ACC_ALT) ? "Alt+" : "",
             (mods & AEUI_ACC_SHIFT) ? "Shift+" : "", (mods & AEUI_ACC_CMD) ? "Meta+" : "", k);
#endif
}

typedef struct { int menu; char canon[48]; char accel[48]; char label[128]; AeClosure* cb; } AeuiAccel;
static AeuiAccel* aeui_accels = NULL;
static int aeui_accel_count = 0, aeui_accel_cap = 0;

/* Note an item's accelerator (the backend calls it from
 * aether_ui_menu_add_item_accel, its native binding made). */
static void aeui_accel_note(int menu, const char* label, const char* accel, void* closure) {
    if (!accel || !*accel) return;
    if (aeui_accel_count >= aeui_accel_cap) {
        int nc = aeui_accel_cap ? aeui_accel_cap * 2 : 32;
        AeuiAccel* n = (AeuiAccel*)realloc(aeui_accels, sizeof(AeuiAccel) * (size_t)nc);
        if (!n) return;
        aeui_accels = n; aeui_accel_cap = nc;
    }
    AeuiAccel* a = &aeui_accels[aeui_accel_count++];
    a->menu = menu;
    aeui_accel_canon(accel, a->canon, sizeof(a->canon));
    snprintf(a->accel, sizeof(a->accel), "%s", accel);
    snprintf(a->label, sizeof(a->label), "%s", label ? label : "");
    a->cb = (AeClosure*)closure;
}

/* menu_clear: its accelerators go too. */
static void aeui_accel_forget(int menu) {
    int k = 0;
    for (int i = 0; i < aeui_accel_count; i++)
        if (aeui_accels[i].menu != menu) aeui_accels[k++] = aeui_accels[i];
    aeui_accel_count = k;
}

/* The driver's key: the item whose accelerator it is runs. 1 if one did. */
int aether_ui_accel_fire(const char* combo) {
    char c[48];
    aeui_accel_canon(combo, c, sizeof(c));
    for (int i = aeui_accel_count - 1; i >= 0; i--) {
        if (strcmp(aeui_accels[i].canon, c) != 0) continue;
        AeClosure* cb = aeui_accels[i].cb;
        if (cb && cb->fn) { ((void(*)(void*))cb->fn)(cb->env); return 1; }
    }
    return 0;
}

/* GET /menus: the accelerator of a menu's item labelled `label`, or "". */
const char* aether_ui_menu_item_accel_of(int menu, const char* label) {
    for (int i = 0; i < aeui_accel_count; i++)
        if (aeui_accels[i].menu == menu && strcmp(aeui_accels[i].label, label) == 0) return aeui_accels[i].accel;
    return "";
}

#endif
