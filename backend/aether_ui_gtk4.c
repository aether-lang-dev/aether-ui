// Aether UI — GTK4 backend for Aether
// Port of aether-ui-gtk4 (Rust) to C, calling the GTK4 C API directly.
//
// This file is compiled separately and linked with Aether programs via:
//   ae build app.ae --extra aether_ui_gtk4.c $(pkg-config --cflags --libs gtk4)

#include "aether_ui_backend.h"
#include "aether_ui_system_extras.h"
#include "aether_ui_sni.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef AEUI_HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

// ---------------------------------------------------------------------------
// Closure struct — must match Aether codegen's _AeClosure layout.
// box_closure() returns a malloc'd copy of this struct.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

// ---------------------------------------------------------------------------
// GTK4 initialization — must be called before creating any widgets.
// ---------------------------------------------------------------------------
static int gtk_initialized = 0;

static void ensure_gtk_init(void) {
    if (!gtk_initialized) {
        gtk_init();
        gtk_initialized = 1;
    }
}

// ---------------------------------------------------------------------------
// AETHER_UI_HEADLESS contract — set by CI, widget smoke tests, or any
// caller that wants to exercise the backend without a user. Every API
// that would otherwise show a modal dialog (alert, file_open, …)
// returns without UI when this flag is set. Without this, modal
// dialogs can block waiting for interaction that never comes.
// ---------------------------------------------------------------------------
static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

// ---------------------------------------------------------------------------
// Widget registry — flat array of GtkWidget*, 1-based handles.
//
// Slots self-NULL when their widget is finalized (weak ref below), so a
// handle held past window teardown resolves to NULL instead of a dangling
// pointer. Every consumer of aether_ui_get_widget() already NULL-checks;
// without this, the vg live-refresh timer's canvas_redraw between window
// destroy and main-loop exit called gtk_widget_queue_draw on freed memory
// (a Gtk-CRITICAL spray on exit, and a latent use-after-free).
// ---------------------------------------------------------------------------
static GtkWidget** widgets = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

static void on_widget_finalized(gpointer data, GObject* where_the_object_was) {
    (void)where_the_object_was;
    int idx = GPOINTER_TO_INT(data);          // index, not pointer: realloc-safe
    if (idx >= 0 && idx < widget_count) widgets[idx] = NULL;
}

int aether_ui_register_widget(void* widget) {
    if (widget_count >= widget_capacity) {
        widget_capacity = widget_capacity == 0 ? 64 : widget_capacity * 2;
        widgets = realloc(widgets, sizeof(GtkWidget*) * widget_capacity);
    }
    widgets[widget_count] = (GtkWidget*)widget;
    g_object_weak_ref(G_OBJECT(widget), on_widget_finalized,
                      GINT_TO_POINTER(widget_count));
    widget_count++;
    return widget_count; // 1-based
}

void* aether_ui_get_widget(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return widgets[handle - 1];
}

// ---------------------------------------------------------------------------
// Typed reactive state — ONE handle space of tagged cells (see
// docs/design/reactivity-unification.md). The original doubles-only
// API is the type-0 facet; string/int/bool are additive. Bindings are
// data-carrying links (kind + fmt fields, deliberately NO closures),
// walked synchronously on every set — PUSH reactivity, GTK-thread only.
// ---------------------------------------------------------------------------

enum { AEUI_STATE_FLOAT = 0, AEUI_STATE_INT = 1,
       AEUI_STATE_BOOL = 2, AEUI_STATE_STRING = 3,
       AEUI_STATE_LIST = 4 };  // opaque list ptr + revision (each_bind)
typedef struct {
    int type;
    double num;   // float/int/bool payload
    char* str;    // string payload (owned)
    void* list;   // LIST payload (opaque std.list ptr, NOT owned)
    int rev;      // LIST: bumps on each set, so the driver sees a change
} StateCell;

// Generic state observers — a boxed Aether closure fired (no args) whenever a
// state cell is set. The unifying primitive behind each_bind and computed
// state: the ui layer registers a closure that re-runs each_update / recompute.
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
        // state → editable widget. Only set if different, so the widget's own
        // "changed" write-back (which set the state that got us here) doesn't
        // re-enter forever.
        const char* cur = aether_ui_textfield_get_text(b->widget_handle);
        const char* want = (c->type == AEUI_STATE_STRING && c->str) ? c->str : "";
        if (!cur || strcmp(cur, want) != 0) {
            aether_ui_textfield_set_text(b->widget_handle, want);
        }
    } else if (b->kind == AEUI_BIND_TEXT) {
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
    // Snapshot the count: an observer's closure may register more (e.g. a
    // computed state that itself has observers) — those fire on their own set.
    int n = state_observer_count;
    for (int i = 0; i < n; i++) {
        if (state_observers[i].state_handle == state_handle) {
            AeClosure* c = state_observers[i].closure;
            if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
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

// Register a boxed closure fired whenever `state_handle` is set. Powers
// each_bind (re-run each_update) and computed state (recompute).
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

// LIST state: an opaque std.list ptr + a revision that bumps on each set.
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
    update_prop_bindings(handle);   // fires observers → each_bind re-runs
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

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
typedef struct {
    char* title;
    int width;
    int height;
    int root_handle;
    GtkApplication* gtk_app;
} AppEntry;

static AppEntry* apps = NULL;
static int app_count = 0;
static int app_capacity = 0;

// The primary application window (set in on_activate). The overlay layer
// resolves handle 0 to this — the driver's common "main window" case. It is
// also window handle 1 in the multi-window registry (extra_windows below).
static GtkWindow* primary_window = NULL;

// ---------------------------------------------------------------------------
// Surface table — "DSL with Scope" surfaces (window / render_to / record).
//
// A surface is the ambient destination a drawing/widget block populates.
// Children attach to the surface's content container (the handle pushed as
// _ctx); the surface row carries the rest: kind, the owning app handle (for
// `window`, so its builder body can run the loop after the block), and a
// collected diagnostic count (interactive verbs used on a bounded surface).
//
// Keyed by the content-container handle so a child verb's _ctx maps back to
// the surface. Single-window-at-a-time is fine — surfaces don't nest.
// ---------------------------------------------------------------------------
#define AUI_SURFACE_WINDOW 0
#define AUI_SURFACE_RENDER 1   // render_to (bounded; flush to target)
#define AUI_SURFACE_RECORD 2   // record    (bounded; capture for tests)

typedef struct {
    int container_handle;  // the _ctx children attach to (key)
    int kind;              // AUI_SURFACE_*
    int app_handle;        // window: owning app (0 for bounded surfaces)
    int diag_count;        // # interactive-verb-on-bounded-surface diagnostics
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

// ---------------------------------------------------------------------------
// Deferred-flush registry — the AeVG `vg { … }` block records its shapes
// without drawing (so a shape's trailing fill()/stroke() is known before the
// backend call), then registers a boxed Aether closure that flushes the
// recorded scene. The enclosing surface body (window / render_to / record)
// drains this registry after its block closes, BEFORE the window's event loop
// blocks — so the scene's command buffer is built with final colours. This
// keeps the layering clean: aether_ui owns the registry of opaque closures and
// invokes them; only the vg layer knows what a scene is.
// ---------------------------------------------------------------------------
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

// Invoke every registered flush closure, then clear the registry (each scene
// flushes exactly once per surface). Called by the surface bodies.
void aether_ui_surface_flush_deferred_impl(void) {
    for (int i = 0; i < deferred_flush_count; i++) {
        AeClosure* c = deferred_flushes[i];
        if (c && c->fn) {
            ((void(*)(void*))c->fn)(c->env);
        }
    }
    deferred_flush_count = 0;
}

// Create a surface container of the given kind: a detached root vstack the
// block's children attach to (pushed as _ctx). No app/loop is created here —
// the zero-arg `with` factory runs BEFORE the block and doesn't know the
// window title/size. The `window` builder BODY (which has title/w/h and the
// now-populated container) creates the app + runs the loop afterward, via
// aether_ui_surface_run_impl. This matches the builder "configure then
// execute" lifecycle: children fill the container, then the body acts.
int aether_ui_surface_container_new_impl(int kind) {
    int container = aether_ui_vstack_create(0);
    surface_add(container, kind, 0);
    return container;
}

// window builder body: create the app, mount the populated container, run
// the loop. Called after the block. Only meaningful for a WINDOW surface.
void aether_ui_surface_run_impl(int container_handle,
                                const char* title, int width, int height) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s || s->kind != AUI_SURFACE_WINDOW) return;
    // Flush any deferred AeVG scenes recorded during the block, so their
    // command buffers are built with final colours before the loop starts.
    aether_ui_surface_flush_deferred_impl();
    int app = aether_ui_app_create(title, width, height);
    s->app_handle = app;
    aether_ui_app_set_body(app, container_handle);
    aether_ui_app_run_raw(app);
}

// Record an interactive-verb-on-bounded-surface diagnostic; returns 1 if the
// container is a bounded surface (so the caller knows the verb is inert), 0
// if it's a live window (verb is fine) or unknown.
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

static void aeui_attach_pending_shortcuts(void);
static void aeui_attach_pending_menus(GtkApplication* app, GtkWindow* window);
static void aeui_register_primary_window(GtkWindow* w, const char* title);

static void on_activate(GtkApplication* gtk_app, gpointer user_data) {
    AppEntry* entry = (AppEntry*)user_data;
    GtkWidget* window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), entry->title);
    gtk_window_set_default_size(GTK_WINDOW(window), entry->width, entry->height);
    primary_window = GTK_WINDOW(window);   // overlay layer resolves handle 0 here
    aeui_register_primary_window(GTK_WINDOW(window), entry->title);  // = handle 1
    aeui_attach_pending_shortcuts();       // item 9: queued ui.shortcut regs
    aeui_attach_pending_menus(gtk_app, GTK_WINDOW(window));  // real menu bar

    if (entry->root_handle > 0) {
        GtkWidget* root = aether_ui_get_widget(entry->root_handle);
        if (root) {
            gtk_window_set_child(GTK_WINDOW(window), root);
        }
    }

    // Honor AETHER_UI_HEADLESS for CI and unattended scenarios. The window
    // is realized, the event loop still pumps, and the test server still
    // responds — but the window is never presented. Matches the Win32
    // backend's SW_HIDE semantics.
    const char* headless = getenv("AETHER_UI_HEADLESS");
    int is_headless = headless && headless[0] && headless[0] != '0';
    if (is_headless) {
        gtk_widget_realize(window);
    } else {
        gtk_window_present(GTK_WINDOW(window));
    }
}

int aether_ui_app_create(const char* title, int width, int height) {
    if (app_count >= app_capacity) {
        app_capacity = app_capacity == 0 ? 4 : app_capacity * 2;
        apps = realloc(apps, sizeof(AppEntry) * app_capacity);
    }
    AppEntry* e = &apps[app_count];
    e->title = strdup(title);
    e->width = width;
    e->height = height;
    e->root_handle = 0;
    e->gtk_app = NULL;
    app_count++;
    return app_count; // 1-based
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    apps[app_handle - 1].root_handle = root_handle;
}

/* #93: the running GApplication, so app_quit can stop it from anywhere. */
static GApplication* aeui_running_app = NULL;

/* Runs on the main context: g_application_quit must not be called from an
   arbitrary thread. */
static gboolean aeui_quit_idle(gpointer data) {
    (void)data;
    if (aeui_running_app) g_application_quit(aeui_running_app);
    return G_SOURCE_REMOVE;
}

void aether_ui_app_quit_impl(void) {
    aether_ui_request_quit();
    if (aeui_running_app) g_idle_add(aeui_quit_idle, NULL);
}

void aether_ui_app_run_raw(int app_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    AppEntry* e = &apps[app_handle - 1];
    e->gtk_app = gtk_application_new("dev.aether.ui",
        G_APPLICATION_DEFAULT_FLAGS | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(e->gtk_app, "activate", G_CALLBACK(on_activate), e);
    aeui_running_app = G_APPLICATION(e->gtk_app);
    g_application_run(G_APPLICATION(e->gtk_app), 0, NULL);
    aeui_running_app = NULL;
    g_object_unref(e->gtk_app);
}


// ---------------------------------------------------------------------------
// Widget creation
// ---------------------------------------------------------------------------

int aether_ui_text_create(const char* text) {
    ensure_gtk_init();
    GtkWidget* label = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return aether_ui_register_widget(label);
}

// A multi-line label that wraps at `wrap_width_px` (word boundaries).
int aether_ui_text_wrapped_create(const char* text, int wrap_width_px) {
    ensure_gtk_init();
    GtkWidget* label = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    if (wrap_width_px > 0) {
        // Request the wrap width; the label grows in height to fit.
        gtk_widget_set_size_request(label, wrap_width_px, -1);
        gtk_label_set_max_width_chars(GTK_LABEL(label), -1);
    }
    return aether_ui_register_widget(label);
}

// Text anchor: 0=start(left) 1=middle(center) 2=end(right). Sets label xalign.
void aether_ui_text_set_anchor(int handle, int anchor) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return;
    float x = anchor == 1 ? 0.5f : anchor == 2 ? 1.0f : 0.0f;
    gtk_label_set_xalign(GTK_LABEL(w), x);
}

// Driver introspection for text widgets.
int aether_ui_text_get_wrap(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    return (w && GTK_IS_LABEL(w) && gtk_label_get_wrap(GTK_LABEL(w))) ? 1 : 0;
}

void aether_ui_text_set_truncate(int handle, int mode) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return;
    PangoEllipsizeMode em = PANGO_ELLIPSIZE_NONE;
    if (mode == 1) em = PANGO_ELLIPSIZE_START;
    else if (mode == 2) em = PANGO_ELLIPSIZE_MIDDLE;
    else if (mode == 3) em = PANGO_ELLIPSIZE_END;
    gtk_label_set_ellipsize(GTK_LABEL(w), em);
}

int aether_ui_text_get_truncate(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return 0;
    switch (gtk_label_get_ellipsize(GTK_LABEL(w))) {
        case PANGO_ELLIPSIZE_START:  return 1;
        case PANGO_ELLIPSIZE_MIDDLE: return 2;
        case PANGO_ELLIPSIZE_END:    return 3;
        default:                     return 0;
    }
}
int aether_ui_text_get_anchor(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return 0;
    float x = gtk_label_get_xalign(GTK_LABEL(w));
    return x > 0.75f ? 2 : x > 0.25f ? 1 : 0;
}

void aether_ui_text_set_string(int handle, const char* text) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_LABEL(w)) {
        gtk_label_set_text(GTK_LABEL(w), text ? text : "");
    }
}

void aether_ui_button_set_label(int handle, const char* label) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_BUTTON(w)) {
        gtk_button_set_label(GTK_BUTTON(w), label ? label : "");
    }
}

// ---------------------------------------------------------------------------
// Right-click context menus.
//
// Two surfaces, chosen at runtime:
//
//   * DEFAULT: a GtkPopover (GtkListBox of rows) parented to the owner. This
//     is the correct native menu on every mainstream Linux desktop — it
//     positions at the pointer, carries the theme's menu styling, and
//     auto-dismisses. Verified working on GNOME/KDE (Wayland) and X11.
//
//   * SOMMELIER FALLBACK: a borderless top-level window. On ChromeOS
//     Crostini's sommelier compositor, GTK4 popovers (xdg_popup subsurfaces)
//     report mapped=1 but never DISPLAY — verified a stock GtkMenuButton /
//     GtkDropDown is equally invisible there. A top-level window maps fine.
//     The trade-off (no pointer-anchored placement, plainer chrome) is
//     accepted only where the popover doesn't work at all. Detected via
//     $SOMMELIER_VERSION (set by sommelier, absent elsewhere).
//     KNOWN WART: sommelier sometimes doesn't present this transient
//     top-level on the first right-click (menu appears on the second tap, or
//     the window can land behind the app) — a sommelier window-management
//     quirk, not a logic bug (each open logs mapped=1). Left as-is; the menu
//     is reachable. A create-once hide/show window may help if it recurs.
//
// The trigger is a GtkEventControllerLegacy filtering for button 3, NOT a
// GtkGestureClick: on sommelier a two-finger tap delivers button-3
// press/release that the click gesture silently drops (verified). The legacy
// controller sees the raw button events on every backend, so it is used
// unconditionally.
//
// Each context_menu_item call records {label, closure}; the surface is built
// on first right-click. Item activation fires the closure and closes.
// ---------------------------------------------------------------------------
typedef struct {
    GtkWidget* owner;
    GtkWidget* popover;             // popover surface (default path)
    GtkWidget* menu_win;            // window surface (sommelier fallback)
    GPtrArray* labels;              // char* (owned copies)
    GPtrArray* closures;            // AeClosure* (boxed, owned by Aether side)
} CtxMenu;

static int aeui_ctx_debug(void) {
    const char* v = getenv("AETHER_UI_DEBUG");
    return v && v[0] && v[0] != '0';
}

// True when GTK4 popovers don't display and we must fall back to a window.
// sommelier (ChromeOS Crostini) is the only known case; it exports
// $SOMMELIER_VERSION. $AETHER_UI_MENU=window forces the fallback for testing.
static int aeui_ctx_use_window(void) {
    const char* force = getenv("AETHER_UI_MENU");
    if (force && strcmp(force, "window") == 0) return 1;
    if (force && strcmp(force, "popover") == 0) return 0;
    return getenv("SOMMELIER_VERSION") != NULL;
}

static void ctx_menu_close(CtxMenu* cm) {
    if (cm->popover) { gtk_popover_popdown(GTK_POPOVER(cm->popover)); }
    if (cm->menu_win) {
        gtk_window_destroy(GTK_WINDOW(cm->menu_win));
        cm->menu_win = NULL;
    }
}

// A menu item fired (popover row or window button) — close, then run closure.
static void ctx_menu_fire(CtxMenu* cm, AeClosure* c) {
    ctx_menu_close(cm);
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
}

static void on_ctx_menu_row_activated(GtkListBox* lb, GtkListBoxRow* row,
                                      gpointer data) {
    (void)lb;
    ctx_menu_fire((CtxMenu*)data, g_object_get_data(G_OBJECT(row), "aeui-closure"));
}

static void on_ctx_menu_item_clicked(GtkButton* btn, gpointer data) {
    ctx_menu_fire((CtxMenu*)data, g_object_get_data(G_OBJECT(btn), "aeui-closure"));
}

static gboolean on_ctx_menu_key(GtkEventControllerKey* k, guint keyval,
                                guint code, GdkModifierType st, gpointer data) {
    (void)k; (void)code; (void)st;
    if (keyval == GDK_KEY_Escape) { ctx_menu_close((CtxMenu*)data); return TRUE; }
    return FALSE;
}

// Window-fallback: losing focus (click elsewhere) closes it.
static void on_ctx_menu_win_active(GObject* win, GParamSpec* ps, gpointer data) {
    (void)ps;
    if (!gtk_window_is_active(GTK_WINDOW(win))) ctx_menu_close((CtxMenu*)data);
}

// --- Default surface: a GtkPopover anchored at (x, y) in owner-local px. ---
static void ctx_menu_open_popover(CtxMenu* cm, double x, double y) {
    ctx_menu_close(cm);
    if (!cm->popover) {
        GtkWidget* list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        g_signal_connect(list, "row-activated",
                         G_CALLBACK(on_ctx_menu_row_activated), cm);
        for (guint i = 0; i < cm->labels->len; i++) {
            GtkWidget* lbl =
                gtk_label_new((const char*)g_ptr_array_index(cm->labels, i));
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_widget_set_margin_start(lbl, 8);  gtk_widget_set_margin_end(lbl, 8);
            gtk_widget_set_margin_top(lbl, 4);    gtk_widget_set_margin_bottom(lbl, 4);
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
            g_object_set_data(G_OBJECT(row), "aeui-closure",
                              g_ptr_array_index(cm->closures, i));
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
        cm->popover = gtk_popover_new();
        gtk_popover_set_has_arrow(GTK_POPOVER(cm->popover), FALSE);
        gtk_popover_set_child(GTK_POPOVER(cm->popover), list);
        gtk_widget_set_parent(cm->popover, cm->owner);
    }
    GdkRectangle at = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(cm->popover), &at);
    gtk_popover_popup(GTK_POPOVER(cm->popover));
    if (aeui_ctx_debug()) {
        fprintf(stderr, "[aeui-ctxmenu] popover at %.0f,%.0f mapped=%d\n",
                x, y, gtk_widget_get_mapped(cm->popover));
    }
}

// --- Fallback surface: a borderless top-level window (sommelier). ---
static void ctx_menu_open_window(CtxMenu* cm) {
    ctx_menu_close(cm);
    GtkWidget* win = gtk_window_new();
    cm->menu_win = win;
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    GtkRoot* owner_root = gtk_widget_get_root(cm->owner);
    if (GTK_IS_WINDOW(owner_root))
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(owner_root));
    gtk_widget_add_css_class(win, "aui-context-menu");

    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (guint i = 0; i < cm->labels->len; i++) {
        GtkWidget* item =
            gtk_button_new_with_label((const char*)g_ptr_array_index(cm->labels, i));
        gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
        GtkWidget* item_label = gtk_button_get_child(GTK_BUTTON(item));
        if (item_label && GTK_IS_LABEL(item_label))
            gtk_label_set_xalign(GTK_LABEL(item_label), 0.0);
        g_object_set_data(G_OBJECT(item), "aeui-closure",
                          g_ptr_array_index(cm->closures, i));
        g_signal_connect(item, "clicked",
                         G_CALLBACK(on_ctx_menu_item_clicked), cm);
        gtk_box_append(GTK_BOX(list), item);
    }
    gtk_window_set_child(GTK_WINDOW(win), list);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_ctx_menu_key), cm);
    gtk_widget_add_controller(win, key);
    g_signal_connect(win, "notify::is-active",
                     G_CALLBACK(on_ctx_menu_win_active), cm);
    gtk_window_present(GTK_WINDOW(win));
    if (aeui_ctx_debug())
        fprintf(stderr, "[aeui-ctxmenu] menu window mapped=%d\n",
                gtk_widget_get_mapped(win));
}

// Open the menu at (x, y) owner-local. Routes to the popover (default) or the
// window fallback (sommelier).
static void ctx_menu_open_local(CtxMenu* cm, double x, double y) {
    if (aeui_ctx_use_window()) ctx_menu_open_window(cm);
    else ctx_menu_open_popover(cm, x, y);
}

// Legacy controller: fire on button-3 RELEASE. A GtkGestureClick set to
// button 3 silently drops the two-finger-tap button-3 events on sommelier;
// the legacy controller sees them on every backend. Release (not press) is
// the context-menu convention.
//
// MUST return gboolean: the legacy "event" signal treats a truthy return as
// "handled — stop propagating". This handler was originally declared void,
// so it returned register garbage (usually the nonzero result of the last
// call) and silently ATE every button event for any controller attached
// after it — on the treemap canvas that killed all left-clicks the moment
// the canvas grew a context menu. Return TRUE only when the menu opens.
static gboolean on_ctx_menu_legacy_event(GtkEventControllerLegacy* c,
                                         GdkEvent* ev, gpointer data) {
    (void)c;
    CtxMenu* cm = (CtxMenu*)data;
    GdkEventType t = gdk_event_get_event_type(ev);
    if ((t == GDK_BUTTON_PRESS || t == GDK_BUTTON_RELEASE) && aeui_ctx_debug()) {
        fprintf(stderr, "[aeui-ctxmenu] RAW %s button=%u\n",
                t == GDK_BUTTON_PRESS ? "press" : "release",
                gdk_button_event_get_button(ev));
    }
    if (t == GDK_BUTTON_RELEASE && gdk_button_event_get_button(ev) == 3) {
        double x = 0, y = 0;
        gdk_event_get_position(ev, &x, &y);
        ctx_menu_open_local(cm, x, y);
        return TRUE;
    }
    return FALSE;
}

// Whichever surface is in use, is it currently mapped?
static int ctx_menu_is_mapped(CtxMenu* cm) {
    if (cm->menu_win) return gtk_widget_get_mapped(cm->menu_win) ? 1 : 0;
    if (cm->popover)  return gtk_widget_get_mapped(cm->popover)  ? 1 : 0;
    return 0;
}

// AetherUIDriver hooks — drive and OBSERVE the context menu from the HTTP
// test server (run on the GTK thread via idle callback).
//   aeui_ctx_menu_present:  -1 no menu attached, else item count
//   aeui_ctx_menu_open:     open at the widget's centre; 1 if it mapped
//   aeui_ctx_menu_mapped:   1 if the menu surface is currently mapped
//   aeui_ctx_menu_activate: fire item[idx]'s closure (as an item click does)
int aeui_ctx_menu_present(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return -1;
    return (int)cm->labels->len;
}

int aeui_ctx_menu_open(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return -1;
    ctx_menu_open_local(cm, gtk_widget_get_width(w) / 2.0,
                            gtk_widget_get_height(w) / 2.0);
    return ctx_menu_is_mapped(cm);
}

int aeui_ctx_menu_mapped(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return 0;
    return ctx_menu_is_mapped(cm);
}

int aeui_ctx_menu_activate(int handle, int idx) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return -1;
    if (idx < 0 || idx >= (int)cm->closures->len) return -2;
    ctx_menu_close(cm);
    AeClosure* c = (AeClosure*)g_ptr_array_index(cm->closures, idx);
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
    /* 1 = activated. The shared server tests `!act.retval` and 404s on false
       (win32 sets retval=1 here for exactly that reason), so returning the
       C-idiomatic 0-for-success reported EVERY successful activation as
       "no such context menu item" -- the whole of context_menu's 1/3 RED. */
    return 1;
}

static void on_ctx_menu_owner_destroy(GtkWidget* owner, gpointer data) {
    (void)owner;
    CtxMenu* cm = (CtxMenu*)data;
    ctx_menu_close(cm);
    // The popover is parented to the owner; unparent it before the owner is
    // disposed or GTK warns about a live child.
    if (cm->popover) { gtk_widget_unparent(cm->popover); cm->popover = NULL; }
}

void aether_ui_context_menu_item_impl(int handle, const char* label,
                                      void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) {
        cm = g_new0(CtxMenu, 1);
        cm->owner = w;
        cm->labels = g_ptr_array_new_with_free_func(g_free);
        cm->closures = g_ptr_array_new();
        GtkEventController* legacy = gtk_event_controller_legacy_new();
        g_signal_connect(legacy, "event",
                         G_CALLBACK(on_ctx_menu_legacy_event), cm);
        gtk_widget_add_controller(w, legacy);
        g_signal_connect(w, "destroy", G_CALLBACK(on_ctx_menu_owner_destroy), cm);
        g_object_set_data(G_OBJECT(w), "aeui-ctxmenu", cm);
    }
    g_ptr_array_add(cm->labels, g_strdup(label ? label : ""));
    g_ptr_array_add(cm->closures, boxed_closure);
}

// Accelerator-labelled variant: the item DISPLAYS its combo ("Rescan
// Ctrl+R"). Display only — wiring the combo is the app author also
// calling ui.shortcut (auto-binding is a recorded follow-up).
void aether_ui_context_menu_item_accel_impl(int handle, const char* label,
                                            const char* accel,
                                            void* boxed_closure) {
    char joined[256];
    if (accel && accel[0]) {
        snprintf(joined, sizeof(joined), "%s    %s",
                 label ? label : "", accel);
    } else {
        snprintf(joined, sizeof(joined), "%s", label ? label : "");
    }
    aether_ui_context_menu_item_impl(handle, joined, boxed_closure);
}

static void on_button_clicked(GtkButton* btn, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
}

// Plain button — no callback. Use aether_ui_set_onclick to add one later.
int aether_ui_button_create_plain(const char* label) {
    ensure_gtk_init();
    GtkWidget* btn = gtk_button_new_with_label(label ? label : "");
    return aether_ui_register_widget(btn);
}

// Set click handler on an existing button (or any widget via GestureClick).
void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    if (GTK_IS_BUTTON(w)) {
        g_signal_connect(w, "clicked", G_CALLBACK(on_button_clicked), boxed_closure);
    }
}

// Styling functions that accept _ctx (void*) instead of int handle.
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aether_ui_set_bg_color((int)(intptr_t)ctx, r, g, b, a);
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

int aether_ui_button_create(const char* label, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* btn = gtk_button_new_with_label(label ? label : "");
    if (boxed_closure) {
        g_signal_connect(btn, "clicked", G_CALLBACK(on_button_clicked), boxed_closure);
    }
    return aether_ui_register_widget(btn);
}

int aether_ui_vstack_create(int spacing) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    return aether_ui_register_widget(box);
}

int aether_ui_hstack_create(int spacing) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    return aether_ui_register_widget(box);
}

// Lay a stack's children right-to-left (RTL). GtkBox honours the widget's text
// direction, so the children reverse natively (first child on the right). Only
// meaningful on an hstack; harmless on a vstack.
void aether_ui_set_rtl(int handle, int rtl) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    gtk_widget_set_direction(w, rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    // A flex-managed stack (weight/on_layout) lays out itself; flag it so the
    // custom layout can reverse too.
    g_object_set_data(G_OBJECT(w), "aeui-rtl", GINT_TO_POINTER(rtl ? 1 : 0));
}

int aether_ui_spacer_create(void) {
    ensure_gtk_init();
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_widget_set_hexpand(spacer, TRUE);
    return aether_ui_register_widget(spacer);
}

int aether_ui_divider_create(void) {
    ensure_gtk_init();
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    return aether_ui_register_widget(sep);
}

// ---------------------------------------------------------------------------
// AeuiFlexLayout — a custom GtkLayoutManager for stacks (GtkBoxLayout is
// final, so it can't be subclassed). Two jobs:
//
//  1. WEIGHTS (Flutter Expanded semantics): children carrying "aeui-weight"
//     qdata > 0 split the leftover main-axis space proportionally after
//     unweighted children take their natural size. Children that expand
//     along the axis (hexpand in an hstack / vexpand in a vstack) but have
//     no explicit weight count as weight 1 — that's GtkBox's own extra-
//     space rule, so spacer() keeps working inside a flex-managed stack.
//
//  2. ON_LAYOUT: allocate() is the only event-driven place GTK4 still
//     reports size changes for arbitrary containers (the size-allocate
//     signal is gone; tick callbacks keep the frame clock running, which
//     violates idle-must-cost-zero). Widgets must NOT be mutated during
//     allocation, so the hook fires from a g_idle_add.
//
// Installed lazily — only on stacks where ui.weight/ui.on_layout is used —
// so every other GtkBox keeps stock GtkBoxLayout behaviour. Spacing is
// copied from the box at install time. Known v1 simplifications: weighted
// shares aren't clamped to the child minimum, baselines are ignored.
// ---------------------------------------------------------------------------

typedef struct {
    GtkLayoutManager parent_instance;
    GtkOrientation orient;
    int spacing;
} AeuiFlexLayout;
typedef struct { GtkLayoutManagerClass parent_class; } AeuiFlexLayoutClass;

G_DEFINE_TYPE(AeuiFlexLayout, aeui_flex_layout, GTK_TYPE_LAYOUT_MANAGER)

static int aeui_child_weight(GtkWidget* c, GtkOrientation orient) {
    int w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "aeui-weight"));
    if (w > 0) return w;
    gboolean expands = (orient == GTK_ORIENTATION_HORIZONTAL)
        ? gtk_widget_get_hexpand(c) : gtk_widget_get_vexpand(c);
    return expands ? 1 : 0;
}

// The request-mode + for_size plumbing below is NOT optional polish:
// GtkWindow computes its default size through the child's request mode,
// and a manager that claims CONSTANT_SIZE makes the window ignore
// set_default_size and snap to natural width (seen as the 187px-wide
// split_demo under ci's xvfb-run).
static GtkSizeRequestMode aeui_flex_layout_get_request_mode(GtkLayoutManager* lm,
                                                            GtkWidget* widget) {
    AeuiFlexLayout* self = (AeuiFlexLayout*)lm;
    (void)widget;
    return self->orient == GTK_ORIENTATION_VERTICAL
        ? GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH
        : GTK_SIZE_REQUEST_WIDTH_FOR_HEIGHT;
}

static void aeui_flex_layout_measure(GtkLayoutManager* lm, GtkWidget* widget,
                                     GtkOrientation orientation, int for_size,
                                     int* minimum, int* natural,
                                     int* minimum_baseline, int* natural_baseline) {
    AeuiFlexLayout* self = (AeuiFlexLayout*)lm;
    // Main axis: children see the cross extent (for_size) — height-for-
    // width for a vstack of wrapping labels. Cross axis: unconstrained.
    int child_for = (orientation == self->orient) ? for_size : -1;
    int min_sum = 0, nat_sum = 0, min_max = 0, nat_max = 0, n = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int cmin = 0, cnat = 0;
        gtk_widget_measure(c, orientation, child_for, &cmin, &cnat, NULL, NULL);
        min_sum += cmin;
        nat_sum += cnat;
        if (cmin > min_max) min_max = cmin;
        if (cnat > nat_max) nat_max = cnat;
        n++;
    }
    if (orientation == self->orient) {
        int sp = n > 1 ? (n - 1) * self->spacing : 0;
        *minimum = min_sum + sp;
        *natural = nat_sum + sp;
    } else {
        *minimum = min_max;
        *natural = nat_max;
    }
    if (minimum_baseline) *minimum_baseline = -1;
    if (natural_baseline) *natural_baseline = -1;
}

// on_layout fire, deferred to idle (no widget mutation inside allocate).
typedef struct { AeClosure* cl; int w; int h; } AeuiLayoutFire;
static gboolean aeui_layout_fire_idle(gpointer data) {
    AeuiLayoutFire* lf = (AeuiLayoutFire*)data;
    if (lf->cl && lf->cl->fn) {
        ((void(*)(void*, intptr_t, intptr_t))lf->cl->fn)(
            lf->cl->env, (intptr_t)lf->w, (intptr_t)lf->h);
    }
    g_free(lf);
    return G_SOURCE_REMOVE;
}

static void aeui_flex_layout_allocate(GtkLayoutManager* lm, GtkWidget* widget,
                                      int width, int height, int baseline) {
    AeuiFlexLayout* self = (AeuiFlexLayout*)lm;
    (void)baseline;
    int axis_total = (self->orient == GTK_ORIENTATION_HORIZONTAL) ? width : height;

    // Pass 1: total weight + natural size of unweighted children (measured
    // against the cross extent, so wrapping labels report honest heights).
    int cross_extent = (self->orient == GTK_ORIENTATION_HORIZONTAL) ? height : width;
    int total_weight = 0, fixed = 0, n = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int wgt = aeui_child_weight(c, self->orient);
        if (wgt > 0) {
            total_weight += wgt;
        } else {
            int cmin = 0, cnat = 0;
            gtk_widget_measure(c, self->orient, cross_extent, &cmin, &cnat, NULL, NULL);
            fixed += cnat;
        }
        n++;
    }
    int leftover = axis_total - fixed - (n > 1 ? (n - 1) * self->spacing : 0);
    if (leftover < 0) leftover = 0;

    // Pass 1b: min-clamp. A weighted child's proportional share can fall below
    // its own minimum when leftover is tight; pinning it there and removing it
    // from the weight pool (then re-splitting among the rest) keeps weighted
    // children from being squeezed under their min. Iterate until stable —
    // pinning one child shrinks the pool and can push another under its min.
    int clamp_leftover = leftover;
    int clamp_weight = total_weight;
    int changed = 1;
    while (changed && clamp_weight > 0) {
        changed = 0;
        for (GtkWidget* c = gtk_widget_get_first_child(widget); c;
             c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c)) continue;
            int wgt = aeui_child_weight(c, self->orient);
            if (wgt <= 0) continue;
            if (g_object_get_data(G_OBJECT(c), "aeui-weight-pinned")) continue;
            int cmin = 0, cnat = 0;
            gtk_widget_measure(c, self->orient, cross_extent, &cmin, &cnat, NULL, NULL);
            int share = clamp_leftover * wgt / clamp_weight;
            if (share < cmin) {
                g_object_set_data(G_OBJECT(c), "aeui-weight-pinned",
                                  GINT_TO_POINTER(cmin));
                clamp_leftover -= cmin;
                if (clamp_leftover < 0) clamp_leftover = 0;
                clamp_weight -= wgt;
                changed = 1;
            }
        }
    }

    // Pass 2: place sequentially; the LAST flexible weighted child absorbs the
    // integer-division remainder so the row always fills exactly. Pinned
    // children take their stored minimum; the rest split clamp_leftover.
    int pos = 0, weighted_seen = 0, weighted_used = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (!gtk_widget_should_layout(c)) continue;
        int wgt = aeui_child_weight(c, self->orient);
        int size;
        gpointer pinned = (wgt > 0)
            ? g_object_get_data(G_OBJECT(c), "aeui-weight-pinned") : NULL;
        if (pinned) {
            size = GPOINTER_TO_INT(pinned);
            g_object_set_data(G_OBJECT(c), "aeui-weight-pinned", NULL);  // reset
        } else if (wgt > 0) {
            weighted_seen += wgt;
            size = (weighted_seen == clamp_weight)
                ? clamp_leftover - weighted_used
                : clamp_leftover * wgt / clamp_weight;
            weighted_used += size;
        } else {
            int cmin = 0, cnat = 0;
            gtk_widget_measure(c, self->orient, cross_extent, &cmin, &cnat, NULL, NULL);
            size = cnat;
        }
        int cw = (self->orient == GTK_ORIENTATION_HORIZONTAL) ? size : width;
        int ch = (self->orient == GTK_ORIENTATION_HORIZONTAL) ? height : size;
        float cx = (self->orient == GTK_ORIENTATION_HORIZONTAL) ? (float)pos : 0.0f;
        float cy = (self->orient == GTK_ORIENTATION_HORIZONTAL) ? 0.0f : (float)pos;
        GskTransform* t = gsk_transform_translate(NULL,
            &GRAPHENE_POINT_INIT(cx, cy));
        gtk_widget_allocate(c, cw, ch, -1, t);
        pos += size + self->spacing;
    }

    // on_layout hook — fire on size CHANGE only, via idle.
    AeClosure* oc = (AeClosure*)g_object_get_data(G_OBJECT(widget),
                                                  "aeui-onlayout-closure");
    if (oc) {
        int lw = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "aeui-onlayout-w"));
        int lh = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "aeui-onlayout-h"));
        if (lw != width || lh != height) {
            g_object_set_data(G_OBJECT(widget), "aeui-onlayout-w", GINT_TO_POINTER(width));
            g_object_set_data(G_OBJECT(widget), "aeui-onlayout-h", GINT_TO_POINTER(height));
            AeuiLayoutFire* lf = g_new0(AeuiLayoutFire, 1);
            lf->cl = oc;
            lf->w = width;
            lf->h = height;
            g_idle_add(aeui_layout_fire_idle, lf);
        }
    }
}

static void aeui_flex_layout_class_init(AeuiFlexLayoutClass* klass) {
    GtkLayoutManagerClass* lm = GTK_LAYOUT_MANAGER_CLASS(klass);
    lm->get_request_mode = aeui_flex_layout_get_request_mode;
    lm->measure = aeui_flex_layout_measure;
    lm->allocate = aeui_flex_layout_allocate;
}
static void aeui_flex_layout_init(AeuiFlexLayout* self) {
    self->orient = GTK_ORIENTATION_VERTICAL;
    self->spacing = 0;
}

// A box's orientation, flex-aware. GtkBox DELEGATES its GtkOrientable to
// its GtkBoxLayout, so once the flex manager is installed the stock getter
// warns ("invalid cast from 'AeuiFlexLayout'") and returns garbage — every
// box-orientation read in this file must come through here.
static GtkOrientation aeui_box_orientation(GtkWidget* box) {
    GtkLayoutManager* lm = gtk_widget_get_layout_manager(box);
    if (lm && G_TYPE_CHECK_INSTANCE_TYPE(lm, aeui_flex_layout_get_type())) {
        return ((AeuiFlexLayout*)lm)->orient;
    }
    return gtk_orientable_get_orientation(GTK_ORIENTABLE(box));
}

