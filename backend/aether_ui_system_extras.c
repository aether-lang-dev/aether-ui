// Aether UI — system tray + desktop-notification registry.
// See aether_ui_system_extras.h for the contract + design notes.

#include "aether_ui_system_extras.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

// Backend-supplied: read a reactive-state cell. Used by tray_current_icon
// to resolve which of (clean / busy / alert) the tray is on. Every backend
// already defines this — see aether_ui_state_get in the per-backend file.
extern double aether_ui_state_get(int handle);

// Closure layout: the box_closure() return contract is `{fn, env}`. We
// only need to invoke it, so a thin local mirror works for both 32- and
// 64-bit pointers.
typedef struct { void* fn; void* env; } AeClosureLocal;
static void invoke_closure(void* boxed) {
    if (!boxed) return;
    AeClosureLocal* c = (AeClosureLocal*)boxed;
    if (!c->fn) return;
    ((void(*)(void*))c->fn)(c->env);
}

// ---------------------------------------------------------------------------
// Tray registry
// ---------------------------------------------------------------------------

#define TRAY_MAX 64
#define NOTIF_MAX 256
#define MENU_ITEM_MAX 512

typedef struct {
    int  in_use;
    char name[64];
    char tooltip[256];
    int  menu_handle;
    int  state_handle;
    char icon_clean[256];
    char icon_busy[256];
    char icon_alert[256];
    int  is_template;
    int  sealed;
    void* left_click_boxed;
} TrayRec;

static TrayRec g_tray[TRAY_MAX];
static int     g_tray_count = 0;

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int aether_ui_tray_register(const char* name, void* boxed_left_click) {
    if (g_tray_count >= TRAY_MAX) return 0;
    int idx = g_tray_count++;
    TrayRec* t = &g_tray[idx];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    copy_str(t->name, sizeof(t->name), name ? name : "");
    t->left_click_boxed = boxed_left_click;
    return idx + 1; // 1-based
}

static TrayRec* tray_lookup(int tray_id) {
    if (tray_id < 1 || tray_id > g_tray_count) return NULL;
    TrayRec* t = &g_tray[tray_id - 1];
    return t->in_use ? t : NULL;
}

void aether_ui_tray_set_tooltip_reg(int tray_id, const char* text) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    copy_str(t->tooltip, sizeof(t->tooltip), text);
}

void aether_ui_tray_set_menu_reg(int tray_id, int menu_handle) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->menu_handle = menu_handle;
}

void aether_ui_tray_set_icon_for_state_reg(int tray_id, int state_handle,
                                            const char* icon_clean,
                                            const char* icon_busy,
                                            const char* icon_alert) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->state_handle = state_handle;
    copy_str(t->icon_clean, sizeof(t->icon_clean), icon_clean);
    copy_str(t->icon_busy,  sizeof(t->icon_busy),  icon_busy);
    copy_str(t->icon_alert, sizeof(t->icon_alert), icon_alert);
}

void aether_ui_tray_set_icon_template_reg(int tray_id, int is_template) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->is_template = is_template ? 1 : 0;
}

void aether_ui_tray_seal_reg(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return;
    t->sealed = 1;
}

int aether_ui_tray_count(void) { return g_tray_count; }

const char* aether_ui_tray_name(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->name : "";
}

const char* aether_ui_tray_tooltip(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->tooltip : "";
}

int aether_ui_tray_menu_handle(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->menu_handle : 0;
}

// Convention from the ask: 0=clean, 1=busy/syncing, 2=conflict/alert.
const char* aether_ui_tray_current_icon(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return "";
    if (t->state_handle <= 0) return t->icon_clean;
    int s = (int)aether_ui_state_get(t->state_handle);
    if (s <= 0) return t->icon_clean;
    if (s == 1) return t->icon_busy;
    return t->icon_alert;
}

int aether_ui_tray_is_template(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->is_template : 0;
}

int aether_ui_tray_is_sealed(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    return t ? t->sealed : 0;
}

int aether_ui_tray_emit_click(int tray_id) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return 3;
    if (t->sealed) return 1;
    if (!t->left_click_boxed) return 4;
    invoke_closure(t->left_click_boxed);
    return 0;
}

int aether_ui_tray_menu_activate(int tray_id, const char* item_label) {
    TrayRec* t = tray_lookup(tray_id);
    if (!t) return 3;
    if (t->sealed) return 1;
    if (t->menu_handle <= 0) return 3;
    return aether_ui_menu_item_invoke(t->menu_handle, item_label);
}

// ---------------------------------------------------------------------------
// Notification registry
// ---------------------------------------------------------------------------

typedef struct {
    int  in_use;
    char title[256];
    char body[1024];
    char icon[256];
    char tag[128];
    int  dismissed;
    void* click_boxed;
} NotifRec;

static NotifRec g_notif[NOTIF_MAX];
static int      g_notif_count = 0;

