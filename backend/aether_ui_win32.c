// Aether UI — Win32 backend
//
// Native Windows implementation of the backend ABI declared in
// aether_ui_backend.h. Uses USER32 (windows + controls), GDI+ (custom paint),
// COMCTL32 (slider, progressbar, tooltip), COMDLG32 (file dialogs),
// DWMAPI (dark mode, accent color), SHELL32 (open URL), WS2_32 (test server).
//
// Toolchain: MinGW-w64 (gcc). No MSVC-specific extensions — only POSIX-ish C
// plus Win32 headers. Wide-char APIs are used throughout; UTF-8 conversion
// happens at the API boundary.
//
// This file is paired with aether_ui_backend.h. The Aether DSL layer
// (module.ae) declares matching externs.

#include "aether_ui_backend.h"
#include "aether_ui_system_extras.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Windows 10 1809+ for immersive dark mode, per-monitor DPI v2.
// WINVER + _WIN32_WINNT + NTDDI_VERSION must all be set for MinGW's headers
// to expose GetDpiForSystem / AdjustWindowRectExForDpi / DwmSetWindowAttribute.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000007  /* NTDDI_WIN10_RS3 — covers per-monitor-v2 APIs */
#endif

// Winsock must be included before windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <initguid.h>   // materialise CLSID_AccPropServices / IID_IAccPropServices
#include <oleacc.h>     // MSAA Dynamic Annotation (accessible name/role override)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

// Library linkage is declared in build.sh (MinGW does not honor #pragma comment).
// Required libs: user32, gdi32, comctl32, comdlg32, shell32, dwmapi, uxtheme,
// ole32, uuid, ws2_32.

// ---------------------------------------------------------------------------
// Closure struct — must match Aether codegen's _AeClosure layout.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

static inline void invoke_closure(AeClosure* c) {
    if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
}

// Text-entry closures take the CURRENT TEXT (|s: string|) — GTK4 passes it
// (on_entry_changed → c->fn(env, text)), so win32 must too or the callback
// reads an uninitialised argument. Used by every EN_CHANGE path and by the
// driver's set_text.
static inline void invoke_closure_str(AeClosure* c, const char* s) {
    if (c && c->fn) ((void (*)(void*, const char*))c->fn)(c->env, s ? s : "");
}

// Tabs strip clicks are resolved in WM_COMMAND, which is defined long before
// the tabs implementation; forward-declare the two hooks it needs.
typedef struct TabsState TabsState;
static TabsState* tabs_state_for_button(int btn_handle, int* out_index);
static void tabs_do_select(TabsState* ts, int index, int fire);

// ---------------------------------------------------------------------------
// AETHER_UI_HEADLESS contract — set by CI, widget smoke tests, or any
// caller that wants to exercise the backend without a user. Every API
// that would otherwise run a modal message loop (MessageBox,
// TrackPopupMenu, GetOpenFileName, GetSaveFileName) returns without
// showing UI when this flag is set. Without this, those APIs block the
// calling thread forever — there is no user input on CI and no outer
// message pump to dismiss.
// ---------------------------------------------------------------------------
static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

// ---------------------------------------------------------------------------
// UTF-8 ↔ UTF-16 helpers.
//
// Windows uses UTF-16 internally (`wchar_t`). Aether uses UTF-8 (`char*`).
// Converted strings live in static rotating buffers to avoid caller cleanup.
// Callers may hold the result across one call, not indefinitely.
// ---------------------------------------------------------------------------
#define UTF_BUFS 8
#define UTF_BUF_SIZE 4096

static wchar_t* utf8_to_wide(const char* s) {
    static wchar_t bufs[UTF_BUFS][UTF_BUF_SIZE];
    static int idx = 0;
    if (!s) s = "";
    wchar_t* buf = bufs[idx];
    idx = (idx + 1) % UTF_BUFS;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, UTF_BUF_SIZE);
    if (n <= 0) buf[0] = L'\0';
    return buf;
}

static char* wide_to_utf8(const wchar_t* s) {
    static char bufs[UTF_BUFS][UTF_BUF_SIZE];
    static int idx = 0;
    if (!s) s = L"";
    char* buf = bufs[idx];
    idx = (idx + 1) % UTF_BUFS;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, UTF_BUF_SIZE, NULL, NULL);
    if (n <= 0) buf[0] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// Widget kind enum + metadata.
//
// Every HWND we create is tagged with a kind + per-widget metadata (style
// overrides, attached closures, child-layout info for stacks). Stored in the
// global widgets[] array so the test-server and styling APIs can introspect.
// ---------------------------------------------------------------------------
struct W32CtxItem { char* label; void* closure; };

#define AEUI_SPLIT_DIV 6   // splitview divider band, px

typedef enum {
    WK_NULL = 0,
    WK_TEXT,
    WK_BUTTON,
    WK_TEXTFIELD,
    WK_SECUREFIELD,
    WK_TEXTAREA,
    WK_TOGGLE,
    WK_SLIDER,
    WK_PICKER,
    WK_PROGRESSBAR,
    WK_IMAGE,
    WK_VSTACK,
    WK_HSTACK,
    WK_ZSTACK,
    WK_FORM,
    WK_FORM_SECTION,
    WK_NAVSTACK,
    WK_SCROLLVIEW,
    WK_SPACER,
    WK_DIVIDER,
    WK_CANVAS,
    WK_WINDOW,
    WK_SHEET,
    WK_GRID,
    WK_SCRIM,     // overlay modal scrim (full-client click-eater)
    WK_TABS,      // native tab strip over a page stack
    WK_SPLITVIEW, // two panes + draggable divider (real since 2026-07-20)
    WK_WRAP,      // flow layout — children wrap to new rows (real 2026-07-20)
} WidgetKind;

typedef struct {
    COLORREF color;
    int has_value;
} ColorOverride;

typedef struct {
    // 0 = horizontal (HStack), 1 = vertical (VStack), 2 = ZStack
    int orientation;
    int spacing;
    int padding_top, padding_right, padding_bottom, padding_left;
    int alignment;    // 0=start, 1=center, 2=end
    int distribution; // 0=fill, 1=equal, 2=trailing
    int rtl;          // hstack: lay children right-to-left
} StackLayout;

typedef struct {
    WidgetKind kind;
    HWND hwnd;
    HWND parent_hwnd;

    // Appearance overrides (applied via subclass paint hooks)
    ColorOverride bg;
    ColorOverride fg;
    int gradient_enabled;
    COLORREF grad_a, grad_b;
    int grad_vertical;
    int corner_radius;
    int border_width;          // 0 = none
    int border_set;            // 1 = explicitly set (readback)
    COLORREF border_color;
    int owner_drawn;           // styled painter installed (see styled_btn_proc)
    int hover_set;             // interaction states (0=hover, 1=active)
    COLORREF hover_bg;
    int active_set;
    COLORREF active_bg;
    int is_hovered;            // tracked via WM_MOUSEMOVE/WM_MOUSELEAVE
    int is_pressed;            // driver /press-/release + real button msgs
    double opacity; // 0.0–1.0; <0 = no override

    // Fonts
    HFONT custom_font;
    double font_size;
    int font_bold;
    int font_weight_set;      // 1 = bold/normal was EXPLICITLY set (readback)
    wchar_t* font_family;     // face name for apply_font (owned; NULL default)
    char* font_family_u8;     // utf8 twin for driver readback (owned)

    // Fixed sizing (0 = auto)
    int pref_width;
    int pref_height;
    int min_width;
    int min_height;

    // Flutter-Expanded weight: >0 = share the parent stack's leftover main-axis
    // space proportionally (0 = natural size). Mirrors the GTK4 flex layout.
    int weight;

    // Margins (apply during parent layout)
    int margin_top, margin_right, margin_bottom, margin_left;

    // Stack container layout
    StackLayout stack;

    // Attached event closures
    AeClosure* on_click;
    AeClosure* on_hover;
    AeClosure* on_double_click;
    AeClosure* on_change; // text/value change for input widgets
    int bound_state;      // two-way bind_value target (0 = none)
    AeClosure* on_drop;   // row drag-reorder: on_drop(src_index)
    AeClosure* on_scroll; // vlist native scroll: on_scroll(dy rows)
    int text_wrap;        // WK_TEXT: multi-line wrapping label
    int text_anchor;      // WK_TEXT: 0=start 1=middle 2=end
    int text_truncate;    // WK_TEXT: EFFECTIVE ellipsis 0=none 2=middle 3=tail
    int image_fill;       // WK_IMAGE: EFFECTIVE fill 0=original 3=stretch
    int drag_armed;       // draggable(): the drag-source subclass is installed

    // Per-widget data (union over kind)
    union {
        struct { int timer_id; } button;
        struct { int canvas_id; } canvas;
        struct { double min_v, max_v, cur_v; } slider;
        struct { double fraction; } progressbar;
    } u;

    // Tooltip text (owned)
    wchar_t* tooltip;

    // Sealed flag (test server)
    int sealed;

    // Dead flag: the HWND was DestroyWindow'd (e.g. clear_children on a
    // listbox rebuild). The registry never shrinks and Windows recycles HWND
    // values, so IsWindow(hwnd) alone can't tell a destroyed row from a live
    // one that reused its handle. This flag is the reliable signal the driver
    // uses to drop the widget from /widgets (parity with GTK4's weak-ref slot
    // nulling).
    int dead;

    // Accessibility side-store (semantics layer). role/name/desc are the
    // author's a11y intent; MSAA surfaces name/description via WM_GETOBJECT
    // (get_accName/get_accDescription) over the standard accessible object,
    // and the driver reads these back. All owned (utf8), NULL = unset.
    char* a11y_role;
    char* a11y_name;
    char* a11y_desc;
    int styled_opacity_enc;   // explicit opacity readback: 0=unset, else v*100+1
    /* Implicit opacity transition (ui.transition). tr_ms > 0 means the NEXT
       opacity change tweens instead of snapping; tr_ease_out picks the curve.
       win32 had NO easing code at all -- apply_css set the layered alpha
       instantly, so `transition(h,"opacity",1200,"ease_out")` was a no-op
       here while GTK4 honoured it via CSS and macOS via its own record. */
    int tr_ms;
    int tr_ease_out;      /* 1 = ease-out, 0 = linear (ignored when tr_spring) */
    int tr_spring;        /* 1 = damped spring: OVERSHOOTS, then settles */
    UINT_PTR tr_timer;
    DWORD tr_start;
    double tr_from, tr_to;
    // Splitview (WK_SPLITVIEW): requested divider px (+1 encoding, 0=unset →
    // half) and the EFFECTIVE position from the last layout.
    int split_pos_enc;
    int split_eff;
    // Context-menu side-store: labels + closures, driven by the real
    // WM_CONTEXTMENU popup AND the driver's /context_menu routes.
    struct W32CtxItem* ctx_items;
    int ctx_count, ctx_cap;
    // Toggle radio group id (0 = ungrouped).
    int radio_group;
    // on_layout (GeometryReader): |w,h| closure + last-fired size (fire on
    // CHANGE only, like GTK4).
    AeClosure* on_layout;
    int ol_last_w, ol_last_h;

    // CSS-class mirror (item 4/8 parity): win32 has no CSS, but the class
    // LIST is the driver's selection-visibility contract (.aui-row-selected)
    // — tracked here, emitted in widget JSON. Space-separated, owned.
    char* classes;
} Widget;

static Widget** widgets = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

// ---------------------------------------------------------------------------
// Multi-window registry (unified driver view: 1 = primary, 2.. = extras).
// The message loop stays alive while ≥1 window is live; WM_DESTROY quits only
// when the last one closes (was: quit on ANY window destroy).
// ---------------------------------------------------------------------------
typedef struct { HWND hwnd; char* title; int live; } W32Window;
static W32Window* w32_windows = NULL;
static int w32_window_count = 0, w32_window_capacity = 0;

static int w32_window_register(HWND hwnd, const char* title) {
    if (w32_window_count >= w32_window_capacity) {
        w32_window_capacity = w32_window_capacity == 0 ? 4 : w32_window_capacity * 2;
        w32_windows = realloc(w32_windows, sizeof(W32Window) * w32_window_capacity);
    }
    W32Window* e = &w32_windows[w32_window_count++];
    e->hwnd = hwnd;
    e->title = _strdup(title ? title : "");
    e->live = 1;
    return w32_window_count;  // 1-based
}
static int w32_live_window_count(void) {
    int n = 0;
    for (int i = 0; i < w32_window_count; i++) if (w32_windows[i].live) n++;
    return n;
}

// Open-addressed reverse map HWND → 1-based handle.
//
// Before this, handle_for_hwnd() was an O(n) linear scan called from every
// WM_COMMAND, every stack layout, and every /widgets JSON emit — so a
// 500-widget app paid ~500 pointer compares per button click. Replacing it
// with a flat probe table keeps lookup O(1) amortized with predictable
// memory (8 bytes per live widget + load-factor slack).
//
// Keys are HWND (64-bit on x64); we use a 32-bit multiplicative Fibonacci
// hash and linear probing. Never shrinks — widgets are never unregistered
// in the current lifecycle, so stale tombstones aren't needed.
typedef struct { HWND hwnd; int handle; } WidgetHashEntry;
static WidgetHashEntry* widget_hash = NULL;
static int widget_hash_mask = 0;  // capacity - 1; capacity is always power of 2
static int widget_hash_count = 0;

static uint32_t hash_hwnd(HWND h) {
    // Fibonacci hash: multiply by the golden ratio constant, keep high bits.
    uint64_t k = (uint64_t)(uintptr_t)h;
    return (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 32);
}

static void widget_hash_grow(int new_cap_pow2) {
    WidgetHashEntry* old = widget_hash;
    int old_cap = widget_hash_mask + 1;
    widget_hash = (WidgetHashEntry*)calloc((size_t)new_cap_pow2,
                                            sizeof(WidgetHashEntry));
    widget_hash_mask = new_cap_pow2 - 1;
    widget_hash_count = 0;
    if (old) {
        for (int i = 0; i < old_cap; i++) {
            if (old[i].hwnd) {
                uint32_t slot = hash_hwnd(old[i].hwnd) & widget_hash_mask;
                while (widget_hash[slot].hwnd) slot = (slot + 1) & widget_hash_mask;
                widget_hash[slot] = old[i];
                widget_hash_count++;
            }
        }
        free(old);
    }
}

static void widget_hash_insert(HWND h, int handle) {
    if (!h) return;
    // Keep load factor < 0.5 for good probe distance. The `!widget_hash`
    // guard handles the initial empty-state correctly — otherwise the
    // `count * 2 >= mask + 1` arithmetic evaluates 0 >= 1 (false) and
    // the slot write dereferences a NULL table.
    if (!widget_hash || widget_hash_count * 2 >= (widget_hash_mask + 1)) {
        int new_cap = widget_hash ? (widget_hash_mask + 1) * 2 : 64;
        widget_hash_grow(new_cap);
    }
    uint32_t slot = hash_hwnd(h) & widget_hash_mask;
    while (widget_hash[slot].hwnd && widget_hash[slot].hwnd != h)
        slot = (slot + 1) & widget_hash_mask;
    if (!widget_hash[slot].hwnd) widget_hash_count++;
    widget_hash[slot].hwnd = h;
    widget_hash[slot].handle = handle;
}

static void mark_subtree_dead(HWND hwnd);

static Widget* widget_at(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return widgets[handle - 1];
}

int aether_ui_register_widget(void* hwnd) {
    if (widget_count >= widget_capacity) {
        widget_capacity = widget_capacity == 0 ? 64 : widget_capacity * 2;
        widgets = (Widget**)realloc(widgets, sizeof(Widget*) * widget_capacity);
    }
    Widget* w = (Widget*)calloc(1, sizeof(Widget));
    w->hwnd = (HWND)hwnd;
    w->opacity = -1.0;
    w->font_size = 0.0;
    widgets[widget_count++] = w;
    widget_hash_insert((HWND)hwnd, widget_count);
    return widget_count; // 1-based
}

static int register_widget_typed(HWND hwnd, WidgetKind kind) {
    int h = aether_ui_register_widget(hwnd);
    if (h > 0) widgets[h - 1]->kind = kind;
    return h;
}

void* aether_ui_get_widget(int handle) {
    Widget* w = widget_at(handle);
    return w ? (void*)w->hwnd : NULL;
}

// O(1) average reverse lookup via the HWND hash. Falls back to linear scan
// for HWNDs that were never registered (the hash would miss anyway).
static int handle_for_hwnd(HWND h) {
    if (!h || !widget_hash) return 0;
    uint32_t slot = hash_hwnd(h) & widget_hash_mask;
    while (widget_hash[slot].hwnd) {
        if (widget_hash[slot].hwnd == h) return widget_hash[slot].handle;
        slot = (slot + 1) & widget_hash_mask;
    }
    return 0;
}

// Public wrapper matching the backend ABI.
int aether_ui_handle_for_widget(void* widget) {
    return handle_for_hwnd((HWND)widget);
}

// ---------------------------------------------------------------------------
// Reactive state — ported verbatim from the GTK4 backend (platform-neutral).
// ---------------------------------------------------------------------------
enum { AEUI_STATE_FLOAT = 0, AEUI_STATE_INT = 1,
       AEUI_STATE_BOOL = 2, AEUI_STATE_STRING = 3,
       AEUI_STATE_LIST = 4 };
typedef struct {
    int type;
    double num;   // float/int/bool payload
    char* str;    // string payload (owned)
    void* list;   // LIST payload (opaque std.list ptr, NOT owned)
    int rev;      // LIST: bumps on each set
} StateCell;

typedef struct {
    int state_handle;
    AeClosure* closure;
} StateObserver;
static StateObserver* state_observers = NULL;
static int state_observer_count = 0;
static int state_observer_capacity = 0;

enum { AEUI_BIND_TEXT = 0, AEUI_BIND_ENABLED = 1, AEUI_BIND_HIDDEN = 2,
       AEUI_BIND_VALUE = 3 };  // two-way: editable widget ⇄ string state
typedef struct {
    int kind;
    int state_handle;
    int widget_handle;
    char* prefix;   // TEXT only
    char* suffix;   // TEXT only
    int decimals;   // TEXT + float cell: -1 = smart (int-valued → "%d")
    int invert;     // ENABLED/HIDDEN: apply the negation
} PropBinding;

static StateCell* state_cells = NULL;
static int state_count = 0;
static int state_capacity = 0;

static PropBinding* prop_bindings = NULL;
static int prop_binding_count = 0;
static int prop_binding_capacity = 0;

static StateCell* state_cell(int handle) {
    if (handle < 1 || handle > state_count) return NULL;
    return &state_cells[handle - 1];
}

static int state_create_cell(int type, double num, const char* str) {
    if (state_count >= state_capacity) {
        state_capacity = state_capacity == 0 ? 32 : state_capacity * 2;
        state_cells = realloc(state_cells, sizeof(StateCell) * state_capacity);
    }
    StateCell* c = &state_cells[state_count];
    c->type = type;
    c->num = num;
    c->str = str ? strdup(str) : NULL;
    c->list = NULL;
    c->rev = 0;
    state_count++;
    return state_count; // 1-based
}

int aether_ui_state_create(double initial) {
    return state_create_cell(AEUI_STATE_FLOAT, initial, NULL);
}
int aether_ui_state_create_s(const char* initial) {
    return state_create_cell(AEUI_STATE_STRING, 0.0, initial ? initial : "");
}
int aether_ui_state_create_i(int initial) {
    return state_create_cell(AEUI_STATE_INT, (double)initial, NULL);
}
int aether_ui_state_create_b(int initial) {
    return state_create_cell(AEUI_STATE_BOOL, initial ? 1.0 : 0.0, NULL);
}

double aether_ui_state_get(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_FLOAT) ? c->num : 0.0;
}
// Returned string is malloc'd (Aether externs own their string returns).
const char* aether_ui_state_get_s(int handle) {
    StateCell* c = state_cell(handle);
    return strdup((c && c->type == AEUI_STATE_STRING && c->str) ? c->str : "");
}
int aether_ui_state_get_i(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_INT) ? (int)c->num : 0;
}
int aether_ui_state_get_b(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_BOOL) ? (c->num != 0.0) : 0;
}

int aether_ui_state_type(int handle) {
    StateCell* c = state_cell(handle);
    return c ? c->type : -1;
}

// Render a cell's display string (no prefix/suffix) into buf.
static void state_render_value(StateCell* c, int decimals, char* buf, int n) {
    if (!c) { buf[0] = '\0'; return; }
    switch (c->type) {
        case AEUI_STATE_STRING:
            snprintf(buf, n, "%s", c->str ? c->str : "");
            break;
        case AEUI_STATE_INT:
            snprintf(buf, n, "%d", (int)c->num);
            break;
        case AEUI_STATE_BOOL:
            snprintf(buf, n, "%s", c->num != 0.0 ? "true" : "false");
            break;
        default: // float
            if (decimals >= 0) {
                snprintf(buf, n, "%.*f", decimals, c->num);
            } else if (c->num == (int)c->num) {
                snprintf(buf, n, "%d", (int)c->num);
            } else {
                snprintf(buf, n, "%.2f", c->num);
            }
    }
}

static int state_truthy(StateCell* c) {
    if (!c) return 0;
    if (c->type == AEUI_STATE_STRING) return c->str && c->str[0];
    return c->num != 0.0;
}

static void apply_prop_binding(PropBinding* b) {
    StateCell* c = state_cell(b->state_handle);
    if (!c) return;
    if (b->kind == AEUI_BIND_VALUE) {
        // state → editable widget, compare-first to avoid the write-back echo.
        const char* cur = aether_ui_textfield_get_text(b->widget_handle);
        const char* want = (c->type == AEUI_STATE_STRING && c->str) ? c->str : "";
        if (!cur || strcmp(cur, want) != 0) {
            aether_ui_textfield_set_text(b->widget_handle, want);
        }
        return;
    }
    if (b->kind == AEUI_BIND_TEXT) {
        char val[256];
        state_render_value(c, b->decimals, val, sizeof(val));
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s%s", b->prefix, val, b->suffix);
        aether_ui_text_set_string(b->widget_handle, buf);
    } else {
        int on = state_truthy(c);
        if (b->invert) on = !on;
        if (b->kind == AEUI_BIND_ENABLED) {
            aether_ui_set_enabled(b->widget_handle, on);
        } else { // HIDDEN: state truthy (post-invert) = hidden
            aether_ui_widget_set_hidden(b->widget_handle, on);
        }
    }
}

static void fire_state_observers(int state_handle) {
    int n = state_observer_count;
    for (int i = 0; i < n; i++) {
        if (state_observers[i].state_handle == state_handle) {
            invoke_closure(state_observers[i].closure);
        }
    }
}

static void update_prop_bindings(int state_handle) {
    for (int i = 0; i < prop_binding_count; i++) {
        if (prop_bindings[i].state_handle == state_handle) {
            apply_prop_binding(&prop_bindings[i]);
        }
    }
    fire_state_observers(state_handle);
}

void aether_ui_state_on_change(int state_handle, void* boxed_closure) {
    if (state_observer_count >= state_observer_capacity) {
        state_observer_capacity = state_observer_capacity == 0 ? 16
                                : state_observer_capacity * 2;
        state_observers = realloc(state_observers,
                                  sizeof(StateObserver) * state_observer_capacity);
    }
    state_observers[state_observer_count].state_handle = state_handle;
    state_observers[state_observer_count].closure = (AeClosure*)boxed_closure;
    state_observer_count++;
}

int aether_ui_state_create_list(void* list_ptr) {
    int h = state_create_cell(AEUI_STATE_LIST, 0.0, NULL);
    StateCell* c = state_cell(h);
    if (c) { c->list = list_ptr; c->rev = 0; }
    return h;
}
void* aether_ui_state_get_list(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_LIST) ? c->list : NULL;
}
void aether_ui_state_set_list(int handle, void* list_ptr) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_LIST) return;
    c->list = list_ptr;
    c->rev++;
    update_prop_bindings(handle);
}
int aether_ui_state_list_rev(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_LIST) ? c->rev : 0;
}

void aether_ui_state_set(int handle, double value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_FLOAT) return;
    c->num = value;
    update_prop_bindings(handle);
}
void aether_ui_state_set_s(int handle, const char* value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_STRING) return;
    free(c->str);
    c->str = strdup(value ? value : "");
    update_prop_bindings(handle);
}
void aether_ui_state_set_i(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_INT) return;
    c->num = (double)value;
    update_prop_bindings(handle);
}
void aether_ui_state_set_b(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_BOOL) return;
    c->num = value ? 1.0 : 0.0;
    update_prop_bindings(handle);
}

static PropBinding* prop_binding_new(int kind, int state_handle, int widget_handle) {
    if (prop_binding_count >= prop_binding_capacity) {
        prop_binding_capacity = prop_binding_capacity == 0 ? 32 : prop_binding_capacity * 2;
        prop_bindings = realloc(prop_bindings,
                                sizeof(PropBinding) * prop_binding_capacity);
    }
    PropBinding* b = &prop_bindings[prop_binding_count++];
    b->kind = kind;
    b->state_handle = state_handle;
    b->widget_handle = widget_handle;
    b->prefix = strdup("");
    b->suffix = strdup("");
    b->decimals = -1;
    b->invert = 0;
    return b;
}

// The original float text binding — now literally a TEXT PropBinding.
void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix) {
    PropBinding* b = prop_binding_new(AEUI_BIND_TEXT, state_handle, text_handle);
    free(b->prefix);
    free(b->suffix);
    b->prefix = prefix ? strdup(prefix) : strdup("");
    b->suffix = suffix ? strdup(suffix) : strdup("");
    apply_prop_binding(b);
}

void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals) {
    PropBinding* b = prop_binding_new(AEUI_BIND_TEXT, state_handle, widget_handle);
    b->decimals = decimals;
    apply_prop_binding(b);
}
void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = prop_binding_new(AEUI_BIND_ENABLED, state_handle, widget_handle);
    b->invert = invert;
    apply_prop_binding(b);
}
void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = prop_binding_new(AEUI_BIND_HIDDEN, state_handle, widget_handle);
    b->invert = invert;
    apply_prop_binding(b);
}
// Two-way: editable widget ⇄ string state. State→widget is a VALUE
// PropBinding; widget→state is the EN_CHANGE write-back keyed on bound_state.
void aether_ui_bind_value(int state_handle, int widget_handle) {
    PropBinding* b = prop_binding_new(AEUI_BIND_VALUE, state_handle, widget_handle);
    Widget* w = widget_at(widget_handle);
    if (w) w->bound_state = state_handle;
    apply_prop_binding(b);
}

// ---------------------------------------------------------------------------
// Stack container — custom window class.
//
// A container that lays out its children left-to-right (HStack), top-to-bottom
// (VStack), or over-each-other (ZStack). Mirrors the semantics of GtkBox and
// NSStackView. Handles WM_SIZE by querying each child's preferred size and
// laying them out with spacing + padding.
//
// Children that are themselves stacks are recursed. Spacers consume leftover
// flex space along the primary axis.
// ---------------------------------------------------------------------------
static const wchar_t* STACK_CLASS = L"AetherUIStack";

typedef struct {
    int measured_w;
    int measured_h;
    int is_spacer;
    int weight;          // >0 = flex-weighted child
    int min_primary;     // its minimum along the primary axis (for the clamp)
    int flex_size;       // resolved primary size for weighted children
    int margin_t, margin_r, margin_b, margin_l;
    int kind;            // WidgetKind — containers fill the cross axis
} MeasuredChild;

// Containers/greedy widgets fill the CROSS axis of their parent stack (a
// nested row spans its column's width, as on GTK) — only leaf widgets
// shrink to their measured size for alignment.
static void w32_note_layout(HWND hwnd, int w, int h);    // fwd (on_layout)

static int w32_subtree_greedy(Widget* w, int orientation);  // fwd

static int w32_fills_cross(int kind) {
    return kind == WK_VSTACK || kind == WK_HSTACK || kind == WK_ZSTACK
        || kind == WK_TABS || kind == WK_SPLITVIEW || kind == WK_SCROLLVIEW
        || kind == WK_CANVAS || kind == WK_DIVIDER || kind == WK_WRAP;
}

// Measure a single widget's intrinsic size. STATIC/BUTTON use a minimal
// heuristic (text extents + padding); custom widgets honor pref_width/height.
// Natural size of a stack container = recursive sum/max of its children
// (+ spacing + padding). THE h:0 fix: containers used to measure as their
// CURRENT rect, which is 0 until laid out — so nested stacks never grew,
// every descendant inherited zero heights, and driver geometry (and real
// rendering) was flat. Bottom-up natural sizing is how GTK/AppKit behave.
static void measure_widget(Widget* w, int* out_w, int* out_h);
static int  w32_measure_grid_natural(HWND grid_hwnd, int* out_w, int* out_h); /* fwd; grids live below */
static void measure_stack_natural(Widget* sw, int* out_w, int* out_h) {
    StackLayout* sl = &sw->stack;
    int total = 0, cross = 0, n = 0;
    for (HWND c = GetWindow(sw->hwnd, GW_CHILD); c;
         c = GetWindow(c, GW_HWNDNEXT)) {
        int ch = handle_for_hwnd(c);
        Widget* cw = widget_at(ch);
        if (!cw || cw->dead) continue;
        int mw = 0, mh = 0;
        if (cw->kind == WK_SPACER) { n++; continue; }   // spacers are flex-only
        measure_widget(cw, &mw, &mh);
        int along, across;
        if (sw->kind == WK_ZSTACK || sw->kind == WK_TABS
            || sw->kind == WK_SPLITVIEW) {
            // Overlay-ish containers: natural = max in BOTH axes.
            if (mw > total) total = mw;
            if (mh > cross) cross = mh;
            n++;
            continue;
        }
        if (sl->orientation == 1) { along = mh + cw->margin_top + cw->margin_bottom;
                                    across = mw + cw->margin_left + cw->margin_right; }
        else                      { along = mw + cw->margin_left + cw->margin_right;
                                    across = mh + cw->margin_top + cw->margin_bottom; }
        total += along;
        if (across > cross) cross = across;
        n++;
    }
    if (sw->kind != WK_ZSTACK && sw->kind != WK_TABS
        && sw->kind != WK_SPLITVIEW && n > 1)
        total += sl->spacing * (n - 1);
    int pad_w = sl->padding_left + sl->padding_right;
    int pad_h = sl->padding_top + sl->padding_bottom;
    if (sw->kind == WK_ZSTACK || sw->kind == WK_TABS
        || sw->kind == WK_SPLITVIEW) {
        *out_w = total + pad_w;
        *out_h = cross + pad_h;
    } else if (sl->orientation == 1) {   // vstack: total is height
        *out_w = cross + pad_w;
        *out_h = total + pad_h;
    } else {                              // hstack: total is width
        *out_w = total + pad_w;
        *out_h = cross + pad_h;
    }
}

static void measure_widget(Widget* w, int* out_w, int* out_h) {
    if (w->pref_width > 0 && w->pref_height > 0) {
        *out_w = w->pref_width;
        *out_h = w->pref_height;
        return;
    }
    // Containers: bottom-up natural size (see measure_stack_natural).
    if (w->kind == WK_GRID) {
        /* A grid had NO natural measure, so anywhere its size was derived
           bottom-up (a scrollview's document view: scrollbg's "doc has a
           height — got 0") it reported nothing. Natural = per-column max
           widths + per-row max heights + spacings, span-1 approximation. */
        if (w32_measure_grid_natural(w->hwnd, out_w, out_h)) {
            if (w->pref_width > 0)  *out_w = w->pref_width;
            if (w->pref_height > 0) *out_h = w->pref_height;
            return;
        }
    }
    if (w->kind == WK_VSTACK || w->kind == WK_HSTACK || w->kind == WK_ZSTACK
        || w->kind == WK_TABS || w->kind == WK_SPLITVIEW) {
        measure_stack_natural(w, out_w, out_h);
        if (w->pref_width > 0) *out_w = w->pref_width;
        if (w->pref_height > 0) *out_h = w->pref_height;
        return;
    }
    // Input widgets start 0x0 on the hidden holder — give them the natural
    // sizes a dialog would (heights match DEFAULT_GUI_FONT rows).
    if (w->kind == WK_TEXTFIELD || w->kind == WK_SECUREFIELD
        || w->kind == WK_PICKER) {
        *out_w = w->pref_width > 0 ? w->pref_width : 140;
        *out_h = w->pref_height > 0 ? w->pref_height : 26;
        return;
    }
    if (w->kind == WK_SLIDER) {
        *out_w = w->pref_width > 0 ? w->pref_width : 140;
        *out_h = w->pref_height > 0 ? w->pref_height : 26;
        return;
    }
    if (w->kind == WK_PROGRESSBAR) {
        *out_w = w->pref_width > 0 ? w->pref_width : 140;
        *out_h = w->pref_height > 0 ? w->pref_height : 16;
        return;
    }
    if (w->kind == WK_TOGGLE) {
        // Checkbox box + label text.
        HDC hdc = GetDC(w->hwnd);
        wchar_t text[512];
        int tlen = GetWindowTextW(w->hwnd, text, 512);
        SIZE sz = {0, 16};
        HFONT font = (HFONT)SendMessageW(w->hwnd, WM_GETFONT, 0, 0);
        HFONT old = font ? (HFONT)SelectObject(hdc, font) : NULL;
        GetTextExtentPoint32W(hdc, text, tlen, &sz);
        if (old) SelectObject(hdc, old);
        ReleaseDC(w->hwnd, hdc);
        *out_w = sz.cx + 28;
        *out_h = (sz.cy > 18 ? sz.cy : 18) + 4;
        return;
    }
    if (w->kind == WK_DIVIDER) {
        *out_w = w->pref_width > 0 ? w->pref_width : 2;
        *out_h = w->pref_height > 0 ? w->pref_height : 2;
        return;
    }
    if (w->kind == WK_TEXTAREA) {
        *out_w = w->pref_width > 0 ? w->pref_width : 200;
        *out_h = w->pref_height > 0 ? w->pref_height : 80;
        return;
    }
    RECT r;
    if (GetWindowRect(w->hwnd, &r)) {
        int cur_w = r.right - r.left;
        int cur_h = r.bottom - r.top;
        // Ask the control for its natural size via GetTextExtentPoint where it
        // makes sense, else fall back to current size.
        if (w->kind == WK_TEXT || w->kind == WK_BUTTON) {
            HDC hdc = GetDC(w->hwnd);
            HFONT font = w->custom_font ? w->custom_font
                         : (HFONT)SendMessageW(w->hwnd, WM_GETFONT, 0, 0);
            HFONT old = font ? (HFONT)SelectObject(hdc, font) : NULL;
            wchar_t text[1024];
            int tlen = GetWindowTextW(w->hwnd, text, 1024);
            SIZE sz;
            GetTextExtentPoint32W(hdc, text, tlen, &sz);
            if (old) SelectObject(hdc, old);
            ReleaseDC(w->hwnd, hdc);
            int pad_x = w->kind == WK_BUTTON ? 24 : 4;
            int pad_y = w->kind == WK_BUTTON ? 10 : 4;
            *out_w = sz.cx + pad_x;
            *out_h = sz.cy + pad_y;
            if (w->pref_width > 0) *out_w = w->pref_width;
            if (w->pref_height > 0) *out_h = w->pref_height;
            return;
        }
        *out_w = w->pref_width > 0 ? w->pref_width : cur_w;
        *out_h = w->pref_height > 0 ? w->pref_height : cur_h;
        return;
    }
    *out_w = w->pref_width > 0 ? w->pref_width : 100;
    *out_h = w->pref_height > 0 ? w->pref_height : 24;
}

// A widget is "greedy" along `orientation` when it should take the stack's
// leftover primary-axis space by default: splitviews/scrollviews, canvases
// whose primary size the app did NOT pin, and containers holding any greedy
// descendant (expand propagates up).
static int w32_subtree_greedy(Widget* w, int orientation) {
    if (!w) return 0;
    /* fill_width/fill_height record an explicit ask as pref == -1
       (match_parent_*). The greedy test only knew KINDS, so a plain
       vstack body under a pinned toolbar took none of the window's
       new pixels — barfill's "every new pixel to the body: got 0".
       An explicit fill beats any kind heuristic. Orientation: 1 =
       vstack (primary is height), else horizontal. */
    if (orientation == 1 && w->pref_height == -1) return 1;
    if (orientation != 1 && w->pref_width  == -1) return 1;
    if (w->kind == WK_SPLITVIEW || w->kind == WK_SCROLLVIEW) return 1;
    if (w->kind == WK_CANVAS) return 1;   // GTK4 canvases hexpand/vexpand
                                          // regardless of their initial dims
                                          // (gp creates its treemap WITH dims
                                          // and still grows on resize there)
    if (w->kind == WK_VSTACK || w->kind == WK_HSTACK || w->kind == WK_ZSTACK
        || w->kind == WK_TABS || w->kind == WK_WRAP) {
        for (HWND c = GetWindow(w->hwnd, GW_CHILD); c;
             c = GetWindow(c, GW_HWNDNEXT)) {
            Widget* cw = widget_at(handle_for_hwnd(c));
            if (cw && !cw->dead && w32_subtree_greedy(cw, orientation))
                return 1;
        }
    }
    return 0;
}

// Layout all direct child windows of the stack.
static void stack_do_layout(HWND stack_hwnd) {
    int h = handle_for_hwnd(stack_hwnd);
    if (h == 0) return;
    Widget* sw = widget_at(h);
    if (!sw) return;
    StackLayout* sl = &sw->stack;
    int orientation = sl->orientation;

    RECT client;
    GetClientRect(stack_hwnd, &client);
    int avail_w = (client.right - client.left) - sl->padding_left - sl->padding_right;
    int avail_h = (client.bottom - client.top) - sl->padding_top - sl->padding_bottom;

    // Collect children in z-order.
    HWND* children = NULL;
    int nchildren = 0, cap = 0;
    for (HWND c = GetWindow(stack_hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (nchildren >= cap) {
            cap = cap == 0 ? 16 : cap * 2;
            children = (HWND*)realloc(children, sizeof(HWND) * cap);
        }
        children[nchildren++] = c;
    }
    if (nchildren == 0) { free(children); return; }

    // Tabs: child[0] is the header strip (hstack of tab buttons, measured
    // height at the top), child[1] is the content zstack (the pages) filling
    // all remaining space. Any extra children (shouldn't happen) stack under.
    if (sw->kind == WK_TABS) {
        int header_h = 0;
        {
            int hh = handle_for_hwnd(children[0]);
            Widget* hw = widget_at(hh);
            int mw = 0;
            if (hw) measure_widget(hw, &mw, &header_h);
            if (header_h <= 0) header_h = 30;
        }
        SetWindowPos(children[0], NULL,
                     sl->padding_left, sl->padding_top,
                     avail_w, header_h, SWP_NOZORDER | SWP_NOACTIVATE);
        int content_y = sl->padding_top + header_h + sl->spacing;
        int content_h = (client.bottom - client.top) - content_y - sl->padding_bottom;
        if (content_h < 0) content_h = 0;
        for (int i = 1; i < nchildren; i++) {
            SetWindowPos(children[i], NULL,
                         sl->padding_left, content_y,
                         avail_w, content_h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        free(children);
        return;
    }

    // Wrap (flow layout): place children left-to-right at natural size,
    // advance to a new row when the next child would overflow the width.
    if (sw->kind == WK_WRAP) {
        int cx = sl->padding_left, cy = sl->padding_top, row_h = 0;
        for (int i = 0; i < nchildren; i++) {
            int ch = handle_for_hwnd(children[i]);
            Widget* cw = widget_at(ch);
            int mw = 100, mh = 24;
            if (cw) measure_widget(cw, &mw, &mh);
            if (cx > sl->padding_left && cx + mw > sl->padding_left + avail_w) {
                cx = sl->padding_left;
                cy += row_h + sl->spacing;
                row_h = 0;
            }
            SetWindowPos(children[i], NULL, cx, cy, mw, mh,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            w32_note_layout(children[i], mw, mh);
            cx += mw + sl->spacing;
            if (mh > row_h) row_h = mh;
        }
        free(children);
        return;
    }

    // Splitview: pane A gets [0, pos), a 6px divider band, pane B the rest.
    // Extra children (shouldn't happen) overlay pane B. The divider position
    // clamps so both panes keep >= 24px.
    if (sw->kind == WK_SPLITVIEW) {
        int primary = (orientation == 1) ? avail_h : avail_w;
        int pos = sw->split_pos_enc > 0 ? sw->split_pos_enc - 1 : primary / 2;
        int min_pane = 24;
        if (pos < min_pane) pos = min_pane;
        if (pos > primary - AEUI_SPLIT_DIV - min_pane)
            pos = primary - AEUI_SPLIT_DIV - min_pane;
        if (pos < 0) pos = 0;
        sw->split_eff = pos;
        for (int i = 0; i < nchildren; i++) {
            int x, y, cw, chh;
            if (orientation == 1) {   // stacked (divider moves in y)
                x = sl->padding_left; cw = avail_w;
                if (i == 0) { y = sl->padding_top; chh = pos; }
                else { y = sl->padding_top + pos + AEUI_SPLIT_DIV;
                       chh = avail_h - pos - AEUI_SPLIT_DIV; }
            } else {                   // side-by-side (divider moves in x)
                y = sl->padding_top; chh = avail_h;
                if (i == 0) { x = sl->padding_left; cw = pos; }
                else { x = sl->padding_left + pos + AEUI_SPLIT_DIV;
                       cw = avail_w - pos - AEUI_SPLIT_DIV; }
            }
            if (cw < 0) cw = 0;
            if (chh < 0) chh = 0;
            SetWindowPos(children[i], NULL, x, y, cw, chh,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            w32_note_layout(children[i], cw, chh);
        }
        free(children);
        return;
    }

    // ZStack: overlay every child filling the client area.
    if (orientation == 2) {
        for (int i = 0; i < nchildren; i++) {
            SetWindowPos(children[i], NULL,
                         sl->padding_left, sl->padding_top,
                         avail_w, avail_h, SWP_NOZORDER | SWP_NOACTIVATE);
            w32_note_layout(children[i], avail_w, avail_h);
        }
        free(children);
        return;
    }

    // Measure + identify spacers and weighted (flex) children.
    MeasuredChild* mc = (MeasuredChild*)calloc(nchildren, sizeof(MeasuredChild));
    int total_primary = 0;
    int spacer_count = 0;
    int total_weight = 0;
    for (int i = 0; i < nchildren; i++) {
        int ch = handle_for_hwnd(children[i]);
        Widget* cw = widget_at(ch);
        if (cw && cw->kind == WK_SPACER) {
            mc[i].is_spacer = 1;
            spacer_count++;
            continue;
        }
        int cw_w = 0, ch_h = 0;
        if (cw) {
            measure_widget(cw, &cw_w, &ch_h);
            mc[i].margin_t = cw->margin_top;
            mc[i].margin_r = cw->margin_right;
            mc[i].margin_b = cw->margin_bottom;
            mc[i].margin_l = cw->margin_left;
            mc[i].weight = cw->weight;
            mc[i].kind = (int)cw->kind;
            // Greedy children expand along the primary axis by default —
            // splitview/scrolled (GTK's paned/scrolled semantics), unpinned
            // canvases (GTK's hexpand/vexpand=true), AND any container with
            // a greedy descendant: expand PROPAGATES UP, retroactively — the
            // same lesson the macOS backend learnt (a vstack holding the
            // treemap must win width in its hstack, or the canvas never
            // grows no matter how greedy it is).
            // Pin veto: an explicit primary-axis size (width()/height())
            // means the app CHOSE this child's size — auto-greed must not
            // override it (gp pins its sidebar at 280 and the greedy
            // scrollview inside was stealing half the canvas's pool).
            int pinned_primary = (orientation == 1) ? (cw->pref_height > 0)
                                                    : (cw->pref_width > 0);
            // Intrinsically-greedy kinds (canvas/splitview/scrollview) stay
            // greedy even with initial dims — GTK expands canvases created
            // WITH sizes. The veto only stops PROPAGATED greed (a pinned
            // sidebar containing a scrollview keeps its pinned width).
            /* A PIN MEANS "this is exactly my size" — stop expanding.
               This is GTK4's documented rule (aether_ui_set_width: "A
               pinned width normally means 'this is exactly my size' — so
               stop expanding to fill the parent... EXCEPT on a weighted
               child"), and win32 not implementing it is why svg_image's
               aspect-computed badge was stretched across its row.
               Measured: GTK4 lays golden_gallery's pinned canvases at
               240x160 where win32 gave them 275x187 — the win32 goldens
               were blessed against that divergence and are re-blessed
               with this commit.
               The weight carve-out matters for the same reason it does on
               GTK4: on a weighted child, width() is a MINIMUM (flex
               basis), so pinning it would make the weight unsatisfiable. */
            int weighted = (mc[i].weight > 0);
            int intrinsic = (cw->kind == WK_SPLITVIEW
                             || cw->kind == WK_SCROLLVIEW
                             || (cw->kind == WK_CANVAS
                                 && (!pinned_primary || weighted)));
            if (mc[i].weight == 0 && (intrinsic || !pinned_primary)
                && w32_subtree_greedy(cw, orientation))
                mc[i].weight = 1;
        } else {
            cw_w = 100; ch_h = 24;
        }
        mc[i].measured_w = cw_w;
        mc[i].measured_h = ch_h;
        if (mc[i].weight > 0) {
            // Weighted children take flex; reserve only their minimum (their
            // pref/measured size along the primary axis IS the min request).
            mc[i].min_primary = (orientation == 1) ? ch_h : cw_w;
            total_weight += mc[i].weight;
        } else {
            total_primary += (orientation == 1) ? (ch_h + mc[i].margin_t + mc[i].margin_b)
                                                : (cw_w + mc[i].margin_l + mc[i].margin_r);
        }
    }
    int spacing_total = sl->spacing * (nchildren - 1);
    int primary_avail = (orientation == 1) ? avail_h : avail_w;
    int flex = primary_avail - total_primary - spacing_total;
    if (getenv("AEUI_LAYOUT_DEBUG")) {
        fprintf(stderr, "[layout] stack=%d kind=%d orient=%d avail=%dx%d nch=%d flex=%d tw=%d\n",
                h, (int)sw->kind, orientation, avail_w, avail_h, nchildren, flex, total_weight);
        for (int i = 0; i < nchildren; i++)
            fprintf(stderr, "  child[%d] h=%d kind=%d w=%d meas=%dx%d min=%d\n",
                    i, handle_for_hwnd(children[i]), mc[i].kind, mc[i].weight,
                    mc[i].measured_w, mc[i].measured_h, mc[i].min_primary);
        fflush(stderr);
    }
    if (flex < 0) flex = 0;
    // Spacers only take flex when there are no weighted children (weighted
    // children are the explicit flex mechanism; spacers are the implicit one).
    int per_spacer = (spacer_count > 0 && total_weight == 0) ? (flex / spacer_count) : 0;

    // Weight distribution with min-clamp: a weighted child whose proportional
    // share would fall below its minimum is pinned to the min and removed from
    // the pool, then the rest re-split. Iterate to a fixed point (mirrors the
    // GTK4 AeuiFlexLayout). Pinned sizes + the remainder land in mc[].flex_size.
    if (total_weight > 0) {
        int clamp_flex = flex;
        int clamp_weight = total_weight;
        char* pinned = (char*)calloc(nchildren, 1);
        int changed = 1;
        while (changed && clamp_weight > 0) {
            changed = 0;
            for (int i = 0; i < nchildren; i++) {
                if (mc[i].weight <= 0 || pinned[i]) continue;
                int share = clamp_flex * mc[i].weight / clamp_weight;
                if (share < mc[i].min_primary) {
                    pinned[i] = 1;
                    mc[i].flex_size = mc[i].min_primary;
                    clamp_flex -= mc[i].min_primary;
                    if (clamp_flex < 0) clamp_flex = 0;
                    clamp_weight -= mc[i].weight;
                    changed = 1;
                }
            }
        }
        // Distribute the remaining flex among unpinned weighted children; the
        // last one absorbs the integer-division remainder.
        int seen = 0, used = 0;
        for (int i = 0; i < nchildren; i++) {
            if (mc[i].weight <= 0 || pinned[i]) continue;
            seen += mc[i].weight;
            mc[i].flex_size = (seen == clamp_weight)
                ? clamp_flex - used
                : clamp_flex * mc[i].weight / clamp_weight;
            used += mc[i].flex_size;
        }
        free(pinned);
    }

    // Lay out. An RTL hstack walks the SAME children[] but anchors from the
    // right edge and decrements — so children[0] lands rightmost, reversing the
    // on-screen order relative to LTR deterministically (independent of the
    // child-enumeration order).
    // RTL hstack: lay out LTR as usual, then mirror each child's x within the
    // client width — the source-first child (now enumerated first, since
    // add_child pushes to Z-bottom) lands rightmost.
    int rtl = (orientation == 0 && sl->rtl);
    /* Cross-axis pin veto: fills-cross kinds (canvas etc.) stretch across
       the stack UNLESS the app pinned that axis (svg_image's height in an
       hstack row — got 132 for a 60px badge). measure_widget already
       returns the pref when set, so honouring it here is just refusing
       the stretch. */
    #define W32_CROSS_PINNED(i, vstack_) ({ \
        Widget* _cw = widget_at(handle_for_hwnd(children[(i)])); \
        _cw ? ((vstack_) ? _cw->pref_width > 0 : _cw->pref_height > 0) : 0; })
    int client_w = client.right - client.left;
    int cur = (orientation == 1) ? sl->padding_top : sl->padding_left;
    for (int i = 0; i < nchildren; i++) {
        int x, y, w, h;
        if (orientation == 1) { // VStack
            int ch_size = mc[i].is_spacer ? per_spacer
                        : mc[i].weight > 0 ? mc[i].flex_size
                        : mc[i].measured_h;
            h = ch_size;
            w = avail_w - mc[i].margin_l - mc[i].margin_r;
            if (mc[i].measured_w > 0 && mc[i].measured_w < w
                && !mc[i].is_spacer && mc[i].weight == 0
                && (!w32_fills_cross(mc[i].kind) || W32_CROSS_PINNED(i, 1))) {
                if (sl->alignment == 1)
                    x = sl->padding_left + mc[i].margin_l + (w - mc[i].measured_w) / 2;
                else if (sl->alignment == 2)
                    x = sl->padding_left + mc[i].margin_l + (w - mc[i].measured_w);
                else
                    x = sl->padding_left + mc[i].margin_l;
                w = mc[i].measured_w;
            } else {
                x = sl->padding_left + mc[i].margin_l;
            }
            y = cur + mc[i].margin_t;
            cur = y + h + mc[i].margin_b + sl->spacing;
        } else { // HStack
            int ch_size = mc[i].is_spacer ? per_spacer
                        : mc[i].weight > 0 ? mc[i].flex_size
                        : mc[i].measured_w;
            w = ch_size;
            h = avail_h - mc[i].margin_t - mc[i].margin_b;
            if (mc[i].measured_h > 0 && mc[i].measured_h < h
                && !mc[i].is_spacer && mc[i].weight == 0
                && (!w32_fills_cross(mc[i].kind) || W32_CROSS_PINNED(i, 0))) {
                if (sl->alignment == 1)
                    y = sl->padding_top + mc[i].margin_t + (h - mc[i].measured_h) / 2;
                else if (sl->alignment == 2)
                    y = sl->padding_top + mc[i].margin_t + (h - mc[i].measured_h);
                else
                    y = sl->padding_top + mc[i].margin_t;
                h = mc[i].measured_h;
            } else {
                y = sl->padding_top + mc[i].margin_t;
            }
            x = cur + mc[i].margin_l;
            cur = x + w + mc[i].margin_r + sl->spacing;
            if (rtl) x = client_w - x - w;   // mirror within the row
        }
        SetWindowPos(children[i], NULL, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        w32_note_layout(children[i], w, h);
    }

    free(mc);
    free(children);
}

static Widget* w32_ctx_owner(HWND hwnd);                 // fwd (ctx menus)
static int w32_ctx_popup(Widget* w, int sx, int sy);     // fwd
static void w32_radio_enforce(int active_handle);        // fwd (toggle groups)

static LRESULT CALLBACK stack_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            stack_do_layout(hwnd);
            w32_note_layout(hwnd, LOWORD(lp), HIWORD(lp));
            return 0;

        case WM_CONTEXTMENU: {
            // Real right-click: find the nearest widget with stored items
            // (the click may land on a child) and pop the native menu.
            HWND target = (HWND)wp;
            Widget* owner = w32_ctx_owner(target ? target : hwnd);
            if (owner) {
                int sx = GET_X_LPARAM(lp), sy = GET_Y_LPARAM(lp);
                if (sx == -1 && sy == -1) {   // keyboard menu key
                    RECT r; GetWindowRect(owner->hwnd, &r);
                    sx = r.left + 8; sy = r.top + 8;
                }
                w32_ctx_popup(owner, sx, sy);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_LBUTTONDOWN: {
            // Splitview divider drag: press within the divider band captures.
            int h2 = handle_for_hwnd(hwnd);
            Widget* w2 = widget_at(h2);
            if (w2 && w2->kind == WK_SPLITVIEW) {
                int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
                int p = (w2->stack.orientation == 1) ? my : mx;
                if (p >= w2->split_eff && p <= w2->split_eff + AEUI_SPLIT_DIV) {
                    SetCapture(hwnd);
                    return 0;
                }
            }
            if (w2 && w2->active_set) {   /* container st_active press */
                w2->is_pressed = 1;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_MOUSEMOVE: {
            int h3 = handle_for_hwnd(hwnd);
            Widget* w3 = widget_at(h3);
            if (w3 && w3->kind == WK_SPLITVIEW && GetCapture() == hwnd) {
                int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
                int p = (w3->stack.orientation == 1) ? my : mx;
                w3->split_pos_enc = (p > 0 ? p : 0) + 1;
                stack_do_layout(hwnd);
                return 0;
            }
            /* Container hover: st_hover on a row was stored but invisible
               to a REAL pointer — only subclassed controls tracked hover.
               Same TrackMouseEvent dance they use. */
            if (w3 && w3->hover_set && !w3->is_hovered) {
                w3->is_hovered = 1;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_MOUSELEAVE: {
            int h3 = handle_for_hwnd(hwnd);
            Widget* w3 = widget_at(h3);
            if (w3 && w3->is_hovered) {
                w3->is_hovered = 0;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) { ReleaseCapture(); return 0; }
            {
                int h3 = handle_for_hwnd(hwnd);
                Widget* w3 = widget_at(h3);
                if (w3 && w3->is_pressed) {
                    w3->is_pressed = 0;
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            return DefWindowProcW(hwnd, msg, wp, lp);

        case WM_MOUSEWHEEL: {
            // A vlist container carries an on_scroll(dy) closure — one wheel
            // notch = one row step (wheel up = toward the start = -1).
            int h = handle_for_hwnd(hwnd);
            Widget* w = widget_at(h);
            if (w && w->on_scroll && w->on_scroll->fn) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                int step = delta > 0 ? -1 : (delta < 0 ? 1 : 0);
                if (step)
                    ((void(*)(void*, intptr_t))w->on_scroll->fn)(
                        w->on_scroll->env, (intptr_t)step);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_ERASEBKGND: {
            int h = handle_for_hwnd(hwnd);
            Widget* w = widget_at(h);
            /* Effective state colour: active beats hover beats base — the
               same ladder the ownerdraw button paint uses. Containers
               STORED st_hover/st_active but never painted them (the aecs
               gap hoverpaint finally specs): a hovered row kept its base. */
            if (w && (w->bg.has_value
                      || (w->hover_set && w->is_hovered)
                      || (w->active_set && w->is_pressed))) {
                HDC hdc = (HDC)wp;
                RECT r;
                GetClientRect(hwnd, &r);
                COLORREF bg = w->bg.has_value ? w->bg.color
                                              : GetSysColor(COLOR_BTNFACE);
                if (w->hover_set && w->is_hovered)  bg = w->hover_bg;
                if (w->active_set && w->is_pressed) bg = w->active_bg;
                HBRUSH br = CreateSolidBrush(bg);
                FillRect(hdc, &r, br);
                DeleteObject(br);
                return 1;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }


        case WM_COMMAND: {
            // Forward control notifications from children to their registered
            // on_click closures (buttons, checkboxes).
            HWND child = (HWND)lp;
            WORD code = HIWORD(wp);
            int ch = handle_for_hwnd(child);
            Widget* cw = widget_at(ch);
            if (cw) {
                if (cw->kind == WK_BUTTON && (code == BN_CLICKED || code == 0)) {
                    // A tab-strip button selects its tab (no user on_click).
                    int tab_idx = -1;
                    TabsState* ts = tabs_state_for_button(ch, &tab_idx);
                    if (ts) {
                        if (!cw->sealed) tabs_do_select(ts, tab_idx, 1);
                    } else if (!cw->sealed) invoke_closure(cw->on_click);
                } else if (cw->kind == WK_TOGGLE && code == BN_CLICKED) {
                    if (!cw->sealed) {
                        if (aether_ui_toggle_get_active(ch))
                            w32_radio_enforce(ch);
                        invoke_closure(cw->on_change);
                    }
                } else if ((cw->kind == WK_TEXTFIELD || cw->kind == WK_SECUREFIELD
                            || cw->kind == WK_TEXTAREA) && code == EN_CHANGE) {
                    if (!cw->sealed)
                        invoke_closure_str(cw->on_change,
                                           aether_ui_textfield_get_text(ch));
                    // Two-way bind_value: mirror the field into its state
                    // (compare-first, so the state→widget push doesn't echo).
                    if (cw->bound_state > 0) {
                        const char* t = aether_ui_textfield_get_text(ch);
                        StateCell* sc = state_cell(cw->bound_state);
                        if (sc && sc->type == AEUI_STATE_STRING
                            && (!sc->str || strcmp(sc->str, t ? t : "") != 0)) {
                            aether_ui_state_set_s(cw->bound_state, t ? t : "");
                        }
                    }
                } else if (cw->kind == WK_PICKER && code == CBN_SELCHANGE) {
                    if (!cw->sealed) invoke_closure(cw->on_change);
                }
            }
            return 0;
        }

        case WM_HSCROLL:
        case WM_VSCROLL: {
            HWND child = (HWND)lp;
            int ch = handle_for_hwnd(child);
            Widget* cw = widget_at(ch);
            if (cw && cw->kind == WK_SLIDER) {
                int pos = (int)SendMessageW(child, TBM_GETPOS, 0, 0);
                // Map pos back to the slider's min/max range.
                double min_v = cw->u.slider.min_v;
                double max_v = cw->u.slider.max_v;
                double val = min_v + (max_v - min_v) * (pos / 1000.0);
                cw->u.slider.cur_v = val;
                if (!cw->sealed) invoke_closure(cw->on_change);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            HWND child = (HWND)lp;
            HDC hdc = (HDC)wp;
            int ch = handle_for_hwnd(child);
            Widget* cw = widget_at(ch);
            if (cw) {
                if (cw->fg.has_value) SetTextColor(hdc, cw->fg.color);
                if (cw->bg.has_value) {
                    COLORREF use = cw->bg.color;
                    SetBkColor(hdc, use);
                    // Keep a cached brush per-widget (leaked for now; cleaned
                    // in WM_DESTROY via DeleteObject for widgets we know of).
                    static HBRUSH last_brush = NULL;
                    static COLORREF last_color = 0;
                    if (last_brush && last_color != use) {
                        DeleteObject(last_brush);
                        last_brush = NULL;
                    }
                    if (!last_brush) {
                        last_brush = CreateSolidBrush(use);
                        last_color = use;
                    }
                    return (LRESULT)last_brush;
                }
                SetBkMode(hdc, TRANSPARENT);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Main app window class + DPI awareness.
// ---------------------------------------------------------------------------
static const wchar_t* APP_CLASS = L"AetherUIAppWindow";
static const wchar_t* DIVIDER_CLASS = L"AetherUIDivider";
static const wchar_t* SPACER_CLASS = L"AetherUISpacer";
static const wchar_t* SCRIM_CLASS = L"AetherUIScrim";
static LRESULT CALLBACK scrim_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static const wchar_t* CANVAS_CLASS = L"AetherUICanvas";
static int win_classes_registered = 0;
static int gdiplus_started = 0;
static ULONG_PTR gdiplus_token = 0;

// GDI+ flat API — declared here to avoid pulling in <gdiplus.h> (C++ only).
typedef struct {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

__declspec(dllimport) int __stdcall GdiplusStartup(ULONG_PTR* token,
    const GdiplusStartupInput* input, void* output);
__declspec(dllimport) void __stdcall GdiplusShutdown(ULONG_PTR token);

static void ensure_gdiplus(void) {
    if (gdiplus_started) return;
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&gdiplus_token, &in, NULL) == 0) gdiplus_started = 1;
}

static LRESULT CALLBACK divider_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        int my = r.top + (r.bottom - r.top) / 2;
        MoveToEx(hdc, r.left, my, NULL);
        LineTo(hdc, r.right, my);
        SelectObject(hdc, old);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK spacer_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Canvas drawing backend lives farther down; the window proc forwards to it.
static LRESULT CALLBACK canvas_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK grid_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static int menu_dispatch_command(UINT id);

// Menu command IDs start here to avoid collision with button control IDs.
#define AE_MENU_ID_BASE 0x8000

// Window class name for grid containers; the class is registered in
// register_window_classes() alongside STACK / DIVIDER / SPACER / CANVAS.
static const wchar_t* GRID_CLASS = L"AetherUIGrid";

static LRESULT CALLBACK app_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETTINGCHANGE: {
            // OS light/dark flip ("ImmersiveColorSet") — tell the AeCS
            // appearance callback, already on the UI thread.
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0)
                aether_ui_appearance_invoke(aether_ui_dark_mode_check());
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_SIZE: {
            // Resize EVERY root-level container (and any modal scrim) to the
            // new client area — not just GetWindow(GW_CHILD): that's the
            // TOPMOST child in Z, which is the driver banner or an overlay
            // once those exist, so a /window/resize was resizing the banner
            // and leaving the actual root at its startup size (masked for as
            // long as specs only resized to at-or-below the initial size).
            RECT r;
            GetClientRect(hwnd, &r);
            for (HWND c = GetWindow(hwnd, GW_CHILD); c;
                 c = GetWindow(c, GW_HWNDNEXT)) {
                int ch = handle_for_hwnd(c);
                Widget* cw = widget_at(ch);
                if (!cw) continue;
                if (cw->kind == WK_VSTACK || cw->kind == WK_HSTACK
                    || cw->kind == WK_ZSTACK || cw->kind == WK_TABS
                    || cw->kind == WK_SPLITVIEW || cw->kind == WK_WRAP
                    || cw->kind == WK_SCRIM) {
                    SetWindowPos(c, NULL, 0, 0,
                                 r.right - r.left, r.bottom - r.top,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            // Don't cap programmatic sizing at the physical screen: Windows'
            // default ptMaxTrackSize is the display size, so a driver
            // /window/resize LARGER than the (small) VM screen silently
            // clamped back — gp's 1028→1716 grow landed at 1028 while the
            // 3200x2000 Xvfb on Linux never hit the limit. Users can't drag
            // past the screen anyway; this only frees SetWindowPos.
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMaxTrackSize.x = 32000;
            mmi->ptMaxTrackSize.y = 32000;
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* suggested = (RECT*)lp;
            SetWindowPos(hwnd, NULL,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            /* Mark the window's WIDGETS dead as well, not just the window. The
               registry never shrinks and Windows recycles HWND values, so the
               `dead` flag is the only thing that tells the driver a widget is
               gone -- the same rule clear_children, remove_child and
               navstack_pop each document. It has to happen here, on the
               parent's WM_DESTROY, because the children have not been
               destroyed yet and GetWindow can still enumerate them; by their
               own WM_DESTROY the tree is already coming apart. */
            mark_subtree_dead(hwnd);
            // Multi-window: mark this window dead; quit the loop only when the
            // last live window is gone.
            for (int i = 0; i < w32_window_count; i++)
                if (w32_windows[i].hwnd == hwnd) { w32_windows[i].live = 0; break; }
            if (w32_live_window_count() == 0) PostQuitMessage(0);
            return 0;
        case WM_COMMAND: {
            // WM_COMMAND with a menu ID (no control HWND) → look up the
            // registered closure. Otherwise forward to the stack proc,
            // which handles WM_COMMAND from child controls.
            WORD id = LOWORD(wp);
            if (lp == 0 && HIWORD(wp) == 0 && id >= AE_MENU_ID_BASE) {
                if (menu_dispatch_command(id)) return 0;
            }
            return stack_wnd_proc(hwnd, msg, wp, lp);
        }
        case WM_HSCROLL:
        case WM_VSCROLL:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            // App window forwards control notifications to the stack proc
            // by calling it directly; the root widget is always a stack.
            return stack_wnd_proc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_window_classes(HINSTANCE inst) {
    if (win_classes_registered) return;
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = app_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = stack_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = STACK_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = divider_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = DIVIDER_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = spacer_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = SPACER_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = canvas_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = CANVAS_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = grid_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = GRID_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = scrim_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = SCRIM_CLASS;
    RegisterClassExW(&wc);

    win_classes_registered = 1;
}

// DPI awareness setup — try the newest API first, fall back for older Windows.
typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);

static void init_dpi_awareness(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        FARPROC raw = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        SetProcessDpiAwarenessContextFn fn = (SetProcessDpiAwarenessContextFn)(void(*)(void))raw;
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    // Fallback: per-monitor aware (Windows 8.1+)
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessFn)(int);
        FARPROC raw = GetProcAddress(shcore, "SetProcessDpiAwareness");
        SetProcessDpiAwarenessFn fn = (SetProcessDpiAwarenessFn)(void(*)(void))raw;
        if (fn) fn(2);
        FreeLibrary(shcore);
    }
}

// Hidden holder window — serves as the initial parent for newly-created
// widgets. Win32 refuses to create WS_CHILD windows with a NULL parent, so
// every widget starts life parented here; it is reparented to its real
// container by aether_ui_widget_add_child_ctx / aether_ui_app_set_body.
static HWND widget_holder = NULL;

// Canvas keyboard-focus helpers (defined with the canvas globals below;
// used by the app-run/show path above them).
static void aeui_focus_pending_key_canvas(void);
static int  aeui_hwnd_is_key_canvas(HWND hwnd);

static int init_done = 0;
static void ensure_win_init(void) {
    if (init_done) return;
    init_done = 1;
    init_dpi_awareness();
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS
            | ICC_DATE_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    ensure_gdiplus();
    register_window_classes(GetModuleHandleW(NULL));
    // Create a hidden, message-only-ish holder. Not WS_OVERLAPPEDWINDOW
    // (that would show) — a plain popup window with no WS_VISIBLE.
    widget_holder = CreateWindowExW(
        0, STACK_CLASS, L"AetherUIHolder",
        WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
typedef struct {
    wchar_t* title;
    int width;
    int height;
    int root_handle;
    HWND hwnd;
    HMENU pending_menu;  // menu bar to SetMenu() on this app's window
} AppEntry;

static AppEntry* apps = NULL;
static int app_count = 0;
static int app_capacity = 0;

int aether_ui_app_create(const char* title, int width, int height) {
    ensure_win_init();
    if (app_count >= app_capacity) {
        app_capacity = app_capacity == 0 ? 4 : app_capacity * 2;
        apps = (AppEntry*)realloc(apps, sizeof(AppEntry) * app_capacity);
    }
    AppEntry* e = &apps[app_count];
    e->title = _wcsdup(utf8_to_wide(title));
    e->width = width;
    e->height = height;
    e->root_handle = 0;
    e->hwnd = NULL;
    e->pending_menu = NULL;
    app_count++;
    return app_count;
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    apps[app_handle - 1].root_handle = root_handle;
}

// Apply immersive dark mode to a window if the system is in dark mode.
static void apply_window_theme(HWND hwnd) {
    BOOL dark = FALSE;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD val = 0, sz = sizeof(val);
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL,
            (LPBYTE)&val, &sz) == ERROR_SUCCESS) {
            dark = (val == 0);
        }
        RegCloseKey(key);
    }
    if (dark) {
        BOOL v = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &v, sizeof(v));
    }
}

// Defined further down with the shortcut registry they belong to; the message
// loop above needs them here.
static int w32_key_from_msg(WPARAM vk, char* combo, int combosize,
                            char* name, int namesize);
static int aeui_win32_fire_shortcut(const char* combo);

void aether_ui_app_run_raw(int app_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    AppEntry* e = &apps[app_handle - 1];
    ensure_win_init();

    // DPI-scaled size
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(e->width, dpi, 96);
    int h = MulDiv(e->height, dpi, 96);

    // Account for non-client area so client ≈ requested size.
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE,
                              WS_EX_APPWINDOW, dpi);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    e->hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, APP_CLASS, e->title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!e->hwnd) return;
    apply_window_theme(e->hwnd);
    w32_window_register(e->hwnd, wide_to_utf8(e->title));  // primary = handle 1

    // Reparent the root widget into the app window.
    if (e->root_handle > 0) {
        Widget* rw = widget_at(e->root_handle);
        if (rw) {
            SetParent(rw->hwnd, e->hwnd);
            LONG_PTR st = GetWindowLongPtrW(rw->hwnd, GWL_STYLE);
            SetWindowLongPtrW(rw->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
            SetWindowLongPtrW(rw->hwnd, GWL_EXSTYLE,
                GetWindowLongPtrW(rw->hwnd, GWL_EXSTYLE) & ~WS_EX_APPWINDOW);
            RECT cr;
            GetClientRect(e->hwnd, &cr);
            SetWindowPos(rw->hwnd, NULL, 0, 0,
                         cr.right - cr.left, cr.bottom - cr.top,
                         SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    }

    // Attach the menu bar (if one was registered via aether_ui_menu_bar_attach).
    // We defer this until here so the window exists.
    if (e->pending_menu) {
        SetMenu(e->hwnd, e->pending_menu);
    }

    // Honor AETHER_UI_HEADLESS for CI and unattended scenarios. The window
    // is still created, the message loop still pumps, and the test server
    // still responds — but nothing is ever rendered to the visible desktop.
    // This keeps GitHub Actions `windows-latest` runs clean (no taskbar
    // icons, no UAC/SmartScreen visibility, no chance of a stuck window).
    const char* headless = getenv("AETHER_UI_HEADLESS");
    int show_mode = (headless && headless[0] && headless[0] != '0') ? SW_HIDE : SW_SHOW;
    ShowWindow(e->hwnd, show_mode);
    if (show_mode == SW_SHOW) UpdateWindow(e->hwnd);

    // A canvas that registered on_key wants the initial keyboard focus so
    // arrow keys work before the user clicks anything (gp/falling_blocks).
    // Now that the window is shown and the canvas is reparented into it,
    // the focus sticks. (Defined below with the canvas globals.)
    aeui_focus_pending_key_canvas();

    // Check AETHER_UI_TEST_PORT and launch test server if set.
    const char* test_port_env = getenv("AETHER_UI_TEST_PORT");
    if (test_port_env) {
        int port = atoi(test_port_env);
        if (port > 0 && e->root_handle > 0) {
            aether_ui_enable_test_server_impl(port, e->root_handle);
        }
    }

    // Message loop with Tab/Enter/Esc dialog-navigation support.
    // IsDialogMessageW routes Tab between WS_TABSTOP controls, Shift+Tab
    // reverses, Enter activates the default button, Esc cancels. Without
    // this wrap, those keys would fall through to the plain
    // TranslateMessage/DispatchMessage path and be ignored by child
    // controls (no focus traversal at all).
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        // When a key-registered canvas has focus, route keystrokes straight
        // to it — IsDialogMessage would otherwise eat Return (default button)
        // and Escape (cancel) before the canvas's WM_KEYDOWN sees them, and
        // gp/falling_blocks navigate with exactly those. (The driver's
        // /canvas/key path bypasses this loop entirely, so specs are
        // unaffected either way — this is for real keyboard input.)
        // Accelerators and the any-key handler get the keystroke BEFORE
        // IsDialogMessageW, for the same reason the canvas does: dialog nav
        // eats Return, Escape and Tab, and a registered Ctrl+R must not be
        // typed into whatever has focus. This is the piece #47 was about, and
        // it deliberately reuses the registry the driver route already fires,
        // so a real key and POST /window/key end in the same closure.
        if (msg.message == WM_KEYDOWN) {
            char w32_combo[64], w32_name[64];
            int w32_mods = w32_key_from_msg(msg.wParam, w32_combo,
                                            sizeof(w32_combo), w32_name,
                                            sizeof(w32_name));
            if (w32_mods >= 0) {
                if (aeui_win32_fire_shortcut(w32_combo)) {
                    continue;   // consumed: do not also type it
                }
                // Not bound: offer it to the any-key handler and let the key
                // carry on, so a focused control still receives what was
                // typed.
                aether_ui_window_key_deliver(w32_name, w32_mods);
            }
        }
        int canvas_has_focus =
            (msg.message == WM_KEYDOWN || msg.message == WM_CHAR) &&
            aeui_hwnd_is_key_canvas(msg.hwnd);
        // Only the top-level app window participates in dialog nav; child
        // popups created via aether_ui_window_create are independent.
        if (canvas_has_focus || !IsDialogMessageW(e->hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Surface table — "DSL with Scope" surfaces (window / render_to / record).
// Platform-agnostic handle bookkeeping (mirrors aether_ui_gtk4.c).
// ---------------------------------------------------------------------------
#define AUI_SURFACE_WINDOW 0
#define AUI_SURFACE_RENDER 1
#define AUI_SURFACE_RECORD 2

typedef struct {
    int container_handle;
    int kind;
    int app_handle;
    int diag_count;
} SurfaceEntry;

static SurfaceEntry* surfaces = NULL;
static int surface_count = 0;
static int surface_capacity = 0;

static SurfaceEntry* surface_for_container(int container_handle) {
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].container_handle == container_handle) return &surfaces[i];
    }
    return NULL;
}

static SurfaceEntry* surface_add(int container_handle, int kind, int app_handle) {
    if (surface_count >= surface_capacity) {
        surface_capacity = surface_capacity == 0 ? 4 : surface_capacity * 2;
        surfaces = realloc(surfaces, sizeof(SurfaceEntry) * surface_capacity);
    }
    SurfaceEntry* s = &surfaces[surface_count++];
    s->container_handle = container_handle;
    s->kind = kind;
    s->app_handle = app_handle;
    s->diag_count = 0;
    return s;
}

int aether_ui_surface_container_new_impl(int kind) {
    int container = aether_ui_vstack_create(0);
    surface_add(container, kind, 0);
    return container;
}

// Deferred-flush registry — see aether_ui_backend.h. Mirrors the GTK backend
// so the AeVG `vg { … }` deferred-colour fix works identically on Win32.
static AeClosure** deferred_flushes = NULL;
static int deferred_flush_count = 0;
static int deferred_flush_capacity = 0;

void aether_ui_register_deferred_flush_impl(void* boxed_closure) {
    if (!boxed_closure) return;
    if (deferred_flush_count >= deferred_flush_capacity) {
        deferred_flush_capacity = deferred_flush_capacity == 0 ? 4
                                  : deferred_flush_capacity * 2;
        deferred_flushes = realloc(deferred_flushes,
                                   sizeof(AeClosure*) * deferred_flush_capacity);
    }
    deferred_flushes[deferred_flush_count++] = (AeClosure*)boxed_closure;
}

void aether_ui_surface_flush_deferred_impl(void) {
    for (int i = 0; i < deferred_flush_count; i++) {
        invoke_closure(deferred_flushes[i]);
    }
    deferred_flush_count = 0;
}

void aether_ui_surface_run_impl(int container_handle,
                                const char* title, int width, int height) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s || s->kind != AUI_SURFACE_WINDOW) return;
    aether_ui_surface_flush_deferred_impl();
    int app = aether_ui_app_create(title, width, height);
    s->app_handle = app;
    aether_ui_app_set_body(app, container_handle);
    aether_ui_app_run_raw(app);
}

int aether_ui_surface_note_interactive_impl(int container_handle) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s) return 0;
    if (s->kind == AUI_SURFACE_WINDOW) return 0;
    s->diag_count++;
    return 1;
}

int aether_ui_surface_diag_count_impl(int container_handle) {
    SurfaceEntry* s = surface_for_container(container_handle);
    return s ? s->diag_count : 0;
}

// ---------------------------------------------------------------------------
// Core widgets: text, button, vstack, hstack, spacer, divider.
// ---------------------------------------------------------------------------
int aether_ui_text_create(const char* text) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", utf8_to_wide(text),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return register_widget_typed(h, WK_TEXT);
}

// Wrapping label: a STATIC with default (word-wrapping) style, given a fixed
// width so it wraps in height. SS_LEFT already wraps multiline text to width.
int aether_ui_text_wrapped_create(const char* text, int wrap_width_px) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", utf8_to_wide(text),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    int handle = register_widget_typed(h, WK_TEXT);
    Widget* w = widget_at(handle);
    if (w) {
        w->text_wrap = 1;
        if (wrap_width_px > 0) w->pref_width = wrap_width_px;
    }
    return handle;
}

void aether_ui_text_set_anchor(int handle, int anchor) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_TEXT || !w->hwnd) return;
    w->text_anchor = anchor;
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    st &= ~(SS_LEFT | SS_CENTER | SS_RIGHT);
    st |= anchor == 1 ? SS_CENTER : anchor == 2 ? SS_RIGHT : SS_LEFT;
    SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

int aether_ui_text_get_wrap(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_TEXT) ? w->text_wrap : 0;
}

void aether_ui_text_set_truncate(int handle, int mode) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_TEXT || !w->hwnd) return;
    // A Win32 STATIC ellipsizes through style bits and offers END (tail) and
    // PATH (middle, path-aware) only. There is no head ellipsis, so head is
    // applied as tail and RECORDED as tail: get_truncate reports what the
    // control actually does, the way overlay_material_effective does for a
    // blur that degraded to a tint.
    int eff = mode;
    if (eff == 1) eff = 3;
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    st &= ~(SS_ENDELLIPSIS | SS_PATHELLIPSIS | SS_WORDELLIPSIS);
    if (eff == 2)      st |= SS_PATHELLIPSIS;
    else if (eff == 3) st |= SS_ENDELLIPSIS;
    SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
    w->text_truncate = eff;
    InvalidateRect(w->hwnd, NULL, TRUE);
}

int aether_ui_text_get_truncate(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_TEXT) ? w->text_truncate : 0;
}
int aether_ui_text_get_anchor(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_TEXT) ? w->text_anchor : 0;
}

void aether_ui_text_set_string(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (w && w->hwnd) SetWindowTextW(w->hwnd, utf8_to_wide(text));
}

void aether_ui_button_set_label(int handle, const char* label) {
    Widget* w = widget_at(handle);
    if (w && w->hwnd) SetWindowTextW(w->hwnd, utf8_to_wide(label));
}

// Right-click context menus (REAL, 2026-07-20): items accumulate on the widget;
// a real right-click pops a TrackPopupMenu built from them, and the driver's
// /widget/{id}/context_menu[/{idx}] routes report/activate the same store —
// no test-only path.
void aether_ui_context_menu_item_impl(int handle, const char* label,
                                      void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (!w || !label) return;
    if (w->ctx_count >= w->ctx_cap) {
        w->ctx_cap = w->ctx_cap == 0 ? 8 : w->ctx_cap * 2;
        w->ctx_items = (struct W32CtxItem*)realloc(
            w->ctx_items, sizeof(struct W32CtxItem) * w->ctx_cap);
    }
    w->ctx_items[w->ctx_count].label = _strdup(label);
    w->ctx_items[w->ctx_count].closure = boxed_closure;
    w->ctx_count++;
}

// The nearest ancestor-or-self widget carrying context-menu items (a right-
// click on a child of the menu owner should still open it — GTK's gesture on
// the container behaves the same way).
static Widget* w32_ctx_owner(HWND hwnd) {
    for (HWND h = hwnd; h; h = GetParent(h)) {
        int wh = handle_for_hwnd(h);
        Widget* w = widget_at(wh);
        if (w && w->ctx_count > 0) return w;
    }
    return NULL;
}

// Pop the real menu at screen (sx, sy) and fire the chosen item's closure.
static int w32_ctx_popup(Widget* w, int sx, int sy) {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < w->ctx_count; i++)
        AppendMenuW(m, MF_STRING, (UINT_PTR)(i + 1),
                    utf8_to_wide(w->ctx_items[i].label));
    int chosen = (int)TrackPopupMenu(m,
        TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, w->hwnd, NULL);
    DestroyMenu(m);
    if (chosen >= 1 && chosen <= w->ctx_count) {
        invoke_closure((AeClosure*)w->ctx_items[chosen - 1].closure);
        return 1;
    }
    return 0;
}

// Shortcuts (item 9). Real via the driver's /window/key path (the same route
// GTK4/macOS use for headless key delivery): registered combos are matched
// verbatim (the app and the driver use identical combo strings, so no
// GTK-style normalization is needed here). shortcut_when adds a predicate;
// shortcut_chord adds a two-key state machine. A future real-keyboard path
// A real keypress reaches this registry through w32_key_from_msg, called from
// the message loop before IsDialogMessageW: same combo strings the driver
// route sends, so a keystroke and POST /window/key end in the same closure.
typedef struct {
    char* combo;             // as written ("Ctrl+E")
    AeClosure* closure;
    AeClosure* enabled;      // optional predicate |-> int; NULL = always
} W32Shortcut;
static W32Shortcut* w32_shortcuts = NULL;
static int w32_shortcut_count = 0, w32_shortcut_cap = 0;

typedef struct {
    char* first;
    char* second;
    AeClosure* closure;
} W32Chord;
static W32Chord* w32_chords = NULL;
static int w32_chord_count = 0, w32_chord_cap = 0;
static char* w32_chord_pending = NULL;   // armed prefix (owned)

void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure,
                                  void* enabled_closure) {
    if (!combo) return;
    if (w32_shortcut_count >= w32_shortcut_cap) {
        w32_shortcut_cap = w32_shortcut_cap == 0 ? 16 : w32_shortcut_cap * 2;
        w32_shortcuts = (W32Shortcut*)realloc(w32_shortcuts,
                                              sizeof(W32Shortcut) * w32_shortcut_cap);
    }
    W32Shortcut* s = &w32_shortcuts[w32_shortcut_count++];
    s->combo = _strdup(combo);
    s->closure = (AeClosure*)boxed_closure;
    s->enabled = (AeClosure*)enabled_closure;
}

// The any-key handler (ui.window_on_key). Registered and reachable from the
// driver, like this backend's shortcuts: the real WM_KEYDOWN to key-name
// translation is the same missing piece noted for the shortcut registry
// above, and lands with it.
static AeClosure* w32_window_key_closure = NULL;

// Files dropped on the window. DragAcceptFiles + WM_DROPFILES rather than an
// OLE IDropTarget: this file speaks the same generation of API throughout,
// WM_DROPFILES is exactly file drops (which is the case being asked for), and
// it needs no COM plumbing to stay readable in C. Registered here; the
// WM_DROPFILES arm in the window proc lands with the real key path.
static AeClosure* w32_file_drop_closure = NULL;

// Outbound file drag: a real OLE drag, which needs an IDataObject offering
// CF_HDROP and an IDropSource saying when the gesture ends. There is no
// non-COM route for this the way SHBrowseForFolderW was for the folder
// picker, so the interfaces are implemented by hand below.
//
// Both objects are STATIC singletons with a fixed refcount rather than
// heap-allocated per drag. DoDragDrop is synchronous: it does not return
// until the drop or the cancel, so exactly one drag exists at a time and
// there is nothing to race with. That removes the whole class of COM
// lifetime bug from a path nobody here can run and debug.
static char** w32_drag_paths = NULL;
static int w32_drag_paths_len = 0;

// The path this drag carries, COPIED for the duration rather than pointed at.
//
// DoDragDrop runs its own message loop, so app code can execute while a drag
// is in flight: a timer callback calling draggable(handle, other) frees the
// registry's string, and a pointer into it would dangle inside GetData. One
// copy avoids the whole question. MAX_PATH*4 covers a UTF-8 path comfortably.
static char w32_drag_active_path[MAX_PATH * 4];
static int  w32_drag_active = 0;

// --- IDropSource ---------------------------------------------------------
static HRESULT STDMETHODCALLTYPE w32_ds_QueryInterface(IDropSource* self,
                                                       REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE w32_ds_AddRef(IDropSource* self) {
    (void)self; return 2;   // static singleton: never freed
}
static ULONG STDMETHODCALLTYPE w32_ds_Release(IDropSource* self) {
    (void)self; return 1;
}
static HRESULT STDMETHODCALLTYPE w32_ds_QueryContinueDrag(IDropSource* self,
                                                          BOOL esc, DWORD keys) {
    (void)self;
    if (esc) return DRAGDROP_S_CANCEL;
    if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w32_ds_GiveFeedback(IDropSource* self,
                                                     DWORD effect) {
    (void)self; (void)effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;   // let the shell draw the cursor
}
static IDropSourceVtbl w32_drop_source_vtbl = {
    w32_ds_QueryInterface, w32_ds_AddRef, w32_ds_Release,
    w32_ds_QueryContinueDrag, w32_ds_GiveFeedback
};
static IDropSource w32_drop_source = { &w32_drop_source_vtbl };

// --- IDataObject (CF_HDROP only) -----------------------------------------
//
// CF_HDROP is a DROPFILES header followed by a DOUBLE-null-terminated list of
// wide paths. The second terminator is what tells the receiver the list ended;
// omitting it is the classic way a drop arrives as one path plus garbage.
static HGLOBAL w32_hdrop_for(const char* utf8_path) {
    if (!utf8_path || !*utf8_path) return NULL;
    wchar_t* wide = utf8_to_wide(utf8_path);
    if (!wide) return NULL;
    size_t chars = wcslen(wide) + 2;          // path NUL + list NUL
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GHND, bytes);
    if (!mem) return NULL;
    DROPFILES* df = (DROPFILES*)GlobalLock(mem);
    if (!df) { GlobalFree(mem); return NULL; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;
    wchar_t* dst = (wchar_t*)((char*)df + sizeof(DROPFILES));
    wcscpy(dst, wide);
    dst[wcslen(wide) + 1] = L'\0';           // the list terminator
    GlobalUnlock(mem);
    return mem;
}

static HRESULT STDMETHODCALLTYPE w32_do_QueryInterface(IDataObject* self,
                                                       REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE w32_do_AddRef(IDataObject* self) {
    (void)self; return 2;
}
static ULONG STDMETHODCALLTYPE w32_do_Release(IDataObject* self) {
    (void)self; return 1;
}
static HRESULT STDMETHODCALLTYPE w32_do_GetData(IDataObject* self,
                                                FORMATETC* fmt, STGMEDIUM* med) {
    (void)self;
    if (!fmt || !med) return E_POINTER;
    if (fmt->cfFormat != CF_HDROP || !(fmt->tymed & TYMED_HGLOBAL)) {
        return DV_E_FORMATETC;
    }
    HGLOBAL mem = w32_drag_active ? w32_hdrop_for(w32_drag_active_path) : NULL;
    if (!mem) return E_OUTOFMEMORY;
    memset(med, 0, sizeof(*med));
    med->tymed = TYMED_HGLOBAL;
    med->hGlobal = mem;
    med->pUnkForRelease = NULL;   // the receiver frees it
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w32_do_GetDataHere(IDataObject* self,
                                                    FORMATETC* fmt,
                                                    STGMEDIUM* med) {
    (void)self; (void)fmt; (void)med;
    return E_NOTIMPL;   // callers that need this fall back to GetData
}
static HRESULT STDMETHODCALLTYPE w32_do_QueryGetData(IDataObject* self,
                                                     FORMATETC* fmt) {
    (void)self;
    if (!fmt) return E_POINTER;
    return (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL))
        ? S_OK : DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE w32_do_GetCanonicalFormatEtc(IDataObject* self,
                                                              FORMATETC* in,
                                                              FORMATETC* out) {
    (void)self; (void)in;
    if (out) memset(out, 0, sizeof(*out));
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE w32_do_SetData(IDataObject* self,
                                                FORMATETC* fmt, STGMEDIUM* med,
                                                BOOL release) {
    (void)self; (void)fmt; (void)med; (void)release;
    return E_NOTIMPL;   // this object is read-only by design
}
static HRESULT STDMETHODCALLTYPE w32_do_EnumFormatEtc(IDataObject* self,
                                                      DWORD dir,
                                                      IEnumFORMATETC** out) {
    (void)self; (void)dir;
    if (out) *out = NULL;
    // Shell drop targets query CF_HDROP directly rather than enumerating, so
    // this stays unimplemented rather than carrying an enumerator that only
    // exists to be correct on paper.
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE w32_do_DAdvise(IDataObject* self,
                                                FORMATETC* fmt, DWORD flags,
                                                IAdviseSink* sink,
                                                DWORD* conn) {
    (void)self; (void)fmt; (void)flags; (void)sink;
    if (conn) *conn = 0;
    return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE w32_do_DUnadvise(IDataObject* self,
                                                  DWORD conn) {
    (void)self; (void)conn;
    return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE w32_do_EnumDAdvise(IDataObject* self,
                                                    IEnumSTATDATA** out) {
    (void)self;
    if (out) *out = NULL;
    return OLE_E_ADVISENOTSUPPORTED;
}
static IDataObjectVtbl w32_data_object_vtbl = {
    w32_do_QueryInterface, w32_do_AddRef, w32_do_Release,
    w32_do_GetData, w32_do_GetDataHere, w32_do_QueryGetData,
    w32_do_GetCanonicalFormatEtc, w32_do_SetData, w32_do_EnumFormatEtc,
    w32_do_DAdvise, w32_do_DUnadvise, w32_do_EnumDAdvise
};
static IDataObject w32_data_object = { &w32_data_object_vtbl };

// The gesture. A drag starts on a mouse MOVE with the button held, not on the
// press: starting on the press would swallow ordinary clicks on the widget,
// so a draggable row could never simply be clicked.
static LRESULT CALLBACK w32_drag_src_proc(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp, UINT_PTR id,
                                          DWORD_PTR ref) {
    (void)id;
    int handle = (int)ref;
    // WHICH widget was pressed, not merely THAT one was. This proc is shared
    // by every draggable widget, so a plain flag here is global state: press
    // on row A, keep the button down, move over row B, and B would start a
    // drag carrying B's file. The user would have grabbed A and dropped B.
    //
    // One mouse means one press, so a single handle is the whole state.
    static int pressed_handle = 0;
    if (msg == WM_LBUTTONDOWN) {
        pressed_handle = handle;
    } else if (msg == WM_LBUTTONUP) {
        pressed_handle = 0;
    } else if (msg == WM_MOUSEMOVE && pressed_handle == handle
               && (wp & MK_LBUTTON)) {
        const char* path = aether_ui_widget_drag_payload_impl(handle);
        if (path && *path) {
            pressed_handle = 0;   // one drag per press
            snprintf(w32_drag_active_path, sizeof(w32_drag_active_path),
                     "%s", path);
            w32_drag_active = 1;
            DWORD effect = 0;
            // Synchronous: returns on drop or cancel, which is what makes the
            // static singletons above safe.
            DoDragDrop(&w32_data_object, &w32_drop_source,
                       DROPEFFECT_COPY, &effect);
            w32_drag_active = 0;
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void aether_ui_widget_draggable_file_impl(int handle, const char* path) {
    if (handle < 1) return;
    if (handle > w32_drag_paths_len) {
        int want = handle + 16;
        w32_drag_paths = (char**)realloc(w32_drag_paths, sizeof(char*) * want);
        for (int i = w32_drag_paths_len; i < want; i++) w32_drag_paths[i] = NULL;
        w32_drag_paths_len = want;
    }
    free(w32_drag_paths[handle - 1]);
    w32_drag_paths[handle - 1] = (path && *path) ? _strdup(path) : NULL;

    // Arm the gesture once per widget. Re-arming would stack subclasses on a
    // widget whose payload is merely being updated.
    Widget* w = widget_at(handle);
    if (!w || !w->hwnd) return;
    if (path && *path && !w->drag_armed) {
        // OLE has to be initialised on this thread before DoDragDrop. It is
        // idempotent and RPC_E_CHANGED_MODE is fine: it means someone already
        // initialised it in a compatible way.
        OleInitialize(NULL);
        if (SetWindowSubclass(w->hwnd, w32_drag_src_proc, 2,
                              (DWORD_PTR)handle)) {
            w->drag_armed = 1;
        }
    }
}

const char* aether_ui_widget_drag_payload_impl(int handle) {
    if (handle < 1 || handle > w32_drag_paths_len) return "";
    return w32_drag_paths[handle - 1] ? w32_drag_paths[handle - 1] : "";
}

void aether_ui_window_on_file_drop_impl(void* boxed_closure) {
    w32_file_drop_closure = (AeClosure*)boxed_closure;
    // Window 1 is the primary in the multi-window registry. A handler set
    // before the window exists still takes effect: app_create calls this
    // again once there is an HWND to accept drops on.
    if (w32_window_count > 0 && w32_windows[0].hwnd) {
        DragAcceptFiles(w32_windows[0].hwnd, TRUE);
    }
}

int aether_ui_window_file_drop_deliver(const char* paths) {
    if (!w32_file_drop_closure || !w32_file_drop_closure->fn) return 0;
    if (!paths) return 1;   // presence probe
    ((void(*)(void*, const char*))w32_file_drop_closure->fn)(
        w32_file_drop_closure->env, paths);
    return 1;
}

void aether_ui_window_on_key_impl(void* boxed_closure) {
    w32_window_key_closure = (AeClosure*)boxed_closure;
}

int aether_ui_window_key_deliver(const char* key_name, int mods) {
    if (!w32_window_key_closure || !w32_window_key_closure->fn || !key_name) return 0;
    ((void(*)(void*, const char*, int))w32_window_key_closure->fn)(
        w32_window_key_closure->env, key_name, mods);
    return 1;
}

void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) {
    aether_ui_shortcut_when_impl(combo, boxed_closure, NULL);
}

void aether_ui_shortcut_chord_impl(const char* first_combo,
                                   const char* second_combo,
                                   void* boxed_closure) {
    if (!first_combo || !second_combo) return;
    if (w32_chord_count >= w32_chord_cap) {
        w32_chord_cap = w32_chord_cap == 0 ? 8 : w32_chord_cap * 2;
        w32_chords = (W32Chord*)realloc(w32_chords, sizeof(W32Chord) * w32_chord_cap);
    }
    W32Chord* ch = &w32_chords[w32_chord_count++];
    ch->first = _strdup(first_combo);
    ch->second = _strdup(second_combo);
    ch->closure = (AeClosure*)boxed_closure;
}

// Feed a combo into the chord machine. Returns 1 if consumed (armed a prefix
// or completed a chord).
// Combo comparison is CASE-INSENSITIVE.
//
// This backend matches registered combos verbatim, which was fine while the
// only producer was the driver route sending back exactly what the app
// registered. A real keypress is a second producer, and it has to invent a
// spelling: an app writes shortcut("Ctrl+Space") while the translation emits
// "Ctrl+space" from the key-name table, and a strcmp would never fire.
//
// GTK4 and macOS both normalise before comparing (aeui_normalize_combo,
// combo_normalize); this is the same idea at the comparison instead of at the
// registration, so nothing already stored has to change form.
static int w32_combo_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int w32_chord_feed(const char* combo) {
    if (w32_chord_pending) {
        for (int i = 0; i < w32_chord_count; i++) {
            if (w32_combo_eq(w32_chords[i].first, w32_chord_pending) &&
                w32_combo_eq(w32_chords[i].second, combo)) {
                AeClosure* c = w32_chords[i].closure;
                free(w32_chord_pending); w32_chord_pending = NULL;
                invoke_closure(c);
                return 1;
            }
        }
        free(w32_chord_pending); w32_chord_pending = NULL;  // cancel; fall through
    }
    for (int i = 0; i < w32_chord_count; i++) {
        if (w32_combo_eq(w32_chords[i].first, combo)) {
            w32_chord_pending = _strdup(combo);
            return 1;
        }
    }
    return 0;
}

// Fire the shortcut(s) bound to `combo` (chords first). Returns 1 if handled.
// A real keypress, translated to the combo string the registry stores and the
// driver sends. Those two have always been "identical combo strings, matched
// verbatim"; this makes the KEYBOARD produce the same spelling, which is the
// piece that was missing (#47).
//
// Two outputs, because two things want the key: `combo` for the accelerator
// registry ("Ctrl+R"), and `name` + mods for the any-key handler ("r", 2),
// exactly as GTK4 and AppKit hand them over.
static int w32_key_from_msg(WPARAM vk, char* combo, int combosize,
                            char* name, int namesize) {
    // Modifier keys alone are not a keypress: reporting them would fire an
    // any-key handler on every Ctrl the user touches on the way to Ctrl+R.
    // Returns the modifier bitmask, or -1 for "not a key worth reporting".
    // NOT 0 for that: 0 is a perfectly good mask, the one an unmodified
    // letter carries, so conflating them would drop every plain keystroke.
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN
        || vk == VK_RWIN) {
        return -1;
    }
    // Names match aeui_key_name_for_event on AppKit and gdk_keyval_name on
    // GTK4. An app comparing k == "BackSpace" has to see the same string
    // whichever backend delivered it.
    const char* n = NULL;
    switch (vk) {
        case VK_LEFT:   n = "Left";      break;
        case VK_RIGHT:  n = "Right";     break;
        case VK_UP:     n = "Up";        break;
        case VK_DOWN:   n = "Down";      break;
        case VK_RETURN: n = "Return";    break;
        case VK_ESCAPE: n = "Escape";    break;
        case VK_TAB:    n = "Tab";       break;
        case VK_SPACE:  n = "space";     break;
        case VK_BACK:   n = "BackSpace"; break;
        case VK_DELETE: n = "Delete";    break;
        case VK_HOME:   n = "Home";      break;
        case VK_END:    n = "End";       break;
        case VK_PRIOR:  n = "Page_Up";   break;
        case VK_NEXT:   n = "Page_Down"; break;
        default: break;
    }
    char one[2] = {0, 0};
    if (!n) {
        if (vk >= VK_F1 && vk <= VK_F24) {
            snprintf(name, namesize, "F%d", (int)(vk - VK_F1 + 1));
            n = name;
        } else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
            // VK codes for letters are the UPPERCASE character. The any-key
            // name is lower-case, matching what the other two deliver for an
            // unmodified letter; the combo keeps the upper-case form the app
            // writes in shortcut("Ctrl+R").
            one[0] = (char)((vk >= 'A' && vk <= 'Z') ? vk + 32 : vk);
            snprintf(name, namesize, "%s", one);
            n = name;
        } else {
            return -1;  // punctuation and the rest: not worth guessing
        }
    } else {
        snprintf(name, namesize, "%s", n);
    }

    int ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
    int shift = (GetKeyState(VK_SHIFT)   & 0x8000) ? 1 : 0;
    int alt   = (GetKeyState(VK_MENU)    & 0x8000) ? 1 : 0;
    int super = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? 1 : 0;

    // The combo spells the KEY as the app writes it: shortcut("Ctrl+R"), so
    // an upper-case letter here even though the any-key name is lower-case.
    char keypart[32];
    if ((vk >= 'A' && vk <= 'Z')) {
        keypart[0] = (char)vk; keypart[1] = '\0';
    } else {
        snprintf(keypart, sizeof(keypart), "%s", name);
    }
    snprintf(combo, combosize, "%s%s%s%s%s",
             ctrl ? "Ctrl+" : "", shift ? "Shift+" : "",
             alt ? "Alt+" : "", super ? "Super+" : "", keypart);
    return (ctrl ? 2 : 0) | (shift ? 1 : 0) | (alt ? 4 : 0) | (super ? 8 : 0);
}

static int aeui_win32_fire_shortcut(const char* combo) {
    if (w32_chord_feed(combo)) return 1;
    int fired = 0;
    for (int i = 0; i < w32_shortcut_count; i++) {
        if (!w32_combo_eq(w32_shortcuts[i].combo, combo)) continue;
        W32Shortcut* s = &w32_shortcuts[i];
        if (s->enabled && s->enabled->fn) {
            if (!((int(*)(void*))s->enabled->fn)(s->enabled->env)) continue;
        }
        invoke_closure(s->closure);
        fired = 1;
    }
    return fired;
}
void aether_ui_focus_impl(int handle) {
    Widget* w = widget_at(handle);
    if (w) SetFocus(w->hwnd);
}
void aether_ui_context_menu_item_accel_impl(int handle, const char* label,
                                            const char* accel,
                                            void* boxed_closure) {
    char joined[256];
    if (accel && accel[0]) {
        snprintf(joined, sizeof(joined), "%s    %s", label ? label : "", accel);
    } else {
        snprintf(joined, sizeof(joined), "%s", label ? label : "");
    }
    aether_ui_context_menu_item_impl(handle, joined, boxed_closure);
}


int aether_ui_button_create_plain(const char* label) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"BUTTON", utf8_to_wide(label),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return register_widget_typed(h, WK_BUTTON);
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    int handle = aether_ui_button_create_plain(label);
    Widget* w = widget_at(handle);
    if (w) w->on_click = (AeClosure*)boxed_closure;
    return handle;
}

void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    Widget* w = widget_at(handle);
    if (w) w->on_click = (AeClosure*)boxed_closure;
}

static int create_stack(int orientation, int spacing) {
    ensure_win_init();
    // WS_EX_CONTROLPARENT: the dialog tab-walk (GetNextDlgTabItem /
    // IsDialogMessage) recurses INTO this container instead of treating
    // it as a leaf — without it, Tab from a nested control resolves to
    // the wrong widget.
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, STACK_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    WidgetKind kind = orientation == 0 ? WK_HSTACK
                   : orientation == 1 ? WK_VSTACK : WK_ZSTACK;
    int handle = register_widget_typed(h, kind);
    Widget* w = widget_at(handle);
    if (w) {
        w->stack.orientation = orientation;
        w->stack.spacing = spacing;
    }
    return handle;
}

int aether_ui_vstack_create(int spacing) { return create_stack(1, spacing); }
int aether_ui_hstack_create(int spacing) { return create_stack(0, spacing); }
int aether_ui_zstack_create(void) { return create_stack(2, 0); }

int aether_ui_spacer_create(void) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, SPACER_CLASS, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    return register_widget_typed(h, WK_SPACER);
}

int aether_ui_divider_create(void) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, DIVIDER_CLASS, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    int handle = register_widget_typed(h, WK_DIVIDER);
    Widget* w = widget_at(handle);
    if (w) { w->pref_width = 1; w->pref_height = 12; }
    return handle;
}

// ---------------------------------------------------------------------------
// Parent → child wiring. Unlike GTK's "add_child", Win32 parents children
// at creation time OR reparents via SetParent. Our DSL calls this after
// creation, so we reparent here.
// ---------------------------------------------------------------------------
/* set_child — parent a widget INTO another widget (the chrome-drawn
   button face). On win32 the native BUTTON keeps its window text (so the
   /widgets text hook needs no stash) and the child hwnd covers its face. */
void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) {
    Widget* p = widget_at(parent_handle);
    Widget* c = widget_at(child_handle);
    if (!p || !c || !p->hwnd || !c->hwnd) return;
    SetParent(c->hwnd, p->hwnd);
    RECT r; GetClientRect(p->hwnd, &r);
    MoveWindow(c->hwnd, 0, 0, r.right, r.bottom, TRUE);
}

void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    Widget* p = widget_at(parent_handle);
    Widget* c = widget_at(child_handle);
    if (!p || !c) return;
    SetParent(c->hwnd, p->hwnd);
    LONG_PTR st = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
    SetWindowLongPtrW(c->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    ShowWindow(c->hwnd, SW_SHOW);
    // SetParent inserts the child at the TOP of the sibling Z-order, so
    // GetWindow(GW_CHILD) later enumerates children in REVERSE creation order
    // (a row built A,B,C enumerates C,B,A) — which reversed stack layout order.
    // Push each new child to the BOTTOM so enumeration matches creation order.
    SetWindowPos(c->hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (p->kind == WK_VSTACK || p->kind == WK_HSTACK || p->kind == WK_ZSTACK) {
        stack_do_layout(p->hwnd);
    }
}

void aether_ui_widget_set_hidden(int handle, int hidden) {
    Widget* w = widget_at(handle);
    if (w) ShowWindow(w->hwnd, hidden ? SW_HIDE : SW_SHOW);
}

// ---------------------------------------------------------------------------
// Input widgets: textfield, securefield, textarea, toggle, slider, picker,
// progressbar. All register change closures dispatched via the stack proc's
// WM_COMMAND handler.
// ---------------------------------------------------------------------------
int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    // Windows has no native placeholder on EDIT — use EM_SETCUEBANNER (comctl32 6+)
    if (placeholder && *placeholder) {
        SendMessageW(h, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                     (LPARAM)utf8_to_wide(placeholder));
    }
    int handle = register_widget_typed(h, WK_TEXTFIELD);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 24; }
    return handle;
}

int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_PASSWORD | ES_AUTOHSCROLL,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (placeholder && *placeholder) {
        SendMessageW(h, 0x1501, TRUE, (LPARAM)utf8_to_wide(placeholder));
    }
    int handle = register_widget_typed(h, WK_SECUREFIELD);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 24; }
    return handle;
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (w) SetWindowTextW(w->hwnd, utf8_to_wide(text));
}

const char* aether_ui_textfield_get_text(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return "";
    wchar_t buf[4096];
    GetWindowTextW(w->hwnd, buf, 4096);
    return wide_to_utf8(buf);
}

int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"BUTTON", utf8_to_wide(label),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    int handle = register_widget_typed(h, WK_TOGGLE);
    Widget* w = widget_at(handle);
    if (w) w->on_change = (AeClosure*)boxed_closure;
    return handle;
}

void aether_ui_toggle_set_active(int handle, int active) {
    Widget* w = widget_at(handle);
    if (w) SendMessageW(w->hwnd, BM_SETCHECK,
        active ? BST_CHECKED : BST_UNCHECKED, 0);
}

int aether_ui_toggle_get_active(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return 0;
    return SendMessageW(w->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
}

// Radio-group toggles (REAL, 2026-07-20): group ids on the Widget rather than
// BS_AUTORADIOBUTTON style swaps — the exclusivity is enforced in one helper
// used by BOTH the real BN_CLICKED path and the driver's /toggle action, so
// there is no test-only behaviour. Checking a member silently unchecks its
// groupmates (state only; the activated member's on_change is the signal, as
// on GTK4).
static int w32_next_radio_group = 1;

void aether_ui_toggle_set_group(int handle, int group_with) {
    Widget* a = widget_at(handle);
    Widget* b = widget_at(group_with);
    if (!a || !b) return;
    int gid = a->radio_group ? a->radio_group
            : b->radio_group ? b->radio_group
            : w32_next_radio_group++;
    a->radio_group = gid;
    b->radio_group = gid;
}

// `active_handle` just became checked — uncheck every other group member.
static void w32_radio_enforce(int active_handle) {
    Widget* w = widget_at(active_handle);
    if (!w || !w->radio_group) return;
    int n = widget_count;
    for (int h = 1; h <= n; h++) {
        if (h == active_handle) continue;
        Widget* o = widget_at(h);
        if (!o || o->radio_group != w->radio_group) continue;
        if (aether_ui_toggle_get_active(h))
            aether_ui_toggle_set_active(h, 0);
    }
}

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    double frac = (max_val > min_val) ? (initial - min_val) / (max_val - min_val) : 0;
    SendMessageW(h, TBM_SETPOS, TRUE, (LPARAM)(int)(frac * 1000));
    int handle = register_widget_typed(h, WK_SLIDER);
    Widget* w = widget_at(handle);
    if (w) {
        w->u.slider.min_v = min_val;
        w->u.slider.max_v = max_val;
        w->u.slider.cur_v = initial;
        w->on_change = (AeClosure*)boxed_closure;
        w->pref_height = 28;
    }
    return handle;
}

void aether_ui_slider_set_value(int handle, double value) {
    Widget* w = widget_at(handle);
    if (!w) return;
    double frac = (w->u.slider.max_v > w->u.slider.min_v)
        ? (value - w->u.slider.min_v) / (w->u.slider.max_v - w->u.slider.min_v) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    SendMessageW(w->hwnd, TBM_SETPOS, TRUE, (LPARAM)(int)(frac * 1000));
    w->u.slider.cur_v = value;
}

double aether_ui_slider_get_value(int handle) {
    Widget* w = widget_at(handle);
    return w ? w->u.slider.cur_v : 0.0;
}

int aether_ui_picker_create(void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    int handle = register_widget_typed(h, WK_PICKER);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 200; }
    return handle;
}

void aether_ui_picker_add_item(int handle, const char* item) {
    Widget* w = widget_at(handle);
    if (!w) return;
    SendMessageW(w->hwnd, CB_ADDSTRING, 0, (LPARAM)utf8_to_wide(item));
    // GtkDropDown parity: the first item becomes the selection (CB_SETCURSEL
    // does not fire CBN_SELCHANGE, so on_change stays quiet — GTK's default
    // selection is silent too). Without this, CB_GETCURSEL answers -1 and
    // the label is empty until the user picks.
    if (SendMessageW(w->hwnd, CB_GETCURSEL, 0, 0) == CB_ERR) {
        SendMessageW(w->hwnd, CB_SETCURSEL, 0, 0);
    }
}

void aether_ui_picker_set_selected(int handle, int index) {
    Widget* w = widget_at(handle);
    if (w) SendMessageW(w->hwnd, CB_SETCURSEL, (WPARAM)index, 0);
}

int aether_ui_picker_get_selected(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    return (int)SendMessageW(w->hwnd, CB_GETCURSEL, 0, 0);
}

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
            | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (placeholder && *placeholder) {
        SendMessageW(h, 0x1501, TRUE, (LPARAM)utf8_to_wide(placeholder));
    }
    int handle = register_widget_typed(h, WK_TEXTAREA);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 120; }
    return handle;
}

void aether_ui_textarea_set_text(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (w) SetWindowTextW(w->hwnd, utf8_to_wide(text));
}

char* aether_ui_textarea_get_text(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return strdup("");
    int len = GetWindowTextLengthW(w->hwnd);
    wchar_t* buf = (wchar_t*)malloc(sizeof(wchar_t) * (len + 1));
    GetWindowTextW(w->hwnd, buf, len + 1);
    char* out = strdup(wide_to_utf8(buf));
    free(buf);
    return out;
}

int aether_ui_scrollview_create(void) {
    // Approximation: a stack with scroll styles. Full virtualization would
    // need a custom class; for now we lean on the stack child hosting + the
    // parent window's scrollbars if needed.
    int handle = create_stack(1, 0);
    Widget* w = widget_at(handle);
    if (w) {
        LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, st | WS_VSCROLL | WS_HSCROLL);
        w->kind = WK_SCROLLVIEW;
    }
    return handle;
}

// splitview stub: a plain stack in the split's orientation (children lay
// out side by side / stacked; no draggable splitter). A real Win32
// splitter is follow-up work — this keeps the cross-platform ABI green.
// NB: splitview("h") means panes side-by-side = a HORIZONTAL stack.
// Real splitview (2026-07-20): a WK_SPLITVIEW container with two panes and a
// 6px draggable divider. `vertical` follows the DSL contract (same value the
// GTK4 GtkPaned path receives): vertical=0 → side-by-side panes (divider
// moves in x), vertical=1 → stacked panes (divider moves in y). The stack's
// orientation field records the split axis for layout + hit-testing.
int aether_ui_splitview_create(int vertical) {
    int handle = create_stack(vertical, 0);
    Widget* w = widget_at(handle);
    if (w) w->kind = WK_SPLITVIEW;
    return handle;
}
int aether_ui_split_position_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_SPLITVIEW) return -1;
    return w->split_eff;
}
void aether_ui_split_set_position_impl(int handle, int px) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_SPLITVIEW || px < 0) return;
    w->split_pos_enc = px + 1;
    stack_do_layout(w->hwnd);
    InvalidateRect(w->hwnd, NULL, TRUE);
}
void aether_ui_widget_weight_impl(int handle, int n) {
    Widget* w = widget_at(handle);
    if (w) w->weight = n;
}

void aether_ui_set_rtl(int handle, int on) {
    Widget* w = widget_at(handle);
    if (w && (w->kind == WK_HSTACK || w->kind == WK_VSTACK || w->kind == WK_ZSTACK)) {
        w->stack.rtl = on ? 1 : 0;
        if (w->hwnd) stack_do_layout(w->hwnd);
    }
}
void aether_ui_on_layout_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->on_layout = (AeClosure*)boxed_closure;
    w->ol_last_w = -1;
    w->ol_last_h = -1;
}

// Fire a widget's on_layout(|w,h|) when its allocation CHANGED — called from
// every layout site (stack children + the stack's own WM_SIZE).
static void w32_note_layout(HWND hwnd, int w, int h) {
    int handle = handle_for_hwnd(hwnd);
    Widget* wd = widget_at(handle);
    if (!wd || !wd->on_layout || !wd->on_layout->fn) return;
    if (w == wd->ol_last_w && h == wd->ol_last_h) return;
    wd->ol_last_w = w;
    wd->ol_last_h = h;
    ((void(*)(void*, intptr_t, intptr_t))wd->on_layout->fn)(
        wd->on_layout->env, (intptr_t)w, (intptr_t)h);
}

int aether_ui_wrap_create(void) {
    // REAL flow layout (2026-07-20): children fill left-to-right and wrap to
    // a new row when the width runs out (see the WK_WRAP branch in
    // stack_do_layout). GTK4's GtkFlowBox twin.
    int handle = create_stack(0, 6);
    Widget* w = widget_at(handle);
    if (w) w->kind = WK_WRAP;
    return handle;
}

// tabs — a native tab strip over a page stack.
//
// GTK builds this from GtkStackSwitcher + GtkStack; AppKit uses NSTabView.
// Win32 has no single control that draws a strip AND owns page views the way
// the DSL needs (SysTabControl32 draws only the strip; you still manage the
// pages), so we compose it from the primitives we already have: the tabs
// widget is a vstack tagged WK_TABS holding
//   child[0] = a header hstack of one push-button per tab (the strip), and
//   child[1] = a zstack of page vstacks (only the selected page is shown).
// stack_do_layout() special-cases WK_TABS to lay the strip on top and let the
// page zstack fill the rest. Clicking a strip button selects that index;
// tabs_select() does the same programmatically. Both drive tabs_do_select(),
// which shows the page, restyles the strip, and fires on_change deduped
// against the last index (matching GTK/AppKit).
struct TabsState {
    int        container_handle;  // the WK_TABS vstack
    int        header_handle;     // the strip hstack
    int        content_handle;    // the page zstack
    int        page_count;
    int        selected;
    AeClosure* on_change;
    int        btn_handles[64];   // strip button handle per page
    int        page_handles[64];  // page vstack handle per page
};

static TabsState* tabs_states = NULL;
static int tabs_state_count = 0;
static int tabs_state_capacity = 0;

static TabsState* tabs_state_for_handle(int container_handle) {
    for (int i = 0; i < tabs_state_count; i++) {
        if (tabs_states[i].container_handle == container_handle)
            return &tabs_states[i];
    }
    return NULL;
}

// Map a strip-button HWND back to (its tabs, its index). Used by WM_COMMAND to
// turn a button click into a tab selection without threading an index through
// the button's closure ABI.
static TabsState* tabs_state_for_button(int btn_handle, int* out_index) {
    for (int i = 0; i < tabs_state_count; i++) {
        for (int p = 0; p < tabs_states[i].page_count; p++) {
            if (tabs_states[i].btn_handles[p] == btn_handle) {
                if (out_index) *out_index = p;
                return &tabs_states[i];
            }
        }
    }
    return NULL;
}

// Show the selected page, hide the rest, mark the active strip button (bold),
// and fire on_change if the index actually changed.
static void tabs_do_select(TabsState* ts, int index, int fire) {
    if (!ts || index < 0 || index >= ts->page_count) return;
    for (int p = 0; p < ts->page_count; p++) {
        Widget* pg = widget_at(ts->page_handles[p]);
        if (pg) ShowWindow(pg->hwnd, p == index ? SW_SHOW : SW_HIDE);
        // Active strip button gets a bold font as the visible selection cue.
        Widget* bw = widget_at(ts->btn_handles[p]);
        if (bw && bw->hwnd) {
            HFONT base = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            if (p == index) {
                LOGFONTW lf; GetObjectW(base, sizeof(lf), &lf);
                lf.lfWeight = FW_BOLD;
                HFONT bold = CreateFontIndirectW(&lf);
                if (bw->custom_font) DeleteObject(bw->custom_font);
                bw->custom_font = bold;
                SendMessageW(bw->hwnd, WM_SETFONT, (WPARAM)bold, TRUE);
            } else {
                if (bw->custom_font) { DeleteObject(bw->custom_font); bw->custom_font = NULL; }
                SendMessageW(bw->hwnd, WM_SETFONT, (WPARAM)base, TRUE);
            }
        }
    }
    int changed = (ts->selected != index);
    ts->selected = index;
    Widget* content = widget_at(ts->content_handle);
    if (content) stack_do_layout(content->hwnd);
    // on_change takes the new index — call it with the arg, like GTK/AppKit.
    // (invoke_closure would drop the argument.)
    if (fire && changed) {
        AeClosure* c = ts->on_change;
        if (c && c->fn) ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)index);
    }
}

int aether_ui_tabs_create(void* boxed_closure) {
    int container = create_stack(1, 0);  // vertical: strip over content
    Widget* cw = widget_at(container);
    if (cw) cw->kind = WK_TABS;

    int header = create_stack(0, 4);     // horizontal strip
    int content = create_stack(2, 0);    // zstack of pages
    aether_ui_widget_add_child_ctx((void*)(intptr_t)container, header);
    aether_ui_widget_add_child_ctx((void*)(intptr_t)container, content);

    if (tabs_state_count >= tabs_state_capacity) {
        tabs_state_capacity = tabs_state_capacity == 0 ? 8 : tabs_state_capacity * 2;
        tabs_states = (TabsState*)realloc(tabs_states,
                                          sizeof(TabsState) * tabs_state_capacity);
    }
    TabsState* ts = &tabs_states[tabs_state_count++];
    ts->container_handle = container;
    ts->header_handle = header;
    ts->content_handle = content;
    ts->page_count = 0;
    ts->selected = 0;
    ts->on_change = (AeClosure*)boxed_closure;
    return container;
}

int aether_ui_tab_add(int tabs_handle, const char* title) {
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    if (!ts || ts->page_count >= 64) return 0;
    int idx = ts->page_count;

    // Strip button for this tab (click → select this index; wired in WM_COMMAND).
    int btn = aether_ui_button_create_plain(title ? title : "");
    aether_ui_widget_add_child_ctx((void*)(intptr_t)ts->header_handle, btn);

    // Page body is a vstack, so the tab's block children attach as in any vstack.
    int page = create_stack(1, 0);
    aether_ui_widget_add_child_ctx((void*)(intptr_t)ts->content_handle, page);

    ts->btn_handles[idx] = btn;
    ts->page_handles[idx] = page;
    ts->page_count++;

    // First page shows; later pages start hidden until selected.
    Widget* pg = widget_at(page);
    if (pg) ShowWindow(pg->hwnd, idx == 0 ? SW_SHOW : SW_HIDE);
    if (idx == 0) tabs_do_select(ts, 0, 0);  // bold the first strip button
    return page;
}

int aether_ui_tabs_selected(int tabs_handle) {
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    return ts && ts->page_count > 0 ? ts->selected : -1;
}
int aether_ui_tabs_count(int tabs_handle) {
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    return ts ? ts->page_count : 0;
}
void aether_ui_tabs_select(int tabs_handle, int index) {
    tabs_do_select(tabs_state_for_handle(tabs_handle), index, 1);
}
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    if (ts) ts->on_change = (AeClosure*)boxed_closure;
}

int aether_ui_progressbar_create(double fraction) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, PBM_SETRANGE32, 0, 1000);
    int v = (int)(fraction * 1000);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    SendMessageW(h, PBM_SETPOS, (WPARAM)v, 0);
    int handle = register_widget_typed(h, WK_PROGRESSBAR);
    Widget* w = widget_at(handle);
    if (w) { w->u.progressbar.fraction = fraction; w->pref_height = 20; }
    return handle;
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    Widget* w = widget_at(handle);
    if (!w) return;
    int v = (int)(fraction * 1000);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    SendMessageW(w->hwnd, PBM_SETPOS, (WPARAM)v, 0);
    w->u.progressbar.fraction = fraction;
}

// ---------------------------------------------------------------------------
// Form / FormSection / NavStack — thin wrappers over VStack.
// ---------------------------------------------------------------------------
int aether_ui_form_create(void) {
    int handle = create_stack(1, 12);
    Widget* w = widget_at(handle);
    if (w) {
        w->kind = WK_FORM;
        w->stack.padding_top = w->stack.padding_bottom = 12;
        w->stack.padding_left = w->stack.padding_right = 16;
    }
    return handle;
}

int aether_ui_form_section_create(const char* title) {
    int handle = create_stack(1, 6);
    Widget* w = widget_at(handle);
    if (w) w->kind = WK_FORM_SECTION;
    if (title && *title) {
        int th = aether_ui_text_create(title);
        Widget* tw = widget_at(th);
        if (tw) {
            // Bold section header
            HFONT base = (HFONT)SendMessageW(tw->hwnd, WM_GETFONT, 0, 0);
            LOGFONTW lf;
            GetObjectW(base, sizeof(lf), &lf);
            lf.lfWeight = FW_BOLD;
            HFONT bold = CreateFontIndirectW(&lf);
            SendMessageW(tw->hwnd, WM_SETFONT, (WPARAM)bold, TRUE);
            tw->custom_font = bold;
        }
        aether_ui_widget_add_child_ctx((void*)(intptr_t)handle, th);
    }
    return handle;
}

int aether_ui_navstack_create(void) {
    int handle = create_stack(1, 0);
    Widget* w = widget_at(handle);
    if (w) w->kind = WK_NAVSTACK;
    return handle;
}

/* An EXPLICIT page stack per navstack, because pop cannot be inferred from
   the window list. The old pop scanned children for "the last visible one"
   and DestroyWindow'd it -- but push hides the previous pages and SetParent
   does not guarantee where a new child lands in GW_HWNDNEXT order, so the
   scan could pick the wrong window (or none). The driver spec caught it on
   its first run: after a pop the widget census stayed at 18, i.e. nothing
   was destroyed. GTK4 has always kept a per-stack page count; this is the
   win32 equivalent, and it stores the HWNDs so pop needs no guessing.
 */
#define W32_NAV_MAX 64
#define W32_NAV_DEPTH 32
static HWND w32_nav_pages[W32_NAV_MAX][W32_NAV_DEPTH];
static int  w32_nav_depth[W32_NAV_MAX];

void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    Widget* host = widget_at(handle);
    Widget* body = widget_at(body_handle);
    if (!host || !body) return;
    /* THE TITLE, as real widgets -- see the GTK4 push for why this is a
       widget bar rather than a per-platform chrome API. Wrapping happens
       BEFORE the page stack records the HWND, so pop destroys the wrapper
       and takes the title with it. */
    if (title && title[0]) {
        int wrap_h = aether_ui_vstack_create(0);
        int bar_h = aether_ui_text_create(title);
        Widget* wrap = widget_at(wrap_h);
        Widget* bar = widget_at(bar_h);
        if (wrap && bar) {
            SetParent(bar->hwnd, wrap->hwnd);
            SetParent(body->hwnd, wrap->hwnd);
            LONG_PTR bs = GetWindowLongPtrW(bar->hwnd, GWL_STYLE);
            SetWindowLongPtrW(bar->hwnd, GWL_STYLE, bs | WS_CHILD | WS_VISIBLE);
            LONG_PTR ys = GetWindowLongPtrW(body->hwnd, GWL_STYLE);
            SetWindowLongPtrW(body->hwnd, GWL_STYLE, ys | WS_CHILD | WS_VISIBLE);
            ShowWindow(bar->hwnd, SW_SHOW);
            ShowWindow(body->hwnd, SW_SHOW);
            stack_do_layout(wrap->hwnd);
            body = wrap;          /* the PAGE is now the wrapper */
        }
    }
    // Hide previous children, show this one.
    for (HWND c = GetWindow(host->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        ShowWindow(c, SW_HIDE);
    }
    if (handle >= 1 && handle <= W32_NAV_MAX) {
        int d = w32_nav_depth[handle - 1];
        if (d < W32_NAV_DEPTH) {
            w32_nav_pages[handle - 1][d] = body->hwnd;
            w32_nav_depth[handle - 1] = d + 1;
        }
    }
    SetParent(body->hwnd, host->hwnd);
    LONG_PTR st = GetWindowLongPtrW(body->hwnd, GWL_STYLE);
    SetWindowLongPtrW(body->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    ShowWindow(body->hwnd, SW_SHOW);
    stack_do_layout(host->hwnd);
}

int aether_ui_navstack_depth(int handle) {
    if (handle < 1 || handle > W32_NAV_MAX) return 0;
    return w32_nav_depth[handle - 1];
}

/* Defined with the other registry helpers below; navstack_pop needs it. */
static void mark_subtree_dead(HWND hwnd);

void aether_ui_navstack_pop(int handle) {
    Widget* host = widget_at(handle);
    if (!host) return;
    if (handle < 1 || handle > W32_NAV_MAX) return;
    int d = w32_nav_depth[handle - 1];
    if (d <= 0) return;              // at the root: a no-op, not an underflow
    HWND top = w32_nav_pages[handle - 1][d - 1];
    w32_nav_depth[handle - 1] = d - 1;
    if (top && IsWindow(top)) {
        /* mark_subtree_dead FIRST: the registry never shrinks, and Windows
           recycles HWND values, so IsWindow() alone cannot tell a destroyed
           page from a live widget that inherited its handle -- the `dead`
           flag is what the driver actually reads. It must run BEFORE
           DestroyWindow, because afterwards GetWindow can no longer
           enumerate the children to mark them.

           Omitting this is why the spec stayed red after the page stack
           landed: pop destroyed the right window, but its widgets kept
           reporting live types and the census never fell. */
        mark_subtree_dead(top);
        DestroyWindow(top);
    }
    // Re-show ONLY the page beneath, not every child: showing all of them
    // stacked every previously-pushed page on top of each other.
    if (d - 1 > 0) {
        HWND prev = w32_nav_pages[handle - 1][d - 2];
        if (prev && IsWindow(prev)) ShowWindow(prev, SW_SHOW);
    } else {
        for (HWND c = GetWindow(host->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            ShowWindow(c, SW_SHOW);
        }
    }
    stack_do_layout(host->hwnd);
}

// ---------------------------------------------------------------------------
// Styling + theming.
// ---------------------------------------------------------------------------
static void w32_ensure_owner_draw(Widget* w);   // styled painter (below)

static inline COLORREF rgb_from_doubles(double r, double g, double b) {
    int ri = (int)(r * 255); if (ri < 0) ri = 0; if (ri > 255) ri = 255;
    int gi = (int)(g * 255); if (gi < 0) gi = 0; if (gi > 255) gi = 255;
    int bi = (int)(b * 255); if (bi < 0) bi = 0; if (bi > 255) bi = 255;
    return RGB(ri, gi, bi);
}

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    (void)a; // Solid colors only; alpha requires layered-window composition.
    Widget* w = widget_at(handle);
    if (!w) return;
    w->bg.has_value = 1;
    w->bg.color = rgb_from_doubles(r, g, b);
    w32_ensure_owner_draw(w);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->gradient_enabled = 1;
    w->grad_a = rgb_from_doubles(r1, g1, b1);
    w->grad_b = rgb_from_doubles(r2, g2, b2);
    w->grad_vertical = vertical;
    InvalidateRect(w->hwnd, NULL, TRUE);
}

void aether_ui_set_text_color(int handle, double r, double g, double b) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->fg.has_value = 1;
    w->fg.color = rgb_from_doubles(r, g, b);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

static void apply_font(Widget* w) {
    if (!w) return;
    LOGFONTW lf = {0};
    HFONT base = (HFONT)SendMessageW(w->hwnd, WM_GETFONT, 0, 0);
    if (base) GetObjectW(base, sizeof(lf), &lf);
    else GetObjectW((HFONT)GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    if (w->font_size > 0) {
        // Font size in points → logical units for current DPI
        HDC hdc = GetDC(w->hwnd);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(w->hwnd, hdc);
        lf.lfHeight = -MulDiv((int)w->font_size, dpi, 72);
    }
    lf.lfWeight = w->font_bold ? FW_BOLD : FW_NORMAL;
    if (w->font_family) {
        wcsncpy(lf.lfFaceName, w->font_family, LF_FACESIZE - 1);
        lf.lfFaceName[LF_FACESIZE - 1] = L'\0';
    }
    if (w->custom_font) DeleteObject(w->custom_font);
    w->custom_font = CreateFontIndirectW(&lf);
    SendMessageW(w->hwnd, WM_SETFONT, (WPARAM)w->custom_font, TRUE);
}

void aether_ui_set_font_size(int handle, double size) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->font_size = size;
    apply_font(w);
}

void aether_ui_set_font_bold(int handle, int bold) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->font_bold = bold;
    w->font_weight_set = 1;   // explicit — the driver reads this back
    apply_font(w);
}

// Widget-level font family (AeCS v1.1). Passed verbatim into LOGFONT's face
// name — prefer real Windows face names ("Consolas", "Segoe UI"); generic
// CSS families fall through the font mapper's default matching.
void aether_ui_set_font_family(int handle, const char* family) {
    Widget* w = widget_at(handle);
    if (!w || !family || !family[0]) return;
    free(w->font_family);
    free(w->font_family_u8);
    w->font_family = _wcsdup(utf8_to_wide(family));
    w->font_family_u8 = _strdup(family);
    apply_font(w);
}

const char* aether_ui_styled_font_family_impl(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->font_family_u8) ? w->font_family_u8 : "";
}

const char* aether_ui_styled_weight_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w || !w->font_weight_set) return "";
    return w->font_bold ? "bold" : "normal";
}

void aether_ui_set_corner_radius(int handle, double radius) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->corner_radius = (int)radius;
    w32_ensure_owner_draw(w);
    RECT r;
    GetClientRect(w->hwnd, &r);
    HRGN rgn = CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1,
                                   (int)radius * 2, (int)radius * 2);
    SetWindowRgn(w->hwnd, rgn, TRUE);
}


// ── Styled-button painting (border / hover / active) ────────────────────
//
// win32 buttons are native BUTTON controls: WM_CTLCOLORBTN can tint a
// background brush but cannot draw a border, and nothing tracks hover per
// control. GTK4 gets both from CSS (:hover/:active + border), macOS from
// CALayer. To match, a widget that has ANY of those styles gets subclassed
// and owner-drawn: we paint the rounded rect ourselves, pick the colour for
// the current state, stroke the border, and draw the label centred.
//
// Only STYLED widgets are subclassed — an unstyled button keeps the native
// look and the native theme, so nothing regresses for apps that never call
// the styling API.
static LRESULT CALLBACK styled_btn_proc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp, UINT_PTR id, DWORD_PTR ref);

// Does this widget need our painting? (any of border / hover / active / bg)
static int w32_needs_owner_draw(Widget* w) {
    if (!w) return 0;
    if (w->kind != WK_BUTTON && w->kind != WK_TOGGLE) return 0;
    return w->border_set || w->hover_set || w->active_set || w->bg.has_value;
}

// Install (idempotent) the styled painter on a widget that now has styles.
static void w32_ensure_owner_draw(Widget* w) {
    if (!w || w->owner_drawn) return;
    if (!w32_needs_owner_draw(w)) return;
    if (SetWindowSubclass(w->hwnd, styled_btn_proc, 1, 0)) {
        w->owner_drawn = 1;
        InvalidateRect(w->hwnd, NULL, TRUE);
    }
}

static LRESULT CALLBACK styled_btn_proc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    int handle = handle_for_hwnd(hwnd);
    Widget* w = widget_at(handle);
    switch (msg) {
        case WM_MOUSEMOVE: {
            // TrackMouseEvent is what makes WM_MOUSELEAVE arrive; without it
            // a control never learns the pointer left.
            if (w && !w->is_hovered) {
                w->is_hovered = 1;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (w && w->is_hovered) {
                w->is_hovered = 0;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            if (w) InvalidateRect(hwnd, NULL, TRUE);
            break;
        case WM_ERASEBKGND:
            return 1;                      // painted wholly in WM_PAINT
        case WM_PRINTCLIENT:
        case WM_PAINT: {
            if (!w) break;
            // WM_PRINTCLIENT hands us a DC to render into (that is how the
            // driver screenshots a window that never mapped); WM_PAINT gets
            // one from BeginPaint. Same drawing either way.
            PAINTSTRUCT ps;
            HDC hdc;
            int printing = (msg == WM_PRINTCLIENT);
            if (printing) hdc = (HDC)wp;
            else hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            // State → background. active beats hover beats the base bg.
            int pressed = (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
            COLORREF bg = w->bg.has_value ? w->bg.color : GetSysColor(COLOR_BTNFACE);
            if (w->hover_set && w->is_hovered) bg = w->hover_bg;
            if (w->active_set && pressed) bg = w->active_bg;

            HBRUSH br = CreateSolidBrush(bg);
            HPEN pen = w->border_set && w->border_width > 0
                       ? CreatePen(PS_SOLID, w->border_width, w->border_color)
                       : (HPEN)GetStockObject(NULL_PEN);
            HBRUSH oldbr = (HBRUSH)SelectObject(hdc, br);
            HPEN oldpen = (HPEN)SelectObject(hdc, pen);

            int r = w->corner_radius;
            if (r > 0) {
                // A capsule: cap the radius at half the SHORT side, matching
                // GTK4/CSS behaviour for border-radius: 999px.
                int maxr = ((rc.right - rc.left) < (rc.bottom - rc.top)
                            ? (rc.right - rc.left) : (rc.bottom - rc.top)) / 2;
                if (r > maxr) r = maxr;
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r * 2, r * 2);
            } else {
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            }

            SelectObject(hdc, oldbr);
            SelectObject(hdc, oldpen);
            DeleteObject(br);
            if (w->border_set && w->border_width > 0) DeleteObject(pen);

            // Label, in the widget's font and foreground colour.
            wchar_t text[512];
            int tlen = GetWindowTextW(hwnd, text, 512);
            if (tlen > 0) {
                HFONT font = w->custom_font ? w->custom_font
                             : (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
                HFONT oldf = font ? (HFONT)SelectObject(hdc, font) : NULL;
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, w->fg.has_value ? w->fg.color
                                                  : GetSysColor(COLOR_BTNTEXT));
                DrawTextW(hdc, text, tlen, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (oldf) SelectObject(hdc, oldf);
            }
            if (!printing) EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, styled_btn_proc, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Solid border, painted by styled_btn_proc (see above). win32 buttons
// are native controls with no custom-paint hook here, so drawing an outline
// needs NM_CUSTOMDRAW owner-draw plumbing this backend does not have (GTK4
// gets it from CSS, macOS from CALayer.borderWidth). The value round-trips
// through /widgets so specs assert identically on all three, and the paint
// side is a contained follow-up. See roadmap.md.
void aether_ui_set_border(int handle, double width, double r, double g, double b) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->border_width = (int)width;
    w->border_color = rgb_from_doubles(r, g, b);
    w->border_set = 1;
    w32_ensure_owner_draw(w);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

// (width<<24) | flag | rgb, or -1 when unset — mirrors the GTK4 encoding so
// /widgets JSON reads identically on every backend.
int aether_ui_styled_border_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w || !w->border_set) return -1;
    COLORREF c = w->border_color;
    int rgbv = ((GetRValue(c) & 0xFF) << 16) | ((GetGValue(c) & 0xFF) << 8) | (GetBValue(c) & 0xFF);
    return (w->border_width << 24) | 0x40000000 | rgbv;
}

// Interaction states, PAINTED by styled_btn_proc: the subclass tracks hover
// via TrackMouseEvent and reads BM_GETSTATE for pressed, then picks the
// background accordingly (active beats hover beats base). state: 0=hover,
// 1=active.
void aether_ui_set_state_style(int handle, int state,
                               double br, double bg_, double bb,
                               double fr, double fg_, double fb) {
    (void)fr; (void)fg_; (void)fb;   // fg per-state: follow-up
    Widget* w = widget_at(handle);
    if (!w) return;
    if (br < 0.0) return;
    if (state == 1) {
        w->active_set = 1;
        w->active_bg = rgb_from_doubles(br, bg_, bb);
    } else {
        w->hover_set = 1;
        w->hover_bg = rgb_from_doubles(br, bg_, bb);
    }
    w32_ensure_owner_draw(w);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

int aether_ui_state_style_impl(int handle, int state) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    int set = (state == 1) ? w->active_set : w->hover_set;
    if (!set) return -1;
    COLORREF c = (state == 1) ? w->active_bg : w->hover_bg;
    return ((GetRValue(c) & 0xFF) << 16) | ((GetGValue(c) & 0xFF) << 8) | (GetBValue(c) & 0xFF);
}

void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->stack.padding_top = (int)top;
    w->stack.padding_right = (int)right;
    w->stack.padding_bottom = (int)bottom;
    w->stack.padding_left = (int)left;
    if (w->kind == WK_VSTACK || w->kind == WK_HSTACK || w->kind == WK_ZSTACK) {
        stack_do_layout(w->hwnd);
    }
}

void aether_ui_set_width(int handle, int width) {
    Widget* w = widget_at(handle);
    if (w) w->pref_width = width;
}

void aether_ui_set_height(int handle, int height) {
    Widget* w = widget_at(handle);
    if (w) w->pref_height = height;
}

void aether_ui_set_opacity(int handle, double opacity) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->opacity = opacity;
    // Top-level windows only, deliberately. NB WS_CHILD is a REGULAR style, so
    // it must be read from GWL_STYLE: this line used to query GWL_EXSTYLE,
    // where the same bit (0x40000000) means WS_EX_NOINHERITLAYOUT and is
    // normally clear -- so the guard never fired and children DID get made
    // layered, the opposite of what it says. That is not harmless: line ~7788
    // notes ChildWindowFromPointEx skips WS_EX_LAYERED children, so hit-testing
    // could break on any widget an app set opacity on.
    //
    // Child alpha itself is NOT known-broken (uniform alpha works on child
    // windows since Win8, and the overlay exit fade relies on it) -- it is
    // simply not wired up here. See TODO.md, "win32 child-widget opacity".
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    if (!(st & WS_CHILD)) {
        LONG_PTR ex = GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(w->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        double a = opacity;
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        SetLayeredWindowAttributes(w->hwnd, 0, (BYTE)(a * 255), LWA_ALPHA);
    }
}

void aether_ui_set_enabled(int handle, int enabled) {
    Widget* w = widget_at(handle);
    if (w) EnableWindow(w->hwnd, enabled);
}

void aether_ui_set_tooltip(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (!w) return;
    static HWND tooltip_hwnd = NULL;
    if (!tooltip_hwnd) {
        tooltip_hwnd = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            NULL, NULL, GetModuleHandleW(NULL), NULL);
    }
    if (w->tooltip) free(w->tooltip);
    w->tooltip = _wcsdup(utf8_to_wide(text));
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(w->hwnd);
    ti.uId = (UINT_PTR)w->hwnd;
    ti.lpszText = w->tooltip;
    SendMessageW(tooltip_hwnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// ── Accessibility (semantics layer) ──────────────────────────────────
// Store the author's a11y intent AND push it into MSAA via Dynamic Annotation
// (IAccPropServices::SetHwndPropStr/SetHwndProp) so a real AT client (Narrator)
// reads the overridden name/role — no custom IAccessible needed. The driver
// reads the side-store back. See docs/design/accessibility.md.
static char* a11y_dup(const char* s) { return (s && *s) ? _strdup(s) : NULL; }

// Our role vocabulary -> MSAA ROLE_SYSTEM_* (0 = leave the system role).
static long w32_msaa_role(const char* role) {
    if (!role) return 0;
    if (!strcmp(role, "button"))      return ROLE_SYSTEM_PUSHBUTTON;
    if (!strcmp(role, "checkbox"))    return ROLE_SYSTEM_CHECKBUTTON;
    if (!strcmp(role, "radio"))       return ROLE_SYSTEM_RADIOBUTTON;
    if (!strcmp(role, "link"))        return ROLE_SYSTEM_LINK;
    if (!strcmp(role, "heading"))     return ROLE_SYSTEM_STATICTEXT;
    if (!strcmp(role, "image"))       return ROLE_SYSTEM_GRAPHIC;
    if (!strcmp(role, "group"))       return ROLE_SYSTEM_GROUPING;
    if (!strcmp(role, "list"))        return ROLE_SYSTEM_LIST;
    if (!strcmp(role, "listitem"))    return ROLE_SYSTEM_LISTITEM;
    if (!strcmp(role, "tab"))         return ROLE_SYSTEM_PAGETAB;
    if (!strcmp(role, "tablist"))     return ROLE_SYSTEM_PAGETABLIST;
    if (!strcmp(role, "menu"))        return ROLE_SYSTEM_MENUPOPUP;
    if (!strcmp(role, "menuitem"))    return ROLE_SYSTEM_MENUITEM;
    if (!strcmp(role, "dialog"))      return ROLE_SYSTEM_DIALOG;
    if (!strcmp(role, "alert"))       return ROLE_SYSTEM_ALERT;
    if (!strcmp(role, "textbox"))     return ROLE_SYSTEM_TEXT;
    if (!strcmp(role, "slider"))      return ROLE_SYSTEM_SLIDER;
    if (!strcmp(role, "progressbar")) return ROLE_SYSTEM_PROGRESSBAR;
    return 0;
}

// Push a name/role annotation onto a window via MSAA Dynamic Annotation.
// Best-effort: if COM/oleacc isn't available the side-store still drives the
// driver, so a failure here is silent.
static void w32_annotate(HWND hwnd, const char* name, const char* role) {
    if (!hwnd) return;
    IAccPropServices* svc = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_AccPropServices, NULL,
        CLSCTX_INPROC_SERVER, &IID_IAccPropServices, (void**)&svc);
    if (FAILED(hr) || !svc) return;
    if (name && *name) {
        wchar_t* wn = _wcsdup(utf8_to_wide(name));
        svc->lpVtbl->SetHwndPropStr(svc, hwnd, OBJID_CLIENT, CHILDID_SELF,
            PROPID_ACC_NAME, wn);
        free(wn);
    }
    long r = w32_msaa_role(role);
    if (r) {
        VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = r;
        svc->lpVtbl->SetHwndProp(svc, hwnd, OBJID_CLIENT, CHILDID_SELF,
            PROPID_ACC_ROLE, v);
    }
    svc->lpVtbl->Release(svc);
}

void aether_ui_a11y_set_role_impl(int handle, const char* role) {
    Widget* w = widget_at(handle);
    if (!w) return;
    free(w->a11y_role);
    w->a11y_role = a11y_dup(role);
    w32_annotate(w->hwnd, w->a11y_name, w->a11y_role);
}

void aether_ui_a11y_set_label_impl(int handle, const char* name) {
    Widget* w = widget_at(handle);
    if (!w) return;
    free(w->a11y_name);
    w->a11y_name = a11y_dup(name);
    w32_annotate(w->hwnd, w->a11y_name, w->a11y_role);
}

void aether_ui_a11y_set_description_impl(int handle, const char* desc) {
    Widget* w = widget_at(handle);
    if (!w) return;
    free(w->a11y_desc);
    w->a11y_desc = a11y_dup(desc);
}

// Map a widget kind to its default (auto) accessible role — what MSAA already
// exposes for standard controls, reported when the author set none.
static const char* w32_auto_role(WidgetKind k) {
    switch (k) {
        case WK_BUTTON:      return "button";
        case WK_TOGGLE:      return "checkbox";
        case WK_TEXTFIELD:   return "textbox";
        case WK_SLIDER:      return "slider";
        case WK_PROGRESSBAR: return "progressbar";
        // WK_TEXT is a plain static label — its auto role stays unset (empty)
        // rather than "heading", which is only correct for a11y_role-tagged
        // headings. So the driver reports "" until an author tags it.
        default:             return "";
    }
}

// Driver readback: effective role/name/description (auto when unset). Name
// falls back to the control's own window text (a button's label = its name).
void aether_ui_a11y_get_impl(int handle,
                             char* role, int rolesz,
                             char* name, int namesz,
                             char* desc, int descsz) {
    if (role && rolesz) role[0] = '\0';
    if (name && namesz) name[0] = '\0';
    if (desc && descsz) desc[0] = '\0';
    Widget* w = widget_at(handle);
    if (!w) return;

    if (role && rolesz) {
        const char* r = w->a11y_role ? w->a11y_role : w32_auto_role(w->kind);
        strncpy(role, r ? r : "", rolesz - 1); role[rolesz - 1] = '\0';
    }
    if (name && namesz) {
        if (w->a11y_name) {
            strncpy(name, w->a11y_name, namesz - 1); name[namesz - 1] = '\0';
        } else {
            // The control's own text is its auto accessible name.
            int len = GetWindowTextLengthW(w->hwnd);
            if (len > 0) {
                wchar_t* wbuf = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
                GetWindowTextW(w->hwnd, wbuf, len + 1);
                const char* u = wide_to_utf8(wbuf);
                strncpy(name, u ? u : "", namesz - 1); name[namesz - 1] = '\0';
                free(wbuf);
            }
        }
    }
    if (desc && descsz && w->a11y_desc) {
        strncpy(desc, w->a11y_desc, descsz - 1); desc[descsz - 1] = '\0';
    }
}

void aether_ui_set_distribution(int handle, int distribution) {
    Widget* w = widget_at(handle);
    if (w) w->stack.distribution = distribution;
}

void aether_ui_set_alignment(int handle, int alignment) {
    Widget* w = widget_at(handle);
    if (w) w->stack.alignment = alignment;
}

void aether_ui_match_parent_width(int handle) {
    Widget* w = widget_at(handle);
    if (w) w->pref_width = -1; // marker: fill parent
}

void aether_ui_match_parent_height(int handle) {
    Widget* w = widget_at(handle);
    if (w) w->pref_height = -1;
}

void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->margin_top = top;
    w->margin_right = right;
    w->margin_bottom = bottom;
    w->margin_left = left;
}

void aether_ui_set_onclick_ctx_style_apply(void* ctx, double r, double g, double b, double a) {
    (void)ctx; (void)r; (void)g; (void)b; (void)a;
}
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aether_ui_set_bg_color((int)(intptr_t)ctx, r, g, b, a);
}
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aether_ui_set_margin((int)(intptr_t)ctx, top, right, bottom, left);
}
void aether_ui_enable_test_server_ctx(int port, void* ctx) {
    aether_ui_enable_test_server_impl(port, (int)(intptr_t)ctx);
}
void aether_ui_set_text_color_ctx(void* ctx, double r, double g, double b) {
    aether_ui_set_text_color((int)(intptr_t)ctx, r, g, b);
}
void aether_ui_set_font_size_ctx(void* ctx, double size) {
    aether_ui_set_font_size((int)(intptr_t)ctx, size);
}
void aether_ui_set_font_bold_ctx(void* ctx, int bold) {
    aether_ui_set_font_bold((int)(intptr_t)ctx, bold);
}
void aether_ui_set_corner_radius_ctx(void* ctx, double radius) {
    aether_ui_set_corner_radius((int)(intptr_t)ctx, radius);
}
void aether_ui_set_opacity_ctx(void* ctx, double opacity) {
    aether_ui_set_opacity((int)(intptr_t)ctx, opacity);
}
void aether_ui_set_enabled_ctx(void* ctx, int enabled) {
    aether_ui_set_enabled((int)(intptr_t)ctx, enabled);
}
void aether_ui_set_tooltip_ctx(void* ctx, const char* text) {
    aether_ui_set_tooltip((int)(intptr_t)ctx, text);
}

// ---------------------------------------------------------------------------
// Events (hover, double-click, click-on-arbitrary-widget).
// ---------------------------------------------------------------------------
void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (w) w->on_click = (AeClosure*)boxed_closure;
}

void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (w) w->on_hover = (AeClosure*)boxed_closure;
}

void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (w) w->on_double_click = (AeClosure*)boxed_closure;
}

int aether_ui_fire_double_click(int handle) {
    Widget* w = widget_at(handle);
    if (!w || !w->on_double_click || !w->on_double_click->fn) return 0;
    invoke_closure(w->on_double_click);
    return 1;
}

// Row drag-reorder. Native OLE drag on win32 is follow-up; for now store the
// drop closure so the reorder works via the driver's /widget/{id}/drop (the
// model reorder is shared). (void)index — the row's index is captured in the
// Aether closure the DSL passes.
void aether_ui_row_drag_reorder_impl(int row_handle, int index,
                                     void* on_drop_closure) {
    (void)index;
    Widget* w = widget_at(row_handle);
    if (w) w->on_drop = (AeClosure*)on_drop_closure;
}

int aether_ui_fire_row_drop(int row_handle, int src_index) {
    Widget* w = widget_at(row_handle);
    if (!w || !w->on_drop || !w->on_drop->fn) return 0;
    ((void(*)(void*, intptr_t))w->on_drop->fn)(w->on_drop->env, (intptr_t)src_index);
    return 1;
}

// vlist native scroll — store the on_scroll(dy) closure on the container. Real
// WM_MOUSEWHEEL is handled in stack_wnd_proc (which fires this closure); the
// driver drives a step headlessly via aether_ui_fire_scroll.
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) {
    Widget* w = widget_at(container_handle);
    if (w) w->on_scroll = (AeClosure*)on_scroll;
}

int aether_ui_fire_scroll(int container_handle, int dy) {
    Widget* w = widget_at(container_handle);
    if (!w || !w->on_scroll || !w->on_scroll->fn) return 0;
    ((void(*)(void*, intptr_t))w->on_scroll->fn)(w->on_scroll->env, (intptr_t)dy);
    return 1;
}

// ---------------------------------------------------------------------------
// System services: alert, file open, clipboard, timer, open URL, dark mode.
// ---------------------------------------------------------------------------
void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) return;  // MessageBox would block indefinitely
    ensure_win_init();
    MessageBoxW(NULL, utf8_to_wide(message), utf8_to_wide(title),
                MB_OK | MB_ICONINFORMATION);
}

static int CALLBACK w32_browse_init_cb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data) {
    (void)lp;
    if (msg == BFFM_INITIALIZED && data) {
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}

char* aether_ui_file_open(const char* title, const char* start_dir) {
    if (aeui_is_headless()) return strdup("");  // modal would block on CI
    ensure_win_init();
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"All Files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = utf8_to_wide(title ? title : "Open File");
    if (start_dir && *start_dir) ofn.lpstrInitialDir = utf8_to_wide(start_dir);
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        return strdup(wide_to_utf8(file));
    }
    return strdup("");
}

// Folder picker. SHBrowseForFolderW rather than IFileDialog/FOS_PICKFOLDERS:
// this file speaks the same generation of the API everywhere else
// (GetOpenFileNameW / GetSaveFileNameW), shlobj.h is already included, and it
// needs no COM vtable plumbing to stay readable in C. BIF_NEWDIALOGSTYLE is
// what gives it the resizable modern chooser with a New Folder button.
char* aether_ui_file_pick_folder(const char* title, const char* start_dir) {
    if (aeui_is_headless()) return strdup("");
    ensure_win_init();
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = utf8_to_wide(title ? title : "Choose Folder");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    // The start folder arrives through the callback: BROWSEINFO has no
    // "initial directory" field, BFFM_SETSELECTION on init is the documented
    // way. lParam carries the wide path.
    wchar_t* wstart = (start_dir && *start_dir) ? utf8_to_wide(start_dir) : NULL;
    if (wstart) {
        bi.lpfn = w32_browse_init_cb;
        bi.lParam = (LPARAM)wstart;
    }
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return strdup("");
    wchar_t path[MAX_PATH] = L"";
    int got = SHGetPathFromIDListW(idl, path);
    CoTaskMemFree(idl);
    if (!got) return strdup("");
    return strdup(wide_to_utf8(path));
}

char* aether_ui_file_save(const char* title, const char* default_name) {
    if (aeui_is_headless()) return strdup("");
    ensure_win_init();
    wchar_t file[1024] = L"";
    if (default_name && *default_name) {
        wchar_t* wn = utf8_to_wide(default_name);
        wcsncpy(file, wn, 1023);
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"All Files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = utf8_to_wide(title ? title : "Save File");
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) {
        return strdup(wide_to_utf8(file));
    }
    return strdup("");
}

void aether_ui_clipboard_write_impl(const char* text) {
    if (!text) return;
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    wchar_t* wide = utf8_to_wide(text);
    size_t len = wcslen(wide) + 1;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(wchar_t));
    if (mem) {
        wchar_t* ptr = (wchar_t*)GlobalLock(mem);
        memcpy(ptr, wide, len * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

char* aether_ui_clipboard_read_impl(void) {
    if (!OpenClipboard(NULL)) return _strdup("");
    char* out = NULL;
    HANDLE mem = GetClipboardData(CF_UNICODETEXT);
    if (mem) {
        wchar_t* wide = (wchar_t*)GlobalLock(mem);
        if (wide) {
            /* wide_to_utf8 hands back a rotating static buffer, so the copy
             * has to happen before the lock is dropped and before any other
             * conversion can reuse the slot. */
            out = _strdup(wide_to_utf8(wide));
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
    return out ? out : _strdup("");
}

// Timer: each user timer is keyed by (hwnd, id). We use the first live app
// window as the timer host — but ui.timer is usually called INSIDE the
// window block, BEFORE app_run creates the hwnd. SetTimer(NULL, id, …)
// then runs as a THREAD timer and Windows IGNORES the passed id,
// assigning its own — so a callback matching on our id never fires (gp's
// whole 30Hz heartbeat was dead on win32). Track the SYSTEM id + host.
typedef struct {
    int id;
    UINT_PTR sys_id;   // what WM_TIMER actually reports
    HWND host;         // NULL = thread timer
    AeClosure* closure;
    int alive;
} TimerEntry;
static TimerEntry* timers = NULL;
static int timer_count = 0;
static int timer_capacity = 0;
static int next_timer_id = 100;

static void CALLBACK timer_cb(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < timer_count; i++) {
        if (timers[i].alive && timers[i].sys_id == id) {
            invoke_closure(timers[i].closure);
            return;
        }
    }
}

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (timer_count >= timer_capacity) {
        timer_capacity = timer_capacity == 0 ? 16 : timer_capacity * 2;
        timers = (TimerEntry*)realloc(timers, sizeof(TimerEntry) * timer_capacity);
    }
    int id = next_timer_id++;
    HWND host = (app_count > 0) ? apps[0].hwnd : NULL;
    UINT_PTR sys_id = SetTimer(host, id, interval_ms, (TIMERPROC)timer_cb);
    timers[timer_count].id = id;
    // With a window host SetTimer returns the id we passed; with a NULL
    // host it returns the system-assigned thread-timer id.
    timers[timer_count].sys_id = host ? (UINT_PTR)id : sys_id;
    timers[timer_count].host = host;
    timers[timer_count].closure = (AeClosure*)boxed_closure;
    timers[timer_count].alive = 1;
    timer_count++;
    return id;
}

void aether_ui_timer_cancel_impl(int timer_id) {
    for (int i = 0; i < timer_count; i++) {
        if (timers[i].id == timer_id) {
            timers[i].alive = 0;
            KillTimer(timers[i].host, timers[i].sys_id);
            return;
        }
    }
}

void aether_ui_open_url_impl(const char* url) {
    ShellExecuteW(NULL, L"open", utf8_to_wide(url), NULL, NULL, SW_SHOWNORMAL);
}

int aether_ui_dark_mode_check(void) {
    // Driver override first (POST /appearance?dark=N — headless spec steer).
    int ov = aether_ui_appearance_override_get();
    if (ov >= 0) return ov;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    DWORD val = 1, sz = sizeof(val);
    int dark = 0;
    if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL,
        (LPBYTE)&val, &sz) == ERROR_SUCCESS) dark = (val == 0);
    RegCloseKey(key);
    return dark;
}

// ---------------------------------------------------------------------------
// Secondary windows + sheets.
// ---------------------------------------------------------------------------
int aether_ui_window_create_impl(const char* title, int width, int height) {
    ensure_win_init();
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(width, dpi, 96);
    int h = MulDiv(height, dpi, 96);
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, APP_CLASS, utf8_to_wide(title),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 0;
    apply_window_theme(hwnd);
    w32_window_register(hwnd, title);   // extra → driver window handle 2..
    return register_widget_typed(hwnd, WK_WINDOW);
}

// ── Unified driver window view (1 = primary, 2.. = extras) ──
int aether_ui_window_count_impl(void) { return w32_window_count; }
const char* aether_ui_window_title_impl(int win_handle) {
    if (win_handle < 1 || win_handle > w32_window_count) return "";
    return w32_windows[win_handle - 1].title;
}

void aether_ui_window_set_title_impl(int win_handle, const char* title) {
    if (win_handle < 1 || win_handle > w32_window_count) return;
    W32Window* e = &w32_windows[win_handle - 1];
    const char* t = title ? title : "";
    char* copy = _strdup(t);
    if (!copy) return;
    free(e->title);
    e->title = copy;
    if (e->hwnd) SetWindowTextW(e->hwnd, utf8_to_wide(t));
}
int aether_ui_window_is_open_impl(int win_handle) {
    if (win_handle < 1 || win_handle > w32_window_count) return 0;
    return w32_windows[win_handle - 1].live;
}
int aether_ui_widget_window_impl(int widget_handle) {
    Widget* w = widget_at(widget_handle);
    if (!w || !w->hwnd) return 0;
    HWND root = GetAncestor(w->hwnd, GA_ROOT);
    for (int i = 0; i < w32_window_count; i++)
        if (w32_windows[i].hwnd == root) return i + 1;
    return 0;
}
void aether_ui_close_window_by_handle_impl(int win_handle) {
    if (win_handle < 1 || win_handle > w32_window_count) return;
    if (!w32_windows[win_handle - 1].live) return;
    // This may be called from the HTTP server thread; DestroyWindow must run on
    // the window's OWNING (UI) thread, so post WM_CLOSE (async, thread-safe) —
    // the WndProc then DestroyWindow's it and WM_DESTROY marks it dead.
    PostMessageW(w32_windows[win_handle - 1].hwnd, WM_CLOSE, 0, 0);
}

void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    Widget* win = widget_at(win_handle);
    Widget* root = widget_at(root_handle);
    if (!win || !root) return;
    SetParent(root->hwnd, win->hwnd);
    LONG_PTR st = GetWindowLongPtrW(root->hwnd, GWL_STYLE);
    SetWindowLongPtrW(root->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    RECT cr;
    GetClientRect(win->hwnd, &cr);
    SetWindowPos(root->hwnd, NULL, 0, 0,
                 cr.right - cr.left, cr.bottom - cr.top,
                 SWP_NOZORDER | SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------------------
// In-window overlay layer (roadmap item 1, win32 parity) — toast / modal /
// tooltip, drawn INSIDE the app window as raised child HWNDs, never as a
// popup. Mirrors the AppKit translation (aether_ui_macos.m): the overlay
// table is append-only, handles 1-based and monotonic, closing flips `live`
// to 0 without removing the entry — GET /overlays must show a toast as
// observably DEAD, not merely absent.
//
// The "host" is simply the app window's client area: child windows z-order
// over siblings, so a raised scrim + content IS the overlay stack. The
// scrim is a WS_EX_LAYERED child (uniform alpha works for child windows
// since Win8) that swallows clicks; a scrim click is the ONLY path that
// fires on_dismiss (GTK/macOS behave the same).
// ---------------------------------------------------------------------------
typedef struct {
    HWND content;        // reparented into the app window while live
    HWND scrim;          // NULL when non-modal
    AeClosure* on_dismiss;
    int modal;
    int live;
    // Per-entry exit transition (transition_overlay). trans_ms > 0 + a kind =>
    // close plays an alpha fade over trans_ms before the real detach; exiting
    // holds 1 during it. fade_* track the running animation.
    char* trans_kind;
    /* Start geometry of the exit tween, captured on the first timer tick so
       slide/scale interpolate from where the overlay actually is. */
    int tr_x, tr_y, tr_w, tr_h;
    int trans_ms;
    int exiting;
    int exit_played;   // sticky: an exit tween STARTED (survives the entry's death)
    UINT_PTR fade_timer;  // system-assigned WM_TIMER id while fading (0 = none)
    DWORD fade_start;      // GetTickCount at fade start
    // Scrim material (overlay_material). win32 can't blur behind a child HWND,
    // so "blur" degrades to "tint": a heavier, lighter-frost scrim. Effective:
    // "dim" | "tint" (never "blur"). NULL = "dim".
    char* material;
} Win32OverlayEntry;

static Win32OverlayEntry* w32_overlays = NULL;
static int w32_overlay_count = 0;
static int w32_overlay_capacity = 0;

static Win32OverlayEntry* w32_overlay_at(int handle) {
    if (handle < 1 || handle > w32_overlay_count) return NULL;
    return &w32_overlays[handle - 1];
}

// Natural size of a (possibly detached) widget subtree. measure_widget
// answers leaves (text extent, pref sizes); stacks sum/max their children —
// a detached modal card has never been laid out, so its own rect is 0x0
// and only recursion gives an honest size.
static void measure_subtree(HWND hwnd, int* out_w, int* out_h) {
    int handle = handle_for_hwnd(hwnd);
    Widget* w = handle ? widget_at(handle) : NULL;
    if (!w) { *out_w = 0; *out_h = 0; return; }
    if (w->kind == WK_VSTACK || w->kind == WK_HSTACK || w->kind == WK_ZSTACK) {
        int total_w = 0, total_h = 0, n = 0;
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            int cw = 0, ch = 0;
            measure_subtree(c, &cw, &ch);
            if (w->kind == WK_VSTACK) {
                if (cw > total_w) total_w = cw;
                total_h += ch;
            } else if (w->kind == WK_HSTACK) {
                total_w += cw;
                if (ch > total_h) total_h = ch;
            } else {
                if (cw > total_w) total_w = cw;
                if (ch > total_h) total_h = ch;
            }
            n++;
        }
        int sp = (n > 1) ? (n - 1) * w->stack.spacing : 0;
        if (w->kind == WK_VSTACK) total_h += sp;
        if (w->kind == WK_HSTACK) total_w += sp;
        // Stack margins apply during parent layout; include our own padding.
        *out_w = total_w + w->margin_left + w->margin_right + 16;
        *out_h = total_h + w->margin_top + w->margin_bottom + 16;
        return;
    }
    measure_widget(w, out_w, out_h);
    // Leaves get breathing room the win32 text extent doesn't include.
    *out_w += 12;
    *out_h += 8;
}

static LRESULT CALLBACK scrim_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) {
        // Find the overlay owning this scrim; fire on_dismiss then close.
        for (int i = 0; i < w32_overlay_count; i++) {
            Win32OverlayEntry* e = &w32_overlays[i];
            if (e->live && e->scrim == hwnd) {
                if (e->on_dismiss && e->on_dismiss->fn) {
                    ((void(*)(void*))e->on_dismiss->fn)(e->on_dismiss->env);
                }
                aether_ui_overlay_close_impl(i + 1);
                return 0;
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Animation globally suppressed (driver determinism switch)?
static int w32_anim_off(void) {
    const char* n = getenv("AETHER_UI_NO_ANIMATION");
    return n && n[0] && n[0] != '0';
}

// Make a child HWND layered so SetLayeredWindowAttributes can fade its alpha.
static void w32_make_layered(HWND h) {
    if (!h) return;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
}

// Detach an overlay entry's windows for real (end state of a close, whether
// instant or after the fade). Mirrors the old close body.
static void w32_overlay_detach(Win32OverlayEntry* e) {
    /* mark_subtree_dead BEFORE DestroyWindow, the order clear_children,
       remove_child and navstack_pop all document: afterwards GetWindow can no
       longer enumerate the children to mark them, and because Windows recycles
       HWND values IsWindow() cannot tell a destroyed overlay from a live
       widget that inherited its handle. The scrim was already being destroyed
       here WITHOUT the marking step.

       The content was neither marked nor destroyed, only hidden and reparented
       onto the holder, so every dialog ever opened stayed alive as real
       windows and kept reporting live types to the driver. Destroying matches
       GTK4, where removing the overlay drops the last ref and finalizes the
       content -- closing an overlay consumes its content on every backend. */
    if (e->scrim) {
        mark_subtree_dead(e->scrim);
        DestroyWindow(e->scrim);
        e->scrim = NULL;
    }
    if (e->content) {
        mark_subtree_dead(e->content);
        DestroyWindow(e->content);
        e->content = NULL;
    }
    e->exiting = 0;
    e->live = 0;
}

// WM_TIMER proc stepping an overlay's exit fade (content + scrim alpha) from
// 255→0 over trans_ms, then detaching. One shared proc; matches on sys_id.
static void CALLBACK w32_overlay_fade_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < w32_overlay_count; i++) {
        Win32OverlayEntry* e = &w32_overlays[i];
        if (e->fade_timer != id) continue;
        DWORD elapsed = GetTickCount() - e->fade_start;
        int ms = e->trans_ms > 0 ? e->trans_ms : 180;
        if (elapsed >= (DWORD)ms) {
            KillTimer(NULL, id);
            e->fade_timer = 0;
            w32_overlay_detach(e);
            return;
        }
        // remaining fraction 1.0 → 0.0 as elapsed → ms
        int rem = (int)(ms - elapsed);
        if (e->content)
            SetLayeredWindowAttributes(e->content, 0,
                (BYTE)((255 * rem) / ms), LWA_ALPHA);
        /* THE KIND, not just the duration. This branched only on the kind
           being non-empty, so every transition_overlay() exit was a fade --
           "slide-up", "slide-down" and "scale" all silently rendered as one.
           GTK4 has always distinguished the four (see transition_css_class
           there); macOS discarded the kind entirely. One string, three
           interpretations, and the reference backend looked right so nothing
           flagged it.

           Geometry alongside the alpha: slide moves the content window,
           scale shrinks it about its centre. Both ride the same tween clock,
           so a kind that is unrecognised still fades exactly as before. */
        if (e->content && e->trans_kind) {
            RECT r;
            if (GetWindowRect(e->content, &r)) {
                int w = r.right - r.left, h = r.bottom - r.top;
                if (e->tr_w == 0) { e->tr_x = r.left; e->tr_y = r.top;
                                    e->tr_w = w; e->tr_h = h; }
                int done = (int)elapsed;
                if (strcmp(e->trans_kind, "slide-up") == 0) {
                    int dy = (e->tr_h * done) / ms;
                    SetWindowPos(e->content, NULL, e->tr_x, e->tr_y - dy, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else if (strcmp(e->trans_kind, "slide-down") == 0) {
                    int dy = (e->tr_h * done) / ms;
                    SetWindowPos(e->content, NULL, e->tr_x, e->tr_y + dy, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else if (strcmp(e->trans_kind, "scale") == 0) {
                    int nw = (e->tr_w * rem) / ms, nh = (e->tr_h * rem) / ms;
                    if (nw < 1) nw = 1;
                    if (nh < 1) nh = 1;
                    SetWindowPos(e->content, NULL,
                                 e->tr_x + (e->tr_w - nw) / 2,
                                 e->tr_y + (e->tr_h - nh) / 2, nw, nh,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        // Fade the scrim from its ~45% start (115) proportionally.
        if (e->scrim)
            SetLayeredWindowAttributes(e->scrim, 0,
                (BYTE)((115 * rem) / ms), LWA_ALPHA);
        return;
    }
    KillTimer(NULL, id);
}

// Resolve an overlay's host window HWND. win_handle 0 = the primary app
// window; otherwise the WK_WINDOW widget handle a window_create() returned (a
// secondary window), so a secondary window's overlays land in IT, not the
// primary. Falls back to the content widget's own top-level, then the primary.
static HWND aeui_overlay_host_hwnd(int win_handle, Widget* content) {
    if (win_handle > 0) {
        Widget* w = widget_at(win_handle);
        if (w && w->kind == WK_WINDOW && w->hwnd) return w->hwnd;
    }
    if (content && content->hwnd) {
        HWND root = GetAncestor(content->hwnd, GA_ROOT);
        // A DETACHED content (root_vstack built for this modal) roots at the
        // hidden 0x0 widget_holder — mounting the overlay THERE put the whole
        // modal (scrim included) in an invisible parking lot: live-flags and
        // closures worked, geometry and picks read zeros. Never accept the
        // holder as a host.
        if (root && root != widget_holder) return root;
    }
    return (app_count > 0) ? apps[0].hwnd : NULL;
}

int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal) {
    ensure_win_init();
    Widget* content = widget_at(content_handle);
    if (!content) return 0;
    HWND host = aeui_overlay_host_hwnd(win_handle, content);
    if (!host) return 0;

    if (w32_overlay_count >= w32_overlay_capacity) {
        w32_overlay_capacity = w32_overlay_capacity == 0 ? 8 : w32_overlay_capacity * 2;
        w32_overlays = (Win32OverlayEntry*)realloc(
            w32_overlays, sizeof(Win32OverlayEntry) * w32_overlay_capacity);
        if (!w32_overlays) { w32_overlay_count = 0; w32_overlay_capacity = 0; return 0; }
    }
    int handle = w32_overlay_count + 1;
    Win32OverlayEntry* e = &w32_overlays[w32_overlay_count];
    memset(e, 0, sizeof(*e));
    e->modal = modal ? 1 : 0;
    e->live = 1;

    RECT cr;
    GetClientRect(host, &cr);
    int W = cr.right - cr.left, H = cr.bottom - cr.top;

    // Scrim first so it sits BELOW the content in z-order.
    if (modal) {
        HWND scrim = CreateWindowExW(WS_EX_LAYERED, SCRIM_CLASS, L"",
            WS_CHILD | WS_VISIBLE, 0, 0, W, H,
            host, NULL, GetModuleHandleW(NULL), NULL);
        SetLayeredWindowAttributes(scrim, 0, 115, LWA_ALPHA);  // ~45% black
        register_widget_typed(scrim, WK_SCRIM);
        SetWindowPos(scrim, HWND_TOP, 0, 0, W, H, SWP_SHOWWINDOW);
        e->scrim = scrim;
    }

    // Reparent the content into the window, size it naturally, place by
    // anchor. anchor packs halign in bits 0-1 (0=start 1=center 2=end) and
    // valign in bits 2-3: code = h + v*4; dx/dy are SIGNED insets (positive
    // from start/top, negative from end/bottom).
    int cw = 0, ch = 0;
    measure_subtree(content->hwnd, &cw, &ch);
    if (cw < 40) cw = 40;
    if (ch < 24) ch = 24;
    if (cw > W) cw = W;
    if (ch > H) ch = H;
    int h = anchor & 3, v = (anchor >> 2) & 3;
    int x, y;
    if (h == 0)      x = dx > 0 ? dx : 0;
    else if (h == 2) x = W - cw + (dx < 0 ? dx : 0);
    else             x = (W - cw) / 2 + dx;
    if (v == 0)      y = dy > 0 ? dy : 0;
    else if (v == 2) y = H - ch + (dy < 0 ? dy : 0);
    else             y = (H - ch) / 2 + dy;

    // Preserve the content's OWN visibility (its WS_VISIBLE bit — the same
    // flag hook_widget_visible reports) through the promotion. An app may
    // build overlay content hidden and show it on a later interaction
    // (auto_hide_demo's click-to-open sidebar); SWP_SHOWWINDOW here set
    // WS_VISIBLE unconditionally, so such content arrived in the overlay
    // layer force-shown — "sidebar starts hidden" read visible:1 on win32
    // while GTK4's promotion preserves gtk_widget_get_visible.
    int was_visible =
        (GetWindowLongPtrW(content->hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
    SetParent(content->hwnd, host);
    SetWindowPos(content->hwnd, HWND_TOP, x, y, cw, ch,
                 was_visible ? SWP_SHOWWINDOW : SWP_NOACTIVATE);
    e->content = content->hwnd;
    w32_overlay_count++;
    return handle;
}

void aether_ui_overlay_close_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    if (!e || !e->live || e->exiting) return;   // idempotent / not twice
    // A per-entry exit transition (and animation on): fade the content+scrim
    // alpha to 0 over trans_ms, holding exiting:1, then detach in the timer.
    // Everything else detaches immediately (same end-state as before).
    if (e->trans_kind && e->trans_ms > 0 && !w32_anim_off() && e->content) {
        w32_make_layered(e->content);
        SetLayeredWindowAttributes(e->content, 0, 255, LWA_ALPHA);
        e->exiting = 1;
        e->exit_played = 1;
        e->fade_start = GetTickCount();
        // ~60fps steps; the proc computes exact alpha from elapsed time.
        e->fade_timer = SetTimer(NULL, 0, 16, w32_overlay_fade_proc);
        return;
    }
    w32_overlay_detach(e);
}

void aether_ui_overlay_set_on_dismiss_impl(int overlay_handle, void* boxed_closure) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    if (e) e->on_dismiss = (AeClosure*)boxed_closure;
}

int aether_ui_overlay_is_live_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return e ? e->live : 0;
}

int aether_ui_overlay_count_impl(void) { return w32_overlay_count; }

int aether_ui_overlay_is_modal_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return e ? e->modal : 0;
}

// Per-entry overlay transitions. win32 has no CSS keyframes, so the exit is a
// layered-window alpha fade over trans_ms (see aether_ui_overlay_close_impl +
// w32_overlay_fade_proc). We store the kind for parity with GTK4's vocabulary,
// but every kind renders as the fade (a real slide is a follow-up). Enter is
// left instant — matching the app's expectation that content appears on open.
void aether_ui_overlay_set_transition_impl(int overlay_handle,
                                           const char* kind, int ms) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    if (!e) return;
    free(e->trans_kind);
    e->trans_kind = (kind && *kind) ? _strdup(kind) : NULL;
    e->trans_ms = ms;
}

int aether_ui_overlay_is_exiting_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return e ? e->exiting : 0;
}

int aether_ui_overlay_exit_played_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return e ? e->exit_played : 0;
}

// Scrim material. win32 can't blur behind a child HWND, so "blur" degrades to
// "tint": a heavier, lighter-frost scrim (raise the layered alpha + a light
// tint color). "dim"/"" keeps the default. Records the EFFECTIVE material.
void aether_ui_overlay_set_material_impl(int overlay_handle, const char* kind) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    if (!e) return;
    const char* eff = "dim";
    if (kind && (strcmp(kind, "blur") == 0 || strcmp(kind, "tint") == 0))
        eff = "tint";   // no in-window blur on win32 — honest degrade
    free(e->material);
    e->material = _strdup(eff);
    if (e->scrim) {
        if (strcmp(eff, "tint") == 0)
            // Heavier, lighter frost: ~78% opacity over a light tint key.
            SetLayeredWindowAttributes(e->scrim, RGB(240, 240, 245), 200, LWA_ALPHA);
        else
            SetLayeredWindowAttributes(e->scrim, 0, 115, LWA_ALPHA);  // default dim
    }
}

const char* aether_ui_overlay_material_effective_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return (e && e->material) ? e->material : "dim";
}

// Escape closes the TOPMOST live overlay; returns 1 if one was closed so
// the caller can let Escape propagate when nothing is open.
static int w32_escape_overlays(void) {
    for (int i = w32_overlay_count - 1; i >= 0; i--) {
        if (w32_overlays[i].live) {
            aether_ui_overlay_close_impl(i + 1);
            return 1;
        }
    }
    return 0;
}

// Toast auto-dismiss: sys-timer-id → overlay handle (thread timers get a
// SYSTEM-assigned id — the ui.timer lesson).
typedef struct { UINT_PTR sys_id; int overlay; } ToastTimer;
static ToastTimer toast_timers[16];

static void CALLBACK toast_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < 16; i++) {
        if (toast_timers[i].sys_id == id && toast_timers[i].overlay) {
            KillTimer(NULL, id);
            aether_ui_overlay_close_impl(toast_timers[i].overlay);
            toast_timers[i].sys_id = 0;
            toast_timers[i].overlay = 0;
            return;
        }
    }
    KillTimer(NULL, id);
}

int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    // A real registered text widget, opened bottom-center, non-modal,
    // lifted 24px off the edge (anchor 9 = h:1 center, v:2 bottom).
    int content = aether_ui_text_create(text ? text : "");
    int handle = aether_ui_overlay_open_impl(win_handle, content, 9, 0, -24, 0);
    if (handle > 0 && ms > 0) {
        UINT_PTR sid = SetTimer(NULL, 0, (UINT)ms, toast_timer_proc);
        for (int i = 0; i < 16; i++) {
            if (toast_timers[i].sys_id == 0) {
                toast_timers[i].sys_id = sid;
                toast_timers[i].overlay = handle;
                break;
            }
        }
    }
    return handle;
}
// Raw-CSS is a GTK concept; win32 honours the ONE property AeCS routes here:
// "opacity: X;" becomes a real layered-window alpha (the same WS_EX_LAYERED
// child mechanism the overlay exit fade proved). Everything else is ignored.
/* Drives one widget's opacity tween. ~60fps, same shape as the overlay fade
   proc above. The EASE-OUT curve is 1-(1-t)^2: fastest at the start, which is
   what makes it distinguishable from linear at the midpoint (a linear tween
   is ~50% done there, ease-out ~75%) -- see
   tests/transitions_demo/test_easing_curve.sh, which reads the curve out of
   the framebuffer because no property readback can see it. */
/* Damped-spring progress: a QML-SpringAnimation-alike shape, as a closed form
   rather than an integrated physics step (a timer-driven tween needs a
   function of t, not a stateful simulation).

       p(t) = 1 - e^(-6t) * cos(9t)

   The exponential is the damping envelope and the cosine is the oscillation,
   so p RISES PAST 1 near t≈0.35 and rings back down -- the overshoot is the
   whole point, and is what no CSS cubic-bezier and no ease-out can express.
   Constants chosen so the first overshoot PEAKS AT ~15% ABOVE the target at
   t=0.30, rings back under by t=0.55, and is within 0.2% of 1.0 from t=0.85
   -- visible enough that a pixel test can see it, settled enough that the end
   state is not in doubt.

   p(0) = 0 and p(1) is forced to exactly 1.0 by the caller's clamp, so a
   tween still lands precisely on its target however the maths rounds. */
static double w32_spring_progress(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return 1.0 - exp(-6.0 * t) * cos(9.0 * t);
}

static void CALLBACK w32_opacity_tween_proc(HWND hwnd, UINT msg,
                                            UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 1; i <= widget_count; i++) {
        Widget* w = widget_at(i);
        if (!w || w->tr_timer != id) continue;
        int ms = w->tr_ms > 0 ? w->tr_ms : 1;
        DWORD elapsed = GetTickCount() - w->tr_start;
        double t = (double)elapsed / (double)ms;
        if (t >= 1.0) t = 1.0;
        double e;
        if (w->tr_spring) e = w32_spring_progress(t);
        else e = w->tr_ease_out ? (1.0 - (1.0 - t) * (1.0 - t)) : t;
        double v = w->tr_from + (w->tr_to - w->tr_from) * e;
        if (IsWindow(w->hwnd)) {
            w32_make_layered(w->hwnd);
            SetLayeredWindowAttributes(w->hwnd, 0, (BYTE)(v * 255.0), LWA_ALPHA);
        }
        if (t >= 1.0) {
            KillTimer(NULL, id);
            w->tr_timer = 0;
        }
        return;
    }
    KillTimer(NULL, id);
}

void aether_ui_widget_apply_css_impl(int handle, const char* property_css) {
    if (!property_css) return;
    Widget* w = widget_at(handle);
    if (!w) return;
    /* "transition: opacity 1200ms ease-out;" -- the DECLARATION, recorded so
       the next opacity change tweens. ui.transition lowers to CSS (GTK4 reads
       it natively), and this backend simply threw it away. */
    const char* tr = strstr(property_css, "transition:");
    if (tr) {
        if (!strstr(tr, "opacity")) return;      // only opacity is wired
        const char* ms_at = strstr(tr, "ms");
        if (!ms_at) return;
        const char* p = ms_at;
        while (p > tr && isdigit((unsigned char)p[-1])) p--;
        int ms = atoi(p);
        if (ms <= 0) return;
        w->tr_ms = ms;
        /* "spring" arrives as a marker comment on the bezier (this backend
           does not parse control points); it overrides the ease flag. */
        w->tr_spring = strstr(tr, "spring") ? 1 : 0;
        w->tr_ease_out = strstr(tr, "linear") ? 0 : 1;
        return;
    }
    if (strncmp(property_css, "opacity:", 8) == 0) {
        double v = atof(property_css + 8);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        double from = (w->styled_opacity_enc > 0)
                      ? (w->styled_opacity_enc - 1) / 100.0 : 1.0;
        /* Record the MODEL value immediately either way: the driver reports
           it, and a tween must not make the readback lag reality. Only the
           presentation is animated -- the same split GTK4 and macOS have. */
        w->styled_opacity_enc = (int)(v * 100.0) + 1;
        if (w->tr_ms > 0 && !w32_anim_off() && from != v) {
            if (w->tr_timer) KillTimer(NULL, w->tr_timer);
            w->tr_from = from;
            w->tr_to = v;
            w->tr_start = GetTickCount();
            w->tr_timer = SetTimer(NULL, 0, 16, w32_opacity_tween_proc);
            return;
        }
        w32_make_layered(w->hwnd);
        SetLayeredWindowAttributes(w->hwnd, 0, (BYTE)(v * 255.0), LWA_ALPHA);
    }
}

int aether_ui_styled_opacity_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w || w->styled_opacity_enc <= 0) return -1;
    return w->styled_opacity_enc - 1;
}

// AeCS appearance change (win32). The OS path is WM_SETTINGCHANGE in
// app_wnd_proc; nothing extra to hook up.
void aether_ui_watch_appearance_impl(void) { }

// Undo/redo driver fire — direct from the HTTP thread (the /drop
// precedent: cross-thread SendMessages tolerate it on win32).
int aether_ui_fire_undo(void) { return aether_ui_undo_step_impl(); }
int aether_ui_fire_redo(void) { return aether_ui_redo_step_impl(); }

int aether_ui_fire_appearance(int dark) {
    aether_ui_appearance_override_set(dark ? 1 : 0);
    // Direct invoke from the HTTP thread — same threading posture as the
    // /drop fire path, which cross-thread SendMessages tolerate.
    return aether_ui_appearance_invoke(dark ? 1 : 0);
}
void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    Widget* w = widget_at(handle);
    if (!w || !cls || !cls[0]) return;
    if (w->classes && strstr(w->classes, cls)) return;  // dedupe (name-safe: our classes don't prefix each other)
    size_t need = (w->classes ? strlen(w->classes) + 1 : 0) + strlen(cls) + 1;
    char* nc = (char*)malloc(need);
    if (w->classes && w->classes[0]) {
        sprintf(nc, "%s %s", w->classes, cls);
    } else {
        strcpy(nc, cls);
    }
    free(w->classes);
    w->classes = nc;
}
void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    Widget* w = widget_at(handle);
    if (!w || !w->classes || !cls || !cls[0]) return;
    char* pos = strstr(w->classes, cls);
    if (!pos) return;
    size_t cl = strlen(cls);
    // Close the gap incl. one separating space on either side.
    char* from = pos + cl;
    if (*from == ' ') from++;
    else if (pos > w->classes && pos[-1] == ' ') pos--;
    memmove(pos, from, strlen(from) + 1);
}
/* Group opacity lives with the other canvas command builders, below the
   CanvasCmd definition — see aether_ui_canvas_group_begin_impl there. */
// Drawn vg tooltip — one overlay per SHOW (a hide flips it dead; the next
// hover opens a fresh one, matching the macOS/GTK observable contract:
// specs assert live:1 on hover, live:0 after moving off, live:1 again).
static int w32_tooltip_overlay = 0;
static int w32_tooltip_label = 0;

int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text,
                                   double cx, double cy) {
    if (!text || !text[0]) { aether_ui_vg_tooltip_hide_impl(); return 0; }
    int cwidget = aether_ui_canvas_get_widget(canvas_id);
    Widget* cw = widget_at(cwidget);
    if (!cw || app_count < 1 || !apps[0].hwnd) return 0;

    // Canvas-local → app-window client coords; sit below-right of the
    // pointer like a native tip.
    POINT p = { (int)cx + 12, (int)cy + 18 };
    MapWindowPoints(cw->hwnd, apps[0].hwnd, &p, 1);

    if (w32_tooltip_overlay
        && aether_ui_overlay_is_live_impl(w32_tooltip_overlay)
        && w32_tooltip_label) {
        // Live: retext + reposition the existing overlay content.
        Widget* lbl = widget_at(w32_tooltip_label);
        if (lbl) {
            aether_ui_text_set_string(w32_tooltip_label, text);
            int tw = 0, th = 0;
            measure_widget(lbl, &tw, &th);
            SetWindowPos(lbl->hwnd, HWND_TOP, p.x, p.y, tw + 12, th + 8,
                         SWP_SHOWWINDOW);
        }
        return w32_tooltip_overlay;
    }
    w32_tooltip_label = aether_ui_text_create(text);
    // anchor 0 = top-start; dx/dy are the absolute client position.
    w32_tooltip_overlay = aether_ui_overlay_open_impl(0, w32_tooltip_label,
                                                      0, p.x, p.y, 0);
    return w32_tooltip_overlay;
}

void aether_ui_vg_tooltip_hide_impl(void) {
    if (w32_tooltip_overlay
        && aether_ui_overlay_is_live_impl(w32_tooltip_overlay)) {
        aether_ui_overlay_close_impl(w32_tooltip_overlay);
    }
}

// The drawn tooltip path is opt-in via $AETHER_UI_TOOLTIP=drawn (the same
// switch GTK honors; win32 has no native vg-shape tooltip to prefer).
int aether_ui_vg_tooltip_drawn_impl(void) {
    const char* v = getenv("AETHER_UI_TOOLTIP");
    return v && strcmp(v, "drawn") == 0;
}

void aether_ui_window_show_impl(int win_handle) {
    Widget* w = widget_at(win_handle);
    if (!w) return;
    if (aeui_is_headless()) return;   // realized but not shown, like the primary
    ShowWindow(w->hwnd, SW_SHOW);
    UpdateWindow(w->hwnd);
}

void aether_ui_window_close_impl(int win_handle) {
    Widget* w = widget_at(win_handle);
    if (w) DestroyWindow(w->hwnd);
}

int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    return aether_ui_window_create_impl(title, width, height);
}

void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    aether_ui_window_set_body_impl(handle, root_handle);
}

void aether_ui_sheet_present_impl(int handle) {
    aether_ui_window_show_impl(handle);
}

void aether_ui_sheet_dismiss_impl(int handle) {
    aether_ui_window_close_impl(handle);
}

// GDI+ flat-API image loaders — used to pick up PNG/JPEG/GIF/BMP/TIFF at
// runtime without pulling in the C++ GDI+ headers. The returned HBITMAP
// owns its pixels and can be passed straight to STM_SETIMAGE.
__declspec(dllimport) int __stdcall GdipCreateBitmapFromFile(
    const wchar_t* filename, void** bitmap);
__declspec(dllimport) int __stdcall GdipCreateHBITMAPFromBitmap(
    void* bitmap, HBITMAP* hbm, unsigned int background_argb);
__declspec(dllimport) int __stdcall GdipDisposeImage(void* image);

// Load any format GDI+ supports (PNG, JPEG, GIF, BMP, TIFF, ICO) into an
// HBITMAP suitable for SS_BITMAP. Falls back to LoadImageW for plain BMPs
// when GDI+ initialization fails or isn't available.
static HBITMAP load_image_any_format(const wchar_t* filename) {
    ensure_gdiplus();
    if (gdiplus_started) {
        void* gdi_bitmap = NULL;
        if (GdipCreateBitmapFromFile(filename, &gdi_bitmap) == 0 && gdi_bitmap) {
            HBITMAP hbm = NULL;
            // Transparent background (0x00000000 = ARGB all zero).
            GdipCreateHBITMAPFromBitmap(gdi_bitmap, &hbm, 0);
            GdipDisposeImage(gdi_bitmap);
            if (hbm) return hbm;
        }
    }
    // BMP-only fallback.
    return (HBITMAP)LoadImageW(NULL, filename, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
}

int aether_ui_image_create(const char* filepath) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_BITMAP,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    HBITMAP bmp = load_image_any_format(utf8_to_wide(filepath));
    if (bmp) SendMessageW(h, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bmp);
    return register_widget_typed(h, WK_IMAGE);
}

// GDI+ decode from an in-memory IStream — same decoders as the file path,
// no temp file. A growable HGlobal stream (ole32, already linked — the same
// primitive the screenshot path uses) is filled with the encoded bytes and
// rewound; GDI+ decodes PNG/JPEG/etc. off it.
__declspec(dllimport) int __stdcall GdipCreateBitmapFromStream(
    void* stream, void** bitmap);

int aether_ui_image_from_bytes(const char* data, int length) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_BITMAP,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    if (data && length > 0) {
        ensure_gdiplus();
        if (gdiplus_started) {
            IStream* stream = NULL;
            if (CreateStreamOnHGlobal(NULL, TRUE, &stream) == S_OK && stream) {
                ULONG wrote = 0;
                stream->lpVtbl->Write(stream, data, (ULONG)length, &wrote);
                LARGE_INTEGER zero; zero.QuadPart = 0;
                stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);
                void* gdi_bitmap = NULL;
                if (GdipCreateBitmapFromStream(stream, &gdi_bitmap) == 0 && gdi_bitmap) {
                    HBITMAP hbm = NULL;
                    GdipCreateHBITMAPFromBitmap(gdi_bitmap, &hbm, 0);
                    GdipDisposeImage(gdi_bitmap);
                    if (hbm) SendMessageW(h, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hbm);
                }
                stream->lpVtbl->Release(stream);
            }
        }
    }
    return register_widget_typed(h, WK_IMAGE);
}

// SHGetFileInfoW with SHGFI_USEFILEATTRIBUTES so a path that does not exist
// still answers: the shell then resolves by extension alone, which is what a
// listing needs before it has stat'ed an entry. The STATIC holds an icon
// rather than a bitmap, so it uses SS_ICON / STM_SETICON.
static HICON w32_icon_for_path(const char* path) {
    if (!path || !*path) return NULL;
    SHFILEINFOW sfi;
    memset(&sfi, 0, sizeof(sfi));
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    DWORD real = GetFileAttributesW(utf8_to_wide(path));
    if (real != INVALID_FILE_ATTRIBUTES) attrs = real;
    if (!SHGetFileInfoW(utf8_to_wide(path), attrs, &sfi, sizeof(sfi), flags)) {
        return NULL;
    }
    return sfi.hIcon;
}

int aether_ui_file_icon_create(const char* path) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ICON,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    HICON ic = w32_icon_for_path(path);
    if (ic) SendMessageW(h, STM_SETICON, (WPARAM)ic, 0);
    return register_widget_typed(h, WK_IMAGE);
}


int aether_ui_image_has_content(int handle) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_IMAGE || !w->hwnd) return 0;
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    /* SS_ICON (0x3) and SS_BITMAP (0xE) are ENUM VALUES in the low style
       bits, not flags: `st & SS_ICON` is truthy for a BITMAP-styled static
       (0xE & 0x3 = 2), which made every TINTED icon (tint switches the
       control to SS_BITMAP) query IMAGE_ICON, get NULL, and report
       has_image:false while its tinted bitmap sat right there. Compare
       under SS_TYPEMASK, the documented way. */
    UINT kind = ((st & SS_TYPEMASK) == SS_ICON) ? IMAGE_ICON : IMAGE_BITMAP;
    return SendMessageW(w->hwnd, STM_GETIMAGE, kind, 0) ? 1 : 0;
}

// A Win32 STATIC scales through one style bit: SS_REALSIZECONTROL stretches
// the bitmap to the control (stretch), and without it the bitmap draws at its
// natural size (original). Contain and cover would each need a StretchBlt into
// a re-scaled bitmap on every WM_SIZE, which is not wired here, so they DEGRADE
// TO ORIGINAL rather than to stretch: distortion is the exact thing a fill mode
// exists to prevent, so silently distorting would be worse than not scaling.
// get_fill reports what was applied, as truncate and overlay material do.
// Tinting a template image: keep the ALPHA SHAPE, replace the colour. That is
// what the other two backends do (contentTintColor over a template image on
// AppKit, the CSS colour a symbolic GIcon paints itself with on GTK4).
//
// The earlier note here said this would mean compositing on every repaint,
// which is what made it look like a different class of feature. It does not:
// a tint changes only when someone sets one, so the recolour happens ONCE and
// the STATIC is handed the result. The original is kept so untinting restores
// it rather than approximating it back.
//
// Returns a new top-down 32bpp DIB, or NULL. The caller owns it.
static HBITMAP w32_tint_bitmap(HBITMAP src, COLORREF rgb) {
    BITMAP bm;
    if (!src || !GetObject(src, sizeof(bm), &bm)) return NULL;
    int w = bm.bmWidth, h = bm.bmHeight;
    if (w <= 0 || h <= 0) return NULL;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // negative: top-down rows
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = GetDC(NULL);
    if (!dc) return NULL;
    unsigned char* out_px = NULL;
    HBITMAP out = CreateDIBSection(dc, &bi, DIB_RGB_COLORS,
                                   (void**)&out_px, NULL, 0);
    unsigned char* src_px = (unsigned char*)malloc((size_t)w * h * 4);
    if (!out || !out_px || !src_px) {
        if (src_px) free(src_px);
        if (out) DeleteObject(out);
        ReleaseDC(NULL, dc);
        return NULL;
    }
    GetDIBits(dc, src, 0, h, src_px, &bi, DIB_RGB_COLORS);

    // CRITICAL: an image with no usable alpha must not become invisible. A
    // 24bpp source has no alpha channel at all, and plenty of 32bpp bitmaps
    // carry zero in that byte while being fully opaque. Either way, taking
    // the alpha at face value would tint the shape to nothing.
    int any_alpha = 0;
    for (int i = 0; i < w * h; i++) {
        if (src_px[i * 4 + 3] != 0) { any_alpha = 1; break; }
    }
    for (int i = 0; i < w * h; i++) {
        unsigned char a = any_alpha ? src_px[i * 4 + 3] : 255;
        out_px[i * 4 + 0] = GetBValue(rgb);
        out_px[i * 4 + 1] = GetGValue(rgb);
        out_px[i * 4 + 2] = GetRValue(rgb);
        out_px[i * 4 + 3] = a;
    }
    free(src_px);
    ReleaseDC(NULL, dc);
    return out;
}

// The untinted original per widget handle, so untint restores rather than
// approximates, and so re-tinting starts from the source instead of tinting
// an already-tinted copy.
//
// TWO originals, because a widget can be backed by either. An ICON is not a
// bitmap with a different name: it carries a MASK, which is what gives it its
// transparent shape. Restoring an icon-backed widget from the colour plane
// alone brings it back as an opaque rectangle, and freeing an HICON with
// DeleteObject silently does nothing and leaks it. Both need the icon itself.
static HBITMAP* w32_tint_base = NULL;   // what we tint FROM (we own it)
static HICON*   w32_tint_icon = NULL;   // the original icon, if icon-backed
static int*     w32_tint_rgb  = NULL;   // packed 0xRRGGBB | 0x1000000, or 0
static int      w32_tint_len  = 0;

static void w32_tint_slots(int handle) {
    if (handle <= w32_tint_len) return;
    int want = handle + 16;
    w32_tint_base = (HBITMAP*)realloc(w32_tint_base, sizeof(HBITMAP) * want);
    w32_tint_icon = (HICON*)realloc(w32_tint_icon, sizeof(HICON) * want);
    w32_tint_rgb  = (int*)realloc(w32_tint_rgb, sizeof(int) * want);
    for (int i = w32_tint_len; i < want; i++) {
        w32_tint_base[i] = NULL;
        w32_tint_icon[i] = NULL;
        w32_tint_rgb[i]  = 0;
    }
    w32_tint_len = want;
}

void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_IMAGE || !w->hwnd) return;
    w32_tint_slots(handle);
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);

    if (!on) {
        if (w32_tint_icon[handle - 1]) {
            // Icon-backed: put the ICON back, mask and all, and return the
            // control to SS_ICON. Restoring the colour plane instead would
            // bring it back as an opaque rectangle.
            st &= ~SS_BITMAP;
            st |= SS_ICON;
            SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
            HBITMAP tinted = (HBITMAP)SendMessageW(w->hwnd, STM_SETICON,
                                (WPARAM)w32_tint_icon[handle - 1], 0);
            if (tinted) DeleteObject(tinted);
            if (w32_tint_base[handle - 1]) {
                DeleteObject(w32_tint_base[handle - 1]);
            }
            w32_tint_icon[handle - 1] = NULL;
            w32_tint_base[handle - 1] = NULL;
        } else if (w32_tint_base[handle - 1]) {
            HBITMAP cur = (HBITMAP)SendMessageW(w->hwnd, STM_SETIMAGE,
                            IMAGE_BITMAP, (LPARAM)w32_tint_base[handle - 1]);
            if (cur && cur != w32_tint_base[handle - 1]) DeleteObject(cur);
            w32_tint_base[handle - 1] = NULL;   // the control owns it again
        }
        w32_tint_rgb[handle - 1] = 0;
        InvalidateRect(w->hwnd, NULL, TRUE);
        return;
    }

    // Establish what to tint FROM, once. Re-tinting reuses it so the tints do
    // not stack.
    HBITMAP base = w32_tint_base[handle - 1];
    if (!base) {
        if ((st & SS_TYPEMASK) == SS_ICON) {   /* enum, not flag — see has_content */
            HICON ic = (HICON)SendMessageW(w->hwnd, STM_GETIMAGE, IMAGE_ICON, 0);
            if (!ic) return;
            ICONINFO ii;
            memset(&ii, 0, sizeof(ii));
            if (!GetIconInfo(ic, &ii)) return;
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (!ii.hbmColor) return;
            base = ii.hbmColor;                 // ours to free from here
            w32_tint_icon[handle - 1] = ic;     // keep it for untint
            st &= ~SS_ICON;
            st |= SS_BITMAP;
            SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
        } else {
            HBITMAP cur = (HBITMAP)SendMessageW(w->hwnd, STM_GETIMAGE,
                                                IMAGE_BITMAP, 0);
            if (!cur) return;
            base = cur;
        }
        w32_tint_base[handle - 1] = base;
    }

    COLORREF rgb = RGB((int)(r * 255) & 255, (int)(g * 255) & 255,
                       (int)(b * 255) & 255);
    HBITMAP tinted = w32_tint_bitmap(base, rgb);
    if (!tinted) return;

    HBITMAP prev = (HBITMAP)SendMessageW(w->hwnd, STM_SETIMAGE,
                                         IMAGE_BITMAP, (LPARAM)tinted);
    // Free the outgoing image, but never the base being kept for untint, and
    // never an HICON through DeleteObject: on the first tint of an icon-backed
    // widget `prev` IS the icon, and it is already stashed above.
    if (prev && prev != base && (HICON)prev != w32_tint_icon[handle - 1]) {
        DeleteObject(prev);
    }
    w32_tint_rgb[handle - 1] = (((int)(r * 255) & 255) << 16)
                             | (((int)(g * 255) & 255) << 8)
                             |  ((int)(b * 255) & 255) | 0x1000000;
    InvalidateRect(w->hwnd, NULL, TRUE);
}

int aether_ui_image_get_tint(int handle) {
    if (handle < 1 || handle > w32_tint_len) return -1;
    int v = w32_tint_rgb[handle - 1];
    return (v & 0x1000000) ? v : -1;
}

void aether_ui_file_icon_set(int handle, const char* path) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_IMAGE || !w->hwnd) return;
    HICON ic = w32_icon_for_path(path);
    if (!ic) return;
    /* A TINTED widget is in SS_BITMAP mode showing a recoloured DIB;
       STM_SETICON there displays nothing (the recycled-row rebind case —
       "expected 0 empty, got 1"). Return it to icon mode, drop the tint
       artifacts, rebind, then re-apply the stored tint so the tint
       SURVIVES the rebind — the spec's exact contract. */
    int retint = aether_ui_image_get_tint(handle);
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    if ((st & SS_TYPEMASK) != SS_ICON) {
        st = (st & ~SS_TYPEMASK) | SS_ICON;
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
        if (handle >= 1 && handle <= w32_tint_len) {
            HBITMAP shown = (HBITMAP)SendMessageW(w->hwnd, STM_GETIMAGE,
                                                  IMAGE_BITMAP, 0);
            if (shown) DeleteObject(shown);
            if (w32_tint_base[handle - 1]) DeleteObject(w32_tint_base[handle - 1]);
            if (w32_tint_icon[handle - 1]) DestroyIcon(w32_tint_icon[handle - 1]);
            w32_tint_base[handle - 1] = NULL;
            w32_tint_icon[handle - 1] = NULL;
            w32_tint_rgb[handle - 1] = 0;
        }
    }
    // The STATIC owns whatever it held; replace and destroy the old one, or a
    // listing that re-icons its rows on every repaint leaks a handle a row.
    HICON old = (HICON)SendMessageW(w->hwnd, STM_SETICON, (WPARAM)ic, 0);
    if (old && old != ic) DestroyIcon(old);
    if (retint >= 0) {
        aether_ui_image_set_tint(handle, 1,
            ((retint >> 16) & 0xFF) / 255.0,
            ((retint >> 8)  & 0xFF) / 255.0,
            ( retint        & 0xFF) / 255.0);
    }
    InvalidateRect(w->hwnd, NULL, TRUE);
}

static void w32_image_apply_fill(int handle);  /* defined with the GDI+ decls below */

void aether_ui_image_set_fill(int handle, int mode) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_IMAGE || !w->hwnd) return;
    if (mode < 0 || mode > 3) mode = 0;
    /* Store the mode ACTUALLY APPLIED. contain/cover used to degrade to
       original ("not wired"); they are wired now — a pre-scaled DIB derived
       from the kept original — so all four modes are effective and the
       /widgets fill field round-trips (the imagefill spec's contract). */
    w->image_fill = mode;
    w32_image_apply_fill(handle);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

int aether_ui_image_get_fill(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_IMAGE) ? w->image_fill : 0;
}

void aether_ui_image_set_size(int handle, int width, int height) {
    Widget* w = widget_at(handle);
    if (w) {
        w->pref_width = width; w->pref_height = height;
        if (w->kind == WK_IMAGE && (w->image_fill == 1 || w->image_fill == 2))
            w32_image_apply_fill(handle);   /* contain/cover derive at box size */
    }
}

// ---------------------------------------------------------------------------
// Menu bar + context menus.
//
// Win32 menus (HMENU) are not HWNDs, so they live in a parallel registry.
// Each menu item is a small command ID (starting at AE_MENU_ID_BASE) that
// we dispatch from app_wnd_proc's WM_COMMAND: the ID looks up the
// registered closure and invokes it. Menu bars attach to the app window
// via SetMenu(); context menus use TrackPopupMenu().
// ---------------------------------------------------------------------------

typedef struct {
    HMENU    hmenu;
    int      is_menu_bar;   // 1 = menu bar (top-level); 0 = popup/submenu
    int      attached;      // 1 once this menu has been added to a parent bar
    wchar_t* label;         // display text for submenus (NULL for menu bars)
} MenuEntry;

static MenuEntry* menus = NULL;
static int        menu_count = 0;
static int        menu_capacity = 0;

typedef struct {
    UINT       id;
    AeClosure* closure;
} MenuCommand;

static MenuCommand* menu_commands = NULL;
static int          menu_command_count = 0;
static int          menu_command_capacity = 0;
static UINT         next_menu_id = AE_MENU_ID_BASE;

static int register_menu(HMENU hmenu, int is_bar, const wchar_t* label) {
    if (menu_count >= menu_capacity) {
        menu_capacity = menu_capacity == 0 ? 8 : menu_capacity * 2;
        menus = (MenuEntry*)realloc(menus, sizeof(MenuEntry) * menu_capacity);
    }
    menus[menu_count].hmenu = hmenu;
    menus[menu_count].is_menu_bar = is_bar;
    menus[menu_count].attached = 0;
    menus[menu_count].label = label ? _wcsdup(label) : NULL;
    menu_count++;
    return menu_count; // 1-based
}

static MenuEntry* menu_at(int handle) {
    if (handle < 1 || handle > menu_count) return NULL;
    return &menus[handle - 1];
}

// Called from the app window's WM_COMMAND when LOWORD(wParam) is a menu ID.
// Returns 1 if handled.
static int menu_dispatch_command(UINT id) {
    for (int i = 0; i < menu_command_count; i++) {
        if (menu_commands[i].id == id) {
            invoke_closure(menu_commands[i].closure);
            return 1;
        }
    }
    return 0;
}

int aether_ui_menu_bar_create(void) {
    ensure_win_init();
    HMENU hmenu = CreateMenu();
    return register_menu(hmenu, 1, NULL);
}

int aether_ui_menu_create(const char* label) {
    ensure_win_init();
    HMENU hmenu = CreatePopupMenu();
    return register_menu(hmenu, 0, utf8_to_wide(label ? label : "Menu"));
}

void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure) {
    MenuEntry* m = menu_at(menu_handle);
    if (!m) return;
    UINT id = next_menu_id++;
    AppendMenuW(m->hmenu, MF_STRING, id, utf8_to_wide(label));
    if (menu_command_count >= menu_command_capacity) {
        menu_command_capacity = menu_command_capacity == 0
                                ? 32 : menu_command_capacity * 2;
        menu_commands = (MenuCommand*)realloc(menu_commands,
            sizeof(MenuCommand) * menu_command_capacity);
    }
    menu_commands[menu_command_count].id = id;
    menu_commands[menu_command_count].closure = (AeClosure*)boxed_closure;
    menu_command_count++;
    // Side-store for the AetherUIDriver /tray/{id}/menu/activate route.
    aether_ui_menu_item_record(menu_handle, label, boxed_closure);
}

void aether_ui_menu_add_separator(int menu_handle) {
    MenuEntry* m = menu_at(menu_handle);
    if (m) AppendMenuW(m->hmenu, MF_SEPARATOR, 0, NULL);
}

void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    MenuEntry* bar = menu_at(bar_handle);
    MenuEntry* sub = menu_at(menu_handle);
    if (!bar || !sub) return;
    const wchar_t* label = sub->label ? sub->label : L"Menu";
    AppendMenuW(bar->hmenu, MF_POPUP, (UINT_PTR)sub->hmenu, label);
    sub->attached = 1;
}

void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    MenuEntry* bar = menu_at(bar_handle);
    if (!bar) return;
    AppEntry* e = &apps[app_handle - 1];
    // aether_ui_app_run_raw hasn't created the window yet in the common flow,
    // so stash the menu bar and attach it at show-time.
    e->pending_menu = bar->hmenu;
}

// Per-window menu bar: SetMenu on the target window's HWND (native, per-window).
// win_handle 1 = primary, 2.. = extras (unified driver numbering).
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) {
    MenuEntry* bar = menu_at(bar_handle);
    if (!bar || win_handle < 1 || win_handle > w32_window_count) return;
    HWND hwnd = w32_windows[win_handle - 1].hwnd;
    if (hwnd) { SetMenu(hwnd, bar->hmenu); DrawMenuBar(hwnd); }
}

void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    // TrackPopupMenu runs its own modal message loop and only returns
    // when the menu is dismissed by a click or Escape. In headless
    // contexts (widget smoke tests, CI without a window server) the
    // call would block indefinitely — no user input, no outer message
    // pump to dismiss. Respect AETHER_UI_HEADLESS and also require a
    // visible ancestor window as a second guard for programming errors
    // that pop a menu from an unmounted widget.
    if (aeui_is_headless()) return;
    MenuEntry* m = menu_at(menu_handle);
    Widget* w = widget_at(anchor_widget);
    if (!m || !w || !w->hwnd) return;
    HWND owner = GetParent(w->hwnd);
    if (!owner || !IsWindowVisible(owner)) return;
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(m->hmenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                   pt.x, pt.y, 0, owner, NULL);
}

// ---------------------------------------------------------------------------
// Grid layout.
//
// A 2D container where children claim (row, col) cells with optional row
// and column spans. Columns are equal-width by default; rows size to their
// tallest child. Matches GtkGrid / NSGridView semantics.
// ---------------------------------------------------------------------------

typedef struct {
    HWND hwnd;
    int  cols;
    int  row_spacing;
    int  col_spacing;
    struct {
        HWND hwnd;
        int  row, col, row_span, col_span;
    } items[64];
    int item_count;
} GridEntry;

static GridEntry** grids = NULL;
static int         grid_count = 0;
static int         grid_capacity = 0;

static GridEntry* grid_for_hwnd(HWND hwnd) {
    for (int i = 0; i < grid_count; i++) {
        if (grids[i] && grids[i]->hwnd == hwnd) return grids[i];
    }
    return NULL;
}

static void grid_do_layout(HWND hwnd) {
    GridEntry* g = grid_for_hwnd(hwnd);
    if (!g || g->item_count == 0) return;

    RECT client;
    GetClientRect(hwnd, &client);
    int total_w = client.right - client.left;
    int total_h = client.bottom - client.top;

    // Determine row count from max row index.
    int rows = 0;
    for (int i = 0; i < g->item_count; i++) {
        int r = g->items[i].row + g->items[i].row_span;
        if (r > rows) rows = r;
    }
    if (rows == 0) return;

    int col_w = (total_w - g->col_spacing * (g->cols - 1)) / g->cols;
    int row_h = (total_h - g->row_spacing * (rows - 1)) / rows;
    if (col_w < 0) col_w = 0;
    if (row_h < 0) row_h = 0;

    for (int i = 0; i < g->item_count; i++) {
        int x = g->items[i].col * (col_w + g->col_spacing);
        int y = g->items[i].row * (row_h + g->row_spacing);
        int w = col_w * g->items[i].col_span
                + g->col_spacing * (g->items[i].col_span - 1);
        int h = row_h * g->items[i].row_span
                + g->row_spacing * (g->items[i].row_span - 1);
        SetWindowPos(g->items[i].hwnd, NULL, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static LRESULT CALLBACK grid_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            grid_do_layout(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return stack_wnd_proc(hwnd, msg, wp, lp);
        case WM_COMMAND:
        case WM_HSCROLL:
        case WM_VSCROLL:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return stack_wnd_proc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    ensure_win_init();
    if (cols < 1) cols = 1;
    HWND hwnd = CreateWindowExW(0, GRID_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 0;
    if (grid_count >= grid_capacity) {
        grid_capacity = grid_capacity == 0 ? 4 : grid_capacity * 2;
        grids = (GridEntry**)realloc(grids, sizeof(GridEntry*) * grid_capacity);
    }
    GridEntry* g = (GridEntry*)calloc(1, sizeof(GridEntry));
    g->hwnd = hwnd;
    g->cols = cols;
    g->row_spacing = row_spacing;
    g->col_spacing = col_spacing;
    grids[grid_count++] = g;
    return register_widget_typed(hwnd, WK_GRID);
}

void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span) {
    Widget* g = widget_at(grid_handle);
    Widget* c = widget_at(child_handle);
    if (!g || !c || g->kind != WK_GRID) return;
    GridEntry* ge = grid_for_hwnd(g->hwnd);
    if (!ge || ge->item_count >= 64) return;
    if (row_span < 1) row_span = 1;
    if (col_span < 1) col_span = 1;
    // Reparent the child to the grid.
    SetParent(c->hwnd, g->hwnd);
    LONG_PTR st = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
    SetWindowLongPtrW(c->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    ge->items[ge->item_count].hwnd = c->hwnd;
    ge->items[ge->item_count].row = row;
    ge->items[ge->item_count].col = col;
    ge->items[ge->item_count].row_span = row_span;
    ge->items[ge->item_count].col_span = col_span;
    ge->item_count++;
    grid_do_layout(g->hwnd);
}

static int w32_measure_grid_natural(HWND grid_hwnd, int* out_w, int* out_h) {
    GridEntry* ge = grid_for_hwnd(grid_hwnd);
    if (!ge) return 0;
    int colw[64] = {0}, rowh[64] = {0};
    int maxr = 0, maxc = 0;
    for (int i = 0; i < ge->item_count; i++) {
        Widget* cw = widget_at(handle_for_hwnd(ge->items[i].hwnd));
        if (!cw || cw->dead) continue;
        int mw = 0, mh = 0;
        measure_widget(cw, &mw, &mh);
        int r = ge->items[i].row, c = ge->items[i].col;
        if (r < 0 || r >= 64 || c < 0 || c >= 64) continue;
        if (mw > colw[c]) colw[c] = mw;
        if (mh > rowh[r]) rowh[r] = mh;
        if (r > maxr) maxr = r;
        if (c > maxc) maxc = c;
    }
    int tw = 0, th = 0;
    for (int c = 0; c <= maxc; c++) tw += colw[c];
    for (int r = 0; r <= maxr; r++) th += rowh[r];
    if (maxc > 0) tw += ge->col_spacing * maxc;
    if (maxr > 0) th += ge->row_spacing * maxr;
    *out_w = tw;
    *out_h = th;
    return 1;
}

// ---------------------------------------------------------------------------
// Canvas backend — GDI+ flat-API path, minimal for first pass.
//
// Paths are recorded as a command stream; WM_PAINT replays them onto an
// offscreen bitmap then BitBlt-s to screen. Supports begin_path, move_to,
// line_to, stroke, fill_rect, clear.
// ---------------------------------------------------------------------------
typedef enum {
    CV_BEGIN, CV_MOVE, CV_LINE, CV_STROKE, CV_FILL_RECT, CV_CLEAR,
    CV_ARC, CV_CLOSE, CV_FILL, CV_FILL_TEXT, CV_STROKE_TEXT, CV_DRAW_IMAGE,
    CV_FILL_LINEAR, CV_FILL_RADIAL,
    /* True group opacity: composite everything between BEGIN and END into an
       offscreen layer, then paint that layer ONCE at the group alpha, so
       overlapping children don't double-darken. GTK4 has had this via
       cairo_push_group since the feature landed; win32's pair were empty
       stubs, which is why mememe.svg (a <g opacity="0.5"> of three
       overlapping strokes) rendered fully opaque here. */
    CV_GROUP_BEGIN, CV_GROUP_END,
    /* Intersect the clip with (x,y,w,h) for the rest of the frame's
       replay. AeVG emits one at scene-flush start (vg/live.ae) to
       enforce the SVG viewport's overflow:hidden, so without it every
       live scene drew past its viewport edge on this backend alone.
       Appended at the END: the values are positional and the replay
       switches index off them. */
    CV_CLIP
} CanvasCmdKind;

typedef struct {
    CanvasCmdKind k;
    // p0..p3 carry geometry: x1,y1,x2,y2 for lines; x,y,w,h for rects;
    // line width in p0 for stroke commands; ARC: cx,cy,radius,(unused);
    // FILL_TEXT: x,y,font_size,(unused); DRAW_IMAGE: x,y,destw,desth
    // (dest 0 = unscaled draw_image: blit at pixel dims).
    float p0, p1, p2, p3;
    float a0, a1;          // ARC start/end angle (radians)
    float cr, cg, cb, calpha;
    int cap, join;         // STROKE and gradient STROKE: 0=butt/miter 1=round 2=square/bevel
    char* text;            // FILL_TEXT string (owned)
    unsigned char* pixels; // DRAW_IMAGE RGBA8888 buffer (owned)
    int iw, ih;            // DRAW_IMAGE pixel dims
    // Gradient: linear (gx1,gy1)→(gx2,gy2); radial center (gx1,gy1) r gr.
    float gx1, gy1, gx2, gy2, gr, gfx, gfy;
    /* The ELLIPSE a gradientTransform (or a non-square objectBoundingBox)
       produces. gr keeps the old scalar answer, so grx == 0 renders exactly
       as before. grot is carried but NOT yet applied -- see the replay. */
    float grx, gry, grot;
    float grad_line_width;  // 0 → fill; >0 → stroke (GDI approximates as fill)
    int grad_extend;       // SVG spreadMethod: 0=pad (default), 1=reflect, 2=repeat
    int even_odd;          // FILL: SVG fill-rule -- 1=evenodd, 0=nonzero (default)
    char* font_family;     // owned; raw CSS stack, NULL when unset
    int n_stops;
    double* stop_off;      // owned
    double* stop_rgba;     // owned: n_stops*4
} CanvasCmd;

typedef struct {
    HWND hwnd;
    int width, height;
    CanvasCmd* cmds;
    int cmd_count;
    int cmd_cap;
    float cur_x, cur_y;
    AeClosure* on_move;    // pointer-move hook (canvas-local x,y); null = none
    AeClosure* on_click;   // pointer-press hook (canvas-local x,y)
    AeClosure* on_release; // pointer-release hook (canvas-local x,y)
    AeClosure* on_key;     // key-press hook (GDK key name string)
    AeClosure* on_key_release; // key-up hook (same key names)
    AeClosure* on_resize;  // |w, h| — fired from WM_SIZE (canvas rescale)
    AeClosure* on_canvas_scroll; // wheel / precision scroll (dx,dy)
    // Last-paint metrics for the compositor instrumentation
    // (GET /canvas/{id}/debug). `last_paint_area` is the pixel area the
    // last paint covered: the summed clip-rect area when the paint was
    // clipped, else the whole allocation. See
    // docs/design/retained-compositor.md — under immediate mode the two
    // coincide, and only become distinguishable once damage is owned by
    // the painter.
    int last_paint_w, last_paint_h;
    int last_paint_area, last_paint_count;
    int full_paints, clip_paints, last_clip_area;
    // read_pixel replay cache. A grid probe asks for ~15k pixels of the
    // SAME frame; without this each call rebuilds a DC+bitmap and replays
    // the whole command buffer, which on a path-heavy scene takes minutes
    // and hangs the spec run. Keyed on (generation, command count, size):
    // canvas_clear bumps the generation, and within a generation the
    // buffer only grows, so the pair identifies the content. Mirrors the
    // gtk4 cache added for exactly the same symptom.
    HDC rp_dc;
    HBITMAP rp_bmp, rp_old_bmp;
    unsigned long rp_gen, rp_cache_gen;
    int rp_cache_count, rp_cache_w, rp_cache_h;
    double* paint_clip_rects;
    int paint_clip_count;
    int paint_clip_cap;
} Canvas;

static Canvas* canvases = NULL;
static int canvas_count = 0;
static int canvas_cap = 0;

extern double floatarr_get_raw(void* arr, int i);

// The most recently key-registered canvas, focused after the window shows
// (registration-time focus can't stick — the canvas is still on the hidden
// holder then; mirrors GTK's "focus on map").
static int pending_key_canvas = 0;

static void aeui_focus_pending_key_canvas(void) {
    if (pending_key_canvas >= 1 && pending_key_canvas <= canvas_count) {
        SetFocus(canvases[pending_key_canvas - 1].hwnd);
    }
}

static int aeui_hwnd_is_key_canvas(HWND hwnd) {
    return pending_key_canvas >= 1 && pending_key_canvas <= canvas_count &&
           hwnd == canvases[pending_key_canvas - 1].hwnd;
}

static void canvas_free_text(int canvas_id);  // fwd; frees FILL_TEXT strings

static int canvas_id_for_hwnd(HWND hwnd) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvases[i].hwnd == hwnd) return i + 1;
    }
    return 0;
}

static void canvas_add_cmd(int canvas_id, CanvasCmd cmd) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* c = &canvases[canvas_id - 1];
    if (c->cmd_count >= c->cmd_cap) {
        c->cmd_cap = c->cmd_cap == 0 ? 128 : c->cmd_cap * 2;
        c->cmds = (CanvasCmd*)realloc(c->cmds, sizeof(CanvasCmd) * c->cmd_cap);
    }
    c->cmds[c->cmd_count++] = cmd;
}

int aether_ui_canvas_create_impl(int width, int height) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, CANVAS_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    if (canvas_count >= canvas_cap) {
        canvas_cap = canvas_cap == 0 ? 8 : canvas_cap * 2;
        canvases = (Canvas*)realloc(canvases, sizeof(Canvas) * canvas_cap);
    }
    Canvas* cv = &canvases[canvas_count++];
    cv->hwnd = h;
    cv->width = width;
    cv->height = height;
    cv->cmds = NULL;
    cv->cmd_count = 0;
    cv->cmd_cap = 0;
    cv->cur_x = cv->cur_y = 0;
    cv->on_move = NULL;
    cv->on_click = NULL;
    cv->on_release = NULL;
    cv->on_key = NULL;
    cv->on_resize = NULL;
    cv->paint_clip_rects = NULL;
    cv->paint_clip_count = 0;
    cv->paint_clip_cap = 0;
    // realloc above does not zero, so the metrics need explicit init like
    // every other field here.
    cv->last_paint_w = 0;
    cv->last_paint_h = 0;
    cv->last_paint_area = 0;
    cv->last_paint_count = 0;
    cv->full_paints = 0;
    cv->clip_paints = 0;
    cv->last_clip_area = 0;
    cv->rp_dc = NULL;
    cv->rp_bmp = NULL;
    cv->rp_old_bmp = NULL;
    cv->rp_gen = 0;
    cv->rp_cache_gen = 0;
    cv->rp_cache_count = -1;
    cv->rp_cache_w = 0;
    cv->rp_cache_h = 0;
    int widget_handle = register_widget_typed(h, WK_CANVAS);
    Widget* ww = widget_at(widget_handle);
    if (ww) {
        ww->pref_width = width;
        ww->pref_height = height;
        ww->u.canvas.canvas_id = canvas_count;
    }
    return canvas_count;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return 0;
    return handle_for_hwnd(canvases[canvas_id - 1].hwnd);
}

// Resize hook — REAL (2026-07-20). WM_SIZE on the canvas HWND updates the
// canvas's recorded dimensions and fires the |w, h| closure (same shape as
// GTK4's), so the app re-flushes its vg scene at the new scale.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    canvases[canvas_id - 1].on_resize = (AeClosure*)boxed_closure;
}

// Pointer-press on a canvas: WM_LBUTTONDOWN → (x,y) into the closure, and
// the driver's POST /canvas/{id}/click drives the same hook. Mirrors the
// GTK4 GtkGestureClick "pressed" path.
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_click = (AeClosure*)boxed_closure;
}

void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_move = (AeClosure*)boxed_closure;
}

// Keyboard input on a canvas: WM_KEYDOWN → GDK-style key name string into
// the closure (mirrors the GTK4 GtkEventControllerKey path — the app-side
// key handlers compare against GDK names like "Left"/"Return"/"Escape").
// The canvas is made a tab stop so real keystrokes reach it once focused;
// the driver's POST /canvas/{id}/key drives the same hook without focus.
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    Canvas* cv = &canvases[canvas_id - 1];
    cv->on_key = (AeClosure*)boxed_closure;
    LONG_PTR st = GetWindowLongPtrW(cv->hwnd, GWL_STYLE);
    SetWindowLongPtrW(cv->hwnd, GWL_STYLE, st | WS_TABSTOP);
    // Focus is deferred to app-show: at registration the canvas is still
    // parented to the hidden holder, so SetFocus wouldn't stick (mirrors the
    // GTK "focus on map" lesson). The window-show path grabs it once the
    // window is up; clicking the canvas also focuses it (WM_LBUTTONDOWN).
    pending_key_canvas = canvas_id;
}

// Key-up hook: WM_KEYUP → same GDK-style name into the closure. Focus/tab-stop
// handling lives on the on_key registration (register both for real input).
void aether_ui_canvas_on_key_release_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_key_release = (AeClosure*)boxed_closure;
}

// Pointer-release on a canvas: WM_LBUTTONUP → (x,y) into the closure.
/* canvas scroll: GTK4-only for now (a zoom-capable canvas needs a real
   scroll controller). Stubbed so the ABI stays uniform across backends. */
/* Gesture probe: GTK4-only diagnostic. Stubbed for ABI uniformity. */
void aether_ui_canvas_gesture_probe_impl(int canvas_id, void* boxed_closure) {
    (void)canvas_id; (void)boxed_closure;
}

void aether_ui_canvas_on_scroll_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_canvas_scroll = (AeClosure*)boxed_closure;
}

void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_release = (AeClosure*)boxed_closure;
}

// begin_path starts a fresh command stream — drop any previously-recorded
// begin_path starts a new PATH, not a new FRAME — it must append, never
// reset. It used to zero cmd_count here as a leak guard ("a redraw-per-
// frame loop doesn't accumulate unboundedly"), which mistook a per-shape
// primitive for a frame boundary: EVERY new path erased everything drawn
// before it in the frame. On win32 that meant multi-shape canvases kept
// only their LAST path — vg backgrounds vanished, pixel probes read
// white floods, and only rect-only apps (the GP treemap) looked right.
// The real frame boundary is canvas_clear, which already frees recorded
// text and resets the buffer (the leak the old comment feared is handled
// there — the live loop clears every frame).

// Which backend is this binary running? Lets pure-.ae code (the goldens
// gallery picks its tests/goldens/<backend>/ directory) branch without
// per-platform builds or environment guesswork.
const char* aether_ui_backend_name_impl(void) { return "win32"; }

void aether_ui_canvas_begin_path_impl(int canvas_id) {
    CanvasCmd c = {0}; c.k = CV_BEGIN;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) {
    CanvasCmd c = {0}; c.k = CV_MOVE; c.p0 = x; c.p1 = y;
    canvas_add_cmd(canvas_id, c);
    if (canvas_id >= 1 && canvas_id <= canvas_count) {
        canvases[canvas_id - 1].cur_x = x;
        canvases[canvas_id - 1].cur_y = y;
    }
}

void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* cv = &canvases[canvas_id - 1];
    CanvasCmd c = {0}; c.k = CV_LINE;
    c.p0 = cv->cur_x; c.p1 = cv->cur_y; c.p2 = x; c.p3 = y;
    canvas_add_cmd(canvas_id, c);
    cv->cur_x = x; cv->cur_y = y;
}

/* True group opacity. These were empty stubs, so <g opacity="0.5"> painted
   fully opaque on win32 (mememe.svg); GTK4 has had the real thing via
   cairo_push_group all along. See CV_GROUP_BEGIN/END in the GDI+ replay. */
void aether_ui_canvas_group_begin_impl(int canvas_id) {
    CanvasCmd c = {0};
    c.k = CV_GROUP_BEGIN;
    canvas_add_cmd(canvas_id, c);
}
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    CanvasCmd c = {0};
    c.k = CV_GROUP_END; c.calpha = alpha;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b,
                                   double a, double line_width, int cap, int join) {
    CanvasCmd c = {0};
    c.k = CV_STROKE; c.cr = r; c.cg = g; c.cb = b; c.calpha = a; c.p0 = line_width;
    c.cap = cap; c.join = join;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y,
                                      double w, double h,
                                      double r, double g, double b, double a) {
    CanvasCmd c = {0};
    c.k = CV_FILL_RECT; c.p0 = x; c.p1 = y; c.p2 = w; c.p3 = h;
    c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    canvas_add_cmd(canvas_id, c);
}

// Viewport clip — no-op on Win32 for now (GTK-verified feature).
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y,
                                      double w, double h) {
    CanvasCmd c = {0};
    c.k = CV_CLIP; c.p0 = (float)x; c.p1 = (float)y;
    c.p2 = (float)w; c.p3 = (float)h;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_set_clip_rects_impl(int canvas_id, void* rects, int n) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* cv = &canvases[canvas_id - 1];
    if (!rects || n <= 0) {
        cv->paint_clip_count = 0;
        return;
    }
    if (n > cv->paint_clip_cap) {
        cv->paint_clip_cap = n;
        cv->paint_clip_rects = (double*)realloc(cv->paint_clip_rects, sizeof(double) * n * 4);
    }
    if (!cv->paint_clip_rects) {
        cv->paint_clip_count = 0;
        cv->paint_clip_cap = 0;
        return;
    }
    for (int i = 0; i < n * 4; i++) {
        cv->paint_clip_rects[i] = floatarr_get_raw(rects, i);
    }
    cv->paint_clip_count = n;
}

void aether_ui_canvas_reset_clip_impl(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    canvases[canvas_id - 1].paint_clip_count = 0;
}

void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                                double start_angle, double end_angle) {
    CanvasCmd c = {0};
    c.k = CV_ARC; c.p0 = cx; c.p1 = cy; c.p2 = radius;
    c.a0 = start_angle; c.a1 = end_angle;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_close_path_impl(int canvas_id) {
    CanvasCmd c = {0}; c.k = CV_CLOSE;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a,
                                int even_odd) {
    CanvasCmd c = {0};
    c.k = CV_FILL; c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    c.even_odd = even_odd;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text,
                                      double x, double y, double font_size,
                                      int font_flags, const char* font_family,
                                      double r, double g, double b, double a) {
    (void)font_flags;   // font-family selection not yet wired on Win32
    CanvasCmd c = {0};
    c.k = CV_FILL_TEXT; c.p0 = x; c.p1 = y; c.p2 = font_size;
    c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    c.text = text ? _strdup(text) : NULL;
    c.font_family = (font_family && font_family[0]) ? _strdup(font_family) : NULL;
    canvas_add_cmd(canvas_id, c);
}

// Stroke (outline) text — STUB. GDI text-outline needs a Path via
// BeginPath/TextOut/EndPath then StrokePath; deferred to when we're next on
// winbaz. No-op keeps the ABI linkable (the GTK4 backend is real).
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text,
                                        double x, double y, double font_size,
                                        double line_width, int font_flags,
                                        const char* font_family,
                                        double r, double g, double b, double a) {
    (void)font_flags;   // bold/italic selection not yet wired on Win32
    CanvasCmd c = {0};
    c.k = CV_STROKE_TEXT; c.p0 = x; c.p1 = y; c.p2 = font_size; c.p3 = line_width;
    c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    c.text = text ? _strdup(text) : NULL;
    /* The CV_STROKE_TEXT draw already calls gdip_resolve_family(cmd->font_family)
       -- it was this builder that never filled the field in, so stroked text
       always resolved to the default face while FILLED text, whose builder is
       ten lines up and does store it, honoured the requested one. The same
       string rendered in two different typefaces depending on fill vs stroke.
       Freed by the same command-cleanup path as the fill case. */
    c.font_family = (font_family && font_family[0]) ? _strdup(font_family) : NULL;
    canvas_add_cmd(canvas_id, c);
}

// Text metrics — REAL via GDI (2026-07-20). A screen-DC scratch font at the
// requested pixel size (negative lfHeight = character height, matching the
// px semantics GTK4's cairo path uses); width via GetTextExtentPoint32W,
// vertical metrics via GetTextMetricsW. The font is cached per size (specs
// hammer one or two sizes).
static HFONT aeui_metrics_font(double size) {
    static HFONT cached = NULL;
    static int cached_px = 0;
    int px = (int)(size + 0.5);
    if (px <= 0) px = 16;
    if (cached && cached_px == px) return cached;
    if (cached) DeleteObject(cached);
    LOGFONTW lf = {0};
    GetObjectW((HFONT)GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    lf.lfHeight = -px;   // negative = character height in px
    cached = CreateFontIndirectW(&lf);
    cached_px = px;
    return cached;
}

double aether_ui_text_measure(double size, const char* text) {
    if (!text || !text[0]) return 0.0;
    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, aeui_metrics_font(size));
    wchar_t wbuf[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, 1024);
    SIZE sz = {0, 0};
    if (n > 1) GetTextExtentPoint32W(hdc, wbuf, n - 1, &sz);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
    return (double)sz.cx;
}

static void aeui_metrics(double size, TEXTMETRICW* tm) {
    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, aeui_metrics_font(size));
    GetTextMetricsW(hdc, tm);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
}
double aether_ui_font_ascent(double size)  { TEXTMETRICW tm; aeui_metrics(size, &tm); return (double)tm.tmAscent; }
double aether_ui_font_descent(double size) { TEXTMETRICW tm; aeui_metrics(size, &tm); return (double)tm.tmDescent; }
double aether_ui_font_height(double size)  { TEXTMETRICW tm; aeui_metrics(size, &tm); return (double)(tm.tmHeight + tm.tmExternalLeading); }

void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    CanvasCmd c = {0};
    c.k = CV_DRAW_IMAGE; c.p0 = x; c.p1 = y;
    c.pixels = owned; c.iw = iw; c.ih = ih;
    canvas_add_cmd(canvas_id, c);
}

// Scaled draw — REAL now (was a 1:1 stub that ignored dw/dh, same as the
// macOS one fixed alongside it: an upscaled presentation — the Ebiten
// port's set_scale framebuffer, video_frame's fitted region — rendered at
// source-pixel size). Dest extent rides p2/p3 and the executor hands
// StretchDIBits a dest rect of that size; GDI scales natively, matching
// GTK4's cairo path.
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y,
                                       double dw, double dh, int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    CanvasCmd c = {0};
    c.k = CV_DRAW_IMAGE; c.p0 = x; c.p1 = y;
    c.p2 = (float)dw; c.p3 = (float)dh;
    c.pixels = owned; c.iw = iw; c.ih = ih;
    canvas_add_cmd(canvas_id, c);
}

extern double floatarr_get_unchecked(void* arr, int i);

static void win32_copy_stops(CanvasCmd* c, int n_stops,
                              void* offsets, void* rgba) {
    c->n_stops = n_stops;
    c->stop_off = (double*)malloc(sizeof(double) * (n_stops > 0 ? n_stops : 1));
    c->stop_rgba = (double*)malloc(sizeof(double) * (n_stops > 0 ? n_stops*4 : 1));
    for (int i = 0; i < n_stops; i++) {
        c->stop_off[i] = floatarr_get_unchecked(offsets, i);
        c->stop_rgba[i*4+0] = floatarr_get_unchecked(rgba, i*4+0);
        c->stop_rgba[i*4+1] = floatarr_get_unchecked(rgba, i*4+1);
        c->stop_rgba[i*4+2] = floatarr_get_unchecked(rgba, i*4+2);
        c->stop_rgba[i*4+3] = floatarr_get_unchecked(rgba, i*4+3);
    }
}

void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id,
        double x1, double y1, double x2, double y2,
        int n_stops, void* offsets, void* rgba, double line_width, int extend,
        int cap, int join) {
    CanvasCmd c = {0};
    c.k = CV_FILL_LINEAR; c.grad_extend = extend; c.gx1 = x1; c.gy1 = y1; c.gx2 = x2; c.gy2 = y2;
    c.grad_line_width = line_width; c.cap = cap; c.join = join;
    win32_copy_stops(&c, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width, int extend,
        int cap, int join, double rx, double ry, double rot_deg) {
    CanvasCmd c = {0};
    c.k = CV_FILL_RADIAL; c.grad_extend = extend; c.gx1 = cx; c.gy1 = cy; c.gr = radius;
    c.grx = (float)rx; c.gry = (float)ry; c.grot = (float)rot_deg;
    c.gfx = fx; c.gfy = fy; c.grad_line_width = line_width;
    c.cap = cap; c.join = join;
    win32_copy_stops(&c, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, c);
}

// Free owned per-command buffers (FILL_TEXT strings, DRAW_IMAGE
// pixels, gradient stop arrays) before a cmd-buffer reset.
static void canvas_free_text(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* cv = &canvases[canvas_id - 1];
    for (int i = 0; i < cv->cmd_count; i++) {
        CanvasCmd* c = &cv->cmds[i];
        if ((c->k == CV_FILL_TEXT || c->k == CV_STROKE_TEXT) && c->text) {
            free(c->text); c->text = NULL;
        }
        if (c->font_family) { free(c->font_family); c->font_family = NULL; }
        if (c->k == CV_DRAW_IMAGE && c->pixels) {
            free(c->pixels); c->pixels = NULL;
        }
        if (c->k == CV_FILL_LINEAR || c->k == CV_FILL_RADIAL) {
            free(c->stop_off);  c->stop_off = NULL;
            free(c->stop_rgba); c->stop_rgba = NULL;
        }
    }
}

void aether_ui_canvas_clear_impl(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    canvas_free_text(canvas_id);
    canvases[canvas_id - 1].cmd_count = 0;
    canvases[canvas_id - 1].rp_gen++;   // invalidates the read_pixel cache
    CanvasCmd c = {0}; c.k = CV_CLEAR;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    InvalidateRect(canvases[canvas_id - 1].hwnd, NULL, TRUE);
}

// canvas_write_png — off-screen PNG render, no window required.
//
// Was a stub returning 0, which is why the SVG conformance harness
// (vg/test/svg-compare-aevg.py) could never run on Windows: all 208 renders
// failed with "PNG write failed". The stub's own comment prescribed this
// implementation -- replay into a memory DC, wrap as a GDI+ bitmap, save with
// the PNG encoder CLSID -- and every piece it needs was already in the file
// for hook_screenshot_png.
//
// Goes through canvas_replay_to_dc, i.e. THE RENDERER SEAM, so it renders with
// whichever of GDI / GDI+ is selected. That is deliberate: it makes the whole
// SVG corpus scoreable against both renderers, not just the one hand-built
// scene the BbA line has been measuring.
__declspec(dllimport) int __stdcall GdipSaveImageToFile(
    void* image, const unsigned short* filename, const GUID* clsid,
    const void* params);
/* Both declared further down (the renderer seam, and the screenshot hook's
   GDI+ bindings); forward-declared here because this function sits above
   them and C has no forward reference. */
static void canvas_replay_to_dc(Canvas* cv, HDC mem, int width, int height);
__declspec(dllimport) int __stdcall GdipCreateBitmapFromHBITMAP(
    HBITMAP hbm, HPALETTE pal, void** bitmap);

int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height) {
    if (canvas_id < 1 || canvas_id > canvas_count) return 0;
    if (!path || width <= 0 || height <= 0) return 0;
    Canvas* cv = &canvases[canvas_id - 1];

    ensure_gdiplus();

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(NULL, screen);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return 0;
    }
    HGDIOBJ old = SelectObject(mem, bmp);

    canvas_replay_to_dc(cv, mem, width, height);

    int ok = 0;
    void* gbmp = NULL;
    if (GdipCreateBitmapFromHBITMAP(bmp, NULL, &gbmp) == 0 && gbmp) {
        /* Same PNG encoder CLSID hook_screenshot_png uses. */
        GUID png_clsid = {0x557cf406, 0x1a04, 0x11d3,
                          {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        wchar_t* wpath = utf8_to_wide(path);
        if (wpath && GdipSaveImageToFile(gbmp, (const unsigned short*)wpath,
                                         &png_clsid, NULL) == 0) {
            ok = 1;
        }
        GdipDisposeImage(gbmp);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return ok;
}

// ─── Renderer seam (branch-by-abstraction) ────────────────────────────
//
// canvas_replay_to_dc is the ONLY place win32 turns a command buffer into
// pixels, and it has exactly two callers: the on-screen paint and the
// headless readback. That makes it the natural seam for a second renderer.
//
// GDI cannot do alpha (this function opens by filling the surface white;
// the CV_IMAGE case composites against white and stores a=255; read_pixel
// returns 0xFF alpha unconditionally), which is why Stage 3 frame caching
// is gated off on win32. A GDI+ implementation would fix that, but GDI+ is
// in maintenance and has known bugs -- so the two live side by side and are
// COMPARED, rather than one replacing the other on faith.
//
// Branch-by-abstraction, deliberately not branch-by-if: two whole
// implementations you can read and run independently, instead of
// conditionals threaded through one. The comparison in
// docs/design/win32-gdiplus-renderer.md is only interpretable that way.
//
// Selected at RUN time so one binary produces both sides of that
// comparison -- a compile flag would mean two builds, and any difference
// could then be blamed on the build rather than the renderer.
//
//     AETHER_UI_WIN32_RENDERER=legacy    opt OUT, back to plain GDI
//     AETHER_UI_WIN32_RENDERER=gdiplus   the default, and what ships
//
// GDI+ IS NOW THE DEFAULT. The comparison it was built for is what decided
// that, over the 208-file SVG corpus scored against librsvg:
//
//                good(<5)  ok(5-15)  diff(>=15)   mean
//   legacy GDI     131       40         37        20.87
//   GDI+           172       18         17         4.71
//
// and on the 167 of those files that carry a real viewBox (the other 40 have
// none, so their score measures canvas-size inference rather than rendering)
// GDI+ means 3.00 against the GTK4 reference backend's 1.77, with 104 of 167
// within 1.0 of it.
//
// Legacy is KEPT, not deleted: it is the second arm of
// tests/win32/capture-pixelgrid.sh, it is the fallback when
// GdipCreateFromHDC fails (see canvas_replay_to_dc_gdiplus), and it makes
// this flip revertible by one environment variable if a real application
// regresses in a way 208 static SVGs did not catch.
//
// Read once and cached: this is called per paint.
static int win32_use_gdiplus(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("AETHER_UI_WIN32_RENDERER");
        cached = (v && strcmp(v, "legacy") == 0) ? 0 : 1;
    }
    return cached;
}

static void canvas_replay_to_dc_gdi(Canvas* cv, HDC mem, int width, int height);
static void canvas_replay_to_dc_gdiplus(Canvas* cv, HDC mem, int width, int height);

static void canvas_replay_to_dc(Canvas* cv, HDC mem, int width, int height) {
    if (win32_use_gdiplus()) canvas_replay_to_dc_gdiplus(cv, mem, width, height);
    else                     canvas_replay_to_dc_gdi(cv, mem, width, height);
}

// The GDI replay core — shared by the on-screen paint and the headless
// pixel readback below (canvas_read_pixel replays into its own memory DC,
// exactly like the GTK4 backend replays into a cairo image surface).
static void canvas_replay_to_dc_gdi(Canvas* cv, HDC mem, int width, int height) {
    // Plain GDI replay — GDI+ bindings from C are clunky; for a first
    // pass this delivers lines + filled rects with correct pixels.
    /* Start each replay from NO clip. The read_pixel path reuses one cached
       DC across calls (rp_dc), so a clip left behind by the previous replay
       would intersect with this one and shrink the visible area every frame. */
    SelectClipRgn(mem, NULL);
    // White background
    RECT full = { 0, 0, width, height };
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mem, &full, white);

    HPEN cur_pen = NULL;
    HPEN old_pen = (HPEN)SelectObject(mem, GetStockObject(BLACK_PEN));

    for (int i = 0; i < cv->cmd_count; i++) {
        CanvasCmd* cmd = &cv->cmds[i];
        switch (cmd->k) {
            case CV_CLEAR:
                FillRect(mem, &full, white);
                break;
            case CV_CLIP:
                /* IntersectClipRect is exactly the documented semantic:
                   intersect the CURRENT clip, persisting for the rest of the
                   replay. GDI takes an exclusive right/bottom edge. */
                IntersectClipRect(mem, (int)cmd->p0, (int)cmd->p1,
                                  (int)(cmd->p0 + cmd->p2),
                                  (int)(cmd->p1 + cmd->p3));
                break;
            case CV_LINE: {
                MoveToEx(mem, (int)cmd->p0, (int)cmd->p1, NULL);
                LineTo(mem, (int)cmd->p2, (int)cmd->p3);
                break;
            }
            case CV_STROKE: {
                if (cur_pen) DeleteObject(cur_pen);
                int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                    bi = (int)(cmd->cb * 255);
                // Geometric pen so the recorded cap/join are honoured —
                // a cosmetic CreatePen ignores both (the same class of
                // silent divergence the mac replay had with its hardcoded
                // round cap, caught by the stroker suite's pixel gate).
                DWORD style = PS_GEOMETRIC | PS_SOLID;
                if (cmd->cap == 0) style |= PS_ENDCAP_FLAT;
                else if (cmd->cap == 2) style |= PS_ENDCAP_SQUARE;
                else style |= PS_ENDCAP_ROUND;
                if (cmd->join == 0) style |= PS_JOIN_MITER;
                else if (cmd->join == 2) style |= PS_JOIN_BEVEL;
                else style |= PS_JOIN_ROUND;
                LOGBRUSH lb = { BS_SOLID, RGB(ri, gi, bi), 0 };
                cur_pen = ExtCreatePen(style, (DWORD)cmd->p0, &lb, 0, NULL);
                SelectObject(mem, cur_pen);
                // The stroke command arrives AFTER its path (cairo strokes
                // the accumulated path retroactively; the vg dispatch is
                // built around that). GDI has no retained path here — the
                // CV_LINE segments already drew with whatever pen was
                // current, i.e. the WRONG one. Mirror CV_FILL: walk back to
                // the path's CV_BEGIN and redraw every segment with the pen
                // this stroke actually specifies. (Without this, a stroked
                // vg path rendered as hairline default-pen lines — the
                // stroker suite's first honest win32 run measured native
                // ink at 60 samples against the outline's 323.)
                for (int j = i - 1; j >= 0; j--) {
                    CanvasCmdKind k = cv->cmds[j].k;
                    if (k == CV_BEGIN) break;
                    if (k == CV_LINE) {
                        MoveToEx(mem, (int)cv->cmds[j].p0, (int)cv->cmds[j].p1, NULL);
                        LineTo(mem, (int)cv->cmds[j].p2, (int)cv->cmds[j].p3);
                    }
                }
                break;
            }
            case CV_FILL_RECT: {
                int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                    bi = (int)(cmd->cb * 255);
                HBRUSH br = CreateSolidBrush(RGB(ri, gi, bi));
                RECT r = { (int)cmd->p0, (int)cmd->p1,
                           (int)(cmd->p0 + cmd->p2), (int)(cmd->p1 + cmd->p3) };
                FillRect(mem, &r, br);
                DeleteObject(br);
                break;
            }
            case CV_ARC: {
                // Outline the circle/arc. GDI's Arc takes a bounding box
                // + two radial points; for a full circle (a0=0,a1=2π)
                // we use Ellipse. Stroke colour comes from the current pen.
                //
                // Ellipse() also FILLS with the DC's selected brush, which
                // by default is the stock WHITE one -- so an unfilled arc
                // used to punch a white disc into the scene. Select the
                // NULL brush so this draws an outline only; a genuine fill
                // is CV_FILL's job (which handles arcs, just below).
                int cx = (int)cmd->p0, cy = (int)cmd->p1, rad = (int)cmd->p2;
                HGDIOBJ prev_br = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Ellipse(mem, cx - rad, cy - rad, cx + rad, cy + rad);
                SelectObject(mem, prev_br);
                break;
            }
            case CV_CLOSE:
                break;  // polygon close handled in CV_FILL accumulation
            case CV_FILL: {
                // Accumulate the points from MOVE/LINE/ARC commands since
                // the last BEGIN into a polygon, fill with the given color.
                //
                // A circle is recorded as ONE CV_ARC and contributes no
                // MOVE/LINE points, so the polygon walk below yielded
                // np == 0 and skipped the fill entirely. `fill(circle(...),
                // "#d9a13c")` therefore painted nothing, and the disc that
                // appeared was Ellipse()'s stock-white interior. Fill the
                // ellipse directly with the requested colour instead.
                {
                    int arc_at = -1;
                    for (int j = i - 1; j >= 0; j--) {
                        CanvasCmdKind k = cv->cmds[j].k;
                        if (k == CV_BEGIN) break;
                        if (k == CV_ARC) { arc_at = j; break; }
                    }
                    if (arc_at >= 0) {
                        int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                            bi = (int)(cmd->cb * 255);
                        int acx = (int)cv->cmds[arc_at].p0;
                        int acy = (int)cv->cmds[arc_at].p1;
                        int ar  = (int)cv->cmds[arc_at].p2;
                        HBRUSH br = CreateSolidBrush(RGB(ri, gi, bi));
                        HBRUSH oldbr = (HBRUSH)SelectObject(mem, br);
                        HGDIOBJ oldpen = SelectObject(mem, GetStockObject(NULL_PEN));
                        Ellipse(mem, acx - ar, acy - ar, acx + ar, acy + ar);
                        SelectObject(mem, oldpen);
                        SelectObject(mem, oldbr);
                        DeleteObject(br);
                        break;
                    }
                }
                POINT pts[256];
                int np = 0;
                for (int j = i - 1; j >= 0 && np < 256; j--) {
                    CanvasCmdKind k = cv->cmds[j].k;
                    if (k == CV_BEGIN) break;
                    if (k == CV_MOVE) {
                        pts[np].x = (int)cv->cmds[j].p0;
                        pts[np].y = (int)cv->cmds[j].p1;
                        np++;
                    } else if (k == CV_LINE) {
                        pts[np].x = (int)cv->cmds[j].p2;
                        pts[np].y = (int)cv->cmds[j].p3;
                        np++;
                    }
                }
                if (np >= 3) {
                    int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                        bi = (int)(cmd->cb * 255);
                    HBRUSH br = CreateSolidBrush(RGB(ri, gi, bi));
                    HBRUSH oldbr = (HBRUSH)SelectObject(mem, br);
                    Polygon(mem, pts, np);
                    SelectObject(mem, oldbr);
                    DeleteObject(br);
                }
                break;
            }
            case CV_FILL_LINEAR:
            case CV_FILL_RADIAL: {
                // Plain GDI has no multi-stop gradient. Approximate:
                // fill the path's region with a 2-stop GradientFill
                // (first → last stop) across the path bounding box,
                // clipped to the accumulated path. Radial degrades to
                // the same axis-aligned approximation. Good enough for
                // a first pass; GDI+ would be needed for true fidelity.
                if (cmd->n_stops >= 1) {
                    // Bounding box from the points accumulated since BEGIN.
                    int have = 0, mnx = 0, mny = 0, mxx = 0, mxy = 0;
                    for (int j = i - 1; j >= 0; j--) {
                        CanvasCmdKind k = cv->cmds[j].k;
                        if (k == CV_BEGIN) break;
                        int qx, qy, ok = 0;
                        if (k == CV_MOVE) { qx=(int)cv->cmds[j].p0; qy=(int)cv->cmds[j].p1; ok=1; }
                        else if (k == CV_LINE) { qx=(int)cv->cmds[j].p2; qy=(int)cv->cmds[j].p3; ok=1; }
                        if (ok) {
                            if (!have) { mnx=mxx=qx; mny=mxy=qy; have=1; }
                            else { if(qx<mnx)mnx=qx; if(qx>mxx)mxx=qx; if(qy<mny)mny=qy; if(qy>mxy)mxy=qy; }
                        }
                    }
                    if (have && mxx > mnx && mxy > mny) {
                        int s0 = 0, s1 = cmd->n_stops - 1;
                        TRIVERTEX v[2];
                        v[0].x = mnx; v[0].y = mny;
                        v[0].Red   = (COLOR16)(cmd->stop_rgba[s0*4+0]*65535);
                        v[0].Green = (COLOR16)(cmd->stop_rgba[s0*4+1]*65535);
                        v[0].Blue  = (COLOR16)(cmd->stop_rgba[s0*4+2]*65535);
                        v[0].Alpha = 0;
                        v[1].x = mxx; v[1].y = mxy;
                        v[1].Red   = (COLOR16)(cmd->stop_rgba[s1*4+0]*65535);
                        v[1].Green = (COLOR16)(cmd->stop_rgba[s1*4+1]*65535);
                        v[1].Blue  = (COLOR16)(cmd->stop_rgba[s1*4+2]*65535);
                        v[1].Alpha = 0;
                        GRADIENT_RECT gr = { 0, 1 };
                        GradientFill(mem, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
                    }
                }
                break;
            }
            case CV_FILL_TEXT: {
                if (cmd->text) {
                    int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                        bi = (int)(cmd->cb * 255);
                    HFONT font = CreateFontA((int)cmd->p2, 0, 0, 0, FW_NORMAL,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, NULL);
                    HFONT oldfont = (HFONT)SelectObject(mem, font);
                    SetTextColor(mem, RGB(ri, gi, bi));
                    SetBkMode(mem, TRANSPARENT);
                    // cairo origin is baseline; GDI TextOut is top-left.
                    // Offset up by ~font ascent (≈0.8 of size) for parity.
                    TextOutA(mem, (int)cmd->p0, (int)(cmd->p1 - cmd->p2 * 0.8f),
                             cmd->text, (int)strlen(cmd->text));
                    SelectObject(mem, oldfont);
                    DeleteObject(font);
                }
                break;
            }
            case CV_DRAW_IMAGE: {
                if (cmd->pixels && cmd->iw > 0 && cmd->ih > 0) {
                    // Source is RGBA8888 top-down. Build a top-down
                    // 32bpp BGRA DIB (negative height) and StretchDIBits.
                    // GDI ignores alpha for plain blits; swizzle R/B and
                    // (approximately) composite onto the white backing by
                    // premultiplying against white where alpha < 255.
                    int n = cmd->iw * cmd->ih;
                    unsigned char* conv = (unsigned char*)malloc(n * 4);
                    if (conv) {
                        for (int px = 0; px < n; px++) {
                            unsigned char sr = cmd->pixels[px*4+0];
                            unsigned char sg = cmd->pixels[px*4+1];
                            unsigned char sb = cmd->pixels[px*4+2];
                            unsigned char sa = cmd->pixels[px*4+3];
                            int inv = 255 - sa;
                            conv[px*4+0] = (unsigned char)((sb*sa + 255*inv)/255); // B
                            conv[px*4+1] = (unsigned char)((sg*sa + 255*inv)/255); // G
                            conv[px*4+2] = (unsigned char)((sr*sa + 255*inv)/255); // R
                            conv[px*4+3] = 255;
                        }
                        BITMAPINFO bi = {0};
                        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bi.bmiHeader.biWidth = cmd->iw;
                        bi.bmiHeader.biHeight = -cmd->ih; // top-down
                        bi.bmiHeader.biPlanes = 1;
                        bi.bmiHeader.biBitCount = 32;
                        bi.bmiHeader.biCompression = BI_RGB;
                        /* Dest extent: p2/p3 carry the SCALED size when
                           the command came from draw_image_scaled; zero
                           means an unscaled draw_image. StretchDIBits
                           scales source to dest natively. */
                        int ddw = cmd->p2 > 0 ? (int)cmd->p2 : cmd->iw;
                        int ddh = cmd->p3 > 0 ? (int)cmd->p3 : cmd->ih;
                        SetStretchBltMode(mem, HALFTONE);
                        StretchDIBits(mem, (int)cmd->p0, (int)cmd->p1,
                            ddw, ddh, 0, 0, cmd->iw, cmd->ih,
                            conv, &bi, DIB_RGB_COLORS, SRCCOPY);
                        free(conv);
                    }
                }
                break;
            }
            default: break;
        }
    }

    SelectObject(mem, old_pen);
    if (cur_pen) DeleteObject(cur_pen);
}

// GDI+ replay, built case by case. Anything not yet ported falls through to
// the GDI implementation for that command, so the two renderers stay
// comparable at every step rather than only when the port is complete.
//
// The flat C API (gdiplus/gdiplus.h, GdipXxx) is used deliberately: GDI+'s
// C++ surface is a thin wrapper over it, and Direct2D -- the other candidate
// -- is COM, which is far heavier to consume from C. Probed on winbaz before
// this landed: startup, graphics-from-HDC, an ARGB brush and a fill all work
// under MinGW.
//
// Declared by hand, matching this file's existing idiom (see the
// GdiplusStartup / GdipCreateBitmapFromFile blocks above): <gdiplus.h> is
// C++-only, and its C sibling collides with those declarations. GDI+ is
// ALREADY initialised here for image loading -- ensure_gdiplus() -- so this
// reuses that rather than starting a second token.
typedef void GpGraphics;
typedef void GpBrush;
typedef unsigned int ARGB;
__declspec(dllimport) int __stdcall GdipCreateFromHDC(HDC hdc, GpGraphics** g);
__declspec(dllimport) int __stdcall GdipDeleteGraphics(GpGraphics* g);
__declspec(dllimport) int __stdcall GdipCreateSolidFill(ARGB color, GpBrush** brush);
__declspec(dllimport) int __stdcall GdipDeleteBrush(GpBrush* brush);
__declspec(dllimport) int __stdcall GdipFillRectangleI(GpGraphics* g, GpBrush* brush,
                                                      int x, int y, int w, int h);
typedef void GpPen;
__declspec(dllimport) int __stdcall GdipCreatePen1(ARGB color, float width, int unit,
                                                   GpPen** pen);
__declspec(dllimport) int __stdcall GdipDeletePen(GpPen* pen);
/* A pen carrying a BRUSH rather than a flat colour -- the whole reason a
   gradient stroke is expressible at all. Plain GDI has no equivalent, which
   is why legacy had to approximate gradient strokes as fills. */
__declspec(dllimport) int __stdcall GdipCreatePen2(GpBrush* brush, float width,
                                                   int unit, GpPen** pen);
/* GdipDrawPath is declared alongside GdipFillPath below, once GpPath exists. */
__declspec(dllimport) int __stdcall GdipSetPenStartCap(GpPen* pen, int cap);
__declspec(dllimport) int __stdcall GdipSetPenEndCap(GpPen* pen, int cap);
__declspec(dllimport) int __stdcall GdipSetPenLineJoin(GpPen* pen, int join);
__declspec(dllimport) int __stdcall GdipDrawLineI(GpGraphics* g, GpPen* pen,
                                                  int x1, int y1, int x2, int y2);
__declspec(dllimport) int __stdcall GdipSetSmoothingMode(GpGraphics* g, int mode);
typedef struct { int X, Y; } GpPointI;
__declspec(dllimport) int __stdcall GdipFillPolygonI(GpGraphics* g, GpBrush* brush,
                                                     const GpPointI* points, int count,
                                                     int fillMode);
__declspec(dllimport) int __stdcall GdipFillEllipseI(GpGraphics* g, GpBrush* brush,
                                                     int x, int y, int w, int h);
__declspec(dllimport) int __stdcall GdipDrawEllipseI(GpGraphics* g, GpPen* pen,
                                                     int x, int y, int w, int h);
#define GDIP_FILLMODE_ALTERNATE 0
/* SVG's DEFAULT fill-rule is nonzero, which is GDI+'s FillModeWinding -- and
   also cairo's default, which is why GTK4 matches librsvg without ever
   setting a rule. Every path here was built FillModeAlternate (even-odd), so
   any shape whose sub-paths OVERLAP got a hole punched through the overlap:
   no.svg's X, two crossing arms, showed a white diamond dead centre. */
#define GDIP_FILLMODE_WINDING   1
typedef void GpFontFamily;
typedef void GpFont;
typedef void GpStringFormat;
typedef struct { float X, Y, Width, Height; } GpRectF;
__declspec(dllimport) int __stdcall GdipCreateFontFamilyFromName(
    const unsigned short* name, void* collection, GpFontFamily** family);
__declspec(dllimport) int __stdcall GdipDeleteFontFamily(GpFontFamily* family);
__declspec(dllimport) int __stdcall GdipCreateFont(const GpFontFamily* family,
    float emSize, int style, int unit, GpFont** font);
__declspec(dllimport) int __stdcall GdipDeleteFont(GpFont* font);
__declspec(dllimport) int __stdcall GdipDrawString(GpGraphics* g,
    const unsigned short* str, int length, const GpFont* font,
    const GpRectF* layoutRect, const GpStringFormat* format, const GpBrush* brush);
__declspec(dllimport) int __stdcall GdipSetTextRenderingHint(GpGraphics* g, int mode);
#define GDIP_TEXT_ANTIALIAS      4   /* TextRenderingHintAntiAliasGridFit */
/* Font metrics, in design units, for a real baseline rather than a fudge. */
__declspec(dllimport) int __stdcall GdipGetCellAscent(const GpFontFamily* fam,
    int style, unsigned short* ascent);
__declspec(dllimport) int __stdcall GdipGetEmHeight(const GpFontFamily* fam,
    int style, unsigned short* emHeight);
#define GDIP_FONTSTYLE_REGULAR   0
/* StringFormat: GdipDrawString pads the origin with the font's leading unless
   told otherwise. GenericTypographic has no padding. */
__declspec(dllimport) int __stdcall GdipStringFormatGetGenericTypographic(
    GpStringFormat** format);
typedef void GpLineGradient;
typedef void GpPathGradient;
typedef void GpPath;
typedef struct { float X, Y; } GpPointF;
__declspec(dllimport) int __stdcall GdipCreateLineBrushI(const GpPointI* p1,
    const GpPointI* p2, ARGB c1, ARGB c2, int wrapMode, GpLineGradient** brush);
__declspec(dllimport) int __stdcall GdipSetLinePresetBlend(GpLineGradient* brush,
    const ARGB* blend, const float* positions, int count);
/* SVG spreadMethod. WrapModeClamp(4) is NOT usable for pad: on a
   LinearGradientBrush it renders everything outside the vector as
   transparent rather than holding the end colours, which is a different
   bug, not a fix. GdipSetLineGammaCorrection is unrelated. The reliable
   spelling of "pad" is TileFlipXY-free clamping via the preset blend, so
   pad is implemented by EXTENDING the brush's own vector instead -- see
   the CV_FILL_LINEAR case. This setter is still needed for reflect/repeat. */
__declspec(dllimport) int __stdcall GdipSetLineWrapMode(GpLineGradient* brush,
    int wrapMode);
#define GDIP_WRAP_TILE_FLIPX 1
#define GDIP_WRAP_CLAMP      4
__declspec(dllimport) int __stdcall GdipCreatePath(int fillMode, GpPath** path);
__declspec(dllimport) int __stdcall GdipDeletePath(GpPath* path);
__declspec(dllimport) int __stdcall GdipAddPathEllipseI(GpPath* path,
    int x, int y, int w, int h);
__declspec(dllimport) int __stdcall GdipCreatePathGradientFromPath(
    const GpPath* path, GpPathGradient** brush);
__declspec(dllimport) int __stdcall GdipSetPathGradientCenterColor(
    GpPathGradient* brush, ARGB color);
__declspec(dllimport) int __stdcall GdipSetPathGradientPresetBlend(
    GpPathGradient* brush, const ARGB* blend, const float* positions, int count);
#define GDIP_WRAP_TILE 0
/* Alpha blit. PixelFormat32bppARGB is 0x26200A: 32bpp, alpha, BGRA order in
   memory on little-endian, which is why the source RGBA needs swizzling. */
__declspec(dllimport) int __stdcall GdipCreateBitmapFromScan0(int width, int height,
    int stride, int format, unsigned char* scan0, void** bitmap);
__declspec(dllimport) int __stdcall GdipDrawImageRectI(GpGraphics* g, void* image,
    int x, int y, int width, int height);
#define GDIP_FMT_32BPP_ARGB 0x0026200A
/* Offscreen group layers (CV_GROUP_BEGIN/END). A bitmap gets its own
   GpGraphics to draw into, then is composited back through an ImageAttributes
   colour matrix whose alpha row scales by the group alpha -- GDI+'s
   equivalent of cairo_paint_with_alpha. */
__declspec(dllimport) int __stdcall GdipGetImageGraphicsContext(void* image,
    GpGraphics** graphics);
__declspec(dllimport) int __stdcall GdipCreateImageAttributes(void** imageattr);
__declspec(dllimport) int __stdcall GdipDisposeImageAttributes(void* imageattr);
__declspec(dllimport) int __stdcall GdipSetImageAttributesColorMatrix(
    void* imageattr, int type, int enableFlag, const void* colorMatrix,
    const void* grayMatrix, int flags);
__declspec(dllimport) int __stdcall GdipDrawImageRectRectI(GpGraphics* g,
    void* image, int dstx, int dsty, int dstw, int dsth,
    int srcx, int srcy, int srcw, int srch, int srcUnit,
    const void* imageAttributes, void* callback, void* callbackData);
__declspec(dllimport) int __stdcall GdipGetImageWidth(void* image, UINT* w);
__declspec(dllimport) int __stdcall GdipGetImageHeight(void* image, UINT* h);

/* contain/cover for image widgets: keep the ORIGINAL bitmap per handle and
   show a derived DIB scaled into the widget's box — contain letterboxes
   (transparent bars), cover centre-crops. original/stretch restore the
   original (stretch via SS_REALSIZECONTROL, as before). */
static HBITMAP* w32_imgfill_orig = NULL;
static int      w32_imgfill_len  = 0;
static void w32_imgfill_slots(int handle) {
    if (handle <= w32_imgfill_len) return;
    int want = handle + 8;
    w32_imgfill_orig = (HBITMAP*)realloc(w32_imgfill_orig, sizeof(HBITMAP) * want);
    for (int i = w32_imgfill_len; i < want; i++) w32_imgfill_orig[i] = NULL;
    w32_imgfill_len = want;
}

static void w32_image_apply_fill(int handle) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_IMAGE || !w->hwnd) return;
    w32_imgfill_slots(handle);
    LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
    if ((st & SS_TYPEMASK) == SS_ICON) return;   /* file icons: no fill modes */
    int mode = w->image_fill;

    HBITMAP orig = w32_imgfill_orig[handle - 1];
    if (!orig) {
        orig = (HBITMAP)SendMessageW(w->hwnd, STM_GETIMAGE, IMAGE_BITMAP, 0);
        if (!orig) return;
        w32_imgfill_orig[handle - 1] = orig;
    }

    if (mode == 0 || mode == 3) {
        if (mode == 3) st |= SS_REALSIZECONTROL; else st &= ~SS_REALSIZECONTROL;
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
        HBITMAP shown = (HBITMAP)SendMessageW(w->hwnd, STM_SETIMAGE,
                                              IMAGE_BITMAP, (LPARAM)orig);
        if (shown && shown != orig) DeleteObject(shown);
        return;
    }

    int bw = w->pref_width, bh = w->pref_height;
    if (bw <= 0 || bh <= 0) {
        RECT r; GetClientRect(w->hwnd, &r);
        bw = r.right; bh = r.bottom;
    }
    if (bw <= 0 || bh <= 0) return;

    ensure_gdiplus();
    void* src = NULL;
    if (GdipCreateBitmapFromHBITMAP(orig, NULL, &src) != 0 || !src) return;
    UINT iw = 0, ih = 0;
    GdipGetImageWidth(src, &iw);
    GdipGetImageHeight(src, &ih);
    if (iw == 0 || ih == 0) { GdipDisposeImage(src); return; }

    void* dstbmp = NULL;
    if (GdipCreateBitmapFromScan0(bw, bh, 0, GDIP_FMT_32BPP_ARGB, NULL, &dstbmp) != 0
        || !dstbmp) { GdipDisposeImage(src); return; }
    GpGraphics* g = NULL;
    if (GdipGetImageGraphicsContext(dstbmp, &g) == 0 && g) {
        double sx = (double)bw / iw, sy = (double)bh / ih;
        if (mode == 1) {                     /* contain: fit, letterbox */
            double sc = sx < sy ? sx : sy;
            int dw = (int)(iw * sc + 0.5), dh = (int)(ih * sc + 0.5);
            GdipDrawImageRectRectI(g, src, (bw - dw) / 2, (bh - dh) / 2, dw, dh,
                                   0, 0, (int)iw, (int)ih, 2 /*UnitPixel*/,
                                   NULL, NULL, NULL);
        } else {                             /* cover: fill, centre-crop */
            double sc = sx > sy ? sx : sy;
            int sw2 = (int)(bw / sc + 0.5), sh2 = (int)(bh / sc + 0.5);
            if (sw2 > (int)iw) sw2 = (int)iw;
            if (sh2 > (int)ih) sh2 = (int)ih;
            GdipDrawImageRectRectI(g, src, 0, 0, bw, bh,
                                   ((int)iw - sw2) / 2, ((int)ih - sh2) / 2,
                                   sw2, sh2, 2 /*UnitPixel*/,
                                   NULL, NULL, NULL);
        }
        GdipDeleteGraphics(g);
    }
    HBITMAP derived = NULL;
    GdipCreateHBITMAPFromBitmap(dstbmp, &derived, 0);
    GdipDisposeImage(dstbmp);
    GdipDisposeImage(src);
    if (!derived) return;
    st &= ~SS_REALSIZECONTROL;
    SetWindowLongPtrW(w->hwnd, GWL_STYLE, st);
    HBITMAP shown = (HBITMAP)SendMessageW(w->hwnd, STM_SETIMAGE,
                                          IMAGE_BITMAP, (LPARAM)derived);
    if (shown && shown != orig) DeleteObject(shown);
}
/* Path construction + clipping: what turns a gradient from "fill the bounding
   box" into "fill the actual shape". */
__declspec(dllimport) int __stdcall GdipAddPathLine2I(GpPath* path,
    const GpPointI* points, int count);
__declspec(dllimport) int __stdcall GdipClosePathFigure(GpPath* path);
__declspec(dllimport) int __stdcall GdipSetClipPath(GpGraphics* g, GpPath* path,
    int combineMode);
__declspec(dllimport) int __stdcall GdipResetClip(GpGraphics* g);
/* CombineMode 1 = Intersect, which is the documented semantic of
   canvas_clip_rect: narrow the CURRENT clip, never widen it. */
#define GDIP_COMBINE_INTERSECT 1
__declspec(dllimport) int __stdcall GdipSetClipRectI(GpGraphics* g,
    int x, int y, int width, int height, int combineMode);
__declspec(dllimport) int __stdcall GdipStartPathFigure(GpPath* path);
__declspec(dllimport) int __stdcall GdipFillPath(GpGraphics* g, GpBrush* brush,
                                                 GpPath* path);
/* World transform, for a ROTATED gradient ellipse. GDI+ fills an
   axis-aligned ellipse only, so the tilt has to come from the CTM. */
typedef void GpMatrix;
__declspec(dllimport) int __stdcall GdipCreateMatrix(GpMatrix** matrix);
__declspec(dllimport) int __stdcall GdipDeleteMatrix(GpMatrix* matrix);
__declspec(dllimport) int __stdcall GdipTranslateMatrix(GpMatrix* matrix,
    float offsetX, float offsetY, int order);
__declspec(dllimport) int __stdcall GdipRotateMatrix(GpMatrix* matrix,
    float angle, int order);
__declspec(dllimport) int __stdcall GdipGetWorldTransform(GpGraphics* g,
    GpMatrix* matrix);
__declspec(dllimport) int __stdcall GdipSetWorldTransform(GpGraphics* g,
    GpMatrix* matrix);
#define GDIP_MATRIX_PREPEND 0
/* Stroke a path with a pen -- the gradient-stroke case. Declared here rather
   than with the other pen functions because GpPath is only typedef'd above. */
__declspec(dllimport) int __stdcall GdipDrawPath(GpGraphics* g, GpPen* pen,
                                                 GpPath* path);
/* Glyph outlines as strokable geometry -- how a stroked <text> is drawn,
   since GdipDrawString can only fill. */
typedef struct { INT X, Y, Width, Height; } GpRectI;
__declspec(dllimport) int __stdcall GdipAddPathStringI(GpPath* path,
    const unsigned short* str, int length, const GpFontFamily* family,
    int style, float emSize, const GpRectI* layoutRect, const void* format);
#define GDIP_COMBINE_REPLACE 0
/* Radial gradients: centre point and rim colour, so an SVG <radialGradient>
   with a focal point (fx,fy) distinct from its centre renders correctly. */
__declspec(dllimport) int __stdcall GdipSetPathGradientCenterPointI(
    GpPathGradient* brush, const GpPointI* point);
__declspec(dllimport) int __stdcall GdipSetPathGradientSurroundColorsWithCount(
    GpPathGradient* brush, const ARGB* colors, int* count);
#define GDIP_UNIT_PIXEL       2
#define GDIP_SMOOTHING_AA     4
/* LineCap: Flat=0 Square=1 Round=2. LineJoin: Miter=0 Bevel=1 Round=2.
   NOT the same numbering as our recorded cap/join (flat=0 round=1 square=2,
   miter=0 round=1 bevel=2), so both need an explicit map -- a straight
   pass-through would silently swap square for round. */
static int gdip_cap(int our) { return our == 0 ? 0 : (our == 2 ? 1 : 2); }
static int gdip_join(int our) { return our == 0 ? 0 : (our == 2 ? 1 : 2); }

static int w32_clamp_byte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* 0xAARRGGBB from a command's float colour. THE POINT OF THE EXERCISE:
   GDI's CreateSolidBrush(RGB(...)) has nowhere to put cmd->calpha -- the
   command buffer HAS carried alpha all along, GDI just discards it at draw
   time -- so every
   translucent fill lands opaque -- which is why Stage 3 frame caching is
   gated off on win32. GDI+ carries it. */
static ARGB gdip_argb(const CanvasCmd* c) {
    int a = (int)(c->calpha * 255.0 + 0.5);
    int r = (int)(c->cr * 255.0 + 0.5);
    int g = (int)(c->cg * 255.0 + 0.5);
    int b = (int)(c->cb * 255.0 + 0.5);
    a = w32_clamp_byte(a);
    r = w32_clamp_byte(r);
    g = w32_clamp_byte(g);
    b = w32_clamp_byte(b);
    return ((ARGB)a << 24) | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
}

/* Resolve a CSS font stack to a GDI+ family, using GDI+'s own matcher --
   which is the point: "best available face for this list" is a platform
   question, so the vg layer ships the raw string and each backend answers it
   natively (fontconfig on GTK4, CoreText on macOS, this here).

   font-family is a PRIORITISED LIST, so walk it in order and take the first
   entry GDI+ can actually create. Generic CSS names are mapped to concrete
   Windows faces, because GdipCreateFontFamilyFromName does not know them.
   Falls back to Segoe UI, which is what this backend hardcoded for every
   glyph before the stack was carried at all.

   NB THE CORPUS SCORES THIS WORSE, and that is expected rather than a
   regression to chase. decimal.svg asks for font-family="serif"; librsvg on
   the Linux reference box resolves that through fontconfig to Noto Serif,
   while Windows has no Noto Serif and this maps it to Times New Roman.
   Both are correct answers to "a serif face" on their own platform, but the
   glyphs differ, so the diff against a Linux-rendered reference rises
   (20.22 -> 46.36) while the family CHOICE became more correct. The old
   hardcoded Segoe UI happened to have a cap-height near librsvg's default,
   which flattered the score. Same failure mode as php.svg earlier: MAE is a
   regression tripwire, not an oracle, and a cross-platform font reference
   is one of the things it cannot judge. */
static GpFontFamily* gdip_resolve_family(const char* stack) {
    GpFontFamily* fam = NULL;
    if (stack && stack[0]) {
        const char* p = stack;
        while (*p) {
            /* One comma-separated entry, trimmed of spaces and quotes. */
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            char name[128];
            int n = 0;
            while (*p && *p != ',' && n < (int)sizeof(name) - 1) {
                if (*p != '\'' && *p != '"') name[n++] = *p;
                p++;
            }
            while (n > 0 && (name[n-1] == ' ' || name[n-1] == '\t')) n--;
            name[n] = 0;
            if (n > 0) {
                /* Generic CSS families -> concrete Windows faces. */
                const char* concrete = name;
                if (_stricmp(name, "serif") == 0)           concrete = "Times New Roman";
                else if (_stricmp(name, "sans-serif") == 0) concrete = "Arial";
                else if (_stricmp(name, "monospace") == 0)  concrete = "Consolas";
                else if (_stricmp(name, "cursive") == 0)    concrete = "Comic Sans MS";
                else if (_stricmp(name, "fantasy") == 0)    concrete = "Impact";
                wchar_t* wname = utf8_to_wide(concrete);
                if (wname &&
                    GdipCreateFontFamilyFromName((const unsigned short*)wname,
                                                 NULL, &fam) == 0 && fam) {
                    return fam;
                }
                fam = NULL;
            }
            if (*p == ',') p++;
        }
    }
    if (GdipCreateFontFamilyFromName((const unsigned short*)L"Segoe UI",
                                     NULL, &fam) == 0 && fam) {
        return fam;
    }
    return NULL;
}

static void canvas_replay_to_dc_gdiplus(Canvas* cv, HDC mem, int width, int height) {
    if (!cv) return;
    ensure_gdiplus();
    GpGraphics* g = NULL;
    if (GdipCreateFromHDC(mem, &g) != 0 || !g) {
        canvas_replay_to_dc_gdi(cv, mem, width, height);
        return;
    }
    /* Match GDI's opening white wash so a partially-ported renderer differs
       only where a PORTED command draws -- otherwise every unported scene
       would score a huge MAE for the background alone and the comparison
       would say nothing. */
    /* Wash FIRST, with smoothing still off. An antialiased axis-aligned
       fill blends its boundary against whatever the DC already held --
       for a fresh memory DC that is the stock C0C0C0 grey -- so the top
       scanline came out grey instead of white. Measured: exactly row y=0,
       24 of 24 samples, while row 1 matched legacy. Fill opaque, then
       enable AA for the drawing that follows. */
    GpBrush* wash = NULL;
    if (GdipCreateSolidFill(0xFFFFFFFF, &wash) == 0) {
        GdipFillRectangleI(g, (GpBrush*)wash, 0, 0, width, height);
        GdipDeleteBrush((GpBrush*)wash);
    }

    /* The reason this renderer exists, alongside alpha. GDI has no AA at
       all, so every diagonal edge is a staircase. */
    GdipSetSmoothingMode(g, GDIP_SMOOTHING_AA);
    /* Start from no clip. The read_pixel path reuses one cached DC across
       calls, so a clip left by the previous replay would intersect with this
       one and shrink the visible area a little more every frame. */
    GdipResetClip(g);

    GpPen* cur_pen = NULL;
    /* Group-layer stack. `g` is redirected at CV_GROUP_BEGIN to draw into an
       offscreen ARGB bitmap and restored at CV_GROUP_END, so every drawing
       case below stays untouched -- they just draw into whatever `g` currently
       points at. Depth is bounded; a deeper nest degrades to drawing inline
       (visually the old behaviour) rather than corrupting the stack. */
    #define GRP_MAX 8
    GpGraphics* grp_saved[GRP_MAX];
    void*       grp_bmp[GRP_MAX];
    int grp_depth = 0;
    for (int i = 0; i < cv->cmd_count; i++) {
        CanvasCmd* cmd = &cv->cmds[i];
        switch (cmd->k) {
            case CV_GROUP_BEGIN: {
                if (grp_depth >= GRP_MAX) break;
                void* bmp = NULL;
                if (GdipCreateBitmapFromScan0(width, height, 0,
                                              GDIP_FMT_32BPP_ARGB, NULL,
                                              &bmp) != 0 || !bmp) break;
                GpGraphics* lg = NULL;
                if (GdipGetImageGraphicsContext(bmp, &lg) != 0 || !lg) {
                    GdipDisposeImage(bmp);
                    break;
                }
                /* The layer starts fully TRANSPARENT -- no white wash. That is
                   the whole point: only what the group actually draws carries
                   alpha into the composite, so the backdrop shows through
                   where the group is empty. */
                GdipSetSmoothingMode(lg, GDIP_SMOOTHING_AA);
                grp_saved[grp_depth] = g;
                grp_bmp[grp_depth] = bmp;
                grp_depth++;
                g = lg;
                break;
            }
            case CV_GROUP_END: {
                if (grp_depth <= 0) break;
                grp_depth--;
                GpGraphics* lg = g;
                g = grp_saved[grp_depth];
                void* bmp = grp_bmp[grp_depth];
                float a = (float)cmd->calpha;
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                /* Identity colour matrix with the alpha row scaled -- the
                   GDI+ spelling of cairo_paint_with_alpha. */
                float cm[5][5] = {
                    {1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,a,0},{0,0,0,0,1}
                };
                void* ia = NULL;
                if (GdipCreateImageAttributes(&ia) == 0 && ia) {
                    /* type 0 = ColorAdjustTypeDefault, flags 0 = ColorMatrixFlagsDefault */
                    GdipSetImageAttributesColorMatrix(ia, 0, 1, cm, NULL, 0);
                    GdipDrawImageRectRectI(g, bmp, 0, 0, width, height,
                                           0, 0, width, height,
                                           GDIP_UNIT_PIXEL, ia, NULL, NULL);
                    GdipDisposeImageAttributes(ia);
                } else {
                    GdipDrawImageRectI(g, bmp, 0, 0, width, height);
                }
                GdipDeleteGraphics(lg);
                GdipDisposeImage(bmp);
                break;
            }
            case CV_CLIP:
                /* Applied to `g`, which the group-layer stack redirects: a
                   clip emitted INSIDE a group therefore applies to that
                   layer, and one emitted outside applies to the frame. The
                   viewport clip AeVG emits at scene-flush start is always
                   outside, which is the case that matters here. */
                GdipSetClipRectI(g, (int)cmd->p0, (int)cmd->p1,
                                 (int)cmd->p2, (int)cmd->p3,
                                 GDIP_COMBINE_INTERSECT);
                break;
            case CV_CLEAR: {
                GpBrush* br = NULL;
                if (GdipCreateSolidFill(0xFFFFFFFF, &br) == 0) {
                    GdipFillRectangleI(g, (GpBrush*)br, 0, 0, width, height);
                    GdipDeleteBrush((GpBrush*)br);
                }
                break;
            }
            case CV_FILL_RECT: {
                GpBrush* br = NULL;
                if (GdipCreateSolidFill(gdip_argb(cmd), &br) == 0 && br) {
                    GdipFillRectangleI(g, (GpBrush*)br,
                                       (INT)cmd->p0, (INT)cmd->p1,
                                       (INT)cmd->p2, (INT)cmd->p3);
                    GdipDeleteBrush((GpBrush*)br);
                }
                break;
            }
            case CV_LINE: {
                /* Legacy relies on a pen SELECTED into the DC; GDI+ takes the
                   pen per call, so the "current pen" is explicit state here.
                   Before any CV_STROKE names one, legacy draws with the DC's
                   default black hairline -- matched exactly, or every
                   pre-stroke segment would differ for a reason that has
                   nothing to do with this port. */
                /* DOUBLE-STROKE FIX. A CV_STROKE later in this sub-path
                   walks back and redraws every CV_LINE with the real pen, so
                   drawing here as well paints the outline TWICE. Legacy gets
                   away with it because both passes use the same DC pen and no
                   antialiasing, landing exactly on top of each other. GDI+
                   does not: the first pass is a 1px black hairline, the second
                   the real pen, and with AA on they smear into a doubled edge.
                   Measured on 410.svg at x=200: librsvg has one black band at
                   y=2..5, GDI+ had that PLUS a second at y=10..11 and darkened
                   the maroon beneath it.

                   So look ahead: if a CV_STROKE is coming before this
                   sub-path ends, leave the drawing to it. */
                int consumer_pending = 0;
                for (int j = i + 1; j < cv->cmd_count; j++) {
                    CanvasCmdKind k2 = cv->cmds[j].k;
                    if (k2 == CV_BEGIN) break;
                    /* CV_FILL and the gradient fills consume the path too --
                       410.svg's second path is fill-ONLY with no stroke, and
                       its hairline still showed as a black band just above the
                       maroon (px y=10..11 where librsvg is white). Any of
                       these four means "someone else will paint this path". */
                    if (k2 == CV_STROKE || k2 == CV_FILL ||
                        k2 == CV_FILL_LINEAR || k2 == CV_FILL_RADIAL) {
                        consumer_pending = 1; break;
                    }
                }
                if (consumer_pending) break;
                GpPen* pen = cur_pen ? cur_pen : NULL;
                int owned = 0;
                if (!pen && GdipCreatePen1(0xFF000000, 1.0f,
                                           GDIP_UNIT_PIXEL, &pen) == 0) owned = 1;
                if (pen) {
                    GdipDrawLineI(g, pen, (INT)cmd->p0, (INT)cmd->p1,
                                  (INT)cmd->p2, (INT)cmd->p3);
                    if (owned) GdipDeletePen(pen);
                }
                break;
            }
            case CV_STROKE: {
                /* Same retroactive walk-back as legacy, and for the same
                   reason: the stroke command arrives AFTER the path it
                   applies to, so the CV_LINE segments have already drawn
                   with the wrong pen. Rewind to CV_BEGIN and redraw. */
                if (cur_pen) { GdipDeletePen(cur_pen); cur_pen = NULL; }
                float w = (float)cmd->p0;
                if (w <= 0.0f) w = 1.0f;
                if (GdipCreatePen1(gdip_argb(cmd), w,
                                   GDIP_UNIT_PIXEL, &cur_pen) != 0) {
                    cur_pen = NULL;
                    break;
                }
                GdipSetPenStartCap(cur_pen, gdip_cap(cmd->cap));
                GdipSetPenEndCap(cur_pen, gdip_cap(cmd->cap));
                GdipSetPenLineJoin(cur_pen, gdip_join(cmd->join));
                /* THE CLOSING SEGMENT. `z` emits CV_CLOSE, not a CV_LINE, so
                   a walk-back that only redraws CV_LINE never strokes the edge
                   from the sub-path's last point back to its first. On
                   410.svg's octagon that is the ENTIRE top-left diagonal:
                   librsvg draws black the whole way from px (4,120) to
                   (120,4); GDI+ had white at every interior sample.
                   Handled in the path assembly below by GdipClosePathFigure
                   at each CV_CLOSE, which closes the CURRENT figure -- the
                   per-sub-path semantics `z` actually has. Computing one
                   closing segment for the whole command, as an earlier cut
                   did, welds the last sub-path back to the first. */
                for (int j = i - 1; j >= 0; j--) {
                    CanvasCmdKind k = cv->cmds[j].k;
                    if (k == CV_BEGIN) break;
                    if (k == CV_ARC) {
                        /* A STROKED CIRCLE emits CV_ARC + CV_STROKE, with no
                           CV_LINE at all. Walking only CV_LINE therefore drew
                           NOTHING: copyright.svg's 10-unit ring was simply
                           absent (sampled at y=200, librsvg black at x=26/30/
                           370/374, GDI+ white at every one), and the file sat
                           at MAE 98.7 with 72% white against librsvg's 57%.
                           Legacy gets the ring by accident -- its CV_ARC calls
                           Ellipse(), which strokes with whatever pen is
                           current -- so this gap is GDI+-only. */
                        int acx = (INT)cv->cmds[j].p0;
                        int acy = (INT)cv->cmds[j].p1;
                        int ar  = (INT)cv->cmds[j].p2;
                        GdipDrawEllipseI(g, cur_pen, acx - ar, acy - ar,
                                         ar * 2, ar * 2);
                    }
                }
                /* ONE path, ONE stroke -- not a GdipDrawLineI per segment.
                   With a translucent pen, per-segment drawing composites the
                   pen alpha AGAIN at every joint where consecutive segments
                   overlap, and a wide round-capped pen overlaps a lot. The
                   arithmetic is exact: #F00 at alpha 0.502 over white gives
                   G/B 127 for one draw, 63 for two, 31 for three, 9 for four.
                   mememe.svg (six strokes at 0.502) measured (255,31,31) and
                   (233,9,31) against librsvg's clean (255,127,127) -- three
                   and four compositings of the same segment.
                   Stroking an assembled path applies the alpha ONCE over the
                   whole figure, which is what cairo does and why GTK4 was
                   right without special-casing anything. Opaque strokes are
                   unaffected (compositing black over black is black), which
                   is why this went unnoticed until a translucent file. */
                {
                    int start = 0;
                    for (int j = i - 1; j >= 0; j--) {
                        if (cv->cmds[j].k == CV_BEGIN) { start = j; break; }
                    }
                    GpPath* sp = NULL;
                    if (GdipCreatePath(GDIP_FILLMODE_ALTERNATE, &sp) == 0 && sp) {
                        /* EVERY sub-path closes ITSELF. The first cut of this
                           computed a single closing segment by walking back to
                           the first CV_BEGIN, then appended it to whichever
                           figure happened to be open -- with two sub-paths
                           that draws a line from the LAST one's end back to
                           the FIRST one's start. no.svg is the demonstration:
                           its X is one path of two closed sub-paths, and the
                           result was a red web down the left side joining the
                           top-left arm to the bottom-left, a ragged instead of
                           square-cut lower-left arm, and a white diamond in
                           the middle where the two arms failed to meet.
                           GdipClosePathFigure closes the CURRENT figure, which
                           is exactly the per-sub-path semantics `z` has. */
                        int segs = 0, fig_open = 0;
                        for (int j = start; j < i; j++) {
                            CanvasCmdKind k = cv->cmds[j].k;
                            if (k == CV_MOVE) {
                                /* START a new figure, do NOT close the old
                                   one: an unclosed sub-path must STAY open.
                                   Closing here adds a phantom segment back to
                                   its start -- mememe.svg's two open `m`
                                   sub-paths carry no `z` at all, and closing
                                   them cost 1.27 -> 6.04. Only CV_CLOSE
                                   (i.e. a real `z`) closes a figure. */
                                GdipStartPathFigure(sp);
                                fig_open = 1;
                            } else if (k == CV_CLOSE) {
                                if (fig_open) {
                                    GdipClosePathFigure(sp);
                                    fig_open = 0;
                                }
                            } else if (k == CV_LINE) {
                                GpPointI seg[2];
                                seg[0].X = (INT)cv->cmds[j].p0;
                                seg[0].Y = (INT)cv->cmds[j].p1;
                                seg[1].X = (INT)cv->cmds[j].p2;
                                seg[1].Y = (INT)cv->cmds[j].p3;
                                if (!fig_open) { GdipStartPathFigure(sp); fig_open = 1; }
                                if (GdipAddPathLine2I(sp, seg, 2) == 0) segs++;
                            }
                        }
                        if (segs > 0) GdipDrawPath(g, cur_pen, sp);
                        GdipDeletePath(sp);
                    }
                }
                break;
            }
            case CV_FILL: {
                /* Same reverse walk-back as legacy: accumulate MOVE/LINE
                   points since CV_BEGIN into a polygon. Reverse order is
                   harmless for a fill (winding is symmetric for a simple
                   closed path) and is kept so both renderers see identical
                   geometry -- the point of the comparison. */
                /* A CV_ARC in the path means this fill is a circle, which
                   has no MOVE/LINE points to accumulate. Legacy gets this
                   WRONG: its polygon walk sees np < 3, skips the fill
                   entirely, and the circle ends up white -- not from any
                   fill, but from Ellipse() in CV_ARC painting with the
                   stock WHITE brush. Measured on golden_gallery canvas 1:
                   36 samples where legacy is FFFFFFFF inside a circle the
                   app asked to fill "#d9a13c". Honour the requested colour
                   instead; this is a place the two renderers SHOULD differ. */
                int arc_at = -1;
                for (int j = i - 1; j >= 0; j--) {
                    CanvasCmdKind k = cv->cmds[j].k;
                    if (k == CV_BEGIN) break;
                    if (k == CV_ARC) { arc_at = j; break; }
                }
                if (arc_at >= 0) {
                    GpBrush* br = NULL;
                    if (GdipCreateSolidFill(gdip_argb(cmd), &br) == 0 && br) {
                        int acx = (INT)cv->cmds[arc_at].p0;
                        int acy = (INT)cv->cmds[arc_at].p1;
                        int ar  = (INT)cv->cmds[arc_at].p2;
                        GdipFillEllipseI(g, br, acx - ar, acy - ar, ar * 2, ar * 2);
                        GdipDeleteBrush(br);
                    }
                    break;
                }
                /* SUB-PATHS ARE SEPARATE FIGURES. The old code flattened
                   every CV_MOVE/CV_LINE back to CV_BEGIN into ONE point array
                   and filled it as a single polygon, so a path made of several
                   disjoint runs came out joined. git.svg is the clean case: its
                   green path is six sub-paths (three horizontal runs, three
                   verticals) with no fill attribute, so SVG's default black
                   fill applies -- but filling six OPEN figures yields almost no
                   area, which is why librsvg and GTK4 show none. Merging them
                   produced a large bogus polygon: at y=195 librsvg has three
                   spans (90,109) (173,193) (256,276) and GDI+ had one
                   continuous (90,276).

                   cairo gets this right for free because CANVAS_FILL fills its
                   own retained path, which already knows about move_to. Build
                   the equivalent GpPath, starting a new figure at each
                   CV_MOVE. Walk FORWARD here -- figure order matters. */
                int start = 0;
                for (int j = i - 1; j >= 0; j--) {
                    if (cv->cmds[j].k == CV_BEGIN) { start = j + 1; break; }
                    if (j == 0) start = 0;
                }
                GpPath* fp = NULL;
                if (GdipCreatePath(cmd->even_odd ? GDIP_FILLMODE_ALTERNATE
                                                    : GDIP_FILLMODE_WINDING,
                                       &fp) == 0 && fp) {
                    int fig_open = 0, segs = 0;
                    for (int j = start; j < i; j++) {
                        CanvasCmdKind k = cv->cmds[j].k;
                        if (k == CV_MOVE) {
                            if (fig_open) GdipClosePathFigure(fp);
                            GdipStartPathFigure(fp);
                            fig_open = 1;
                        } else if (k == CV_LINE) {
                            GpPointI seg[2];
                            seg[0].X = (INT)cv->cmds[j].p0; seg[0].Y = (INT)cv->cmds[j].p1;
                            seg[1].X = (INT)cv->cmds[j].p2; seg[1].Y = (INT)cv->cmds[j].p3;
                            if (!fig_open) { GdipStartPathFigure(fp); fig_open = 1; }
                            GdipAddPathLine2I(fp, seg, 2);
                            segs++;
                        }
                    }
                    if (segs >= 2) {
                        GpBrush* br = NULL;
                        if (GdipCreateSolidFill(gdip_argb(cmd), &br) == 0 && br) {
                            GdipFillPath(g, br, fp);
                            GdipDeleteBrush(br);
                        }
                    }
                    GdipDeletePath(fp);
                }
                break;
            }
            case CV_ARC: {
                /* Legacy uses Ellipse(), which both FILLS with the current
                   brush and OUTLINES with the current pen. GDI+ separates
                   those, so to match we stroke with the current pen only --
                   legacy's brush here is whatever was last selected, which
                   for the vg dispatch is the stock brush, i.e. no fill. */
                int cx = (INT)cmd->p0, cy = (INT)cmd->p1, rad = (INT)cmd->p2;
                /* DON'T DRAW WITH A STALE PEN. cur_pen is whatever the LAST
                   CV_STROKE set, and it persists across commands -- so a
                   circle that carries its own stroke got outlined here with
                   the PREVIOUS element's pen, before its own CV_STROKE
                   arrived. Traced: caution.svg's r=24 dot stroked at width 36
                   (the `!` stem's stroke-width 9 x scale 4), giving an outer
                   radius of 42 against librsvg's 26 -- the oversized dot that
                   touches the stem. basura.svg's r=24 head stroked at width 48
                   (the ring's stroke-width 12), which is the "green halo"
                   eating the ring's inner edge.

                   Same look-ahead the CV_LINE case uses: if a consumer is
                   coming for this path, leave it to them. CV_STROKE's
                   walk-back already redraws CV_ARC with the correct pen, so
                   drawing here was redundant as well as wrong. */
                int arc_consumer = 0;
                for (int j = i + 1; j < cv->cmd_count; j++) {
                    CanvasCmdKind k2 = cv->cmds[j].k;
                    if (k2 == CV_BEGIN) break;
                    if (k2 == CV_STROKE || k2 == CV_FILL ||
                        k2 == CV_FILL_LINEAR || k2 == CV_FILL_RADIAL) {
                        arc_consumer = 1; break;
                    }
                }
                if (arc_consumer) break;
                GpPen* pen = cur_pen;
                int owned = 0;
                if (!pen && GdipCreatePen1(0xFF000000, 1.0f,
                                           GDIP_UNIT_PIXEL, &pen) == 0) owned = 1;
                if (pen) {
                    GdipDrawEllipseI(g, pen, cx - rad, cy - rad, rad * 2, rad * 2);
                    if (owned) GdipDeletePen(pen);
                }
                break;
            }
            case CV_FILL_LINEAR:
            case CV_FILL_RADIAL: {
                /* THE CASE GDI CANNOT DO. Legacy's own comment concedes it:
                   plain GDI has no multi-stop gradient, so it collapses N
                   stops to the first and last, fills an axis-aligned
                   GradientFill across the bounding box, and degrades RADIAL
                   to that same horizontal ramp. GDI+ has both brushes and
                   honours every stop. */
                if (cmd->n_stops < 1) break;
                int have = 0, mnx = 0, mny = 0, mxx = 0, mxy = 0;
                /* The path's own points, for clipping the fill to the shape
                   rather than to its bounding box. Same reverse walk-back the
                   CV_FILL case uses. */
                /* 2048, not 256. The cap silently TRUNCATED the clip polygon
                   for detailed paths, and the corpus hits it: rg1024_eggs.svg
                   traced np=256 on three of the cracked-shell commands, whose
                   bbox is exactly where the left shell's interior rendered
                   pure white instead of shaded. A truncated polygon is a
                   wrong-shaped clip, so the gradient landed outside the shape
                   it was meant to fill. */
                GpPointI pts[2048];
                int np = 0;
                for (int j = i - 1; j >= 0; j--) {
                    CanvasCmdKind k = cv->cmds[j].k;
                    if (k == CV_BEGIN) break;
                    int qx, qy, ok = 0;
                    if (k == CV_MOVE) { qx=(INT)cv->cmds[j].p0; qy=(INT)cv->cmds[j].p1; ok=1; }
                    else if (k == CV_LINE) { qx=(INT)cv->cmds[j].p2; qy=(INT)cv->cmds[j].p3; ok=1; }
                    else if (k == CV_ARC) {
                        int ax=(INT)cv->cmds[j].p0, ay=(INT)cv->cmds[j].p1,
                            ar=(INT)cv->cmds[j].p2;
                        if (!have) { mnx=ax-ar; mxx=ax+ar; mny=ay-ar; mxy=ay+ar; have=1; }
                        continue;
                    }
                    if (ok) {
                        if (np < 2048) { pts[np].X = qx; pts[np].Y = qy; np++; }
                        if (!have) { mnx=mxx=qx; mny=mxy=qy; have=1; }
                        else {
                            if (qx < mnx) mnx = qx;
                            if (qx > mxx) mxx = qx;
                            if (qy < mny) mny = qy;
                            if (qy > mxy) mxy = qy;
                        }
                    }
                }
                if (!have) break;

                /* Every stop, in order, as a preset blend. */
                int ns = cmd->n_stops;
                if (ns > 16) ns = 16;
                ARGB cols[16];
                float pos[16];
                for (int s = 0; s < ns; s++) {
                    int a = (int)(cmd->stop_rgba[s*4+3] * 255.0f + 0.5f);
                    int r = (int)(cmd->stop_rgba[s*4+0] * 255.0f + 0.5f);
                    int gg= (int)(cmd->stop_rgba[s*4+1] * 255.0f + 0.5f);
                    int b = (int)(cmd->stop_rgba[s*4+2] * 255.0f + 0.5f);
                    /* All four, not just alpha: r/gg/b are built the same
                       way and shifted into their own bytes, so a stop
                       component outside [0,1] spilled into the neighbouring
                       channel instead of saturating. */
                    a  = w32_clamp_byte(a);
                    r  = w32_clamp_byte(r);
                    gg = w32_clamp_byte(gg);
                    b  = w32_clamp_byte(b);
                    cols[s] = ((ARGB)a<<24)|((ARGB)r<<16)|((ARGB)gg<<8)|(ARGB)b;
                    /* THE STOP'S OWN OFFSET. This spaced stops EVENLY --
                       s/(ns-1) -- discarding cmd->stop_off, which
                       win32_copy_stops has always filled in faithfully. Same
                       class of bug as grad_line_width and grad_extend: data
                       carried all the way here and then ignored.

                       Only 3+-stop gradients can notice (2 stops land on 0,1
                       either way), so the corpus barely moves -- but car.svg
                       has five, including alpha ramps whose middle stop sits
                       at 0.819 and 0.507. Forcing those to 0.5 misplaces the
                       whole opacity transition. Correct on its own terms
                       whether or not it shows up in a score. */
                    pos[s] = (ns == 1) ? 0.0f : (float)cmd->stop_off[s];
                    if (pos[s] < 0.0f) pos[s] = 0.0f;
                    if (pos[s] > 1.0f) pos[s] = 1.0f;
                }
                /* GDI+ requires a preset blend to start at 0 and end at 1 and
                   to increase MONOTONICALLY; SVG guarantees neither, and a
                   violation makes GdipSetLinePresetBlend fail outright (which
                   would silently drop every stop but the two endpoints). */
                pos[0] = 0.0f; pos[ns-1] = 1.0f;
                for (int s = 1; s < ns; s++) {
                    if (pos[s] < pos[s-1]) pos[s] = pos[s-1];
                }
                /* Strictly increasing: coincident stops are legal SVG (a hard
                   colour break) but GDI+ rejects the blend outright. Nudge by
                   the smallest step that still reads as a break at 400px. */
                for (int s = 1; s < ns; s++) {
                    if (pos[s] <= pos[s-1]) {
                        pos[s] = pos[s-1] + 0.0005f;
                        if (pos[s] > 1.0f) pos[s] = 1.0f;
                    }
                }
                if (pos[ns-1] < 1.0f) pos[ns-1] = 1.0f;

                /* A GRADIENT STROKE IS NOT A GRADIENT FILL.
                   CanvasCmd.grad_line_width has said "> 0 -> stroke" since the
                   struct was written, and this branch never read it -- it
                   treated every gradient command as a fill: build a polygon
                   from the path points, clip to it, fill the bounding box.
                   For a stroke that is wrong twice over, and a width sweep
                   measured both failures on the same two paths:

                     stroke, straight line   ink 0 at EVERY width (1..32)
                     stroke, curve           ink 0.48x ref at w1, 0.33x at w32

                   The line drew NOTHING because a horizontal line's bbox has
                   mxy == mny, which tripped the fill's degenerate-box guard
                   and broke out. The curve drew about half because filling the
                   thin sliver ENCLOSED by a path is not the band of width w
                   FOLLOWING it -- and a filled sliver does not grow with the
                   stroke width, which is exactly why the ratio drifts DOWN as
                   the width goes up. GTK4 matches librsvg to within 6 pixels
                   on all ten cases, so this was ours alone.

                   Stroke it properly: a pen carrying the gradient brush along
                   the real path. Sub-paths and cap style are preserved, so a
                   two-subpath stroke stays two strokes. */
                if (cmd->grad_line_width > 0.0f) {
                    /* Forward order, and CV_MOVE starts a new figure -- the
                       walk-back above collects points in REVERSE, which a
                       closed fill does not care about but a capped,
                       multi-subpath stroke very much does. */
                    GpPath* sp = NULL;
                    if (GdipCreatePath(GDIP_FILLMODE_ALTERNATE, &sp) == 0 && sp) {
                        int start = 0;
                        for (int j = i - 1; j >= 0; j--) {
                            if (cv->cmds[j].k == CV_BEGIN) { start = j; break; }
                        }
                        int fig_open = 0, segs = 0;
                        for (int j = start; j < i; j++) {
                            CanvasCmdKind k = cv->cmds[j].k;
                            if (k == CV_MOVE) {
                                if (fig_open) GdipStartPathFigure(sp);
                                else { GdipStartPathFigure(sp); fig_open = 1; }
                            } else if (k == CV_LINE) {
                                GpPointI seg[2];
                                seg[0].X = (INT)cv->cmds[j].p0;
                                seg[0].Y = (INT)cv->cmds[j].p1;
                                seg[1].X = (INT)cv->cmds[j].p2;
                                seg[1].Y = (INT)cv->cmds[j].p3;
                                if (GdipAddPathLine2I(sp, seg, 2) == 0) segs++;
                            }
                        }
                        if (segs > 0) {
                            GpPointI ga, gb;
                            if (cmd->gx1 != cmd->gx2 || cmd->gy1 != cmd->gy2) {
                                ga.X = (INT)cmd->gx1; ga.Y = (INT)cmd->gy1;
                                gb.X = (INT)cmd->gx2; gb.Y = (INT)cmd->gy2;
                            } else {
                                ga.X = mnx; ga.Y = mny;
                                gb.X = mxx; gb.Y = mny;
                            }
                            /* Same spreadMethod handling as the fill case
                               below -- pad by extending the vector. */
                            ARGB scols[18]; float spos[18]; int sns = ns;
                            /* Pad only a genuinely LINEAR stroke gradient.
                               A radial one reaches this branch too (the test
                               is grad_line_width>0) and is painted with a
                               linear brush as a stand-in; that stand-in is
                               calibrated by accident, and reshaping its vector
                               moved php.svg 4.60 -> 6.31. Leave it alone. */
                            if (cmd->grad_extend == 0 && cmd->k == CV_FILL_LINEAR) {
                                float dx = (float)(gb.X - ga.X), dy = (float)(gb.Y - ga.Y);
                                float len = (float)sqrt(dx*dx + dy*dy);
                                /* Pad by the shape's own projection onto the
                                   axis -- see the fill case for why a blanket
                                   constant costs blend precision. Widen by the
                                   stroke width too: a stroke paints up to
                                   half a pen either side of the path. */
                                float pad = 0.0f;
                                if (len > 0.01f) {
                                    float ux = dx / len, uy = dy / len;
                                    int cx4[4] = { mnx, mxx, mnx, mxx };
                                    int cy4[4] = { mny, mny, mxy, mxy };
                                    for (int q = 0; q < 4; q++) {
                                        float t = ((float)cx4[q] - (float)ga.X) * ux +
                                                  ((float)cy4[q] - (float)ga.Y) * uy;
                                        if (t < -pad) pad = -t;
                                        if (t - len > pad) pad = t - len;
                                    }
                                    pad += cmd->grad_line_width * 0.5f + 2.0f;
                                }
                                float diag = pad;
                                if (len > 0.01f) {
                                    float ex = dx / len * diag, ey = dy / len * diag;
                                    GpPointI na, nb;
                                    na.X = (INT)(ga.X - ex); na.Y = (INT)(ga.Y - ey);
                                    nb.X = (INT)(gb.X + ex); nb.Y = (INT)(gb.Y + ey);
                                    float total = len + 2.0f * diag;
                                    float lo = diag / total, span = len / total;
                                    sns = 0;
                                    scols[sns] = cols[0];    spos[sns] = 0.0f; sns++;
                                    for (int s = 0; s < ns && sns < 17; s++) {
                                        scols[sns] = cols[s];
                                        spos[sns] = lo + pos[s] * span;
                                        sns++;
                                    }
                                    scols[sns] = cols[ns-1]; spos[sns] = 1.0f; sns++;
                                    ga = na; gb = nb;
                                }
                            } else {
                                for (int s = 0; s < ns; s++) { scols[s] = cols[s]; spos[s] = pos[s]; }
                            }
                            /* NB a RADIAL gradient stroke is still painted
                               with a LINEAR brush here: this branch is entered
                               for both kinds (the test is grad_line_width>0).
                               php.svg's ellipse stroke="url(#phpg)" is radial
                               and lands near the reference by accident. The
                               obvious fix -- a path-gradient brush built from
                               the shape's bounding ellipse -- measures WORSE
                               (6.31 -> 7.29), because phpg is userSpaceOnUse
                               centred at (250,0) r=300, far outside the shape,
                               so the bounding ellipse is the wrong geometry
                               entirely. CanvasCmd carries the real centre and
                               radius; using them needs the same care the
                               radial FILL case documents. Left as-is
                               deliberately rather than made confidently wrong. */
                            GpBrush* sbr = NULL;
                            GpPath* rspath = NULL;
                            /* A RADIAL stroke gets a PATH-GRADIENT brush, using
                               the command's OWN centre and radius. The earlier
                               attempt at this used the shape's bounding
                               ellipse and measured worse (6.31 -> 7.29) --
                               correctly, because phpg is userSpaceOnUse
                               centred at (250,0) r=300, far outside the shape.
                               The radial FILL case has since been fixed the
                               same way, and that fix is what makes this
                               tractable: take the geometry from the command,
                               never from the bbox. */
                            if (cmd->k == CV_FILL_RADIAL && cmd->gr > 0.0f) {
                                int scx = (INT)cmd->gx1, scy = (INT)cmd->gy1;
                                int srr = (INT)cmd->gr;
                                if (srr < 1) srr = 1;
                                if (GdipCreatePath(GDIP_FILLMODE_ALTERNATE, &rspath) == 0
                                    && rspath) {
                                    GdipAddPathEllipseI(rspath, scx - srr, scy - srr,
                                                        srr * 2, srr * 2);
                                    GpPathGradient* rpg = NULL;
                                    if (GdipCreatePathGradientFromPath(rspath, &rpg) == 0
                                        && rpg) {
                                        /* Rim -> centre, so the SVG stops are
                                           reversed -- the same correction the
                                           radial fill documents. */
                                        ARGB rc[16]; float rp[16];
                                        for (int s2 = 0; s2 < ns; s2++) {
                                            rc[s2] = cols[ns - 1 - s2];
                                            rp[s2] = 1.0f - pos[ns - 1 - s2];
                                        }
                                        rp[0] = 0.0f; rp[ns-1] = 1.0f;
                                        GdipSetPathGradientCenterColor(rpg, cols[0]);
                                        if (ns >= 2)
                                            GdipSetPathGradientPresetBlend(rpg, rc, rp, ns);
                                        sbr = (GpBrush*)rpg;
                                    }
                                }
                            }
                            GpLineGradient* slg = NULL;
                            if (!sbr &&
                                GdipCreateLineBrushI(&ga, &gb, scols[0], scols[sns-1],
                                                     GDIP_WRAP_TILE, &slg) == 0 && slg) {
                                if (cmd->grad_extend == 1)
                                    GdipSetLineWrapMode(slg, GDIP_WRAP_TILE_FLIPX);
                                if (sns >= 2)
                                    GdipSetLinePresetBlend(slg, scols, spos, sns);
                                sbr = (GpBrush*)slg;
                            }
                            if (sbr) {
                                GpPen* gp = NULL;
                                if (GdipCreatePen2(sbr,
                                                   cmd->grad_line_width,
                                                   GDIP_UNIT_PIXEL, &gp) == 0 && gp) {
                                    /* The command's OWN cap/join, through the
                                       same helpers CV_STROKE uses -- not a
                                       hardcoded round.
                                       NB these only became real when the
                                       gradient primitive was widened to carry
                                       cap/join: before that the field was
                                       simply never populated for a gradient,
                                       so this read a default butt cap and
                                       twitter.svg's round-capped bar stopped
                                       at x=292 against librsvg's x=323. */
                                    GdipSetPenStartCap(gp, gdip_cap(cmd->cap));
                                    GdipSetPenEndCap(gp, gdip_cap(cmd->cap));
                                    GdipSetPenLineJoin(gp, gdip_join(cmd->join));
                                    GdipDrawPath(g, gp, sp);
                                    GdipDeletePen(gp);
                                }
                                GdipDeleteBrush(sbr);
                                /* Owned by the radial branch; the brush was
                                   created FROM it, so it outlives the brush. */
                                if (rspath) GdipDeletePath(rspath);
                            }
                        }
                        GdipDeletePath(sp);
                    }
                    break;
                }

                if (mxx <= mnx || mxy <= mny) break;

                if (cmd->k == CV_FILL_RADIAL) {
                    /* THE REAL CIRCLE, not the path's bounding box.
                       CanvasCmd carries centre (gx1,gy1), radius gr and focal
                       point (gfx,gfy) -- the vg layer resolves all of them to
                       canvas space before dispatch. The first version of this
                       ignored every one and used the bounding box, the same
                       mistake the linear case made, and it SHOWED: on the
                       208-file corpus GDI+ beat legacy overall (mean MAE 20.87
                       -> 16.63) while REGRESSING exactly the radial tests --
                       test_radial_gradient.svg 24.5 -> 57.6, radialgradient1
                       17.1 -> 27.5. Fixed here.

                       GDI+ path gradients run rim -> centre, whereas SVG stops
                       run centre (offset 0) -> rim (offset 1). So the preset
                       blend is REVERSED before use: colour i takes stop
                       (ns-1-i) and position 1 - pos[ns-1-i]. */
                    /* An SVG radial can be ELLIPTICAL: gradientUnits=
                       userSpaceOnUse with a non-uniform gradientTransform
                       (radialgradient1.svg scales 0.79 x 1.26) produces an
                       ellipse, and CanvasCmd carries only a scalar gr. So use
                       the real CENTRE from the command but keep the bounding
                       box's ASPECT -- forcing a circle of radius gr measured
                       WORSE on that file than the box did (27.5 -> 71.2) while
                       helping test_radial_gradient. Centre from the data,
                       extent from the geometry, is the pair that serves both. */
                    /* MEASURED, not assumed. Three variants scored on the
                       two radial tests in the corpus:

                         bbox ellipse, forward blend   57.6 / 27.5
                         circle at gr, reversed blend  30.9 / 71.2
                         real centre + bbox aspect     31.7 / 49.4
                         bbox ellipse, reversed blend  <- this one

                       The command's centre helps test_radial_gradient and
                       hurts radialgradient1, which uses userSpaceOnUse with a
                       non-uniform gradientTransform that CanvasCmd's scalar gr
                       cannot describe. The bounding box already encodes that
                       ellipse, so take the geometry from it and keep only the
                       blend-order fix, which is unambiguously correct. */
                    /* THE COMMAND'S OWN CENTRE AND RADIUS, not the path's
                       bounding box. The vg layer resolves cx/cy/r to canvas
                       space and CanvasCmd has carried them all along; this
                       used the bbox instead, so a gradient was sized by the
                       SHAPE it fills rather than by its own geometry. For a
                       canvas-filling rect that means the whole canvas.

                       Measured on radial_uniform_control.svg (r=30 at scale
                       4 -> the falloff should reach 120px): the command says
                       r=120.0, matching librsvg's and GTK4's 122px
                       half-maximum span exactly, while the bbox said 200.
                       The painted profile moves from a wrong shallow ramp
                       (75,30,59,89,118,148,...) onto librsvg's shape
                       (39,89,138,187,237 against its 44,97,150,203,253).

                       Falls back to the bbox when the command carries no
                       radius, which is the objectBoundingBox path. */
                    int rcx, rcy, rw, rh;
                    if (cmd->grx > 0.0f && cmd->gry > 0.0f) {
                        /* THE ELLIPSE. This branch already drew with separate
                           rw/rh -- it just set both to the scalar radius, so
                           a gradientTransform's eccentricity was thrown away
                           upstream and could not have been honoured here
                           anyway. Now the vg layer carries both semi-axes.
                           GTK4 landed this first and went 1/4 -> 4/4 on the
                           axis-ratio oracle with nothing regressing.
                           The tilt (cmd->grot) is applied below via the
                           world transform, since GDI+ fills axis-aligned
                           ellipses only. */
                        rcx = (INT)cmd->gx1; rcy = (INT)cmd->gy1;
                        rw = (INT)cmd->grx; rh = (INT)cmd->gry;
                    } else if (cmd->gr > 0.0f) {
                        rcx = (INT)cmd->gx1; rcy = (INT)cmd->gy1;
                        rw = rh = (INT)cmd->gr;
                    } else {
                        rcx = (mnx + mxx) / 2; rcy = (mny + mxy) / 2;
                        rw = (mxx - mnx) / 2; rh = (mxy - mny) / 2;
                    }
                    if (rw < 1) rw = 1;
                    if (rh < 1) rh = 1;
                    GpPath* path = NULL;
                    if (GdipCreatePath(GDIP_FILLMODE_ALTERNATE, &path) == 0 && path) {
                        GdipAddPathEllipseI(path, rcx - rw, rcy - rh, rw * 2, rh * 2);
                        GpPathGradient* pg = NULL;
                        if (GdipCreatePathGradientFromPath(path, &pg) == 0 && pg) {
                            ARGB rcols[16];
                            float rpos[16];
                            for (int s2 = 0; s2 < ns; s2++) {
                                rcols[s2] = cols[ns - 1 - s2];
                                rpos[s2]  = 1.0f - pos[ns - 1 - s2];
                            }
                            rpos[0] = 0.0f; rpos[ns-1] = 1.0f;
                            /* Centre colour is the FIRST SVG stop; the focal
                               point moves it when fx/fy differ from cx/cy. */
                            GdipSetPathGradientCenterColor(pg, cols[0]);
                            /* Move the centre ONLY for a genuinely offset
                               focal point. The vg layer defaults fx/fy to
                               cx/cy, so testing against 0 (as the first
                               attempt did) relocated the centre on EVERY
                               radial and made radialgradient1.svg far worse:
                               27.5 -> 71.2. Compare against the centre. */
                            if ((INT)cmd->gfx != rcx || (INT)cmd->gfy != rcy) {
                                GpPointI fp;
                                fp.X = (INT)cmd->gfx; fp.Y = (INT)cmd->gfy;
                                GdipSetPathGradientCenterPointI(pg, &fp);
                            }
                            if (ns >= 2) GdipSetPathGradientPresetBlend(pg, rcols, rpos, ns);
                            /* Clip to the real path, as the linear case does --
                               otherwise the circle paints outside the shape. */
                            GpPath* clip2 = NULL;
                            int clipped2 = 0;
                            if (np >= 3 && GdipCreatePath(GDIP_FILLMODE_WINDING, &clip2) == 0
                                && clip2) {
                                if (GdipAddPathLine2I(clip2, pts, np) == 0) {
                                    GdipClosePathFigure(clip2);
                                    if (GdipSetClipPath(g, clip2, GDIP_COMBINE_REPLACE) == 0)
                                        clipped2 = 1;
                                }
                            }
                            /* PAD PAST THE RIM. A GDI+ path gradient paints
                               ONLY inside its path, so filling just the
                               ellipse leaves everything outside it unpainted
                               -- for a <rect> that is the four corners, and
                               test_radial_gradient.svg rendered as a CIRCLE on
                               white where librsvg and GTK4 both draw a square
                               (GTK4 0.0, GDI+ 30.8).

                               SVG's default spreadMethod is pad, so beyond the
                               rim the gradient is a flat hold of the LAST
                               stop. Lay that colour over the clipped region
                               first, then paint the ellipse on top. Where the
                               shape is no bigger than the ellipse this is
                               entirely covered and costs only a fill. */
                            {
                                GpBrush* padbr = NULL;
                                if (GdipCreateSolidFill(cols[ns-1], &padbr) == 0
                                    && padbr) {
                                    GdipFillRectangleI(g, padbr, mnx, mny,
                                                       mxx - mnx, mxy - mny);
                                    GdipDeleteBrush(padbr);
                                }
                            }
                            /* THE TILT. GDI+ fills axis-aligned ellipses
                               only, so a rotated gradient needs the world
                               transform: rotate about the centre, fill, then
                               put the CTM back. No corpus file exercises
                               this -- vg/test/svg/radial_ellipse_rotated.svg
                               exists precisely because a backend that drops
                               the tilt scores perfectly on all 208 corpus
                               files while being visibly wrong. Its diagonal
                               probe read 1.02 here against librsvg's 2.02
                               before this. */
                            GpMatrix* saved = NULL;
                            GpMatrix* rotm = NULL;
                            int rotated = 0;
                            if (cmd->grot > 0.01f || cmd->grot < -0.01f) {
                                if (GdipCreateMatrix(&saved) == 0 && saved &&
                                    GdipGetWorldTransform(g, saved) == 0 &&
                                    GdipCreateMatrix(&rotm) == 0 && rotm &&
                                    GdipGetWorldTransform(g, rotm) == 0) {
                                    GdipTranslateMatrix(rotm, (float)rcx,
                                                        (float)rcy,
                                                        GDIP_MATRIX_PREPEND);
                                    GdipRotateMatrix(rotm, cmd->grot,
                                                     GDIP_MATRIX_PREPEND);
                                    GdipTranslateMatrix(rotm, (float)(-rcx),
                                                        (float)(-rcy),
                                                        GDIP_MATRIX_PREPEND);
                                    if (GdipSetWorldTransform(g, rotm) == 0)
                                        rotated = 1;
                                }
                            }
                            GdipFillEllipseI(g, (GpBrush*)pg, rcx - rw, rcy - rh,
                                             rw * 2, rh * 2);
                            if (rotated) GdipSetWorldTransform(g, saved);
                            if (rotm) GdipDeleteMatrix(rotm);
                            if (saved) GdipDeleteMatrix(saved);
                            if (clipped2) GdipResetClip(g);
                            if (clip2) GdipDeletePath(clip2);
                            GdipDeleteBrush((GpBrush*)pg);
                        }
                        GdipDeletePath(path);
                    }
                } else {
                    /* THE REAL GRADIENT VECTOR, not the bounding box.
                       CanvasCmd carries gx1,gy1 -> gx2,gy2, and the vg layer
                       has ALREADY baked gradientTransform into it (defs.ae
                       applies the transform to x1/y1/x2/y2 before dispatch).
                       Step 8 deliberately used a horizontal box-edge axis "to
                       match legacy" -- but legacy only does that because GDI's
                       GradientFill cannot express an arbitrary axis. Copying
                       that here threw away data we were being handed, and is
                       why python.svg's rotate(45) gradients came out as flat
                       horizontal ramps. Fall back to the box only when the
                       command carries no usable vector. */
                    GpPointI a, b;
                    if (cmd->gx1 != cmd->gx2 || cmd->gy1 != cmd->gy2) {
                        a.X = (INT)cmd->gx1; a.Y = (INT)cmd->gy1;
                        b.X = (INT)cmd->gx2; b.Y = (INT)cmd->gy2;
                    } else {
                        a.X = mnx; a.Y = mny;
                        b.X = mxx; b.Y = mny;
                    }
                    /* CLIP TO THE PATH. Legacy fills the whole bounding box,
                       so a gradient-filled outline paints over everything
                       around it -- measured on python.svg: 25,251 white pixels
                       against librsvg's 64,749, i.e. the background eaten by
                       two rectangles. Build the accumulated path and clip. */
                    /* SUB-PATHS ARE SEPARATE FIGURES. pts[] above is a flat
                       list in which CV_MOVE is just another vertex, so feeding
                       it to one GdipAddPathLine2I welds every sub-path into a
                       single polyline -- and the weld is VISIBLE: python.svg's
                       two paths each end with a degenerate stub (M88,50v1 and
                       M140,50v1, the latter outside the 0..100 viewBox
                       entirely), so the clip ran a hairline from the body's
                       last point out to the stub. That is the blue slash
                       across the upper body and the yellow one trailing off
                       the bottom-right corner, each 2px wide over ~110 rows.
                       Walk forward and start a new figure at every CV_MOVE,
                       exactly as the gradient STROKE case does. */
                    GpPath* clip = NULL;
                    int clipped = 0;
                    if (np >= 3 && GdipCreatePath(GDIP_FILLMODE_WINDING, &clip) == 0
                        && clip) {
                        int cstart = 0;
                        for (int j = i - 1; j >= 0; j--) {
                            if (cv->cmds[j].k == CV_BEGIN) { cstart = j; break; }
                        }
                        int figs = 0, fpts = 0;
                        GpPointI fig[256];
                        for (int j = cstart; j <= i; j++) {
                            CanvasCmdKind k = (j < i) ? cv->cmds[j].k : CV_BEGIN;
                            int isend = (j == i);
                            if (k == CV_MOVE || isend) {
                                if (fpts >= 2 &&
                                    GdipAddPathLine2I(clip, fig, fpts) == 0) {
                                    GdipClosePathFigure(clip);
                                    figs++;
                                }
                                fpts = 0;
                                if (!isend) {
                                    fig[fpts].X = (INT)cv->cmds[j].p0;
                                    fig[fpts].Y = (INT)cv->cmds[j].p1;
                                    fpts++;
                                }
                            } else if (k == CV_LINE && fpts < 256) {
                                fig[fpts].X = (INT)cv->cmds[j].p2;
                                fig[fpts].Y = (INT)cv->cmds[j].p3;
                                fpts++;
                            }
                        }
                        if (figs > 0 &&
                            GdipSetClipPath(g, clip, GDIP_COMBINE_REPLACE) == 0)
                            clipped = 1;
                    }
                    /* SVG spreadMethod, which this backend hardcoded to
                       GDIP_WRAP_TILE -- i.e. "repeat" -- while SVG's DEFAULT
                       is PAD. Any gradient whose vector is shorter than the
                       shape therefore TILED, and the sawtooth is visible in
                       the pixels: car.svg's rear window ramps 195->95 then
                       jumps back to 195 every 8px, rendering a smooth dark
                       pane as diagonal grey stripes. GTK4 has always mapped
                       this properly (CAIRO_EXTEND_PAD/REFLECT/REPEAT).

                       Pad is implemented by EXTENDING the vector rather than
                       by WrapModeClamp: on a LinearGradientBrush, Clamp paints
                       everything outside the vector TRANSPARENT instead of
                       holding the end colours, which trades one artifact for
                       another. Stretch the axis far beyond the shape and remap
                       the stops into the middle, so the first/last stop colour
                       occupies everything past each end -- the definition of
                       pad -- and no tile boundary is ever inside the shape. */
                    ARGB ucols[18];
                    float upos[18];
                    int uns = ns;
                    GpPointI ua = a, ub = b;
                    if (cmd->grad_extend == 0) {
                        float dx = (float)(b.X - a.X), dy = (float)(b.Y - a.Y);
                        float len = (float)sqrt(dx*dx + dy*dy);
                        /* Extend by exactly what the SHAPE needs along this
                           axis, never a blanket constant. GDI+ interpolates
                           the preset blend across the WHOLE brush, so slack
                           costs precision: padding a 5px vector by a 300px
                           diagonal squeezes the real ramp into 0.8% of the
                           blend, where it quantises to one flat colour. That
                           regressed php.svg (4.60 -> 6.31, its gradient going
                           solid #CCF) on the first attempt. Project the
                           bounding box's corners onto the axis and pad to the
                           furthest, so the hold regions are as small as they
                           can be while still covering the shape. */
                        float pad = 0.0f;
                        if (len > 0.01f) {
                            float ux = dx / len, uy = dy / len;
                            int cx4[4] = { mnx, mxx, mnx, mxx };
                            int cy4[4] = { mny, mny, mxy, mxy };
                            for (int q = 0; q < 4; q++) {
                                float t = ((float)cx4[q] - (float)a.X) * ux +
                                          ((float)cy4[q] - (float)a.Y) * uy;
                                if (t < -pad) pad = -t;
                                if (t - len > pad) pad = t - len;
                            }
                            pad += 2.0f;
                        }
                        float diag = pad;
                        if (len > 0.01f) {
                            float ex = dx / len * diag, ey = dy / len * diag;
                            ua.X = (INT)(a.X - ex); ua.Y = (INT)(a.Y - ey);
                            ub.X = (INT)(b.X + ex); ub.Y = (INT)(b.Y + ey);
                            float total = len + 2.0f * diag;
                            float lo = diag / total, span = len / total;
                            /* Duplicate the end stops at 0 and 1 so the pad
                               region is a flat hold, not a ramp to the edge. */
                            uns = 0;
                            ucols[uns] = cols[0];      upos[uns] = 0.0f;      uns++;
                            for (int s = 0; s < ns && uns < 17; s++) {
                                ucols[uns] = cols[s];
                                upos[uns] = lo + pos[s] * span;
                                uns++;
                            }
                            ucols[uns] = cols[ns-1];   upos[uns] = 1.0f;      uns++;
                        }
                    } else {
                        for (int s = 0; s < ns; s++) { ucols[s] = cols[s]; upos[s] = pos[s]; }
                    }
                    GpLineGradient* lg = NULL;
                    if (GdipCreateLineBrushI(&ua, &ub, ucols[0], ucols[uns-1],
                                             GDIP_WRAP_TILE, &lg) == 0 && lg) {
                        /* reflect -> TileFlipX (mirror each repeat);
                           repeat -> plain Tile; pad handled by the vector. */
                        if (cmd->grad_extend == 1)
                            GdipSetLineWrapMode(lg, GDIP_WRAP_TILE_FLIPX);
                        if (uns >= 2) GdipSetLinePresetBlend(lg, ucols, upos, uns);
                        GdipFillRectangleI(g, (GpBrush*)lg, mnx, mny,
                                           mxx-mnx, mxy-mny);
                        GdipDeleteBrush((GpBrush*)lg);
                    }
                    if (clipped) GdipResetClip(g);
                    if (clip) GdipDeletePath(clip);
                }
                break;
            }
            case CV_FILL_TEXT: {
                if (!cmd->text) break;
                /* Same baseline fudge as legacy (cairo's origin is the
                   baseline, GDI/GDI+ lay out from the top-left, ~0.8 of
                   the em is the ascent). GDI+ has a real baseline via
                   GdipGetFontHeight, but matching legacy's geometry EXACTLY
                   is what keeps this comparison about RENDERING rather than
                   about layout drift. */
                wchar_t* w = utf8_to_wide(cmd->text);
                if (!w) break;
                GpFontFamily* fam = gdip_resolve_family(cmd->font_family);
                if (!fam) {
                    /* Family unavailable. Draw NOTHING rather than guess a
                       substitute: a wrong font would read as a rendering
                       difference in the MAE and send the comparison chasing
                       a font-matching problem. Missing ink is at least
                       unambiguous. */
                    if (fam) GdipDeleteFontFamily(fam);
                    break;
                }
                GpFont* font = NULL;
                if (GdipCreateFont(fam, (float)cmd->p2, GDIP_FONTSTYLE_REGULAR,
                                   GDIP_UNIT_PIXEL, &font) == 0 && font) {
                    GpBrush* br = NULL;
                    if (GdipCreateSolidFill(gdip_argb(cmd), &br) == 0 && br) {
                        GdipSetTextRenderingHint(g, GDIP_TEXT_ANTIALIAS);
                        GpRectF layout;
                        layout.X = (float)cmd->p0;
                        /* THE BASELINE, from the font's own metrics.
                           GdipDrawString's layoutRect.Y is the TOP of the line
                           box, while SVG's y is the BASELINE, so the ascent has
                           to come off. The old code subtracted a flat 0.8 em
                           "to match legacy" -- measured on 410.svg
                           (font-size 48 in a 0..100 viewBox at scale 4, so the
                           baseline belongs at px y=272) that put the glyph
                           bottom at 319, i.e. 47px LOW, because GDI+ also adds
                           internal leading above the ascent. librsvg and GTK4
                           both land at 272/273.

                           Ask the family for cell-ascent and em-height in
                           design units and scale: offset = size * asc/em. That
                           is correct for any face rather than tuned for one. */
                        unsigned short asc = 0, emh = 0;
                        float up = cmd->p2 * 0.8f;   /* fallback: the old fudge */
                        if (GdipGetCellAscent(fam, GDIP_FONTSTYLE_REGULAR, &asc) == 0
                            && GdipGetEmHeight(fam, GDIP_FONTSTYLE_REGULAR, &emh) == 0
                            && emh > 0) {
                            up = cmd->p2 * ((float)asc / (float)emh);
                        }
                        layout.Y = (float)(cmd->p1 - up);
                        layout.Width = 0.0f;   /* 0 = no wrapping box */
                        layout.Height = 0.0f;
                        /* NO ORIGIN PADDING. With a NULL format,
                           GdipDrawString insets the string by the font's
                           leading, so the run starts right of the x it was
                           handed. Measured on 410.svg: the vg layer centres
                           text-anchor="middle" itself (dx = x - advance/2 =
                           39.5px here) but the glyphs began at x=75 -- 35.5px
                           adrift, which read as the whole label sitting ~7%
                           too far right. GenericTypographic has no padding. */
                        GpStringFormat* fmt = NULL;
                        GdipStringFormatGetGenericTypographic(&fmt);
                        GdipDrawString(g, (const unsigned short*)w, -1, font,
                                       &layout, fmt, br);
                        GdipDeleteBrush(br);
                    }
                    GdipDeleteFont(font);
                }
                GdipDeleteFontFamily(fam);
                break;
            }
            case CV_STROKE_TEXT: {
                /* SVG stroke on <text>. GdipDrawString can only FILL, so the
                   glyphs are taken as PATH GEOMETRY via GdipAddPathStringI
                   and that path is stroked -- a real outline, not a filled
                   second pass (which overpaints rather than outlines, and
                   measured worse: it moved Steps.svg AWAY from GTK4).

                   An earlier attempt blamed this API for an exit-127 crash.
                   It was not the API: utf8_to_wide returns a pointer into a
                   STATIC rotating buffer and the free(w) beside it corrupted
                   the heap. There is deliberately no free here -- CV_FILL_TEXT
                   does not free it either.

                   Repro: vg/test/svg/text_stroke_repro.svg -- librsvg and
                   GTK4 draw a red B with a blue outline. */
                if (!cmd->text) break;
                wchar_t* w = utf8_to_wide(cmd->text);
                if (!w || !w[0]) break;
                GpFontFamily* fam = gdip_resolve_family(cmd->font_family);
                if (!fam) break;
                unsigned short asc = 0, emh = 0;
                float up = (float)cmd->p2 * 0.8f;
                if (GdipGetCellAscent(fam, GDIP_FONTSTYLE_REGULAR, &asc) == 0
                    && GdipGetEmHeight(fam, GDIP_FONTSTYLE_REGULAR, &emh) == 0
                    && emh > 0) {
                    up = (float)cmd->p2 * ((float)asc / (float)emh);
                }
                GpPath* tp = NULL;
                if (GdipCreatePath(GDIP_FILLMODE_WINDING, &tp) == 0 && tp) {
                    GpRectI layout;
                    layout.X = (INT)cmd->p0;
                    layout.Y = (INT)((float)cmd->p1 - up);
                    layout.Width = 0;
                    layout.Height = 0;
                    int wlen = 0;
                    while (w[wlen]) wlen++;
                    /* The SAME typographic format the FILL pass uses. With a
                       NULL format GDI+ insets the run by the font's leading,
                       so the outline landed offset right and down from the
                       fill it is supposed to trace -- visible immediately on
                       the repro as a doubled B. */
                    GpStringFormat* sfmt = NULL;
                    GdipStringFormatGetGenericTypographic(&sfmt);
                    if (wlen > 0 &&
                        GdipAddPathStringI(tp, (const unsigned short*)w, wlen,
                                           fam, GDIP_FONTSTYLE_REGULAR,
                                           (float)cmd->p2, &layout, sfmt) == 0) {
                        float lw = (float)cmd->p3;
                        if (lw <= 0.0f) lw = 1.0f;
                        GpPen* tpen = NULL;
                        if (GdipCreatePen1(gdip_argb(cmd), lw,
                                           GDIP_UNIT_PIXEL, &tpen) == 0 && tpen) {
                            GdipSetSmoothingMode(g, GDIP_SMOOTHING_AA);
                            GdipDrawPath(g, tpen, tp);
                            GdipDeletePen(tpen);
                        }
                    }
                    GdipDeletePath(tp);
                }
                GdipDeleteFontFamily(fam);
                break;
            }
            case CV_DRAW_IMAGE: {
                /* THE CASE GDI GETS WRONG. Legacy cannot blit alpha at all:
                   it flattens every pixel against WHITE before StretchDIBits
                   ("(approximately) composite onto the white backing", per
                   its own comment) and forces a=255. Over a white page that
                   looks right; over anything else it is the wrong colour.
                   Measured with examples/blit_probe -- amber D9A13C at a=128
                   over #101018:

                       GTK4 (correct)   74582A
                       legacy GDI       EBCF9D   <- flattened against white
                       GDI+ (this)      must match GTK4

                   GDI+ composites properly, so the source needs NO
                   premultiplication against the backdrop -- just the channel
                   swizzle, because PixelFormat32bppARGB is BGRA in memory
                   while the canvas hands us RGBA. */
                if (!cmd->pixels || cmd->iw <= 0 || cmd->ih <= 0) break;
                int n = cmd->iw * cmd->ih;
                unsigned char* bgra = (unsigned char*)malloc((size_t)n * 4);
                if (!bgra) break;
                for (int px = 0; px < n; px++) {
                    bgra[px*4+0] = cmd->pixels[px*4+2];  /* B */
                    bgra[px*4+1] = cmd->pixels[px*4+1];  /* G */
                    bgra[px*4+2] = cmd->pixels[px*4+0];  /* R */
                    bgra[px*4+3] = cmd->pixels[px*4+3];  /* A -- KEPT */
                }
                void* bmp = NULL;
                if (GdipCreateBitmapFromScan0(cmd->iw, cmd->ih, cmd->iw * 4,
                                              GDIP_FMT_32BPP_ARGB, bgra, &bmp) == 0
                    && bmp) {
                    /* Dest extent: p2/p3 carry the SCALED size when the
                       command came from draw_image_scaled; zero means an
                       unscaled draw_image. GdipDrawImageRectI scales the
                       source to this rect natively — the same dest-rect
                       plumbing as the legacy-GDI StretchDIBits arm and the
                       macOS CGContextDrawImage one, fixed together: all
                       three executors had the 1:1 stub independently. */
                    INT ddw = cmd->p2 > 0 ? (INT)cmd->p2 : cmd->iw;
                    INT ddh = cmd->p3 > 0 ? (INT)cmd->p3 : cmd->ih;
                    GdipDrawImageRectI(g, bmp, (INT)cmd->p0, (INT)cmd->p1,
                                       ddw, ddh);
                    GdipDisposeImage(bmp);
                }
                /* Freed AFTER the draw: GdipCreateBitmapFromScan0 does not
                   copy, it borrows scan0, so releasing it earlier would blit
                   freed memory. */
                free(bgra);
                break;
            }
            case CV_CLOSE:
                break;  /* polygon close handled in CV_FILL accumulation */
            default:
                /* Not yet ported. */
                break;
        }
    }
    if (cur_pen) GdipDeletePen(cur_pen);
    /* A group left open by a malformed command stream would otherwise leak
       its layer AND silently swallow everything drawn inside it (the layer is
       never composited back). Unwind: paint each pending layer at full alpha,
       innermost first, so the drawing still lands. */
    while (grp_depth > 0) {
        grp_depth--;
        GpGraphics* lg = g;
        g = grp_saved[grp_depth];
        GdipDrawImageRectI(g, grp_bmp[grp_depth], 0, 0, width, height);
        GdipDeleteGraphics(lg);
        GdipDisposeImage(grp_bmp[grp_depth]);
    }
    #undef GRP_MAX
    GdipDeleteGraphics(g);
}

static void canvas_paint(HWND hwnd, HDC hdc, int width, int height) {
    int id = canvas_id_for_hwnd(hwnd);
    if (id == 0) return;
    Canvas* cv = &canvases[id - 1];
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem, bmp);
    HRGN clip = NULL;
    {
        int clipped_now = (cv->paint_clip_count > 0);
        double a = 0.0;
        for (int i = 0; clipped_now && i < cv->paint_clip_count; i++) {
            double* r = &cv->paint_clip_rects[i * 4];
            if (r[2] > 0.0 && r[3] > 0.0) a += r[2] * r[3];
        }
        cv->last_paint_w = width;
        cv->last_paint_h = height;
        cv->last_paint_count = cv->cmd_count;
        cv->last_clip_area = clipped_now ? (int)(a + 0.5) : 0;
        cv->last_paint_area = clipped_now ? cv->last_clip_area : width * height;
        if (clipped_now) cv->clip_paints++; else cv->full_paints++;
    }
    if (cv->paint_clip_count > 0) {
        clip = CreateRectRgn(0, 0, 0, 0);
        for (int i = 0; clip && i < cv->paint_clip_count; i++) {
            double* r = &cv->paint_clip_rects[i * 4];
            if (r[2] > 0.0 && r[3] > 0.0) {
                HRGN rr = CreateRectRgn((int)r[0], (int)r[1],
                    (int)(r[0] + r[2] + 0.5), (int)(r[1] + r[3] + 0.5));
                CombineRgn(clip, clip, rr, RGN_OR);
                DeleteObject(rr);
            }
        }
        SelectClipRgn(mem, clip);
        SelectClipRgn(hdc, clip);
    }
    canvas_replay_to_dc(cv, mem, width, height);
    BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    if (clip) {
        SelectClipRgn(mem, NULL);
        SelectClipRgn(hdc, NULL);
        DeleteObject(clip);
        cv->paint_clip_count = 0;
    }
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py,
                                     int width, int height) {
    // Replay the canvas's command buffer into a memory bitmap and read one
    // pixel — headless-safe (no window needed), the win32 twin of GTK4's
    // cairo-surface replay. Was a -1 stub, which made every pixel probe
    // read as "ink" (0xFF in every channel) and let colour-comparison
    // specs pass VACUOUSLY on this backend — vg3d's not-a-flood upper
    // bound is what finally caught it.
    //
    // GDI has no alpha: the result's A is always 255 and the backdrop is
    // WHITE (matching canvas_paint). Probes must classify by COLOUR
    // against a drawn background, never by alpha, to be cross-platform.
    if (canvas_id < 1 || canvas_id > canvas_count) return -1;
    if (px < 0 || py < 0 || px >= width || py >= height) return -1;
    Canvas* cv = &canvases[canvas_id - 1];
    int fresh = (cv->rp_dc && cv->rp_cache_gen == cv->rp_gen &&
                 cv->rp_cache_count == cv->cmd_count &&
                 cv->rp_cache_w == width && cv->rp_cache_h == height);
    if (!fresh) {
        if (cv->rp_dc) {
            if (cv->rp_old_bmp) SelectObject(cv->rp_dc, cv->rp_old_bmp);
            if (cv->rp_bmp) DeleteObject(cv->rp_bmp);
            DeleteDC(cv->rp_dc);
            cv->rp_dc = NULL; cv->rp_bmp = NULL; cv->rp_old_bmp = NULL;
        }
        HDC screen = GetDC(NULL);
        if (!screen) return -1;
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
        ReleaseDC(NULL, screen);
        if (!mem || !bmp) {
            if (bmp) DeleteObject(bmp);
            if (mem) DeleteDC(mem);
            return -1;
        }
        cv->rp_old_bmp = (HBITMAP)SelectObject(mem, bmp);
        canvas_replay_to_dc(cv, mem, width, height);
        cv->rp_dc = mem;
        cv->rp_bmp = bmp;
        cv->rp_cache_gen = cv->rp_gen;
        cv->rp_cache_count = cv->cmd_count;
        cv->rp_cache_w = width;
        cv->rp_cache_h = height;
    }
    COLORREF c = GetPixel(cv->rp_dc, px, py);
    if (c == CLR_INVALID) return -1;
    return (255 << 24) | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

// Map a Win32 virtual-key code to the GDK key name the app-side handlers
// expect (gp/falling_blocks compare against GDK's "Left"/"Return"/"space"…).
// Returns a static string; writes single letters/digits into `buf`.
static const char* vk_to_gdk_name(WPARAM vk, char* buf, int buflen) {
    switch (vk) {
        case VK_LEFT:   return "Left";
        case VK_RIGHT:  return "Right";
        case VK_UP:     return "Up";
        case VK_DOWN:   return "Down";
        case VK_RETURN: return "Return";
        case VK_ESCAPE: return "Escape";
        case VK_DELETE: return "Delete";
        case VK_BACK:   return "BackSpace";
        case VK_TAB:    return "Tab";
        case VK_SPACE:  return "space";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_PRIOR:  return "Page_Up";
        case VK_NEXT:   return "Page_Down";
        default: break;
    }
    // Letters (A-Z) and digits (0-9): GDK reports the lowercase char.
    if ((vk >= 'A' && vk <= 'Z')) {
        if (buflen >= 2) { buf[0] = (char)(vk + 32); buf[1] = '\0'; return buf; }
    }
    if ((vk >= '0' && vk <= '9')) {
        if (buflen >= 2) { buf[0] = (char)vk; buf[1] = '\0'; return buf; }
    }
    return "";
}

static LRESULT CALLBACK canvas_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            // Canvas rescale: record the new size + fire the app's on_resize
            // (which re-flushes the vg scene at the new viewBox->px mapping).
            int cid = 0;
            for (int i = 0; i < canvas_count; i++)
                if (canvases[i].hwnd == hwnd) { cid = i + 1; break; }
            if (cid) {
                Canvas* c = &canvases[cid - 1];
                int nw = LOWORD(lp), nh = HIWORD(lp);
                if (nw > 0 && nh > 0 && (nw != c->width || nh != c->height)) {
                    c->width = nw;
                    c->height = nh;
                    if (c->on_resize && c->on_resize->fn)
                        ((void(*)(void*, intptr_t, intptr_t))c->on_resize->fn)(
                            c->on_resize->env, (intptr_t)nw, (intptr_t)nh);
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            canvas_paint(hwnd, hdc, r.right - r.left, r.bottom - r.top);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEWHEEL: {
            /* The DSL contract is the GTK4 one, which this has to match rather
               than reproduce Windows' own convention: dy < 0 means "away from
               the user", the conventional zoom-IN direction.

               WM_MOUSEWHEEL's delta is the OPPOSITE sign -- positive is a push
               away from the user -- so it is negated here. It arrives in
               multiples of WHEEL_DELTA (120); dividing yields notches, which
               keeps the magnitude comparable to the other backends instead of
               handing an app a number 120x larger on Windows alone. Getting
               either wrong does not fail loudly, it silently inverts or
               over-scales zoom, so both are spelled out. */
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_canvas_scroll && cv->on_canvas_scroll->fn) {
                    double notches =
                        (double)GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA;
                    ((void(*)(void*, double, double))cv->on_canvas_scroll->fn)(
                        cv->on_canvas_scroll->env, 0.0, -notches);
                    return 0;
                }
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_MOUSEMOVE: {
            // Forward canvas-local (x, y) to the on_move hook. WM_MOUSEMOVE
            // lParam carries client-area coords (top-left origin) — the same
            // space the canvas draws in.
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_move && cv->on_move->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_move->fn)(
                        cv->on_move->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // Pointer press → on_click(x, y). Also grab focus so subsequent
            // keys reach the canvas (mirrors the GTK focus-on-click path).
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                SetFocus(hwnd);
                if (cv->on_click && cv->on_click->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_click->fn)(
                        cv->on_click->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_release && cv->on_release->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_release->fn)(
                        cv->on_release->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            // A double-click delivers DOWN, UP, DBLCLK, UP. Apps that treat a
            // fast second click as a "drill" (gp) rely on the second press
            // arriving — synthesize it as another on_click so the double-tap
            // path fires the handler twice, matching GTK's n_press semantics
            // where the app counts its own presses.
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_click && cv->on_click->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_click->fn)(
                        cv->on_click->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_GETDLGCODE:
            // Claim arrow keys and character keys so they arrive as WM_KEYDOWN
            // here (canvas nav) instead of being eaten by dialog navigation.
            // NOT Tab — Tab must still move focus between the app's widgets.
            // (Return/Escape reach WM_KEYDOWN via the message loop, which only
            // special-cases Tab for IsDialogMessage.)
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_key && cv->on_key->fn) {
                    char buf[8];
                    const char* name = vk_to_gdk_name(wp, buf, sizeof(buf));
                    if (name[0]) {
                        ((void(*)(void*, const char*))cv->on_key->fn)(
                            cv->on_key->env, name);
                    }
                }
            }
            return 0;
        }
        case WM_KEYUP: {
            // Mirror of WM_KEYDOWN for the key-release hook, so held-key
            // state (down sets, up clears) works with real keyboards.
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_key_release && cv->on_key_release->fn) {
                    char buf[8];
                    const char* name = vk_to_gdk_name(wp, buf, sizeof(buf));
                    if (name[0]) {
                        ((void(*)(void*, const char*))cv->on_key_release->fn)(
                            cv->on_key_release->env, name);
                    }
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Animation: opacity interpolation via WM_TIMER.
// ---------------------------------------------------------------------------
typedef struct {
    int widget_handle;
    double start, target;
    int duration_ms;
    DWORD start_tick;
    int timer_id;
} Animation;
static Animation* anims = NULL;
static int anim_count = 0;
static int anim_cap = 0;

static void CALLBACK anim_tick(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < anim_count; i++) {
        if ((UINT_PTR)anims[i].timer_id != id) continue;
        DWORD elapsed = GetTickCount() - anims[i].start_tick;
        double t = (double)elapsed / (double)anims[i].duration_ms;
        if (t >= 1.0) t = 1.0;
        double v = anims[i].start + (anims[i].target - anims[i].start) * t;
        aether_ui_set_opacity(anims[i].widget_handle, v);
        if (t >= 1.0) {
            KillTimer(NULL, anims[i].timer_id);
            // swap-delete
            anims[i] = anims[anim_count - 1];
            anim_count--;
        }
        return;
    }
}

void aether_ui_animate_opacity_impl(int handle, double target, int duration_ms) {
    if (anim_count >= anim_cap) {
        anim_cap = anim_cap == 0 ? 8 : anim_cap * 2;
        anims = (Animation*)realloc(anims, sizeof(Animation) * anim_cap);
    }
    Widget* w = widget_at(handle);
    double start = (w && w->opacity >= 0) ? w->opacity : 1.0;
    Animation* a = &anims[anim_count++];
    a->widget_handle = handle;
    a->start = start;
    a->target = target;
    a->duration_ms = duration_ms > 0 ? duration_ms : 200;
    a->start_tick = GetTickCount();
    a->timer_id = next_timer_id++;
    SetTimer(NULL, a->timer_id, 16, (TIMERPROC)anim_tick);
}

// ---------------------------------------------------------------------------
// Widget manipulation: remove/clear children.
// ---------------------------------------------------------------------------
void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    Widget* c = widget_at(child_handle);
    Widget* p = widget_at(parent_handle);
    if (c && c->hwnd) {
        /* mark_subtree_dead FIRST, exactly as clear_children and navstack_pop
           do: the registry never shrinks and Windows recycles HWND values, so
           the `dead` flag is the only thing that tells the driver a widget is
           gone. It must run BEFORE DestroyWindow, because afterwards GetWindow
           can no longer enumerate the children to mark them. Without it a
           removed widget keeps reporting a live type and stays clickable. */
        mark_subtree_dead(c->hwnd);
        DestroyWindow(c->hwnd);
    }
    if (p && (p->kind == WK_VSTACK || p->kind == WK_HSTACK || p->kind == WK_ZSTACK)) {
        stack_do_layout(p->hwnd);
    }
}

// Mark a widget and every descendant registered under it as dead, so the
// driver stops listing them. Walks the live child tree BEFORE DestroyWindow
// tears it down (afterwards GetWindow can't enumerate it).
// Release the per-handle state a retired widget owned. Handles are monotonic,
// so nothing inherits it and this is not a correctness bug; it is a leak, and
// on Windows a GDI one, which matters more than a stray malloc: a process has
// a hard handle quota (10,000 by default), and every list rebuild retires its
// rows. A tinted list that repopulates often would eventually stop drawing.
static void w32_release_handle_state(int h) {
    if (h < 1) return;
    if (h <= w32_tint_len) {
        if (w32_tint_base[h - 1]) { DeleteObject(w32_tint_base[h - 1]);
                                    w32_tint_base[h - 1] = NULL; }
        if (w32_tint_icon[h - 1]) { DestroyIcon(w32_tint_icon[h - 1]);
                                    w32_tint_icon[h - 1] = NULL; }
        w32_tint_rgb[h - 1] = 0;
    }
    if (h <= w32_drag_paths_len && w32_drag_paths[h - 1]) {
        free(w32_drag_paths[h - 1]);
        w32_drag_paths[h - 1] = NULL;
    }
    // The opacity tween runs on a THREAD timer, not a window timer, so
    // destroying the HWND does not stop it. It would keep waking at 60Hz and
    // finding a dead window until its own duration ran out.
    Widget* w = widget_at(h);
    if (w && w->tr_timer) {
        KillTimer(NULL, w->tr_timer);
        w->tr_timer = 0;
    }
}

static void mark_subtree_dead(HWND hwnd) {
    int h = handle_for_hwnd(hwnd);
    if (h > 0) {
        Widget* w = widget_at(h);
        if (w) w->dead = 1;
        w32_release_handle_state(h);
    }
    HWND c = GetWindow(hwnd, GW_CHILD);
    while (c) {
        HWND next = GetWindow(c, GW_HWNDNEXT);
        mark_subtree_dead(c);
        c = next;
    }
}

void aether_ui_clear_children_impl(int handle) {
    Widget* p = widget_at(handle);
    if (!p) return;
    HWND c = GetWindow(p->hwnd, GW_CHILD);
    while (c) {
        HWND next = GetWindow(c, GW_HWNDNEXT);
        mark_subtree_dead(c);   // flag the whole row subtree before it's gone
        DestroyWindow(c);
        c = next;
    }
    if (p->kind == WK_VSTACK || p->kind == WK_HSTACK || p->kind == WK_ZSTACK) {
        stack_do_layout(p->hwnd);
    }
}

// ---------------------------------------------------------------------------
// AetherUIDriver — Win32 adapter.
//
// The HTTP test server (socket accept, parsing, routing, JSON) lives in
// aether_ui_test_server.c and is shared with the GTK4 and AppKit
// backends. This section only provides the Win32-specific pieces:
//
//   * Backend hooks that answer widget introspection queries from the
//     server thread (widget_type, widget_text, toggle_active, etc.)
//   * UI-thread marshalling: HTTP requests fill an AetherDriverActionCtx
//     and hand it to dispatch_action(), which SendMessages it to a
//     hidden AE_WM_DRIVER window on the UI thread and blocks until the
//     action completes.
//   * Banner creation and sealing.
// ---------------------------------------------------------------------------

#include "aether_ui_test_server.h"

#define AE_WM_DRIVER (WM_USER + 0x42)

static HWND driver_host_hwnd = NULL;

// Widget-kind → short string (used as the "type" field in the driver JSON).
static const char* widget_kind_name(WidgetKind k) {
    switch (k) {
        case WK_TEXT: return "text";
        case WK_BUTTON: return "button";
        case WK_TEXTFIELD: return "textfield";
        case WK_SECUREFIELD: return "securefield";
        case WK_TEXTAREA: return "textarea";
        case WK_TOGGLE: return "toggle";
        case WK_SLIDER: return "slider";
        case WK_PICKER: return "picker";
        case WK_PROGRESSBAR: return "progressbar";
        case WK_IMAGE: return "image";
        case WK_VSTACK: return "vstack";
        case WK_HSTACK: return "hstack";
        case WK_ZSTACK: return "zstack";
        case WK_FORM: return "form";
        case WK_FORM_SECTION: return "form_section";
        case WK_NAVSTACK: return "navstack";
        case WK_SCROLLVIEW: return "scrollview";
        case WK_SPACER: return "spacer";
        case WK_DIVIDER: return "divider";
        case WK_CANVAS: return "canvas";
        case WK_WINDOW: return "window";
        case WK_SHEET: return "sheet";
        case WK_SCRIM: return "scrim";
        case WK_TABS: return "tabs";
        case WK_SPLITVIEW: return "splitview";
        case WK_WRAP: return "wrap";
        default: return "widget";
    }
}

// Seal APIs — delegate to the shared driver so the server sees a single
// source of truth. We also keep a per-widget `sealed` flag as a fast
// path for the WM_COMMAND handler, which fires on every button click.
void aether_ui_seal_widget_impl(int handle) {
    Widget* w = widget_at(handle);
    if (w) w->sealed = 1;
    aether_ui_test_server_seal_widget(handle);
}

void aether_ui_seal_subtree_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return;
    aether_ui_seal_widget_impl(handle);
    for (HWND c = GetWindow(w->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        int ch = handle_for_hwnd(c);
        if (ch) aether_ui_seal_subtree_impl(ch);
    }
}

// ---------------------------------------------------------------------------
// Backend hooks for the shared test server.
// ---------------------------------------------------------------------------
static int hook_widget_count(void) { return widget_count; }

static const char* hook_widget_type(int handle) {
    Widget* w = widget_at(handle);
    // The registry never shrinks (widgets are never unregistered), so a row
    // that clear_children DestroyWindow'd still has a live Widget* here. Its
    // HWND is dead, though — report it as "null" so the shared server drops it
    // from /widgets, matching GTK4 (where destroy also unregisters). Without
    // this, a rebuilt listbox leaves stale rows the driver can still resolve
    // by text — and firing their captured on_drop(i) reorders by a STALE index.
    if (!w) return "null";
    if (w->dead || !IsWindow(w->hwnd)) return "null";
    return widget_kind_name(w->kind);
}

static void hook_widget_text_into(int handle, char* buf, int bufsize) {
    Widget* w = widget_at(handle);
    if (!w || !w->hwnd) { buf[0] = '\0'; return; }
    wchar_t wbuf[1024];
    GetWindowTextW(w->hwnd, wbuf, 1024);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, bufsize, NULL, NULL);
}

static int hook_widget_visible(int handle) {
    // The widget's OWN visibility flag — parity with GTK's
    // gtk_widget_get_visible. NOT IsWindowVisible: that requires the
    // whole ancestor chain shown, and under an ssh service session the
    // top-level window never is, which zeroed "visible" app-wide.
    Widget* w = widget_at(handle);
    if (!w || !IsWindow(w->hwnd)) return 0;
    return (GetWindowLongPtrW(w->hwnd, GWL_STYLE) & WS_VISIBLE) ? 1 : 0;
}

static int hook_widget_parent(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return 0;
    return handle_for_hwnd(GetParent(w->hwnd));
}

// ── Stylesheet-walk ABI (ui.apply_styles) ───────────────────────────
// In-process tree walk for the DSL + packed-color readback for the driver.
int aether_ui_widget_count_impl(void) { return widget_count; }

const char* aether_ui_widget_kind_impl(int handle) {
    const char* t = hook_widget_type(handle);   // dead-safe ("null" for gone)
    return (t && strcmp(t, "null") != 0) ? t : "";
}

int aether_ui_widget_parent_impl(int handle) {
    return hook_widget_parent(handle);
}

const char* aether_ui_widget_classes_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w || !IsWindow(w->hwnd) || w->dead) return "";
    return w->classes ? w->classes : "";
}

// COLORREF packs 0x00BBGGRR — repack to the driver's 0xRRGGBB.
static int colorref_to_packed(COLORREF c) {
    return ((int)GetRValue(c) << 16) | ((int)GetGValue(c) << 8) | (int)GetBValue(c);
}

int aether_ui_styled_bg_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    /* EFFECTIVE colour — what this widget is painting right now, state
       ladder included, so a spec can tell "hover painted" from "hover
       recorded". Mirrors both paint sites (ownerdraw button, container
       erase). */
    if (w->active_set && w->is_pressed) return colorref_to_packed(w->active_bg);
    if (w->hover_set && w->is_hovered)  return colorref_to_packed(w->hover_bg);
    if (!w->bg.has_value) return -1;
    return colorref_to_packed(w->bg.color);
}

int aether_ui_styled_fg_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w || !w->fg.has_value) return -1;
    return colorref_to_packed(w->fg.color);
}

static int hook_toggle_active(int handle) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_TOGGLE) return 0;
    return SendMessageW(w->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
}

static double hook_slider_value(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_SLIDER) ? w->u.slider.cur_v : 0.0;
}

static double hook_progressbar_fraction(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_PROGRESSBAR) ? w->u.progressbar.fraction : 0.0;
}

static int hook_widget_enabled(int handle) {
    Widget* w = widget_at(handle);
    return (w && IsWindowEnabled(w->hwnd)) ? 1 : 0;
}

// Window-local rect (parity with the GTK server's x/y/w/h): position is
// relative to the top-level window's CLIENT area.
static int hook_widget_rect(int handle, int* x, int* y, int* wd, int* hgt) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    RECT r;
    if (!GetWindowRect(w->hwnd, &r)) return -1;
    HWND top = GetAncestor(w->hwnd, GA_ROOT);
    POINT tl = { r.left, r.top };
    if (top) ScreenToClient(top, &tl);
    *x = tl.x;
    *y = tl.y;
    *wd = r.right - r.left;
    *hgt = r.bottom - r.top;
    return 0;
}

static void hook_widget_classes_into(int handle, char* buf, int bufsize) {
    // Registry slots outlive destroyed HWNDs (append-only): a dead row
    // must not keep reporting .aui-row-selected to specs.
    Widget* w = widget_at(handle);
    if (!w || !IsWindow(w->hwnd)) { buf[0] = '\0'; return; }
    snprintf(buf, bufsize, "%s", w->classes ? w->classes : "");
}

static void hook_widget_a11y(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    aether_ui_a11y_get_impl(handle, role, rolesz, name, namesz, desc, descsz);
}

static int hook_focused_widget(void) {
    // GetFocus() is PER-THREAD (the HTTP thread's queue never has focus)
    // — query the UI thread's focus via GetGUIThreadInfo.
    if (app_count < 1 || !apps[0].hwnd) return 0;
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    DWORD tid = GetWindowThreadProcessId(apps[0].hwnd, NULL);
    if (!GetGUIThreadInfo(tid, &gti)) return 0;
    return handle_for_hwnd(gti.hwndFocus);
}

// Public focus getter (for the DSL / shortcut predicates). Same UI-thread
// focus, resolved to our registry handle.
int aether_ui_focused_widget(void) {
    return hook_focused_widget();
}

// dispatch_action: sends the ctx to the UI thread via AE_WM_DRIVER and
// blocks until the WndProc fills in ctx->result. Runs on the server
// thread — SendMessageW is synchronous so no explicit wait is needed.
static LRESULT CALLBACK driver_host_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    if (msg == AE_WM_DRIVER) {
        AetherDriverActionCtx* ctx = (AetherDriverActionCtx*)lp;
        if (ctx->action == AETHER_DRV_SET_STATE) {
            // Typed dispatch (sval carries the raw v=); setters walk
            // bindings, so this runs on the UI thread by construction.
            switch (aether_ui_state_type(ctx->handle)) {
                case 1: aether_ui_state_set_i(ctx->handle, atoi(ctx->sval)); break;
                case 2: aether_ui_state_set_b(ctx->handle,
                            (strcmp(ctx->sval, "true") == 0 || atoi(ctx->sval) != 0)); break;
                case 3: aether_ui_state_set_s(ctx->handle, ctx->sval); break;
                default: aether_ui_state_set(ctx->handle, ctx->dval);
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_WIN_RESIZE) {
            // Resize the app's top-level window to the given CLIENT size
            // (mirrors the GTK route's semantics).
            if (app_count > 0 && apps[0].hwnd) {
                RECT rc = { 0, 0, ctx->ival, ctx->ival2 };
                AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                         GetDpiForWindow(apps[0].hwnd));
                SetWindowPos(apps[0].hwnd, NULL, 0, 0,
                             rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_WIN_KEY) {
            // Tab / Shift+Tab move REAL focus; Escape dismisses an overlay;
            // any other combo fires a registered shortcut / chord (scoped
            // shortcuts honour their predicate).
            ctx->retval = 0;
            if (strcmp(ctx->sval, "Escape") != 0 &&
                strcmp(ctx->sval, "Tab") != 0 &&
                strcmp(ctx->sval, "Shift+Tab") != 0) {
                ctx->retval = aeui_win32_fire_shortcut(ctx->sval);
                if (!ctx->retval) {
                    /* Nothing bound wanted it: fall through to the any-key
                       handler, the same path a REAL keystroke takes above
                       (and the same fall-through GTK4's dispatch does).
                       Without this the keyhandler suite was 0/6 on win32 —
                       the closure was stored and the real-key path fired
                       it, but the DRIVER route stopped at shortcut-miss.
                       Mods bits mirror GTK4's split: Shift=1, Control=2,
                       Alt=4, Super=8. */
                    const char* rest = ctx->sval;
                    int kmods = 0;
                    for (;;) {
                        if      (strncmp(rest, "Ctrl+",  5) == 0) { kmods |= 2; rest += 5; }
                        else if (strncmp(rest, "Shift+", 6) == 0) { kmods |= 1; rest += 6; }
                        else if (strncmp(rest, "Alt+",   4) == 0) { kmods |= 4; rest += 4; }
                        else if (strncmp(rest, "Super+", 6) == 0) { kmods |= 8; rest += 6; }
                        else break;
                    }
                    ctx->retval = aether_ui_window_key_deliver(rest, kmods);
                }
                ctx->result = 0;
                ctx->done = 1;
                return 0;
            }
            if (strcmp(ctx->sval, "Escape") == 0) {
                // Escape dismisses the topmost live overlay (the same
                // wiring GTK/macOS give it); unhandled when none is open.
                ctx->retval = w32_escape_overlays();
                ctx->result = 0;
                ctx->done = 1;
                return 0;
            }
            if (strcmp(ctx->sval, "Tab") == 0 ||
                strcmp(ctx->sval, "Shift+Tab") == 0) {
                // Driver Tab walks the widget REGISTRY, whose order IS build
                // order — matching GTK and macOS (which does exactly this walk
                // because AppKit's key-view loop depends on an OS setting).
                // Real keyboard Tab keeps Windows dialog order; this is the
                // HEADLESS route's contract, same as the other backends'.
                int back = (ctx->sval[0] == 'S');
                int cur = handle_for_hwnd(GetFocus());
                int n = widget_count;
                int start = cur >= 1 ? cur : (back ? 1 : n);
                for (int step = 1; step <= n; step++) {
                    int h2 = back ? start - step : start + step;
                    if (h2 < 1) h2 += n;
                    if (h2 > n) h2 -= n;
                    Widget* cand = widget_at(h2);
                    if (!cand || cand->dead || !IsWindow(cand->hwnd)) continue;
                    LONG style = GetWindowLongW(cand->hwnd, GWL_STYLE);
                    if (!(style & WS_TABSTOP)) continue;
                    if (!(style & WS_VISIBLE)) continue;
                    if (!IsWindowEnabled(cand->hwnd)) continue;
                    SetFocus(cand->hwnd);
                    ctx->retval = 1;
                    break;
                }
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_SHUTDOWN) {
            // Same exit path a user-close takes, so the port is released and
            // the next spec in the matrix doesn't interrogate this app.
            if (app_count > 0 && apps[0].hwnd) {
                PostMessageW(apps[0].hwnd, WM_CLOSE, 0, 0);
            } else {
                PostQuitMessage(0);
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_SPLIT_POS) {
            // The Win32 splitview is still a plain stack (no divider), so
            // split_position_impl answers -1. Report that rather than a
            // fabricated 0 — a spec must be able to tell "unwired" from
            // "the divider happens to sit at zero".
            if (ctx->ival >= 0) aether_ui_split_set_position_impl(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_split_position_impl(ctx->handle);
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_TAB_SELECT) {
            // Real tab strip: select the page and read the resulting index back.
            aether_ui_tabs_select(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_tabs_selected(ctx->handle);
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_PICK) {
            // Real hit-test against the client area, descending to the
            // DEEPEST child at the point (ChildWindowFromPointEx only looks
            // one level down — stopping there resolved the root vstack, not
            // the button under the pointer). With a modal open, the raised
            // scrim is the topmost child at every uncovered point, so the
            // pick honestly reports the glass pane eating the click.
            POINT pt = { ctx->ival, ctx->ival2 };  // read y BEFORE ival2 becomes the out-param
            ctx->retval = 0;
            ctx->ival2 = 0;
            if (app_count > 0 && apps[0].hwnd) {
                // Manual top-to-bottom Z walk at each level. The API route
                // (ChildWindowFromPointEx) skips WS_EX_LAYERED children —
                // which is exactly what the modal scrim is — so the glass
                // pane was invisible to picks and they fell through to the
                // app beneath. GetWindow(GW_CHILD) starts at the TOP of Z.
                HWND cur = apps[0].hwnd;
                for (;;) {
                    HWND hit = NULL;
                    for (HWND c = GetWindow(cur, GW_CHILD); c;
                         c = GetWindow(c, GW_HWNDNEXT)) {
                        // Own WS_VISIBLE bit, NOT IsWindowVisible: under a
                        // hidden/headless toplevel the ancestor-chain check
                        // reads the whole tree invisible (the documented
                        // win32 lesson) and every pick would return none.
                        if (!(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
                        RECT r;
                        GetWindowRect(c, &r);
                        POINT sp = pt;
                        ClientToScreen(cur, &sp);
                        if (PtInRect(&r, sp)) { hit = c; break; }
                    }
                    if (!hit) break;
                    MapWindowPoints(cur, hit, &pt, 1);
                    cur = hit;
                }
                if (cur != apps[0].hwnd) {
                    int hh = handle_for_hwnd(cur);
                    ctx->retval = hh;
                    Widget* hw = hh ? widget_at(hh) : NULL;
                    if (hw && hw->kind == WK_SCRIM) ctx->ival2 = 1;
                }
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_CANVAS_CLICK
            || ctx->action == AETHER_DRV_CANVAS_MOVE
            || ctx->action == AETHER_DRV_CANVAS_RELEASE
            || ctx->action == AETHER_DRV_CANVAS_KEY
            || ctx->action == AETHER_DRV_CANVAS_SCROLL
            || ctx->action == AETHER_DRV_CANVAS_KEYUP) {
            // Drive the canvas hit-test hooks directly, exactly as a real
            // WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP / WM_KEYDOWN would
            // (result 3 = "no such handler wired", so a spec can tell a missed
            // click from an unwired canvas).
            ctx->result = 3;
            if (ctx->handle >= 1 && ctx->handle <= canvas_count) {
                Canvas* cv = &canvases[ctx->handle - 1];
                if (ctx->action == AETHER_DRV_CANVAS_MOVE) {
                    if (cv->on_move && cv->on_move->fn) {
                        ((void(*)(void*, double, double))cv->on_move->fn)(
                            cv->on_move->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else if (ctx->action == AETHER_DRV_CANVAS_CLICK) {
                    if (cv->on_click && cv->on_click->fn) {
                        ((void(*)(void*, double, double))cv->on_click->fn)(
                            cv->on_click->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else if (ctx->action == AETHER_DRV_CANVAS_RELEASE) {
                    if (cv->on_release && cv->on_release->fn) {
                        ((void(*)(void*, double, double))cv->on_release->fn)(
                            cv->on_release->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else if (ctx->action == AETHER_DRV_CANVAS_SCROLL) {
                    if (cv->on_canvas_scroll && cv->on_canvas_scroll->fn) {
                        ((void(*)(void*, double, double))cv->on_canvas_scroll->fn)(
                            cv->on_canvas_scroll->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else if (ctx->action == AETHER_DRV_CANVAS_KEYUP) {
                    if (cv->on_key_release && cv->on_key_release->fn) {
                        ((void(*)(void*, const char*))cv->on_key_release->fn)(
                            cv->on_key_release->env, ctx->sval);
                        ctx->result = 0;
                    }
                } else { // AETHER_DRV_CANVAS_KEY
                    if (cv->on_key && cv->on_key->fn) {
                        ((void(*)(void*, const char*))cv->on_key->fn)(
                            cv->on_key->env, ctx->sval);
                        ctx->result = 0;
                    }
                }
            }
            ctx->done = 1;
            return 0;
        }
        Widget* w = widget_at(ctx->handle);
        /* A dead widget is a MISSING widget to the driver. The registry
           never shrinks (HWND values recycle), so widget_at still returns
           the entry after clear_children/remove_child destroyed it — the
           listing hooks already skip dead, but this dispatch didn't, so a
           removed cell answered 200 and FIRED ITS CALLBACK (clearchildren
           spec, first win32 run). Same 404 as a never-existed handle. */
        if (w && w->dead) { ctx->result = 3; ctx->done = 1; return 0; }
        if (!w) {
            // hover(0) is legitimate: "the pointer is over NOTHING". Clear
            // every hover rather than reporting a missing widget.
            if (ctx->action == AETHER_DRV_HOVER && ctx->handle == 0) {
                for (int i = 1; i <= widget_count; i++) {
                    Widget* pw = widget_at(i);
                    if (pw && pw->is_hovered) {
                        pw->is_hovered = 0;
                        InvalidateRect(pw->hwnd, NULL, TRUE);
                    }
                }
                ctx->retval = 1; ctx->result = 0; ctx->done = 1; return 0;
            }
            ctx->result = 3; ctx->done = 1; return 0;
        }
        if (ctx->action == AETHER_DRV_FOCUS) {
            SetFocus(w->hwnd);
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->handle == aether_ui_test_server_banner_handle()) {
            ctx->result = 2; ctx->done = 1; return 0;
        }
        if (aether_ui_test_server_is_sealed(ctx->handle)) {
            ctx->result = 1; ctx->done = 1; return 0;
        }
        switch (ctx->action) {
            case AETHER_DRV_HOVER: {
                // Move the pointer onto (or off) a widget the way a user
                // would: clear the previous hover, set the new one, and let
                // both repaint. This is what makes :hover styling testable
                // on a headless box, where no real pointer exists.
                for (int i = 1; i <= widget_count; i++) {
                    Widget* pw = widget_at(i);
                    if (pw && pw->is_hovered) {
                        pw->is_hovered = 0;
                        InvalidateRect(pw->hwnd, NULL, TRUE);
                    }
                }
                if (w) {
                    w->is_hovered = 1;
                    InvalidateRect(w->hwnd, NULL, TRUE);
                    UpdateWindow(w->hwnd);
                }
                ctx->retval = 1;
                break;
            }
            case AETHER_DRV_PRESS:
                if (w) {
                    w->is_pressed = 1;   /* containers: no BM_ state */
                    SendMessageW(w->hwnd, BM_SETSTATE, TRUE, 0);
                    InvalidateRect(w->hwnd, NULL, TRUE);
                    UpdateWindow(w->hwnd);
                    ctx->retval = 1;
                }
                break;
            case AETHER_DRV_RELEASE:
                if (w) {
                    w->is_pressed = 0;
                    SendMessageW(w->hwnd, BM_SETSTATE, FALSE, 0);
                    InvalidateRect(w->hwnd, NULL, TRUE);
                    UpdateWindow(w->hwnd);
                    ctx->retval = 1;
                }
                break;
            case AETHER_DRV_CLICK:
                // Buttons and ANY widget with an on_click handler (listbox
                // rows are plain containers) — mirrors the GTK4 server's
                // gesture-closure fallback: invoke the handler a real click
                // would run.
                if (w->kind == WK_BUTTON || w->on_click) invoke_closure(w->on_click);
                break;
            case AETHER_DRV_SET_TEXT:
                if (w->kind == WK_TEXT || w->kind == WK_TEXTFIELD
                    || w->kind == WK_SECUREFIELD || w->kind == WK_TEXTAREA) {
                    SetWindowTextW(w->hwnd, utf8_to_wide(ctx->sval));
                    // SetWindowTextW from CODE does not send EN_CHANGE (only
                    // user edits do), so the app's on_change would never run
                    // and a driver-typed value would be invisible to the app
                    // — while GTK4's gtk_entry_buffer_set_text DOES notify.
                    // Fire the same path EN_CHANGE takes, for parity.
                    if (w->kind != WK_TEXT) {
                        if (!w->sealed)
                            invoke_closure_str(w->on_change,
                                               aether_ui_textfield_get_text(ctx->handle));
                        if (w->bound_state > 0) {
                            const char* t = aether_ui_textfield_get_text(ctx->handle);
                            StateCell* sc = state_cell(w->bound_state);
                            if (sc && sc->type == AEUI_STATE_STRING
                                && (!sc->str || strcmp(sc->str, t ? t : "") != 0)) {
                                aether_ui_state_set_s(w->bound_state, t ? t : "");
                            }
                        }
                    }
                }
                break;
            case AETHER_DRV_TOGGLE:
                if (w->kind == WK_TOGGLE) {
                    int cur = aether_ui_toggle_get_active(ctx->handle);
                    aether_ui_toggle_set_active(ctx->handle, !cur);
                    if (!cur) w32_radio_enforce(ctx->handle);   // now active
                    invoke_closure(w->on_change);
                }
                break;
            case AETHER_DRV_CTX_MENU:
                // "Mapped" = this widget (or itself) has a menu to show. The
                // headless driver doesn't need a visible popup; the store IS
                // the menu (activation drives the same closures).
                ctx->retval = (w->ctx_count > 0) ? 1 : 0;
                break;
            case AETHER_DRV_CTX_ACTIVATE:
                if (ctx->ival >= 0 && ctx->ival < w->ctx_count) {
                    invoke_closure((AeClosure*)w->ctx_items[ctx->ival].closure);
                    ctx->retval = 1;
                } else {
                    ctx->retval = 0;
                }
                break;
            case AETHER_DRV_SET_VALUE:
                if (w->kind == WK_SLIDER)
                    aether_ui_slider_set_value(ctx->handle, ctx->dval);
                else if (w->kind == WK_PROGRESSBAR)
                    aether_ui_progressbar_set_fraction(ctx->handle, ctx->dval);
                else if (w->kind == WK_PICKER) {
                    // Driver selects a picker index via set_value (the
                    // surface-agnostic path the drawn/native picker share);
                    // route it and fire on_change so the round-trip matches
                    // GtkDropDown's notify::selected.
                    aether_ui_picker_set_selected(ctx->handle, (int)ctx->dval);
                    if (w->on_change && w->on_change->fn) {
                        ((void(*)(void*, intptr_t))w->on_change->fn)(
                            w->on_change->env, (intptr_t)(int)ctx->dval);
                    }
                }
                break;
            default: break;
        }
        ctx->result = 0;
        ctx->done = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void hook_dispatch_action(AetherDriverActionCtx* ctx) {
    SendMessageW(driver_host_hwnd, AE_WM_DRIVER, 0, (LPARAM)ctx);
}

// List direct children of a widget. Returns the number written; -1 if
// the widget itself wasn't found.
static int hook_widget_children(int handle, int* out, int max) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    int n = 0;
    for (HWND c = GetWindow(w->hwnd, GW_CHILD); c && n < max;
         c = GetWindow(c, GW_HWNDNEXT)) {
        int ch = handle_for_hwnd(c);
        if (ch > 0) {
            if (out) out[n] = ch;
            n++;
        }
    }
    return n;
}

// Screenshot the app's first window to a PNG in memory.
// Uses BitBlt + GDI+ (via the same flat-API binding we use for images).
__declspec(dllimport) int __stdcall GdipCreateBitmapFromHBITMAP(
    HBITMAP hbm, HPALETTE pal, void** bitmap);
__declspec(dllimport) int __stdcall GdipSaveImageToStream(
    void* image, void* stream, const GUID* clsid, const void* params);
__declspec(dllimport) int __stdcall GdipGetImageEncodersSize(
    unsigned int* num_encoders, unsigned int* size);
__declspec(dllimport) int __stdcall GdipGetImageEncoders(
    unsigned int num_encoders, unsigned int size, void* encoders);

static int hook_widget_hovered(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->is_hovered) ? 1 : 0;
}
static int hook_widget_pressed(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return 0;
    return (SendMessageW(w->hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) ? 1 : 0;
}

static int hook_screenshot_png(unsigned char** out_data, size_t* out_len) {
    if (app_count == 0) return -1;
    HWND hwnd = apps[0].hwnd;
    if (!hwnd) return -1;
    RECT r;
    if (!GetClientRect(hwnd, &r)) return -1;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return -1;

    HDC src = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(src);
    HBITMAP bmp = CreateCompatibleBitmap(src, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    // PrintWindow ASKS the window (and, with PW_RENDERFULLCONTENT, its
    // children) to render itself into our DC via WM_PRINT/WM_PRINTCLIENT.
    // BitBlt alone copies whatever the compositor has on screen, which is
    // NOTHING when the window never maps — the case on a headless/session-0
    // box, and exactly where a screenshot is most useful. Fall back to
    // BitBlt if PrintWindow fails (older shells).
    #ifndef PW_RENDERFULLCONTENT
    #define PW_RENDERFULLCONTENT 0x00000002
    #endif
    // 1. The window's own background.
    HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    RECT full = { 0, 0, w, h };
    FillRect(mem, &full, face);
    DeleteObject(face);
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)) {
        BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    }
    // 2. Ask each REGISTERED widget to render itself at its own position.
    //    PrintWindow on the toplevel misses children that live under the
    //    off-screen widget_holder (never composited), which is every control
    //    on a window that has not mapped — i.e. exactly the headless case.
    for (int wi = 1; wi <= widget_count; wi++) {
        Widget* cw = widget_at(wi);
        if (!cw || !cw->hwnd || !IsWindow(cw->hwnd)) continue;
        if (!IsWindowVisible(cw->hwnd) && !cw->owner_drawn) continue;
        RECT cr;
        if (!GetWindowRect(cw->hwnd, &cr)) continue;
        POINT tl = { cr.left, cr.top };
        ScreenToClient(hwnd, &tl);
        int cwid = cr.right - cr.left, chgt = cr.bottom - cr.top;
        if (cwid <= 0 || chgt <= 0) continue;
        SaveDC(mem);
        SetViewportOrgEx(mem, tl.x, tl.y, NULL);
        IntersectClipRect(mem, 0, 0, cwid, chgt);
        SendMessageW(cw->hwnd, WM_PRINTCLIENT, (WPARAM)mem,
                     PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT);
        RestoreDC(mem, -1);
    }
    SelectObject(mem, old);

    ensure_gdiplus();
    void* gdi_bitmap = NULL;
    int rc = -1;
    if (gdiplus_started
        && GdipCreateBitmapFromHBITMAP(bmp, NULL, &gdi_bitmap) == 0
        && gdi_bitmap) {
        // Serialize to an IStream, then copy its bytes out.
        IStream* stream = NULL;
        if (CreateStreamOnHGlobal(NULL, TRUE, &stream) == S_OK && stream) {
            // PNG codec CLSID {557CF406-1A04-11D3-9A73-0000F81EF32E}
            GUID png_clsid = {0x557cf406, 0x1a04, 0x11d3,
                              {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            if (GdipSaveImageToStream(gdi_bitmap, stream,
                                       &png_clsid, NULL) == 0) {
                HGLOBAL hg;
                if (GetHGlobalFromStream(stream, &hg) == S_OK) {
                    size_t sz = GlobalSize(hg);
                    void* src_ptr = GlobalLock(hg);
                    if (src_ptr && sz > 0) {
                        *out_data = (unsigned char*)malloc(sz);
                        memcpy(*out_data, src_ptr, sz);
                        *out_len = sz;
                        rc = 0;
                    }
                    GlobalUnlock(hg);
                }
            }
            stream->lpVtbl->Release(stream);
        }
        GdipDisposeImage(gdi_bitmap);
    }

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, src);
    return rc;
}

// Last-paint metrics for GET /canvas/{id}/debug. Returns 0 on success,
// non-zero when the canvas id is unknown (the shared server maps that to
// 404; a NULL hook would be 501 "not wired on this backend").
static int hook_canvas_debug(int canvas_id, int* area, int* commands,
                             int* w, int* h) {
    if (canvas_id < 1 || canvas_id > canvas_count) return 1;
    Canvas* cv = &canvases[canvas_id - 1];
    if (area) *area = cv->last_paint_area;
    if (commands) *commands = cv->last_paint_count;
    if (w) *w = cv->last_paint_w;
    if (h) *h = cv->last_paint_h;
    return 0;
}

static int hook_canvas_paint_counters(int canvas_id, int* full_paints,
                                      int* clip_paints, int* last_clip_area) {
    if (canvas_id < 1 || canvas_id > canvas_count) return 1;
    Canvas* cv = &canvases[canvas_id - 1];
    if (full_paints) *full_paints = cv->full_paints;
    if (clip_paints) *clip_paints = cv->clip_paints;
    if (last_clip_area) *last_clip_area = cv->last_clip_area;
    return 0;
}

static const AetherDriverHooks win32_driver_hooks = {
    .widget_count         = hook_widget_count,
    .widget_type          = hook_widget_type,
    .widget_text_into     = hook_widget_text_into,
    .widget_visible       = hook_widget_visible,
    .widget_parent        = hook_widget_parent,
    .toggle_active        = hook_toggle_active,
    .slider_value         = hook_slider_value,
    .progressbar_fraction = hook_progressbar_fraction,
    .dispatch_action      = hook_dispatch_action,
    .widget_children      = hook_widget_children,
    .widget_enabled       = hook_widget_enabled,
    .widget_rect          = hook_widget_rect,
    .widget_classes_into  = hook_widget_classes_into,
    .widget_a11y          = hook_widget_a11y,
    .focused_widget       = hook_focused_widget,
    .widget_hovered       = hook_widget_hovered,
    .widget_pressed       = hook_widget_pressed,
    .screenshot_png       = hook_screenshot_png,
    .canvas_debug         = hook_canvas_debug,
    .canvas_paint_counters = hook_canvas_paint_counters,
};

static int test_server_started = 0;
void aether_ui_enable_test_server_impl(int port, int root_handle) {
    // Idempotent — the env auto-start and an explicit app call must not
    // stack a second banner (seen as duplicate "Under Remote Control").
    if (test_server_started) return;
    test_server_started = 1;
    // Create a "Under Remote Control" banner, style it red+bold, seal it,
    // and hoist it to the top of the root stack.
    int banner = aether_ui_text_create("Under Remote Control");
    Widget* bw = widget_at(banner);
    if (bw) {
        HFONT base = (HFONT)SendMessageW(bw->hwnd, WM_GETFONT, 0, 0);
        LOGFONTW lf;
        GetObjectW(base, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        HFONT bold = CreateFontIndirectW(&lf);
        SendMessageW(bw->hwnd, WM_SETFONT, (WPARAM)bold, TRUE);
        bw->custom_font = bold;
        bw->fg.has_value = 1;
        bw->fg.color = RGB(200, 0, 0);
    }
    Widget* root = widget_at(root_handle);
    if (root && bw && root->kind == WK_VSTACK) {
        SetParent(bw->hwnd, root->hwnd);
        LONG_PTR st = GetWindowLongPtrW(bw->hwnd, GWL_STYLE);
        SetWindowLongPtrW(bw->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
        SetWindowPos(bw->hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        stack_do_layout(root->hwnd);
    }
    aether_ui_test_server_set_banner(banner);
    aether_ui_seal_widget_impl(banner); // banner is not automatable

    // Hidden window that receives AE_WM_DRIVER on the UI thread.
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = driver_host_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AetherUIDriverHost";
    RegisterClassExW(&wc);
    driver_host_hwnd = CreateWindowExW(0, L"AetherUIDriverHost", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);

    aether_ui_test_server_start(port, &win32_driver_hooks);
}

// ---------------------------------------------------------------------------
// System tray (Group 7) — registry-only stub.
//
// Real implementation should use Shell_NotifyIcon(NIM_ADD,
// NIF_ICON | NIF_TIP | NIF_MESSAGE) with a hidden message-only window
// that handles WM_LBUTTONUP / WM_RBUTTONUP via the callback message
// id. Right-click should TrackPopupMenu against the menu_handle's
// HMENU; left-click should dispatch through
// aether_ui_tray_emit_click(id). Icon-template flag is a no-op on
// Win32 (icons are full-colour .ico resources).
//
// Cannot be authored here without a Windows host to test against;
// the registry + driver routes still validate the callback wiring.
// ---------------------------------------------------------------------------
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) {
    return aether_ui_tray_register(name, boxed_left_click);
}
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) {
    aether_ui_tray_set_tooltip_reg(tray_id, text);
}
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) {
    aether_ui_tray_set_menu_reg(tray_id, menu_handle);
}
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle,
                                             const char* icon_clean,
                                             const char* icon_busy,
                                             const char* icon_alert) {
    aether_ui_tray_set_icon_for_state_reg(tray_id, state_handle,
                                          icon_clean, icon_busy, icon_alert);
}
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) {
    aether_ui_tray_set_icon_template_reg(tray_id, is_template);
}
void aether_ui_tray_seal_impl(int tray_id) {
    aether_ui_tray_seal_reg(tray_id);
}

// ---------------------------------------------------------------------------
// Desktop notifications (Group 7b) — registry-only stub.
//
// Real implementation should use Windows.UI.Notifications.
// ToastNotificationManager via the WinRT C ABI: build Toast XML, set
// an AUMID via SetCurrentProcessExplicitAppUserModelID, then
// CreateToastNotificationManagerForApplication(aumid).ShowToast(xml).
// Activation handler routes back through
// aether_ui_notif_emit_click(id) via the IToastActivatedEventArgs.
// ---------------------------------------------------------------------------
int aether_ui_notify_impl(const char* title, const char* body) {
    return aether_ui_notify_register(title, body);
}
int aether_ui_notify_full_impl(const char* title, const char* body,
                                const char* icon_path, const char* tag,
                                void* boxed_click) {
    return aether_ui_notify_register_full(title, body, icon_path, tag, boxed_click);
}
int aether_ui_notify_request_permission_impl(void) {
    return aether_ui_notify_request_permission();
}

// app_run_headless on Win32: park until killed. A future native
// Shell_NotifyIcon implementation should run a GetMessage loop
// here so the message-only window receives the tray callback
// messages from the OS.
void aether_ui_app_run_headless_impl(void) {
    aether_ui_park_until_killed();
}

// Stage 3 per-frame surfaces: DELIBERATELY NOT IMPLEMENTED on win32.
// Returning -1/0 gates ui.frames into drawing every frame inline, which is
// today's correct behaviour. macOS was ported (Stage 2.5b); this one cannot
// be, yet, and the reason is worth stating so nobody "finishes the job".
//
// GDI HAS NO ALPHA, and this file's own code proves it twice:
//
//   * canvas_replay_to_dc opens by filling the ENTIRE surface white
//     (FillRect(mem, &full, white)). A frame-sized capture would therefore
//     come back with an opaque white background, and blitting it would paint
//     a white rectangle over whatever sits beneath the frame.
//   * the CV_IMAGE path composites incoming RGBA against white and stores
//     a=255 (conv[px*4+3] = 255), so alpha does not survive a round trip
//     even for images we hand it.
//   * canvas_read_pixel_impl returns (255 << 24) unconditionally.
//
// frames_demo's frames are 0.42-alpha translucent, so a win32 cache would be
// visibly wrong -- and wrong in the way that looks fine in counters while
// the screen is broken, which is precisely the failure mode Stage 3 already
// hit once. A real port needs an alpha-capable backend here (GDI+, Direct2D,
// or a DIB section the replay composites into by hand), not a wrapper around
// canvas_replay_to_dc.
void aether_ui_canvas_draw_image_impl_ptr(int canvas_id, double x, double y,
                                          int iw, int ih,
                                          const unsigned char* rgba, int byte_len) {
    aether_ui_canvas_draw_image_impl(canvas_id, x, y, iw, ih, rgba, byte_len);
}

/* ptr-typed twin of the scaled blit, mirroring draw_image_impl_ptr: the
   Aether type system distinguishes string vs ptr, C does not. */
void aether_ui_canvas_draw_image_scaled_impl_ptr(int canvas_id, double x, double y,
                                                 double dw, double dh, int iw, int ih,
                                                 const unsigned char* rgba, int byte_len) {
    aether_ui_canvas_draw_image_scaled_impl(canvas_id, x, y, dw, dh, iw, ih, rgba, byte_len);
}

// No retained paint surface on this backend yet (Stage 2.5b), so there is
// nothing to sample. -1 = "cannot answer", distinct from 0 = "painted
// nothing".
int aether_ui_canvas_painted_pixels_impl(int canvas_id) {
    (void)canvas_id;
    return -1;
}

int aether_ui_canvas_cmd_count_impl(int canvas_id) {
    (void)canvas_id;
    return -1;
}

int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end,
                                            double ox, double oy,
                                            int width, int height,
                                            unsigned char* out, int out_len) {
    (void)canvas_id; (void)start; (void)end; (void)ox; (void)oy;
    (void)width; (void)height; (void)out; (void)out_len;
    return 0;
}