// Swap a GtkBox's stock layout manager for the flex one (idempotent).
// Returns NULL if w isn't a box. Orientation + spacing are captured from
// the stock GtkBoxLayout BEFORE the swap (they live in the layout, not
// the box).
static AeuiFlexLayout* aeui_ensure_flex_layout(GtkWidget* w) {
    if (!w || !GTK_IS_BOX(w)) return NULL;
    GtkLayoutManager* cur = gtk_widget_get_layout_manager(w);
    if (cur && G_TYPE_CHECK_INSTANCE_TYPE(cur, aeui_flex_layout_get_type())) {
        return (AeuiFlexLayout*)cur;
    }
    GtkOrientation orient = gtk_orientable_get_orientation(GTK_ORIENTABLE(w));
    int spacing = gtk_box_get_spacing(GTK_BOX(w));
    AeuiFlexLayout* fl = g_object_new(aeui_flex_layout_get_type(), NULL);
    fl->orient = orient;
    fl->spacing = spacing;
    gtk_widget_set_layout_manager(w, GTK_LAYOUT_MANAGER(fl));
    return fl;
}

// ui.weight(child, n): proportional main-axis sharing among stack children.
void aether_ui_widget_weight_impl(int handle, int n) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || n < 1) return;
    GtkWidget* parent = gtk_widget_get_parent(w);
    if (aeui_ensure_flex_layout(parent) == NULL) return;
    g_object_set_data(G_OBJECT(w), "aeui-weight", GINT_TO_POINTER(n));
    gtk_widget_queue_resize(parent);
}

// ui.on_layout(handle, cb): cb(w, h) after the stack's allocation changes.
// Stacks (vstack/hstack) only — for canvases use canvas_on_resize.
void aether_ui_on_layout_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    if (aeui_ensure_flex_layout(w) == NULL) return;
    g_object_set_data(G_OBJECT(w), "aeui-onlayout-closure", boxed_closure);
    gtk_widget_queue_resize(w);
}

// wrap — GtkFlowBox: a flow container whose children wrap to the next
// line when the width runs out. Children arrive via the GTK_IS_FLOW_BOX
// arm in aether_ui_widget_add_child_ctx; GTK interposes a GtkFlowBoxChild
// between the flowbox and each child ("wrapitem" in the widget JSON).
int aether_ui_wrap_create(void) {
    ensure_gtk_init();
    GtkWidget* fb = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), FALSE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 6);
    // Without a cap GtkFlowBox happily stays on one line forever; a high
    // cap keeps "wrap when narrow" while never forcing early wraps.
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 100);
    return aether_ui_register_widget(fb);
}

// splitview — GtkPaned, the native user-draggable two-pane splitter. The
// first two children attached become the start/end panes (see the
// GTK_IS_PANED arm in aether_ui_widget_add_child_ctx). Panes resize with
// the window and don't shrink below their minimum.
int aether_ui_splitview_create(int vertical) {
    ensure_gtk_init();
    GtkWidget* paned = gtk_paned_new(vertical ? GTK_ORIENTATION_VERTICAL
                                              : GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);
    return aether_ui_register_widget(paned);
}

// Splitter position in px from the start edge; -1 if not a splitview.
int aether_ui_split_position_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_PANED(w)) return -1;
    return gtk_paned_get_position(GTK_PANED(w));
}

/* #95: give a widget a width, or ask what width it has. Nothing in the
 * surface could do either, so a panel layout had no way to state how wide a
 * panel should be. size_request is GTK's floor; paired with the widget not
 * expanding, it is what actually holds a panel at a width. get_ reads the
 * ALLOCATION, which is what the widget really got, not what was asked for. */
void aether_ui_set_width_impl(int handle, int px) {
    GtkWidget* w = (GtkWidget*)aether_ui_get_widget(handle);
    if (!w) return;
    int cur_h = -1;
    gtk_widget_get_size_request(w, NULL, &cur_h);
    gtk_widget_set_size_request(w, px, cur_h);
    gtk_widget_set_hexpand(w, FALSE);
}

void aether_ui_set_height_impl(int handle, int px) {
    GtkWidget* w = (GtkWidget*)aether_ui_get_widget(handle);
    if (!w) return;
    int cur_w = -1;
    gtk_widget_get_size_request(w, &cur_w, NULL);
    gtk_widget_set_size_request(w, cur_w, px);
    gtk_widget_set_vexpand(w, FALSE);
}

int aether_ui_get_width_impl(int handle) {
    GtkWidget* w = (GtkWidget*)aether_ui_get_widget(handle);
    return w ? gtk_widget_get_width(w) : 0;
}

int aether_ui_get_height_impl(int handle) {
    GtkWidget* w = (GtkWidget*)aether_ui_get_widget(handle);
    return w ? gtk_widget_get_height(w) : 0;
}

void aether_ui_split_set_position_impl(int handle, int px) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_PANED(w)) return;
    gtk_paned_set_position(GTK_PANED(w), px);
}

// ---------------------------------------------------------------------------
// Input widgets (Group 2)
// ---------------------------------------------------------------------------

// Textfield — single-line text input with on_change callback.
// Callback receives the new text: ((void(*)(void*, const char*))fn)(env, text)
static void on_entry_changed(GtkEditable* editable, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        const char* text = gtk_editable_get_text(editable);
        ((void(*)(void*, const char*))c->fn)(c->env, text ? text : "");
    }
}

// Two-way value binding: the editable widget's text writes back to a string
// state cell (compare-first, so it doesn't fight the state→widget push).
static void on_value_binding_changed(GtkEditable* editable, gpointer data) {
    int state_handle = (int)(intptr_t)data;
    StateCell* c = state_cell(state_handle);
    if (!c || c->type != AEUI_STATE_STRING) return;
    const char* text = gtk_editable_get_text(editable);
    if (!text) text = "";
    if (c->str && strcmp(c->str, text) == 0) return;  // no change → no echo
    aether_ui_state_set_s(state_handle, text);
}

// bind_value(widget, string_state): typing updates the state; setting the
// state updates the field. One verb, both directions, no callback.
void aether_ui_bind_value(int state_handle, int widget_handle) {
    PropBinding* b = prop_binding_new(AEUI_BIND_VALUE, state_handle, widget_handle);
    GtkWidget* w = aether_ui_get_widget(widget_handle);
    if (w && GTK_IS_EDITABLE(w)) {
        g_signal_connect(w, "changed", G_CALLBACK(on_value_binding_changed),
                         (gpointer)(intptr_t)state_handle);
    }
    apply_prop_binding(b);  // seed the field from the state's initial value
}

int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* entry = gtk_entry_new();
    if (placeholder && *placeholder) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
        g_object_set_data_full(G_OBJECT(entry), "aeui-placeholder",
                               g_strdup(placeholder), g_free);
    }
    if (boxed_closure) {
        g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), boxed_closure);
    }
    return aether_ui_register_widget(entry);
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_ENTRY(w)) {
        GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
        gtk_entry_buffer_set_text(buf, text ? text : "", -1);
    }
}

const char* aether_ui_textfield_get_text(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_ENTRY(w)) {
        GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
        return gtk_entry_buffer_get_text(buf);
    }
    return "";
}

// Securefield — password entry (masked input).
int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    if (placeholder && *placeholder) {
        // GtkPasswordEntry doesn't have placeholder API directly;
        // set via the internal GtkText child
        GtkWidget* text_widget = gtk_widget_get_first_child(entry);
        if (text_widget && GTK_IS_TEXT(text_widget)) {
            gtk_text_set_placeholder_text(GTK_TEXT(text_widget), placeholder);
        g_object_set_data_full(G_OBJECT(entry), "aeui-placeholder",
                               g_strdup(placeholder), g_free);
        }
    }
    if (boxed_closure) {
        g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), boxed_closure);
    }
    return aether_ui_register_widget(entry);
}

// Toggle — checkbox with label.
// Callback receives 1 (active) or 0 (inactive).
static void on_toggle_changed(GtkCheckButton* btn, GParamSpec* pspec, gpointer data) {
    (void)pspec;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        int active = gtk_check_button_get_active(btn) ? 1 : 0;
        ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)active);
    }
}

int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* check = gtk_check_button_new_with_label(label ? label : "");
    if (boxed_closure) {
        g_signal_connect(check, "notify::active", G_CALLBACK(on_toggle_changed), boxed_closure);
    }
    return aether_ui_register_widget(check);
}

void aether_ui_toggle_set_active(int handle, int active) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_CHECK_BUTTON(w)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w), active != 0);
    }
}

int aether_ui_toggle_get_active(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_CHECK_BUTTON(w)) {
        return gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? 1 : 0;
    }
    return 0;
}

// Put a toggle in a mutual-exclusion group with another: GTK4 renders
// grouped check buttons as RADIO buttons (round indicator) and enforces
// exactly-one-active itself. Chain each new member to the group's first
// toggle. Radio groups beat a dropdown here: GtkDropDown's option list is a
// popover, which never displays under sommelier (ChromeOS).
void aether_ui_toggle_set_group(int handle, int group_with) {
    GtkWidget* w = aether_ui_get_widget(handle);
    GtkWidget* g = aether_ui_get_widget(group_with);
    if (w && g && GTK_IS_CHECK_BUTTON(w) && GTK_IS_CHECK_BUTTON(g)) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(w), GTK_CHECK_BUTTON(g));
    }
}

// Slider — horizontal scale with min/max/initial and on_change callback.
// Callback receives the new double value.
static void on_slider_changed(GtkRange* range, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        double val = gtk_range_get_value(range);
        ((void(*)(void*, double))c->fn)(c->env, val);
    }
}

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure) {
    ensure_gtk_init();
    GtkAdjustment* adj = gtk_adjustment_new(initial, min_val, max_val,
                                             1.0, 10.0, 0.0);
    GtkWidget* scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_widget_set_hexpand(scale, TRUE);
    if (boxed_closure) {
        g_signal_connect(scale, "value-changed", G_CALLBACK(on_slider_changed), boxed_closure);
    }
    return aether_ui_register_widget(scale);
}

void aether_ui_slider_set_value(int handle, double value) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_SCALE(w)) {
        gtk_range_set_value(GTK_RANGE(w), value);
    }
}

double aether_ui_slider_get_value(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_SCALE(w)) {
        return gtk_range_get_value(GTK_RANGE(w));
    }
    return 0.0;
}

// Picker — dropdown / combo box.
// Callback receives the selected index (int).
//
// Two surfaces, chosen at runtime (same pattern as the context menu):
//   * DEFAULT: a GtkDropDown — the native combo on mainstream Linux.
//   * DRAWN: a GtkButton trigger + an overlay list, drawn IN-WINDOW so it
//     works where GtkDropDown's popup doesn't (sommelier: the list is an
//     xdg_popup that never displays). Selected by $AETHER_UI_PICKER=drawn,
//     default under $SOMMELIER_VERSION. Both surfaces share the picker ABI
//     (create/add_item/set_selected/get_selected) behind the same handle.
static void on_picker_changed(GtkDropDown* dropdown, GParamSpec* pspec, gpointer data) {
    (void)pspec;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        guint idx = gtk_drop_down_get_selected(dropdown);
        ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)idx);
    }
}

// True when the drawn picker surface should be used (sommelier, or forced).
static int aeui_picker_use_drawn(void) {
    const char* force = getenv("AETHER_UI_PICKER");
    if (force && strcmp(force, "drawn") == 0) return 1;
    if (force && strcmp(force, "native") == 0) return 0;
    return getenv("SOMMELIER_VERSION") != NULL;
}

// Per-drawn-picker state, hung off the trigger button via g_object_set_data.
typedef struct {
    GPtrArray* items;      // char* (owned copies)
    int selected;
    AeClosure* on_change;  // boxed, owned by Aether side
    GtkWidget* trigger;    // the button (weak)
    int overlay_handle;    // the open list overlay, or 0
} DrawnPicker;

static DrawnPicker* drawn_picker_of(GtkWidget* w) {
    if (!w || !GTK_IS_BUTTON(w)) return NULL;
    return (DrawnPicker*)g_object_get_data(G_OBJECT(w), "aeui-drawn-picker");
}

static void drawn_picker_update_label(DrawnPicker* dp) {
    const char* txt = "";
    if (dp->selected >= 0 && dp->selected < (int)dp->items->len)
        txt = (const char*)g_ptr_array_index(dp->items, dp->selected);
    gtk_button_set_label(GTK_BUTTON(dp->trigger), txt);
}

// A row in the open list was chosen: set selection, fire change, close list.
static void on_drawn_picker_row(GtkListBox* lb, GtkListBoxRow* row, gpointer data) {
    (void)lb;
    DrawnPicker* dp = (DrawnPicker*)data;
    int idx = gtk_list_box_row_get_index(row);
    if (dp->overlay_handle) {
        aether_ui_overlay_close_impl(dp->overlay_handle);
        dp->overlay_handle = 0;
    }
    if (idx >= 0 && idx != dp->selected) {
        dp->selected = idx;
        drawn_picker_update_label(dp);
        if (dp->on_change && dp->on_change->fn)
            ((void(*)(void*, intptr_t))dp->on_change->fn)(dp->on_change->env,
                                                          (intptr_t)idx);
    }
}

// Trigger clicked → open an overlay list anchored under the button.
static void on_drawn_picker_clicked(GtkButton* btn, gpointer data) {
    DrawnPicker* dp = (DrawnPicker*)data;
    if (dp->overlay_handle && aether_ui_overlay_is_live_impl(dp->overlay_handle)) {
        aether_ui_overlay_close_impl(dp->overlay_handle);
        dp->overlay_handle = 0;
        return;
    }
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "aui-drawn-dropdown");
    for (guint i = 0; i < dp->items->len; i++) {
        GtkWidget* lbl = gtk_label_new((const char*)g_ptr_array_index(dp->items, i));
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_widget_set_margin_start(lbl, 10); gtk_widget_set_margin_end(lbl, 10);
        gtk_widget_set_margin_top(lbl, 4);    gtk_widget_set_margin_bottom(lbl, 4);
        gtk_list_box_append(GTK_LIST_BOX(list), lbl);
    }
    g_signal_connect(list, "row-activated", G_CALLBACK(on_drawn_picker_row), dp);
    // Anchor just below the trigger (top-start + the trigger's window-local
    // top-left offset by its height).
    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(btn));
    int mx = 0, my = 0;
    graphene_rect_t r;
    if (root && GTK_IS_WINDOW(root) &&
        gtk_widget_compute_bounds(GTK_WIDGET(btn), GTK_WIDGET(root), &r)) {
        mx = (int)r.origin.x;
        my = (int)(r.origin.y + r.size.height + 2);
    }
    int content = aether_ui_register_widget(list);
    dp->overlay_handle = aether_ui_overlay_open_impl(0, content, 0, mx, my, 0);
}

int aether_ui_picker_create(void* boxed_closure) {
    ensure_gtk_init();
    if (aeui_picker_use_drawn()) {
        GtkWidget* btn = gtk_button_new_with_label("");
        DrawnPicker* dp = g_new0(DrawnPicker, 1);
        dp->items = g_ptr_array_new_with_free_func(g_free);
        dp->selected = 0;
        dp->on_change = (AeClosure*)boxed_closure;
        dp->trigger = btn;
        dp->overlay_handle = 0;
        g_object_set_data(G_OBJECT(btn), "aeui-drawn-picker", dp);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_drawn_picker_clicked), dp);
        return aether_ui_register_widget(btn);
    }
    GtkStringList* model = gtk_string_list_new(NULL);
    GtkWidget* dropdown = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
    if (boxed_closure) {
        g_signal_connect(dropdown, "notify::selected",
                         G_CALLBACK(on_picker_changed), boxed_closure);
    }
    return aether_ui_register_widget(dropdown);
}

void aether_ui_picker_add_item(int handle, const char* item) {
    GtkWidget* w = aether_ui_get_widget(handle);
    DrawnPicker* dp = drawn_picker_of(w);
    if (dp) {
        g_ptr_array_add(dp->items, g_strdup(item ? item : ""));
        // First item becomes the shown label (selected defaults to 0).
        if (dp->items->len == 1) drawn_picker_update_label(dp);
        return;
    }
    if (w && GTK_IS_DROP_DOWN(w)) {
        GListModel* model = gtk_drop_down_get_model(GTK_DROP_DOWN(w));
        if (GTK_IS_STRING_LIST(model)) {
            gtk_string_list_append(GTK_STRING_LIST(model), item ? item : "");
        }
    }
}

void aether_ui_picker_set_selected(int handle, int index) {
    GtkWidget* w = aether_ui_get_widget(handle);
    DrawnPicker* dp = drawn_picker_of(w);
    if (dp) {
        dp->selected = index;
        drawn_picker_update_label(dp);
        return;
    }
    if (w && GTK_IS_DROP_DOWN(w)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w), (guint)index);
    }
}

int aether_ui_picker_get_selected(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    DrawnPicker* dp = drawn_picker_of(w);
    if (dp) return dp->selected;
    if (w && GTK_IS_DROP_DOWN(w)) {
        return (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
    }
    return 0;
}

// Textarea — multi-line text input in a scrolled window.
// Callback receives the full text content.
static void on_textbuffer_changed(GtkTextBuffer* buffer, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_end_iter(buffer, &end);
        char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        ((void(*)(void*, const char*))c->fn)(c->env, text ? text : "");
        g_free(text);
    }
}

/* GtkTextView has no placeholder-text property -- that is GtkEntry's, and the
   textarea is a text VIEW. The idiomatic GTK4 substitute is an overlay: a
   dimmed label sitting on top of the view, hidden the moment the buffer is
   non-empty. Kept in sync from the buffer's own "changed" signal, so it is
   correct however the text arrives (typed, pasted, or set by the app through
   textarea_set_text). */
static void aeui_textarea_sync_placeholder(GtkTextBuffer* buf, gpointer data) {
    GtkWidget* label = GTK_WIDGET(data);
    if (!GTK_IS_WIDGET(label)) return;
    gtk_widget_set_visible(label, gtk_text_buffer_get_char_count(buf) == 0);
}

const char* aether_ui_placeholder_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "";
    const char* p = (const char*)g_object_get_data(G_OBJECT(w), "aeui-placeholder");
    return p ? p : "";
}

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* textview = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textview), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(textview), 4);

    // Wrap in a scrolled window
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 80);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(scrolled, TRUE);

    if (placeholder && *placeholder) {
        /* The overlay wraps the TEXT VIEW, not the scrolled window, so the
           hint scrolls and clips with the content instead of floating over
           the frame. The label must not take input, or it would eat clicks
           meant for the view underneath it. */
        GtkWidget* ov = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(ov), textview);
        GtkWidget* hint = gtk_label_new(placeholder);
        gtk_widget_set_halign(hint, GTK_ALIGN_START);
        gtk_widget_set_valign(hint, GTK_ALIGN_START);
        gtk_widget_set_margin_start(hint, 6);
        gtk_widget_set_margin_top(hint, 4);
        gtk_widget_set_can_target(hint, FALSE);
        gtk_widget_set_can_focus(hint, FALSE);
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), hint);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), ov);

        GtkTextBuffer* pbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
        g_signal_connect(pbuf, "changed",
                         G_CALLBACK(aeui_textarea_sync_placeholder), hint);
        aeui_textarea_sync_placeholder(pbuf, hint);
        g_object_set_data(G_OBJECT(textview), "aeui-placeholder-label", hint);
        /* Stashed on BOTH halves, because the two backends disagree about
           which one IS the textarea: this backend reports the TEXT VIEW as
           "textarea" (GTK_IS_TEXT_VIEW) and the scrolled window as
           "scrollview", while AppKit registers the SCROLL VIEW as
           AUI_TEXTAREA and its inner view as AUI_TEXTAREA_INNER. So a spec
           that looks a textarea up by type gets a different widget per
           platform. Answering from either handle costs one extra pointer and
           means the readback does not depend on that disagreement. */
        g_object_set_data_full(G_OBJECT(scrolled), "aeui-placeholder",
                               g_strdup(placeholder), g_free);
        g_object_set_data_full(G_OBJECT(textview), "aeui-placeholder",
                               g_strdup(placeholder), g_free);
    } else {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), textview);
    }

    if (boxed_closure) {
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
        g_signal_connect(buf, "changed", G_CALLBACK(on_textbuffer_changed), boxed_closure);
    }

    // Store the textview handle separately so set/get text works on it
    // Register the scrolled window as the visible widget
    int scroll_handle = aether_ui_register_widget(scrolled);
    // Also register the textview for direct text access
    aether_ui_register_widget(textview);
    // The textview handle is scroll_handle + 1
    return scroll_handle;
}

void aether_ui_textarea_set_text(int handle, const char* text) {
    // Textarea handle points to scrolled window; textview is handle+1
    GtkWidget* w = aether_ui_get_widget(handle + 1);
    if (w && GTK_IS_TEXT_VIEW(w)) {
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
        gtk_text_buffer_set_text(buf, text ? text : "", -1);
    }
}

char* aether_ui_textarea_get_text(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle + 1);
    if (w && GTK_IS_TEXT_VIEW(w)) {
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_end_iter(buf, &end);
        return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    }
    return strdup("");
}

// Scrollview container
int aether_ui_scrollview_create(void) {
    ensure_gtk_init();
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(scrolled, TRUE);
    return aether_ui_register_widget(scrolled);
}

// Progressbar
int aether_ui_progressbar_create(double fraction) {
    ensure_gtk_init();
    GtkWidget* bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), fraction);
    gtk_widget_set_hexpand(bar, TRUE);
    return aether_ui_register_widget(bar);
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_PROGRESS_BAR(w)) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), fraction);
    }
}

// ---------------------------------------------------------------------------
// Layout containers (Group 3)
// ---------------------------------------------------------------------------

// ZStack — overlay container where children stack on top of each other.
int aether_ui_zstack_create(void) {
    ensure_gtk_init();
    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_widget_set_hexpand(overlay, TRUE);
    return aether_ui_register_widget(overlay);
}

// Form — vertical box with padding, suitable for form layouts.
int aether_ui_form_create(void) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    return aether_ui_register_widget(box);
}

// Form section — frame with title and inner content box.
int aether_ui_form_section_create(const char* title) {
    ensure_gtk_init();
    GtkWidget* frame = gtk_frame_new(title);
    GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(inner, 8);
    gtk_widget_set_margin_bottom(inner, 8);
    gtk_widget_set_margin_start(inner, 8);
    gtk_widget_set_margin_end(inner, 8);
    gtk_frame_set_child(GTK_FRAME(frame), inner);
    // Register both: frame for tree, inner for child appending
    int frame_handle = aether_ui_register_widget(frame);
    aether_ui_register_widget(inner);
    return frame_handle;
}

// ---------------------------------------------------------------------------
// Tabs — GtkStackSwitcher (the clickable strip) over a GtkStack (the page
// bodies), packed in a vertical box. tabs() returns the box handle; tab()
// adds a titled page and returns its inner container. Native, themed,
// keyboard-navigable — no reinvention. The select callback is a boxed
// closure fired with the new index, exactly like picker's on_change.
// ---------------------------------------------------------------------------
typedef struct {
    GtkWidget* stack;       // the GtkStack holding page bodies (weak)
    GtkStackSwitcher* strip; // the switcher (weak)
    int page_count;
    int last_index;         // to fire on_change only on real changes
    AeClosure* on_change;   // boxed, owned by Aether side
} TabsState;

static TabsState* tabs_state_of(GtkWidget* box) {
    if (!box || !GTK_IS_BOX(box)) return NULL;
    return (TabsState*)g_object_get_data(G_OBJECT(box), "aeui-tabs");
}

static void on_tabs_page_changed(GObject* stack, GParamSpec* pspec, gpointer data) {
    (void)pspec;
    TabsState* ts = (TabsState*)data;
    if (!ts) return;
    // Resolve the visible page's index from its stored name ("page_N").
    GtkWidget* vis = gtk_stack_get_visible_child(GTK_STACK(stack));
    if (!vis) return;
    GtkStackPage* pg = gtk_stack_get_page(GTK_STACK(stack), vis);
    const char* name = gtk_stack_page_get_name(pg);
    int idx = (name && strncmp(name, "page_", 5) == 0) ? atoi(name + 5) : 0;
    if (idx == ts->last_index) return;
    ts->last_index = idx;
    if (ts->on_change && ts->on_change->fn) {
        ((void(*)(void*, intptr_t))ts->on_change->fn)(ts->on_change->env,
                                                      (intptr_t)idx);
    }
}

int aether_ui_tabs_create(void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 150);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_set_hexpand(stack, TRUE);

    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));

    gtk_box_append(GTK_BOX(box), switcher);
    gtk_box_append(GTK_BOX(box), stack);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_hexpand(box, TRUE);

    TabsState* ts = g_new0(TabsState, 1);
    ts->stack = stack;
    ts->strip = GTK_STACK_SWITCHER(switcher);
    ts->page_count = 0;
    ts->last_index = 0;
    ts->on_change = (AeClosure*)boxed_closure;
    g_object_set_data_full(G_OBJECT(box), "aeui-tabs", ts, g_free);
    g_signal_connect(stack, "notify::visible-child",
                     G_CALLBACK(on_tabs_page_changed), ts);
    return aether_ui_register_widget(box);
}

// tab(title): add a titled page, return its inner container handle so the
// DSL block's children attach INSIDE the page. Returns 0 if `tabs_handle`
// isn't a tabs box.
int aether_ui_tab_add(int tabs_handle, const char* title) {
    GtkWidget* box = aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_of(box);
    if (!ts) return 0;
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(page, TRUE);
    gtk_widget_set_hexpand(page, TRUE);
    char name[32];
    snprintf(name, sizeof(name), "page_%d", ts->page_count);
    GtkStackPage* pg = gtk_stack_add_titled(GTK_STACK(ts->stack), page,
                                            name, title ? title : "");
    (void)pg;
    ts->page_count++;
    return aether_ui_register_widget(page);
}

int aether_ui_tabs_selected(int tabs_handle) {
    GtkWidget* box = aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_of(box);
    if (!ts) return -1;
    GtkWidget* vis = gtk_stack_get_visible_child(GTK_STACK(ts->stack));
    if (!vis) return -1;
    GtkStackPage* pg = gtk_stack_get_page(GTK_STACK(ts->stack), vis);
    const char* name = gtk_stack_page_get_name(pg);
    return (name && strncmp(name, "page_", 5) == 0) ? atoi(name + 5) : 0;
}

int aether_ui_tabs_count(int tabs_handle) {
    GtkWidget* box = aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_of(box);
    return ts ? ts->page_count : 0;
}

void aether_ui_tabs_select(int tabs_handle, int index) {
    GtkWidget* box = aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_of(box);
    if (!ts || index < 0 || index >= ts->page_count) return;
    char name[32];
    snprintf(name, sizeof(name), "page_%d", index);
    GtkWidget* page = gtk_stack_get_child_by_name(GTK_STACK(ts->stack), name);
    if (page) gtk_stack_set_visible_child(GTK_STACK(ts->stack), page);
}

void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    GtkWidget* box = aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_of(box);
    if (ts) ts->on_change = (AeClosure*)boxed_closure;
}

// NavStack — page stack with slide transitions.
static int navstack_page_counts[64] = {0};

int aether_ui_navstack_create(void) {
    ensure_gtk_init();
    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 250);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_set_hexpand(stack, TRUE);
    int handle = aether_ui_register_widget(stack);
    if (handle <= 64) navstack_page_counts[handle - 1] = 0;
    return handle;
}

void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    GtkWidget* stack = aether_ui_get_widget(handle);
    GtkWidget* body = aether_ui_get_widget(body_handle);
    if (!stack || !body || !GTK_IS_STACK(stack)) return;
    int idx = (handle <= 64) ? navstack_page_counts[handle - 1]++ : 0;
    char name[32];
    snprintf(name, sizeof(name), "page_%d", idx);

    /* THE TITLE, as a real widget. Every backend discarded it ((void)title
       here, in win32 and in macOS alike), which is why a navstack had no back
       chrome at all -- the eleventh ABI leak, and the only one found after the
       audit was declared complete.
       
       Deliberately NOT gtk_stack_page_set_title(): that is GTK-native, and
       win32/AppKit have no equivalent, so each backend would invent its own
       chrome -- the exact pattern this codebase has spent the day removing.
       A title BAR built from ordinary widgets renders identically on all
       three AND is visible to the driver as plain text, so a spec can assert
       it with no new driver surface. */
    GtkWidget* page = body;
    if (title && title[0]) {
        /* REGISTERED widgets, not raw GTK ones: a bare gtk_label_new is
           invisible to the driver, so the title would render on screen and
           be unassertable -- which for this codebase means untested. Going
           through aether_ui_text_create / _vstack_create puts both in the
           widget registry, and the title arrives in /widgets as ordinary
           text. */
        int wrap_h = aether_ui_vstack_create(0);
        int bar_h = aether_ui_text_create(title);
        GtkWidget* wrap = aether_ui_get_widget(wrap_h);
        GtkWidget* bar = aether_ui_get_widget(bar_h);
        if (wrap && bar) {
            gtk_widget_add_css_class(bar, "aui-nav-title");
            gtk_box_append(GTK_BOX(wrap), bar);
            gtk_box_append(GTK_BOX(wrap), body);
            gtk_widget_set_vexpand(body, TRUE);
            page = wrap;
        }
    }
    gtk_stack_add_named(GTK_STACK(stack), page, name);
    gtk_stack_set_visible_child(GTK_STACK(stack), page);
}

int aether_ui_navstack_depth(int handle) {
    if (handle < 1 || handle > 64) return 0;
    return navstack_page_counts[handle - 1];
}

void aether_ui_navstack_pop(int handle) {
    GtkWidget* stack = aether_ui_get_widget(handle);
    if (!stack || !GTK_IS_STACK(stack)) return;
    if (handle > 64) return;
    int count = navstack_page_counts[handle - 1];
    if (count <= 1) return;
    char name[32];
    snprintf(name, sizeof(name), "page_%d", count - 1);
    GtkWidget* page = gtk_stack_get_child_by_name(GTK_STACK(stack), name);
    if (page) {
        // Show previous page
        char prev_name[32];
        snprintf(prev_name, sizeof(prev_name), "page_%d", count - 2);
        GtkWidget* prev = gtk_stack_get_child_by_name(GTK_STACK(stack), prev_name);
        if (prev) gtk_stack_set_visible_child(GTK_STACK(stack), prev);
        gtk_stack_remove(GTK_STACK(stack), page);
        navstack_page_counts[handle - 1]--;
    }
}

// ---------------------------------------------------------------------------
// Styling (Group 4)
// ---------------------------------------------------------------------------

// Apply CSS scoped to a single widget via a unique class name.
// Each widget gets "aui-{handle}" as a CSS class, and the CSS rule
// targets that class specifically — no global side effects.
// Single global CSS provider — accumulates all styling rules.
static char* global_css_buf = NULL;
static int global_css_len = 0;
static int global_css_cap = 0;
static GtkCssProvider* global_css_provider = NULL;

static void global_css_append(const char* rule) {
    int rlen = (int)strlen(rule);
    if (global_css_len + rlen + 2 > global_css_cap) {
        global_css_cap = (global_css_cap == 0) ? 4096 : global_css_cap * 2;
        global_css_buf = realloc(global_css_buf, global_css_cap);
    }
    memcpy(global_css_buf + global_css_len, rule, rlen);
    global_css_len += rlen;
    global_css_buf[global_css_len++] = '\n';
    global_css_buf[global_css_len] = '\0';
}

static void global_css_reload(void) {
    if (!global_css_buf) return;
    ensure_gtk_init();
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    if (!global_css_provider) {
        global_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(global_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_css_provider_load_from_data(global_css_provider, global_css_buf, -1);
}

static void aether_ui_apply_css(int handle, GtkWidget* widget, const char* property_css) {
    char class_name[32];
    snprintf(class_name, sizeof(class_name), "aui-%d", handle);
    gtk_widget_add_css_class(widget, class_name);

    char css[512];
    snprintf(css, sizeof(css), ".%s { %s }", class_name, property_css);
    global_css_append(css);
    global_css_reload();
}

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;

    char cls[32];
    snprintf(cls, sizeof(cls), "aui-bg-%d", handle);
    gtk_widget_add_css_class(w, cls);
    if (GTK_IS_BUTTON(w)) gtk_widget_add_css_class(w, "flat");

    char css[512];
    int ri = (int)(r * 255), gi = (int)(g * 255), bi = (int)(b * 255);
    if (GTK_IS_BUTTON(w)) {
        snprintf(css, sizeof(css),
            "button.flat.%s { background-color: rgb(%d,%d,%d); background-image: none; }\n"
            "button.flat.%s:hover { background-color: rgb(%d,%d,%d); background-image: none; }",
            cls, ri, gi, bi, cls, ri, gi, bi);
    } else {
        snprintf(css, sizeof(css),
            ".%s { background: rgba(%d,%d,%d,%.2f); }",
            cls, ri, gi, bi, a);
    }
    global_css_append(css);
    global_css_reload();
    // Stash the packed color for driver readback (styled_bg_impl): a theme
    // swap must be provable from the backend, not just the DSL model.
    g_object_set_data(G_OBJECT(w), "aeui-styled-bg",
                      GINT_TO_POINTER(((ri & 255) << 16) | ((gi & 255) << 8) | (bi & 255) | 0x1000000));
}

void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2,
                               int vertical) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    const char* dir = vertical ? "to bottom" : "to right";
    char prop[256];
    snprintf(prop, sizeof(prop),
        "background: linear-gradient(%s, rgb(%d,%d,%d), rgb(%d,%d,%d));",
        dir,
        (int)(r1 * 255), (int)(g1 * 255), (int)(b1 * 255),
        (int)(r2 * 255), (int)(g2 * 255), (int)(b2 * 255));
    aether_ui_apply_css(handle, w, prop);
}

void aether_ui_set_text_color(int handle, double r, double g, double b) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    if (GTK_IS_LABEL(w)) {
        PangoAttrList* attrs = pango_attr_list_new();
        PangoAttribute* attr = pango_attr_foreground_new(
            (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535));
        pango_attr_list_insert(attrs, attr);
        gtk_label_set_attributes(GTK_LABEL(w), attrs);
        pango_attr_list_unref(attrs);
    } else {
        char prop[128];
        snprintf(prop, sizeof(prop), "color: rgb(%d,%d,%d);",
            (int)(r * 255), (int)(g * 255), (int)(b * 255));
        aether_ui_apply_css(handle, w, prop);
    }
    // Stash for driver readback (styled_fg_impl); marker bit distinguishes
    // "explicitly black" from "never set".
    g_object_set_data(G_OBJECT(w), "aeui-styled-fg",
        GINT_TO_POINTER((((int)(r * 255) & 255) << 16) |
                        (((int)(g * 255) & 255) << 8) |
                        ((int)(b * 255) & 255) | 0x1000000));
}

void aether_ui_set_font_size(int handle, double size) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return;
    PangoAttrList* attrs = gtk_label_get_attributes(GTK_LABEL(w));
    if (!attrs) attrs = pango_attr_list_new();
    else attrs = pango_attr_list_copy(attrs);
    PangoAttribute* attr = pango_attr_size_new((int)(size * PANGO_SCALE));
    pango_attr_list_insert(attrs, attr);
    gtk_label_set_attributes(GTK_LABEL(w), attrs);
    pango_attr_list_unref(attrs);
}

void aether_ui_set_font_bold(int handle, int bold) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    // Stash the explicit weight for driver readback (AeCS re-theme proof) —
    // even for non-labels, where the visual is CSS-side.
    g_object_set_data(G_OBJECT(w), "aeui-styled-weight",
                      GINT_TO_POINTER(bold ? 1 : 2));
    if (!GTK_IS_LABEL(w)) {
        aether_ui_apply_css(handle, w,
            bold ? "font-weight: bold;" : "font-weight: normal;");
        return;
    }
    PangoAttrList* attrs = gtk_label_get_attributes(GTK_LABEL(w));
    if (!attrs) attrs = pango_attr_list_new();
    else attrs = pango_attr_list_copy(attrs);
    PangoAttribute* attr = pango_attr_weight_new(
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_attr_list_insert(attrs, attr);
    gtk_label_set_attributes(GTK_LABEL(w), attrs);
    pango_attr_list_unref(attrs);
}

// Widget-level font family (AeCS v1.1). Labels get a Pango family attribute
// (composes with the size/weight attrs above); everything else goes through
// per-widget CSS. The family string is stashed for driver readback.
void aether_ui_set_font_family(int handle, const char* family) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !family || !family[0]) return;
    g_object_set_data_full(G_OBJECT(w), "aeui-styled-family",
                           g_strdup(family), g_free);
    if (GTK_IS_LABEL(w)) {
        PangoAttrList* attrs = gtk_label_get_attributes(GTK_LABEL(w));
        if (!attrs) attrs = pango_attr_list_new();
        else attrs = pango_attr_list_copy(attrs);
        pango_attr_list_insert(attrs, pango_attr_family_new(family));
        gtk_label_set_attributes(GTK_LABEL(w), attrs);
        pango_attr_list_unref(attrs);
    } else {
        char prop[192];
        snprintf(prop, sizeof(prop), "font-family: %s;", family);
        aether_ui_apply_css(handle, w, prop);
    }
}

const char* aether_ui_styled_font_family_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "";
    const char* f = (const char*)g_object_get_data(G_OBJECT(w), "aeui-styled-family");
    return f ? f : "";
}

const char* aether_ui_styled_weight_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "";
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-styled-weight"));
    if (v == 1) return "bold";
    if (v == 2) return "normal";
    return "";
}

void aether_ui_set_corner_radius(int handle, double radius) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char prop[64];
    snprintf(prop, sizeof(prop), "border-radius: %dpx;", (int)radius);
    aether_ui_apply_css(handle, w, prop);
}

// Solid border. width 0 clears it (border: none), so a re-theme can remove a
// border the previous sheet set — same two-valued discipline as font weight.
void aether_ui_set_border(int handle, double width, double r, double g, double b) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char prop[128];
    if (width <= 0.0) {
        snprintf(prop, sizeof(prop), "border: none;");
    } else {
        snprintf(prop, sizeof(prop), "border: %dpx solid rgb(%d,%d,%d);",
            (int)width, (int)(r * 255), (int)(g * 255), (int)(b * 255));
    }
    aether_ui_apply_css(handle, w, prop);
    // Stash for driver readback, so a spec can prove the border reached the
    // backend rather than merely being recorded in the model.
    g_object_set_data(G_OBJECT(w), "aeui-styled-border",
                      GINT_TO_POINTER(((int)width << 24) | 0x40000000
                                      | (((int)(r*255) & 0xFF) << 16)
                                      | (((int)(g*255) & 0xFF) << 8)
                                      | ((int)(b*255) & 0xFF)));
}

// (width<<24) | flag | rgb, or -1 when unset. Width 0 with the flag set means
// an explicit "no border" (a re-theme clearing one).
int aether_ui_styled_border_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-styled-border"));
    return (v & 0x40000000) ? v : -1;
}