// Tag-replace: if a new notification matches an existing record's tag,
// reuse its slot (mirroring libnotify replaces_id / UN identifier / Toast
// tag+group semantics). Empty tag never replaces.
static int notif_slot_for_tag(const char* tag) {
    if (!tag || !*tag) return -1;
    for (int i = 0; i < g_notif_count; i++) {
        if (g_notif[i].in_use && strcmp(g_notif[i].tag, tag) == 0) return i;
    }
    return -1;
}

static int notif_alloc_slot(void) {
    if (g_notif_count >= NOTIF_MAX) return -1;
    return g_notif_count++;
}

int aether_ui_notify_register(const char* title, const char* body) {
    return aether_ui_notify_register_full(title, body, "", "", NULL);
}

int aether_ui_notify_register_full(const char* title, const char* body,
                                    const char* icon_path, const char* tag,
                                    void* boxed_click) {
    int slot = notif_slot_for_tag(tag);
    if (slot < 0) {
        slot = notif_alloc_slot();
        if (slot < 0) return 0;
    }
    NotifRec* n = &g_notif[slot];
    memset(n, 0, sizeof(*n));
    n->in_use = 1;
    copy_str(n->title, sizeof(n->title), title);
    copy_str(n->body,  sizeof(n->body),  body);
    copy_str(n->icon,  sizeof(n->icon),  icon_path);
    copy_str(n->tag,   sizeof(n->tag),   tag);
    n->click_boxed = boxed_click;
    return slot + 1;
}

int aether_ui_notify_request_permission(void) {
    // Linux + Win: always granted. macOS would call
    // UNUserNotificationCenter requestAuthorizationWithOptions here
    // when the real backend lands.
    return 1;
}

int aether_ui_notif_count(void) { return g_notif_count; }

static NotifRec* notif_lookup(int notif_id) {
    if (notif_id < 1 || notif_id > g_notif_count) return NULL;
    NotifRec* n = &g_notif[notif_id - 1];
    return n->in_use ? n : NULL;
}

const char* aether_ui_notif_title(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->title : "";
}
const char* aether_ui_notif_body(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->body : "";
}
const char* aether_ui_notif_icon(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->icon : "";
}
const char* aether_ui_notif_tag(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->tag : "";
}
int aether_ui_notif_dismissed(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    return n ? n->dismissed : 0;
}

int aether_ui_notif_emit_click(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    if (!n) return 3;
    if (!n->click_boxed) return 4;
    invoke_closure(n->click_boxed);
    return 0;
}

int aether_ui_notif_mark_dismissed(int notif_id) {
    NotifRec* n = notif_lookup(notif_id);
    if (!n) return 3;
    n->dismissed = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Menu-item side-store
// ---------------------------------------------------------------------------

typedef struct {
    int   in_use;
    int   menu_handle;
    char  label[128];
    void* boxed;
} MenuItemRec;

static MenuItemRec g_menu_items[MENU_ITEM_MAX];
static int         g_menu_item_count = 0;

void aether_ui_menu_item_record(int menu_handle, const char* label,
                                 void* boxed_closure) {
    if (g_menu_item_count >= MENU_ITEM_MAX) return;
    MenuItemRec* m = &g_menu_items[g_menu_item_count++];
    m->in_use = 1;
    m->menu_handle = menu_handle;
    copy_str(m->label, sizeof(m->label), label);
    m->boxed = boxed_closure;
}

int aether_ui_menu_item_invoke(int menu_handle, const char* label) {
    if (!label) return 3;
    for (int i = 0; i < g_menu_item_count; i++) {
        MenuItemRec* m = &g_menu_items[i];
        if (!m->in_use) continue;
        if (m->menu_handle != menu_handle) continue;
        if (strcmp(m->label, label) != 0) continue;
        if (!m->boxed) return 4;
        invoke_closure(m->boxed);
        return 0;
    }
    return 3;
}

int aether_ui_menu_item_count_for(int menu_handle) {
    int n = 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        if (g_menu_items[i].in_use && g_menu_items[i].menu_handle == menu_handle)
            n++;
    }
    return n;
}

const char* aether_ui_menu_item_label_at(int menu_handle, int index) {
    int seen = 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        MenuItemRec* m = &g_menu_items[i];
        if (!m->in_use || m->menu_handle != menu_handle) continue;
        if (seen == index) return m->label;
        seen++;
    }
    return "";
}