// ── Interaction states ──────────────────────────────────────────────────
//
// A control that does not react to the pointer reads as dead, and until now
// the only way to get a hover effect was to swap whole stylesheets by hand
// from an enter/leave handler. GTK4 gives us the pseudo-classes directly:
// the per-widget class installed by aether_ui_apply_css is the hook, so a
// hover/active rule is the same class with :hover / :active appended.
//
// state: 0 = hover, 1 = active (pressed). Colours are 0..1; a negative
// bg/fg component means "leave that property alone for this state".
void aether_ui_set_state_style(int handle, int state,
                               double br, double bg_, double bb,
                               double fr, double fg_, double fb) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char cls[32];
    snprintf(cls, sizeof(cls), "aui-st-%d", handle);
    gtk_widget_add_css_class(w, cls);
    if (GTK_IS_BUTTON(w)) gtk_widget_add_css_class(w, "flat");
    const char* pseudo = (state == 1) ? "active" : "hover";
    char decl[256];
    int n = 0;
    decl[0] = '\0';
    if (br >= 0.0) {
        n += snprintf(decl + n, sizeof(decl) - n,
                      "background-color: rgb(%d,%d,%d); background-image: none;",
                      (int)(br * 255), (int)(bg_ * 255), (int)(bb * 255));
    }
    if (fr >= 0.0) {
        n += snprintf(decl + n, sizeof(decl) - n, "color: rgb(%d,%d,%d);",
                      (int)(fr * 255), (int)(fg_ * 255), (int)(fb * 255));
    }
    /* Record the colour beside the CSS, in the same packed form the base
       background uses. The stylesheet is what PAINTS it, but nothing can ask
       GTK what background a widget currently renders -- there is no getter --
       so the readback reconstructs it from the widget's state flags and these.
       Generated from the same values as the CSS above, so the two agree by
       construction rather than by a second guess. */
    if (br >= 0.0) {
        g_object_set_data(G_OBJECT(w),
            (state == 1) ? "aeui-active-bg" : "aeui-hover-bg",
            GINT_TO_POINTER(0x1000000 | (((int)(br * 255) & 0xFF) << 16)
                                      | (((int)(bg_ * 255) & 0xFF) << 8)
                                      |  ((int)(bb * 255) & 0xFF)));
    }
    // Specificity: a bare `.cls:hover` loses to the theme's own
    // `button:hover`, so qualify with the element like set_bg_color does
    // (button.flat.CLS), and keep the plain form for non-buttons.
    char css[512];
    if (GTK_IS_BUTTON(w)) {
        snprintf(css, sizeof(css), "button.flat.%s:%s { %s }", cls, pseudo, decl);
    } else {
        snprintf(css, sizeof(css), ".%s:%s { %s }", cls, pseudo, decl);
    }
    global_css_append(css);
    global_css_reload();
    // Driver readback: prove the state style reached the backend.
    g_object_set_data(G_OBJECT(w),
                      state == 1 ? "aeui-active-style" : "aeui-hover-style",
                      GINT_TO_POINTER(0x1000000
                          | (((int)(br * 255) & 0xFF) << 16)
                          | (((int)(bg_ * 255) & 0xFF) << 8)
                          | ((int)(bb * 255) & 0xFF)));
}

int aether_ui_state_style_impl(int handle, int state) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w),
                state == 1 ? "aeui-active-style" : "aeui-hover-style"));
    return (v & 0x1000000) ? (v & 0xFFFFFF) : -1;
}

void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char prop[128];
    snprintf(prop, sizeof(prop), "padding: %dpx %dpx %dpx %dpx;",
        (int)top, (int)right, (int)bottom, (int)left);
    aether_ui_apply_css(handle, w, prop);
}


// Which backend is this binary running? Lets pure-.ae code (the goldens
// gallery picks its tests/goldens/<backend>/ directory) branch without
// per-platform builds or environment guesswork.
const char* aether_ui_backend_name_impl(void) { return "gtk4"; }

void aether_ui_set_width(int handle, int width) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    gtk_widget_set_size_request(w, width, -1);
    // A pinned width normally means "this is exactly my size" — so stop
    // expanding to fill the parent. Canvases are created hexpand=TRUE (a
    // full-window canvas should fill), which otherwise stretches a 16px ICON
    // to the whole row and pushes its label out of view.
    //
    // EXCEPT on a weighted child. `weight` is an explicit "grow me to my
    // share", and there width() means a MINIMUM (its flex-basis / min-width)
    // — the clamp that keeps a weighted child from being squeezed to nothing.
    // Pinning that child would make weight unsatisfiable, so weight wins and
    // size_request above stands alone as the floor.
    if (width > 0 && !g_object_get_data(G_OBJECT(w), "aeui-weight")) {
        gtk_widget_set_hexpand(w, FALSE);
        gtk_widget_set_halign(w, GTK_ALIGN_START);
    }
}

void aether_ui_set_height(int handle, int height) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    // Use the size request, as aether_ui_set_width does — NOT CSS. GTK4 CSS
    // has min-height but NO max-height, so the old
    //   "min-height: Npx; max-height: Npx;"
    // made GTK log a theme-parser error for EVERY height() call, on every
    // widget, forever ("No property named max-height"). The min-height half
    // did apply, so the sizing looked right and the noise went unnoticed.
    gtk_widget_set_size_request(w, -1, height);
    // A pinned height means "this is exactly my size" — stop expanding.
    // valign FILL (not CENTER): a height()'d CONTAINER — e.g. a scrollview
    // holding a list — must fill the box it was just given, or it centres a
    // shrunken viewport inside the reserved space.
    if (height > 0) {
        gtk_widget_set_vexpand(w, FALSE);
        gtk_widget_set_valign(w, GTK_ALIGN_FILL);
    }
}

void aether_ui_set_opacity(int handle, double opacity) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_opacity(w, opacity);
}

void aether_ui_set_enabled(int handle, int enabled) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_sensitive(w, enabled != 0);
}

void aether_ui_set_tooltip(int handle, const char* text) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_tooltip_text(w, text);
}

// ── Accessibility (semantics layer) ──────────────────────────────────
// Map our cross-platform role vocabulary <-> GtkAccessibleRole. Roles GTK4
// can't retarget after construction are still reported from the side-store so
// the driver reflects the author's intent; LABEL/DESCRIPTION are set for real
// via gtk_accessible_update_property (screen-reader-visible).
static const struct { const char* name; GtkAccessibleRole role; } aeui_a11y_roles[] = {
    {"button", GTK_ACCESSIBLE_ROLE_BUTTON},
    {"checkbox", GTK_ACCESSIBLE_ROLE_CHECKBOX},
    {"radio", GTK_ACCESSIBLE_ROLE_RADIO},
    {"link", GTK_ACCESSIBLE_ROLE_LINK},
    {"heading", GTK_ACCESSIBLE_ROLE_HEADING},
    {"image", GTK_ACCESSIBLE_ROLE_IMG},
    {"group", GTK_ACCESSIBLE_ROLE_GROUP},
    {"table", GTK_ACCESSIBLE_ROLE_TABLE},
    {"columnheader", GTK_ACCESSIBLE_ROLE_COLUMN_HEADER},
    {"row", GTK_ACCESSIBLE_ROLE_ROW},
    {"list", GTK_ACCESSIBLE_ROLE_LIST},
    {"listitem", GTK_ACCESSIBLE_ROLE_LIST_ITEM},
    {"tab", GTK_ACCESSIBLE_ROLE_TAB},
    {"tablist", GTK_ACCESSIBLE_ROLE_TAB_LIST},
    {"menu", GTK_ACCESSIBLE_ROLE_MENU},
    {"menuitem", GTK_ACCESSIBLE_ROLE_MENU_ITEM},
    {"dialog", GTK_ACCESSIBLE_ROLE_DIALOG},
    {"alert", GTK_ACCESSIBLE_ROLE_ALERT},
    {"textbox", GTK_ACCESSIBLE_ROLE_TEXT_BOX},
    {"slider", GTK_ACCESSIBLE_ROLE_SLIDER},
    {"progressbar", GTK_ACCESSIBLE_ROLE_PROGRESS_BAR},
    {"none", GTK_ACCESSIBLE_ROLE_PRESENTATION},
};

// GtkAccessibleRole -> our name (for readback of the AUTO role). Only the
// vocabulary we expose is mapped; anything else reports "".
static const char* aeui_a11y_role_name(GtkAccessibleRole r) {
    for (size_t i = 0; i < G_N_ELEMENTS(aeui_a11y_roles); i++)
        if (aeui_a11y_roles[i].role == r) return aeui_a11y_roles[i].name;
    return "";
}

void aether_ui_a11y_set_role_impl(int handle, const char* role) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !role) return;
    // Stash the requested role name; GTK4 fixes a widget's role at
    // construction, so the side-store is the authoritative author intent the
    // driver reports. (A future construction-time role map can honour it on
    // the platform side for our own container widgets.)
    g_object_set_data_full(G_OBJECT(w), "aeui-a11y-role",
                           g_strdup(role), g_free);
}

void aether_ui_a11y_set_label_impl(int handle, const char* name) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !name) return;
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
        GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_object_set_data_full(G_OBJECT(w), "aeui-a11y-name",
                           g_strdup(name), g_free);
}

void aether_ui_a11y_set_description_impl(int handle, const char* desc) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !desc) return;
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, desc, -1);
    g_object_set_data_full(G_OBJECT(w), "aeui-a11y-desc",
                           g_strdup(desc), g_free);
}

// Driver readback: report the EFFECTIVE role/name/description. Role prefers an
// explicit override, else the widget's real GtkAccessibleRole. Name/desc
// prefer the in-process AT view (gtk_test_accessible_check_property), else our
// side-store, else the widget's own text (a button's label is its auto name).
void aether_ui_a11y_get_impl(int handle,
                             char* role, int rolesz,
                             char* name, int namesz,
                             char* desc, int descsz) {
    if (role && rolesz) role[0] = '\0';
    if (name && namesz) name[0] = '\0';
    if (desc && descsz) desc[0] = '\0';
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;

    if (role && rolesz) {
        const char* over = g_object_get_data(G_OBJECT(w), "aeui-a11y-role");
        const char* rn = over ? over
            : aeui_a11y_role_name(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(w)));
        g_strlcpy(role, rn ? rn : "", rolesz);
    }

    if (name && namesz) {
        // Effective accessible name: our side-store (set for real via
        // gtk_accessible_update_property(LABEL)) if present, else the widget's
        // own label text — a button/label's auto accessible name. (We read the
        // side-store rather than round-tripping through the test-AT here
        // because this can run on the test-server thread, where touching GTK's
        // AT-context internals is unsafe.)
        const char* store = g_object_get_data(G_OBJECT(w), "aeui-a11y-name");
        if (store && store[0]) {
            g_strlcpy(name, store, namesz);
        } else if (GTK_IS_BUTTON(w)) {
            const char* lbl = gtk_button_get_label(GTK_BUTTON(w));
            if (lbl) g_strlcpy(name, lbl, namesz);
        } else if (GTK_IS_LABEL(w)) {
            const char* lbl = gtk_label_get_text(GTK_LABEL(w));
            if (lbl) g_strlcpy(name, lbl, namesz);
        }
    }

    if (desc && descsz) {
        const char* store = g_object_get_data(G_OBJECT(w), "aeui-a11y-desc");
        if (store) g_strlcpy(desc, store, descsz);
    }
}

void aether_ui_set_distribution(int handle, int distribution) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_BOX(w)) {
        gtk_box_set_homogeneous(GTK_BOX(w), distribution == 1);
    }
}

void aether_ui_set_alignment(int handle, int alignment) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_BOX(w)) return;
    int is_vertical = (aeui_box_orientation(w) == GTK_ORIENTATION_VERTICAL);
    GtkAlign align;
    if (is_vertical) {
        switch (alignment) {
            case 5: align = GTK_ALIGN_START; break;   // Leading
            case 9: align = GTK_ALIGN_CENTER; break;  // CenterX
            case 7: align = GTK_ALIGN_FILL; break;    // Fill
            default: align = GTK_ALIGN_FILL; break;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c;
             c = gtk_widget_get_next_sibling(c)) {
            gtk_widget_set_halign(c, align);
        }
    } else {
        switch (alignment) {
            case 3: align = GTK_ALIGN_START; break;   // Top
            case 12: align = GTK_ALIGN_CENTER; break; // CenterY
            case 4: align = GTK_ALIGN_END; break;     // Bottom
            default: align = GTK_ALIGN_FILL; break;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c;
             c = gtk_widget_get_next_sibling(c)) {
            gtk_widget_set_valign(c, align);
        }
    }
}

void aether_ui_match_parent_width(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) {
        gtk_widget_set_hexpand(w, TRUE);
        gtk_widget_set_halign(w, GTK_ALIGN_FILL);
    }
}

void aether_ui_match_parent_height(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) {
        gtk_widget_set_vexpand(w, TRUE);
        gtk_widget_set_valign(w, GTK_ALIGN_FILL);
    }
}

void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    gtk_widget_set_margin_top(w, top);
    gtk_widget_set_margin_end(w, right);
    gtk_widget_set_margin_bottom(w, bottom);
    gtk_widget_set_margin_start(w, left);
}

// _ctx variant: the ambient builder-context handle arrives as a void*.
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aether_ui_set_margin((int)(intptr_t)ctx, top, right, bottom, left);
}

// Enable the AetherUIDriver test server against the ambient root (_ctx).
void aether_ui_enable_test_server_ctx(int port, void* ctx) {
    aether_ui_enable_test_server_impl(port, (int)(intptr_t)ctx);
}

// ---------------------------------------------------------------------------
// System integration (Group 5)
// ---------------------------------------------------------------------------

// Alert dialog — modal message box with OK button.
void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) return;  // modal dialog would block on CI
    ensure_gtk_init();
    GtkWidget* dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", message ? message : "");
    if (title) gtk_window_set_title(GTK_WINDOW(dialog), title);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_widget_show(dialog);
}

// File dialogs — block and return the chosen path, "" if cancelled. Native
// modals, so headless-no-op (like alert / menu_popup): CI has no seat to
// dismiss them. GtkFileChooserNative is portable back to GTK 4.8; it's
// "async-oriented" only in that it has no run() — we drive its response
// synchronously with a nested GMainLoop, exactly as the modal message
// dialogs do, so the ABI stays blocking-and-returns-a-path across backends.
typedef struct { GMainLoop* loop; char* path; } AeuiFileResult;

static void on_file_chooser_response(GtkNativeDialog* dlg, int response,
                                     gpointer data) {
    AeuiFileResult* r = (AeuiFileResult*)data;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile* f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        if (f) { char* p = g_file_get_path(f); if (p) r->path = strdup(p);
                 g_free(p); g_object_unref(f); }
    }
    if (r->loop) g_main_loop_quit(r->loop);
}

static char* aeui_run_file_chooser(const char* title, GtkFileChooserAction action,
                                   const char* default_name,
                                   const char* start_dir) {
    ensure_gtk_init();
    const char* hl = getenv("AETHER_UI_HEADLESS");
    if (hl && hl[0] && hl[0] != '0') return strdup("");
    const char* accept = "_Open";
    if (action == GTK_FILE_CHOOSER_ACTION_SAVE) accept = "_Save";
    else if (action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER) accept = "_Select";
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
        title ? title : "", primary_window, action, accept, "_Cancel");
    if (default_name && *default_name && action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), default_name);
    }
    if (start_dir && *start_dir) {
        GFile* dir = g_file_new_for_path(start_dir);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir, NULL);
        g_object_unref(dir);
    }
    AeuiFileResult r = { g_main_loop_new(NULL, FALSE), NULL };
    g_signal_connect(dlg, "response", G_CALLBACK(on_file_chooser_response), &r);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
    g_main_loop_run(r.loop);
    g_main_loop_unref(r.loop);
    g_object_unref(dlg);
    return r.path ? r.path : strdup("");
}

char* aether_ui_file_open(const char* title, const char* start_dir) {
    return aeui_run_file_chooser(title, GTK_FILE_CHOOSER_ACTION_OPEN, NULL, start_dir);
}
char* aether_ui_file_save(const char* title, const char* default_name) {
    return aeui_run_file_chooser(title, GTK_FILE_CHOOSER_ACTION_SAVE, default_name, NULL);
}
char* aether_ui_file_pick_folder(const char* title, const char* start_dir) {
    return aeui_run_file_chooser(title, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                 NULL, start_dir);
}

// Clipboard read/write
void aether_ui_clipboard_write_impl(const char* text) {
    ensure_gtk_init();
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    GdkClipboard* clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_set_text(clipboard, text ? text : "");
}

/* GTK4 only offers the clipboard as an async read, but the Aether surface is a
 * plain call, so the read is driven to completion on a nested main loop.
 *
 * Two things make that safe rather than a trap:
 *
 *   - the wait is bounded. A clipboard whose owner never answers (a dead X11
 *     client still holding the selection is the classic case) would otherwise
 *     block the UI thread forever, so a timeout ends the loop.
 *
 *   - the context is heap-allocated and ownership moves to whoever runs last.
 *     If the timeout fires first the caller returns and marks the context
 *     abandoned; the callback, arriving into a stack frame that no longer
 *     exists, frees the context instead of writing to it. */
typedef struct {
    GMainLoop* loop;
    char*      text;
    gboolean   finished;
    gboolean   abandoned;
} AeuiClipRead;

static void aeui_clip_read_done(GObject* src, GAsyncResult* res, gpointer data) {
    AeuiClipRead* c = (AeuiClipRead*)data;
    GError* err = NULL;
    char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, &err);
    if (err) g_error_free(err);
    if (c->abandoned) {      /* caller already gave up: we own the context */
        g_free(text);
        g_free(c);
        return;
    }
    c->text = text;
    c->finished = TRUE;
    if (c->loop && g_main_loop_is_running(c->loop)) g_main_loop_quit(c->loop);
}

static gboolean aeui_clip_read_timeout(gpointer data) {
    AeuiClipRead* c = (AeuiClipRead*)data;
    if (c->loop && g_main_loop_is_running(c->loop)) g_main_loop_quit(c->loop);
    return G_SOURCE_REMOVE;
}

char* aether_ui_clipboard_read_impl(void) {
    ensure_gtk_init();
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return strdup("");
    GdkClipboard* clipboard = gdk_display_get_clipboard(display);

    AeuiClipRead* c = g_new0(AeuiClipRead, 1);
    c->loop = g_main_loop_new(NULL, FALSE);
    guint timeout_id = g_timeout_add(500, aeui_clip_read_timeout, c);
    gdk_clipboard_read_text_async(clipboard, NULL, aeui_clip_read_done, c);
    g_main_loop_run(c->loop);

    if (!c->finished) {          /* timed out: the callback inherits the context */
        c->abandoned = TRUE;
        g_main_loop_unref(c->loop);
        c->loop = NULL;
        return strdup("");
    }
    g_source_remove(timeout_id);
    g_main_loop_unref(c->loop);
    char* out = strdup(c->text ? c->text : "");
    g_free(c->text);
    g_free(c);
    return out;
}

// Timer — schedule a repeating callback at interval_ms.
static gboolean on_timer_tick(gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
    return G_SOURCE_CONTINUE;
}

/* Timer handles are OURS, not GLib's, which is what the macOS and win32
   backends have always done and this one did not: it handed the raw GSource
   id straight back and passed it straight to g_source_remove.

   That makes cancel NON-IDEMPOTENT here and nowhere else. Cancelling twice, or
   cancelling a handle whose timer is already gone, is a harmless no-op on the
   other two backends -- macOS invalidates an already-nil NSTimer, win32 finds
   no live entry -- but on GLib it is

       GLib-CRITICAL: Source ID N was not found when attempting to remove it

   which is fatal under G_DEBUG=fatal-criticals. Measured, along with two
   things it is NOT, so the next reader does not have to re-derive them:
   GLib does not recycle source ids (remove id 1, the next allocation is 3),
   so a stale handle cannot name somebody else's source; and removing a source
   from inside its own dispatch, which canvas_on_hover's watchdog does, is
   fine and raises nothing.

   The indirection buys idempotence, and costs one small table that mirrors the
   one win32 already keeps. */
typedef struct {
    int    id;
    guint  source;
    int    alive;
} AeuiTimerEntry;
static AeuiTimerEntry* aeui_timers = NULL;
static int aeui_timer_count = 0;
static int aeui_timer_cap = 0;
static int aeui_next_timer_id = 1;

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (!boxed_closure || interval_ms <= 0) return 0;
    guint source = g_timeout_add((guint)interval_ms, on_timer_tick, boxed_closure);
    if (aeui_timer_count >= aeui_timer_cap) {
        aeui_timer_cap = aeui_timer_cap == 0 ? 16 : aeui_timer_cap * 2;
        aeui_timers = realloc(aeui_timers, sizeof(AeuiTimerEntry) * aeui_timer_cap);
    }
    int id = aeui_next_timer_id++;
    aeui_timers[aeui_timer_count].id = id;
    aeui_timers[aeui_timer_count].source = source;
    aeui_timers[aeui_timer_count].alive = 1;
    aeui_timer_count++;
    return id;
}

void aether_ui_timer_cancel_impl(int timer_id) {
    if (timer_id < 1) return;
    for (int i = 0; i < aeui_timer_count; i++) {
        if (aeui_timers[i].id != timer_id) continue;
        // The alive flag, not the search, is what makes a second cancel a
        // no-op instead of a removal of somebody else's recycled source.
        if (aeui_timers[i].alive) {
            g_source_remove(aeui_timers[i].source);
            aeui_timers[i].alive = 0;
        }
        return;
    }
}

// Open URL in default browser
void aether_ui_open_url_impl(const char* url) {
    if (!url) return;
    ensure_gtk_init();
    gtk_show_uri(NULL, url, GDK_CURRENT_TIME);
}

// Dark mode detection
int aether_ui_dark_mode_check(void) {
    // Driver override first (POST /appearance?dark=N) so headless specs can
    // steer styles_for_mode without a settings daemon.
    int ov = aether_ui_appearance_override_get();
    if (ov >= 0) return ov;
    ensure_gtk_init();
    GtkSettings* settings = gtk_settings_get_default();
    if (!settings) return 0;
    gboolean dark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &dark, NULL);
    if (dark) return 1;
    // Also check theme name
    char* theme = NULL;
    g_object_get(settings, "gtk-theme-name", &theme, NULL);
    if (theme) {
        int is_dark = (strstr(theme, "dark") != NULL || strstr(theme, "Dark") != NULL);
        g_free(theme);
        if (is_dark) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// In-window overlay layer (roadmap item 1) — the Swing JLayeredPane / Flutter
// Overlay, aimed at the sommelier wound.
//
// A window's overlay entries (toasts, dropdowns, tooltips, modal scrims) are
// drawn IN-WINDOW via a GtkOverlay interposed between the window and its root
// widget — NOT as compositor popups (xdg_popup surfaces never display under
// ChromeOS/Crostini sommelier; a drawn overlay always does, by construction).
//
// The GtkOverlay is interposed LAZILY the first time an overlay opens on a
// window: we take the window's current child, make it the overlay's main
// child, and set the overlay as the window's child. When no overlay is open
// the window structure is byte-identical to before — so existing apps and
// specs see zero change. (Verified design: GtkOverlay adds no chrome and the
// main child fills it exactly as it filled the window.)
//
// Positioning is anchor (9 halign/valign combos) + margin (dx, dy). A modal
// entry first inserts a full-window scrim that eats clicks and, on click/Esc,
// fires the on-dismiss closure (Swing's glass pane).
// ---------------------------------------------------------------------------

// Anchor enum: halign in bits 0-1 (0=start,1=center,2=end), valign in bits
// 2-3. Matches the DSL string→int mapping in ui/module.ae.
static GtkAlign aeui_anchor_h(int anchor) {
    switch (anchor & 3) {
        case 0: return GTK_ALIGN_START;
        case 2: return GTK_ALIGN_END;
        default: return GTK_ALIGN_CENTER;
    }
}
static GtkAlign aeui_anchor_v(int anchor) {
    switch ((anchor >> 2) & 3) {
        case 0: return GTK_ALIGN_START;
        case 2: return GTK_ALIGN_END;
        default: return GTK_ALIGN_CENTER;
    }
}

typedef struct {
    GtkWidget* content;      // the overlay entry's content widget (weak: owned by GTK)
    GtkWidget* scrim;        // modal scrim (NULL for non-modal)
    GtkWindow* window;       // owning window
    AeClosure* on_dismiss;   // fired on scrim click / dismiss (owned copy, may be NULL)
    int modal;
    int live;                // 0 once closed
    // Per-entry enter/exit transition (transition_overlay). exit_kind is one
    // of "fade"/"slide-up"/"slide-down"/"scale" (NULL = no per-entry exit; the
    // chrome still fades via the static CSS). exiting is 1 while the exit tween
    // plays (dismissed, not yet removed).
    char* exit_kind;
    int exit_ms;
    int exiting;
    int exit_played;   // sticky: an exit tween STARTED (survives the entry's death)
    // Scrim material (overlay_material). GTK4 has no backdrop-filter, so "blur"
    // degrades to "tint" (a lighter, higher-contrast scrim). "dim" = default.
    char* material;   // effective: "dim" | "tint" (never "blur" on GTK4)
} OverlayEntry;

static OverlayEntry* overlays = NULL;
static int overlay_count = 0;
static int overlay_capacity = 0;

// Install overlay CSS (scrim dim, toast/tooltip chrome) once, on first use.
static void aeui_overlay_css_once(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    GdkDisplay* display = gdk_display_get_default();
    if (!display) { done = 0; return; }
    GtkCssProvider* prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov,
        ".aui-overlay-scrim { background-color: rgba(0,0,0,0.45); }"
        // "tint" material (the degrade of a requested "blur" — GTK4 has no
        // backdrop-filter): a lighter, frostier-looking scrim. Honestly a
        // tint, not a blur.
        ".aui-overlay-scrim-tint { background-color: rgba(245,245,250,0.35); }"
        ".aui-toast {"
        "  background-color: rgba(30,30,30,0.92); color: white;"
        "  padding: 10px 18px; border-radius: 8px; margin: 24px;"
        "  box-shadow: 0 3px 10px rgba(0,0,0,0.45);"
        "}"
        ".aui-tooltip {"
        "  background-color: rgba(20,20,20,0.95); color: white;"
        "  padding: 4px 8px; border-radius: 4px;"
        "}"
        ".aui-drawn-dropdown {"
        "  background-color: @theme_bg_color;"
        "  border: 1px solid alpha(currentColor, 0.25); border-radius: 6px;"
        "}"
        ".aui-overlay-card {"
        "  background-color: @theme_bg_color;"
        "  border: 1px solid alpha(currentColor, 0.2); border-radius: 10px;"
        "  padding: 20px;"
        "  box-shadow: 0 6px 22px rgba(0,0,0,0.35);"
        "}"
        ".aui-row-selected {"
        "  background-color: alpha(@theme_selected_bg_color, 0.85);"
        "  border-radius: 4px;"
        "}", -1);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    g_object_unref(prov);

    // Enter transitions for overlay chrome (item 6): toasts and the modal
    // scrim fade in via CSS keyframes — zero code, zero timers. Suppressed
    // by AETHER_UI_NO_ANIMATION so driver suites stay deterministic.
    const char* noanim = getenv("AETHER_UI_NO_ANIMATION");
    if (!(noanim && noanim[0] && noanim[0] != '0')) {
        GtkCssProvider* anim = gtk_css_provider_new();
        gtk_css_provider_load_from_data(anim,
            "@keyframes aui-fade-in { from { opacity: 0; } to { opacity: 1; } }"
            ".aui-toast { animation: aui-fade-in 200ms ease-out; }"
            ".aui-overlay-scrim { animation: aui-fade-in 150ms ease-out; }"
            ".aui-overlay-card { animation: aui-fade-in 150ms ease-out; }"
            // Per-entry enter transitions (transition_overlay). The class is
            // added on open; the matching -out class replaces it on dismiss so
            // GTK plays the reverse keyframes before the real removal fires.
            "@keyframes aui-slide-up-in { from { opacity: 0; margin-top: 24px; } to { opacity: 1; } }"
            "@keyframes aui-slide-up-out { from { opacity: 1; } to { opacity: 0; margin-top: 24px; } }"
            "@keyframes aui-slide-down-in { from { opacity: 0; margin-bottom: 24px; } to { opacity: 1; } }"
            "@keyframes aui-slide-down-out { from { opacity: 1; } to { opacity: 0; margin-bottom: 24px; } }"
            "@keyframes aui-scale-in { from { opacity: 0; } to { opacity: 1; } }"
            "@keyframes aui-scale-out { from { opacity: 1; } to { opacity: 0; } }"
            "@keyframes aui-fade-out { from { opacity: 1; } to { opacity: 0; } }"
            ".aui-tr-fade-in       { animation: aui-fade-in 180ms ease-out; }"
            ".aui-tr-fade-out      { animation: aui-fade-out 180ms ease-in forwards; }"
            ".aui-tr-slide-up-in   { animation: aui-slide-up-in 200ms ease-out; }"
            ".aui-tr-slide-up-out  { animation: aui-slide-up-out 200ms ease-in forwards; }"
            ".aui-tr-slide-down-in { animation: aui-slide-down-in 200ms ease-out; }"
            ".aui-tr-slide-down-out{ animation: aui-slide-down-out 200ms ease-in forwards; }"
            ".aui-tr-scale-in      { animation: aui-scale-in 180ms ease-out; }"
            ".aui-tr-scale-out     { animation: aui-scale-out 180ms ease-in forwards; }", -1);
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(anim),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
        g_object_unref(anim);
    }
}

// Interpose (lazily) and return the GtkOverlay hosting a window's content.
static GtkOverlay* aeui_window_overlay(GtkWindow* win) {
    if (!win) return NULL;
    aeui_overlay_css_once();
    GtkWidget* child = gtk_window_get_child(win);
    if (child && GTK_IS_OVERLAY(child) &&
        g_object_get_data(G_OBJECT(child), "aeui-overlay-host")) {
        return GTK_OVERLAY(child);
    }
    // Interpose: window -> overlay -> (old child as main).
    GtkWidget* ov = gtk_overlay_new();
    g_object_set_data(G_OBJECT(ov), "aeui-overlay-host", GINT_TO_POINTER(1));
    if (child) {
        g_object_ref(child);
        gtk_window_set_child(win, NULL);
        gtk_overlay_set_child(GTK_OVERLAY(ov), child);
        g_object_unref(child);
    }
    gtk_window_set_child(win, ov);
    return GTK_OVERLAY(ov);
}

// Resolve a 1-based aether window handle (0 = the primary app window) to its
// GtkWindow. Overlay APIs accept 0 to mean "the main window" (the driver's
// common case); >0 indexes extra_windows.
static GtkWindow* aeui_resolve_window(int win_handle);   // fwd (defined after WindowEntry)

static void overlay_fire_dismiss(OverlayEntry* e) {
    if (e->on_dismiss && e->on_dismiss->fn)
        ((void(*)(void*))e->on_dismiss->fn)(e->on_dismiss->env);
}

// Animation globally suppressed (driver determinism switch)?
static int aeui_anim_off(void) {
    const char* n = getenv("AETHER_UI_NO_ANIMATION");
    return n && n[0] && n[0] != '0';
}

// Map a transition kind to its enter/exit CSS class. Returns NULL for an
// unknown kind (caller falls back to no per-entry animation).
static const char* aeui_tr_class(const char* kind, int entering) {
    if (!kind) return NULL;
    if (strcmp(kind, "fade") == 0)       return entering ? "aui-tr-fade-in"       : "aui-tr-fade-out";
    if (strcmp(kind, "slide-up") == 0)   return entering ? "aui-tr-slide-up-in"   : "aui-tr-slide-up-out";
    if (strcmp(kind, "slide-down") == 0) return entering ? "aui-tr-slide-down-in" : "aui-tr-slide-down-out";
    if (strcmp(kind, "scale") == 0)      return entering ? "aui-tr-scale-in"      : "aui-tr-scale-out";
    return NULL;
}

// Actually detach the entry's widgets. Split out so a deferred exit tween can
// call it once the animation finishes.
static void unregister_widget_tree(GtkWidget* w);

static void overlay_detach(OverlayEntry* e) {
    GtkOverlay* ov = aeui_window_overlay(e->window);
    /* UNREGISTER FIRST, and it must be first: `widgets` holds RAW, unreffed
       GtkWidget pointers, and gtk_overlay_remove_overlay drops the last ref,
       so the widgets are finalized on the way out. Clearing the registry
       afterwards is impossible (the tree is gone to walk) and leaving it
       populated is worse than a leak -- every handle inside a closed overlay
       would be a dangling pointer the next driver or app call dereferences. */
    if (e->scrim)   unregister_widget_tree(e->scrim);
    if (e->content) unregister_widget_tree(e->content);
    if (ov) {
        if (e->scrim) gtk_overlay_remove_overlay(ov, e->scrim);
        if (e->content) gtk_overlay_remove_overlay(ov, e->content);
    }
    e->scrim = NULL;
    e->content = NULL;
    e->exiting = 0;
    e->live = 0;
}

// g_timeout callback: the exit tween has played, remove for real.
static gboolean overlay_exit_done(gpointer data) {
    overlay_detach((OverlayEntry*)data);
    return G_SOURCE_REMOVE;
}

static void overlay_do_close(OverlayEntry* e) {
    if (!e->live || e->exiting) return;
    // A per-entry exit transition (and animation on): swap the enter class for
    // the -out class so GTK plays the reverse keyframes, mark the entry exiting
    // (still counted live so the layer stays interactive-blocked), and detach
    // once the tween finishes. Everything else removes immediately.
    const char* out = aeui_tr_class(e->exit_kind, 0);
    if (out && e->content && !aeui_anim_off()) {
        const char* in = aeui_tr_class(e->exit_kind, 1);
        if (in) gtk_widget_remove_css_class(e->content, in);
        gtk_widget_add_css_class(e->content, out);
        if (e->scrim) gtk_widget_add_css_class(e->scrim, "aui-tr-fade-out");
        e->exiting = 1;
        e->exit_played = 1;
        g_timeout_add(e->exit_ms > 0 ? e->exit_ms : 180, overlay_exit_done, e);
        return;
    }
    overlay_detach(e);
}

static void on_scrim_pressed(GtkGestureClick* g, int n, double x, double y,
                             gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    OverlayEntry* e = (OverlayEntry*)data;
    overlay_fire_dismiss(e);
    overlay_do_close(e);
}

// aether_ui_overlay_open_impl(win_handle, content_handle, anchor, dx, dy,
// modal) -> overlay handle (1-based, 0 on failure).
int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal) {
    GtkWindow* win = aeui_resolve_window(win_handle);
    GtkWidget* content = aether_ui_get_widget(content_handle);
    if (!win || !content) return 0;
    GtkOverlay* ov = aeui_window_overlay(win);
    if (!ov) return 0;

    if (overlay_count >= overlay_capacity) {
        overlay_capacity = overlay_capacity == 0 ? 8 : overlay_capacity * 2;
        overlays = realloc(overlays, sizeof(OverlayEntry) * overlay_capacity);
    }
    OverlayEntry* e = &overlays[overlay_count++];
    e->content = content;
    e->scrim = NULL;
    e->window = win;
    e->on_dismiss = NULL;
    e->modal = modal;
    e->live = 1;
    e->exit_kind = NULL;
    e->exit_ms = 0;
    e->exiting = 0;
    // CRITICAL: this entry comes from realloc'd memory with no memset, so
    // every field has to be initialised here by hand or it reads garbage.
    e->exit_played = 0;
    e->material = NULL;   // "dim" until overlay_material sets it

    if (modal) {
        // Full-window scrim first (below the content, above the app). A
        // GtkDrawingArea with a translucent CSS background; a click gesture
        // eats the press and dismisses.
        GtkWidget* scrim = gtk_drawing_area_new();
        gtk_widget_set_hexpand(scrim, TRUE);
        gtk_widget_set_vexpand(scrim, TRUE);
        gtk_widget_add_css_class(scrim, "aui-overlay-scrim");
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(on_scrim_pressed), e);
        gtk_widget_add_controller(scrim, GTK_EVENT_CONTROLLER(click));
        gtk_overlay_add_overlay(ov, scrim);
        e->scrim = scrim;
    }

    // A modal's content is a card floating over the scrim — give it an opaque
    // themed background so the dimmed app doesn't show through it.
    if (modal) gtk_widget_add_css_class(content, "aui-overlay-card");

    gtk_widget_set_halign(content, aeui_anchor_h(anchor));
    gtk_widget_set_valign(content, aeui_anchor_v(anchor));
    gtk_widget_set_margin_start(content, dx > 0 ? dx : 0);
    gtk_widget_set_margin_end(content, dx < 0 ? -dx : 0);
    gtk_widget_set_margin_top(content, dy > 0 ? dy : 0);
    gtk_widget_set_margin_bottom(content, dy < 0 ? -dy : 0);

    /* DETACH FIRST. gtk_overlay_add_overlay refuses a widget that already has
       a parent ("Can't set new parent GtkOverlay ... which already has parent
       GtkBox") and leaves it where it was -- but this function then returned a
       non-zero handle anyway, so the DSL and /overlays both reported a live
       overlay for a widget that never moved. Three of them in
       examples/auto_hide_demo, whose sidebar and handle come out of ui.edge()
       already parented into edge's wrapper; the demo carries a retry timer
       built around the failure.

       Promoting an already-parented widget is the NORMAL case for an overlay
       built from ordinary DSL widgets -- they have to be created somewhere --
       so unparent rather than reject. A GtkBox child needs gtk_box_remove;
       gtk_widget_unparent alone leaves the box's bookkeeping stale. */
    GtkWidget* old_parent = gtk_widget_get_parent(content);
    if (old_parent) {
        g_object_ref(content);
        if (GTK_IS_BOX(old_parent))      gtk_box_remove(GTK_BOX(old_parent), content);
        else if (GTK_IS_OVERLAY(old_parent)) gtk_overlay_remove_overlay(GTK_OVERLAY(old_parent), content);
        else                              gtk_widget_unparent(content);
        gtk_overlay_add_overlay(ov, content);
        g_object_unref(content);
    } else {
        gtk_overlay_add_overlay(ov, content);
    }

    /* Report the truth: if the promotion did not take, this is not an
       overlay. Rolling the entry back keeps /overlays honest rather than
       listing a live overlay that is still sitting in its old container. */
    if (gtk_widget_get_parent(content) != GTK_WIDGET(ov)) {
        overlay_count--;
        return 0;
    }

    return overlay_count; // 1-based
}

void aether_ui_overlay_close_impl(int overlay_handle) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return;
    overlay_do_close(&overlays[overlay_handle - 1]);
}

void aether_ui_overlay_set_on_dismiss_impl(int overlay_handle,
                                           void* boxed_closure) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return;
    overlays[overlay_handle - 1].on_dismiss = (AeClosure*)boxed_closure;
}

// True if an overlay handle is still open (for the driver route + tests).
int aether_ui_overlay_is_live_impl(int overlay_handle) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return 0;
    return overlays[overlay_handle - 1].live;
}

int aether_ui_overlay_count_impl(void) { return overlay_count; }
int aether_ui_overlay_is_modal_impl(int overlay_handle) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return 0;
    return overlays[overlay_handle - 1].modal;
}

// Scrim material. GTK4 has no backdrop-filter, so a requested "blur" degrades
// to "tint"; "dim"/"" is the default. Applies the matching CSS class to the
// scrim and records the EFFECTIVE material for the driver.
void aether_ui_overlay_set_material_impl(int overlay_handle, const char* kind) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return;
    OverlayEntry* e = &overlays[overlay_handle - 1];
    const char* eff = "dim";
    if (kind && (strcmp(kind, "blur") == 0 || strcmp(kind, "tint") == 0))
        eff = "tint";   // no in-window blur on GTK4 — honest degrade
    g_free(e->material);
    e->material = g_strdup(eff);
    if (e->scrim) {
        if (strcmp(eff, "tint") == 0)
            gtk_widget_add_css_class(e->scrim, "aui-overlay-scrim-tint");
        else
            gtk_widget_remove_css_class(e->scrim, "aui-overlay-scrim-tint");
    }
}

const char* aether_ui_overlay_material_effective_impl(int overlay_handle) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return "dim";
    const char* m = overlays[overlay_handle - 1].material;
    return m ? m : "dim";
}

// Declare a per-entry enter/exit transition. Stores the kind/ms and adds the
// ENTER css class now (the content is already in the overlay, so GTK plays the
// enter keyframes immediately). Exit is played by overlay_do_close.
void aether_ui_overlay_set_transition_impl(int overlay_handle,
                                           const char* kind, int ms) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return;
    OverlayEntry* e = &overlays[overlay_handle - 1];
    g_free(e->exit_kind);
    e->exit_kind = kind ? g_strdup(kind) : NULL;
    e->exit_ms = ms;
    const char* in = aeui_tr_class(kind, 1);
    if (in && e->content && !aeui_anim_off())
        gtk_widget_add_css_class(e->content, in);
}

int aether_ui_overlay_is_exiting_impl(int overlay_handle) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return 0;
    return overlays[overlay_handle - 1].exiting;
}

int aether_ui_overlay_exit_played_impl(int overlay_handle) {
    if (overlay_handle < 1 || overlay_handle > overlay_count) return 0;
    return overlays[overlay_handle - 1].exit_played;
}

// ---------------------------------------------------------------------------
// Shortcuts (item 9) — a window-scoped GLOBAL GtkShortcutController:
// Swing InputMap WHEN_IN_FOCUSED_WINDOW semantics. Fires no matter which
// widget has focus (an entry swallows plain keys but not modified combos;
// controllers, never gestures — the sommelier focus-click lesson).
// ui.shortcut can be called during the window block, BEFORE the GTK
// window exists — registrations queue and attach at activate.
// ---------------------------------------------------------------------------
typedef struct {
    char* combo;         // as the app wrote it ("Ctrl+R")
    char* trigger_str;   // normalized GTK syntax ("<Control>r")
    AeClosure* closure;
    AeClosure* enabled;  // optional predicate |-> int (1=active); NULL = always
    int attached;
} AeuiShortcut;
static AeuiShortcut* aeui_shortcuts = NULL;
static int aeui_shortcut_count = 0, aeui_shortcut_capacity = 0;
static GtkShortcutController* aeui_shortcut_ctl = NULL;

static gboolean aeui_shortcut_activate(GtkWidget* widget, GVariant* args,
                                       gpointer data) {
    (void)widget; (void)args;
    // data is the 1-based shortcut index (stable across the array realloc,
    // unlike a struct pointer). A conditional shortcut whose `enabled`
    // predicate returns 0 reports UNHANDLED so the key propagates normally.
    int idx = GPOINTER_TO_INT(data) - 1;
    if (idx < 0 || idx >= aeui_shortcut_count) return FALSE;
    AeuiShortcut* s = &aeui_shortcuts[idx];
    if (s->enabled && s->enabled->fn) {
        int on = ((int(*)(void*))s->enabled->fn)(s->enabled->env);
        if (!on) return FALSE;   // scope inactive → not handled here
    }
    AeClosure* c = s->closure;
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
    return TRUE;
}

// "Ctrl+R" → "<Control>r". GTK's own syntax ("<Control>r") passes
// through untouched. Named keys (Escape, Delete, F5…) keep their case.
static char* aeui_normalize_combo(const char* combo) {
    if (!combo) return strdup("");
    if (combo[0] == '<') return strdup(combo);
    char mods[128] = "";
    char key[64] = "";
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", combo);
    char* save = NULL;
    for (char* tok = strtok_r(tmp, "+", &save); tok;
         tok = strtok_r(NULL, "+", &save)) {
        if (!g_ascii_strcasecmp(tok, "Ctrl") || !g_ascii_strcasecmp(tok, "Control")) {
            strcat(mods, "<Control>");
        } else if (!g_ascii_strcasecmp(tok, "Shift")) {
            strcat(mods, "<Shift>");
        } else if (!g_ascii_strcasecmp(tok, "Alt")) {
            strcat(mods, "<Alt>");
        } else if (!g_ascii_strcasecmp(tok, "Super") ||
                   !g_ascii_strcasecmp(tok, "Cmd") ||
                   !g_ascii_strcasecmp(tok, "Meta")) {
            strcat(mods, "<Super>");
        } else {
            snprintf(key, sizeof(key), "%s", tok);
        }
    }
    if (strlen(key) == 1 && key[0] >= 'A' && key[0] <= 'Z') key[0] += 32;
    char out[256];
    snprintf(out, sizeof(out), "%s%s", mods, key);
    return strdup(out);
}

static void aeui_attach_shortcut(AeuiShortcut* s) {
    if (!primary_window || s->attached) return;
    if (!aeui_shortcut_ctl) {
        aeui_shortcut_ctl = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
        gtk_shortcut_controller_set_scope(aeui_shortcut_ctl,
                                          GTK_SHORTCUT_SCOPE_GLOBAL);
        gtk_widget_add_controller(GTK_WIDGET(primary_window),
                                  GTK_EVENT_CONTROLLER(aeui_shortcut_ctl));
    }
    GtkShortcutTrigger* trig =
        gtk_shortcut_trigger_parse_string(s->trigger_str);
    if (!trig) {
        fprintf(stderr, "[aeui-shortcut] unparseable combo '%s' ('%s')\n",
                s->combo, s->trigger_str);
        return;
    }
    // Pass the 1-based index (stable across realloc) as the callback data.
    int one_based = (int)(s - aeui_shortcuts) + 1;
    gtk_shortcut_controller_add_shortcut(aeui_shortcut_ctl,
        gtk_shortcut_new(trig,
            gtk_callback_action_new(aeui_shortcut_activate,
                                    GINT_TO_POINTER(one_based), NULL)));
    s->attached = 1;
}

// The any-key handler (ui.window_on_key): what was pressed, rather than "was
// THIS combo pressed". Registered in the BUBBLE phase so the shortcut
// controller (capture phase) still wins, and it returns FALSE so the key
// carries on to whatever has focus.
/* A LIST, not one slot. Every registration used to overwrite the last, so a
   second window_on_key silently disabled the first -- and ui.on_key() is built
   on this, which means two on_key() calls on different widgets left only the
   later one working, with no error anywhere. The listbox hit exactly that:
   registering a handler per row left only the final row's arrows alive.
   Handlers fire in registration order; "handled" stays "at least one ran", so
   the return value means what it did before. */
static AeClosure** aeui_window_key_closures = NULL;
static int aeui_window_key_closure_count = 0;
static int aeui_window_key_closure_cap = 0;

static void aeui_window_key_closure_add(AeClosure* c) {
    if (!c) return;
    if (aeui_window_key_closure_count >= aeui_window_key_closure_cap) {
        aeui_window_key_closure_cap = aeui_window_key_closure_cap ? aeui_window_key_closure_cap * 2 : 8;
        aeui_window_key_closures = realloc(aeui_window_key_closures, sizeof(AeClosure*) * aeui_window_key_closure_cap);
    }
    aeui_window_key_closures[aeui_window_key_closure_count++] = c;
}

static int aeui_window_key_closure_fire(const char* key_name, int mods) {
    if (!key_name) return 0;
    int fired = 0;
    for (int i = 0; i < aeui_window_key_closure_count; i++) {
        AeClosure* c = aeui_window_key_closures[i];
        if (!c || !c->fn) continue;
        ((void(*)(void*, const char*, int))c->fn)(c->env, key_name, mods);
        fired = 1;
    }
    return fired;
}

int aether_ui_window_key_deliver(const char* key_name, int mods) {
    return aeui_window_key_closure_fire(key_name, mods);
}

static gboolean on_window_any_key(GtkEventControllerKey* ctl, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer data) {
    (void)ctl; (void)keycode; (void)data;
    if (aeui_window_key_closure_count == 0) return FALSE;
    const char* name = gdk_keyval_name(keyval);
    int mods = 0;
    if (state & GDK_SHIFT_MASK)   mods |= 1;
    if (state & GDK_CONTROL_MASK) mods |= 2;
    if (state & GDK_ALT_MASK)     mods |= 4;
    if (state & GDK_SUPER_MASK)   mods |= 8;
    aether_ui_window_key_deliver(name ? name : "", mods);
    return FALSE;   // never swallow: the focused widget still gets the key
}

// Files dropped on the window. A GtkDropTarget over GDK_TYPE_FILE_LIST on the
// window itself, so a drop anywhere in it counts rather than only over one
// widget.
static AeClosure* aeui_file_drop_closure = NULL;

int aether_ui_window_file_drop_deliver(const char* paths) {
    if (!aeui_file_drop_closure || !aeui_file_drop_closure->fn) return 0;
    if (!paths) return 1;   // presence probe
    ((void(*)(void*, const char*))aeui_file_drop_closure->fn)(
        aeui_file_drop_closure->env, paths);
    return 1;
}

static gboolean on_window_file_drop(GtkDropTarget* target, const GValue* value,
                                    double x, double y, gpointer data) {
    (void)target; (void)x; (void)y; (void)data;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
    GSList* files = g_value_get_boxed(value);
    GString* joined = g_string_new(NULL);
    for (GSList* l = files; l; l = l->next) {
        char* path = g_file_get_path(G_FILE(l->data));
        if (!path) continue;   // non-local (smb://, trash://): no local path
        // CRITICAL: a NEWLINE is legal in a filename, and this ABI separates
        // paths with one. Delivering such a path would split it into two
        // strings, NEITHER of which names a file that exists, and the app
        // would act on both. For a file manager that could mean operating on
        // the wrong path entirely, so the file is dropped from the batch
        // instead: fewer paths, all of them real.
        if (strchr(path, '\n')) { g_free(path); continue; }
        if (joined->len) g_string_append_c(joined, '\n');
        g_string_append(joined, path);
        g_free(path);
    }
    int ok = joined->len ? aether_ui_window_file_drop_deliver(joined->str) : 0;
    g_string_free(joined, TRUE);
    return ok ? TRUE : FALSE;
}

void aether_ui_window_on_file_drop_impl(void* boxed_closure) {
    aeui_file_drop_closure = (AeClosure*)boxed_closure;
    if (!primary_window) return;   // attaches when the window appears
    static int drop_attached = 0;
    if (drop_attached) return;
    GtkDropTarget* tgt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(tgt, "drop", G_CALLBACK(on_window_file_drop), NULL);
    gtk_widget_add_controller(GTK_WIDGET(primary_window),
                              GTK_EVENT_CONTROLLER(tgt));
    drop_attached = 1;
}

void aether_ui_window_on_key_impl(void* boxed_closure) {
    aeui_window_key_closure_add((AeClosure*)boxed_closure);
    if (!primary_window) return;   // attaches when the window appears
    static int attached = 0;
    if (attached) return;
    GtkEventController* key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_BUBBLE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_window_any_key), NULL);
    gtk_widget_add_controller(GTK_WIDGET(primary_window), key);
    attached = 1;
}

// Split a normalized trigger ("<Control>a") into modifier bits and bare key,
// so the driver path hands the closure what a real keypress would.
static int aeui_trigger_split(const char* trig, char* name, int namesize) {
    int mods = 0;
    const char* p = trig ? trig : "";
    while (*p == '<') {
        const char* end = strchr(p, '>');
        if (!end) break;
        size_t n = (size_t)(end - p - 1);
        if      (n == 7 && strncmp(p + 1, "Control", 7) == 0) mods |= 2;
        else if (n == 5 && strncmp(p + 1, "Shift",   5) == 0) mods |= 1;
        else if (n == 3 && strncmp(p + 1, "Alt",     3) == 0) mods |= 4;
        else if (n == 5 && strncmp(p + 1, "Super",   5) == 0) mods |= 8;
        p = end + 1;
    }
    snprintf(name, namesize, "%s", p);
    return mods;
}

void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) {
    aether_ui_shortcut_when_impl(combo, boxed_closure, NULL);
}

// ── Chorded shortcuts (two-key sequences, "Ctrl+K Ctrl+S") ──────────
// Emacs/VSCode style: a prefix combo arms a pending state; the next combo,
// within a short timeout, completes the chord. Modelled as two normal
// shortcuts sharing a small state machine.
typedef struct {
    char* prefix_trig;   // normalized first combo
    char* second_trig;   // normalized second combo
    AeClosure* closure;
} AeuiChord;
static AeuiChord* aeui_chords = NULL;
static int aeui_chord_count = 0, aeui_chord_capacity = 0;
static char* aeui_chord_pending = NULL;   // normalized prefix currently armed
static guint aeui_chord_timeout_id = 0;

static gboolean aeui_chord_timeout(gpointer data) {
    (void)data;
    if (aeui_chord_pending) { free(aeui_chord_pending); aeui_chord_pending = NULL; }
    aeui_chord_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

// Called from the key path with a normalized trigger. Returns 1 if it armed a
// prefix or completed a chord (i.e. the key was consumed by the chord layer).
static int aeui_chord_feed(const char* norm_trig) {
    // If a prefix is armed, try to complete.
    if (aeui_chord_pending) {
        for (int i = 0; i < aeui_chord_count; i++) {
            if (strcmp(aeui_chords[i].prefix_trig, aeui_chord_pending) == 0 &&
                strcmp(aeui_chords[i].second_trig, norm_trig) == 0) {
                AeClosure* c = aeui_chords[i].closure;
                free(aeui_chord_pending); aeui_chord_pending = NULL;
                if (aeui_chord_timeout_id) { g_source_remove(aeui_chord_timeout_id); aeui_chord_timeout_id = 0; }
                if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
                return 1;
            }
        }
        // Armed but this key isn't a valid completion — cancel the prefix and
        // let the key fall through as a normal shortcut.
        free(aeui_chord_pending); aeui_chord_pending = NULL;
        if (aeui_chord_timeout_id) { g_source_remove(aeui_chord_timeout_id); aeui_chord_timeout_id = 0; }
    }
    // Not armed: does this key start a chord?
    for (int i = 0; i < aeui_chord_count; i++) {
        if (strcmp(aeui_chords[i].prefix_trig, norm_trig) == 0) {
            aeui_chord_pending = strdup(norm_trig);
            aeui_chord_timeout_id = g_timeout_add(1500, aeui_chord_timeout, NULL);
            return 1;
        }
    }
    return 0;
}

// A chord key's controller action: feed the normalized trigger (passed as the
// action data) into the state machine. Returns handled iff the chord layer
// consumed it (armed or completed).
static gboolean aeui_chord_key_activate(GtkWidget* widget, GVariant* args,
                                        gpointer data) {
    (void)widget; (void)args;
    const char* trig = (const char*)data;
    return aeui_chord_feed(trig) ? TRUE : FALSE;
}

static void aeui_register_chord_key(const char* norm_trig) {
    if (!aeui_shortcut_ctl || !primary_window) return;  // attaches at activate
    GtkShortcutTrigger* trig = gtk_shortcut_trigger_parse_string(norm_trig);
    if (!trig) return;
    gtk_shortcut_controller_add_shortcut(aeui_shortcut_ctl,
        gtk_shortcut_new(trig,
            gtk_callback_action_new(aeui_chord_key_activate,
                                    (gpointer)norm_trig, NULL)));
}

void aether_ui_shortcut_chord_impl(const char* first_combo,
                                   const char* second_combo,
                                   void* boxed_closure) {
    if (aeui_chord_count >= aeui_chord_capacity) {
        aeui_chord_capacity = aeui_chord_capacity == 0 ? 8 : aeui_chord_capacity * 2;
        aeui_chords = realloc(aeui_chords, sizeof(AeuiChord) * aeui_chord_capacity);
    }
    AeuiChord* ch = &aeui_chords[aeui_chord_count++];
    ch->prefix_trig = aeui_normalize_combo(first_combo);
    ch->second_trig = aeui_normalize_combo(second_combo);
    ch->closure = (AeClosure*)boxed_closure;
    // Register both combos as controller shortcuts so GTK actually delivers
    // the keys to us; their actions route into the chord state machine.
    aeui_register_chord_key(ch->prefix_trig);
    aeui_register_chord_key(ch->second_trig);
}

// Conditional shortcut: `enabled_closure` (|-> int) gates whether the combo
// fires. Returns 0 → the key propagates as if no shortcut were bound (scope
// inactive). NULL predicate = always active (plain ui.shortcut).
void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure,
                                  void* enabled_closure) {
    if (aeui_shortcut_count >= aeui_shortcut_capacity) {
        aeui_shortcut_capacity = aeui_shortcut_capacity == 0
            ? 16 : aeui_shortcut_capacity * 2;
        aeui_shortcuts = realloc(aeui_shortcuts,
                                 sizeof(AeuiShortcut) * aeui_shortcut_capacity);
    }
    AeuiShortcut* s = &aeui_shortcuts[aeui_shortcut_count++];
    s->combo = strdup(combo ? combo : "");
    s->trigger_str = aeui_normalize_combo(combo);
    s->closure = (AeClosure*)boxed_closure;
    s->enabled = (AeClosure*)enabled_closure;
    s->attached = 0;
    aeui_attach_shortcut(s);   // no-op before the window exists
}

// Escape dismisses the TOPMOST live overlay (runs its on_dismiss path via
// overlay_close). Registered as an internal shortcut at activate; when no
// overlay is open it reports unhandled so Escape propagates normally.
static gboolean aeui_escape_overlays(GtkWidget* widget, GVariant* args,
                                     gpointer data) {
    (void)widget; (void)args; (void)data;
    for (int i = overlay_count - 1; i >= 0; i--) {
        if (overlays[i].live) {
            aether_ui_overlay_close_impl(i + 1);
            return TRUE;
        }
    }
    return FALSE;
}

// Called from on_activate once primary_window exists.
static void aeui_register_chord_key(const char* norm_trig);

static void aeui_attach_pending_shortcuts(void) {
    for (int i = 0; i < aeui_shortcut_count; i++) {
        aeui_attach_shortcut(&aeui_shortcuts[i]);
    }
    if (!aeui_shortcut_ctl) {
        aeui_shortcut_ctl = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
        gtk_shortcut_controller_set_scope(aeui_shortcut_ctl,
                                          GTK_SHORTCUT_SCOPE_GLOBAL);
        gtk_widget_add_controller(GTK_WIDGET(primary_window),
                                  GTK_EVENT_CONTROLLER(aeui_shortcut_ctl));
    }
    // Now the controller exists, wire any chord keys registered pre-window.
    for (int i = 0; i < aeui_chord_count; i++) {
        aeui_register_chord_key(aeui_chords[i].prefix_trig);
        aeui_register_chord_key(aeui_chords[i].second_trig);
    }
    gtk_shortcut_controller_add_shortcut(aeui_shortcut_ctl,
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("Escape"),
            gtk_callback_action_new(aeui_escape_overlays, NULL, NULL)));
}

// The driver's /window/key: activate the registered shortcut whose
// normalized trigger matches — the SAME closure the controller would run;
// no fake input events, no seat/keymap dependency (works headless).
// Tab / Shift+Tab move focus through the real GTK focus chain. Escape
// runs the overlay-dismiss path. Returns 1 if something handled it.
int aeui_window_key_fire(const char* combo) {
    char* want = aeui_normalize_combo(combo);
    // Chord layer first: a prefix key arms, a completion key fires the chord.
    // If the chord machine consumed it (armed or completed), we're done.
    if (aeui_chord_feed(want) == 1) { free(want); return 1; }
    int fired = 0;
    for (int i = 0; i < aeui_shortcut_count; i++) {
        if (strcmp(aeui_shortcuts[i].trigger_str, want) == 0) {
            AeuiShortcut* s = &aeui_shortcuts[i];
            // Honour the conditional predicate on the driver path too.
            if (s->enabled && s->enabled->fn) {
                if (!((int(*)(void*))s->enabled->fn)(s->enabled->env)) continue;
            }
            AeClosure* c = s->closure;
            if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
            fired = 1;
        }
    }
    if (!fired && primary_window) {
        if (strcmp(want, "Tab") == 0) {
            fired = gtk_widget_child_focus(GTK_WIDGET(primary_window),
                                           GTK_DIR_TAB_FORWARD);
        } else if (strcmp(want, "<Shift>Tab") == 0) {
            fired = gtk_widget_child_focus(GTK_WIDGET(primary_window),
                                           GTK_DIR_TAB_BACKWARD);
        } else if (strcmp(want, "Escape") == 0) {
            fired = aeui_escape_overlays(NULL, NULL, NULL);
        }
    }
    if (!fired) {
        // Nothing bound wanted it: the same fall-through a real key takes.
        char kname[64];
        int kmods = aeui_trigger_split(want, kname, sizeof(kname));
        fired = aether_ui_window_key_deliver(kname, kmods);
    }
    free(want);
    return fired;
}

// ui.focus(handle) — explicit focus. The default Tab order is GTK's
// focus chain over the build order; this is the override for when a
// flow needs it (dialog primary field etc.).
void aether_ui_set_focusable_impl(int handle, int on) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_focusable(w, on ? TRUE : FALSE);
}

void aether_ui_focus_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_grab_focus(w);
}

// Toast — a self-contained styled label opened as a bottom-center overlay,
// auto-dismissed after `ms` via a one-shot timer. Returns the overlay handle.
// (A convenience over overlay_open; the chrome lives in the .aui-toast CSS.)
typedef struct { int overlay_handle; } ToastTimer;

static gboolean on_toast_expire(gpointer data) {
    ToastTimer* t = (ToastTimer*)data;
    aether_ui_overlay_close_impl(t->overlay_handle);
    free(t);
    return G_SOURCE_REMOVE;
}

int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    GtkWindow* win = aeui_resolve_window(win_handle);
    if (!win) return 0;
    GtkWidget* lbl = gtk_label_new(text ? text : "");
    gtk_widget_add_css_class(lbl, "aui-toast");
    int content = aether_ui_register_widget(lbl);
    // Bottom-center (anchor v=bottom(2)<<2 | h=center(1) = 9), margin handled
    // by the .aui-toast CSS.
    int h = aether_ui_overlay_open_impl(win_handle, content, 9, 0, 0, 0);
    if (h > 0 && ms > 0) {
        ToastTimer* t = malloc(sizeof(ToastTimer));
        t->overlay_handle = h;
        g_timeout_add((guint)ms, on_toast_expire, t);
    }
    return h;
}

// Drawn tooltip for vg scenes — a styled label overlay near the pointer,
// replacing the native GtkTooltip (a compositor surface, invisible under
// sommelier). One reused overlay per process: show updates the text+position,
// hide closes it. Coords are CANVAS-LOCAL px; we translate to window-local via
// the canvas widget's bounds so the label sits at the pointer.
static int drawn_tooltip_overlay = 0;   // live overlay handle, or 0
static GtkWidget* drawn_tooltip_label = NULL;
void aether_ui_vg_tooltip_hide_impl(void);   // fwd

int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text,
                                   double cx, double cy) {
    if (!text || !text[0]) { aether_ui_vg_tooltip_hide_impl(); return 0; }
    GtkWidget* canvas = aether_ui_get_widget(
        aether_ui_canvas_get_widget(canvas_id));
    if (!canvas) return 0;
    // Canvas-local → window-local px.
    GtkRoot* root = gtk_widget_get_root(canvas);
    if (!root || !GTK_IS_WINDOW(root)) return 0;
    graphene_rect_t r;
    double wx = cx, wy = cy;
    if (gtk_widget_compute_bounds(canvas, GTK_WIDGET(root), &r)) {
        wx = r.origin.x + cx;
        wy = r.origin.y + cy;
    }
    // Offset a touch below-right of the pointer, like a native tooltip.
    int mx = (int)wx + 12;
    int my = (int)wy + 18;

    if (drawn_tooltip_overlay && aether_ui_overlay_is_live_impl(drawn_tooltip_overlay)
        && drawn_tooltip_label) {
        // Reuse: just update text + margins (top-start anchor).
        gtk_label_set_text(GTK_LABEL(drawn_tooltip_label), text);
        gtk_widget_set_margin_start(drawn_tooltip_label, mx);
        gtk_widget_set_margin_top(drawn_tooltip_label, my);
        return drawn_tooltip_overlay;
    }
    // Fresh.
    GtkWidget* lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "aui-tooltip");
    drawn_tooltip_label = lbl;
    int content = aether_ui_register_widget(lbl);
    // anchor top-start = h:start(0) + v:top(0) = 0.
    drawn_tooltip_overlay = aether_ui_overlay_open_impl(0, content, 0, mx, my, 0);
    return drawn_tooltip_overlay;
}

void aether_ui_vg_tooltip_hide_impl(void) {
    if (drawn_tooltip_overlay) {
        aether_ui_overlay_close_impl(drawn_tooltip_overlay);
        drawn_tooltip_overlay = 0;
        drawn_tooltip_label = NULL;
    }
}

// Whether the drawn-tooltip path is active: forced by $AETHER_UI_TOOLTIP,
// else on under sommelier (native tooltips don't display there).
int aether_ui_vg_tooltip_drawn_impl(void) {
    const char* force = getenv("AETHER_UI_TOOLTIP");
    if (force && strcmp(force, "drawn") == 0) return 1;
    if (force && strcmp(force, "native") == 0) return 0;
    return getenv("SOMMELIER_VERSION") != NULL;
}

// Multi-window support. Two numbering spaces coexist:
//   - The OVERLAY win_handle: 0 = primary, N = extra_windows[N-1]. Historical;
//     aeui_resolve_window keeps it (overlays/menus were written against it).
//   - The DRIVER window handle: 1 = primary, 2.. = extras. Unified so /windows
//     and the "window":N widget field number every window consistently.
typedef struct {
    GtkWindow* window;
    char*      title;
    int        root_handle;
    int        live;
} WindowEntry;

static WindowEntry* extra_windows = NULL;
static int extra_window_count = 0;
static int extra_window_capacity = 0;

// The primary window's title/live, tracked so the unified driver view (handle
// 1) can report them alongside the extras.
static char* primary_title = NULL;
static int   primary_live = 0;

// The application is HELD once per live window and released when each closes,
// so the loop stays alive exactly while ≥1 window is live — deterministic
// regardless of present-vs-realize (headless windows aren't "shown", which
// otherwise confuses GTK's implicit last-window-close exit).
static GApplication* aeui_held_app(void) {
    return (app_count > 0 && apps[0].gtk_app) ? G_APPLICATION(apps[0].gtk_app)
                                              : NULL;
}

static void unregister_widget_tree(GtkWidget* w);

static void on_extra_window_destroyed(GtkWindow* w, gpointer data) {
    (void)data;
    /* UNREGISTER the window's widgets FIRST, and it must be first: `widgets`
       holds RAW, unreffed GtkWidget pointers, and destroying the window
       finalizes its whole child tree. Leaving the registry populated is worse
       than a leak -- every handle inside a closed window becomes a dangling
       pointer the next driver or app call dereferences. The signal runs at the
       start of disposal, so the children are still walkable here; after it
       there is no tree left to walk. This handler serves the primary window
       too, and covers a close from the title bar as well as window_close(). */
    GtkWidget* child = gtk_window_get_child(w);
    if (child) unregister_widget_tree(child);

    int was_live = 0;
    if (primary_window == w) { was_live = primary_live; primary_live = 0; }
    else {
        for (int i = 0; i < extra_window_count; i++)
            if (extra_windows[i].window == w) {
                was_live = extra_windows[i].live; extra_windows[i].live = 0; break;
            }
    }
    GApplication* app = aeui_held_app();
    if (was_live && app) g_application_release(app);  // may exit the loop at 0
}

static void aeui_register_primary_window(GtkWindow* w, const char* title) {
    primary_title = strdup(title ? title : "");
    primary_live = 1;
    g_signal_connect(w, "destroy", G_CALLBACK(on_extra_window_destroyed), NULL);
    GApplication* app = aeui_held_app();
    if (app) g_application_hold(app);
}

// Resolve an overlay window handle: 0 = the primary app window; >0 indexes
// the extra_windows table (1-based).
static GtkWindow* aeui_resolve_window(int win_handle) {
    if (win_handle == 0) return primary_window;
    if (win_handle < 1 || win_handle > extra_window_count) return NULL;
    return extra_windows[win_handle - 1].window;
}

int aether_ui_window_create_impl(const char* title, int width, int height) {
    ensure_gtk_init();
    if (extra_window_count >= extra_window_capacity) {
        extra_window_capacity = extra_window_capacity == 0 ? 8 : extra_window_capacity * 2;
        extra_windows = realloc(extra_windows, sizeof(WindowEntry) * extra_window_capacity);
    }
    // Attach to the RUNNING application so the window shares the loop's
    // lifecycle (keeps the app alive while open; loop exits when the last
    // window closes). A bare gtk_window_new() is invisible to the app and
    // wouldn't hold it open.
    GtkApplication* app = (app_count > 0) ? apps[0].gtk_app : NULL;
    GtkWidget* win = app ? gtk_application_window_new(app) : gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title ? title : "");
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
    WindowEntry* e = &extra_windows[extra_window_count++];
    e->window = GTK_WINDOW(win);
    e->title = strdup(title ? title : "");
    e->root_handle = 0;
    e->live = 1;
    g_signal_connect(win, "destroy",
                     G_CALLBACK(on_extra_window_destroyed), NULL);
    GApplication* gapp = aeui_held_app();
    if (gapp) g_application_hold(gapp);  // released when this window closes
    return extra_window_count; // 1-based (overlay space)
}

void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    if (win_handle < 1 || win_handle > extra_window_count) return;
    WindowEntry* e = &extra_windows[win_handle - 1];
    e->root_handle = root_handle;
    GtkWidget* root = aether_ui_get_widget(root_handle);
    if (root) gtk_window_set_child(e->window, root);
}

void aether_ui_window_show_impl(int win_handle) {
    if (win_handle < 1 || win_handle > extra_window_count) return;
    const char* headless = getenv("AETHER_UI_HEADLESS");
    if (headless && headless[0] && headless[0] != '0') {
        gtk_widget_realize(GTK_WIDGET(extra_windows[win_handle - 1].window));
    } else {
        gtk_window_present(extra_windows[win_handle - 1].window);
    }
}

// ── Unified driver window view (handle 1 = primary, 2.. = extras) ──
int aether_ui_window_count_impl(void) { return 1 + extra_window_count; }

const char* aether_ui_window_title_impl(int win_handle) {
    if (win_handle == 1) return primary_title ? primary_title : "";
    int idx = win_handle - 2;
    if (idx < 0 || idx >= extra_window_count) return "";
    return extra_windows[idx].title ? extra_windows[idx].title : "";
}

void aether_ui_window_set_title_impl(int win_handle, const char* title) {
    const char* t = title ? title : "";
    GtkWindow* w = NULL;
    char** slot = NULL;
    if (win_handle == 1) {
        w = primary_window;
        slot = &primary_title;
    } else {
        int idx = win_handle - 2;
        if (idx < 0 || idx >= extra_window_count) return;
        w = extra_windows[idx].window;
        slot = &extra_windows[idx].title;
    }
    /* The cached copy is what the driver reports, so it is updated even when
     * the GtkWindow is not up yet (retitle before show). */
    char* copy = strdup(t);
    if (!copy) return;
    free(*slot);
    *slot = copy;
    if (w) gtk_window_set_title(w, t);
}

int aether_ui_window_is_open_impl(int win_handle) {
    if (win_handle == 1) return primary_live;
    int idx = win_handle - 2;
    if (idx < 0 || idx >= extra_window_count) return 0;
    return extra_windows[idx].live;
}

// The DRIVER window handle a widget currently lives in (top-level ancestor), or
// 0 if unmounted. 1 = primary, 2.. = extras.
int aether_ui_widget_window_impl(int widget_handle) {
    GtkWidget* w = aether_ui_get_widget(widget_handle);
    if (!w) return 0;
    GtkRoot* root = gtk_widget_get_root(w);
    if (!root || !GTK_IS_WINDOW(root)) return 0;
    GtkWindow* gw = GTK_WINDOW(root);
    if (gw == primary_window) return 1;
    for (int i = 0; i < extra_window_count; i++)
        if (extra_windows[i].window == gw) return i + 2;
    return 0;
}

// close_window by the DRIVER handle (1 = primary, 2.. = extras).
void aether_ui_close_window_by_handle_impl(int win_handle) {
    GtkWindow* gw = NULL;
    if (win_handle == 1) gw = primary_window;
    else if (win_handle - 2 >= 0 && win_handle - 2 < extra_window_count)
        gw = extra_windows[win_handle - 2].window;
    if (gw) gtk_window_destroy(gw);
}

void aether_ui_window_close_impl(int win_handle) {
    if (win_handle < 1 || win_handle > extra_window_count) return;
    gtk_window_close(extra_windows[win_handle - 1].window);
}

// Sheet (modal dialog window)
int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    ensure_gtk_init();
    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title ? title : "");
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
    /* Same destroy hook the secondary windows get, so dismissing a sheet
       unregisters the widgets inside it. Without it the registry keeps raw,
       unreffed pointers to children GTK has already finalized. The handler
       looks the window up in extra_windows, does not find a sheet there, and
       so touches no live flag -- it only does the unregistering. */
    g_signal_connect(win, "destroy",
                     G_CALLBACK(on_extra_window_destroyed), NULL);
    return aether_ui_register_widget(win);
}

void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    GtkWidget* win = aether_ui_get_widget(handle);
    GtkWidget* root = aether_ui_get_widget(root_handle);
    if (win && root && GTK_IS_WINDOW(win)) {
        gtk_window_set_child(GTK_WINDOW(win), root);
    }
}

void aether_ui_sheet_present_impl(int handle) {
    GtkWidget* win = aether_ui_get_widget(handle);
    if (!win || !GTK_IS_WINDOW(win)) return;
    /* Honour AETHER_UI_HEADLESS, exactly as window_show_impl does forty lines
       up. This called gtk_window_present unconditionally, so a headless run
       put a real sheet on the desktop -- on the backend CI uses for Linux, and
       against a contract tests/test_widgets.c states in its own header and
       names sheet_present in. Realize without mapping, so the widgets inside
       are laid out and reachable but nothing appears. */
    const char* headless = getenv("AETHER_UI_HEADLESS");
    if (headless && headless[0] && headless[0] != '0') {
        gtk_widget_realize(win);
        return;
    }
    gtk_window_present(GTK_WINDOW(win));
}

void aether_ui_sheet_dismiss_impl(int handle) {
    GtkWidget* win = aether_ui_get_widget(handle);
    if (win && GTK_IS_WINDOW(win)) {
        gtk_window_close(GTK_WINDOW(win));
    }
}

// Image widget
// GtkPicture, not GtkImage, for anything that shows a PICTURE: GtkImage is an
// icon widget and always scales down keeping aspect, so cover and stretch are
// not expressible on it. GtkPicture has content-fit, which is exactly the four
// modes. file_icon stays on GtkImage because a themed GIcon is an icon, and
// only GtkImage can show one.
int aether_ui_image_create(const char* filepath) {
    ensure_gtk_init();
    GtkWidget* img;
    if (filepath && *filepath) {
        img = gtk_picture_new_for_filename(filepath);
    } else {
        img = gtk_picture_new();
    }
    gtk_picture_set_can_shrink(GTK_PICTURE(img), TRUE);
    // Explicit, so both backends agree on what an untouched image does.
    gtk_picture_set_content_fit(GTK_PICTURE(img), GTK_CONTENT_FIT_CONTAIN);
    return aether_ui_register_widget(img);
}

void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    // A symbolic GIcon paints itself in the widget's CSS colour, which is
    // exactly a tint. A GtkPicture holds a texture and has no such notion,
    // so tinting one is not expressible and is reported as not applied.
    if (!GTK_IS_IMAGE(w)) return;
    if (!on) {
        g_object_set_data(G_OBJECT(w), "aeui-img-tint", NULL);
        aether_ui_apply_css(handle, w, "color: unset;");
        return;
    }
    char prop[128];
    snprintf(prop, sizeof(prop), "color: rgb(%d,%d,%d);",
             (int)(r * 255), (int)(g * 255), (int)(b * 255));
    aether_ui_apply_css(handle, w, prop);
    g_object_set_data(G_OBJECT(w), "aeui-img-tint",
        GINT_TO_POINTER((((int)(r * 255) & 255) << 16) |
                        (((int)(g * 255) & 255) << 8) |
                        ((int)(b * 255) & 255) | 0x1000000));
}

int aether_ui_image_get_tint(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_IMAGE(w)) return -1;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-img-tint"));
    return (v & 0x1000000) ? v : -1;
}

void aether_ui_image_set_fill(int handle, int mode) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_PICTURE(w)) return;
    GtkContentFit fit = GTK_CONTENT_FIT_SCALE_DOWN;
    if (mode == 1)      fit = GTK_CONTENT_FIT_CONTAIN;
    else if (mode == 2) fit = GTK_CONTENT_FIT_COVER;
    else if (mode == 3) fit = GTK_CONTENT_FIT_FILL;
    gtk_picture_set_content_fit(GTK_PICTURE(w), fit);
}

int aether_ui_image_get_fill(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_PICTURE(w)) return 0;
    switch (gtk_picture_get_content_fit(GTK_PICTURE(w))) {
        case GTK_CONTENT_FIT_CONTAIN: return 1;
        case GTK_CONTENT_FIT_COVER:   return 2;
        case GTK_CONTENT_FIT_FILL:    return 3;
        default:                      return 0;
    }
}

// Decode encoded image bytes into an image widget (no temp file). GTK4's
// gdk_texture_new_from_bytes handles PNG/JPEG/etc. via the pixbuf loaders;
// the resulting GdkTexture is a GdkPaintable a GtkImage displays directly.
int aether_ui_image_from_bytes(const char* data, int length) {
    ensure_gtk_init();
    GtkWidget* img = gtk_picture_new();
    gtk_picture_set_can_shrink(GTK_PICTURE(img), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(img), GTK_CONTENT_FIT_CONTAIN);
    if (data && length > 0) {
        GBytes* bytes = g_bytes_new(data, (gsize)length);
        GError* err = NULL;
        GdkTexture* tex = gdk_texture_new_from_bytes(bytes, &err);
        g_bytes_unref(bytes);
        if (tex) {
            gtk_picture_set_paintable(GTK_PICTURE(img), GDK_PAINTABLE(tex));
            g_object_unref(tex);
        } else if (err) {
            g_clear_error(&err);   // stays an empty picture on decode failure
        }
    }
    return aether_ui_register_widget(img);
}

// The themed icon for a path. Query the file when it exists (so a directory
// gets the folder icon rather than whatever its name suggests) and fall back
// to guessing from the name when it does not, which is what a listing needs
// before it has stat'ed an entry.
static GIcon* aeui_gicon_for_path(const char* path) {
    if (!path || !*path) return NULL;
    GFile* f = g_file_new_for_path(path);
    GFileInfo* info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_ICON,
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    GIcon* icon = NULL;
    if (info) {
        GIcon* got = g_file_info_get_icon(info);
        if (got) icon = g_object_ref(got);
        g_object_unref(info);
    }
    g_object_unref(f);
    if (!icon) {
        gboolean uncertain = FALSE;
        char* ct = g_content_type_guess(path, NULL, 0, &uncertain);
        if (ct) { icon = g_content_type_get_icon(ct); g_free(ct); }
    }
    return icon;
}

int aether_ui_file_icon_create(const char* path) {
    ensure_gtk_init();
    GtkWidget* img = gtk_image_new();
    GIcon* icon = aeui_gicon_for_path(path);
    if (icon) {
        gtk_image_set_from_gicon(GTK_IMAGE(img), icon);
        g_object_unref(icon);
    }
    return aether_ui_register_widget(img);
}

void aether_ui_file_icon_set(int handle, const char* path) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_IMAGE(w)) return;
    GIcon* icon = aeui_gicon_for_path(path);
    if (icon) {
        gtk_image_set_from_gicon(GTK_IMAGE(w), icon);
        g_object_unref(icon);
    } else {
        gtk_image_clear(GTK_IMAGE(w));
    }
}