int aether_ui_menu_handles(int* out, int max) {
    int n = 0;
    for (int i = 0; i < g_menu_item_count; i++) {
        if (!g_menu_items[i].in_use) continue;
        int h = g_menu_items[i].menu_handle;
        int dup = 0;
        for (int j = 0; j < n; j++) { if (out[j] == h) { dup = 1; break; } }
        if (dup) continue;
        if (n >= max) break;
        out[n++] = h;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Cross-backend headless park fallback.
//
// `aether_ui_app_run_headless_impl` lives in each backend file because
// the GTK4 backend wants to run a real GMainLoop (so SNI/DBusMenu
// signals get delivered), while macOS/Win32 need their own equivalents
// when those backends gain native tray support. This sleep loop is the
// shared no-op the backends fall through to when they can't or don't
// want to pump a real loop.
void aether_ui_park_until_killed(void) {
#ifdef _WIN32
    for (;;) Sleep(60000);
#else
    for (;;) sleep(60);
#endif
}

// ---------------------------------------------------------------------------
// AeCS shared state (styles layer — docs/design/styling.md).
// Lives here because this TU links into every backend build: the "current
// sheet" (use_styles) that widget constructors consult, and the
// appearance-change registry behind styles_for_mode auto re-theming.
// ---------------------------------------------------------------------------

// Opaque Aether-heap sheet ptr; the DSL owns it, C only holds it.
static void* aecs_current_sheet = NULL;
void aether_ui_current_sheet_set_impl(void* sheet) { aecs_current_sheet = sheet; }
void* aether_ui_current_sheet_get_impl(void) { return aecs_current_sheet; }

// Single-slot appearance callback (|dark: int| closure) + the driver's
// override (-1 = follow the OS; 0/1 forced via POST /appearance).
static void* appearance_boxed = NULL;
static int appearance_override = -1;
void aether_ui_appearance_register_impl(void* boxed) { appearance_boxed = boxed; }
int aether_ui_appearance_override_get(void) { return appearance_override; }
void aether_ui_appearance_override_set(int dark) { appearance_override = dark; }

// Invoke the registered callback with the new mode. Backends call this from
// their OS event ON THE UI THREAD; the driver's fire goes through the
// per-backend aether_ui_fire_appearance, which owns thread marshalling.
int aether_ui_appearance_invoke(int dark) {
    AeClosureLocal* c = (AeClosureLocal*)appearance_boxed;
    if (!c || !c->fn) return 0;
    ((void (*)(void*, long long))c->fn)(c->env, (long long)dark);
    return 1;
}

// ---------------------------------------------------------------------------
// Undo/redo edit stack (the Swing UndoManager shape; docs: roadmap backlog).
// Edits [0, cursor) are undoable, [cursor, len) redoable. Pushing a new edit
// truncates the redo tail (the classic rule). Closures are zero-arg.
// ---------------------------------------------------------------------------
#define AEUI_UNDO_CAP 128
typedef struct { char* label; void* undo_boxed; void* redo_boxed; } AeUndoEdit;
static AeUndoEdit undo_stack[AEUI_UNDO_CAP];
static int undo_len = 0, undo_cursor = 0;

void aether_ui_undo_push_impl(const char* label, void* undo_boxed, void* redo_boxed) {
    for (int i = undo_cursor; i < undo_len; i++) free(undo_stack[i].label);
    undo_len = undo_cursor;
    if (undo_len >= AEUI_UNDO_CAP) {          // drop the oldest edit
        free(undo_stack[0].label);
        memmove(undo_stack, undo_stack + 1, sizeof(AeUndoEdit) * (AEUI_UNDO_CAP - 1));
        undo_len--; undo_cursor--;
    }
    undo_stack[undo_len].label = strdup(label ? label : "");
    undo_stack[undo_len].undo_boxed = undo_boxed;
    undo_stack[undo_len].redo_boxed = redo_boxed;
    undo_len++; undo_cursor = undo_len;
}

// Direct steps — call ON THE UI THREAD (app-side undo()/redo(), or the
// per-backend marshalled aether_ui_fire_undo/redo for driver routes).
int aether_ui_undo_step_impl(void) {
    if (undo_cursor <= 0) return 0;
    undo_cursor--;
    invoke_closure(undo_stack[undo_cursor].undo_boxed);
    return 1;
}
int aether_ui_redo_step_impl(void) {
    if (undo_cursor >= undo_len) return 0;
    invoke_closure(undo_stack[undo_cursor].redo_boxed);
    undo_cursor++;
    return 1;
}
int aether_ui_undo_depth_impl(void) { return undo_cursor; }
int aether_ui_redo_depth_impl(void) { return undo_len - undo_cursor; }
const char* aether_ui_undo_label_impl(void) {
    return undo_cursor > 0 ? undo_stack[undo_cursor - 1].label : "";
}

// ─── vg outline-font holder ──────────────────────────────────────────────
// Aether has no module-level mutable globals, so vg/module.ae's text-path
// verbs keep the current outline font (a vg.font TtfFont*) here. Process-
// wide by design: one font serves every scene, use_font() swaps it.
static void* g_aeui_vgfont = 0;
void* aether_ui_vgfont_get(void) { return g_aeui_vgfont; }
void aether_ui_vgfont_set(void* f) { g_aeui_vgfont = f; }