int aether_ui_image_has_content(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    if (GTK_IS_PICTURE(w)) {
        return gtk_picture_get_paintable(GTK_PICTURE(w)) != NULL ? 1 : 0;
    }
    if (GTK_IS_IMAGE(w)) {
        return gtk_image_get_storage_type(GTK_IMAGE(w)) != GTK_IMAGE_EMPTY ? 1 : 0;
    }
    return 0;
}

void aether_ui_image_set_size(int handle, int width, int height) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    if (GTK_IS_IMAGE(w)) {
        // An icon sizes by pixel-size; the request alone leaves it tiny.
        gtk_image_set_pixel_size(GTK_IMAGE(w), width > height ? width : height);
    }
    gtk_widget_set_size_request(w, width, height);
}

// ---------------------------------------------------------------------------
// Canvas drawing + Events + Animation (Group 6)
// ---------------------------------------------------------------------------

// Canvas — command-buffer drawing via Cairo on a GtkDrawingArea.

typedef enum {
    CANVAS_BEGIN_PATH,
    CANVAS_MOVE_TO,
    CANVAS_LINE_TO,
    CANVAS_STROKE,
    CANVAS_FILL_RECT,
    CANVAS_CLEAR,
    CANVAS_ARC,         // add a circle/arc to the current path
    CANVAS_CLOSE_PATH,  // close the current sub-path
    CANVAS_FILL,        // fill the current path with a color
    CANVAS_FILL_TEXT,   // draw text at (x, y), size in w
    CANVAS_STROKE_TEXT, // stroke text outline at (x,y): size in w, line width in h
    CANVAS_DRAW_IMAGE,  // blit an RGBA buffer at (x, y), size w×h
    CANVAS_FILL_LINEAR, // fill current path with a linear gradient
    CANVAS_FILL_RADIAL, // fill current path with a radial gradient
    CANVAS_CLIP_RECT,   // intersect the clip region with rect (x,y,w,h)
    CANVAS_GROUP_BEGIN, // cairo_push_group — composite following cmds offscreen
    CANVAS_GROUP_END,   // pop group, paint at alpha (x) — TRUE group opacity
    /* Drop the clips added since this compositing scope began, back to its
       baseline. Appended at the END: the values are positional. */
    CANVAS_RESET_CLIP
} CanvasCmdType;

typedef struct {
    CanvasCmdType type;
    double x, y;              // MOVE_TO, LINE_TO, ARC center, FILL_TEXT origin,
                              //   DRAW_IMAGE top-left
    double r, g, b, a;        // STROKE / FILL color
    double w, h;              // FILL_RECT w/h; STROKE line_width (x); ARC radius (w);
                              //   ARC start angle (h); FILL_TEXT font size (w);
                              //   DRAW_IMAGE pixel dims
    double a0, a1;            // ARC start/end angle (radians)
    char* text;              // FILL_TEXT string (owned, freed on clear/destroy)
    unsigned char* pixels;   // DRAW_IMAGE RGBA8888 buffer (owned), iw*ih*4 bytes
    int iw, ih;              // DRAW_IMAGE pixel width/height
    // Gradient (FILL_LINEAR / FILL_RADIAL): geometry in x,y..(see impls),
    // plus owned stop arrays. FILL_LINEAR uses (gx1,gy1)→(gx2,gy2);
    // FILL_RADIAL uses center (gx1,gy1) radius gr + focal (gfx,gfy).
    // grad_line_width: 0 → fill the path; >0 → stroke it at that width.
    double gx1, gy1, gx2, gy2, gr, gfx, gfy, grad_line_width;
    int n_stops;
    int grad_extend;         // 0=pad, 1=reflect, 2=repeat (SVG spreadMethod)
    /* The ELLIPSE a gradientTransform (or a non-square objectBoundingBox)
       produces: semi-axes and tilt. gr keeps the old scalar answer, so a
       command with grx == 0 renders exactly as it always did. */
    double grx, gry, grot;
    char* font_family;       /* owned; raw CSS stack, NULL when unset */
    double* stop_off;        // owned: n_stops offsets (0..1)
    double* stop_rgba;       // owned: n_stops*4 colour comps (0..1)
} CanvasCmd;

typedef struct {
    CanvasCmd* cmds;
    int count;
    int capacity;
    int widget_handle;
    // Resize hook — the AeVG vg{} scope registers a closure here so it can
    // re-map its viewBox and re-flush its shapes at the new canvas size. Fired
    // from the draw func when the allocation differs from last_w/last_h.
    AeClosure* on_resize;
    int last_w;
    int last_h;
    // Click hook — AeVG's vg{} scope registers a closure here to receive
    // canvas-local (x, y) on a single click, so it can hit-test shapes and
    // dispatch per-element handlers (the widget-level on_click gives no coords).
    AeClosure* on_click;
    // Pointer-move hook — receives canvas-local (x, y) on every motion event,
    // for hover hit-testing (e.g. a treemap status line tracking the tile under
    // the cursor). Widget enter/leave (on_hover_enter/leave) gives no coords.
    AeClosure* on_move;
    // Key-press hook — receives a key NAME string (e.g. "Left", "a", "space",
    // "Escape") on every key-down while the canvas has focus. For games /
    // keyboard-driven canvases. The scene's event model already has the
    // dispatch side (grammar_events.dispatch_key_down); this bridges real keys.
    AeClosure* on_key;
    // Key-release hook — same key NAME string, on key-up. Pairs with on_key
    // so a game can hold per-key pressed state (down sets, up clears).
    AeClosure* on_key_release;
    // Pointer-release hook — canvas-local (x, y) where the button came up.
    // Pairs with on_click + on_move to form a press→drag→release swipe.
    AeClosure* on_release;
    // read_pixel replay cache: one rendered surface per (generation,
    // cmd-count, size). gen bumps on canvas_clear (each frame rebuild);
    // within a generation the command list only grows, so gen+count
    // uniquely identify buffer content. Without this a grid probe
    // re-rasterizes the WHOLE canvas per pixel — a 15k-sample sweep of a
    // path-heavy page takes minutes and stalls the main loop until the
    // driver's client times out.
    cairo_surface_t* replay_cache;
    unsigned long gen;
    unsigned long cache_gen;
    int cache_count, cache_w, cache_h;
    int last_paint_w;
    int last_paint_h;
    int last_paint_area;
    int last_paint_count;
    int paint_full_count;
    int paint_clip_count_total;
    int last_clip_area;
    double* paint_clip_rects;
    int paint_clip_count;
    int paint_clip_capacity;
    cairo_surface_t* paint_surface;
    int paint_surface_w;
    int paint_surface_h;
    int paint_surface_dirty;
} CanvasState;

static CanvasState* canvas_states = NULL;
static int canvas_state_count = 0;
static int canvas_state_capacity = 0;

extern double floatarr_get_raw(void* arr, int i);

static CanvasState* get_canvas_state(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_state_count) return NULL;
    return &canvas_states[canvas_id - 1];
}

static void canvas_add_cmd(int canvas_id, CanvasCmd cmd) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    if (cs->count >= cs->capacity) {
        cs->capacity = cs->capacity == 0 ? 64 : cs->capacity * 2;
        cs->cmds = realloc(cs->cmds, sizeof(CanvasCmd) * cs->capacity);
    }
    cs->cmds[cs->count++] = cmd;
    cs->paint_surface_dirty = 1;
}

static int canvas_clip_area_sum(const double* rects, int n) {
    double area = 0.0;
    for (int i = 0; rects && i < n; i++) {
        double w = rects[i*4+2], h = rects[i*4+3];
        if (w > 0.0 && h > 0.0) area += w * h;
    }
    return (int)(area + 0.5);
}

static void canvas_apply_paint_clip(cairo_t* cr, CanvasState* cs) {
    if (!cr || !cs || cs->paint_clip_count <= 0) return;
    cairo_new_path(cr);
    for (int i = 0; i < cs->paint_clip_count; i++) {
        double* r = &cs->paint_clip_rects[i * 4];
        if (r[2] > 0.0 && r[3] > 0.0) cairo_rectangle(cr, r[0], r[1], r[2], r[3]);
    }
    cairo_clip(cr);
    cairo_new_path(cr);
}

static int canvas_has_paint_clip(CanvasState* cs) {
    return cs && cs->paint_clip_count > 0;
}

static cairo_surface_t* canvas_ensure_paint_surface(CanvasState* cs,
                                                     int width, int height) {
    if (!cs || width <= 0 || height <= 0) return NULL;
    if (cs->paint_surface && cs->paint_surface_w == width &&
        cs->paint_surface_h == height) {
        return cs->paint_surface;
    }
    if (cs->paint_surface) {
        cairo_surface_destroy(cs->paint_surface);
        cs->paint_surface = NULL;
    }
    cs->paint_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                   width, height);
    if (cairo_surface_status(cs->paint_surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cs->paint_surface);
        cs->paint_surface = NULL;
        cs->paint_surface_w = 0;
        cs->paint_surface_h = 0;
        return NULL;
    }
    cs->paint_surface_w = width;
    cs->paint_surface_h = height;
    return cs->paint_surface;
}

static void canvas_clear_current_clip(cairo_t* cr) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

// Replay a canvas command buffer onto any cairo context. Shared by the live
// GtkDrawingArea draw func and the off-screen PNG renderer (canvas_write_png),
// so on-screen and headless output are byte-identical.
// Select a cairo toy font from packed flags: bit0 = monospace, bit1 = bold,
// bit2 = italic. font-family maps to the generic cairo families
// ("monospace"/"serif"/"sans-serif") — enough for the corpus's courier/serif/
// sans without a full font-name database. Called before every text draw so
// each run uses its own family/slant/weight (a prior run's face doesn't leak).
/* fam: the RAW CSS stack from the SVG, or NULL/"" when none was declared.
   cairo_select_font_face hands it to fontconfig, which is exactly the
   platform matcher we want: resolving "Helvetica, Arial, sans-serif"
   against what is installed is its job, not ours. The flags remain the
   fallback, and are still the source of slant/weight.

   Before this the family was DISCARDED upstream -- shapes.ae reduced
   font-family to a single is_monospace() bit -- so every backend invented
   its own face: cairo's generic "sans-serif" here, hardcoded "Segoe UI" on
   win32, systemFontOfSize on macOS. See vg/test/test_font_family.ae. */
static void canvas_select_font_family(cairo_t* cr, int flags, const char* fam) {
    const char* family = (fam && fam[0]) ? fam
                       : ((flags & 1) ? "monospace" : "sans-serif");
    cairo_font_slant_t slant = (flags & 4) ? CAIRO_FONT_SLANT_ITALIC
                                           : CAIRO_FONT_SLANT_NORMAL;
    cairo_font_weight_t weight = (flags & 2) ? CAIRO_FONT_WEIGHT_BOLD
                                             : CAIRO_FONT_WEIGHT_NORMAL;
    cairo_select_font_face(cr, family, slant, weight);
}

// Replay a RANGE [start, end) of the command buffer. Stage 3 (per-frame
// surfaces) needs this: frames emit their commands sequentially into the
// shared buffer, so one frame's drawing is a contiguous slice, and rendering
// that slice alone into an offscreen surface is what lets an unchanged frame
// be blitted instead of re-drawn. The whole-buffer canvas_replay below is
// this with the full range, so every existing caller is unaffected.
static void canvas_replay_range(cairo_t* cr, CanvasState* cs,
                                int start, int end) {
    if (!cs) return;
    if (start < 0) start = 0;
    if (end > cs->count) end = cs->count;
    /* INVARIANT: every cairo_save here has exactly one matching restore, and
       CANVAS_RESET_CLIP restores/re-saves in place. Breaking the pairing
       leaks clip state into the next frame. */
    cairo_save(cr);
    for (int i = start; i < end; i++) {
        CanvasCmd* c = &cs->cmds[i];
        switch (c->type) {
            case CANVAS_BEGIN_PATH:
                cairo_new_path(cr);
                break;
            case CANVAS_MOVE_TO:
                cairo_move_to(cr, c->x, c->y);
                break;
            case CANVAS_LINE_TO:
                cairo_line_to(cr, c->x, c->y);
                break;
            case CANVAS_STROKE:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_set_line_width(cr, c->x); // line_width stored in x
                // cap/join from iw/ih (SVG defaults butt/miter).
                if (c->iw == 1)      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                else if (c->iw == 2) cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
                else                 cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
                if (c->ih == 1)      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                else if (c->ih == 2) cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
                else                 cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
                cairo_stroke(cr);
                break;
            case CANVAS_FILL_RECT:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_rectangle(cr, c->x, c->y, c->w, c->h);
                cairo_fill(cr);
                break;
            case CANVAS_CLIP_RECT:
                // Intersect the clip region with (x,y,w,h). Persists for the
                // rest of this replay (a fresh cairo_t per frame resets it), so
                // the AeVG scene emits one at the start to enforce the SVG
                // viewport's overflow:hidden — shapes outside the viewBox are
                // cut at its edge.
                cairo_rectangle(cr, c->x, c->y, c->w, c->h);
                cairo_clip(cr);
                cairo_new_path(cr);  // clear the path the rectangle added
                break;
            case CANVAS_RESET_CLIP:
                // Drop every clip added since this compositing scope began,
                // back to the baseline (the widget's own bounds, or the
                // group's clip at GROUP_BEGIN). Restoring also rolls back
                // source/line width, which is harmless: every drawing command
                // sets its own before it paints.
                cairo_restore(cr);
                cairo_save(cr);
                cairo_new_path(cr);
                break;
            case CANVAS_ARC:
                // Append an arc to the current path. w = radius,
                // a0/a1 = start/end angle (radians). For a full
                // circle the caller passes 0..2π.
                cairo_arc(cr, c->x, c->y, c->w, c->a0, c->a1);
                break;
            case CANVAS_CLOSE_PATH:
                cairo_close_path(cr);
                break;
            case CANVAS_FILL:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                /* SVG fill-rule. cairo's default is WINDING, which matches
                   SVG's nonzero default, so this was right by accident until
                   a file asked for evenodd. iw carries the flag. */
                cairo_set_fill_rule(cr, c->iw ? CAIRO_FILL_RULE_EVEN_ODD
                                              : CAIRO_FILL_RULE_WINDING);
                cairo_fill(cr);
                cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
                break;
            case CANVAS_FILL_TEXT:
                canvas_select_font_family(cr, c->iw, c->font_family);
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_set_font_size(cr, c->w);
                cairo_move_to(cr, c->x, c->y);
                if (c->text) cairo_show_text(cr, c->text);
                cairo_new_path(cr); // show_text leaves a path; clear it
                break;
            case CANVAS_STROKE_TEXT: {
                // Outline the glyphs: convert to a path, then stroke. Emitted
                // AFTER the fill for the same run, so the outline sits on top.
                // Round join/cap so a wide stroke fills without miter spikes.
                cairo_line_join_t pj = cairo_get_line_join(cr);
                cairo_line_cap_t  pc = cairo_get_line_cap(cr);
                /* The family, not just the flags -- the FILL pass above uses
                   it, and a stroke drawn in a different face than the fill it
                   outlines would ghost. */
                canvas_select_font_family(cr, c->iw, c->font_family);
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_set_font_size(cr, c->w);
                cairo_set_line_width(cr, c->h);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_move_to(cr, c->x, c->y);
                if (c->text) cairo_text_path(cr, c->text);
                cairo_stroke(cr);   // consumes the path
                cairo_set_line_join(cr, pj);
                cairo_set_line_cap(cr, pc);
                break;
            }
            case CANVAS_DRAW_IMAGE:
                if (c->pixels && c->iw > 0 && c->ih > 0) {
                    // Incoming buffer is RGBA8888 (R,G,B,A bytes).
                    // Cairo ARGB32 is native-endian premultiplied — on
                    // little-endian that's B,G,R,A byte order with
                    // premultiplied colour. Build a converted buffer.
                    int n = c->iw * c->ih;
                    unsigned char* conv = (unsigned char*)malloc(n * 4);
                    if (conv) {
                        for (int px = 0; px < n; px++) {
                            unsigned char sr = c->pixels[px*4+0];
                            unsigned char sg = c->pixels[px*4+1];
                            unsigned char sb = c->pixels[px*4+2];
                            unsigned char sa = c->pixels[px*4+3];
                            // premultiply
                            conv[px*4+0] = (unsigned char)(sb * sa / 255); // B
                            conv[px*4+1] = (unsigned char)(sg * sa / 255); // G
                            conv[px*4+2] = (unsigned char)(sr * sa / 255); // R
                            conv[px*4+3] = sa;                              // A
                        }
                        int stride = cairo_format_stride_for_width(
                            CAIRO_FORMAT_ARGB32, c->iw);
                        cairo_surface_t* surf = cairo_image_surface_create_for_data(
                            conv, CAIRO_FORMAT_ARGB32, c->iw, c->ih, stride);
                        if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                            // c->w / c->h, when > 0, are the DESTINATION extent
                            // (canvas px). If they differ from the source pixel
                            // dims (iw/ih), scale the source surface to fit —
                            // e.g. a 640x480 video frame into a smaller region.
                            // w/h <= 0 means blit 1:1 at native size.
                            int have_dest = (c->w > 0.0 && c->h > 0.0);
                            cairo_save(cr);
                            if (have_dest) {
                                cairo_translate(cr, c->x, c->y);
                                cairo_scale(cr, c->w / (double)c->iw,
                                                c->h / (double)c->ih);
                                cairo_set_source_surface(cr, surf, 0, 0);
                            } else {
                                cairo_set_source_surface(cr, surf, c->x, c->y);
                            }
                            // Nearest/good filtering for scaled video frames.
                            cairo_pattern_set_filter(cairo_get_source(cr),
                                                     CAIRO_FILTER_GOOD);
                            cairo_paint(cr);
                            cairo_restore(cr);
                            cairo_set_source_rgba(cr, 0, 0, 0, 1); // reset source
                        }
                        cairo_surface_destroy(surf);
                        free(conv);
                    }
                }
                break;
            case CANVAS_FILL_LINEAR:
            case CANVAS_FILL_RADIAL: {
                cairo_pattern_t* pat;
                if (c->type == CANVAS_FILL_LINEAR) {
                    pat = cairo_pattern_create_linear(c->gx1, c->gy1, c->gx2, c->gy2);
                } else if (c->grx > 0.0 && c->gry > 0.0 &&
                           (fabs(c->grx - c->gry) > 0.01 || fabs(c->grot) > 0.01)) {
                    /* A TRUE ELLIPSE. cairo only makes circular radial
                       patterns, but a pattern carries its own matrix, so
                       build a unit circle and map it onto the ellipse:
                       translate to the centre, rotate by the tilt, scale the
                       axes. The pattern matrix runs USER -> PATTERN space, so
                       it is the INVERSE of that composition.

                       Without this the ellipse was flattened to a circle of
                       the average radius before any backend saw it -- see
                       vg/test/svg/radial_ellipse_repro.svg (axis ratio 2.00
                       required, a circle reads 1.00). */
                    /* The focal point lives in PATTERN space too, so scale
                       its offset PER AXIS rather than collapsing it to one
                       distance -- dividing |f-c| by grx alone is only right
                       for a circle, and it moved intertwingly.svg (an
                       objectBoundingBox radial with fx/fy on a non-square
                       shape) by +2.90. Undo the tilt first so the offset is
                       expressed along the ellipse's own axes. */
                    double fdx = c->gfx - c->gx1, fdy = c->gfy - c->gy1;
                    if (fabs(c->grot) > 0.01) {
                        double a = -c->grot * G_PI / 180.0;
                        double ca = cos(a), sa = sin(a);
                        double tx = fdx * ca - fdy * sa;
                        double ty = fdx * sa + fdy * ca;
                        fdx = tx; fdy = ty;
                    }
                    double fpx = (c->grx > 0.0) ? fdx / c->grx : 0.0;
                    double fpy = (c->gry > 0.0) ? fdy / c->gry : 0.0;
                    pat = cairo_pattern_create_radial(fpx, fpy, 0.0, 0.0, 0.0, 1.0);
                    cairo_matrix_t m;
                    cairo_matrix_init_identity(&m);
                    cairo_matrix_translate(&m, c->gx1, c->gy1);
                    if (fabs(c->grot) > 0.01)
                        cairo_matrix_rotate(&m, c->grot * G_PI / 180.0);
                    cairo_matrix_scale(&m, c->grx, c->gry);
                    cairo_matrix_invert(&m);
                    cairo_pattern_set_matrix(pat, &m);
                } else {
                    // Radial: inner circle at the focal point (radius 0),
                    // outer circle at the center with radius gr.
                    pat = cairo_pattern_create_radial(c->gfx, c->gfy, 0.0,
                                                      c->gx1, c->gy1, c->gr);
                }
                for (int si = 0; si < c->n_stops; si++) {
                    cairo_pattern_add_color_stop_rgba(pat, c->stop_off[si],
                        c->stop_rgba[si*4+0], c->stop_rgba[si*4+1],
                        c->stop_rgba[si*4+2], c->stop_rgba[si*4+3]);
                }
                // SVG spreadMethod → cairo extend (pad is the cairo default).
                if (c->grad_extend == 1) {
                    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REFLECT);
                } else if (c->grad_extend == 2) {
                    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
                } else {
                    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
                }
                cairo_set_source(cr, pat);
                if (c->grad_line_width > 0) {
                    cairo_set_line_width(cr, c->grad_line_width);
                    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                    cairo_stroke(cr);
                } else {
                    cairo_fill(cr);
                }
                cairo_pattern_destroy(pat);
                cairo_set_source_rgba(cr, 0, 0, 0, 1); // reset source
                break;
            }
            case CANVAS_CLEAR:
                break;
            case CANVAS_GROUP_BEGIN:
                // Composite everything until GROUP_END into an offscreen
                // group, then paint it ONCE at the group alpha — overlapping
                // children don't double-darken (the reason this exists).
                cairo_push_group(cr);
                cairo_save(cr);
                break;
            case CANVAS_GROUP_END:
                cairo_restore(cr);
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, c->x);
                break;
        }
    }
    cairo_restore(cr);
}

static void canvas_replay(cairo_t* cr, CanvasState* cs) {
    if (!cs) return;
    canvas_replay_range(cr, cs, 0, cs->count);
}

// How many commands the buffer currently holds. Lets the caller bracket a
// frame's drawing: read the count before and after, and the difference is
// that frame's contiguous slice.
// Same as canvas_draw_image_impl, but taking raw memory. Stage 3's frame
// surfaces are malloc'd buffers, not Aether strings.
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

int aether_ui_canvas_cmd_count_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->count : -1;
}

// Render command range [start, end) offscreen and copy it out as tightly
// packed RGBA8 (w*h*4 bytes) into caller-provided storage. Returns the bytes
// written, or 0 on error.
//
// This is the primitive Stage 3 was missing. Everything else already
// existed: canvas_write_png proves the offscreen replay works, and
// canvas_draw_image_scaled puts a buffer back. What no extern could do was
// hand a rendered buffer BACK to Aether -- RGBA has only ever travelled into
// the backend.
//
// Cairo's ARGB32 is premultiplied and native-endian, whereas
// canvas_draw_image expects straight (non-premultiplied) RGBA bytes, so this
// unpremultiplies on the way out. Skipping that step makes translucent
// content -- which is exactly what frames_demo uses -- come out too dark.
int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end,
                                            double ox, double oy,
                                            int width, int height,
                                            unsigned char* out, int out_len) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !out || width <= 0 || height <= 0) return 0;
    int need = width * height * 4;
    if (out_len < need) return 0;
    cairo_surface_t* surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }
    cairo_t* cr = cairo_create(surf);
    // Commands carry ABSOLUTE canvas coordinates, but the surface is
    // frame-local. Translating by -(ox, oy) puts the frame's top-left at the
    // buffer's origin; without this a frame-sized buffer would capture the
    // wrong region of the canvas entirely.
    cairo_translate(cr, -ox, -oy);
    canvas_replay_range(cr, cs, start, end);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    unsigned char* data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < height; y++) {
        unsigned char* row = data + (size_t)y * stride;
        unsigned char* dst = out + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            unsigned int v = *(unsigned int*)(row + x * 4);
            unsigned int a = (v >> 24) & 0xff;
            unsigned int r = (v >> 16) & 0xff;
            unsigned int g = (v >> 8) & 0xff;
            unsigned int b = v & 0xff;
            if (a != 0 && a != 255) {
                r = (r * 255 + a / 2) / a;
                g = (g * 255 + a / 2) / a;
                b = (b * 255 + a / 2) / a;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            dst[x * 4 + 0] = (unsigned char)r;
            dst[x * 4 + 1] = (unsigned char)g;
            dst[x * 4 + 2] = (unsigned char)b;
            dst[x * 4 + 3] = (unsigned char)a;
        }
    }
    cairo_surface_destroy(surf);
    return need;
}

static void canvas_draw_func(GtkDrawingArea* area, cairo_t* cr,
                              int width, int height, gpointer data) {
    (void)area;
    int canvas_id = (int)(intptr_t)data;
    CanvasState* cs = get_canvas_state(canvas_id);
    // AEUI_CANVAS_DEBUG=1 traces the canvas frame lifecycle to stderr:
    // draw / clear / redraw with the command-buffer count at each. This
    // is what identified vg.live's half-painting click hook (refreshes
    // alternating 8414 commands / 2 commands on ONE physical click) —
    // keep it: paint bugs that only appear under a real pointer cannot
    // be seen any other way, and the gate costs nothing when unset.
    int clipped = canvas_has_paint_clip(cs);
    int must_repaint_surface = cs &&
        (cs->paint_surface_dirty || !cs->paint_surface ||
         cs->paint_surface_w != width || cs->paint_surface_h != height);
    if (cs) {
        cs->last_paint_w = width;
        cs->last_paint_h = height;
        cs->last_paint_area = (must_repaint_surface && clipped)
            ? canvas_clip_area_sum(cs->paint_clip_rects, cs->paint_clip_count)
            : width * height;
        cs->last_paint_count = cs->count;
        if (must_repaint_surface && clipped) {
            cs->paint_clip_count_total++;
            cs->last_clip_area = cs->last_paint_area;
        } else if (must_repaint_surface) {
            cs->paint_full_count++;
        }
    }
    if (getenv("AEUI_CANVAS_DEBUG") && cs)
        fprintf(stderr, "[draw] id=%d count=%d w=%d h=%d area=%d last=%dx%d resize_hook=%d\n",
                canvas_id, cs->count, width, height, cs->last_paint_area, cs->last_w, cs->last_h,
                cs->on_resize && (width != cs->last_w || height != cs->last_h) ? 1 : 0);
    // Resize: GTK re-invokes the draw func with the new allocation. If a
    // scene registered a resize hook and the size changed, fire it so it can
    // re-map + re-flush its shapes at the new scale BEFORE we replay. The hook
    // receives (w, h) and must rebuild the command buffer (clear + re-dispatch);
    // it must NOT queue another draw (we're already drawing).
    if (cs && cs->on_resize && cs->on_resize->fn &&
        (width != cs->last_w || height != cs->last_h)) {
        cs->last_w = width;
        cs->last_h = height;
        /* #94: doubles, matching the other canvas callbacks. */
        ((void(*)(void*, double, double))cs->on_resize->fn)(
            cs->on_resize->env, (double)width, (double)height);
    }
    if (cs) {
        cairo_surface_t* surf = canvas_ensure_paint_surface(cs, width, height);
        if (!surf) {
            if (must_repaint_surface && clipped) {
                cairo_save(cr);
                canvas_apply_paint_clip(cr, cs);
                canvas_replay(cr, cs);
                cairo_restore(cr);
                cs->paint_clip_count = 0;
            } else {
                canvas_replay(cr, cs);
            }
            return;
        }
        if (must_repaint_surface) {
            cairo_t* bcr = cairo_create(surf);
            if (clipped) {
                cairo_save(bcr);
                canvas_apply_paint_clip(bcr, cs);
                canvas_clear_current_clip(bcr);
                canvas_replay(bcr, cs);
                cairo_restore(bcr);
                cs->paint_clip_count = 0;
            } else {
                cairo_save(bcr);
                cairo_set_operator(bcr, CAIRO_OPERATOR_CLEAR);
                cairo_paint(bcr);
                cairo_restore(bcr);
                canvas_replay(bcr, cs);
            }
            cairo_destroy(bcr);
            cairo_surface_flush(surf);
            cs->paint_surface_dirty = 0;
        }
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
    } else {
        canvas_replay(cr, cs);
    }
}

// Render the canvas command buffer to a PNG file off-screen — works HEADLESS
// (no window, pure cairo image surface). The driver's screenshot endpoint and
// the AeVG parity harness both use this; it's byte-identical to the on-screen
// render because both go through canvas_replay. Returns 1 on success, 0 on
// failure. Background is transparent (ARGB32) unless the buffer fills it.
int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !path || width <= 0 || height <= 0) return 0;
    cairo_surface_t* surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }
    cairo_t* cr = cairo_create(surf);
    canvas_replay(cr, cs);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS ? 1 : 0;
}

// Group-opacity command pair (see CANVAS_GROUP_BEGIN/END replay cases).
void aether_ui_canvas_group_begin_impl(int canvas_id) {
    CanvasCmd cmd = { .type = CANVAS_GROUP_BEGIN };
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    CanvasCmd cmd = { .type = CANVAS_GROUP_END, .x = alpha };
    canvas_add_cmd(canvas_id, cmd);
}

// Read one pixel of the canvas's CURRENT command buffer, rendered offscreen
// (same replay as canvas_write_png). Returns packed 0xAARRGGBB, or -1 on
// error. The honest primitive for pixel assertions (group opacity, shadows)
// in headless tests and the driver's /canvas/{id}/pixel route.
/* Native collection backing: not implemented on this backend, so the DSL
   keeps its composed window path. Answering 0 here is the whole contract;
   the rest exist so the symbols resolve. */
int aether_ui_native_list_available_impl(void) { return 0; }
int aether_ui_native_list_create_impl(int horizontal, int window_rows) { return 0; }
void aether_ui_native_list_set_row_builder_impl(int handle, void* builder) { }
void aether_ui_native_list_set_count_impl(int handle, int count) { }
void aether_ui_native_list_scroll_to_impl(int handle, int index) { }
int aether_ui_native_list_first_visible_impl(int handle) { return 0; }

int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py,
                                     int width, int height) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || px < 0 || py < 0 || px >= width || py >= height) return -1;
    if (!(cs->replay_cache && cs->cache_gen == cs->gen &&
          cs->cache_count == cs->count &&
          cs->cache_w == width && cs->cache_h == height)) {
        if (cs->replay_cache) {
            cairo_surface_destroy(cs->replay_cache);
            cs->replay_cache = NULL;
        }
        cairo_surface_t* surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surf);
            return -1;
        }
        cairo_t* cr = cairo_create(surf);
        canvas_replay(cr, cs);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        cs->replay_cache = surf;
        cs->cache_gen = cs->gen;
        cs->cache_count = cs->count;
        cs->cache_w = width;
        cs->cache_h = height;
    }
    unsigned char* data = cairo_image_surface_get_data(cs->replay_cache);
    int stride = cairo_image_surface_get_stride(cs->replay_cache);
    // ARGB32 is premultiplied, native-endian 32-bit.
    unsigned int v = *(unsigned int*)(data + py * stride + px * 4);
    return (int)v;
}

// How many pixels of the PAINTER'S OWN retained surface differ from the
// surface's top-left pixel. A blank canvas -- background colour and nothing
// else -- reports 0.
//
// LIMIT, measured 2026-08-09: this reports what is ON the surface, NOT what
// the last paint drew. The Stage 2.5 surface is RETAINED and not cleared
// between paints, so an app that stops drawing entirely keeps reporting its
// last good frame. Verified on video_frame: with all drawing disabled the
// /screenshot came back blank (3890 bytes) while this still read 38400.
//
// So it catches a canvas that NEVER drew (the Stage 3 blank-canvas
// regression, where it correctly read 0), and does NOT catch one that drew
// once and then stopped. For the latter, compare successive /screenshot
// sizes or assert on content that must CHANGE.
//
// This exists because every other canvas metric can be green while the app
// renders nothing. canvas_read_pixel and /screenshot's write_png path both
// REPLAY the command buffer into a private surface, so they see commands
// that were queued; they cannot see that those commands were freed by
// canvas_clear before the painter ever ran. Stage 3's first attempt was
// caught only by a human looking at a PNG: counters, acceptance test,
// goldens and eleven specs were all green against a blank window.
// This reads what actually reached the paint surface.
//
// Sampled on a grid rather than every pixel: a 960x640 canvas is 614400
// pixels and this runs inside a driver request. Step 4 in each axis still
// gives ~38k samples, far more than enough to tell "drew something" from
// "drew nothing", which is the distinction that matters.
int aether_ui_canvas_painted_pixels_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !cs->paint_surface) return -1;
    cairo_surface_flush(cs->paint_surface);
    if (cairo_image_surface_get_format(cs->paint_surface) != CAIRO_FORMAT_ARGB32 &&
        cairo_image_surface_get_format(cs->paint_surface) != CAIRO_FORMAT_RGB24) {
        return -1;
    }
    unsigned char* data = cairo_image_surface_get_data(cs->paint_surface);
    if (!data) return -1;
    int w = cairo_image_surface_get_width(cs->paint_surface);
    int h = cairo_image_surface_get_height(cs->paint_surface);
    int stride = cairo_image_surface_get_stride(cs->paint_surface);
    if (w <= 0 || h <= 0) return -1;
    unsigned int bg = *(unsigned int*)data;   // top-left = the background wash
    int differing = 0;
    for (int y = 0; y < h; y += 4) {
        unsigned char* row = data + (size_t)y * stride;
        for (int x = 0; x < w; x += 4) {
            if (*(unsigned int*)(row + x * 4) != bg) differing++;
        }
    }
    return differing;
}

int aether_ui_canvas_last_paint_area_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->last_paint_area : -1;
}

int aether_ui_canvas_last_paint_count_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->last_paint_count : -1;
}

int aether_ui_canvas_last_paint_w_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->last_paint_w : -1;
}

int aether_ui_canvas_last_paint_h_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->last_paint_h : -1;
}

int aether_ui_canvas_create_impl(int width, int height) {
    ensure_gtk_init();
    GtkWidget* da = gtk_drawing_area_new();
    // Focusable so it can receive key events (canvas_on_key); a GtkDrawingArea
    // isn't focusable by default. Harmless for canvases that never take keys.
    gtk_widget_set_focusable(da, TRUE);
    // content_width/height is the canvas's NATURAL (minimum) size — used when
    // no parent forces a size (headless PNG, canvas in a scroll view). But a
    // GtkDrawingArea won't grow past its content size on its own, so a canvas
    // in a window would stay 400x300 and just center. Make it expand + fill so
    // it takes the whole allocation; the draw func then gets the real size and
    // the resize hook fires, letting the vg scene rescale.
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), width);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), height);
    gtk_widget_set_hexpand(da, TRUE);
    gtk_widget_set_vexpand(da, TRUE);
    gtk_widget_set_halign(da, GTK_ALIGN_FILL);
    gtk_widget_set_valign(da, GTK_ALIGN_FILL);

    if (canvas_state_count >= canvas_state_capacity) {
        canvas_state_capacity = canvas_state_capacity == 0 ? 16 : canvas_state_capacity * 2;
        canvas_states = realloc(canvas_states, sizeof(CanvasState) * canvas_state_capacity);
    }
    CanvasState* cs = &canvas_states[canvas_state_count];
    cs->cmds = NULL;
    cs->count = 0;
    cs->capacity = 0;
    cs->on_resize = NULL;
    cs->on_move = NULL;
    cs->on_key = NULL;
    cs->on_key_release = NULL;
    cs->on_release = NULL;
    cs->on_click = NULL;
    cs->replay_cache = NULL;
    cs->gen = 0;
    cs->cache_gen = 0;
    cs->cache_count = -1;
    cs->cache_w = 0;
    cs->cache_h = 0;
    cs->last_w = width;
    cs->last_h = height;
    cs->last_paint_w = 0;
    cs->last_paint_h = 0;
    cs->last_paint_area = 0;
    cs->last_paint_count = 0;
    cs->paint_full_count = 0;
    cs->paint_clip_count_total = 0;
    cs->last_clip_area = 0;
    cs->paint_clip_rects = NULL;
    cs->paint_clip_count = 0;
    cs->paint_clip_capacity = 0;
    cs->paint_surface = NULL;
    cs->paint_surface_w = 0;
    cs->paint_surface_h = 0;
    cs->paint_surface_dirty = 1;
    canvas_state_count++;
    int canvas_id = canvas_state_count; // 1-based

    cs->widget_handle = aether_ui_register_widget(da);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), canvas_draw_func,
                                    (gpointer)(intptr_t)canvas_id, NULL);
    return canvas_id;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->widget_handle : 0;
}

// Register a resize hook on a canvas. The boxed Aether closure takes (w, h)
// and is fired from the draw func when the allocation changes (see
// canvas_draw_func). AeVG's vg{} scope uses this to rescale its scene.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    cs->on_resize = (AeClosure*)boxed_closure;
    // Seed last_w/h to the widget's content size so the first real allocation
    // that differs triggers a remap. (0 would fire spuriously on first draw.)
    cs->last_w = -1;
    cs->last_h = -1;
}

// Canvas click gesture — forwards the canvas-LOCAL (x, y) of a single click to
// the registered closure. Unlike on_double_click/on_button_clicked (which drop
// the coords), AeVG needs them to hit-test which shape was clicked. The
// "pressed" signal of a GtkGestureClick reports coordinates relative to the
// widget the controller is attached to (the drawing area), which is the same
// space AeVG stores shape bounds in (post-flush canvas px).
// Canvas button events use a LEGACY event controller, NOT GtkGestureClick.
// sommelier (ChromeOS Crostini) delivers touchpad taps as button press/
// release pairs that GtkGestureClick silently drops — first seen with
// button-3 in the context-menu saga, and equally true for BUTTON-1 taps on
// the treemap canvas (raw events reached the widget, "pressed" never fired,
// so clicks did nothing on the Chromebook). The legacy controller sees every
// event on every backend; filter by type + button ourselves.
//
// Translate an event's surface position into WIDGET-local coordinates
// (gdk_event_get_position is surface-relative; hit-testing needs canvas px).
static void event_widget_coords(GtkWidget* w, GdkEvent* ev,
                                 double* wx, double* wy) {
    double sx = 0, sy = 0;
    gdk_event_get_position(ev, &sx, &sy);
    GtkNative* native = gtk_widget_get_native(w);
    if (native) {
        double nx = 0, ny = 0;
        gtk_native_get_surface_transform(native, &nx, &ny);
        graphene_point_t p = GRAPHENE_POINT_INIT((float)(sx - nx), (float)(sy - ny));
        graphene_point_t out;
        if (gtk_widget_compute_point(GTK_WIDGET(native), w, &p, &out)) {
            *wx = out.x;
            *wy = out.y;
            return;
        }
    }
    *wx = sx;
    *wy = sy;
}

// One hook instance per registration: which widget (for coords + focus),
// which closure, and which event type it fires on.
typedef struct {
    GtkWidget* widget;
    AeClosure* closure;
    GdkEventType fires_on;   // GDK_BUTTON_PRESS (click) or _RELEASE (release)
} CanvasButtonHook;

static gboolean on_canvas_button_legacy(GtkEventControllerLegacy* c,
                                         GdkEvent* ev, gpointer data) {
    (void)c;
    CanvasButtonHook* hook = (CanvasButtonHook*)data;
    GdkEventType t = gdk_event_get_event_type(ev);
    if (t != hook->fires_on) return FALSE;
    if (gdk_button_event_get_button(ev) != 1) return FALSE;
    if (aeui_ctx_debug()) {
        GdkDevice* dev = gdk_event_get_device(ev);
        fprintf(stderr, "aeui: canvas btn1 %s dev=%s src=%d time=%u\n",
                t == GDK_BUTTON_PRESS ? "press" : "release",
                dev ? gdk_device_get_name(dev) : "(null)",
                dev ? (int)gdk_device_get_source(dev) : -1,
                gdk_event_get_time(ev));
    }
    double x = 0, y = 0;
    event_widget_coords(hook->widget, ev, &x, &y);
    if (aeui_ctx_debug())
        fprintf(stderr, "aeui: canvas %s px=(%.1f, %.1f)\n",
                t == GDK_BUTTON_PRESS ? "press" : "release", x, y);
    // Keep keyboard input flowing back to the canvas after a toolbar button
    // stole focus (the gesture-based focus-click never fires on sommelier).
    if (t == GDK_BUTTON_PRESS) gtk_widget_grab_focus(hook->widget);
    AeClosure* cl = hook->closure;
    if (cl && cl->fn) {
        ((void(*)(void*, double, double))cl->fn)(cl->env, x, y);
    }
    return FALSE;   // propagate — the ctx-menu legacy controller needs it too
}

static void canvas_add_button_hook(GtkWidget* w, AeClosure* closure,
                                    GdkEventType fires_on) {
    CanvasButtonHook* hook = g_new0(CanvasButtonHook, 1);
    hook->widget = w;
    hook->closure = closure;
    hook->fires_on = fires_on;
    GtkEventController* legacy = gtk_event_controller_legacy_new();
    g_signal_connect_data(legacy, "event",
                          G_CALLBACK(on_canvas_button_legacy), hook,
                          (GClosureNotify)g_free, 0);
    gtk_widget_add_controller(w, legacy);
}

// Register a single-click hook on a canvas. The boxed Aether closure takes
// (x: float, y: float) in canvas-local pixels. AeVG's vg{} scope uses this to
// dispatch_click into its scene, hit-testing tracked shape elements.
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_click = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    canvas_add_button_hook(w, cs->on_click, GDK_BUTTON_PRESS);
}

// Register a pointer-release hook on a canvas. Closure takes (x, y) in
// canvas-local px (the point where the button was released). With on_click +
// on_move this completes a press→drag→release gesture so callers can compute
// a swipe (drag delta) without per-element drag tracking. Built for the cube.
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_release = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    canvas_add_button_hook(w, cs->on_release, GDK_BUTTON_RELEASE);
}

// Pointer-move on the canvas: forward canvas-local (x, y) to the boxed closure.
// The GtkEventControllerMotion "motion" signal already reports widget-relative
// coords (unlike the enter/leave handlers, which discard them).
//
// Suppressed while the AetherUIDriver is armed: GTK re-synthesizes a motion
// at the parked pointer after every relayout, and under Xvfb that phantom
// hover clobbers state (status text) between a test's action and its
// assertion. Automation injects pointer-moves via POST /canvas/{id}/move,
// which calls the closure directly and stays deterministic.
static int aeui_remote_controlled(void);
static void on_canvas_move(GtkEventControllerMotion* ctrl, double x, double y,
                            gpointer data) {
    (void)ctrl;
    if (aeui_remote_controlled()) return;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*, double, double))c->fn)(c->env, x, y);
    }
}

// Register a pointer-move hook on a canvas. The boxed Aether closure takes
// (x: float, y: float) in canvas-local pixels — fired on every motion event.
// Used for hover hit-testing (e.g. a status line tracking the item under the
// cursor) where the widget-level enter/leave hooks are too coarse.
void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_move = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_canvas_move), boxed_closure);
    gtk_widget_add_controller(w, motion);
}

// Key-press on the canvas: forward the key NAME (gdk_keyval_name, e.g. "Left",
// "a", "space", "Escape") to the boxed closure. The widget must have focus —
// the canvas is set focusable at creation, and on_key registration grabs it.
static gboolean on_canvas_key(GtkEventControllerKey* ctrl, guint keyval,
                               guint keycode, GdkModifierType state, gpointer data) {
    (void)ctrl; (void)keycode; (void)state;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        const char* name = gdk_keyval_name(keyval);
        ((void(*)(void*, const char*))c->fn)(c->env, name ? name : "");
    }
    return TRUE;   // handled
}

// Grab keyboard focus once the widget is actually on screen. Calling
// grab_focus at registration is too early — the window isn't mapped yet, so
// the grab doesn't stick and keys go nowhere. Defer it to the "map" signal.
static void on_canvas_map_focus(GtkWidget* w, gpointer data) {
    (void)data;
    gtk_widget_grab_focus(w);
}

// A click anywhere on the canvas returns keyboard focus to it (clicking a
// button steals focus; this gets it back so keys keep working). Runs on the
// CAPTURE phase so it fires even if another gesture also handles the press.
static void on_canvas_focus_click(GtkGestureClick* g, int n, double x, double y,
                                   gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    gtk_widget_grab_focus(GTK_WIDGET(data));
}

// Register a key-down hook on a canvas. The boxed Aether closure takes
// (key: string). Bridges real GTK key events into the scene's keyboard model
// (which already has grammar_events.dispatch_key_down on the Aether side).
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_canvas_key), boxed_closure);
    gtk_widget_add_controller(w, keys);

    // Take focus when mapped (grab here is too early — window not shown yet).
    g_signal_connect(w, "map", G_CALLBACK(on_canvas_map_focus), NULL);
    if (gtk_widget_get_mapped(w)) gtk_widget_grab_focus(w);

    // Re-focus on click so the canvas keeps keyboard input after a button steals
    // focus. Capture phase so it co-exists with the coords click gesture.
    GtkGesture* fclick = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(fclick),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(fclick, "pressed", G_CALLBACK(on_canvas_focus_click), w);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(fclick));
}

// Key-release on the canvas: same key NAME as on_canvas_key, fired on key-up.
static void on_canvas_key_release(GtkEventControllerKey* ctrl, guint keyval,
                                  guint keycode, GdkModifierType state, gpointer data) {
    (void)ctrl; (void)keycode; (void)state;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        const char* name = gdk_keyval_name(keyval);
        ((void(*)(void*, const char*))c->fn)(c->env, name ? name : "");
    }
}

// Register a key-UP hook on a canvas. Pairs with canvas_on_key: a game sets a
// key's pressed flag on key-down and clears it here. Focus handling lives on
// the on_key registration (register both for real keyboard input).
void aether_ui_canvas_on_key_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key_release = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-released", G_CALLBACK(on_canvas_key_release),
                     boxed_closure);
    gtk_widget_add_controller(w, keys);
}

void aether_ui_canvas_begin_path_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_BEGIN_PATH });
}

void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_MOVE_TO, .x = x, .y = y });
}

void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_LINE_TO, .x = x, .y = y });
}

// cap: 0=butt (SVG default), 1=round, 2=square. join: 0=miter (default),
// 1=round, 2=bevel. Stored in the unused iw/ih int fields.
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b,
                             double a, double line_width, int cap, int join) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_STROKE, .r = r, .g = g, .b = b, .a = a, .x = line_width,
        .iw = cap, .ih = join
    });
}

void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y,
                                double w, double h,
                                double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL_RECT, .x = x, .y = y, .w = w, .h = h,
        .r = r, .g = g, .b = b, .a = a
    });
}

// Intersect the canvas clip region with rect (x,y,w,h). AeVG emits one at the
// start of a scene flush to clip to the viewport (SVG overflow:hidden).
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y,
                                double w, double h) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_CLIP_RECT, .x = x, .y = y, .w = w, .h = h
    });
}

void aether_ui_canvas_set_clip_rects_impl(int canvas_id, void* rects, int n) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !rects || n <= 0) {
        if (cs) cs->paint_clip_count = 0;
        return;
    }
    if (n > cs->paint_clip_capacity) {
        cs->paint_clip_capacity = n;
        cs->paint_clip_rects = realloc(cs->paint_clip_rects, sizeof(double) * n * 4);
    }
    if (!cs->paint_clip_rects) {
        cs->paint_clip_count = 0;
        cs->paint_clip_capacity = 0;
        return;
    }
    for (int i = 0; i < n * 4; i++) {
        cs->paint_clip_rects[i] = floatarr_get_raw(rects, i);
    }
    cs->paint_clip_count = n;
}

void aether_ui_canvas_reset_clip_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_RESET_CLIP });
}

void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                                double start_angle, double end_angle) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_ARC, .x = cx, .y = cy, .w = radius,
        .a0 = start_angle, .a1 = end_angle
    });
}

void aether_ui_canvas_close_path_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_CLOSE_PATH });
}

void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a,
                                int even_odd) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL, .r = r, .g = g, .b = b, .a = a, .iw = even_odd
    });
}

void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text,
                                      double x, double y, double font_size,
                                      int font_flags, const char* font_family,
                                      double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL_TEXT, .x = x, .y = y, .w = font_size,
        .iw = font_flags,   // mono|bold<<1|italic<<2 (see canvas_select_font)
        .r = r, .g = g, .b = b, .a = a,
        .font_family = (font_family && font_family[0]) ? strdup(font_family) : NULL,
        .text = text ? strdup(text) : NULL
    });
}

// Stroke (outline) text at (x, y). font_size in w, line width in h, colour in
// r,g,b,a. Queue AFTER the fill for the same run so the outline paints on top.
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text,
                                        double x, double y, double font_size,
                                        double line_width, int font_flags,
                                        const char* font_family,
                                        double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_STROKE_TEXT, .x = x, .y = y, .w = font_size, .h = line_width,
        .iw = font_flags,
        .r = r, .g = g, .b = b, .a = a,
        .font_family = (font_family && font_family[0]) ? strdup(font_family) : NULL,
        .text = text ? strdup(text) : NULL
    });
}

// ─── Text metrics ────────────────────────────────────────────────
//
// Measurement must work HEADLESS (no window, no canvas) — the layout code
// calls it during build. A static 1x1 image-surface scratch cairo_t,
// created on first use, gives cairo's text_extents/font_extents without a
// display. It uses cairo's DEFAULT toy font at the requested size — exactly
// what CANVAS_FILL_TEXT renders with (cairo_set_font_size + cairo_show_text,
// no cairo_select_font_face), so measurements agree with what's drawn.
static cairo_t* text_scratch_cr(void) {
    static cairo_t* cr = NULL;
    if (!cr) {
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cr = cairo_create(s);
        cairo_surface_destroy(s);   // cr keeps a ref
    }
    return cr;
}

// Advance width of `text` at `size` (px). 0 for NULL/empty.
double aether_ui_text_measure(double size, const char* text) {
    if (!text || !text[0]) return 0.0;
    cairo_t* cr = text_scratch_cr();
    cairo_set_font_size(cr, size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    return ext.x_advance;   // pen advance, the width you position siblings by
}

// Font vertical metrics at `size` (px): ascent, descent, line height. The
// three are read from one font_extents call; separate getters keep the ABI
// dumb (no tuple/out-param dance across the Aether extern boundary).
static void text_font_extents(double size, cairo_font_extents_t* fe) {
    cairo_t* cr = text_scratch_cr();
    cairo_set_font_size(cr, size);
    cairo_font_extents(cr, fe);
}
double aether_ui_font_ascent(double size)  { cairo_font_extents_t fe; text_font_extents(size, &fe); return fe.ascent; }
double aether_ui_font_descent(double size) { cairo_font_extents_t fe; text_font_extents(size, &fe); return fe.descent; }
double aether_ui_font_height(double size)  { cairo_font_extents_t fe; text_font_extents(size, &fe); return fe.height; }

void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;  // truncated buffer — skip
    // Own a copy: the Aether-side buffer doesn't outlive this call.
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_DRAW_IMAGE, .x = x, .y = y,
        .pixels = owned, .iw = iw, .ih = ih
    });
}

// As draw_image, but scale the iw×ih source to a dw×dh destination rect at
// (x,y) — for a video/raster frame whose pixel resolution differs from the
// region's canvas-px extent. dw/dh <= 0 falls back to a 1:1 native blit.
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y,
                                       double dw, double dh, int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_DRAW_IMAGE, .x = x, .y = y, .w = dw, .h = dh,
        .pixels = owned, .iw = iw, .ih = ih
    });
}

// std.collections FloatArray accessor (libaether) — read gradient
// stop arrays handed over as opaque FloatArray* from Aether.
extern double floatarr_get_unchecked(void* arr, int i);
extern int floatarr_size(void* arr);

// Copy n_stops offsets + n_stops*4 rgba comps out of the Aether
// FloatArray handles into freshly-owned C arrays on the command.
static void canvas_copy_stops(CanvasCmd* c, int n_stops,
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
    CanvasCmd cmd = { .type = CANVAS_FILL_LINEAR,
                      .gx1 = x1, .gy1 = y1, .gx2 = x2, .gy2 = y2,
                      .grad_line_width = line_width, .grad_extend = extend,
                      .iw = cap, .ih = join };
    canvas_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width, int extend,
        int cap, int join, double rx, double ry, double rot_deg) {
    CanvasCmd cmd = { .type = CANVAS_FILL_RADIAL,
                      .gx1 = cx, .gy1 = cy, .gr = radius, .gfx = fx, .gfy = fy,
                      .grad_line_width = line_width, .grad_extend = extend,
                      .iw = cap, .ih = join,
                      .grx = rx, .gry = ry, .grot = rot_deg };
    canvas_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_clear_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs) {
        // Free any owned text strings / image buffers / gradient stops.
        for (int i = 0; i < cs->count; i++) {
            CanvasCmd* c = &cs->cmds[i];
            if (c->font_family) { free(c->font_family); c->font_family = NULL; }
            if (c->type == CANVAS_FILL_TEXT && c->text) {
                free(c->text); c->text = NULL;
            }
            if (c->type == CANVAS_DRAW_IMAGE && c->pixels) {
                free(c->pixels); c->pixels = NULL;
            }
            if (c->type == CANVAS_FILL_LINEAR || c->type == CANVAS_FILL_RADIAL) {
                free(c->stop_off);  c->stop_off = NULL;
                free(c->stop_rgba); c->stop_rgba = NULL;
            }
        }
        cs->count = 0;
        cs->gen++;   // invalidates the read_pixel replay cache
        cs->paint_surface_dirty = 1;
        if (getenv("AEUI_CANVAS_DEBUG"))
            fprintf(stderr, "[clear] id=%d gen=%lu\n", canvas_id, cs->gen);
        GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
        if (w) gtk_widget_queue_draw(w);
    }
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs) {
        if (getenv("AEUI_CANVAS_DEBUG"))
            fprintf(stderr, "[redraw] id=%d count=%d\n", canvas_id, cs->count);
        GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
        if (w) gtk_widget_queue_draw(w);
    }
}

// Event handlers — hover and double-click

static void on_hover_enter(GtkEventControllerMotion* ctrl, double x, double y,
                            gpointer data) {
    (void)ctrl; (void)x; (void)y;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*, intptr_t))c->fn)(c->env, 1);
    }
}

static void on_hover_leave(GtkEventControllerMotion* ctrl, gpointer data) {
    (void)ctrl;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*, intptr_t))c->fn)(c->env, 0);
    }
}

void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_hover_enter), boxed_closure);
    g_signal_connect(motion, "leave", G_CALLBACK(on_hover_leave), boxed_closure);
    gtk_widget_add_controller(w, motion);
}

static void on_double_click(GtkGestureClick* gesture, int n_press,
                             double x, double y, gpointer data) {
    (void)gesture; (void)x; (void)y;
    if (n_press != 2) return;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
}

void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    GtkGesture* gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_double_click), boxed_closure);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(gesture));
    // Stash the closure so the driver can fire it headlessly (no real gesture).
    g_object_set_data(G_OBJECT(w), "aeui-dblclick", boxed_closure);
}

// Driver hook: invoke a widget's double-click closure directly. Returns 1 if
// one was registered and fired, 0 otherwise.
int aether_ui_fire_double_click(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    AeClosure* c = (AeClosure*)g_object_get_data(G_OBJECT(w), "aeui-dblclick");
    if (!c || !c->fn) return 0;
    ((void(*)(void*))c->fn)(c->env);
    return 1;
}

// ── Row drag-reorder ────────────────────────────────────────────────
// Each reorderable row is a GtkDragSource (its drag payload is its own index)
// and a GtkDropTarget (accepting an int). On drop, the target row fires its
// on_drop(source_index) closure — which the DSL wires to listbox_move. The
// index each row carries is stashed as gobject data so the driver can fire a
// drop headlessly (aether_ui_fire_row_drop).
static GdkContentProvider* on_row_drag_prepare(GtkDragSource* src, double x,
                                               double y, gpointer data) {
    (void)src; (void)x; (void)y;
    int index = GPOINTER_TO_INT(data);
    GValue v = G_VALUE_INIT;
    g_value_init(&v, G_TYPE_INT);
    g_value_set_int(&v, index);
    return gdk_content_provider_new_for_value(&v);
}

static gboolean on_row_drop(GtkDropTarget* target, const GValue* value,
                            double x, double y, gpointer data) {
    (void)target; (void)x; (void)y;
    if (!G_VALUE_HOLDS_INT(value)) return FALSE;
    int src_index = g_value_get_int(value);
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)src_index);
    return TRUE;
}

// Outbound file drag. The payload is stashed on the widget so the prepare
// callback and the driver read the same string, which is what keeps the
// headless view of it honest.
static GdkContentProvider* on_file_drag_prepare(GtkDragSource* src, double x,
                                                double y, gpointer data) {
    (void)x; (void)y; (void)data;
    // Read the CURRENT payload off the widget rather than capturing it when
    // the source was armed. Capturing was wrong twice over: draggable() with
    // a new path frees the old string through the set_data_full destructor,
    // leaving the captured pointer dangling, and even before that free the
    // drag would have offered the FIRST path forever, so a recycled row that
    // rebinds its file would hand over the wrong one.
    GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(src));
    const char* path = w ? (const char*)g_object_get_data(G_OBJECT(w),
                                                          "aeui-dragfile")
                         : NULL;
    if (!path || !*path) return NULL;
    GFile* f = g_file_new_for_path(path);
    GdkContentProvider* p = gdk_content_provider_new_typed(G_TYPE_FILE, f);
    g_object_unref(f);
    return p;
}

void aether_ui_widget_draggable_file_impl(int handle, const char* path) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    if (!path || !*path) {
        g_object_set_data(G_OBJECT(w), "aeui-dragfile", NULL);
        return;
    }
    char* owned = g_strdup(path);
    g_object_set_data_full(G_OBJECT(w), "aeui-dragfile", owned, g_free);
    // Arm the source once; re-arming would stack controllers on a widget
    // whose payload is simply being updated.
    if (!g_object_get_data(G_OBJECT(w), "aeui-dragarmed")) {
        GtkDragSource* src = gtk_drag_source_new();
        gtk_drag_source_set_actions(src, GDK_ACTION_COPY);
        // No user_data: the handler reads the live payload off the widget.
        g_signal_connect(src, "prepare", G_CALLBACK(on_file_drag_prepare), NULL);
        gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(src));
        g_object_set_data(G_OBJECT(w), "aeui-dragarmed", GINT_TO_POINTER(1));
    }
}

const char* aether_ui_widget_drag_payload_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "";
    const char* p = (const char*)g_object_get_data(G_OBJECT(w), "aeui-dragfile");
    return p ? p : "";
}

void aether_ui_row_drag_reorder_impl(int row_handle, int index,
                                     void* on_drop_closure) {
    GtkWidget* row = aether_ui_get_widget(row_handle);
    if (!row) return;
    // Drag source — payload is this row's index.
    GtkDragSource* src = gtk_drag_source_new();
    g_signal_connect(src, "prepare", G_CALLBACK(on_row_drag_prepare),
                     GINT_TO_POINTER(index));
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(src));
    // Drop target — accepts an int (the dragged row's index) → on_drop.
    GtkDropTarget* tgt = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
    g_signal_connect(tgt, "drop", G_CALLBACK(on_row_drop), on_drop_closure);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(tgt));
    // Stash for the headless driver-fire path.
    g_object_set_data(G_OBJECT(row), "aeui-rowdrop", on_drop_closure);
}

// Driver hook: simulate dropping row `src_index` onto `row_handle` (fires that
// row's on_drop). Real drag needs a seat; this drives it headlessly.
int aether_ui_fire_row_drop(int row_handle, int src_index) {
    GtkWidget* row = aether_ui_get_widget(row_handle);
    if (!row) return 0;
    AeClosure* c = (AeClosure*)g_object_get_data(G_OBJECT(row), "aeui-rowdrop");
    if (!c || !c->fn) return 0;
    ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)src_index);
    return 1;
}

// ── vlist native scroll ─────────────────────────────────────────────
// A GtkEventControllerScroll on the vlist container turns wheel/two-finger
// scroll into on_scroll(dy) — dy = the rounded delta_y (down = +1 = toward the
// end). The closure is stashed so the driver can fire a step headlessly.
static gboolean on_vlist_scroll(GtkEventControllerScroll* c, double dx,
                                double dy, gpointer data) {
    (void)c; (void)dx;
    AeClosure* cl = (AeClosure*)data;
    int step = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    if (step && cl && cl->fn)
        ((void(*)(void*, intptr_t))cl->fn)(cl->env, (intptr_t)step);
    return TRUE;
}

// ── gesture PROBE (diagnostic; apps/gesture_probe) ──────────────────
// Wires every touchpad-relevant GTK4 controller to ONE callback that reports
// what actually arrived: kind, two numbers, and the modifier state. It exists
// because we could not answer "what does a Chromebook trackpad pinch
// deliver?" from documentation -- the first zoom attempt assumed a pinch
// arrives as ctrl+scroll, and it does not.
//
// The callback is cb(kind: string, a: float, b: float, mods: int) where
// `mods` is the GdkModifierType bitmask (SHIFT=1, CTRL=4 on X11/Wayland).
// kind is one of: scroll, zoom, rotate, drag-begin, drag-update, drag-end,
// touchpad-raw.
typedef struct { AeClosure* cl; } AeuiProbe;

static void probe_fire(AeuiProbe* p, const char* kind, double a, double b, int mods) {
    if (p && p->cl && p->cl->fn)
        ((void(*)(void*, const char*, double, double, intptr_t))p->cl->fn)
            (p->cl->env, kind, a, b, (intptr_t)mods);
}

static gboolean probe_scroll(GtkEventControllerScroll* c, double dx, double dy, gpointer d) {
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
    probe_fire((AeuiProbe*)d, "scroll", dx, dy, (int)m);
    return FALSE;   /* do not consume: let other controllers see it too */
}
static void probe_zoom(GtkGestureZoom* g, double scale, gpointer d) {
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    probe_fire((AeuiProbe*)d, "zoom", scale, 0.0, (int)m);
}
static void probe_rotate(GtkGestureRotate* g, double angle, gpointer d) {
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    probe_fire((AeuiProbe*)d, "rotate", angle, 0.0, (int)m);
}
static void probe_drag_begin(GtkGestureDrag* g, double x, double y, gpointer d) {
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    probe_fire((AeuiProbe*)d, "drag-begin", x, y, (int)m);
}
static void probe_drag_update(GtkGestureDrag* g, double x, double y, gpointer d) {
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    probe_fire((AeuiProbe*)d, "drag-update", x, y, (int)m);
}
static void probe_drag_end(GtkGestureDrag* g, double x, double y, gpointer d) {
    GdkModifierType m = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    probe_fire((AeuiProbe*)d, "drag-end", x, y, (int)m);
}

/* Raw tap: GDK_TOUCHPAD_PINCH / _SWIPE never reach the high-level gestures on
   some stacks, so report them directly rather than inferring their absence. */
static gboolean probe_legacy(GtkEventControllerLegacy* c, GdkEvent* ev, gpointer d) {
    (void)c;
    GdkEventType t = gdk_event_get_event_type(ev);
    if (t == GDK_TOUCHPAD_PINCH) {
        probe_fire((AeuiProbe*)d, "touchpad-pinch",
                   gdk_touchpad_event_get_pinch_scale(ev),
                   gdk_touchpad_event_get_pinch_angle_delta(ev), 0);
    } else if (t == GDK_TOUCHPAD_SWIPE) {
        double dx = 0, dy = 0;
        gdk_touchpad_event_get_deltas(ev, &dx, &dy);
        probe_fire((AeuiProbe*)d, "touchpad-swipe", dx, dy, 0);
    }
    return FALSE;
}

void aether_ui_canvas_gesture_probe_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    AeuiProbe* p = g_new0(AeuiProbe, 1);
    p->cl = (AeClosure*)boxed_closure;

    GtkEventControllerScroll* sc = GTK_EVENT_CONTROLLER_SCROLL(
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES));
    g_signal_connect(sc, "scroll", G_CALLBACK(probe_scroll), p);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(sc));

    GtkGesture* zm = gtk_gesture_zoom_new();
    g_signal_connect(zm, "scale-changed", G_CALLBACK(probe_zoom), p);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(zm));

    GtkGesture* rot = gtk_gesture_rotate_new();
    g_signal_connect(rot, "angle-changed", G_CALLBACK(probe_rotate), p);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(rot));

    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(probe_drag_begin), p);
    g_signal_connect(drag, "drag-update", G_CALLBACK(probe_drag_update), p);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(probe_drag_end), p);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(drag));

    GtkEventController* legacy = gtk_event_controller_legacy_new();
    g_signal_connect(legacy, "event", G_CALLBACK(probe_legacy), p);
    gtk_widget_add_controller(w, legacy);
}

// ── canvas scroll (wheel / two-finger) ──────────────────────────────
// Same GtkEventControllerScroll shape as the vlist one below, but on a
// CANVAS widget and reporting the raw delta rather than a +/-1 step: a zoom
// wants direction, and a caller that only needs a step can sign it itself.
static gboolean on_canvas_scroll(GtkEventControllerScroll* c, double dx,
                                 double dy, gpointer data) {
    (void)c;
    AeClosure* cl = (AeClosure*)data;
    if (cl && cl->fn)
        ((void(*)(void*, double, double))cl->fn)(cl->env, dx, dy);
    return TRUE;
}

void aether_ui_canvas_on_scroll_impl(int canvas_id, void* boxed_closure) {
    int wh = aether_ui_canvas_get_widget(canvas_id);
    GtkWidget* w = aether_ui_get_widget(wh);
    if (!w || !boxed_closure) return;
    /* VERTICAL only, matching the vlist controller below.
       BOTH_AXES was tried first and is WRONG here: it also delivers kinetic
       (deceleration) deltas, which under Xvfb arrive unprompted -- the turtle
       app's zoom drifted 100% -> 42.4% in 3 seconds of complete idleness,
       which is 1.1^-9, i.e. nine phantom callbacks. A zoom is a discrete user
       gesture; it must not be driven by momentum events nobody asked for. */
    GtkEventControllerScroll* sc = GTK_EVENT_CONTROLLER_SCROLL(
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL));
    g_signal_connect(sc, "scroll", G_CALLBACK(on_canvas_scroll), boxed_closure);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(sc));
    g_object_set_data(G_OBJECT(w), "aeui-canvas-scroll", boxed_closure);
}

void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) {
    GtkWidget* box = aether_ui_get_widget(container_handle);
    if (!box) return;
    GtkEventControllerScroll* sc = GTK_EVENT_CONTROLLER_SCROLL(
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL));
    g_signal_connect(sc, "scroll", G_CALLBACK(on_vlist_scroll), on_scroll);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(sc));
    g_object_set_data(G_OBJECT(box), "aeui-vscroll", on_scroll);
}

// Driver hook: fire the container's on_scroll(dy) headlessly (a real wheel
// event needs a seat). dy>0 scrolls toward the end.
int aether_ui_fire_scroll(int container_handle, int dy) {
    GtkWidget* box = aether_ui_get_widget(container_handle);
    if (!box) return 0;
    AeClosure* c = (AeClosure*)g_object_get_data(G_OBJECT(box), "aeui-vscroll");
    if (!c || !c->fn) return 0;
    ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)dy);
    return 1;
}

// GtkGestureClick "released" marshals (gesture, n_press, x, y, user_data) —
// a DIFFERENT signature from GtkButton "clicked" (button, user_data). Wiring
// on_button_clicked here made the handler read n_press (1) as its closure
// pointer and dereference it: `data=0x1`, SIGSEGV. It only bit where a
// gesture actually fires during presentation (Wayland realize/present timing
// differs from X11 — which is why the X11-only Xvfb spec matrix never saw it).
static void on_gesture_click_released(GtkGestureClick* gesture, int n_press,
                                      double x, double y, gpointer data) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
}

// Click handler (single click on any widget, not just buttons)
void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    GtkGesture* gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
    g_signal_connect(gesture, "released",
        G_CALLBACK(on_gesture_click_released), boxed_closure);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(gesture));
    // Remember the closure so the AetherUIDriver's /widget/{id}/click can
    // fire gesture-based handlers on NON-buttons (listbox rows are plain
    // containers). Same philosophy as emitting "clicked" on a GtkButton:
    // invoke the handler the user's click would run, not a fake input event.
    g_object_set_data(G_OBJECT(w), "aeui-click-closure", boxed_closure);
}

// CSS class add/remove — the selection-visual primitive (listbox rows toggle
// .aui-row-selected). Chrome CSS installs on first use (idempotent).
//
// A space-joined copy of the classes set THROUGH THIS API is mirrored into
// gobject data ("aeui-classes") so widget_to_json can report them WITHOUT
// calling gtk_widget_get_css_classes: the test server serializes widgets on
// its own thread, and the CSS-class getter walks/allocates in GTK's css-node
// machinery — off-thread that corrupts the global parent cache and GTK
// aborts at teardown (gtkcssnode.c:321 "node->cache == NULL", reproduced by
// spec-driven /widgets polling). The mirror is a plain strdup'd string — the
// same read-only-racy-but-inert class as the other JSON fields.
static void aeui_track_class(GtkWidget* w, const char* cls, int add) {
    const char* cur = (const char*)g_object_get_data(G_OBJECT(w), "aeui-classes");
    GString* out = g_string_new("");
    if (cur && cur[0]) {
        char** parts = g_strsplit(cur, " ", -1);
        for (int i = 0; parts[i]; i++) {
            if (!parts[i][0]) continue;
            if (strcmp(parts[i], cls) == 0) continue;   // drop; re-added below if add
            if (out->len) g_string_append_c(out, ' ');
            g_string_append(out, parts[i]);
        }
        g_strfreev(parts);
    }
    if (add) {
        if (out->len) g_string_append_c(out, ' ');
        g_string_append(out, cls);
    }
    g_object_set_data_full(G_OBJECT(w), "aeui-classes",
                           g_strdup(out->str), g_free);
    g_string_free(out, TRUE);
}

// Public CSS-fragment ABI (ui.style_shadow and friends): route through the
// same per-widget class+provider machinery the style_* setters use.
void aether_ui_widget_apply_css_impl(int handle, const char* property_css) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !property_css || !property_css[0]) return;
    // Stash an explicit opacity for driver readback (AeCS st_opacity proof) —
    // style_opacity routes through here as "opacity: X;". +1 encoding so an
    // explicit 0.0 differs from never-set.
    if (strncmp(property_css, "opacity:", 8) == 0) {
        double v = atof(property_css + 8);
        g_object_set_data(G_OBJECT(w), "aeui-styled-opacity",
                          GINT_TO_POINTER((int)(v * 100.0) + 1));
    }
    aether_ui_apply_css(handle, w, property_css);
}

int aether_ui_styled_opacity_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-styled-opacity"));
    return v > 0 ? v - 1 : -1;
}

// ── AeCS appearance change (GTK4) ───────────────────────────────────
// Real-OS path: GtkSettings' prefer-dark notify → invoke the registered
// |dark| callback on the GTK thread. Driver path: fire_appearance sets the
// override and marshals the invoke onto the GTK thread via idle.
static void on_prefer_dark_notify(GObject* obj, GParamSpec* pspec, gpointer d) {
    (void)obj; (void)pspec; (void)d;
    aether_ui_appearance_invoke(aether_ui_dark_mode_check());
}

void aether_ui_watch_appearance_impl(void) {
    static int watched = 0;
    if (watched) return;
    watched = 1;
    ensure_gtk_init();
    GtkSettings* settings = gtk_settings_get_default();
    if (settings)
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
                         G_CALLBACK(on_prefer_dark_notify), NULL);
}

static gboolean fire_appearance_idle(gpointer data) {
    aether_ui_appearance_invoke(GPOINTER_TO_INT(data));
    return G_SOURCE_REMOVE;
}

// Undo/redo driver fire — marshalled to the GTK thread (edit closures
// mutate widgets); the HTTP thread spins on done like the other idles.
/* HEAP-allocated, and owned by whoever gives up on it last. The waiter used
   to pass a STACK struct and spin `while (!r.done)` forever; if the idle never
   ran, the HTTP thread hung and the app kept the driver port open until
   killed. Observed 2026-08-17: undo_demo held port 9222 for 15 minutes
   mid-matrix (main thread in hrtimer_nanosleep, i.e. the g_usleep), which
   stalls every LATER suite -- the aeci ask filed the same hang from a fresh
   headless box.

   Bounding the wait means the waiter can return while the callback is still
   queued, so the request cannot live on the waiter's stack. `abandoned` is the
   handshake: whichever side finishes second frees it. */
typedef struct {
    int redo; int did; volatile int done; volatile int abandoned;
} AeuiUndoReq;
static gboolean aeui_undo_idle(gpointer data) {
    AeuiUndoReq* r = (AeuiUndoReq*)data;
    if (r->abandoned) { g_free(r); return G_SOURCE_REMOVE; }
    r->did = r->redo ? aether_ui_redo_step_impl() : aether_ui_undo_step_impl();
    r->done = 1;
    return G_SOURCE_REMOVE;
}
static int aeui_fire_undo_redo(int redo) {
    /* CRITICAL: never queue-and-wait from the thread that runs the loop.
     *
     * This backend supplies run_on_ui_thread, and the driver services the
     * WHOLE request over there, so /undo already arrives on the GTK thread.
     * Posting an idle and then blocking here for it cannot complete: the
     * callback only runs when this thread returns to the main loop, and this
     * thread is the one sleeping. It spun the full 2s and answered
     * {"did":0,...} with undo_depth 2, an honest-looking "nothing to undo"
     * for a stack that had two entries. Ctrl+Z kept working throughout,
     * because that path never leaves the UI thread.
     *
     * Already on the loop's thread means the step can just be taken. */
    if (g_main_context_is_owner(g_main_context_default())) {
        return redo ? aether_ui_redo_step_impl() : aether_ui_undo_step_impl();
    }
    AeuiUndoReq* r = g_new0(AeuiUndoReq, 1);
    r->redo = redo;
    g_idle_add(aeui_undo_idle, r);
    /* ~2s at 1ms. An undo step is a closure call on an idle callback; if the
       loop has not serviced it by then it is not going to, and answering the
       driver beats hanging it. */
    int spins = 0;
    while (!r->done && spins < 2000) { g_usleep(1000); spins++; }
    if (!r->done) {
        r->abandoned = 1;      /* the idle frees it if it ever runs */
        return 0;              /* "nothing was undone" -- honest, and 200s */
    }
    int did = r->did;
    g_free(r);
    return did;
}
int aether_ui_fire_undo(void) { return aeui_fire_undo_redo(0); }
int aether_ui_fire_redo(void) { return aeui_fire_undo_redo(1); }

int aether_ui_fire_appearance(int dark) {
    aether_ui_appearance_override_set(dark ? 1 : 0);
    // The callback re-themes live widgets — GTK thread only.
    g_idle_add(fire_appearance_idle, GINT_TO_POINTER(dark ? 1 : 0));
    return 1;
}

void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !cls || !cls[0]) return;
    aeui_overlay_css_once();
    gtk_widget_add_css_class(w, cls);
    aeui_track_class(w, cls, 1);
}

void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !cls || !cls[0]) return;
    gtk_widget_remove_css_class(w, cls);
    aeui_track_class(w, cls, 0);
}

// Animation — opacity and position


// Widget removal: the mirror of aether_ui_widget_add_child_ctx / grid_place.
//
// CRITICAL: every container arm that can ADD a child needs a removal arm here.
// A missing arm is silent: the child stays parented and stays alive, so the
// widget keeps rendering AND the driver keeps reporting it (the registry slot
// is cleared by the weak ref on finalize, which never runs while the parent
// still holds the last reference).
// Drop `w` and every descendant from the widget registry.
//
// CRITICAL: do NOT leave this to the g_object_weak_ref planted at
// registration. That only fires on finalize, and a removed widget is not
// necessarily finalized: GTK can still hold a reference (a pending unrealize,
// the focus chain), and then the driver keeps listing the widget as visible
// and keeps firing its callback from a stale id. macOS (unregister_view_tree)
// and win32 (mark_subtree_dead) both do this explicitly; so does this.
static void unregister_widget_tree(GtkWidget* w) {
    if (!w) return;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; ) {
        GtkWidget* next = gtk_widget_get_next_sibling(c);
        unregister_widget_tree(c);
        c = next;
    }
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == w) { widgets[i] = NULL; break; }
    }
}

static void aeui_container_remove(GtkWidget* parent, GtkWidget* child) {
    if (GTK_IS_BOX(parent)) {
        gtk_box_remove(GTK_BOX(parent), child);
    } else if (GTK_IS_GRID(parent)) {
        gtk_grid_remove(GTK_GRID(parent), child);
    } else if (GTK_IS_FLOW_BOX(parent)) {
        // gtk_flow_box_insert wraps each child in a GtkFlowBoxChild, so the
        // widget walked out of the parent is the wrapper, not the handle's
        // widget. gtk_flow_box_remove accepts either.
        gtk_flow_box_remove(GTK_FLOW_BOX(parent), child);
    } else if (GTK_IS_SCROLLED_WINDOW(parent)) {
        if (gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(parent)) == child) {
            gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(parent), NULL);
        }
    } else if (GTK_IS_OVERLAY(parent)) {
        if (gtk_overlay_get_child(GTK_OVERLAY(parent)) == child) {
            gtk_overlay_set_child(GTK_OVERLAY(parent), NULL);
        } else {
            gtk_overlay_remove_overlay(GTK_OVERLAY(parent), child);
        }
    } else if (GTK_IS_PANED(parent)) {
        if (gtk_paned_get_start_child(GTK_PANED(parent)) == child) {
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
        } else if (gtk_paned_get_end_child(GTK_PANED(parent)) == child) {
            gtk_paned_set_end_child(GTK_PANED(parent), NULL);
        }
    } else {
        gtk_widget_unparent(child);
    }
}

void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    GtkWidget* parent = aether_ui_get_widget(parent_handle);
    GtkWidget* child = aether_ui_get_widget(child_handle);
    if (!parent || !child) return;
    if (gtk_widget_get_parent(child) != parent) return;
    unregister_widget_tree(child);
    aeui_container_remove(parent, child);
}

void aether_ui_clear_children_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(w)) != NULL) {
        unregister_widget_tree(child);
        aeui_container_remove(w, child);
        // An arm that does not actually unparent would spin here forever.
        if (gtk_widget_get_first_child(w) == child) {
            gtk_widget_unparent(child);
            if (gtk_widget_get_first_child(w) == child) return;
        }
    }
}

// ---------------------------------------------------------------------------
// AetherUIDriver — Selenium-like AetherUIDriver
//
// An embedded HTTP server for Selenium-like remote control. When enabled:
//   1. A "Under Remote Control" banner is injected into the root — it cannot
//      be dismissed or hidden via the test API.
//   2. Widgets marked with seal_widget() are non-automatable — the test server
//      returns 403 Forbidden for any action on sealed widgets.
//   3. A simple HTTP API exposes widget state and actions.
//
// Endpoints:
//   GET  /widgets                    — list all widget handles + types (JSON)
//   GET  /widget/{id}               — widget state (type, text, value, visible)
//   POST /widget/{id}/click         — simulate button click
//   POST /widget/{id}/set_text?v=X  — set text/textfield value
//   POST /widget/{id}/toggle        — toggle a checkbox
//   POST /widget/{id}/set_value?v=X — set slider value
//   GET  /state/{id}                — get reactive state value
//   POST /state/{id}/set?v=X        — set reactive state value
//   POST /canvas/{id}/click?x=&y=   — click a canvas at (x,y); hit-tests AeVG shapes
//   POST /canvas/{id}/move?x=&y=    — pointer-move (hover) at (x,y)
//   POST /canvas/{id}/release?x=&y= — pointer-release (completes a drag)
//   POST /canvas/{id}/key?name=X    — key press by GDK name ("Down", "Return", …)
//   POST /window/resize?w=&h=       — resize the app's top-level window
//   POST /shutdown                  — close the app window; the process exits
//                                     cleanly (same path as the user closing
//                                     it). Harnesses use this instead of
//                                     signal-killing xvfb-run wrappers, which
//                                     leaves the app alive holding the port.
// ---------------------------------------------------------------------------

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
// The SHARED driver's hook contract. GTK4 fills AetherDriverHooks below;
// aether_ui_test_server.c is not yet compiled into this backend, so only the
// type declarations are used for now (docs/design/one-driver-not-two.md).
#include "aether_ui_test_server.h"

// Sealed widgets — test server refuses to interact with these.
static int* sealed_widgets = NULL;
static int sealed_count = 0;
static int sealed_capacity = 0;

void aether_ui_seal_widget_impl(int handle) {
    if (sealed_count >= sealed_capacity) {
        sealed_capacity = sealed_capacity == 0 ? 32 : sealed_capacity * 2;
        sealed_widgets = realloc(sealed_widgets, sizeof(int) * sealed_capacity);
    }
    sealed_widgets[sealed_count++] = handle;
    // Mirror into the SHARED server's own sealed registry, which is what
    // enforces the 403 once GTK4 runs on it (macOS does the same, and only
    // this). Kept as a mirror rather than a replacement so the embedded
    // server -- still reachable via AETHER_UI_GTK_SERVER=embedded -- keeps
    // refusing the same widgets. A sealed widget that silently became
    // clickable would be the worst possible regression from this migration.
    aether_ui_test_server_seal_widget(handle);
}

// Walk GTK widget tree and seal every descendant.
static void seal_subtree_recursive(GtkWidget* w) {
    if (!w) return;
    // Find this widget's handle in the registry
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == w) {
            aether_ui_seal_widget_impl(i + 1);
            break;
        }
    }
    // Recurse into children
    for (GtkWidget* child = gtk_widget_get_first_child(w);
         child; child = gtk_widget_get_next_sibling(child)) {
        seal_subtree_recursive(child);
    }
}

void aether_ui_seal_subtree_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) seal_subtree_recursive(w);
}

static int is_widget_sealed(int handle) {
    for (int i = 0; i < sealed_count; i++) {
        if (sealed_widgets[i] == handle) return 1;
    }
    return 0;
}

// Find the registry handle for a GtkWidget, or 0 if not registered.
static int handle_for_widget(GtkWidget* w) {
    if (!w) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == w) return i + 1;
    }
    return 0;
}

// Find parent handle for a widget by walking the GTK widget tree. Skips
// GTK-internal wrapper widgets the app never created (GtkStack inside a
// tabs composite, GtkViewport inside a scrollview, etc.) so a tab page's
// parent resolves to the registered tabs box, not to an unregistered
// intermediary that would report 0 and orphan the subtree.
static int parent_handle_for(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    GtkWidget* parent = gtk_widget_get_parent(w);
    while (parent) {
        int h = handle_for_widget(parent);
        if (h) return h;
        parent = gtk_widget_get_parent(parent);
    }
    return 0;
}

// Banner widget handle — protected from the test API.
static int banner_handle = 0;

static const char* widget_type_name(GtkWidget* w) {
    if (!w) return "null";
    if (GTK_IS_POPOVER_MENU_BAR(w)) return "menubar";
    if (GTK_IS_LABEL(w)) return "text";
    if (drawn_picker_of(w)) return "picker";   // drawn picker is a GtkButton
    if (GTK_IS_BUTTON(w)) return "button";
    if (GTK_IS_ENTRY(w)) return "textfield";
    if (GTK_IS_PASSWORD_ENTRY(w)) return "securefield";
    if (GTK_IS_CHECK_BUTTON(w)) return "toggle";
    if (GTK_IS_SCALE(w)) return "slider";
    if (GTK_IS_DROP_DOWN(w)) return "picker";
    if (GTK_IS_TEXT_VIEW(w)) return "textarea";
    if (GTK_IS_PROGRESS_BAR(w)) return "progressbar";
    if (GTK_IS_SEPARATOR(w)) return "divider";
    if (GTK_IS_SCROLLED_WINDOW(w)) return "scrollview";
    if (GTK_IS_PANED(w)) return "splitview";
    if (GTK_IS_FLOW_BOX(w)) return "wrap";
    if (GTK_IS_FLOW_BOX_CHILD(w)) return "wrapitem";
    // A tabs composite is a GtkBox carrying our aeui-tabs state — check
    // before the generic GTK_IS_BOX vstack/hstack fallback below.
    if (GTK_IS_BOX(w) && g_object_get_data(G_OBJECT(w), "aeui-tabs")) return "tabs";
    if (GTK_IS_OVERLAY(w)) return "zstack";
    if (GTK_IS_DRAWING_AREA(w)) return "canvas";
    if (GTK_IS_IMAGE(w) || GTK_IS_PICTURE(w)) return "image";
    if (GTK_IS_BOX(w)) {
        GtkOrientation o = aeui_box_orientation(w);
        return o == GTK_ORIENTATION_VERTICAL ? "vstack" : "hstack";
    }
    return "widget";
}

// ── Stylesheet-walk ABI (ui.apply_styles) ───────────────────────────
// The DSL's tree walk runs in-process: count + kind + parent + classes per
// widget, and packed-color readback so the driver can prove a restyle.
int aether_ui_widget_count_impl(void) { return widget_count; }

const char* aether_ui_widget_kind_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "";
    const char* t = widget_type_name(w);
    return t ? t : "";
}

int aether_ui_widget_parent_impl(int handle) {
    return parent_handle_for(handle);
}

const char* aether_ui_widget_classes_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "";
    const char* cls = (const char*)g_object_get_data(G_OBJECT(w), "aeui-classes");
    return cls ? cls : "";
}

int aether_ui_styled_bg_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    /* Answer for the state the widget is ACTUALLY in. The :hover and :active
       rules set_state_style installs really do paint -- the driver's hover
       sets GTK_STATE_FLAG_PRELIGHT exactly as a pointer would -- so a readback
       that only ever reported the resting colour described a widget that was
       not on screen. Active beats hover, matching the CSS cascade and the
       AppKit backend. Widgets with no state style fall through unchanged. */
    GtkStateFlags f = gtk_widget_get_state_flags(w);
    int st = 0;
    if (f & GTK_STATE_FLAG_ACTIVE)
        st = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-active-bg"));
    if (!(st & 0x1000000) && (f & GTK_STATE_FLAG_PRELIGHT))
        st = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-hover-bg"));
    if (st & 0x1000000) return st & 0xFFFFFF;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-styled-bg"));
    return (v & 0x1000000) ? (v & 0xFFFFFF) : -1;
}

int aether_ui_styled_fg_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aeui-styled-fg"));
    return (v & 0x1000000) ? (v & 0xFFFFFF) : -1;
}

// GET /window/pick?x=&y= — real GTK hit-test at a window-local point. Returns
// the topmost widget's type + registered handle. This is the honest test of
// overlay input policy: with a modal scrim up, a pick at a button's location
// resolves to the scrim (a full-window widget above the app), NOT the button —
// proving the glass pane eats clicks. Unlike /widget/{id}/click (which invokes
// the handler directly, bypassing hit-testing), pick sees the real z-order.
typedef struct { int canvas_id, x, y, w, h, result, done; } PixelReq;

static gboolean pixel_req_idle(gpointer data) {
    PixelReq* rq = (PixelReq*)data;
    rq->result = aether_ui_canvas_read_pixel_impl(rq->canvas_id, rq->x, rq->y,
                                                  rq->w, rq->h);
    rq->done = 1;
    return G_SOURCE_REMOVE;
}

typedef struct {
    int canvas_id;
    int area;
    int count;
    int w;
    int h;
    int full_paints;
    int clip_paints;
    int last_clip_area;
    int painted_pixels;
    int done;
} CanvasDebugReq;

static gboolean canvas_debug_req_idle(gpointer data) {
    CanvasDebugReq* rq = (CanvasDebugReq*)data;
    CanvasState* cs = get_canvas_state(rq->canvas_id);
    rq->area = cs ? cs->last_paint_area : -1;
    rq->count = cs ? cs->last_paint_count : -1;
    rq->w = cs ? cs->last_paint_w : -1;
    rq->h = cs ? cs->last_paint_h : -1;
    rq->full_paints = cs ? cs->paint_full_count : 0;
    rq->clip_paints = cs ? cs->paint_clip_count_total : 0;
    rq->last_clip_area = cs ? cs->last_clip_area : 0;
    rq->painted_pixels = aether_ui_canvas_painted_pixels_impl(rq->canvas_id);
    rq->done = 1;
    return G_SOURCE_REMOVE;
}

typedef struct { int done; int handle; char type[32]; } FocusQuery;

// Close a window on the GTK thread (window ops must not run off it).
// Overlay dismissal from the driver. Closing an overlay touches widgets, so
// it must run on the GTK main thread like every other mutating route.
// win32/macOS answered POST /overlay/{id}/dismiss long before GTK4 did — the
// route-parity spec exists because that gap was invisible.
typedef struct { int overlay_handle; int done; } OverlayDismissReq;
static gboolean overlay_dismiss_idle(gpointer data) {
    OverlayDismissReq* od = (OverlayDismissReq*)data;
    aether_ui_overlay_close_impl(od->overlay_handle);
    od->done = 1;
    return G_SOURCE_REMOVE;
}

typedef struct { int win_handle; int done; } WindowCloseReq;
static gboolean window_close_idle(gpointer data) {
    WindowCloseReq* wc = (WindowCloseReq*)data;
    aether_ui_close_window_by_handle_impl(wc->win_handle);
    wc->done = 1;
    return G_SOURCE_REMOVE;
}

// The registry handle of the currently keyboard-focused widget, or 0. Safe to
// call on the GTK main thread (where shortcuts fire) — no idle marshaling.
// Walks up from GTK's focus target to a tracked widget (GTK focuses the GtkText
// inside a GtkEntry; we want the entry's handle).
int aether_ui_focused_widget(void) {
    if (!primary_window) return 0;
    GtkWidget* f = gtk_window_get_focus(primary_window);
    while (f) {
        int h = handle_for_widget(f);
        if (h > 0) return h;
        f = gtk_widget_get_parent(f);
    }
    return 0;
}

static gboolean focus_query_idle(gpointer data) {
    FocusQuery* fq = (FocusQuery*)data;
    fq->handle = 0;
    strcpy(fq->type, "none");
    if (primary_window) {
        GtkWidget* f = gtk_window_get_focus(primary_window);
        // The registry tracks OUR widgets; GTK may focus an internal child
        // (e.g. the GtkText inside a GtkEntry) — walk up to a tracked one.
        while (f) {
            int h = handle_for_widget(f);
            if (h > 0) {
                fq->handle = h;
                snprintf(fq->type, sizeof(fq->type), "%s", widget_type_name(f));
                break;
            }
            f = gtk_widget_get_parent(f);
        }
    }
    fq->done = 1;
    return G_SOURCE_REMOVE;
}

typedef struct { double x, y; int done; int handle; char type[32]; int on_scrim; } PickAction;

static gboolean window_pick_idle(gpointer data) {
    PickAction* pa = (PickAction*)data;
    pa->handle = 0; pa->on_scrim = 0;
    strcpy(pa->type, "none");
    if (primary_window) {
        GtkWidget* picked = gtk_widget_pick(GTK_WIDGET(primary_window),
                                            pa->x, pa->y, GTK_PICK_DEFAULT);
        // Walk up to the first widget we can name / that is a scrim.
        for (GtkWidget* w = picked; w; w = gtk_widget_get_parent(w)) {
            if (gtk_widget_has_css_class(w, "aui-overlay-scrim")) {
                pa->on_scrim = 1;
                strcpy(pa->type, "scrim");
                break;
            }
            int h = handle_for_widget(w);
            if (h > 0) {
                pa->handle = h;
                strncpy(pa->type, widget_type_name(w), sizeof(pa->type) - 1);
                break;
            }
        }
    }
    pa->done = 1;
    return G_SOURCE_REMOVE;
}

// Get widget text content (for text labels and text fields).
static const char* widget_text_content(GtkWidget* w) {
    if (!w) return "";
    /* A chrome-drawn face replaced this widget's native label child
       (aether_ui_widget_set_child_impl stashed the text first) — report
       the stashed label so /widgets and widget_id_by_text stay
       drawn-vs-native indistinguishable, the phase-2 parity contract. */
    const char* face = (const char*)g_object_get_data(G_OBJECT(w), "aeui-face-label");
    if (face) return face;
    if (GTK_IS_LABEL(w)) return gtk_label_get_text(GTK_LABEL(w));
    if (GTK_IS_ENTRY(w)) {
        GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
        return gtk_entry_buffer_get_text(buf);
    }
    if (GTK_IS_CHECK_BUTTON(w)) return gtk_check_button_get_label(GTK_CHECK_BUTTON(w));
    if (GTK_IS_BUTTON(w)) return gtk_button_get_label(GTK_BUTTON(w));
    // Native picker: report the selected item's string, so "text" tracks the
    // selection identically to the drawn picker (whose button label does).
    if (GTK_IS_DROP_DOWN(w)) {
        GListModel* model = gtk_drop_down_get_model(GTK_DROP_DOWN(w));
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
        if (GTK_IS_STRING_LIST(model) && idx != GTK_INVALID_LIST_POSITION)
            return gtk_string_list_get_string(GTK_STRING_LIST(model), idx);
    }
    return "";
}

// JSON-escape SRC into DST (NUL-terminated, never overflows DST). Mirrors the
// shared test server's escaper (aether_ui_test_server.c widget_to_json) so this
// GTK4 embedded server produces byte-identical JSON: " \ \n \r \t plus any
// other control char (U+0000–U+001F) as \u00XX. Without it a raw control byte
// or quote in widget text (e.g. a search-result title) yields invalid JSON that
// breaks client-side json.parse. (both-servers parity — found via LisMusic.)
// A \uXXXX expansion is 6 bytes; the ei < dstsize-8 guard leaves the headroom.
static void aeui_json_escape(const char* src, char* dst, int dstsize) {
    static const char hex[] = "0123456789abcdef";
    int ei = 0;
    for (int i = 0; src[i] && ei < dstsize - 8; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') { dst[ei++] = '\\'; dst[ei++] = (char)ch; }
        else if (ch == '\n') { dst[ei++] = '\\'; dst[ei++] = 'n'; }
        else if (ch == '\r') { dst[ei++] = '\\'; dst[ei++] = 'r'; }
        else if (ch == '\t') { dst[ei++] = '\\'; dst[ei++] = 't'; }
        else if (ch < 0x20) {
            dst[ei++] = '\\'; dst[ei++] = 'u';
            dst[ei++] = '0'; dst[ei++] = '0';
            dst[ei++] = hex[(ch >> 4) & 0xF];
            dst[ei++] = hex[ch & 0xF];
        }
        else dst[ei++] = (char)ch;
    }
    dst[ei] = '\0';
}

// Build JSON response for a single widget.
static int widget_to_json(int handle, char* buf, int bufsize) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) {
        return snprintf(buf, bufsize, "{\"id\":%d,\"type\":\"null\"}", handle);
    }
    const char* type = widget_type_name(w);
    const char* text = widget_text_content(w);
    char text_esc[2560];
    aeui_json_escape(text ? text : "", text_esc, (int)sizeof(text_esc));
    int visible = gtk_widget_get_visible(w) ? 1 : 0;
    int sealed = is_widget_sealed(handle) ? 1 : 0;
    int is_banner = (handle == banner_handle) ? 1 : 0;

    int enabled = gtk_widget_get_sensitive(w) ? 1 : 0;
    int n = snprintf(buf, bufsize,
        "{\"id\":%d,\"type\":\"%s\",\"text\":\"%s\",\"visible\":%s,\"sealed\":%s,\"banner\":%s,\"enabled\":%s",
        handle, type, text_esc,
        visible ? "true" : "false",
        sealed ? "true" : "false",
        is_banner ? "true" : "false",
        enabled ? "true" : "false");

    // Parent handle
    int parent_id = parent_handle_for(handle);
    n += snprintf(buf + n, bufsize - n, ",\"parent\":%d", parent_id);
    // Multi-window: which top-level window this widget lives in (1=primary,
    // 2..=extras, 0=unmounted). Lets a spec target/assert widgets per window.
    n += snprintf(buf + n, bufsize - n, ",\"window\":%d",
                  aether_ui_widget_window_impl(handle));

    // Interaction state, so a spec can assert hover/pressed without reading
    // pixels (and on a box with no pointer at all).
    {
        GtkStateFlags st = gtk_widget_get_state_flags(w);
        if (st & GTK_STATE_FLAG_PRELIGHT)
            n += snprintf(buf + n, bufsize - n, ",\"hovered\":true");
        if (st & GTK_STATE_FLAG_ACTIVE)
            n += snprintf(buf + n, bufsize - n, ",\"pressed\":true");
    }

    // Current allocation (0x0 until mapped). Tests use a canvas's real size
    // to compute the viewBox→pixel mapping after a /window/resize.
    n += snprintf(buf + n, bufsize - n, ",\"w\":%d,\"h\":%d",
                  gtk_widget_get_width(w), gtk_widget_get_height(w));

    // Window-local position (x,y) of the widget's top-left, for /window/pick
    // based tests. 0,0 until mapped or if the root isn't a window.
    {
        GtkRoot* root = gtk_widget_get_root(w);
        graphene_rect_t r;
        if (root && GTK_IS_WIDGET(root) &&
            gtk_widget_compute_bounds(w, GTK_WIDGET(root), &r)) {
            n += snprintf(buf + n, bufsize - n, ",\"x\":%d,\"y\":%d",
                          (int)r.origin.x, (int)r.origin.y);
        } else {
            n += snprintf(buf + n, bufsize - n, ",\"x\":0,\"y\":0");
        }
    }

    // Add type-specific values
    if (GTK_IS_CHECK_BUTTON(w)) {
        int active = gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? 1 : 0;
        n += snprintf(buf + n, bufsize - n, ",\"active\":%s", active ? "true" : "false");
    } else if (GTK_IS_SCALE(w)) {
        double val = gtk_range_get_value(GTK_RANGE(w));
        n += snprintf(buf + n, bufsize - n, ",\"value\":%.2f", val);
    } else if (GTK_IS_PROGRESS_BAR(w)) {
        double val = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(w));
        n += snprintf(buf + n, bufsize - n, ",\"value\":%.2f", val);
    } else if (GTK_IS_DROP_DOWN(w) || drawn_picker_of(w)) {
        // Picker selection (both surfaces) — "selected" for spec round-trips.
        n += snprintf(buf + n, bufsize - n, ",\"selected\":%d",
                      aether_ui_picker_get_selected(handle));
    } else if (GTK_IS_PANED(w)) {
        n += snprintf(buf + n, bufsize - n, ",\"splitPosition\":%d",
                      gtk_paned_get_position(GTK_PANED(w)));
    } else if (GTK_IS_BOX(w) && g_object_get_data(G_OBJECT(w), "aeui-tabs")) {
        n += snprintf(buf + n, bufsize - n,
                      ",\"tabSelected\":%d,\"tabCount\":%d",
                      aether_ui_tabs_selected(handle),
                      aether_ui_tabs_count(handle));
    } else if (GTK_IS_LABEL(w)) {
        static const char* an[] = {"start", "middle", "end"};
        n += snprintf(buf + n, bufsize - n, ",\"wrap\":%s,\"anchor\":\"%s\"",
                      aether_ui_text_get_wrap(handle) ? "true" : "false",
                      an[aether_ui_text_get_anchor(handle)]);
    }

    // CSS classes set via ui.add_css_class — specs assert selection visuals
    // (.aui-row-selected) from the tracked mirror. NEVER call
    // gtk_widget_get_css_classes here: this runs on the test-server thread
    // and that getter touches GTK css-node internals (see aeui_track_class).
    {
        const char* cls = (const char*)g_object_get_data(G_OBJECT(w), "aeui-classes");
        if (cls && cls[0]) {
            n += snprintf(buf + n, bufsize - n, ",\"classes\":\"%s\"", cls);
        }
    }

    // Explicitly-styled colors (stylesheet layer): emitted only when set, so a
    // spec can prove a theme swap restyled the backend, not just the model.
    {
        int bg = aether_ui_styled_bg_impl(handle);
        int fg = aether_ui_styled_fg_impl(handle);
        if (bg >= 0) n += snprintf(buf + n, bufsize - n, ",\"bg\":\"#%06x\"", bg);
        if (fg >= 0) n += snprintf(buf + n, bufsize - n, ",\"fg\":\"#%06x\"", fg);
        const char* fam = aether_ui_styled_font_family_impl(handle);
        const char* wt  = aether_ui_styled_weight_impl(handle);
        if (fam[0]) n += snprintf(buf + n, bufsize - n, ",\"fontFamily\":\"%s\"", fam);
        if (wt[0])  n += snprintf(buf + n, bufsize - n, ",\"fontWeight\":\"%s\"", wt);
        int op = aether_ui_styled_opacity_impl(handle);
        if (op >= 0) n += snprintf(buf + n, bufsize - n, ",\"opacity\":%.2f", op / 100.0);
        int bd = aether_ui_styled_border_impl(handle);
        if (bd >= 0) {
            n += snprintf(buf + n, bufsize - n,
                          ",\"borderWidth\":%d,\"borderColor\":\"#%06x\"",
                          (bd >> 24) & 0x3F, bd & 0xFFFFFF);
        }
        int hv = aether_ui_state_style_impl(handle, 0);
        int ac = aether_ui_state_style_impl(handle, 1);
        if (hv >= 0) n += snprintf(buf + n, bufsize - n, ",\"hoverBg\":\"#%06x\"", hv);
        if (ac >= 0) n += snprintf(buf + n, bufsize - n, ",\"activeBg\":\"#%06x\"", ac);
    }

    // Accessibility: effective role + accessible name (emitted only when
    // present, so existing specs are unaffected).
    {
        char role[64] = "", name[512] = "", desc[512] = "";
        aether_ui_a11y_get_impl(handle, role, sizeof(role), name, sizeof(name),
                                desc, sizeof(desc));
        if (role[0]) n += snprintf(buf + n, bufsize - n, ",\"role\":\"%s\"", role);
        if (name[0]) {
            char ne[1040];
            aeui_json_escape(name, ne, (int)sizeof(ne));
            n += snprintf(buf + n, bufsize - n, ",\"a11y_name\":\"%s\"", ne);
        }
    }

    n += snprintf(buf + n, bufsize - n, "}");
    return n;
}

// Parse a simple HTTP request: "GET /path HTTP/1.1\r\n..."
// Returns the method (0=GET, 1=POST) and path.
static int parse_http_request(const char* req, char* path, int pathsize) {
    int method = 0;
    if (strncmp(req, "POST", 4) == 0) method = 1;
    const char* p = strchr(req, ' ');
    if (!p) return -1;
    p++;
    const char* end = strchr(p, ' ');
    if (!end) end = strchr(p, '\r');
    if (!end) end = p + strlen(p);
    int len = (int)(end - p);
    if (len >= pathsize) len = pathsize - 1;
    memcpy(path, p, len);
    path[len] = '\0';
    return method;
}

// Extract integer from path like "/widget/5/click" → 5
static int extract_id_from_path(const char* path, const char* prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return -1;
    return atoi(path + plen);
}

// Extract query parameter: "/widget/5/set_text?v=hello" → "hello"
static const char* extract_query_param(const char* path, const char* key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char* p = strstr(path, needle);
    if (!p) return NULL;
    return p + strlen(needle);
}

// Data passed to the GTK idle callback for thread-safe widget interaction.
typedef struct {
    int action;  // 0=click 1=set_text 2=toggle 3=set_value 4=set_state
                 // 5=ctx_open 6=ctx_activate 7=split_position 8=focus
                 // 9=window_key 10=tab_select
    int handle;
    double dval;
    int ival;    // ctx item index (action 6)
    char sval[512];
    int done;
    int result;  // 0=ok, 1=sealed, 2=banner, 3=not_found
    int retval;  // action-specific return (ctx: mapped/count)
} TestAction;

static gboolean test_action_idle(gpointer data) {
    TestAction* ta = (TestAction*)data;
    GtkWidget* w = aether_ui_get_widget(ta->handle);

    if (ta->action == 14 && ta->handle == 0) {
        // hover(0) = "the pointer is over NOTHING" — clear every hover.
        // A real widget id is not required for this one.
        int n0 = aether_ui_widget_count_impl();
        for (int i = 1; i <= n0; i++) {
            GtkWidget* pw0 = aether_ui_get_widget(i);
            if (pw0) gtk_widget_unset_state_flags(pw0, GTK_STATE_FLAG_PRELIGHT);
        }
        ta->retval = 1;
        ta->result = 0;
        ta->done = 1;
        return G_SOURCE_REMOVE;
    }

    if (ta->action == 9) {
        // Window key — not a widget action; fire the combo through the
        // shortcut dispatch (or Tab/Escape special handling).
        ta->retval = aeui_window_key_fire(ta->sval);
        ta->result = 0;
        ta->done = 1;
        return G_SOURCE_REMOVE;
    }

    if (ta->action == 4) {
        // State set — not a widget action. Dispatch on the cell's type
        // (sval carries the raw v= for typed cells; typed setters walk
        // bindings, hence this GTK-thread hop).
        switch (aether_ui_state_type(ta->handle)) {
            case 1: aether_ui_state_set_i(ta->handle, atoi(ta->sval)); break;
            case 2: aether_ui_state_set_b(ta->handle,
                        (strcmp(ta->sval, "true") == 0 || atoi(ta->sval) != 0)); break;
            case 3: aether_ui_state_set_s(ta->handle, ta->sval); break;
            default: aether_ui_state_set(ta->handle, ta->dval);
        }
        ta->result = 0;
        ta->done = 1;
        return G_SOURCE_REMOVE;
    }

    if (!w) { ta->result = 3; ta->done = 1; return G_SOURCE_REMOVE; }
    if (ta->handle == banner_handle) { ta->result = 2; ta->done = 1; return G_SOURCE_REMOVE; }
    if (is_widget_sealed(ta->handle)) { ta->result = 1; ta->done = 1; return G_SOURCE_REMOVE; }

    switch (ta->action) {
        case 0: { // click
            if (GTK_IS_BUTTON(w)) {
                g_signal_emit_by_name(w, "clicked");
            } else {
                // Non-buttons with an on_click gesture (listbox rows, any
                // container): invoke the registered closure directly — the
                // handler a real click would run (see aeui-click-closure).
                AeClosure* cc = (AeClosure*)g_object_get_data(G_OBJECT(w),
                                                              "aeui-click-closure");
                if (cc && cc->fn) ((void(*)(void*))cc->fn)(cc->env);
            }
            break;
        }
        case 1: // set_text
            if (GTK_IS_LABEL(w)) {
                gtk_label_set_text(GTK_LABEL(w), ta->sval);
            } else if (GTK_IS_ENTRY(w)) {
                GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
                gtk_entry_buffer_set_text(buf, ta->sval, -1);
            }
            break;
        case 2: // toggle
            if (GTK_IS_CHECK_BUTTON(w)) {
                gboolean cur = gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
                gtk_check_button_set_active(GTK_CHECK_BUTTON(w), !cur);
            }
            break;
        case 3: // set_value
            if (GTK_IS_SCALE(w)) {
                gtk_range_set_value(GTK_RANGE(w), ta->dval);
            } else if (GTK_IS_PROGRESS_BAR(w)) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), ta->dval);
            } else if (GTK_IS_DROP_DOWN(w) || drawn_picker_of(w)) {
                // Picker (either surface): select index via the ABI, which
                // fires the change callback exactly as a user pick does.
                aether_ui_picker_set_selected(ta->handle, (int)ta->dval);
            }
            break;
        case 5: // ctx_open — open the right-click menu, report mapped state
            ta->retval = aeui_ctx_menu_open(ta->handle);
            break;
        case 6: // ctx_activate — fire item[ival]'s closure
            ta->retval = aeui_ctx_menu_activate(ta->handle, ta->ival);
            break;
        case 7: // split_position — move the splitter (px ignored if < 0)
            if (GTK_IS_PANED(w) && ta->ival >= 0) {
                gtk_paned_set_position(GTK_PANED(w), ta->ival);
            }
            ta->retval = GTK_IS_PANED(w)
                ? gtk_paned_get_position(GTK_PANED(w)) : -1;
            break;
        case 8: // focus — grab keyboard focus
            ta->retval = gtk_widget_grab_focus(w) ? 1 : 0;
            break;
        case 10: // tab_select — activate tab[ival], report the resulting index
            aether_ui_tabs_select(ta->handle, ta->ival);
            ta->retval = aether_ui_tabs_selected(ta->handle);
            break;
        case 11: // double_click — fire the widget's dbl-click closure
            ta->retval = aether_ui_fire_double_click(ta->handle);
            break;
        case 12: // drop — fire this row's on_drop(src_index)
            ta->retval = aether_ui_fire_row_drop(ta->handle, ta->ival);
            break;
        case 13: // scroll — fire the vlist container's on_scroll(dy)
            ta->retval = aether_ui_fire_scroll(ta->handle, ta->ival);
            break;
        case 14: {  // hover — set the CSS :hover state as a pointer would
            // GTK4 drives :hover from widget state flags, so a spec can
            // assert hover styling with no real pointer (headless CI).
            // Clear any previous hover first: only one widget is "under
            // the pointer" at a time, same as reality.
            int n = aether_ui_widget_count_impl();
            for (int i = 1; i <= n; i++) {
                GtkWidget* pw = aether_ui_get_widget(i);
                if (pw) gtk_widget_unset_state_flags(pw, GTK_STATE_FLAG_PRELIGHT);
            }
            GtkWidget* hw = aether_ui_get_widget(ta->handle);
            if (hw) {
                gtk_widget_set_state_flags(hw, GTK_STATE_FLAG_PRELIGHT, FALSE);
                ta->retval = 1;
            }
            break;
        }
        case 15: {  // press — hold the widget in its ACTIVE visual state
            GtkWidget* pw2 = aether_ui_get_widget(ta->handle);
            if (pw2) {
                gtk_widget_set_state_flags(pw2, GTK_STATE_FLAG_ACTIVE, FALSE);
                ta->retval = 1;
            }
            break;
        }
        case 16: {  // release
            GtkWidget* rw = aether_ui_get_widget(ta->handle);
            if (rw) {
                gtk_widget_unset_state_flags(rw, GTK_STATE_FLAG_ACTIVE);
                ta->retval = 1;
            }
            break;
        }
    }
    ta->result = 0;
    ta->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas click — fires a canvas's registered on_click closure with (x, y) on
// the GTK thread, exactly as the GtkGestureClick "pressed" handler would. Lets
// the HTTP driver exercise AeVG vg-scene shape clicks (hit-tested + recoloured)
// that the widget-level /widget/{id}/click can't reach.
typedef struct {
    int canvas_id;
    double x, y;
    int done;
    int result;  // 0=ok, 3=no such canvas / no handler
} CanvasClickAction;

static gboolean canvas_click_idle(gpointer data) {
    CanvasClickAction* a = (CanvasClickAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_click && cs->on_click->fn) {
        ((void(*)(void*, double, double))cs->on_click->fn)(cs->on_click->env, a->x, a->y);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas pointer-move — fires on_move with (x, y), as the motion controller
// would. Lets the driver test hover paths (hit-test → status line).
static gboolean canvas_move_idle(gpointer data) {
    CanvasClickAction* a = (CanvasClickAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_move && cs->on_move->fn) {
        ((void(*)(void*, double, double))cs->on_move->fn)(cs->on_move->env, a->x, a->y);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas pointer-release — fires on_release with (x, y), completing a
// press→drag→release gesture. Without it a drag-to-swipe app (rubiks_cube)
// can't be driven at all: the swipe only fires on release.
static gboolean canvas_release_idle(gpointer data) {
    CanvasClickAction* a = (CanvasClickAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_release && cs->on_release->fn) {
        ((void(*)(void*, double, double))cs->on_release->fn)(cs->on_release->env, a->x, a->y);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas key press — fires on_key with a GDK key name ("Down", "Return",
// "Escape", "a"), as the key controller would. No focus needed: the closure
// is invoked directly, so tests can drive keyboard nav deterministically.
typedef struct {
    int canvas_id;
    char name[64];
    int done;
    int result;
} CanvasKeyAction;

/* Canvas scroll. Unlike the pointer hooks this closure does not live in
   CanvasState: canvas_on_scroll attaches it to the widget itself, next to the
   GtkEventControllerScroll that normally delivers it, so the driver reads it
   from the same place rather than keeping a second copy that could disagree. */
static gboolean canvas_scroll_idle(gpointer data) {
    CanvasClickAction* a = (CanvasClickAction*)data;
    int wh = aether_ui_canvas_get_widget(a->canvas_id);
    GtkWidget* w = aether_ui_get_widget(wh);
    AeClosure* c = w ? (AeClosure*)g_object_get_data(G_OBJECT(w),
                                                     "aeui-canvas-scroll")
                     : NULL;
    if (c && c->fn) {
        ((void(*)(void*, double, double))c->fn)(c->env, a->x, a->y);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

static gboolean canvas_key_idle(gpointer data) {
    CanvasKeyAction* a = (CanvasKeyAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_key && cs->on_key->fn) {
        ((void(*)(void*, const char*))cs->on_key->fn)(cs->on_key->env, a->name);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas key release — fires on_key_release, pairing canvas_key_idle so a
// spec can drive held-key (down…up) sequences deterministically.
static gboolean canvas_keyup_idle(gpointer data) {
    CanvasKeyAction* a = (CanvasKeyAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_key_release && cs->on_key_release->fn) {
        ((void(*)(void*, const char*))cs->on_key_release->fn)(
            cs->on_key_release->env, a->name);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Shutdown — destroy the top-level window on the GTK thread. GtkApplication
// ends its run loop when the last window goes, so the process exits by the
// SAME teardown path as a user closing the window. This is how test
// harnesses should end a driver session: signal-killing an xvfb-run wrapper
// leaves the app child alive and holding the driver port, and the next
// test then interrogates the wrong app.
static gboolean shutdown_idle(gpointer data) {
    (void)data;
    GtkWidget* root = aether_ui_get_widget(1);
    if (root) {
        GtkWidget* toplevel = root;
        while (gtk_widget_get_parent(toplevel))
            toplevel = gtk_widget_get_parent(toplevel);
        if (GTK_IS_WINDOW(toplevel)) gtk_window_destroy(GTK_WINDOW(toplevel));
    }
    return G_SOURCE_REMOVE;
}

// Window resize — set_default_size on the top-level acts as a resize request
// on a mapped GTK4 window. Lets tests exercise the resize → viewBox-remap →
// coordinate-unmapping path (a maximize/unmaximize regression hid here).
typedef struct {
    int w, h;
    int done;
    int result;
} WindowResizeAction;

static gboolean window_resize_idle(gpointer data) {
    WindowResizeAction* a = (WindowResizeAction*)data;
    GtkWidget* root = aether_ui_get_widget(1);
    a->result = 3;
    if (root) {
        GtkWidget* toplevel = root;
        while (gtk_widget_get_parent(toplevel))
            toplevel = gtk_widget_get_parent(toplevel);
        if (GTK_IS_WINDOW(toplevel)) {
            gtk_window_set_default_size(GTK_WINDOW(toplevel), a->w, a->h);
            a->result = 0;
        }
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// write(2) may PARTIALLY write on a socket (loopback send buffer is ~64KB):
// a 200-row /widgets body (>100KB) silently truncated mid-payload and the
// client saw a Content-Length mismatch. Loop until drained.
static void write_all(int fd, const char* buf, int len) {
    int off = 0;
    while (off < len) {
        int n = (int)write(fd, buf + off, (size_t)(len - off));
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return;   // peer gone; nothing useful to do
        }
        off += n;
    }
}


// Screenshot result — shared between server thread and GTK idle callback.
static unsigned char* ss_result_data = NULL;
static gsize ss_result_len = 0;
static volatile int ss_result_done = 0;

static cairo_status_t png_array_write(void* closure,
                                      const unsigned char* data,
                                      unsigned int length) {
    GByteArray* arr = (GByteArray*)closure;
    g_byte_array_append(arr, data, length);
    return CAIRO_STATUS_SUCCESS;
}

static int screenshot_from_canvas_surface(void) {
    for (int i = 0; i < canvas_state_count; i++) {
        CanvasState* cs = &canvas_states[i];
        if (!cs->paint_surface) continue;
        cairo_surface_flush(cs->paint_surface);
        GByteArray* arr = g_byte_array_new();
        cairo_status_t st = cairo_surface_write_to_png_stream(
            cs->paint_surface, png_array_write, arr);
        if (st == CAIRO_STATUS_SUCCESS && arr->len > 0) {
            ss_result_data = malloc(arr->len);
            if (ss_result_data) {
                memcpy(ss_result_data, arr->data, arr->len);
                ss_result_len = arr->len;
            }
        }
        g_byte_array_unref(arr);
        return ss_result_data ? 1 : 0;
    }
    return 0;
}

static gboolean screenshot_idle_cb(gpointer data) {
    (void)data;
    ss_result_data = NULL;
    ss_result_len = 0;

    GtkWidget* root = aether_ui_get_widget(1);
    if (!root) {
        screenshot_from_canvas_surface();
        ss_result_done = 1;
        return G_SOURCE_REMOVE;
    }

    GtkWidget* toplevel = root;
    while (gtk_widget_get_parent(toplevel))
        toplevel = gtk_widget_get_parent(toplevel);
    if (!GTK_IS_WINDOW(toplevel)) {
        screenshot_from_canvas_surface();
        ss_result_done = 1;
        return G_SOURCE_REMOVE;
    }

    GdkPaintable* paintable = gtk_widget_paintable_new(toplevel);
    int w = gdk_paintable_get_intrinsic_width(paintable);
    int h = gdk_paintable_get_intrinsic_height(paintable);
    if (w <= 0 || h <= 0) { w = 400; h = 300; }

    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, (double)w, (double)h);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    if (!node) {
        g_object_unref(paintable);
        screenshot_from_canvas_surface();
        ss_result_done = 1;
        return G_SOURCE_REMOVE;
    }

    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(toplevel));
    GskRenderer* renderer = gsk_renderer_new_for_surface(surface);
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node,
        &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!texture) {
        gsk_render_node_unref(node);
        gsk_renderer_unrealize(renderer);
        g_object_unref(renderer);
        g_object_unref(paintable);
        screenshot_from_canvas_surface();
        ss_result_done = 1;
        return G_SOURCE_REMOVE;
    }
    GBytes* bytes = gdk_texture_save_to_png_bytes(texture);
    if (!bytes) {
        g_object_unref(texture);
        gsk_render_node_unref(node);
        gsk_renderer_unrealize(renderer);
        g_object_unref(renderer);
        g_object_unref(paintable);
        screenshot_from_canvas_surface();
        ss_result_done = 1;
        return G_SOURCE_REMOVE;
    }

    gsize len = 0;
    const unsigned char* raw = g_bytes_get_data(bytes, &len);
    if (raw && len > 0) {
        ss_result_data = malloc(len);
        if (ss_result_data) {
            memcpy(ss_result_data, raw, len);
            ss_result_len = len;
        }
    }

    g_bytes_unref(bytes);
    g_object_unref(texture);
    gsk_render_node_unref(node);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    g_object_unref(paintable);
    ss_result_done = 1;
    return G_SOURCE_REMOVE;
}


// ---------------------------------------------------------------------------
// Widget-JSON building must run ON THE GTK THREAD. It walks the live widget
// tree (types, text, visibility, allocation, compute_bounds); with dynamic
// children (ui.each / listbox rebuild rows at scan rate) a server-thread walk
// races the mutation and trips GTK's css-node global parent cache — the app
// aborts with gtkcssnode.c:321 "node->cache == NULL" (seen live in gp's
// fileops spec once the list pane became widgets). Same discipline as
// test_action_idle: fill a request, g_idle_add, spin for done.
// mode 0: all widgets (with optional type/text filters)
// mode 1: single widget by id
// mode 2: children of id



// Inject the "Under Remote Control" banner into the app root.
// Called from the GTK activate callback when test mode is on.
static void inject_remote_control_banner(int root_handle) {
    GtkWidget* root = aether_ui_get_widget(root_handle);
    if (!root || !GTK_IS_BOX(root)) return;

    GtkWidget* banner = gtk_label_new("Under Remote Control");
    gtk_widget_add_css_class(banner, "aether-rc-banner");

    GtkCssProvider* prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov,
        ".aether-rc-banner {"
        "  background-color: #cc3333;"
        "  color: white;"
        "  font-weight: bold;"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}", -1);
    GdkDisplay* display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(prov),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    }
    g_object_unref(prov);

    banner_handle = aether_ui_register_widget(banner);
    // Mirror into the SHARED server's banner slot, which is what stamps
    // "banner":true in /widgets once GTK4 runs on it. The local
    // banner_handle above still drives the embedded server (reachable via
    // AETHER_UI_GTK_SERVER=embedded), so both agree. Same shape as the
    // sealed-widget mirror in aether_ui_seal_widget_impl, and found the same
    // way: the flip turned testable red because the banner widget existed but
    // reported "banner":false.
    aether_ui_test_server_set_banner(banner_handle);
    gtk_box_prepend(GTK_BOX(root), banner);
}

static int test_server_port = 0;

// Nonzero once the driver's test server is armed (see on_canvas_move).
static int aeui_remote_controlled(void) { return test_server_port != 0; }

// ---------------------------------------------------------------------------
// AetherDriverHooks — GTK4's side of the SHARED driver
// ---------------------------------------------------------------------------
//
// Added ahead of the switch-over, so the table can be compiled and reviewed
// while the embedded server still runs. Nothing calls these yet; the flip is a
// separate commit (docs/design/one-driver-not-two.md).
//
// THE ONE THING THAT IS NOT LIKE macOS. macOS fills these and lets the shared
// server call the read-only ones straight from the HTTP thread, because AppKit
// tolerates that. GTK does not: a server-thread widget walk races the UI
// thread and trips GTK's css-node global parent cache, aborting in
// gtkcssnode.c with "node->cache == NULL" (observed live -- see the comment
// above widgets_json_idle). So GTK4 supplies run_on_ui_thread and the shared
// server services the WHOLE request over there. These hooks are therefore
// written as if already on the GTK thread, and MUST NOT be called otherwise.

typedef struct { void (*fn)(void*); void* arg; volatile int done; } AeuiUiCall;

static gboolean aeui_ui_call_idle(gpointer data) {
    AeuiUiCall* c = (AeuiUiCall*)data;
    c->fn(c->arg);
    c->done = 1;
    return G_SOURCE_REMOVE;
}

// Blocking hop onto the GTK thread. Same fill/g_idle_add/spin discipline as
// test_action_idle and widgets_json_idle, which is what the shared server's
// contract requires ("MUST block: the caller reads the response back
// immediately").
static void hook_run_on_ui_thread(void (*fn)(void*), void* arg) {
    AeuiUiCall c = { fn, arg, 0 };
    g_idle_add(aeui_ui_call_idle, &c);
    while (!c.done) g_usleep(200);
}

static int hook_widget_count(void) { return aether_ui_widget_count_impl(); }

static const char* hook_widget_type(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return "null";
    return widget_type_name(w);
}

static void hook_widget_text_into(int handle, char* buf, int bufsize) {
    buf[0] = '\0';
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    const char* t = widget_text_content(w);
    if (t) { strncpy(buf, t, (size_t)bufsize - 1); buf[bufsize - 1] = '\0'; }
}

static int hook_widget_visible(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    return (w && gtk_widget_get_visible(w)) ? 1 : 0;
}

static int hook_widget_parent(int handle) { return parent_handle_for(handle); }

static int hook_widget_enabled(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    return (w && gtk_widget_get_sensitive(w)) ? 1 : 0;
}

static int hook_toggle_active(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_CHECK_BUTTON(w)) return 0;
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? 1 : 0;
}

static double hook_slider_value(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_RANGE(w)) return 0.0;
    return gtk_range_get_value(GTK_RANGE(w));
}

static double hook_progressbar_fraction(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_PROGRESS_BAR(w)) return 0.0;
    return gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(w));
}

static int hook_widget_hovered(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    return (gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_PRELIGHT) ? 1 : 0;
}

static int hook_widget_pressed(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    return (gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_ACTIVE) ? 1 : 0;
}

static int hook_focused_widget(void) { return aether_ui_focused_widget(); }

static int hook_widget_children(int handle, int* out_handles, int max) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    int n = 0;
    for (GtkWidget* child = gtk_widget_get_first_child(w);
         child; child = gtk_widget_get_next_sibling(child)) {
        int ch = handle_for_widget(child);
        if (ch > 0) {
            if (out_handles && n < max) out_handles[n] = ch;
            n++;
        }
    }
    return n;
}

// Window-local geometry. 0 = success, per the hook contract.
static int hook_widget_rect(int handle, int* x, int* y, int* w, int* hgt) {
    GtkWidget* wd = aether_ui_get_widget(handle);
    if (!wd) return 1;
    graphene_rect_t r;
    GtkWidget* root = GTK_WIDGET(gtk_widget_get_root(wd));
    if (!root || !gtk_widget_compute_bounds(wd, root, &r)) return 1;
    *x = (int)r.origin.x; *y = (int)r.origin.y;
    *w = (int)r.size.width; *hgt = (int)r.size.height;
    return 0;
}

// Space-separated CSS class list, "" when none. Read from the gobject data
// widget_to_json already uses, so the shared server reports what the embedded
// one did.
static void hook_widget_classes_into(int handle, char* buf, int bufsize) {
    buf[0] = '\0';
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    const char* cur = (const char*)g_object_get_data(G_OBJECT(w), "aeui-classes");
    if (cur) { strncpy(buf, cur, (size_t)bufsize - 1); buf[bufsize - 1] = '\0'; }
}

static void hook_widget_a11y(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    aether_ui_a11y_get_impl(handle, role, rolesz, name, namesz, desc, descsz);
}

static int hook_canvas_debug(int canvas_id, int* area, int* commands,
                             int* w, int* h) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    *area = cs->last_paint_area;
    *commands = cs->last_paint_count;
    *w = cs->last_paint_w;
    *h = cs->last_paint_h;
    return 0;
}

static int hook_canvas_paint_counters(int canvas_id, int* full_paints,
                                      int* clip_paints, int* last_clip_area) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    *full_paints = cs->paint_full_count;
    *clip_paints = cs->paint_clip_count_total;
    *last_clip_area = cs->last_clip_area;
    return 0;
}

// ---------------------------------------------------------------------------
// dispatch_action — BY VERB, never by number
// ---------------------------------------------------------------------------
//
// THE HAZARD, stated plainly because it is the whole reason this function is
// written out longhand instead of casting one enum to the other:
//
//   n   GTK4 TestAction     shared AETHER_DRV_*   verdict
//   0   click               CLICK                 agree
//   1   set_text            SET_TEXT              agree
//   2   toggle              TOGGLE                agree
//   3   set_value           SET_VALUE             agree
//   4   set_state           SET_STATE             agree
//   5   ctx_open            FOCUS                 COLLIDE
//   6   ctx_activate        WIN_RESIZE            COLLIDE
//   7   split_position      WIN_KEY               COLLIDE
//   8   focus               SPLIT_POS             COLLIDE
//   9   window_key          CANVAS_CLICK          COLLIDE
//  10   tab_select          CANVAS_MOVE           COLLIDE
//
// 0-4 agree by luck. EVERY number from 5 up means something different in each
// scheme, and both readings are valid integers, so a mis-map answers 200 and
// silently does the wrong thing. tests/driver_actions exists to catch exactly
// that and is the gate for this function.
//
// The authoritative source is TestAction's own comment ("5=ctx_open
// 6=ctx_activate"), NOT the route strings: my first pass read the mapping off
// grep of the route arms and got 5, 11, 12, 13, 15, 16 wrong.
//
// Runs on the GTK thread already (the shared server hopped there via
// run_on_ui_thread), so this calls test_action_idle's body directly rather
// than marshalling a second time.
static void hook_dispatch_action(AetherDriverActionCtx* ctx) {
    TestAction ta;
    memset(&ta, 0, sizeof(ta));
    ta.handle = ctx->handle;
    ta.dval   = ctx->dval;
    ta.ival   = ctx->ival;
    strncpy(ta.sval, ctx->sval, sizeof(ta.sval) - 1);

    int supported = 1;
    switch (ctx->action) {
        case AETHER_DRV_CLICK:        ta.action = 0;  break;
        case AETHER_DRV_SET_TEXT:     ta.action = 1;  break;
        case AETHER_DRV_TOGGLE:       ta.action = 2;  break;
        case AETHER_DRV_SET_VALUE:    ta.action = 3;  break;
        case AETHER_DRV_SET_STATE:    ta.action = 4;  break;
        case AETHER_DRV_CTX_MENU:     ta.action = 5;  break;
        case AETHER_DRV_CTX_ACTIVATE: ta.action = 6;  break;
        case AETHER_DRV_SPLIT_POS:    ta.action = 7;  break;
        case AETHER_DRV_FOCUS:        ta.action = 8;  break;
        case AETHER_DRV_WIN_KEY:      ta.action = 9;  break;
        case AETHER_DRV_TAB_SELECT:   ta.action = 10; break;
        /* Verified against test_action_idle's own switch, NOT against the
           route strings -- reading the routes produced a wrong table the
           first time (see 011dba1). NB double_click / drop / scroll are NOT
           here: the shared server serves those directly via
           aether_ui_fire_double_click / _fire_row_drop / _fire_scroll and
           never calls dispatch_action for them, which is why they have no
           AETHER_DRV_* kind at all. */
        case AETHER_DRV_HOVER:        ta.action = 14; break;
        case AETHER_DRV_PRESS:        ta.action = 15; break;
        case AETHER_DRV_RELEASE:      ta.action = 16; break;

        /* SHUTDOWN and WIN_RESIZE do not go through test_action_idle at all --
           they have their own *_idle handlers below. They were left in the
           default arm (result 3 -> 404) when the embedded server was deleted,
           which would have been honest EXCEPT that /shutdown replies 200
           BEFORE dispatching (it has to: the app is about to exit). So the
           404 never reached the client and the route reported {"ok":true} for
           an app that carried on running.
           
           Cost: every suite in spec_matrix asked its app to quit, was told
           yes, and then waited out its teardown budget for an exit that never
           came -- ~96s wall for a suite doing 0.4s of work. Already on the
           GTK thread here, so call the handlers' bodies directly. */
        case AETHER_DRV_SHUTDOWN:
            shutdown_idle(NULL);
            ctx->result = 0; ctx->done = 1;
            return;
        case AETHER_DRV_WIN_RESIZE: {
            WindowResizeAction wr = {0};
            wr.w = ctx->ival; wr.h = ctx->ival2;
            window_resize_idle(&wr);
            ctx->result = wr.result; ctx->done = 1;
            return;
        }

        /* The canvas verbs and PICK have their own *_idle handlers too, and
           were orphaned by the same deletion. Their routes answered
           "not wired on this backend" / "no such context menu item", which
           is what kept context_menu, overlay and vg_tooltip red. */
        case AETHER_DRV_CANVAS_CLICK: {
            CanvasClickAction ca = {0};
            ca.canvas_id = ctx->handle; ca.x = ctx->dval; ca.y = ctx->dval2;
            canvas_click_idle(&ca);
            ctx->result = ca.result; ctx->done = 1;
            return;
        }
        case AETHER_DRV_CANVAS_MOVE: {
            CanvasClickAction ca = {0};
            ca.canvas_id = ctx->handle; ca.x = ctx->dval; ca.y = ctx->dval2;
            canvas_move_idle(&ca);
            ctx->result = ca.result; ctx->done = 1;
            return;
        }
        case AETHER_DRV_CANVAS_RELEASE: {
            CanvasClickAction ca = {0};
            ca.canvas_id = ctx->handle; ca.x = ctx->dval; ca.y = ctx->dval2;
            canvas_release_idle(&ca);
            ctx->result = ca.result; ctx->done = 1;
            return;
        }
        case AETHER_DRV_CANVAS_KEY: {
            CanvasKeyAction ka = {0};
            ka.canvas_id = ctx->handle;
            strncpy(ka.name, ctx->sval, sizeof(ka.name) - 1);
            canvas_key_idle(&ka);
            ctx->result = ka.result; ctx->done = 1;
            return;
        }
        case AETHER_DRV_CANVAS_SCROLL: {
            CanvasClickAction ca = {0};
            ca.canvas_id = ctx->handle; ca.x = ctx->dval; ca.y = ctx->dval2;
            canvas_scroll_idle(&ca);
            ctx->result = ca.result; ctx->done = 1;
            return;
        }
        case AETHER_DRV_CANVAS_KEYUP: {
            CanvasKeyAction ka = {0};
            ka.canvas_id = ctx->handle;
            strncpy(ka.name, ctx->sval, sizeof(ka.name) - 1);
            canvas_keyup_idle(&ka);
            ctx->result = ka.result; ctx->done = 1;
            return;
        }
        case AETHER_DRV_PICK: {
            /* PICK returns THREE values: handle, the widget type string, and
               whether an overlay scrim intercepted. ival2 is both an input
               (y) and an output (on_scrim) -- read y before overwriting. */
            PickAction pa = {0};
            pa.x = (double)ctx->ival; pa.y = (double)ctx->ival2;
            window_pick_idle(&pa);
            ctx->retval = pa.handle;
            ctx->ival2  = pa.on_scrim;
            strncpy(ctx->sval, pa.type, sizeof(ctx->sval) - 1);
            ctx->result = 0; ctx->done = 1;
            return;
        }
        default:
            /* Verbs the embedded server routes through paths OTHER than
               test_action_idle (canvas click/move/key/release, pick, hover,
               press, release, resize, shutdown) are not mapped here yet.
               result 3 -> 404 is the honest answer; inventing a number would
               fire the wrong handler and still answer 200. */
            supported = 0;
            break;
    }
    if (!supported) { ctx->result = 3; ctx->done = 1; return; }

    ta.done = 0;
    test_action_idle(&ta);        /* already on the GTK thread */
    ctx->retval = ta.retval;
    ctx->result = ta.result;
    ctx->done   = 1;
}

// Capture the top-level to PNG bytes. The hook's contract is "set *out_data
// (caller frees with free()) and *out_len, return 0".
//
// I expected this to be the one hook needing a real conversion -- GTK4
// captures through a paintable snapshot and I assumed a GBytes the caller
// could not free(). It does not: screenshot_idle_cb already memcpy's into a
// plain malloc'd ss_result_data, and the embedded route already free()s it.
// So this is a wrapper after all, and the ownership transfer is exactly what
// the hook wants.
//
// Runs on the GTK thread (run_on_ui_thread got us here), so it calls the idle
// body directly instead of g_idle_add-ing a second time.
static int hook_screenshot_png(unsigned char** out_data, size_t* out_len) {
    ss_result_done = 0;
    screenshot_idle_cb(NULL);
    if (!ss_result_data || ss_result_len == 0) return 1;
    *out_data = ss_result_data;
    *out_len  = ss_result_len;
    // Hand ownership to the caller; do not free here and do not leave a
    // dangling pointer for the embedded route to double-free.
    ss_result_data = NULL;
    ss_result_len  = 0;
    return 0;
}

static const AetherDriverHooks gtk4_driver_hooks = {
    .widget_count         = hook_widget_count,
    .widget_type          = hook_widget_type,
    .widget_text_into     = hook_widget_text_into,
    .widget_visible       = hook_widget_visible,
    .widget_parent        = hook_widget_parent,
    .toggle_active        = hook_toggle_active,
    .slider_value         = hook_slider_value,
    .progressbar_fraction = hook_progressbar_fraction,
    .widget_children      = hook_widget_children,
    .widget_enabled       = hook_widget_enabled,
    .focused_widget       = hook_focused_widget,
    .widget_hovered       = hook_widget_hovered,
    .widget_pressed       = hook_widget_pressed,
    .widget_rect          = hook_widget_rect,
    .widget_classes_into  = hook_widget_classes_into,
    .widget_a11y          = hook_widget_a11y,
    .canvas_debug         = hook_canvas_debug,
    .canvas_paint_counters = hook_canvas_paint_counters,
    .dispatch_action      = hook_dispatch_action,
    .screenshot_png       = hook_screenshot_png,
    // The GTK-specific one macOS and win32 leave NULL.
    .run_on_ui_thread     = hook_run_on_ui_thread,
    // Still NULL, each for its own reason:
    //
    //   dispatch_action  its own commit. Must be written BY VERB, never by
    //                    number: GTK4's private action ids collide with the
    //                    shared enum while meaning different things, and both
    //                    spellings answer 200 (see tests/driver_actions).
};

// GTK4 serves AetherUIDriver from the SHARED aether_ui_test_server.c, as
// win32 and macOS do. Route parity is structural: a route added to the shared
// server lands on all four backends at once.
//
// The ~945-line HTTP server that used to live in this file is gone, along with
// the AETHER_UI_GTK_SERVER=embedded switch that kept it reachable while the
// migration was unverified. Both came out once all four platforms ran green on
// the shared path (routeparity 6/0, driveractions 6/0, testable 7/0 on Linux,
// win32, FreeBSD and macOS).
void aether_ui_enable_test_server_impl(int port, int root_handle) {
    test_server_port = port;
    inject_remote_control_banner(root_handle);
    aether_ui_test_server_start(port, &gtk4_driver_hooks);
}

// ---------------------------------------------------------------------------
// Widget tree
// ---------------------------------------------------------------------------

// Called from Aether DSL wrappers where parent comes through _ctx (void*).
/* set_child(parent, child) — make `child` a BIN parent's single child.
   First consumer: the chrome-drawn button face (vg-drawn controls phase 2):
   a native GtkButton keeps every native behaviour — click, focus, a11y,
   the driver contract — and only its FACE is replaced by a chrome-rendered
   canvas. The native label is stashed as object data first, so the
   /widgets text hook keeps answering the original label (parity). */
void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) {
    GtkWidget* p = aether_ui_get_widget(parent_handle);
    GtkWidget* c = aether_ui_get_widget(child_handle);
    if (!p || !c) return;
    GtkWidget* cur = gtk_widget_get_parent(c);
    if (cur) gtk_widget_unparent(c);
    if (GTK_IS_CHECK_BUTTON(p)) {
        /* GTK4 CheckButton is NOT a GtkButton subclass — own branch, same
           stash-then-replace: the drawn toggle face covers the whole
           control, and /widgets keeps answering the original label. */
        const char* lbl = gtk_check_button_get_label(GTK_CHECK_BUTTON(p));
        if (lbl && *lbl)
            g_object_set_data_full(G_OBJECT(p), "aeui-face-label",
                                   g_strdup(lbl), g_free);
        gtk_check_button_set_child(GTK_CHECK_BUTTON(p), c);
        return;
    }
    if (GTK_IS_BUTTON(p)) {
        const char* lbl = gtk_button_get_label(GTK_BUTTON(p));
        if (lbl && *lbl)
            g_object_set_data_full(G_OBJECT(p), "aeui-face-label",
                                   g_strdup(lbl), g_free);
        gtk_button_set_child(GTK_BUTTON(p), c);
    }
}

void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    GtkWidget* parent = aether_ui_get_widget(parent_handle);
    GtkWidget* child = aether_ui_get_widget(child_handle);
    if (!parent || !child) return;

    if (GTK_IS_BOX(parent)) {
        if (aeui_box_orientation(parent) == GTK_ORIENTATION_HORIZONTAL) {
            if (gtk_widget_get_hexpand(child) && gtk_widget_get_vexpand(child)) {
                gtk_widget_set_vexpand(child, FALSE);
            }
        }
        gtk_box_append(GTK_BOX(parent), child);
    } else if (GTK_IS_SCROLLED_WINDOW(parent)) {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(parent), child);
    } else if (GTK_IS_OVERLAY(parent)) {
        if (gtk_overlay_get_child(GTK_OVERLAY(parent)) == NULL) {
            gtk_overlay_set_child(GTK_OVERLAY(parent), child);
        } else {
            gtk_overlay_add_overlay(GTK_OVERLAY(parent), child);
        }
    } else if (GTK_IS_FLOW_BOX(parent)) {
        gtk_flow_box_insert(GTK_FLOW_BOX(parent), child, -1);
    } else if (GTK_IS_PANED(parent)) {
        // splitview: exactly two panes, in declaration order. A third
        // attach is silently dropped (documented in ui.splitview).
        if (gtk_paned_get_start_child(GTK_PANED(parent)) == NULL) {
            gtk_paned_set_start_child(GTK_PANED(parent), child);
            gtk_paned_set_resize_start_child(GTK_PANED(parent), TRUE);
            gtk_paned_set_shrink_start_child(GTK_PANED(parent), FALSE);
        } else if (gtk_paned_get_end_child(GTK_PANED(parent)) == NULL) {
            gtk_paned_set_end_child(GTK_PANED(parent), child);
            gtk_paned_set_resize_end_child(GTK_PANED(parent), TRUE);
            gtk_paned_set_shrink_end_child(GTK_PANED(parent), FALSE);
        }
    }
}

void aether_ui_widget_set_hidden(int handle, int hidden) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_visible(w, !hidden);
}

// ---------------------------------------------------------------------------
// Menus.
// GTK4 removed traditional GtkMenu in favor of GMenuModel + GtkPopoverMenu.
// Full native wiring (GMenu / GActionGroup) is follow-up work; for now
// these stubs maintain the cross-platform ABI so the test suite links and
// the DSL can be written once against a stable API. Menu items won't
// actually fire on GTK4 yet — track via the stub_actions list when it's
// worth wiring up.
// ---------------------------------------------------------------------------

// A menu (bar or submenu) is backed by a real GMenu model. Items append to
// the menu's current section; each carries a per-item GAction name
// (app.aeui.m<handle>.i<idx>). The GActions can only live on the GApplication,
// which doesn't exist until app_run — the DSL builds the menu first — so we
// build the GMenu models eagerly and QUEUE the (action_name → closure) pairs,
// then register them all on the app in on_activate (same deferral shortcuts
// use). menu_item_record still runs so the driver's /menu route reaches the
// same closure by label independently of the native path.
typedef struct {
    int     is_bar;
    char*   label;
    GMenu*  model;      // the bar's or submenu's GMenu
    GMenu*  section;    // current section (separator starts a new one)
} GtkMenuEntry;

static GtkMenuEntry* gtk_menus = NULL;
static int           gtk_menu_count = 0;
static int           gtk_menu_capacity = 0;

// Pending item actions, registered on the app in on_activate.
typedef struct {
    char*      action_name;   // "aeui.m2.i0" (no "app." prefix — that's the scope)
    AeClosure* closure;
} GtkMenuAction;

static GtkMenuAction* gtk_menu_actions = NULL;
static int            gtk_menu_action_count = 0;
static int            gtk_menu_action_capacity = 0;

// The bar to attach as the window menubar (set by menu_bar_attach).
static GMenu* gtk_pending_menubar = NULL;

static GtkMenuEntry* gtk_menu_at(int handle) {
    if (handle < 1 || handle > gtk_menu_count) return NULL;
    return &gtk_menus[handle - 1];
}

static int gtk_register_menu(int is_bar, const char* label) {
    if (gtk_menu_count >= gtk_menu_capacity) {
        gtk_menu_capacity = gtk_menu_capacity == 0 ? 8 : gtk_menu_capacity * 2;
        gtk_menus = (GtkMenuEntry*)realloc(gtk_menus,
                                           sizeof(GtkMenuEntry) * gtk_menu_capacity);
    }
    GtkMenuEntry* e = &gtk_menus[gtk_menu_count];
    e->is_bar = is_bar;
    e->label = label ? strdup(label) : NULL;
    e->model = g_menu_new();
    // Items go into a section so separators can split without rebuilding.
    e->section = g_menu_new();
    g_menu_append_section(e->model, NULL, G_MENU_MODEL(e->section));
    gtk_menu_count++;
    return gtk_menu_count;
}

int aether_ui_menu_bar_create(void) {
    return gtk_register_menu(1, NULL);
}

int aether_ui_menu_create(const char* label) {
    return gtk_register_menu(0, label);
}

// The GAction trampoline: activate → the recorded closure.
static void gtk_menu_action_cb(GSimpleAction* action, GVariant* param,
                               gpointer user_data) {
    (void)action; (void)param;
    AeClosure* c = (AeClosure*)user_data;
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
}

void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure) {
    // Driver path (unchanged): the side-store lets /menu/{h}/activate?label=…
    // reach this closure by label on every backend.
    aether_ui_menu_item_record(menu_handle, label, boxed_closure);

    GtkMenuEntry* e = gtk_menu_at(menu_handle);
    if (!e) return;

    // Native path: append a GMenuItem bound to a unique per-item action, and
    // queue that action for registration on the app in on_activate.
    int idx = (int)g_menu_model_get_n_items(G_MENU_MODEL(e->section));
    char action[64];
    snprintf(action, sizeof(action), "aeui.m%d.i%d", menu_handle, idx);
    char detailed[80];
    snprintf(detailed, sizeof(detailed), "app.%s", action);
    g_menu_append(e->section, label ? label : "", detailed);

    if (gtk_menu_action_count >= gtk_menu_action_capacity) {
        gtk_menu_action_capacity = gtk_menu_action_capacity == 0 ? 16
                                 : gtk_menu_action_capacity * 2;
        gtk_menu_actions = (GtkMenuAction*)realloc(gtk_menu_actions,
                          sizeof(GtkMenuAction) * gtk_menu_action_capacity);
    }
    gtk_menu_actions[gtk_menu_action_count].action_name = strdup(action);
    gtk_menu_actions[gtk_menu_action_count].closure = (AeClosure*)boxed_closure;
    gtk_menu_action_count++;
}

void aether_ui_menu_add_separator(int menu_handle) {
    // Close the current section and start a new one — GtkPopoverMenuBar draws
    // an inter-section divider.
    GtkMenuEntry* e = gtk_menu_at(menu_handle);
    if (!e) return;
    e->section = g_menu_new();
    g_menu_append_section(e->model, NULL, G_MENU_MODEL(e->section));
}

void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    GtkMenuEntry* bar = gtk_menu_at(bar_handle);
    GtkMenuEntry* sub = gtk_menu_at(menu_handle);
    if (!bar || !sub) return;
    g_menu_append_submenu(bar->model, sub->label ? sub->label : "",
                          G_MENU_MODEL(sub->model));
}

void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    (void)app_handle;
    GtkMenuEntry* bar = gtk_menu_at(bar_handle);
    if (bar) gtk_pending_menubar = bar->model;
}

// Register a menu bar's queued item actions on the running app (so its items
// fire). Idempotent-ish: re-registering the same action name replaces it.
static void aeui_register_menu_actions(GtkApplication* app) {
    if (!app) return;
    for (int i = 0; i < gtk_menu_action_count; i++) {
        if (g_action_map_lookup_action(G_ACTION_MAP(app),
                                        gtk_menu_actions[i].action_name))
            continue;  // already registered
        GSimpleAction* act = g_simple_action_new(
            gtk_menu_actions[i].action_name, NULL);
        g_signal_connect(act, "activate", G_CALLBACK(gtk_menu_action_cb),
                         gtk_menu_actions[i].closure);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act));
        g_object_unref(act);
    }
}

// Per-window menu bar: a GtkPopoverMenuBar from the bar's GMenu, packed ABOVE
// the window's content in a vbox. win_handle 1 = primary, 2.. = extras (the
// unified driver numbering). Unlike the app menubar (one model for all
// windows), this gives each window its OWN bar.
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) {
    GtkMenuEntry* bar = gtk_menu_at(bar_handle);
    if (!bar || !bar->model) return;
    GtkWindow* win = NULL;
    if (win_handle == 1) win = primary_window;
    else if (win_handle - 2 >= 0 && win_handle - 2 < extra_window_count)
        win = extra_windows[win_handle - 2].window;
    if (!win) return;

    GtkApplication* app = (app_count > 0) ? apps[0].gtk_app : NULL;
    aeui_register_menu_actions(app);

    GtkWidget* menubar = gtk_popover_menu_bar_new_from_model(
        G_MENU_MODEL(bar->model));
    aether_ui_register_widget(menubar);  // driver visibility (type:menubar, window:N)
    GtkWidget* child = gtk_window_get_child(win);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), menubar);
    if (child) {
        g_object_ref(child);
        gtk_window_set_child(win, NULL);   // detach before re-parenting
        gtk_widget_set_vexpand(child, TRUE);
        gtk_box_append(GTK_BOX(box), child);
        g_object_unref(child);
    }
    gtk_window_set_child(win, box);
}

// Register the queued item actions on the app and set the menubar. Called from
// on_activate once gtk_app exists.
static void aeui_attach_pending_menus(GtkApplication* app, GtkWindow* window) {
    for (int i = 0; i < gtk_menu_action_count; i++) {
        GSimpleAction* act = g_simple_action_new(
            gtk_menu_actions[i].action_name, NULL);
        g_signal_connect(act, "activate",
                         G_CALLBACK(gtk_menu_action_cb),
                         gtk_menu_actions[i].closure);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act));
        g_object_unref(act);
    }
    if (gtk_pending_menubar) {
        gtk_application_set_menubar(app, G_MENU_MODEL(gtk_pending_menubar));
        gtk_application_window_set_show_menubar(
            GTK_APPLICATION_WINDOW(window), TRUE);
    }
}

void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    // A standalone popup of a menu's GMenu model, anchored under the widget.
    // Like the win32 TrackPopupMenu / macOS popUpMenuPositioningItem paths,
    // this shows real UI, so it's a no-op under headless (CI, smoke tests) —
    // there's no seat to dismiss it and no user to see it. The items remain
    // reachable via the /menu/{h}/activate driver route regardless.
    {
        const char* hl = getenv("AETHER_UI_HEADLESS");
        if (hl && hl[0] && hl[0] != '0') return;
    }
    GtkMenuEntry* e = gtk_menu_at(menu_handle);
    GtkWidget* anchor = aether_ui_get_widget(anchor_widget);
    if (!e || !e->model || !anchor) return;

    GtkWidget* pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(e->model));
    gtk_widget_set_parent(pop, anchor);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    // Free the popover when it closes (it unparents itself on dispose).
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

// ---------------------------------------------------------------------------
// Grid layout (GtkGrid).
// ---------------------------------------------------------------------------
int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    (void)cols; // GtkGrid sizes to content rather than using a fixed col count.
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(grid), col_spacing);
    return aether_ui_register_widget(grid);
}

void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span) {
    GtkWidget* grid = aether_ui_get_widget(grid_handle);
    GtkWidget* child = aether_ui_get_widget(child_handle);
    if (!grid || !child || !GTK_IS_GRID(grid)) return;
    // CRITICAL: hold a reference across the reparent. gtk_widget_unparent
    // drops the parent's reference, and the parent normally holds the LAST
    // one, so unparenting alone finalizes the child: the attach below then
    // gets a dead pointer (Gtk-CRITICAL: assertion GTK_IS_WIDGET failed) and
    // the cell silently never appears. Same idiom as the menubar reparent.
    if (gtk_widget_get_parent(child)) {
        g_object_ref(child);
        gtk_widget_unparent(child);
        gtk_grid_attach(GTK_GRID(grid), child, col, row, col_span, row_span);
        g_object_unref(child);
        return;
    }
    gtk_grid_attach(GTK_GRID(grid), child, col, row, col_span, row_span);
}

// ---------------------------------------------------------------------------
// Reverse lookup — aether_ui_handle_for_widget.
// Linear scan over the widget registry. The hash-backed O(1) version
// ships in the Win32 backend; porting the hash to GTK4 is straightforward
// follow-up (GtkWidget* maps cleanly to the same hash).
// ---------------------------------------------------------------------------
int aether_ui_handle_for_widget(void* widget) {
    if (!widget) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == widget) return i + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// System tray (Group 7).
//
// Registry-only on GTK4 today. The platform options for a real native
// tray in 2026 are:
//
//   - libayatana-appindicator (-1 / -3 packages): hard-wired to GTK3.
//     The header pulls in <gtk/gtk.h> at the GTK3 search path, which
//     conflicts with the GTK4 typedefs already in this file. Mixing
//     GTK3 and GTK4 in the same process is the original menu-stubbing
//     reason — same trap here.
//
//   - StatusNotifierItem (KDE freedesktop spec) over GDBus. The
//     GTK-version-agnostic path forward; ~400 LOC of GIO D-Bus code to
//     register an SNI on the bus + a DBusMenu for the popup. Adopted
//     by GNOME (via gnome-shell-extension-appindicator), KDE,
//     Cinnamon, Budgie. The right answer for an actual Linux tray.
//
// Phase 1 (this ask) ships the DSL + registry + AetherUIDriver routes
// so AvnSync v2 can wire its callbacks against a stable surface and
// validate them in CI via /tray/{id}/click and
// /tray/{id}/menu/activate. When the SNI implementation lands, it
// only needs to call aether_ui_tray_emit_click() / _menu_activate()
// on dbus events; the DSL contract stays unchanged.
//
// AETHER_UI_HEADLESS does not gate anything here because there's no
// native call to suppress yet. The flag still matters for app_run_
// headless's logging.
// ---------------------------------------------------------------------------
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) {
    int id = aether_ui_tray_register(name, boxed_left_click);
    if (!aeui_is_headless()) aether_ui_sni_register(id);
    return id;
}

void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) {
    aether_ui_tray_set_tooltip_reg(tray_id, text);
    aether_ui_sni_invalidate_tooltip(tray_id);
}

void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) {
    aether_ui_tray_set_menu_reg(tray_id, menu_handle);
    aether_ui_sni_invalidate_menu(tray_id);
}

void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle,
                                             const char* icon_clean,
                                             const char* icon_busy,
                                             const char* icon_alert) {
    aether_ui_tray_set_icon_for_state_reg(tray_id, state_handle,
                                          icon_clean, icon_busy, icon_alert);
    aether_ui_sni_invalidate_icon(tray_id);
}

void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) {
    aether_ui_tray_set_icon_template_reg(tray_id, is_template);
}

void aether_ui_tray_seal_impl(int tray_id) {
    aether_ui_tray_seal_reg(tray_id);
}

// ---------------------------------------------------------------------------
// Desktop notifications (Group 7b).
//
// libnotify is GTK-version-agnostic — depends only on glib/gobject/
// gdk-pixbuf — so the real native path ships on GTK4 today. When
// libnotify is missing (AEUI_HAVE_LIBNOTIFY undefined), the build
// falls through to registry-only, matching the tray story.
//
// Click callback: NotifyNotification emits "action-invoked" on the
// glib main loop, so the callback already runs on the right thread.
// We close over the boxed closure via g_object_set_data + the action
// handler invokes the registry's dispatch helper.
//
// AETHER_UI_HEADLESS suppresses the OS-level notify_notification_show
// call; the record still lands in the registry so driver tests can
// exercise click/dismiss.
// ---------------------------------------------------------------------------

#ifdef AEUI_HAVE_LIBNOTIFY
static int gtk_libnotify_init = 0;

static void ensure_libnotify_init(void) {
    if (!gtk_libnotify_init) {
        notify_init("aether-ui");
        gtk_libnotify_init = 1;
    }
}

// glib action handler: when the user clicks the notification, dispatch
// through the shared registry so the registered Aether closure fires.
static void on_libnotify_action(NotifyNotification* n, char* action,
                                 gpointer user_data) {
    (void)n; (void)action;
    int notif_id = GPOINTER_TO_INT(user_data);
    aether_ui_notif_emit_click(notif_id);
}

static void on_libnotify_closed(NotifyNotification* n, gpointer user_data) {
    (void)n;
    int notif_id = GPOINTER_TO_INT(user_data);
    aether_ui_notif_mark_dismissed(notif_id);
}

static void show_libnotify(int notif_id, const char* title, const char* body,
                            const char* icon_path, int with_click) {
    if (aeui_is_headless()) return;
    ensure_libnotify_init();
    const char* icon = (icon_path && icon_path[0]) ? icon_path : "dialog-information";
    NotifyNotification* n = notify_notification_new(
        title ? title : "", body ? body : "", icon);
    if (with_click) {
        // "default" is the freedesktop spec's reserved action key for
        // "user clicked the bubble (not a specific button)".
        notify_notification_add_action(n, "default", "Default",
            on_libnotify_action, GINT_TO_POINTER(notif_id), NULL);
    }
    g_signal_connect(n, "closed", G_CALLBACK(on_libnotify_closed),
                     GINT_TO_POINTER(notif_id));
    GError* err = NULL;
    if (!notify_notification_show(n, &err)) {
        // D-Bus notifier daemon may not be running (headless test
        // env, minimal WMs). Silently drop — the registry record
        // still exists for the driver to act on.
        if (err) g_error_free(err);
    }
}
#endif

int aether_ui_notify_impl(const char* title, const char* body) {
    int id = aether_ui_notify_register(title, body);
#ifdef AEUI_HAVE_LIBNOTIFY
    show_libnotify(id, title, body, NULL, 0);
#endif
    return id;
}

int aether_ui_notify_full_impl(const char* title, const char* body,
                                const char* icon_path, const char* tag,
                                void* boxed_click) {
    int id = aether_ui_notify_register_full(title, body, icon_path, tag, boxed_click);
#ifdef AEUI_HAVE_LIBNOTIFY
    show_libnotify(id, title, body, icon_path, boxed_click ? 1 : 0);
#endif
    return id;
}

int aether_ui_notify_request_permission_impl(void) {
    // Linux: always granted (libnotify just talks to the running
    // notification daemon over D-Bus; nothing to prompt for).
    return aether_ui_notify_request_permission();
}

// app_run_headless on Linux: pump a GMainLoop so D-Bus signals
// (SNI Activate/ContextMenu, DBusMenu Event/AboutToShow) get
// delivered. Without a running loop the tray icon would never
// register and menu clicks would never fire — the bus connection
// would be inert.
//
// AETHER_UI_HEADLESS=1: skip the loop (CI doesn't need D-Bus
// signal delivery — the AetherUIDriver routes drive everything
// directly through the registry). Fall through to the shared park
// helper so the process stays alive for the HTTP server thread.
void aether_ui_app_run_headless_impl(void) {
    if (aeui_is_headless()) {
        aether_ui_park_until_killed();
        return;
    }
    ensure_gtk_init();
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}
