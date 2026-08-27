// Aether UI — macOS AppKit backend for Aether
// Port of aether-ui-macos (Rust/objc2) to Objective-C.
//
// This file implements the same C API as aether_ui_gtk4.c using AppKit.
// The Aether module.ae is platform-agnostic — only the backend changes.
//
// Compile on macOS with:
//   clang -fobjc-arc -framework AppKit -framework Foundation \
//         aether_ui_macos.m -c -o aether_ui_macos.o

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>     // text metrics (CTLine typographic bounds)
#import <ImageIO/ImageIO.h>       // headless canvas_write_png
#import <objc/runtime.h>          // objc_setAssociatedObject (dbl-click fire)
#include <time.h>                  // clock_gettime (chord timeout)
#include "aether_ui_backend.h"  // cross-platform backend ABI
#include "aether_ui_system_extras.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ---------------------------------------------------------------------------
// Closure struct — must match Aether codegen's _AeClosure layout.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

// ---------------------------------------------------------------------------
// AETHER_UI_HEADLESS contract — set by CI, widget smoke tests, or any
// caller that wants to exercise the backend without a user sitting at
// the keyboard. Every API that would otherwise run a modal message
// loop (alert, sheet, file/save dialog, popup menu) returns immediately
// when this flag is set. Without this, those APIs can spin their own
// tracking loop and block the process forever — there is no user input
// on CI and no outer runloop to dismiss the modal.
// ---------------------------------------------------------------------------
static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

// ---------------------------------------------------------------------------
// Widget type tags — mirror of widget_type_name() in the GTK4 backend.
// Kept in a parallel array so the test server can report types without
// guessing via isKindOfClass:.
// ---------------------------------------------------------------------------
enum {
    AUI_UNKNOWN = 0,
    AUI_TEXT, AUI_BUTTON, AUI_TOGGLE, AUI_SLIDER, AUI_PICKER,
    AUI_TEXTFIELD, AUI_SECUREFIELD, AUI_TEXTAREA, AUI_TEXTAREA_INNER,
    AUI_PROGRESSBAR, AUI_DIVIDER, AUI_SCROLLVIEW,
    AUI_VSTACK, AUI_HSTACK, AUI_ZSTACK, AUI_SPACER,
    AUI_CANVAS, AUI_IMAGE, AUI_FORM_SECTION, AUI_FORM_SECTION_INNER,
    AUI_NAVSTACK, AUI_BANNER, AUI_WINDOW, AUI_SHEET,
    AUI_SPLITVIEW, AUI_WRAP, AUI_SCRIM, AUI_TABS
};

// ---------------------------------------------------------------------------
// Widget registry — flat array of NSView*, 1-based handles.
// ---------------------------------------------------------------------------
static NSView* __strong *widgets = NULL;
static int* widget_types = NULL;
// Per-widget CSS class list — a space-separated string, malloc'd, NULL when
// the widget has no classes. AppKit has no stylesheet engine, so unlike GTK
// these classes carry no styling; they exist because the driver's `classes`
// field is how specs read selection state (the .aui-row-selected mirror the
// win32 backend established). Parallel array, same 1-based handle space.
static char** widget_classes = NULL;
// Per-widget click closure, addressable by handle. The live click path uses a
// gesture recognizer (which owns its own closure), but the DRIVER has only a
// handle — and a listbox row is a plain container, not an NSButton, so
// performClick: does nothing for it. This is the table that lets
// POST /widget/{id}/click fire what a real click would.
static AeClosure** widget_clicks = NULL;
// Main-axis flex weight (Flutter Expanded). 0 = unweighted, keeps natural size.
static int* widget_weights = NULL;
// Does this widget want to absorb slack? Bit 0 = horizontally, bit 1 = vertically.
// GTK propagates expand UP the tree (a box containing an expanding child itself
// expands); AppKit's hugging is strictly per-view, so we replicate the
// propagation by hand — see aether_ui_widget_add_child_ctx.
static int* widget_expand = NULL;
#define AEUI_EXPAND_H 1
#define AEUI_EXPAND_V 2
// Radio-group leader for a toggle (0 = ungrouped). GTK renders grouped check
// buttons as radios and enforces exactly-one-active itself; AppKit does not,
// so exclusivity is enforced here.
static int* widget_group = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

static int register_widget_typed(void* widget, int type) {
    if (widget_count >= widget_capacity) {
        int new_cap = widget_capacity == 0 ? 64 : widget_capacity * 2;
        NSView* __strong *new_widgets = (__strong NSView**)calloc(new_cap, sizeof(NSView*));
        int* new_types = (int*)calloc(new_cap, sizeof(int));
        char** new_classes = (char**)calloc(new_cap, sizeof(char*));
        AeClosure** new_clicks = (AeClosure**)calloc(new_cap, sizeof(AeClosure*));
        int* new_weights = (int*)calloc(new_cap, sizeof(int));
        int* new_expand = (int*)calloc(new_cap, sizeof(int));
        int* new_group = (int*)calloc(new_cap, sizeof(int));
        if (widgets) {
            for (int i = 0; i < widget_count; i++) {
                new_widgets[i] = widgets[i];
                new_types[i] = widget_types[i];
                new_classes[i] = widget_classes[i];
                new_clicks[i] = widget_clicks[i];
                new_weights[i] = widget_weights[i];
                new_expand[i] = widget_expand[i];
                new_group[i] = widget_group[i];
            }
            free(widgets);
            free(widget_types);
            free(widget_classes);
            free(widget_clicks);
            free(widget_weights);
            free(widget_expand);
            free(widget_group);
        }
        widgets = new_widgets;
        widget_types = new_types;
        widget_classes = new_classes;
        widget_clicks = new_clicks;
        widget_weights = new_weights;
        widget_expand = new_expand;
        widget_group = new_group;
        widget_capacity = new_cap;
    }
    widgets[widget_count] = (__bridge NSView*)widget;
    widget_types[widget_count] = type;
    widget_classes[widget_count] = NULL;
    widget_clicks[widget_count] = NULL;
    widget_weights[widget_count] = 0;
    // A canvas, splitview, scrollview or tabs composite is a slack-absorber in
    // both axes — the AppKit equivalent of GTK setting hexpand/vexpand TRUE on
    // a drawing area (the vg scene rescales its viewBox to fill).
    widget_expand[widget_count] =
        (type == AUI_CANVAS || type == AUI_SPLITVIEW || type == AUI_SCROLLVIEW
         || type == AUI_TABS)
            ? (AEUI_EXPAND_H | AEUI_EXPAND_V) : 0;
    widget_group[widget_count] = 0;
    widget_count++;
    return widget_count;
}

static AeClosure* widget_click_closure(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return widget_clicks[handle - 1];
}

int aether_ui_register_widget(void* widget) {
    return register_widget_typed(widget, AUI_UNKNOWN);
}

void* aether_ui_get_widget(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return (__bridge void*)widgets[handle - 1];
}

static int get_widget_type(int handle) {
    if (handle < 1 || handle > widget_count) return AUI_UNKNOWN;
    return widget_types[handle - 1];
}

static int handle_for_view(NSView* v) {
    if (!v) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == v) return i + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Reactive state
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

// Set while a state→field seed is running, so the set_text below doesn't
// bounce the value straight back into the state (which would also alias and
// free the cell's own string mid-copy). External driver set_text runs with
// this clear, so those DO propagate — matching GtkEntry/EN_CHANGE semantics.
static int g_seeding_bound_field = 0;

static void apply_prop_binding(PropBinding* b) {
    StateCell* c = state_cell(b->state_handle);
    if (!c) return;
    if (b->kind == AEUI_BIND_VALUE) {
        // state → editable widget, compare-first to avoid re-setting the
        // field mid-edit (which would also jump the caret).
        const char* cur = aether_ui_textfield_get_text(b->widget_handle);
        const char* want = (c->type == AEUI_STATE_STRING && c->str) ? c->str : "";
        if (!cur || strcmp(cur, want) != 0) {
            g_seeding_bound_field = 1;
            aether_ui_textfield_set_text(b->widget_handle, want);
            g_seeding_bound_field = 0;
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


// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

@interface AetherAppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow* window;
@property (assign) int rootHandle;
// The size the app ASKED for. Mounting an Auto Layout content view lets the
// window resize itself to the content's natural size, so this has to be
// re-asserted afterwards — see applicationDidFinishLaunching.
@property (assign) int wantW;
@property (assign) int wantH;
@end

// The overlay host — a plain view interposed between the window and the app's
// root, so overlays have somewhere to live that is NOT inside the root stack.
// GTK does exactly this with GtkOverlay; without it, adding a toast to an
// NSStackView root would make it an arranged subview and shove the app's own
// widgets aside. When no overlay is open the tree is visually identical to
// having mounted the root directly.
// The window's content view doubles as the file-drop target: it already
// spans the whole window and outlives every widget in it, so a drop lands
// wherever the pointer is rather than only over a particular control.
@interface AetherOverlayHost : NSView
@end
@implementation AetherOverlayHost
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    (void)sender;
    return aether_ui_window_file_drop_deliver(NULL) ? NSDragOperationCopy
                                                    : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [[sender draggingPasteboard]
        readObjectsForClasses:@[[NSURL class]]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (![urls count]) return NO;
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* u in urls) {
        NSString* p = [u path];
        if (!p) continue;
        // CRITICAL: a NEWLINE is legal in a filename (APFS accepts one), and
        // this ABI separates paths with one. Delivering such a path would
        // split it into two strings, NEITHER of which names a file that
        // exists, and the app would act on both. Dropping it from the batch
        // gives fewer paths but every one of them real.
        if ([p rangeOfString:@"\n"].location != NSNotFound) continue;
        [paths addObject:p];
    }
    if (![paths count]) return NO;
    NSString* joined = [paths componentsJoinedByString:@"\n"];
    return aether_ui_window_file_drop_deliver([joined UTF8String]) ? YES : NO;
}
@end

static AeClosure* window_file_drop_closure = NULL;

// Outbound file drag. beginDraggingSession: is an NSView method, so any
// widget can start one without changing its class; the source object only has
// to say which operations it allows. One shared instance serves every
// draggable widget, since it carries no per-drag state.
@interface AetherDragSource : NSObject <NSDraggingSource>
@end
@implementation AetherDragSource
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session; (void)context;
    return NSDragOperationCopy;
}
@end
static AetherDragSource* aeui_drag_source = nil;

// Payload per widget handle. A plain parallel array rather than associated
// objects so the driver can read it back without touching AppKit.
static char** aeui_drag_paths = NULL;
static int aeui_drag_paths_len = 0;

static void aeui_drag_path_set(int handle, const char* path) {
    if (handle < 1) return;
    if (handle > aeui_drag_paths_len) {
        int want = handle + 16;
        aeui_drag_paths = (char**)realloc(aeui_drag_paths, sizeof(char*) * want);
        for (int i = aeui_drag_paths_len; i < want; i++) aeui_drag_paths[i] = NULL;
        aeui_drag_paths_len = want;
    }
    free(aeui_drag_paths[handle - 1]);
    aeui_drag_paths[handle - 1] = (path && *path) ? strdup(path) : NULL;
}

const char* aether_ui_widget_drag_payload_impl(int handle) {
    if (handle < 1 || handle > aeui_drag_paths_len) return "";
    return aeui_drag_paths[handle - 1] ? aeui_drag_paths[handle - 1] : "";
}

@interface AetherDragRecognizer : NSPanGestureRecognizer
@property (nonatomic) int widgetHandle;
@end
@implementation AetherDragRecognizer
@end

// stringWithUTF8String: returns NIL for bytes that are not valid UTF-8, and
// fileURLWithPath:nil RAISES. So a path from anywhere other than this Mac's
// own filesystem could take the app down: a path read from a network share, a
// config file, or a listing produced on Linux, all of which an app may hand
// to file_icon or draggable.
//
// Measured rather than assumed: APFS REJECTS a non-UTF-8 filename outright
// (creating one fails with EILSEQ), so the local filesystem will never hand
// you such a path, and both conversions return nil for one. The exposure is
// the API surface, not the disk. Returning nil and doing nothing is the point
// either way: an icon that does not appear beats a process that raises.
static NSString* aeui_path_string(const char* path) {
    if (!path || !*path) return nil;
    NSString* s = [[NSFileManager defaultManager]
        stringWithFileSystemRepresentation:path length:strlen(path)];
    if (s) return s;
    return [NSString stringWithUTF8String:path];   // last resort; may be nil
}

@interface AetherDragStarter : NSObject
+ (void)begin:(AetherDragRecognizer*)g;
@end
@implementation AetherDragStarter
+ (void)begin:(AetherDragRecognizer*)g {
    if ([g state] != NSGestureRecognizerStateBegan) return;
    const char* path = aether_ui_widget_drag_payload_impl(g.widgetHandle);
    if (!path || !*path) return;
    NSView* v = [g view];
    NSString* p = aeui_path_string(path);
    if (!p) return;                       // undecodable: no drag, not a crash
    NSURL* url = [NSURL fileURLWithPath:p];
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
    [item setDraggingFrame:[v bounds] contents:nil];
    if (!aeui_drag_source) aeui_drag_source = [[AetherDragSource alloc] init];
    [v beginDraggingSessionWithItems:@[item]
                               event:[[NSApplication sharedApplication] currentEvent]
                              source:aeui_drag_source];
}
@end

void aether_ui_widget_draggable_file_impl(int handle, const char* path) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    aeui_drag_path_set(handle, path);
    if (!path || !*path) return;
    for (NSGestureRecognizer* g in [v gestureRecognizers]) {
        // Already armed: the payload above is all that changes.
        if ([g isKindOfClass:[AetherDragRecognizer class]]) return;
    }
    AetherDragRecognizer* rec = [[AetherDragRecognizer alloc]
        initWithTarget:[AetherDragStarter class] action:@selector(begin:)];
    rec.widgetHandle = handle;
    [v addGestureRecognizer:rec];
}

int aether_ui_window_file_drop_deliver(const char* paths) {
    if (!window_file_drop_closure || !window_file_drop_closure->fn) return 0;
    // A NULL probe (draggingEntered) asks only whether anyone is listening,
    // so the cursor can show copy-or-refuse before the mouse is released.
    if (!paths) return 1;
    ((void(*)(void*, const char*))window_file_drop_closure->fn)(
        window_file_drop_closure->env, paths);
    return 1;
}

static AetherOverlayHost* overlay_host = nil;

void aether_ui_window_on_file_drop_impl(void* boxed_closure) {
    window_file_drop_closure = (AeClosure*)boxed_closure;
    if (overlay_host) {
        [overlay_host registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
}
// The initial-size floor (see applicationDidFinishLaunching). Retracted by the
// first explicit resize, so window(w,h) is a starting size, not a cage.
static NSLayoutConstraint* aeui_win_floor_w = nil;
static NSLayoutConstraint* aeui_win_floor_h = nil;
// An explicit /window/resize pins the content to that size (strong, not
// required — required content mins still win). This BOUNDS the layout so
// weight sharing has a fixed width to divide: a weighted child with a large
// min clamps and its flexible sibling shrinks, instead of the near-required
// share inflating the whole window to fit both. Without a bound the content
// adopts its fitting size and the clamp has nothing to resolve against.
static NSLayoutConstraint* aeui_win_cap_w = nil;
static NSLayoutConstraint* aeui_win_cap_h = nil;

@implementation AetherAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    if (self.rootHandle > 0) {
        NSView* root = (__bridge NSView*)aether_ui_get_widget(self.rootHandle);
        if (root) {
            AetherOverlayHost* host = [[AetherOverlayHost alloc] initWithFrame:
                [[self.window contentView] bounds]];
            [host setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            // Layer-backed so the whole tree gets layers — /screenshot reads the
            // presentation layer to see animations in flight (see
            // hook_screenshot_png), and there is no presentation layer without
            // this.
            [host setWantsLayer:YES];
            [host addSubview:root];
            [root setTranslatesAutoresizingMaskIntoConstraints:NO];
            [root.leadingAnchor constraintEqualToAnchor:host.leadingAnchor].active = YES;
            [root.trailingAnchor constraintEqualToAnchor:host.trailingAnchor].active = YES;
            [root.topAnchor constraintEqualToAnchor:host.topAnchor].active = YES;
            [root.bottomAnchor constraintEqualToAnchor:host.bottomAnchor].active = YES;
            [self.window setContentView:host];
            overlay_host = host;
            // A handler registered BEFORE the window existed still gets its
            // types registered, the same deferral shortcuts use.
            if (window_file_drop_closure) {
                [host registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
            }

            // Force a layout pass now. A headless run never displays the
            // window, and without this every widget_rect would answer 0x0 —
            // the driver's geometry fields would be silently useless on CI.
            [host layoutSubtreeIfNeeded];

            // Re-assert the requested size — AFTER layout, which is the whole
            // point. Mounting an Auto Layout content view makes the window
            // adopt the content's FITTING size the moment it lays out:
            // table_demo asks for 520x420 and settles at 318x420, the natural
            // width of its widest row. Setting the size before that pass gets
            // silently undone by it. window(w, h) means what it says.
            if (self.wantW > 0 && self.wantH > 0) {
                // Express the requested size as a FLOOR on the content, not as
                // a one-shot setContentSize:.
                //
                // A window whose contentView uses Auto Layout re-adopts the
                // content's fitting size on every layout pass, so an imperative
                // setContentSize: is silently undone a frame later (measured:
                // 520 → 318 within 300ms). table_demo asks for 520x420 and its
                // rows are only 318 wide, so it settled at 318 and could not be
                // widened even through /window/resize.
                //
                // A >= constraint is the durable form of the same intent: the
                // content may never be smaller than what the app asked for, and
                // layout has nothing to revert.
                //
                // It guarantees the STARTING size only. An explicit resize
                // retracts it (see AETHER_DRV_WIN_RESIZE) — otherwise the floor
                // becomes a cage and a window can never be made smaller than it
                // was born, which broke the split spec's shrink.
                aeui_win_floor_w = [host.widthAnchor
                    constraintGreaterThanOrEqualToConstant:self.wantW];
                aeui_win_floor_h = [host.heightAnchor
                    constraintGreaterThanOrEqualToConstant:self.wantH];
                aeui_win_floor_w.active = YES;
                aeui_win_floor_h.active = YES;
                [self.window setContentSize:NSMakeSize(self.wantW, self.wantH)];
                [host layoutSubtreeIfNeeded];
            }
        }
    }
    // Honor AETHER_UI_HEADLESS for CI and unattended scenarios. The window
    // still exists and receives events (so the test server keeps working),
    // but it is never ordered onto the visible desktop. Matches the
    // SW_HIDE / gtk_widget_realize semantics in the other backends.
    const char* headless = getenv("AETHER_UI_HEADLESS");
    int is_headless = headless && headless[0] && headless[0] != '0';
    if (!is_headless) {
        [self.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    // Headless apps are driven by the test server and exit via /shutdown, never
    // by window closes — none of their windows are ever ordered onto the desktop,
    // so a driver closing a secondary window would otherwise read as "last window
    // closed" and terminate the process (killing the server mid-spec).
    const char* h = getenv("AETHER_UI_HEADLESS");
    if (h && h[0] && h[0] != '0') return NO;
    return YES;
}
@end

static AetherAppDelegate* app_delegate = nil;
static NSWindow* primary_window = nil;

int aether_ui_app_create(const char* title, int width, int height) {
    NSRect frame = NSMakeRect(200, 200, width, height);
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                               NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setTitle:[NSString stringWithUTF8String:title ? title : ""]];

    app_delegate = [[AetherAppDelegate alloc] init];
    app_delegate.window = window;
    app_delegate.rootHandle = 0;
    app_delegate.wantW = width;
    app_delegate.wantH = height;
    primary_window = window;
    return 1;
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    (void)app_handle;
    if (app_delegate) app_delegate.rootHandle = root_handle;
}

void aether_ui_app_run_raw(int app_handle) {
    (void)app_handle;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setDelegate:app_delegate];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        [app setMainMenu:menubar];

        [app run];
    }
}

// ---------------------------------------------------------------------------
// Surface table — "DSL with Scope" surfaces (window / render_to / record).
// Platform-agnostic handle bookkeeping (mirrors aether_ui_gtk4.c); the
// only platform calls are through the shared app/vstack ABI.
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
// so the AeVG `vg { … }` deferred-colour fix works identically on macOS.
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
        AeClosure* c = deferred_flushes[i];
        if (c && c->fn) {
            ((void(*)(void*))c->fn)(c->env);
        }
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
    // Auto-start the driver test server when AETHER_UI_TEST_PORT is set, as
    // win32/GTK4 do in their run loops. The window(){} block form OWNS the
    // event loop, so a demo that calls enable_test_server AFTER the block never
    // reaches it — without this, such a demo has no driver on macOS. Idempotent
    // (enable_test_server_impl guards test_server_started), so a demo that calls
    // it INSIDE the block still starts exactly one server.
    const char* test_port_env = getenv("AETHER_UI_TEST_PORT");
    if (test_port_env) {
        int port = atoi(test_port_env);
        if (port > 0) aether_ui_enable_test_server_impl(port, container_handle);
    }
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
// Widget creation
// ---------------------------------------------------------------------------

int aether_ui_text_create(const char* text) {
    NSTextField* label = [NSTextField labelWithString:
        [NSString stringWithUTF8String:text ? text : ""]];
    [label setEditable:NO];
    [label setBordered:NO];
    [label setSelectable:NO];
    [label setBackgroundColor:[NSColor clearColor]];
    return register_widget_typed((__bridge void*)label, AUI_TEXT);
}

int aether_ui_text_wrapped_create(const char* text, int wrap_width_px) {
    NSTextField* label = [NSTextField wrappingLabelWithString:
        [NSString stringWithUTF8String:text ? text : ""]];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setBackgroundColor:[NSColor clearColor]];
    if (wrap_width_px > 0) {
        [label setPreferredMaxLayoutWidth:(CGFloat)wrap_width_px];
        [[label widthAnchor] constraintEqualToConstant:(CGFloat)wrap_width_px].active = YES;
    }
    int handle = register_widget_typed((__bridge void*)label, AUI_TEXT);
    label.tag = 0x57524150;  // 'WRAP' marker so get_wrap can report it
    return handle;
}

void aether_ui_text_set_anchor(int handle, int anchor) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSTextField class]]) return;
    NSTextAlignment a = anchor == 1 ? NSTextAlignmentCenter
                      : anchor == 2 ? NSTextAlignmentRight
                                    : NSTextAlignmentLeft;
    [(NSTextField*)v setAlignment:a];
}

int aether_ui_text_get_wrap(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSTextField class]]) return 0;
    return ((NSTextField*)v).tag == 0x57524150 ? 1 : 0;
}

void aether_ui_text_set_truncate(int handle, int mode) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSTextField class]]) return;
    NSTextField* tf = (NSTextField*)v;
    NSLineBreakMode lb = NSLineBreakByClipping;
    if (mode == 1) lb = NSLineBreakByTruncatingHead;
    else if (mode == 2) lb = NSLineBreakByTruncatingMiddle;
    else if (mode == 3) lb = NSLineBreakByTruncatingTail;
    else if (aether_ui_text_get_wrap(handle)) lb = NSLineBreakByWordWrapping;
    [[tf cell] setLineBreakMode:lb];
    // AppKit only ellipsizes when the text is allowed to run past the box.
    // A wrapped label keeps its line budget; a single-line one gets 1.
    if (mode != 0 && !aether_ui_text_get_wrap(handle)) [tf setMaximumNumberOfLines:1];
    [tf setNeedsDisplay:YES];
}

int aether_ui_text_get_truncate(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSTextField class]]) return 0;
    switch ([[(NSTextField*)v cell] lineBreakMode]) {
        case NSLineBreakByTruncatingHead:   return 1;
        case NSLineBreakByTruncatingMiddle: return 2;
        case NSLineBreakByTruncatingTail:   return 3;
        default:                            return 0;
    }
}
int aether_ui_text_get_anchor(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSTextField class]]) return 0;
    NSTextAlignment a = [(NSTextField*)v alignment];
    return a == NSTextAlignmentCenter ? 1 : a == NSTextAlignmentRight ? 2 : 0;
}

void aether_ui_text_set_string(int handle, const char* text) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSTextField class]]) {
        [(NSTextField*)v setStringValue:
            [NSString stringWithUTF8String:text ? text : ""]];
    }
}

void aether_ui_button_set_label(int handle, const char* label) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSButton class]]) {
        [(NSButton*)v setTitle:
            [NSString stringWithUTF8String:label ? label : ""]];
    }
}

// ---------------------------------------------------------------------------
// Right-click context menus.
//
// -[NSView setMenu:] IS the native shape: AppKit pops it on right-click with
// no gesture wiring of our own. The menu is created lazily on the first item,
// so a widget that never asks for one carries no menu at all.
//
// The driver reaches the same menu by handle (POST /widget/{id}/context_menu
// [/{idx}]), so a spec exercises the very closures a real right-click fires
// rather than a parallel test-only path.
// ---------------------------------------------------------------------------
@interface AetherMenuTarget : NSObject
- (void)fire:(id)sender;
@end

static AetherMenuTarget* g_menu_target;   // defined with the menubar code below

// The context menu attached to a widget, or nil.
static NSMenu* widget_context_menu(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    return v ? [v menu] : nil;
}

void aether_ui_context_menu_item_impl(int handle, const char* label,
                                      void* boxed_closure) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    if (!g_menu_target) g_menu_target = [[AetherMenuTarget alloc] init];

    NSMenu* m = [v menu];
    if (!m) {                       // lazily created on the first item
        m = [[NSMenu alloc] initWithTitle:@""];
        [m setAutoenablesItems:NO];
        [v setMenu:m];
    }
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithUTF8String:(label ? label : "")]
               action:@selector(fire:) keyEquivalent:@""];
    [item setTarget:g_menu_target];
    [item setRepresentedObject:[NSNumber numberWithLongLong:(intptr_t)boxed_closure]];
    [m addItem:item];
}

// Driver support: does this widget have a menu (and how many items)?
static int aeui_ctx_menu_map(int handle) {
    NSMenu* m = widget_context_menu(handle);
    return m ? 1 : 0;
}

// Fire item `idx`. Returns 1 on success, 0 if there's no such item — the
// driver turns that into an honest 404 rather than a silent green.
static int aeui_ctx_menu_activate(int handle, int idx) {
    NSMenu* m = widget_context_menu(handle);
    if (!m || idx < 0 || idx >= (int)[m numberOfItems]) return 0;
    NSMenuItem* item = [m itemAtIndex:idx];
    if (![item target]) return 0;
    [g_menu_target fire:item];
    return 1;
}

// ---------------------------------------------------------------------------
// Shortcuts + focus (item 9).
//
// Accelerators are window-scoped and fire regardless of focus — the same
// contract GTK4's GtkShortcutController gives. AppKit has no equivalent
// controller, so we install ONE local NSEvent key-down monitor and match
// against a registry of normalized combos.
//
// Normalization is the crux: the DSL accepts "Ctrl+R", GTK's "<Control>r",
// and bare keys like "Tab". Both the live NSEvent path and the driver's
// POST /window/key path funnel through combo_normalize(), so a spec that
// posts "Ctrl+B" fires exactly the accelerator a human pressing it would.
//
// Ctrl maps to the literal Control key, NOT Command. Command would be the
// idiomatic Mac accelerator, but the combo string is the cross-platform
// contract and a spec asserting "Ctrl+B" must mean what it says. Cmd+<key>
// is accepted as an explicit alias so apps can still ask for it by name.
// ---------------------------------------------------------------------------
#define AEUI_MOD_CTRL  (1 << 0)
#define AEUI_MOD_ALT   (1 << 1)
#define AEUI_MOD_SHIFT (1 << 2)
#define AEUI_MOD_CMD   (1 << 3)

typedef struct {
    char       combo[64];   // canonical: "ctrl+shift+b"
    AeClosure* closure;
    AeClosure* enabled;     // optional predicate |-> int (1=active); NULL = always
} ShortcutEntry;

static ShortcutEntry* shortcuts = NULL;
static int shortcut_count = 0;
static int shortcut_capacity = 0;
static id  shortcut_monitor = nil;

// Render (mods, key) into the canonical form: modifiers in a fixed order,
// key lowercased. Fixed order is what makes "Shift+Ctrl+B" and "Ctrl+Shift+B"
// the same accelerator.
static void combo_canonical(int mods, const char* key, char* out, int outsize) {
    char lowered[32];
    int i = 0;
    for (; key && key[i] && i < (int)sizeof(lowered) - 1; i++) {
        lowered[i] = (char)tolower((unsigned char)key[i]);
    }
    lowered[i] = '\0';
    snprintf(out, outsize, "%s%s%s%s%s",
             (mods & AEUI_MOD_CTRL)  ? "ctrl+"  : "",
             (mods & AEUI_MOD_ALT)   ? "alt+"   : "",
             (mods & AEUI_MOD_SHIFT) ? "shift+" : "",
             (mods & AEUI_MOD_CMD)   ? "cmd+"   : "",
             lowered);
}

// Parse "Ctrl+Shift+R" / "<Control><Shift>r" / "Tab" → canonical form.
static void combo_normalize(const char* combo, char* out, int outsize) {
    out[0] = '\0';
    if (!combo || !*combo) return;

    int mods = 0;
    char key[32] = "";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", combo);

    // GTK angle-bracket form: <Control><Shift>r
    if (strchr(buf, '<')) {
        const char* p = buf;
        while (*p == '<') {
            const char* close = strchr(p, '>');
            if (!close) break;
            size_t len = (size_t)(close - p - 1);
            char name[32];
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, p + 1, len);
            name[len] = '\0';
            if (strcasecmp(name, "Control") == 0 || strcasecmp(name, "Primary") == 0
                || strcasecmp(name, "Ctrl") == 0)       mods |= AEUI_MOD_CTRL;
            else if (strcasecmp(name, "Shift") == 0)     mods |= AEUI_MOD_SHIFT;
            else if (strcasecmp(name, "Alt") == 0)       mods |= AEUI_MOD_ALT;
            else if (strcasecmp(name, "Meta") == 0
                     || strcasecmp(name, "Super") == 0)  mods |= AEUI_MOD_CMD;
            p = close + 1;
        }
        snprintf(key, sizeof(key), "%s", p);
    } else {
        // Plus-separated form: Ctrl+Shift+R (a bare "Tab" has no '+' and
        // falls through with mods == 0).
        for (char* tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
            if (strcasecmp(tok, "Ctrl") == 0 || strcasecmp(tok, "Control") == 0
                || strcasecmp(tok, "Primary") == 0)      mods |= AEUI_MOD_CTRL;
            else if (strcasecmp(tok, "Shift") == 0)      mods |= AEUI_MOD_SHIFT;
            else if (strcasecmp(tok, "Alt") == 0
                     || strcasecmp(tok, "Option") == 0)  mods |= AEUI_MOD_ALT;
            else if (strcasecmp(tok, "Cmd") == 0 || strcasecmp(tok, "Command") == 0
                     || strcasecmp(tok, "Meta") == 0
                     || strcasecmp(tok, "Super") == 0)   mods |= AEUI_MOD_CMD;
            else snprintf(key, sizeof(key), "%s", tok);
        }
    }
    combo_canonical(mods, key, out, outsize);
}

// Fire the accelerator registered for `combo`. Returns 1 if one fired.
// Shared by the NSEvent monitor and the driver's /window/key route.
static int shortcut_fire(const char* canonical) {
    if (!canonical || !*canonical) return 0;
    for (int i = 0; i < shortcut_count; i++) {
        if (strcmp(shortcuts[i].combo, canonical) != 0) continue;
        ShortcutEntry* s = &shortcuts[i];
        // A conditional shortcut whose predicate returns 0 is inert: keep
        // scanning (another binding of the same combo may be active), and if
        // none fires the key propagates as if unbound.
        if (s->enabled && s->enabled->fn) {
            if (!((int(*)(void*))s->enabled->fn)(s->enabled->env)) continue;
        }
        AeClosure* c = s->closure;
        if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
        return 1;
    }
    return 0;
}

// Map a live NSEvent key-down to the canonical combo string.
static void event_to_combo(NSEvent* ev, char* out, int outsize) {
    NSEventModifierFlags f = [ev modifierFlags];
    int mods = 0;
    if (f & NSEventModifierFlagControl)  mods |= AEUI_MOD_CTRL;
    if (f & NSEventModifierFlagOption)   mods |= AEUI_MOD_ALT;
    if (f & NSEventModifierFlagShift)    mods |= AEUI_MOD_SHIFT;
    if (f & NSEventModifierFlagCommand)  mods |= AEUI_MOD_CMD;

    // charactersIgnoringModifiers gives the unmodified key ("b" for Ctrl+B).
    NSString* chars = [ev charactersIgnoringModifiers];
    const char* key = (chars && [chars length]) ? [chars UTF8String] : "";
    char named[16];
    // Named keys the DSL spells out; their unicode forms are unprintable.
    switch ([ev keyCode]) {
        case 48:  snprintf(named, sizeof(named), "tab");    key = named; break;
        case 53:  snprintf(named, sizeof(named), "escape"); key = named; break;
        case 51:  snprintf(named, sizeof(named), "backspace"); key = named; break;
        case 117: snprintf(named, sizeof(named), "delete"); key = named; break;
        case 36:  snprintf(named, sizeof(named), "return"); key = named; break;
        case 49:  snprintf(named, sizeof(named), "space");  key = named; break;
        default: break;
    }
    // Shift is folded into the character for printable keys ("B" not Shift+b),
    // so only keep it as a modifier for the named keys above.
    if (key != named) mods &= ~AEUI_MOD_SHIFT;
    combo_canonical(mods, key, out, outsize);
}

// ── Chorded shortcuts (two-key sequences, "Ctrl+K Ctrl+S") ──────────
// Emacs/VSCode style: a prefix combo arms a pending state; the next combo,
// within ~1.5s, completes the chord. Mirrors the GTK4 state machine, keyed off
// the same canonical combo strings the rest of the shortcut layer uses.
typedef struct {
    char       prefix[64];   // canonical first combo
    char       second[64];   // canonical second combo
    AeClosure* closure;
} ChordEntry;
static ChordEntry* chords = NULL;
static int chord_count = 0, chord_capacity = 0;
static char chord_pending[64] = "";   // armed prefix, "" = none
static double chord_armed_at = 0.0;   // CLOCK_MONOTONIC seconds when armed

static double chord_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Feed a canonical combo into the chord machine. Returns 1 if it consumed the
// key (armed a prefix or completed a chord), 0 to fall through to shortcuts.
static int chord_feed(const char* canonical) {
    if (!canonical || !*canonical) return 0;
    // Expire a stale prefix (mirrors GTK4's 1.5s g_timeout).
    if (chord_pending[0] && (chord_mono_now() - chord_armed_at) > 1.5) {
        chord_pending[0] = '\0';
    }
    if (chord_pending[0]) {
        for (int i = 0; i < chord_count; i++) {
            if (strcmp(chords[i].prefix, chord_pending) == 0 &&
                strcmp(chords[i].second, canonical) == 0) {
                AeClosure* c = chords[i].closure;
                chord_pending[0] = '\0';
                if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
                return 1;
            }
        }
        // Armed but not a valid completion — cancel and let it fall through.
        chord_pending[0] = '\0';
    }
    // Not armed: does this key start a chord?
    for (int i = 0; i < chord_count; i++) {
        if (strcmp(chords[i].prefix, canonical) == 0) {
            snprintf(chord_pending, sizeof(chord_pending), "%s", canonical);
            chord_armed_at = chord_mono_now();
            return 1;
        }
    }
    return 0;
}

// The single resolved-combo entry point: chord layer first (a prefix arms, a
// completion fires), then plain/conditional shortcuts. Returns 1 if consumed.
static int shortcut_dispatch(const char* canonical) {
    if (chord_feed(canonical)) return 1;
    return shortcut_fire(canonical);
}

// The any-key handler (ui.window_on_key). One per window, matching the one
// shortcut registry above.
static AeClosure* window_key_closure = NULL;
static void aeui_key_name_for_event(NSEvent* ev, char* out, int outsize);

void aether_ui_window_on_key_impl(void* boxed_closure) {
    window_key_closure = (AeClosure*)boxed_closure;
}

int aether_ui_window_key_deliver(const char* key_name, int mods) {
    if (!window_key_closure || !window_key_closure->fn || !key_name) return 0;
    ((void(*)(void*, const char*, int))window_key_closure->fn)(
        window_key_closure->env, key_name, mods);
    return 1;
}

// Split a canonical combo ("ctrl+shift+a") into its modifier bits and bare
// key, so the driver path hands the closure the same two arguments a real
// keypress does.
static int aeui_combo_split(const char* canonical, char* name, int namesize) {
    int mods = 0;
    const char* p = canonical;
    while (p && *p) {
        const char* plus = strchr(p, '+');
        if (!plus) break;
        size_t n = (size_t)(plus - p);
        if      (n == 5 && strncmp(p, "shift", 5) == 0) mods |= 1;
        else if (n == 4 && strncmp(p, "ctrl",  4) == 0) mods |= 2;
        else if (n == 3 && strncmp(p, "alt",   3) == 0) mods |= 4;
        else if (n == 5 && strncmp(p, "super", 5) == 0) mods |= 8;
        else if (n == 3 && strncmp(p, "cmd",   3) == 0) mods |= 8;
        else break;   // not a modifier: the rest is the key name
        p = plus + 1;
    }
    // CRITICAL: combo_normalize lower-cases, but the real key path spells
    // names as aeui_key_name_for_event does ("BackSpace", "Page_Up"). If the
    // driver delivered "backspace" while a keypress delivered "BackSpace",
    // every spec would be testing a string the app never actually receives.
    static const char* canon[] = {
        "left","Left", "right","Right", "up","Up", "down","Down",
        "return","Return", "escape","Escape", "tab","Tab", "space","space",
        "backspace","BackSpace", "delete","Delete", "home","Home", "end","End",
        "page_up","Page_Up", "page_down","Page_Down", NULL
    };
    for (int i = 0; canon[i]; i += 2) {
        if (strcmp(p, canon[i]) == 0) {
            snprintf(name, namesize, "%s", canon[i + 1]);
            return mods;
        }
    }
    snprintf(name, namesize, "%s", p ? p : "");
    return mods;
}

// One local key-down monitor serves every accelerator AND the chord layer.
// Swallow the event when consumed so the key doesn't also reach the focused
// control (a Ctrl+B accelerator must not type "b" into the focused entry).
static void ensure_shortcut_monitor(void) {
    if (shortcut_monitor) return;
    shortcut_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent*(NSEvent* ev) {
            char canonical[64];
            event_to_combo(ev, canonical, sizeof(canonical));
            if (shortcut_dispatch(canonical)) return nil;  // consumed
            // No accelerator wanted it: offer it to the any-key handler, and
            // pass the event on regardless so the focused control still sees
            // what was typed.
            if (window_key_closure) {
                char name[64];
                aeui_key_name_for_event(ev, name, sizeof(name));
                NSEventModifierFlags f = [ev modifierFlags];
                int mods = 0;
                if (f & NSEventModifierFlagShift)   mods |= 1;
                if (f & NSEventModifierFlagControl) mods |= 2;
                if (f & NSEventModifierFlagOption)  mods |= 4;
                if (f & NSEventModifierFlagCommand) mods |= 8;
                aether_ui_window_key_deliver(name, mods);
            }
            return ev;
        }];
}

// Conditional shortcut: `enabled_closure` (|-> int) gates the combo; 0 → inert
// (key propagates). NULL predicate = always active (plain ui.shortcut).
void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure,
                                  void* enabled_closure) {
    if (!combo || !*combo) return;
    if (shortcut_count >= shortcut_capacity) {
        shortcut_capacity = shortcut_capacity == 0 ? 8 : shortcut_capacity * 2;
        shortcuts = (ShortcutEntry*)realloc(shortcuts,
                                            sizeof(ShortcutEntry) * shortcut_capacity);
        if (!shortcuts) { shortcut_count = 0; shortcut_capacity = 0; return; }
    }
    combo_normalize(combo, shortcuts[shortcut_count].combo,
                    sizeof(shortcuts[shortcut_count].combo));
    shortcuts[shortcut_count].closure = (AeClosure*)boxed_closure;
    shortcuts[shortcut_count].enabled = (AeClosure*)enabled_closure;
    shortcut_count++;
    ensure_shortcut_monitor();
}

void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) {
    aether_ui_shortcut_when_impl(combo, boxed_closure, NULL);
}

// Chorded shortcut: `first_combo` then `second_combo` (within ~1.5s) fires cb.
void aether_ui_shortcut_chord_impl(const char* first_combo,
                                   const char* second_combo,
                                   void* boxed_closure) {
    if (!first_combo || !*first_combo || !second_combo || !*second_combo) return;
    if (chord_count >= chord_capacity) {
        chord_capacity = chord_capacity == 0 ? 8 : chord_capacity * 2;
        chords = (ChordEntry*)realloc(chords, sizeof(ChordEntry) * chord_capacity);
        if (!chords) { chord_count = 0; chord_capacity = 0; return; }
    }
    combo_normalize(first_combo, chords[chord_count].prefix,
                    sizeof(chords[chord_count].prefix));
    combo_normalize(second_combo, chords[chord_count].second,
                    sizeof(chords[chord_count].second));
    chords[chord_count].closure = (AeClosure*)boxed_closure;
    chord_count++;
    ensure_shortcut_monitor();
}

// Explicit keyboard-focus grab. AppKit's equivalent of gtk_widget_grab_focus
// is makeFirstResponder: on the view's window.
void aether_ui_focus_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    NSWindow* win = [v window] ?: primary_window;
    if (!win) return;
    // NSTextField's editing first responder is its field editor, so focusing
    // an entry makes the WINDOW's firstResponder an NSTextView whose delegate
    // is the field. focused_widget() below resolves that back to the field.
    [win makeFirstResponder:v];
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


// ---------------------------------------------------------------------------
// Button click target — holds an AeClosure and dispatches to it.
// ---------------------------------------------------------------------------
@interface AetherButtonTarget : NSObject
@property (assign) AeClosure* closure;
- (void)buttonPressed:(id)sender;
@end

@implementation AetherButtonTarget
- (void)buttonPressed:(id)sender {
    (void)sender;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*))self.closure->fn)(self.closure->env);
    }
}
@end

// Keep strong refs so ARC doesn't release them
static NSMutableArray* retained_targets = nil;
static void retain_target(id obj) {
    if (!retained_targets) retained_targets = [NSMutableArray array];
    [retained_targets addObject:obj];
}

// Default low content-hugging priority so buttons fill horizontal space in
// hstacks (matching GTK4's grid-like look on single-char button rows).
// NSBezelStyleRegularSquare also makes the button render edge-to-edge inside
// its frame — AppKit's default rounded bezel has a fixed intrinsic height and
// refuses to stretch, leaving wasted space in tall cells (calculator grid).
static void configure_button(NSButton* btn) {
    [btn setContentHuggingPriority:200
                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [btn setContentHuggingPriority:200
                    forOrientation:NSLayoutConstraintOrientationVertical];
    [btn setBezelStyle:NSBezelStyleRegularSquare];
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    NSButton* btn = [NSButton buttonWithTitle:
        [NSString stringWithUTF8String:label ? label : ""]
                                       target:nil action:nil];
    configure_button(btn);
    if (boxed_closure) {
        AetherButtonTarget* target = [[AetherButtonTarget alloc] init];
        target.closure = (AeClosure*)boxed_closure;
        [btn setTarget:target];
        [btn setAction:@selector(buttonPressed:)];
        retain_target(target);
    }
    return register_widget_typed((__bridge void*)btn, AUI_BUTTON);
}

int aether_ui_button_create_plain(const char* label) {
    NSButton* btn = [NSButton buttonWithTitle:
        [NSString stringWithUTF8String:label ? label : ""]
                                       target:nil action:nil];
    configure_button(btn);
    return register_widget_typed((__bridge void*)btn, AUI_BUTTON);
}

void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    if ([v isKindOfClass:[NSButton class]]) {
        AetherButtonTarget* target = [[AetherButtonTarget alloc] init];
        target.closure = (AeClosure*)boxed_closure;
        [(NSButton*)v setTarget:target];
        [(NSButton*)v setAction:@selector(buttonPressed:)];
        retain_target(target);
    } else {
        // For non-button widgets, attach a click gesture recognizer
        aether_ui_on_click_impl(handle, boxed_closure);
    }
}

int aether_ui_vstack_create(int spacing) {
    NSStackView* stack = [[NSStackView alloc] init];
    [stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [stack setSpacing:spacing];
    [stack setAlignment:NSLayoutAttributeLeading];
    // Fill distribution: vertical slack goes to children by hugging priority
    // so spacer() absorbs most of it and hstack rows grow to fill leftover
    // — matches GTK4's box behaviour.
    [stack setDistribution:NSStackViewDistributionFill];
    [stack setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)stack, AUI_VSTACK);
}

// ---------------------------------------------------------------------------
// splitview — a real NSSplitView with a draggable divider.
//
// CAREFUL: GTK and AppKit use OPPOSITE senses of "vertical". GTK's
// vertical=1 means the PANES are stacked vertically (a horizontal divider);
// NSSplitView's vertical=YES means the DIVIDER is vertical (panes side by
// side). The ABI follows GTK, so the flag is inverted on the way in.
// ---------------------------------------------------------------------------
int aether_ui_splitview_create(int vertical) {
    NSSplitView* sv = [[NSSplitView alloc] init];
    [sv setVertical:(vertical ? NO : YES)];   // inverted — see above
    [sv setDividerStyle:NSSplitViewDividerStyleThin];
    [sv setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)sv, AUI_SPLITVIEW);
}

// Position = pixels from the START edge (left for side-by-side panes, top for
// stacked ones) to the divider. -1 means "not a splitview" — a spec has to be
// able to tell an unwired backend from a divider that happens to sit at 0.
int aether_ui_split_position_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSSplitView class]]) return -1;
    NSSplitView* sv = (NSSplitView*)v;
    NSArray* subs = [sv subviews];
    if ([subs count] < 1) return 0;
    NSRect first = [(NSView*)subs[0] frame];
    // isVertical == YES → side-by-side → the first pane's WIDTH is the offset.
    return (int)lround([sv isVertical] ? first.size.width : first.size.height);
}

void aether_ui_split_set_position_impl(int handle, int px) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSSplitView class]]) return;
    NSSplitView* sv = (NSSplitView*)v;
    if ([[sv subviews] count] < 2) return;
    [sv setPosition:(CGFloat)px ofDividerAtIndex:0];
    [sv layoutSubtreeIfNeeded];
}

// ---------------------------------------------------------------------------
// weight — proportional main-axis sharing (Flutter Expanded semantics):
// unweighted children keep their natural size, weighted ones split whatever
// is left over in the ratio of their weights.
//
// GTK gets this from a custom GtkLayoutManager. AppKit can express it in Auto
// Layout directly: weighted children get near-zero hugging (so they, and only
// they, absorb the slack), and each is pinned to the first weighted sibling by
// a multiplier constraint, which fixes the ratio.
// ---------------------------------------------------------------------------
static int aeui_effective_weight(int handle) {
    if (handle < 1 || handle > widget_count) return 0;
    if (widget_weights[handle - 1] > 0) return widget_weights[handle - 1];
    // A spacer is the implicit weight-1 child — same rule GTK applies to any
    // child with hexpand/vexpand set.
    return (get_widget_type(handle) == AUI_SPACER) ? 1 : 0;
}

static void aeui_apply_flex(NSStackView* stack) {
    if (!stack) return;
    BOOL horiz = ([stack orientation] == NSUserInterfaceLayoutOrientationHorizontal);
    NSLayoutAttribute attr = horiz ? NSLayoutAttributeWidth : NSLayoutAttributeHeight;
    NSLayoutConstraintOrientation orient = horiz
        ? NSLayoutConstraintOrientationHorizontal
        : NSLayoutConstraintOrientationVertical;

    // Drop the constraints from the previous pass — weight() can be called
    // again (or on a second child) and these must not accumulate.
    NSMutableArray* stale = [NSMutableArray array];
    for (NSLayoutConstraint* c in [stack constraints]) {
        if ([[c identifier] isEqualToString:@"aeui-flex"]) [stale addObject:c];
    }
    [stack removeConstraints:stale];

    NSView* ref = nil;
    int ref_weight = 0;
    for (NSView* child in [stack arrangedSubviews]) {
        int h = handle_for_view(child);
        int w = aeui_effective_weight(h);
        if (w <= 0) continue;   // unweighted: leave its natural size alone
        [child setContentHuggingPriority:1 forOrientation:orient];
        // A weighted child's explicit size (via width()/height(), an == on the
        // main axis) is a MINIMUM, not a fixed size — the proportional share
        // fills above it and it clamps to the min when space is tight. Downgrade
        // any such == to >= so it stops fighting the flex sharing (covers the
        // width()-before-weight() order; set_width covers the reverse).
        NSMutableArray* toMin = [NSMutableArray array];
        for (NSLayoutConstraint* wc in [child constraints]) {
            if (wc.firstAttribute == attr && wc.secondItem == nil
                && wc.relation == NSLayoutRelationEqual && [wc isActive]
                && ![[wc identifier] isEqualToString:@"aeui-flexmin"]) {
                [toMin addObject:wc];
            }
        }
        for (NSLayoutConstraint* wc in toMin) {
            wc.active = NO;
            NSLayoutConstraint* mn = [NSLayoutConstraint
                constraintWithItem:child attribute:attr
                         relatedBy:NSLayoutRelationGreaterThanOrEqual
                            toItem:nil attribute:NSLayoutAttributeNotAnAttribute
                        multiplier:1 constant:wc.constant];
            [mn setIdentifier:@"aeui-flexmin"];
            [mn setActive:YES];
        }
        if (!ref) {
            ref = child;
            ref_weight = w;
            continue;
        }
        NSLayoutConstraint* c =
            [NSLayoutConstraint constraintWithItem:child attribute:attr
                                         relatedBy:NSLayoutRelationEqual
                                            toItem:ref attribute:attr
                                        multiplier:(CGFloat)w / (CGFloat)ref_weight
                                          constant:0];
        [c setIdentifier:@"aeui-flex"];
        // Below the row-fill stretch (DefaultHigh, 750) AND the required
        // min-width, so the equality YIELDS rather than inflating the content:
        // a clamped child holds its min and the flexible sibling shrinks to fill
        // the remainder, instead of both being dragged up to the larger min
        // (which would grow the whole window). Still well above hugging (1), so
        // with room the proportional share holds.
        [c setPriority:700];
        [c setActive:YES];
    }
    [stack setNeedsLayout:YES];
}

void aether_ui_widget_weight_impl(int handle, int n) {
    if (handle < 1 || handle > widget_count || n < 1) return;
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    widget_weights[handle - 1] = n;
    NSView* parent = [v superview];
    if ([parent isKindOfClass:[NSStackView class]]) {
        aeui_apply_flex((NSStackView*)parent);
    }
}

// RTL: an NSStackView honours userInterfaceLayoutDirection, reversing its
// arranged subviews (first child on the right).
void aether_ui_set_rtl(int handle, int on) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSStackView class]]) return;
    [(NSStackView*)v setUserInterfaceLayoutDirection:
        on ? NSUserInterfaceLayoutDirectionRightToLeft
           : NSUserInterfaceLayoutDirectionLeftToRight];
}

// ---------------------------------------------------------------------------
// on_layout — the GeometryReader: cb(w, h) after the container's allocation
// changes. Frame-change notifications give us this without subclassing every
// stack.
// ---------------------------------------------------------------------------
@interface AetherLayoutObserver : NSObject
@property (assign) AeClosure* closure;
@property (assign) int lastW;
@property (assign) int lastH;
@end

@implementation AetherLayoutObserver
- (void)frameChanged:(NSNotification*)note {
    NSView* v = [note object];
    if (!v) return;
    int w = (int)lround([v bounds].size.width);
    int h = (int)lround([v bounds].size.height);
    if (w == self.lastW && h == self.lastH) return;   // CHANGE only, never per-frame
    self.lastW = w;
    self.lastH = h;
    AeClosure* c = self.closure;
    if (!c || !c->fn) return;
    // Deferred, exactly as GTK defers to an idle callback: the closure is
    // free to build or mutate widgets, which is the entire point of a
    // GeometryReader, and doing that inside a layout pass is a re-entrancy bug.
    dispatch_async(dispatch_get_main_queue(), ^{
        ((void(*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)w, (intptr_t)h);
    });
}
@end

void aether_ui_on_layout_impl(int handle, void* boxed_closure) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    AetherLayoutObserver* obs = [[AetherLayoutObserver alloc] init];
    obs.closure = (AeClosure*)boxed_closure;
    obs.lastW = 0;
    obs.lastH = 0;
    [v setPostsFrameChangedNotifications:YES];
    [[NSNotificationCenter defaultCenter] addObserver:obs
                                             selector:@selector(frameChanged:)
                                                 name:NSViewFrameDidChangeNotification
                                               object:v];
    retain_target(obs);
}

// ---------------------------------------------------------------------------
// wrap — a flow container: children run left-to-right and drop to the next
// line when the width runs out. NSStackView cannot do this at any price, so
// this is a manual flow layout.
// ---------------------------------------------------------------------------
#define AEUI_WRAP_GAP 6.0

@interface AetherWrapView : NSView
@end

@implementation AetherWrapView
// Lay out at a GIVEN width, optionally committing frames. Returns the total
// height consumed — which is also how intrinsicContentSize answers, so the
// wrap reports the right height to the stack that contains it.
- (CGFloat)flowWithWidth:(CGFloat)maxw commit:(BOOL)commit {
    CGFloat x = 0, y = 0, row_h = 0;
    for (NSView* c in [self subviews]) {
        if ([c isHidden]) continue;
        NSSize s = [c fittingSize];
        if (s.width <= 0 || s.height <= 0) s = [c intrinsicContentSize];
        if (s.width <= 0) s = [c frame].size;
        if (x > 0 && x + s.width > maxw) {   // doesn't fit — next line
            x = 0;
            y += row_h + AEUI_WRAP_GAP;
            row_h = 0;
        }
        if (commit) {
            [c setTranslatesAutoresizingMaskIntoConstraints:YES];
            // AppKit's origin is bottom-left; flow runs top-down, so flip y.
            [c setFrame:NSMakeRect(x, [self bounds].size.height - y - s.height,
                                   s.width, s.height)];
        }
        x += s.width + AEUI_WRAP_GAP;
        if (s.height > row_h) row_h = s.height;
    }
    return y + row_h;
}

- (void)layout {
    [super layout];
    [self flowWithWidth:[self bounds].size.width commit:YES];
}

- (NSSize)intrinsicContentSize {
    CGFloat w = [self bounds].size.width;
    if (w <= 0) return NSMakeSize(NSViewNoIntrinsicMetric, NSViewNoIntrinsicMetric);
    return NSMakeSize(NSViewNoIntrinsicMetric, [self flowWithWidth:w commit:NO]);
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    // A new width means a new line count means a new height.
    [self invalidateIntrinsicContentSize];
}
@end

int aether_ui_wrap_create(void) {
    AetherWrapView* wv = [[AetherWrapView alloc] init];
    [wv setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)wv, AUI_WRAP);
}

// ---------------------------------------------------------------------------
// tabs — a native tab strip over a page stack (NSTabView).
//
// GTK builds this from GtkStackSwitcher + GtkStack; the AppKit native is
// NSTabView, which draws the clickable strip and owns the page views itself.
// Each tab() adds an NSTabViewItem whose view is a vstack, and returns that
// vstack's handle so the tab's DSL block children lay out inside the page.
//
// The change callback fires on a REAL selection change — a strip click or a
// programmatic tabs_select — deduped against the last index, matching GTK.
// The dedup also swallows the initial selection AppKit makes when the first
// item is added, so on_change never fires spuriously at build time.
// ---------------------------------------------------------------------------
typedef struct {
    int        widget_handle;   // the NSTabView's registry handle
    int        page_count;
    int        last_index;
    AeClosure* on_change;
} TabsState;

static TabsState* tabs_states = NULL;
static int tabs_state_count = 0;
static int tabs_state_capacity = 0;

static TabsState* tabs_state_for_handle(int handle) {
    for (int i = 0; i < tabs_state_count; i++) {
        if (tabs_states[i].widget_handle == handle) return &tabs_states[i];
    }
    return NULL;
}

// The delegate turns AppKit's didSelect into the deduped on_change fire. One
// shared instance keyed by the tab view's handle covers every tabs composite.
@interface AetherTabsDelegate : NSObject <NSTabViewDelegate>
@end

@implementation AetherTabsDelegate
- (void)tabView:(NSTabView*)tabView didSelectTabViewItem:(NSTabViewItem*)item {
    int handle = handle_for_view(tabView);
    TabsState* ts = tabs_state_for_handle(handle);
    if (!ts) return;
    NSInteger idx = [tabView indexOfTabViewItem:item];
    if (idx == NSNotFound || (int)idx == ts->last_index) return;  // dedup
    ts->last_index = (int)idx;
    AeClosure* c = ts->on_change;
    if (c && c->fn) ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)idx);
}
@end

static AetherTabsDelegate* g_tabs_delegate = nil;

int aether_ui_tabs_create(void* boxed_closure) {
    NSTabView* tv = [[NSTabView alloc] init];
    [tv setTranslatesAutoresizingMaskIntoConstraints:NO];
    if (!g_tabs_delegate) g_tabs_delegate = [[AetherTabsDelegate alloc] init];
    [tv setDelegate:g_tabs_delegate];

    int handle = register_widget_typed((__bridge void*)tv, AUI_TABS);

    if (tabs_state_count >= tabs_state_capacity) {
        tabs_state_capacity = tabs_state_capacity == 0 ? 8 : tabs_state_capacity * 2;
        tabs_states = (TabsState*)realloc(tabs_states,
                                          sizeof(TabsState) * tabs_state_capacity);
    }
    TabsState* ts = &tabs_states[tabs_state_count++];
    ts->widget_handle = handle;
    ts->page_count = 0;
    ts->last_index = 0;
    ts->on_change = (AeClosure*)boxed_closure;
    return handle;
}

int aether_ui_tab_add(int tabs_handle, const char* title) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    if (!v || !ts || ![v isKindOfClass:[NSTabView class]]) return 0;
    NSTabView* tv = (NSTabView*)v;

    // The page body is a vstack, so the tab's block children attach the same
    // way they would in any vstack.
    int page = aether_ui_vstack_create(8);
    NSView* pageView = (__bridge NSView*)aether_ui_get_widget(page);

    NSString* ident = [NSString stringWithFormat:@"page_%d", ts->page_count];
    NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:ident];
    [item setLabel:[NSString stringWithUTF8String:title ? title : ""]];
    [item setView:pageView];
    [tv addTabViewItem:item];
    ts->page_count++;
    return page;
}

int aether_ui_tabs_selected(int tabs_handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(tabs_handle);
    if (!v || ![v isKindOfClass:[NSTabView class]]) return -1;
    NSTabView* tv = (NSTabView*)v;
    NSTabViewItem* sel = [tv selectedTabViewItem];
    if (!sel) return -1;
    NSInteger idx = [tv indexOfTabViewItem:sel];
    return idx == NSNotFound ? -1 : (int)idx;
}

int aether_ui_tabs_count(int tabs_handle) {
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    return ts ? ts->page_count : 0;
}

void aether_ui_tabs_select(int tabs_handle, int index) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(tabs_handle);
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    if (!v || !ts || ![v isKindOfClass:[NSTabView class]]) return;
    if (index < 0 || index >= ts->page_count) return;
    // Drives the delegate → fires on_change (deduped), exactly as a strip
    // click does.
    [(NSTabView*)v selectTabViewItemAtIndex:index];
}

void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    TabsState* ts = tabs_state_for_handle(tabs_handle);
    if (ts) ts->on_change = (AeClosure*)boxed_closure;
}

int aether_ui_hstack_create(int spacing) {
    NSStackView* stack = [[NSStackView alloc] init];
    [stack setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [stack setSpacing:spacing];
    [stack setAlignment:NSLayoutAttributeCenterY];
    // Fill distribution matches GTK4's box behavior: children grow/shrink
    // according to their content-hugging priority. Buttons (set to 200 at
    // creation) absorb leftover space; spacers (priority 1) soak up the rest.
    [stack setDistribution:NSStackViewDistributionFill];
    // Low vertical hugging so hstack rows can absorb vertical slack inside
    // a vstack with Fill distribution (grid-like rows in the calculator).
    [stack setContentHuggingPriority:200
                      forOrientation:NSLayoutConstraintOrientationVertical];
    [stack setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)stack, AUI_HSTACK);
}

int aether_ui_spacer_create(void) {
    NSView* spacer = [[NSView alloc] init];
    [spacer setTranslatesAutoresizingMaskIntoConstraints:NO];
    [spacer setContentHuggingPriority:1
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    [spacer setContentHuggingPriority:1
                       forOrientation:NSLayoutConstraintOrientationVertical];
    return register_widget_typed((__bridge void*)spacer, AUI_SPACER);
}

int aether_ui_divider_create(void) {
    NSBox* sep = [[NSBox alloc] init];
    [sep setBoxType:NSBoxSeparator];
    return register_widget_typed((__bridge void*)sep, AUI_DIVIDER);
}

// ---------------------------------------------------------------------------
// Input widgets — wire up AppKit target/action or delegates to AeClosures.
// ---------------------------------------------------------------------------

@interface AetherTextFieldDelegate : NSObject <NSTextFieldDelegate>
@property (assign) AeClosure* closure;
@property (assign) int stateHandle;   // >0 = two-way bind_value target
@end

@implementation AetherTextFieldDelegate
- (void)controlTextDidChange:(NSNotification*)n {
    NSTextField* tf = [n object];
    const char* cs = [[tf stringValue] UTF8String];
    if (!cs) cs = "";
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, const char*))self.closure->fn)(self.closure->env, cs);
    }
    // Two-way bind_value: mirror the field into its state (compare-first via
    // state_set_s → apply_prop_binding, which only re-sets on a real diff).
    if (self.stateHandle > 0) {
        aether_ui_state_set_s(self.stateHandle, cs);
    }
}
@end

int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    NSTextField* field = [[NSTextField alloc] init];
    [field setTranslatesAutoresizingMaskIntoConstraints:NO];
    [field setEditable:YES];
    [field setBordered:YES];
    [field setBezeled:YES];
    if (placeholder && *placeholder) {
        [field setPlaceholderString:[NSString stringWithUTF8String:placeholder]];
    }
    if (boxed_closure) {
        AetherTextFieldDelegate* d = [[AetherTextFieldDelegate alloc] init];
        d.closure = (AeClosure*)boxed_closure;
        [field setDelegate:d];
        retain_target(d);
    }
    return register_widget_typed((__bridge void*)field, AUI_TEXTFIELD);
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSTextField class]]) {
        NSTextField* field = (NSTextField*)v;
        [field setStringValue:[NSString stringWithUTF8String:text ? text : ""]];
        // AppKit does NOT emit controlTextDidChange for a programmatic
        // setStringValue: (unlike GtkEntry's "changed" / win32's EN_CHANGE),
        // so a two-way bound field wouldn't propagate to its state on a
        // driver-side set_text. Mirror the write-back here. Compare-first in
        // apply_prop_binding breaks the echo, so this can't recurse.
        id d = [field delegate];
        if (!g_seeding_bound_field && d
            && [d isKindOfClass:[AetherTextFieldDelegate class]]) {
            int sh = ((AetherTextFieldDelegate*)d).stateHandle;
            if (sh > 0) aether_ui_state_set_s(sh, text ? text : "");
        }
    }
}

const char* aether_ui_textfield_get_text(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSTextField class]]) {
        return [[(NSTextField*)v stringValue] UTF8String];
    }
    return "";
}

// Two-way: editable widget ⇄ string state. State→widget is a VALUE
// PropBinding; widget→state is the delegate's controlTextDidChange write-back,
// keyed on stateHandle. Reuses the field's existing change-delegate if it has
// one, else attaches a fresh delegate carrying just the state handle. (Defined
// here, after the delegate class + retain_target, which it needs.)
void aether_ui_bind_value(int state_handle, int widget_handle) {
    PropBinding* b = prop_binding_new(AEUI_BIND_VALUE, state_handle, widget_handle);
    NSView* v = (__bridge NSView*)aether_ui_get_widget(widget_handle);
    if (v && [v isKindOfClass:[NSTextField class]]) {
        NSTextField* field = (NSTextField*)v;
        id existing = [field delegate];
        if (existing && [existing isKindOfClass:[AetherTextFieldDelegate class]]) {
            ((AetherTextFieldDelegate*)existing).stateHandle = state_handle;
        } else {
            AetherTextFieldDelegate* d = [[AetherTextFieldDelegate alloc] init];
            d.stateHandle = state_handle;
            [field setDelegate:d];
            retain_target(d);
        }
    }
    apply_prop_binding(b);  // seed the field from the state's initial value
}

int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    NSSecureTextField* field = [[NSSecureTextField alloc] init];
    [field setTranslatesAutoresizingMaskIntoConstraints:NO];
    if (placeholder && *placeholder) {
        [field setPlaceholderString:[NSString stringWithUTF8String:placeholder]];
    }
    if (boxed_closure) {
        AetherTextFieldDelegate* d = [[AetherTextFieldDelegate alloc] init];
        d.closure = (AeClosure*)boxed_closure;
        [field setDelegate:d];
        retain_target(d);
    }
    return register_widget_typed((__bridge void*)field, AUI_SECUREFIELD);
}

// Toggle — NSButton with switch style, target invokes closure with 0/1.
@interface AetherToggleTarget : NSObject
@property (assign) AeClosure* closure;
- (void)toggleChanged:(id)sender;
@end

// Turn OFF every other member of `handle`'s radio group. AppKit's own radio
// behaviour needs same-superview + same-action NSButtons, which our toggles
// are not (each carries its own target), so exclusivity is enforced here
// against the group table. Called from the action AND from set_active, so a
// programmatic select and a real click behave identically.
static void toggle_enforce_group(int handle) {
    if (handle < 1 || handle > widget_count) return;
    int leader = widget_group[handle - 1];
    if (!leader) return;
    for (int i = 1; i <= widget_count; i++) {
        if (i == handle || widget_group[i - 1] != leader) continue;
        NSView* peer = (__bridge NSView*)aether_ui_get_widget(i);
        if ([peer isKindOfClass:[NSButton class]]) {
            [(NSButton*)peer setState:NSControlStateValueOff];
        }
    }
}

@implementation AetherToggleTarget
- (void)toggleChanged:(id)sender {
    NSButton* btn = (NSButton*)sender;
    int active = [btn state] == NSControlStateValueOn ? 1 : 0;
    if (active) toggle_enforce_group(handle_for_view(btn));
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, intptr_t))self.closure->fn)(self.closure->env, (intptr_t)active);
    }
}
@end

int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    NSButton* check = [NSButton checkboxWithTitle:
        [NSString stringWithUTF8String:label ? label : ""]
                                           target:nil action:nil];
    if (boxed_closure) {
        AetherToggleTarget* target = [[AetherToggleTarget alloc] init];
        target.closure = (AeClosure*)boxed_closure;
        [check setTarget:target];
        [check setAction:@selector(toggleChanged:)];
        retain_target(target);
    }
    return register_widget_typed((__bridge void*)check, AUI_TOGGLE);
}

void aether_ui_toggle_set_active(int handle, int active) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSButton class]]) {
        [(NSButton*)v setState:active ? NSControlStateValueOn : NSControlStateValueOff];
        if (active) toggle_enforce_group(handle);
    }
}

int aether_ui_toggle_get_active(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSButton class]]) {
        return [(NSButton*)v state] == NSControlStateValueOn ? 1 : 0;
    }
    return 0;
}

// Put a toggle in a mutual-exclusion group with another, and render both as
// radios — the same thing GTK4 does when you group two check buttons.
//
// The group is keyed by a LEADER handle so that chaining (b→a, c→a, d→c) all
// resolves to one group: follow the target's leader if it already has one,
// otherwise the target itself becomes the leader.
void aether_ui_toggle_set_group(int handle, int group_with) {
    if (handle < 1 || handle > widget_count) return;
    if (group_with < 1 || group_with > widget_count) return;

    int leader = widget_group[group_with - 1];
    if (!leader) leader = group_with;

    widget_group[group_with - 1] = leader;
    widget_group[handle - 1] = leader;

    // Radio look for every member, including the leader (which was created as
    // a plain checkbox before it knew it was in a group).
    NSView* a = (__bridge NSView*)aether_ui_get_widget(handle);
    NSView* b = (__bridge NSView*)aether_ui_get_widget(leader);
    if ([a isKindOfClass:[NSButton class]]) {
        [(NSButton*)a setButtonType:NSButtonTypeRadio];
    }
    if ([b isKindOfClass:[NSButton class]]) {
        [(NSButton*)b setButtonType:NSButtonTypeRadio];
    }
}

// Slider — continuous; target invokes closure with double value.
@interface AetherSliderTarget : NSObject
@property (assign) AeClosure* closure;
- (void)sliderChanged:(id)sender;
@end

@implementation AetherSliderTarget
- (void)sliderChanged:(id)sender {
    NSSlider* s = (NSSlider*)sender;
    double val = [s doubleValue];
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, double))self.closure->fn)(self.closure->env, val);
    }
}
@end

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure) {
    NSSlider* slider = [NSSlider sliderWithValue:initial
                                        minValue:min_val
                                        maxValue:max_val
                                          target:nil action:nil];
    [slider setTranslatesAutoresizingMaskIntoConstraints:NO];
    [slider setContinuous:YES];
    if (boxed_closure) {
        AetherSliderTarget* target = [[AetherSliderTarget alloc] init];
        target.closure = (AeClosure*)boxed_closure;
        [slider setTarget:target];
        [slider setAction:@selector(sliderChanged:)];
        retain_target(target);
    }
    return register_widget_typed((__bridge void*)slider, AUI_SLIDER);
}

void aether_ui_slider_set_value(int handle, double value) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSSlider class]]) {
        [(NSSlider*)v setDoubleValue:value];
    }
}

double aether_ui_slider_get_value(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSSlider class]]) {
        return [(NSSlider*)v doubleValue];
    }
    return 0.0;
}

// Picker — NSPopUpButton; target invokes closure with selected index.
@interface AetherPickerTarget : NSObject
@property (assign) AeClosure* closure;
- (void)pickerChanged:(id)sender;
@end

@implementation AetherPickerTarget
- (void)pickerChanged:(id)sender {
    NSPopUpButton* p = (NSPopUpButton*)sender;
    intptr_t idx = [p indexOfSelectedItem];
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, intptr_t))self.closure->fn)(self.closure->env, idx);
    }
}
@end

int aether_ui_picker_create(void* boxed_closure) {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [popup setTranslatesAutoresizingMaskIntoConstraints:NO];
    if (boxed_closure) {
        AetherPickerTarget* target = [[AetherPickerTarget alloc] init];
        target.closure = (AeClosure*)boxed_closure;
        [popup setTarget:target];
        [popup setAction:@selector(pickerChanged:)];
        retain_target(target);
    }
    return register_widget_typed((__bridge void*)popup, AUI_PICKER);
}

void aether_ui_picker_add_item(int handle, const char* item) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSPopUpButton class]]) {
        [(NSPopUpButton*)v addItemWithTitle:
            [NSString stringWithUTF8String:item ? item : ""]];
    }
}

void aether_ui_picker_set_selected(int handle, int index) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSPopUpButton class]]) {
        [(NSPopUpButton*)v selectItemAtIndex:index];
    }
}

int aether_ui_picker_get_selected(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSPopUpButton class]]) {
        return (int)[(NSPopUpButton*)v indexOfSelectedItem];
    }
    return 0;
}

// Textarea — NSTextView in NSScrollView. Delegate fires closure on text change.
@interface AetherTextViewDelegate : NSObject <NSTextViewDelegate>
@property (assign) AeClosure* closure;
@end

@implementation AetherTextViewDelegate
- (void)textDidChange:(NSNotification*)n {
    NSTextView* tv = [n object];
    if (self.closure && self.closure->fn) {
        const char* cs = [[tv string] UTF8String];
        ((void(*)(void*, const char*))self.closure->fn)(self.closure->env, cs ? cs : "");
    }
}
@end

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    (void)placeholder;
    NSTextView* tv = [[NSTextView alloc] init];
    [tv setRichText:NO];
    [tv setEditable:YES];
    [tv setSelectable:YES];
    [tv setAutoresizingMask:NSViewWidthSizable];

    NSScrollView* scrollView = [[NSScrollView alloc] init];
    [scrollView setDocumentView:tv];
    [scrollView setHasVerticalScroller:YES];
    [scrollView setHasHorizontalScroller:NO];
    [scrollView setBorderType:NSBezelBorder];
    [scrollView setTranslatesAutoresizingMaskIntoConstraints:NO];
    [scrollView.heightAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;

    if (boxed_closure) {
        AetherTextViewDelegate* d = [[AetherTextViewDelegate alloc] init];
        d.closure = (AeClosure*)boxed_closure;
        [tv setDelegate:d];
        retain_target(d);
    }

    int scroll_handle = register_widget_typed((__bridge void*)scrollView, AUI_TEXTAREA);
    register_widget_typed((__bridge void*)tv, AUI_TEXTAREA_INNER);
    return scroll_handle;
}

void aether_ui_textarea_set_text(int handle, const char* text) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle + 1);
    if (v && [v isKindOfClass:[NSTextView class]]) {
        [(NSTextView*)v setString:
            [NSString stringWithUTF8String:text ? text : ""]];
    }
}

char* aether_ui_textarea_get_text(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle + 1);
    if (v && [v isKindOfClass:[NSTextView class]]) {
        return strdup([[(NSTextView*)v string] UTF8String]);
    }
    return strdup("");
}

// AppKit's clip view is bottom-left origin, so a document view SMALLER than
// the viewport sits at the BOTTOM of the scroll area, which for content that
// reads top-down means it is off the top of what you can see. A flipped clip
// view puts the origin where every other container in this backend has it.
@interface AetherFlippedClipView : NSClipView
@end
@implementation AetherFlippedClipView
- (BOOL)isFlipped { return YES; }
@end

int aether_ui_scrollview_create(void) {
    NSScrollView* sv = [[NSScrollView alloc] init];
    [sv setHasVerticalScroller:YES];
    [sv setTranslatesAutoresizingMaskIntoConstraints:NO];
    [sv setContentView:[[AetherFlippedClipView alloc] init]];
    // The scroll view paints its OWN background over any layer colour, so
    // leave it off until someone asks for one (set_bg_color turns it on).
    [sv setDrawsBackground:NO];
    return register_widget_typed((__bridge void*)sv, AUI_SCROLLVIEW);
}

int aether_ui_progressbar_create(double fraction) {
    NSProgressIndicator* bar = [[NSProgressIndicator alloc] init];
    [bar setStyle:NSProgressIndicatorStyleBar];
    [bar setIndeterminate:NO];
    [bar setMinValue:0.0];
    [bar setMaxValue:1.0];
    [bar setDoubleValue:fraction];
    [bar setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)bar, AUI_PROGRESSBAR);
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSProgressIndicator class]]) {
        [(NSProgressIndicator*)v setDoubleValue:fraction];
    }
}

// ---------------------------------------------------------------------------
// Layout containers
// ---------------------------------------------------------------------------

int aether_ui_zstack_create(void) {
    NSView* container = [[NSView alloc] init];
    [container setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)container, AUI_ZSTACK);
}

int aether_ui_form_create(void) {
    NSStackView* stack = [[NSStackView alloc] init];
    [stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [stack setSpacing:16];
    [stack setAlignment:NSLayoutAttributeLeading];
    [stack setEdgeInsets:NSEdgeInsetsMake(20, 20, 20, 20)];
    [stack setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)stack, AUI_VSTACK);
}

int aether_ui_form_section_create(const char* title) {
    NSBox* box = [[NSBox alloc] init];
    [box setTitle:[NSString stringWithUTF8String:title ? title : ""]];
    [box setTranslatesAutoresizingMaskIntoConstraints:NO];

    NSStackView* inner = [[NSStackView alloc] init];
    [inner setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [inner setSpacing:8];
    [inner setAlignment:NSLayoutAttributeLeading];
    [inner setEdgeInsets:NSEdgeInsetsMake(8, 8, 8, 8)];
    [box setContentView:inner];

    int frame_handle = register_widget_typed((__bridge void*)box, AUI_FORM_SECTION);
    register_widget_typed((__bridge void*)inner, AUI_FORM_SECTION_INNER);
    return frame_handle;
}

int aether_ui_navstack_create(void) {
    NSView* container = [[NSView alloc] init];
    [container setTranslatesAutoresizingMaskIntoConstraints:NO];
    return register_widget_typed((__bridge void*)container, AUI_NAVSTACK);
}

/* Page depth, which this backend did not track at all -- pop was a no-op
   here until the navstack work, so there was nothing to count. */
#define MAC_NAV_MAX 64
static int mac_nav_depth[MAC_NAV_MAX];

void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    NSView* container = (__bridge NSView*)aether_ui_get_widget(handle);
    NSView* body = (__bridge NSView*)aether_ui_get_widget(body_handle);
    if (!container || !body) return;
    /* THE TITLE, as real widgets -- see the GTK4 push for why this is a
       widget bar rather than a per-platform chrome API (NSTitlebar and
       friends have no win32 equivalent, so each backend would invent its
       own). Registered creators, so the driver sees the title as plain
       text and a spec can assert it. */
    if (title && title[0]) {
        int wrap_h = aether_ui_vstack_create(0);
        int bar_h = aether_ui_text_create(title);
        NSView* wrap = (__bridge NSView*)aether_ui_get_widget(wrap_h);
        NSView* bar = (__bridge NSView*)aether_ui_get_widget(bar_h);
        if (wrap && bar) {
            [bar setTranslatesAutoresizingMaskIntoConstraints:NO];
            [body setTranslatesAutoresizingMaskIntoConstraints:NO];
            [wrap addSubview:bar];
            [wrap addSubview:body];
            [bar.leadingAnchor constraintEqualToAnchor:wrap.leadingAnchor].active = YES;
            [bar.trailingAnchor constraintEqualToAnchor:wrap.trailingAnchor].active = YES;
            [bar.topAnchor constraintEqualToAnchor:wrap.topAnchor].active = YES;
            [body.leadingAnchor constraintEqualToAnchor:wrap.leadingAnchor].active = YES;
            [body.trailingAnchor constraintEqualToAnchor:wrap.trailingAnchor].active = YES;
            [body.topAnchor constraintEqualToAnchor:bar.bottomAnchor].active = YES;
            [body.bottomAnchor constraintEqualToAnchor:wrap.bottomAnchor].active = YES;
            body = wrap;          /* the PAGE is now the wrapper */
        }
    }
    if (handle >= 1 && handle <= MAC_NAV_MAX &&
        mac_nav_depth[handle - 1] < 32) {
        mac_nav_depth[handle - 1]++;
    }
    for (NSView* sub in [[container subviews] copy]) {
        [sub removeFromSuperview];
    }
    [body setTranslatesAutoresizingMaskIntoConstraints:NO];
    [container addSubview:body];
    [body.leadingAnchor constraintEqualToAnchor:container.leadingAnchor].active = YES;
    [body.trailingAnchor constraintEqualToAnchor:container.trailingAnchor].active = YES;
    [body.topAnchor constraintEqualToAnchor:container.topAnchor].active = YES;
    [body.bottomAnchor constraintEqualToAnchor:container.bottomAnchor].active = YES;
}

int aether_ui_navstack_depth(int handle) {
    if (handle < 1 || handle > MAC_NAV_MAX) return 0;
    return mac_nav_depth[handle - 1];
}

/* Defined with the other registry helpers below; navstack_pop needs it. */
static void unregister_view_tree(NSView* v);

void aether_ui_navstack_pop(int handle) {
    /* Was an empty stub: push replaced the container's subviews, and nothing
       ever put the previous page back, so a navstack could only go forwards.
       The container holds exactly one page at a time (push removes every
       subview first), so popping means dropping the current one and letting
       depth fall -- there is no previous view retained to restore, which is
       the honest limit of this design and is why depth is what the driver
       can see. */
    NSView* container = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!container) return;
    if (handle < 1 || handle > MAC_NAV_MAX) return;
    if (mac_nav_depth[handle - 1] <= 0) return;   // root: no-op, not underflow
    mac_nav_depth[handle - 1]--;
    for (NSView* sub in [[container subviews] copy]) {
        /* UNREGISTER, not just detach. removeFromSuperview takes the views out
           of the AppKit tree but leaves them in the widget registry, so
           /widgets keeps counting a page that is gone and `pop SHRANK the
           widget tree` fails while the UI looks right. Same defect the spec
           caught on win32, where the fix was mark_subtree_dead. The AppKit
           equivalent already existed (unregister_view_tree, written for
           remove_child) -- pop simply was not calling it. */
        unregister_view_tree(sub);
        [sub removeFromSuperview];
    }
}

// ---------------------------------------------------------------------------
// Styling
// ---------------------------------------------------------------------------

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setWantsLayer:YES];
    v.layer.backgroundColor = [[NSColor colorWithRed:r green:g blue:b alpha:a] CGColor];
    if ([v isKindOfClass:[NSButton class]]) {
        [(NSButton*)v setBordered:NO];
    }
    if ([v isKindOfClass:[NSScrollView class]]) {
        // An NSScrollView draws its own background OVER the layer, so setting
        // the layer colour alone is invisible. Say it in the scroll view's own
        // terms, and turn drawing back on (create leaves it off).
        NSScrollView* sv = (NSScrollView*)v;
        [sv setBackgroundColor:[NSColor colorWithRed:r green:g blue:b alpha:a]];
        [sv setDrawsBackground:YES];
    }
    // Stash packed 0xRRGGBB (+ marker bit) for driver readback (styled_bg_impl).
    int packed = (((int)(r * 255) & 255) << 16) | (((int)(g * 255) & 255) << 8)
               | ((int)(b * 255) & 255) | 0x1000000;
    objc_setAssociatedObject(v, "aeui_styled_bg", @(packed), OBJC_ASSOCIATION_RETAIN);
}

void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setWantsLayer:YES];
    CAGradientLayer* grad = [CAGradientLayer layer];
    grad.frame = v.bounds;
    grad.colors = @[
        (id)[[NSColor colorWithRed:r1 green:g1 blue:b1 alpha:1.0] CGColor],
        (id)[[NSColor colorWithRed:r2 green:g2 blue:b2 alpha:1.0] CGColor]
    ];
    if (vertical) {
        grad.startPoint = CGPointMake(0.5, 0.0);
        grad.endPoint = CGPointMake(0.5, 1.0);
    } else {
        grad.startPoint = CGPointMake(0.0, 0.5);
        grad.endPoint = CGPointMake(1.0, 0.5);
    }
    grad.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [v.layer insertSublayer:grad atIndex:0];
}

void aether_ui_set_text_color(int handle, double r, double g, double b) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSTextField class]]) {
        [(NSTextField*)v setTextColor:[NSColor colorWithRed:r green:g blue:b alpha:1.0]];
    }
    if (v) {
        int packed = (((int)(r * 255) & 255) << 16) | (((int)(g * 255) & 255) << 8)
                   | ((int)(b * 255) & 255) | 0x1000000;
        objc_setAssociatedObject(v, "aeui_styled_fg", @(packed), OBJC_ASSOCIATION_RETAIN);
    }
}

void aether_ui_set_font_size(int handle, double size) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSTextField class]]) {
        [(NSTextField*)v setFont:[NSFont systemFontOfSize:size]];
    }
}

void aether_ui_set_font_bold(int handle, int bold) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    // Stash the explicit weight for driver readback (AeCS re-theme proof).
    objc_setAssociatedObject(v, "aeui_styled_weight",
        @(bold ? 1 : 2), OBJC_ASSOCIATION_RETAIN);
    if ([v isKindOfClass:[NSTextField class]]) {
        NSFont* font = [(NSTextField*)v font];
        CGFloat size = font ? [font pointSize] : 13.0;
        if (bold)
            [(NSTextField*)v setFont:[NSFont boldSystemFontOfSize:size]];
        else
            [(NSTextField*)v setFont:[NSFont systemFontOfSize:size]];
    }
}

// Widget-level font family (AeCS v1.1). Verbatim family name via NSFont —
// generic CSS families map through userFixedPitch/system fallbacks.
void aether_ui_set_font_family(int handle, const char* family) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || !family || !family[0]) return;
    objc_setAssociatedObject(v, "aeui_styled_family",
        [NSString stringWithUTF8String:family], OBJC_ASSOCIATION_RETAIN);
    if ([v respondsToSelector:@selector(setFont:)]) {
        NSFont* cur = [(id)v font];
        CGFloat size = cur ? [cur pointSize] : 13.0;
        NSFont* f = nil;
        if (strcmp(family, "monospace") == 0)
            f = [NSFont userFixedPitchFontOfSize:size];
        else
            f = [NSFont fontWithName:[NSString stringWithUTF8String:family]
                                size:size];
        if (f) [(id)v setFont:f];
    }
}

const char* aether_ui_styled_font_family_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return "";
    NSString* f = objc_getAssociatedObject(v, "aeui_styled_family");
    return f ? f.UTF8String : "";
}

const char* aether_ui_styled_weight_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return "";
    NSNumber* n = objc_getAssociatedObject(v, "aeui_styled_weight");
    if (!n) return "";
    return [n intValue] == 1 ? "bold" : "normal";
}

void aether_ui_set_corner_radius(int handle, double radius) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setWantsLayer:YES];
    v.layer.cornerRadius = radius;
    v.layer.masksToBounds = YES;
}

// Solid border via the backing layer (same mechanism as cornerRadius, so a
// bordered pill composes correctly). width 0 clears it.
void aether_ui_set_border(int handle, double width, double r, double g, double b) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setWantsLayer:YES];
    v.layer.borderWidth = width;
    if (width > 0.0) {
        v.layer.borderColor = [[NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0] CGColor];
    }
    objc_setAssociatedObject(v, "aeui-styled-border",
        @(((int)width << 24) | 0x40000000
          | (((int)(r*255) & 0xFF) << 16) | (((int)(g*255) & 0xFF) << 8)
          | ((int)(b*255) & 0xFF)),
        OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// (width<<24) | flag | rgb, or -1 — same encoding as GTK4/win32.
int aether_ui_styled_border_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return -1;
    NSNumber* n = objc_getAssociatedObject(v, "aeui-styled-border");
    return n ? [n intValue] : -1;
}

// Interaction states. macOS already installs an NSTrackingArea for hover
// (see the hover section), so the colours are stashed on the view and the
// enter/exit handlers swap the layer background. state: 0=hover, 1=active.
void aether_ui_set_state_style(int handle, int state,
                               double br, double bg_, double bb,
                               double fr, double fg_, double fb) {
    (void)fr; (void)fg_; (void)fb;   // fg per-state: follow-up
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || br < 0.0) return;
    [v setWantsLayer:YES];
    objc_setAssociatedObject(v,
        state == 1 ? "aeui-active-style" : "aeui-hover-style",
        @(0x1000000 | (((int)(br*255) & 0xFF) << 16)
                    | (((int)(bg_*255) & 0xFF) << 8) | ((int)(bb*255) & 0xFF)),
        OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

int aether_ui_state_style_impl(int handle, int state) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return -1;
    NSNumber* n = objc_getAssociatedObject(v,
        state == 1 ? "aeui-active-style" : "aeui-hover-style");
    if (!n) return -1;
    return [n intValue] & 0xFFFFFF;
}

void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSStackView class]]) {
        [(NSStackView*)v setEdgeInsets:NSEdgeInsetsMake(top, left, bottom, right)];
    }
}

// Does this view carry its own width-to-constant constraint?
static int aeui_has_explicit_width(NSView* v) {
    for (NSLayoutConstraint* c in [v constraints]) {
        if (c.firstAttribute == NSLayoutAttributeWidth
            && c.secondItem == nil && [c isActive]) return 1;
    }
    return 0;
}

void aether_ui_set_width(int handle, int width) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setTranslatesAutoresizingMaskIntoConstraints:NO];

    // Drop any equal-width chain this view is part of. A table header sets an
    // explicit per-column width; leaving the button-row equality in place makes
    // the two required constraints unsatisfiable, and Auto Layout resolves that
    // by breaking one — silently, and with the wrong column widths surviving.
    NSView* p = [v superview];
    if (p) {
        NSMutableArray* drop = [NSMutableArray array];
        for (NSLayoutConstraint* c in [p constraints]) {
            if (![[c identifier] isEqualToString:@"aeui-btneq"]) continue;
            if (c.firstItem == v || c.secondItem == v) [drop addObject:c];
        }
        if ([drop count]) [p removeConstraints:drop];
    }

    // On a weighted child, width() is a FLOOR (>=), not a fixed size: the flex
    // share fills above it, clamping to this min only when space is tight. A
    // fixed == would fight aeui_apply_flex's proportional sharing. (The reverse
    // order — width() then weight() — is handled in aeui_apply_flex.)
    int weighted = (handle >= 1 && handle <= widget_count
                    && widget_weights[handle - 1] > 0);
    if (weighted) {
        NSLayoutConstraint* mn =
            [v.widthAnchor constraintGreaterThanOrEqualToConstant:width];
        [mn setIdentifier:@"aeui-flexmin"];
        mn.active = YES;
    } else {
        [v.widthAnchor constraintEqualToConstant:width].active = YES;
    }
}

void aether_ui_set_height(int handle, int height) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setTranslatesAutoresizingMaskIntoConstraints:NO];
    [v.heightAnchor constraintEqualToConstant:height].active = YES;
}

// Implicit-transition lookups — defined with apply_css, which records them.
static int aeui_opacity_transition_ms(int handle);
static int aeui_opacity_transition_ease_out(int handle);
static int aeui_opacity_transition_spring(int handle);
static void aeui_start_opacity_tween(NSView* v, int handle,
                                     double from, double to, int ms, int ease_out,
                                     int spring);

void aether_ui_set_opacity(int handle, double opacity) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    // Implicit transition (QML-Behavior-alike): if the app declared
    // ui.transition(h, "opacity", ms, …) once, every later opacity change
    // TWEENS instead of snapping. The declaration is recorded by apply_css.
    int ms = aeui_opacity_transition_ms(handle);
    if (ms > 0) {
        aeui_start_opacity_tween(v, handle, [v alphaValue], opacity, ms,
                                 aeui_opacity_transition_ease_out(handle),
                                 aeui_opacity_transition_spring(handle));
        return;
    }
    [v setAlphaValue:opacity];
}

void aether_ui_set_enabled(int handle, int enabled) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSControl class]]) {
        [(NSControl*)v setEnabled:enabled != 0];
    }
}

void aether_ui_set_tooltip(int handle, const char* text) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v) [v setToolTip:[NSString stringWithUTF8String:text ? text : ""]];
}

// ── Accessibility (semantics layer) ──────────────────────────────────
// NSView conforms to NSAccessibility, so set the role/label/help directly on
// the view — real, VoiceOver-visible. The driver reads the effective values
// back. See docs/design/accessibility.md.
static NSAccessibilityRole aeui_ns_role(const char* role) {
    if (!role) return nil;
    if (!strcmp(role, "button"))      return NSAccessibilityButtonRole;
    if (!strcmp(role, "checkbox"))    return NSAccessibilityCheckBoxRole;
    if (!strcmp(role, "radio"))       return NSAccessibilityRadioButtonRole;
    if (!strcmp(role, "link"))        return NSAccessibilityLinkRole;
    if (!strcmp(role, "heading"))     return NSAccessibilityStaticTextRole;
    if (!strcmp(role, "image"))       return NSAccessibilityImageRole;
    if (!strcmp(role, "group"))       return NSAccessibilityGroupRole;
    if (!strcmp(role, "list"))        return NSAccessibilityListRole;
    if (!strcmp(role, "listitem"))    return NSAccessibilityRowRole;
    if (!strcmp(role, "tab"))         return NSAccessibilityRadioButtonRole;
    if (!strcmp(role, "tablist"))     return NSAccessibilityTabGroupRole;
    if (!strcmp(role, "menu"))        return NSAccessibilityMenuRole;
    if (!strcmp(role, "menuitem"))    return NSAccessibilityMenuItemRole;
    if (!strcmp(role, "dialog"))      return NSAccessibilitySheetRole;
    if (!strcmp(role, "textbox"))     return NSAccessibilityTextFieldRole;
    if (!strcmp(role, "slider"))      return NSAccessibilitySliderRole;
    if (!strcmp(role, "progressbar")) return NSAccessibilityProgressIndicatorRole;
    if (!strcmp(role, "none"))        return NSAccessibilityUnknownRole;
    return nil;
}

// Our role name for an NSAccessibilityRole (readback of an override/auto role).
static const char* aeui_role_name_from_ns(NSAccessibilityRole r) {
    if (!r) return "";
    if ([r isEqualToString:NSAccessibilityButtonRole])            return "button";
    if ([r isEqualToString:NSAccessibilityCheckBoxRole])          return "checkbox";
    if ([r isEqualToString:NSAccessibilityRadioButtonRole])       return "radio";
    if ([r isEqualToString:NSAccessibilityLinkRole])              return "link";
    if ([r isEqualToString:NSAccessibilityStaticTextRole])        return "heading";
    if ([r isEqualToString:NSAccessibilityImageRole])             return "image";
    if ([r isEqualToString:NSAccessibilityGroupRole])             return "group";
    if ([r isEqualToString:NSAccessibilityListRole])              return "list";
    if ([r isEqualToString:NSAccessibilityRowRole])               return "listitem";
    if ([r isEqualToString:NSAccessibilityTabGroupRole])          return "tablist";
    if ([r isEqualToString:NSAccessibilityMenuRole])              return "menu";
    if ([r isEqualToString:NSAccessibilityMenuItemRole])          return "menuitem";
    if ([r isEqualToString:NSAccessibilityTextFieldRole])         return "textbox";
    if ([r isEqualToString:NSAccessibilitySliderRole])            return "slider";
    if ([r isEqualToString:NSAccessibilityProgressIndicatorRole]) return "progressbar";
    return "";
}

void aether_ui_a11y_set_role_impl(int handle, const char* role) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    NSAccessibilityRole r = aeui_ns_role(role);
    if (r) [v setAccessibilityRole:r];
}

void aether_ui_a11y_set_label_impl(int handle, const char* name) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setAccessibilityLabel:[NSString stringWithUTF8String:name ? name : ""]];
}

void aether_ui_a11y_set_description_impl(int handle, const char* desc) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setAccessibilityHelp:[NSString stringWithUTF8String:desc ? desc : ""]];
}

// Default (auto) accessible role for a widget kind — mirrors win32's
// w32_auto_role, so a control the author never explicitly tagged still reports
// the same role on all three backends. AppKit computes a view's DEFAULT role
// lazily and it reads back empty from the driver's (server-thread) query, so
// unlike an explicit setAccessibilityRole override we can't rely on the native
// value here — infer from the type instead.
static const char* aeui_auto_role_for_type(int t) {
    switch (t) {
        case AUI_BUTTON:      return "button";
        case AUI_TOGGLE:      return "checkbox";
        case AUI_TEXT:        return "heading";   // a static label
        case AUI_TEXTFIELD:   return "textbox";
        case AUI_SLIDER:      return "slider";
        case AUI_PROGRESSBAR: return "progressbar";
        default:              return "";
    }
}

void aether_ui_a11y_get_impl(int handle,
                             char* role, int rolesz,
                             char* name, int namesz,
                             char* desc, int descsz) {
    if (role && rolesz) role[0] = '\0';
    if (name && namesz) name[0] = '\0';
    if (desc && descsz) desc[0] = '\0';
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    if (role && rolesz) {
        // An explicit override reads back from the native role; when none was
        // set (native default is empty here) fall back to the type's auto-role.
        const char* rn = aeui_role_name_from_ns([v accessibilityRole]);
        if (!rn || !rn[0]) rn = aeui_auto_role_for_type(get_widget_type(handle));
        strncpy(role, rn, rolesz - 1); role[rolesz - 1] = '\0';
    }
    if (name && namesz) {
        NSString* lbl = [v accessibilityLabel];
        // Fall back to the view's accessible title (a button's label) when no
        // explicit label was set.
        if (!lbl || lbl.length == 0) {
            if ([v respondsToSelector:@selector(accessibilityTitle)])
                lbl = [v accessibilityTitle];
        }
        if (lbl) { strncpy(name, lbl.UTF8String, namesz - 1); name[namesz - 1] = '\0'; }
    }
    if (desc && descsz) {
        NSString* h = [v accessibilityHelp];
        if (h) { strncpy(desc, h.UTF8String, descsz - 1); desc[descsz - 1] = '\0'; }
    }
}

void aether_ui_set_distribution(int handle, int distribution) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSStackView class]]) {
        [(NSStackView*)v setDistribution:distribution];
    }
}

void aether_ui_set_alignment(int handle, int alignment) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSStackView class]]) {
        [(NSStackView*)v setAlignment:alignment];
    }
}

void aether_ui_match_parent_width(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setContentHuggingPriority:1
                  forOrientation:NSLayoutConstraintOrientationHorizontal];
}

void aether_ui_match_parent_height(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [v setContentHuggingPriority:1
                  forOrientation:NSLayoutConstraintOrientationVertical];
}

void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSStackView class]]) {
        [(NSStackView*)v setEdgeInsets:NSEdgeInsetsMake(top, left, bottom, right)];
    }
}

// ---------------------------------------------------------------------------
// Context-aware styling — cast _ctx (void*) to int handle and delegate.
// ---------------------------------------------------------------------------
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
// System integration
// ---------------------------------------------------------------------------

void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) return;  // runModal would block forever on CI
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:title ? title : ""]];
    [alert setInformativeText:[NSString stringWithUTF8String:message ? message : ""]];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

// One NSOpenPanel drives both the file and the folder picker; the only
// difference is which of canChooseFiles/canChooseDirectories is set.
static char* aeui_run_open_panel(const char* title, const char* start_dir,
                                 BOOL folders) {
    if (aeui_is_headless()) return strdup("");  // runModal would block forever
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    if (title) [panel setTitle:[NSString stringWithUTF8String:title]];
    [panel setCanChooseFiles:folders ? NO : YES];
    [panel setCanChooseDirectories:folders ? YES : NO];
    [panel setAllowsMultipleSelection:NO];
    if (start_dir && *start_dir) {
        NSString* dir = aeui_path_string(start_dir);
        // No start directory beats a crash: the panel simply opens wherever
        // the platform would have opened it.
        if (dir) {
            [panel setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
        }
    }
    if ([panel runModal] == NSModalResponseOK) {
        NSURL* url = [[panel URLs] firstObject];
        if (url) return strdup([[url path] UTF8String]);
    }
    return strdup("");
}

char* aether_ui_file_open(const char* title, const char* start_dir) {
    return aeui_run_open_panel(title, start_dir, NO);
}

char* aether_ui_file_pick_folder(const char* title, const char* start_dir) {
    return aeui_run_open_panel(title, start_dir, YES);
}

char* aether_ui_file_save(const char* title, const char* default_name) {
    if (aeui_is_headless()) return strdup("");
    NSSavePanel* panel = [NSSavePanel savePanel];
    if (title) [panel setTitle:[NSString stringWithUTF8String:title]];
    if (default_name && *default_name) {
        [panel setNameFieldStringValue:
            [NSString stringWithUTF8String:default_name]];
    }
    if ([panel runModal] == NSModalResponseOK) {
        NSURL* url = [panel URL];
        if (url) return strdup([[url path] UTF8String]);
    }
    return strdup("");
}

void aether_ui_clipboard_write_impl(const char* text) {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:text ? text : ""]
          forType:NSPasteboardTypeString];
}

char* aether_ui_clipboard_read_impl(void) {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    NSString* s = [pb stringForType:NSPasteboardTypeString];
    if (!s) return strdup("");
    const char* u = [s UTF8String];
    return strdup(u ? u : "");
}

// Timer — NSTimer scheduled on the main runloop. Fires closure on every tick.
@interface AetherTimerTarget : NSObject
@property (assign) AeClosure* closure;
@property (strong) NSTimer* timer;
- (void)tick:(NSTimer*)t;
@end

@implementation AetherTimerTarget
- (void)tick:(NSTimer*)t {
    (void)t;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*))self.closure->fn)(self.closure->env);
    }
}
@end

static NSMutableArray<AetherTimerTarget*>* active_timers = nil;

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (!boxed_closure || interval_ms <= 0) return 0;
    if (!active_timers) active_timers = [NSMutableArray array];
    AetherTimerTarget* t = [[AetherTimerTarget alloc] init];
    t.closure = (AeClosure*)boxed_closure;
    NSTimeInterval interval = (NSTimeInterval)interval_ms / 1000.0;
    t.timer = [NSTimer scheduledTimerWithTimeInterval:interval
                                               target:t
                                             selector:@selector(tick:)
                                             userInfo:nil
                                              repeats:YES];
    [active_timers addObject:t];
    return (int)[active_timers count];  // 1-based id
}

void aether_ui_timer_cancel_impl(int timer_id) {
    if (!active_timers) return;
    if (timer_id < 1 || timer_id > (int)[active_timers count]) return;
    AetherTimerTarget* t = active_timers[timer_id - 1];
    [t.timer invalidate];
    t.timer = nil;
}

void aether_ui_open_url_impl(const char* url) {
    if (!url) return;
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:url]]];
}

int aether_ui_dark_mode_check(void) {
    // Driver override first (POST /appearance?dark=N — headless spec steer).
    int ov = aether_ui_appearance_override_get();
    if (ov >= 0) return ov;
    if (@available(macOS 10.14, *)) {
        NSAppearance* a = [NSApp effectiveAppearance];
        NSAppearanceName name = [a bestMatchFromAppearancesWithNames:@[
            NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        return [name isEqualToString:NSAppearanceNameDarkAqua] ? 1 : 0;
    }
    return 0;
}

int aether_ui_styled_opacity_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return -1;
    NSNumber* n = objc_getAssociatedObject(v, "aeui_styled_opacity");
    if (!n || [n intValue] <= 0) return -1;
    return [n intValue] - 1;
}

// AeCS appearance change (macOS). OS path: the distributed
// AppleInterfaceThemeChangedNotification; driver path: fire marshals to the
// main thread (the callback re-themes AppKit views).
void aether_ui_watch_appearance_impl(void) {
    static int watched = 0;
    if (watched) return;
    watched = 1;
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                    (void)note;
                    aether_ui_appearance_invoke(aether_ui_dark_mode_check());
                }];
}

// Undo/redo driver fire — marshalled to the main thread (edit closures
// mutate AppKit views).
static int aeui_fire_undo_redo(int redo) {
    __block int did = 0;
    void (^fire)(void) = ^{
        did = redo ? aether_ui_redo_step_impl() : aether_ui_undo_step_impl();
    };
    if ([NSThread isMainThread]) fire();
    else dispatch_sync(dispatch_get_main_queue(), fire);
    return did;
}
int aether_ui_fire_undo(void) { return aeui_fire_undo_redo(0); }
int aether_ui_fire_redo(void) { return aeui_fire_undo_redo(1); }

int aether_ui_fire_appearance(int dark) {
    aether_ui_appearance_override_set(dark ? 1 : 0);
    void (^fire)(void) = ^{ aether_ui_appearance_invoke(dark ? 1 : 0); };
    if ([NSThread isMainThread]) fire();
    else dispatch_sync(dispatch_get_main_queue(), fire);
    return 1;
}

// ---------------------------------------------------------------------------
// Multi-window support — NSWindow per handle.
// ---------------------------------------------------------------------------
static NSMutableArray<NSWindow*>* extra_windows = nil;

// Wrap a window's root in an AetherOverlayHost (as the primary gets in
// applicationDidFinishLaunching), so EACH window has its own overlay layer and
// a secondary window's overlays/sheets/toasts parent to IT, not the primary.
static AetherOverlayHost* aeui_interpose_overlay_host(NSWindow* win, NSView* root) {
    if (!win || !root) return nil;
    AetherOverlayHost* host = [[AetherOverlayHost alloc]
        initWithFrame:[[win contentView] bounds]];
    [host setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [host setWantsLayer:YES];
    [host addSubview:root];
    [root setTranslatesAutoresizingMaskIntoConstraints:NO];
    [root.leadingAnchor  constraintEqualToAnchor:host.leadingAnchor].active  = YES;
    [root.trailingAnchor constraintEqualToAnchor:host.trailingAnchor].active = YES;
    [root.topAnchor      constraintEqualToAnchor:host.topAnchor].active      = YES;
    [root.bottomAnchor   constraintEqualToAnchor:host.bottomAnchor].active   = YES;
    [win setContentView:host];
    [host layoutSubtreeIfNeeded];
    return host;
}

// Resolve an overlay_open/toast win_handle to its host. The app-level handle is
// 1-based into extra_windows (window_create's return); 0 (or anything unknown)
// means the primary window's host.
static AetherOverlayHost* overlay_host_for(int win_handle) {
    if (win_handle >= 1 && extra_windows
        && win_handle <= (int)[extra_windows count]) {
        NSView* cv = [extra_windows[win_handle - 1] contentView];
        if ([cv isKindOfClass:[AetherOverlayHost class]])
            return (AetherOverlayHost*)cv;
    }
    return overlay_host;
}

int aether_ui_window_create_impl(const char* title, int width, int height) {
    if (!extra_windows) extra_windows = [NSMutableArray array];
    NSRect frame = NSMakeRect(250, 250, width, height);
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                               NSWindowStyleMaskClosable |
                               NSWindowStyleMaskResizable;
    NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    // Keep the object alive after -close: extra_windows holds the only strong
    // ref, and the default releasedWhenClosed:YES would over-release it (→ a
    // dangling pointer the /windows route then reads). We manage the lifetime.
    [win setReleasedWhenClosed:NO];
    [win setTitle:[NSString stringWithUTF8String:title ? title : ""]];
    [extra_windows addObject:win];
    return (int)[extra_windows count];
}

void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    if (!extra_windows || win_handle < 1 || win_handle > (int)[extra_windows count]) return;
    NSWindow* win = extra_windows[win_handle - 1];
    NSView* root = (__bridge NSView*)aether_ui_get_widget(root_handle);
    if (root) aeui_interpose_overlay_host(win, root);
}

void aether_ui_window_show_impl(int win_handle) {
    if (!extra_windows || win_handle < 1 || win_handle > (int)[extra_windows count]) return;
    // Headless: the window exists and its widgets are reachable, but it is never
    // ordered onto the desktop (matches the primary's headless handling and the
    // GTK4/win32 no-op). Ordering a secondary window key here would also make
    // closing it read as the last visible window closing.
    const char* h = getenv("AETHER_UI_HEADLESS");
    if (h && h[0] && h[0] != '0') return;
    [extra_windows[win_handle - 1] makeKeyAndOrderFront:nil];
}

// -[NSWindow close] must run on the main thread; the driver's /window/{id}/close
// route calls in on the HTTP server thread.
static void aeui_close_window_main(NSWindow* w) {
    if (!w) return;
    void (^b)(void) = ^{ [w close]; };
    if ([NSThread isMainThread]) b();
    else dispatch_sync(dispatch_get_main_queue(), b);
}

void aether_ui_window_close_impl(int win_handle) {
    if (!extra_windows || win_handle < 1 || win_handle > (int)[extra_windows count]) return;
    aeui_close_window_main(extra_windows[win_handle - 1]);
}

// ── Unified driver window view (1 = primary, 2.. = extras) ──
// (NSApplicationShouldTerminateAfterLastWindowClosed gives the last-close rule
// natively; these expose the registry to the driver.)
static NSWindow* mac_window_for_handle(int win_handle) {
    if (win_handle == 1) return primary_window;
    int idx = win_handle - 2;
    if (!extra_windows || idx < 0 || idx >= (int)[extra_windows count]) return nil;
    return extra_windows[idx];
}
int aether_ui_window_count_impl(void) {
    return 1 + (extra_windows ? (int)[extra_windows count] : 0);
}
const char* aether_ui_window_title_impl(int win_handle) {
    NSWindow* w = mac_window_for_handle(win_handle);
    if (!w) return "";
    const char* t = [[w title] UTF8String];
    return t ? t : "";
}

void aether_ui_window_set_title_impl(int win_handle, const char* title) {
    NSWindow* w = mac_window_for_handle(win_handle);
    if (!w) return;
    [w setTitle:[NSString stringWithUTF8String:title ? title : ""]];
}
int aether_ui_window_is_open_impl(int win_handle) {
    NSWindow* w = mac_window_for_handle(win_handle);
    // A closed NSWindow is released/hidden; visible OR the primary counts live.
    return (w && ([w isVisible] || w == primary_window)) ? 1 : 0;
}
int aether_ui_widget_window_impl(int widget_handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(widget_handle);
    if (!v) return 0;
    NSWindow* win = [v window];
    if (!win) return 0;
    if (win == primary_window) return 1;
    if (extra_windows) {
        NSUInteger i = [extra_windows indexOfObject:win];
        if (i != NSNotFound) return (int)i + 2;
    }
    return 0;
}
void aether_ui_close_window_by_handle_impl(int win_handle) {
    aeui_close_window_main(mac_window_for_handle(win_handle));
}

// ---------------------------------------------------------------------------
// In-window overlay layer (roadmap item 1) — toast / modal / tooltip /
// dropdown, drawn INSIDE the window, never as a compositor popup.
//
// The overlay table is append-only and never compacted: handles are 1-based
// and monotonic, and closing an overlay flips `live` to 0 without removing
// the entry. GET /overlays therefore lists every overlay ever opened. Specs
// depend on that — a toast has to be observably DEAD, not merely absent, or
// "it expired" and "it never opened" would look identical.
// ---------------------------------------------------------------------------
typedef struct {
    NSView* __unsafe_unretained content;  // owned by the view tree
    NSView* __unsafe_unretained scrim;    // modal scrim; nil when non-modal
    // The two positioning constraints, kept so an overlay can be MOVED without
    // being torn down and rebuilt. The drawn tooltip follows the pointer this
    // way (one overlay reused for the life of the process, as on GTK).
    NSLayoutConstraint* __unsafe_unretained cx;
    NSLayoutConstraint* __unsafe_unretained cy;
    AeClosure* on_dismiss;
    int modal;
    int live;
    int exiting;    // 1 while the exit tween plays (before live flips to 0)
    int exit_played;   // sticky: an exit tween STARTED (survives the entry's death)
    int trans_ms;   // per-entry transition duration; 0 = no tween (instant)
    char* trans_kind;  // "fade"/"slide-up"/"slide-down"/"scale"; NULL = fade
    // Scrim material (overlay_material). macOS has a REAL frosted material —
    // "blur" installs an NSVisualEffectView behind the scrim. effective is
    // "dim" | "blur" | "tint". material_view is the effect view (nil = none).
    char* material;
    NSVisualEffectView* __unsafe_unretained material_view;
} OverlayEntry;

static OverlayEntry* overlays = NULL;
static int overlay_count = 0;
static int overlay_capacity = 0;

static OverlayEntry* overlay_at(int handle) {
    if (handle < 1 || handle > overlay_count) return NULL;
    return &overlays[handle - 1];
}

// A full-bounds view that swallows every click. Input blocking is purely
// hit-test z-order — there is no keyboard grab and no focus trap, which is
// exactly GTK's behaviour (Tab still walks the app beneath a modal).
@interface AetherScrimView : NSView
@property (assign) int overlayHandle;
@end

@implementation AetherScrimView
- (NSView*)hitTest:(NSPoint)p {
    // Claim the point unconditionally when it's inside us — the whole job.
    return NSPointInRect([self convertPoint:p fromView:[self superview]], [self bounds])
        ? self : nil;
}
- (void)mouseDown:(NSEvent*)ev {
    (void)ev;
    // A scrim click is the ONLY path that fires on_dismiss. Closing an
    // overlay programmatically does not (GTK behaves the same way).
    OverlayEntry* e = overlay_at(self.overlayHandle);
    if (!e || !e->live) return;
    if (e->on_dismiss && e->on_dismiss->fn) {
        ((void(*)(void*))e->on_dismiss->fn)(e->on_dismiss->env);
    }
    aether_ui_overlay_close_impl(self.overlayHandle);
}
@end

int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal) {
    // Each window carries its own AetherOverlayHost (the primary's from
    // applicationDidFinishLaunching, extras' from window_set_body), so a
    // secondary window's overlays parent to IT — win32/GTK4 do the same.
    NSView* content = (__bridge NSView*)aether_ui_get_widget(content_handle);
    AetherOverlayHost* host = overlay_host_for(win_handle);
    if (!content || !host) return 0;

    if (overlay_count >= overlay_capacity) {
        overlay_capacity = overlay_capacity == 0 ? 8 : overlay_capacity * 2;
        overlays = (OverlayEntry*)realloc(overlays,
                                          sizeof(OverlayEntry) * overlay_capacity);
        if (!overlays) { overlay_count = 0; overlay_capacity = 0; return 0; }
    }
    int handle = overlay_count + 1;
    OverlayEntry* e = &overlays[overlay_count];
    memset(e, 0, sizeof(*e));
    e->modal = modal ? 1 : 0;
    e->live = 1;

    // Scrim first so it sits BELOW the content in z-order.
    if (modal) {
        AetherScrimView* scrim = [[AetherScrimView alloc] initWithFrame:[host bounds]];
        scrim.overlayHandle = handle;
        [scrim setWantsLayer:YES];
        scrim.layer.backgroundColor =
            [[NSColor colorWithRed:0 green:0 blue:0 alpha:0.45] CGColor];
        [scrim setTranslatesAutoresizingMaskIntoConstraints:NO];
        [host addSubview:scrim positioned:NSWindowAbove relativeTo:nil];
        [scrim.leadingAnchor constraintEqualToAnchor:host.leadingAnchor].active = YES;
        [scrim.trailingAnchor constraintEqualToAnchor:host.trailingAnchor].active = YES;
        [scrim.topAnchor constraintEqualToAnchor:host.topAnchor].active = YES;
        [scrim.bottomAnchor constraintEqualToAnchor:host.bottomAnchor].active = YES;
        register_widget_typed((__bridge void*)scrim, AUI_SCRIM);
        e->scrim = scrim;
    }

    [content setTranslatesAutoresizingMaskIntoConstraints:NO];
    [host addSubview:content positioned:NSWindowAbove relativeTo:nil];

    // anchor packs halign in bits 0-1 (0=start 1=center 2=end) and valign in
    // bits 2-3 (0=top 1=center 2=bottom): code = h + v*4.
    int h = anchor & 3, v = (anchor >> 2) & 3;
    // dx/dy are SIGNED: positive insets from start/top, negative from end/bottom.
    NSLayoutConstraint* cx;
    NSLayoutConstraint* cy;
    if (h == 0) {
        cx = [content.leadingAnchor constraintEqualToAnchor:host.leadingAnchor
                                                   constant:(dx > 0 ? dx : 0)];
    } else if (h == 2) {
        cx = [content.trailingAnchor constraintEqualToAnchor:host.trailingAnchor
                                                    constant:(dx < 0 ? dx : 0)];
    } else {
        cx = [content.centerXAnchor constraintEqualToAnchor:host.centerXAnchor
                                                   constant:dx];
    }
    if (v == 0) {
        cy = [content.topAnchor constraintEqualToAnchor:host.topAnchor
                                               constant:(dy > 0 ? dy : 0)];
    } else if (v == 2) {
        cy = [content.bottomAnchor constraintEqualToAnchor:host.bottomAnchor
                                                  constant:(dy < 0 ? dy : 0)];
    } else {
        cy = [content.centerYAnchor constraintEqualToAnchor:host.centerYAnchor
                                                   constant:dy];
    }
    cx.active = YES;
    cy.active = YES;

    e->cx = cx;
    e->cy = cy;
    e->content = content;
    overlay_count++;
    [host layoutSubtreeIfNeeded];
    return handle;
}

// True when the app was launched with AETHER_UI_NO_ANIMATION — the CI/matrix
// default. Transitions then resolve to their end-state instantly (no tween),
// which is the deterministic contract specs assert against.
static int aeui_animations_off(void) {
    const char* v = getenv("AETHER_UI_NO_ANIMATION");
    return v && v[0] && v[0] != '0';
}

// Tear an overlay entry's views out of the tree and mark it dead. Shared by the
// instant path and the animated path's completion.
static void overlay_finalize_close(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    if (!e) return;
    e->live = 0;
    e->exiting = 0;
    if (e->scrim)   [e->scrim removeFromSuperview];
    if (e->content) [e->content removeFromSuperview];
    e->scrim = nil;
    e->content = nil;
    if (overlay_host) [overlay_host layoutSubtreeIfNeeded];
}

void aether_ui_overlay_close_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    if (!e || !e->live) return;   // idempotent
    // Animated exit: play the fade-out tween, THEN remove. is_exiting reads 1
    // for the duration so a spec can observe the exit before the entry dies
    // (Phase 5h2). AETHER_UI_NO_ANIMATION and untransitioned entries skip it.
    if (e->trans_ms > 0 && e->content && !aeui_animations_off()) {
        e->exiting = 1;
        e->exit_played = 1;
        int dur_ms = e->trans_ms;
        int cap = overlay_handle;
        NSView* content = e->content;
        NSView* scrim = e->scrim;
        /* Alpha always; GEOMETRY as well for the kinds that ask for it.
           Core Animation tweens the frame directly, so slide is a frame
           offset and scale an inset about the centre. An unrecognised kind
           (or none) is a plain fade, exactly as before. */
        const char* kind = e->trans_kind;
        NSRect start = [content frame];
        NSRect target = start;
        if (kind) {
            if (strcmp(kind, "slide-up") == 0)        target.origin.y += start.size.height;
            else if (strcmp(kind, "slide-down") == 0) target.origin.y -= start.size.height;
            else if (strcmp(kind, "scale") == 0) {
                target = NSInsetRect(start, start.size.width * 0.5 - 1.0,
                                            start.size.height * 0.5 - 1.0);
            }
        }
        BOOL moves = !NSEqualRects(start, target);
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
            ctx.duration = (double)dur_ms / 1000.0;
            [[content animator] setAlphaValue:0.0];
            if (moves) [[content animator] setFrame:target];
            if (scrim) [[scrim animator] setAlphaValue:0.0];
        } completionHandler:^{
            overlay_finalize_close(cap);
        }];
        return;
    }
    overlay_finalize_close(overlay_handle);
}

void aether_ui_overlay_set_on_dismiss_impl(int overlay_handle, void* boxed_closure) {
    OverlayEntry* e = overlay_at(overlay_handle);
    if (e) e->on_dismiss = (AeClosure*)boxed_closure;
}

int aether_ui_overlay_is_live_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    return e ? e->live : 0;
}

int aether_ui_overlay_count_impl(void) { return overlay_count; }

int aether_ui_overlay_is_modal_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    return e ? e->modal : 0;
}

// Per-entry overlay transitions. AppKit tweens the content's opacity via the
// NSView animator on close (see overlay_close_impl); the `kind` (fade/slide-*)
// currently all resolve to a fade — a directional slide is a refinement. When
// animations are off the tween is skipped and exit is instant.
void aether_ui_overlay_set_transition_impl(int overlay_handle,
                                           const char* kind, int ms) {
    OverlayEntry* e = overlay_at(overlay_handle);
    if (!e) return;
    /* The KIND was discarded here -- (void)kind -- so every transition played
       as a fade regardless of what was asked for. GTK4 has always
       distinguished fade/slide-up/slide-down/scale; win32 stored the string
       but branched only on non-emptiness. One string, three interpretations.
       See TODO.md, "push rendering semantics out of the backends". */
    free(e->trans_kind);
    e->trans_kind = (kind && *kind) ? strdup(kind) : NULL;
    e->trans_ms = ms > 0 ? ms : 0;
}

int aether_ui_overlay_is_exiting_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    return e ? e->exiting : 0;
}

int aether_ui_overlay_exit_played_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    return e ? e->exit_played : 0;
}

// Scrim material. macOS has a REAL frosted material: "blur" installs an
// NSVisualEffectView (behindWindow blending) below the click-eating scrim and
// dials the scrim's own dim right down so the frost shows through. "tint"/"dim"
// keep the plain scrim. Records the EFFECTIVE material.
void aether_ui_overlay_set_material_impl(int overlay_handle, const char* kind) {
    OverlayEntry* e = overlay_at(overlay_handle);
    if (!e) return;
    const char* eff = "dim";
    if (kind && strcmp(kind, "blur") == 0)      eff = "blur";
    else if (kind && strcmp(kind, "tint") == 0) eff = "tint";
    free(e->material);
    e->material = strdup(eff);

    NSView* scrim = e->scrim;
    if (!scrim) return;
    void (^apply)(void) = ^{
        // Remove any prior effect view.
        if (e->material_view) { [e->material_view removeFromSuperview]; e->material_view = nil; }
        if (strcmp(eff, "blur") == 0) {
            NSView* host = [scrim superview];
            NSVisualEffectView* fx = [[NSVisualEffectView alloc] initWithFrame:[host bounds]];
            fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
            fx.material = NSVisualEffectMaterialHUDWindow;
            fx.state = NSVisualEffectStateActive;
            [fx setTranslatesAutoresizingMaskIntoConstraints:NO];
            // Insert BELOW the scrim (which keeps eating clicks on top).
            [host addSubview:fx positioned:NSWindowBelow relativeTo:scrim];
            [fx.leadingAnchor constraintEqualToAnchor:host.leadingAnchor].active = YES;
            [fx.trailingAnchor constraintEqualToAnchor:host.trailingAnchor].active = YES;
            [fx.topAnchor constraintEqualToAnchor:host.topAnchor].active = YES;
            [fx.bottomAnchor constraintEqualToAnchor:host.bottomAnchor].active = YES;
            e->material_view = fx;
            // Let the frost dominate: drop the scrim's own dim to a faint veil.
            scrim.layer.backgroundColor =
                [[NSColor colorWithRed:0 green:0 blue:0 alpha:0.08] CGColor];
        } else if (strcmp(eff, "tint") == 0) {
            scrim.layer.backgroundColor =
                [[NSColor colorWithRed:0.95 green:0.95 blue:0.98 alpha:0.35] CGColor];
        } else {
            scrim.layer.backgroundColor =
                [[NSColor colorWithRed:0 green:0 blue:0 alpha:0.45] CGColor];
        }
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_sync(dispatch_get_main_queue(), apply);
}

const char* aether_ui_overlay_material_effective_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    return (e && e->material) ? e->material : "dim";
}

// Escape closes the TOPMOST live overlay. Returns 1 if one was closed, so the
// caller can let Escape propagate when nothing is open.
static int aeui_escape_overlays(void) {
    for (int i = overlay_count - 1; i >= 0; i--) {
        if (overlays[i].live) {
            aether_ui_overlay_close_impl(i + 1);
            return 1;
        }
    }
    return 0;
}

int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    if (!overlay_host) return 0;
    NSTextField* lbl = [NSTextField labelWithString:
        [NSString stringWithUTF8String:text ? text : ""]];
    [lbl setEditable:NO];
    [lbl setBordered:NO];
    [lbl setSelectable:NO];
    [lbl setTextColor:[NSColor whiteColor]];
    [lbl setWantsLayer:YES];
    lbl.layer.backgroundColor =
        [[NSColor colorWithRed:0.15 green:0.15 blue:0.17 alpha:0.95] CGColor];
    lbl.layer.cornerRadius = 8.0;
    [lbl setAlignment:NSTextAlignmentCenter];

    // A real registered widget, so the toast shows up in /widgets like GTK's.
    int content = register_widget_typed((__bridge void*)lbl, AUI_TEXT);
    // anchor 9 = bottom-center (h=1, v=2), non-modal, lifted 24px off the edge.
    int handle = aether_ui_overlay_open_impl(win_handle, content, 9, 0, -24, 0);
    if (handle > 0 && ms > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            aether_ui_overlay_close_impl(handle);  // flips live, fires no dismiss
        });
    }
    return handle;
}
// ---------------------------------------------------------------------------
// apply_css on AppKit.
//
// There is no stylesheet engine here, and most of what GTK expresses in CSS
// the Mac expresses through typed setters (set_bg_color, set_font_*). So this
// stays a no-op for styling — with ONE exception.
//
// `ui.transition(h, "opacity", 1200, "ease_out")` is carried across the ABI as
// the CSS declaration "transition: opacity 1200ms ease-out;" — GTK's stylesheet
// IS the transition engine, so no dedicated ABI call was ever needed. Dropping
// it on the floor here means every declared transition silently snaps on macOS.
// We therefore parse just this one declaration (a string we emit ourselves) and
// record it; set_opacity then tweens instead of snapping.
// ---------------------------------------------------------------------------
typedef struct {
    int  ms;         // 0 = no transition declared
    int  ease_out;   // 1 = ease-out, 0 = linear (ignored when spring)
    int  spring;     // 1 = damped spring: OVERSHOOTS the target, then settles
} OpacityTransition;

static OpacityTransition* widget_transitions = NULL;   // indexed by handle-1
static int widget_transition_cap = 0;

static void aeui_transitions_reserve(int handle) {
    if (handle <= widget_transition_cap) return;
    int cap = widget_transition_cap == 0 ? 64 : widget_transition_cap;
    while (cap < handle) cap *= 2;
    widget_transitions = (OpacityTransition*)realloc(
        widget_transitions, sizeof(OpacityTransition) * cap);
    if (!widget_transitions) { widget_transition_cap = 0; return; }
    memset(widget_transitions + widget_transition_cap, 0,
           sizeof(OpacityTransition) * (cap - widget_transition_cap));
    widget_transition_cap = cap;
}

static int aeui_opacity_transition_ms(int handle) {
    if (handle < 1 || handle > widget_transition_cap) return 0;
    return widget_transitions[handle - 1].ms;
}

static int aeui_opacity_transition_ease_out(int handle) {
    if (handle < 1 || handle > widget_transition_cap) return 1;
    return widget_transitions[handle - 1].ease_out;
}

static int aeui_opacity_transition_spring(int handle) {
    if (handle < 1 || handle > widget_transition_cap) return 0;
    return widget_transitions[handle - 1].spring;
}

/* Damped-spring progress -- the QML-SpringAnimation-alike shape as a closed form,
   matching w32_spring_progress in the win32 backend exactly:

       p(t) = 1 - e^(-6t) * cos(9t)

   Exponential damping envelope times an oscillation, so p rises PAST 1
   (peaking ~15% over at t=0.30) and rings back -- the overshoot no CSS
   cubic-bezier or ease-out can express. Both endpoints are pinned so a tween
   still lands exactly on its target. */
static double aeui_spring_progress(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return 1.0 - exp(-6.0 * t) * cos(9.0 * t);
}

// The tween is driven BY HAND, on a timer, rather than through
// -[NSView animator].
//
// The animator proxy sets the model alpha to its target immediately and lets
// CoreAnimation interpolate the presentation layer. Nothing that reads the view
// — including cacheDisplayInRect:, i.e. /screenshot — can then observe an
// intermediate value: the fade is real on screen but every mid-flight capture
// already shows the settled frame, so a driver can't tell a 1200ms tween from
// an instant snap. Stepping alphaValue ourselves makes the intermediate states
// real, which is what the spec is entitled to assert.
//
// The timer exists only while a tween is in flight — idle still costs zero.
@interface AetherTween : NSObject
@property (weak)   NSView*  view;
@property (assign) double   from;
@property (assign) double   to;
@property (assign) double   duration;   // seconds
@property (assign) int      easeOut;
@property (assign) int      spring;
@property (assign) CFTimeInterval start;
@property (strong) NSTimer* timer;
@end

@implementation AetherTween
- (void)step:(NSTimer*)t {
    NSView* v = self.view;
    if (!v) { [t invalidate]; self.timer = nil; return; }
    double elapsed = CACurrentMediaTime() - self.start;
    double p = (self.duration > 0) ? (elapsed / self.duration) : 1.0;
    if (p >= 1.0) {
        [v setAlphaValue:self.to];
        [t invalidate];
        self.timer = nil;
        return;
    }
    double e;
    if (self.spring) e = aeui_spring_progress(p);
    else e = self.easeOut ? (1.0 - (1.0 - p) * (1.0 - p)) : p;   // quad ease-out
    [v setAlphaValue:self.from + (self.to - self.from) * e];
}
@end

// One live tween per widget; a new set_opacity supersedes the one in flight.
static NSMutableDictionary<NSNumber*, AetherTween*>* aeui_tweens = nil;

static void aeui_start_opacity_tween(NSView* v, int handle,
                                     double from, double to, int ms, int ease_out,
                                     int spring) {
    if (!aeui_tweens) aeui_tweens = [NSMutableDictionary dictionary];
    NSNumber* key = @(handle);
    AetherTween* old = aeui_tweens[key];
    if (old) { [old.timer invalidate]; old.timer = nil; }

    if (ms <= 0 || from == to) { [v setAlphaValue:to]; return; }

    AetherTween* tw = [[AetherTween alloc] init];
    tw.view = v;
    tw.from = from;
    tw.to = to;
    tw.duration = (double)ms / 1000.0;
    tw.easeOut = ease_out;
    tw.spring = spring;
    tw.start = CACurrentMediaTime();
    tw.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                                target:tw
                                              selector:@selector(step:)
                                              userInfo:nil
                                               repeats:YES];
    // The driver's screenshot runs a dispatch_sync onto the main queue; a
    // default-mode-only timer would stall while modal/tracking loops run.
    [[NSRunLoop mainRunLoop] addTimer:tw.timer forMode:NSRunLoopCommonModes];
    aeui_tweens[key] = tw;
}

void aether_ui_widget_apply_css_impl(int handle, const char* property_css) {
    if (!property_css || handle < 1) return;

    const char* t = strstr(property_css, "transition:");
    if (t) {
        // "transition: opacity 1200ms ease-out;" — the declaration. Record it;
        // the next opacity change tweens instead of snapping.
        if (!strstr(t, "opacity")) return;            // only opacity is wired
        const char* ms_at = strstr(t, "ms");
        if (!ms_at) return;
        const char* p = ms_at;                        // walk back over the digits
        while (p > t && isdigit((unsigned char)p[-1])) p--;
        int ms = atoi(p);
        if (ms <= 0) return;
        aeui_transitions_reserve(handle);
        if (handle > widget_transition_cap) return;
        widget_transitions[handle - 1].ms = ms;
        /* "spring" arrives as a marker comment on the bezier (this backend
           does not parse control points); it overrides the ease flag. */
        widget_transitions[handle - 1].spring   = strstr(t, "spring") ? 1 : 0;
        widget_transitions[handle - 1].ease_out = strstr(t, "linear") ? 0 : 1;
        return;
    }

    // "opacity: 0.15;" — ui.style_opacity() goes through CSS too, not through
    // the set_opacity ABI call. Miss this and the declared transition above has
    // nothing to act on, and every fade snaps.
    const char* o = strstr(property_css, "opacity:");
    if (o) {
        double ov = atof(o + 8);
        // Stash for driver readback (AeCS st_opacity proof; +1 encoding).
        NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
        if (v) objc_setAssociatedObject(v, "aeui_styled_opacity",
                   @((int)(ov * 100.0) + 1), OBJC_ASSOCIATION_RETAIN);
        aether_ui_set_opacity(handle, ov);
        return;
    }
    // Any other CSS (box-shadow, colours…): no-op. AppKit styles through the
    // typed setters instead.
}

// Does `cls` appear as a whole space-delimited token in `list`?
static int class_list_has(const char* list, const char* cls) {
    if (!list || !cls || !*cls) return 0;
    size_t n = strlen(cls);
    for (const char* p = list; (p = strstr(p, cls)) != NULL; p += n) {
        int left_ok  = (p == list) || (p[-1] == ' ');
        int right_ok = (p[n] == '\0') || (p[n] == ' ');
        if (left_ok && right_ok) return 1;
    }
    return 0;
}

void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    if (handle < 1 || handle > widget_count || !cls || !*cls) return;
    char* cur = widget_classes[handle - 1];
    if (class_list_has(cur, cls)) return;  // idempotent, like GTK's
    if (!cur) {
        widget_classes[handle - 1] = strdup(cls);
        return;
    }
    char* joined = (char*)malloc(strlen(cur) + strlen(cls) + 2);
    if (!joined) return;
    sprintf(joined, "%s %s", cur, cls);
    free(cur);
    widget_classes[handle - 1] = joined;
}

void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    if (handle < 1 || handle > widget_count || !cls || !*cls) return;
    char* cur = widget_classes[handle - 1];
    if (!class_list_has(cur, cls)) return;
    // Rebuild the list, dropping every occurrence of the token.
    char* out = (char*)malloc(strlen(cur) + 1);
    if (!out) return;
    out[0] = '\0';
    char* work = strdup(cur);
    if (!work) { free(out); return; }
    for (char* tok = strtok(work, " "); tok; tok = strtok(NULL, " ")) {
        if (strcmp(tok, cls) == 0) continue;
        if (out[0]) strcat(out, " ");
        strcat(out, tok);
    }
    free(work);
    free(cur);
    widget_classes[handle - 1] = out;  // "" when the last class was removed
}
void aether_ui_canvas_group_begin_impl(int canvas_id) { (void)canvas_id; }
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    (void)canvas_id; (void)alpha;
}
// ---------------------------------------------------------------------------
// The drawn tooltip — a vg-drawn shape's tooltip, rendered as an overlay
// rather than a native NSToolTip (which can't be positioned per-shape and
// wouldn't be visible to the driver).
//
// ONE overlay is reused for the life of the process: hovering from shape to
// shape moves it rather than churning the overlay table, which would otherwise
// grow an entry per pointer motion.
// ---------------------------------------------------------------------------
static int drawn_tooltip_overlay = 0;
static NSTextField* drawn_tooltip_label = nil;

int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text,
                                   double cx, double cy) {
    if (!text || !text[0]) { aether_ui_vg_tooltip_hide_impl(); return 0; }
    NSView* canvas = (__bridge NSView*)aether_ui_get_widget(
        aether_ui_canvas_get_widget(canvas_id));
    if (!canvas || !overlay_host) return 0;

    // Canvas-local → host-local. The canvas is isFlipped and the host is not,
    // so convertPoint: does the y-flip for us; the anchor is top-start, whose
    // dy is measured DOWN from the top, hence the subtraction.
    NSPoint p = [canvas convertPoint:NSMakePoint(cx, cy) toView:overlay_host];
    int mx = (int)lround(p.x) + 12;      // sit below-right of the pointer,
    int my = (int)lround([overlay_host bounds].size.height - p.y) + 18;  // like a native tip

    NSString* s = [NSString stringWithUTF8String:text];

    if (drawn_tooltip_overlay
        && aether_ui_overlay_is_live_impl(drawn_tooltip_overlay)
        && drawn_tooltip_label) {
        OverlayEntry* e = overlay_at(drawn_tooltip_overlay);
        [drawn_tooltip_label setStringValue:s];
        if (e && e->cx && e->cy) {
            e->cx.constant = mx;
            e->cy.constant = my;
        }
        [overlay_host layoutSubtreeIfNeeded];
        return drawn_tooltip_overlay;
    }

    NSTextField* lbl = [NSTextField labelWithString:s];
    [lbl setEditable:NO];
    [lbl setBordered:NO];
    [lbl setSelectable:NO];
    [lbl setTextColor:[NSColor whiteColor]];
    [lbl setWantsLayer:YES];
    lbl.layer.backgroundColor =
        [[NSColor colorWithRed:0.1 green:0.1 blue:0.12 alpha:0.95] CGColor];
    lbl.layer.cornerRadius = 4.0;

    int content = register_widget_typed((__bridge void*)lbl, AUI_TEXT);
    // anchor 0 = top-start; the offsets ARE the position.
    drawn_tooltip_overlay = aether_ui_overlay_open_impl(0, content, 0, mx, my, 0);
    drawn_tooltip_label = lbl;
    return drawn_tooltip_overlay;
}

void aether_ui_vg_tooltip_hide_impl(void) {
    if (drawn_tooltip_overlay) {
        aether_ui_overlay_close_impl(drawn_tooltip_overlay);
        drawn_tooltip_overlay = 0;   // the table keeps the entry as live:0
        drawn_tooltip_label = nil;
    }
}

// Whether the drawn-tooltip PATH is active — a policy question, not a state
// one. vg/live.ae calls this once at setup to decide whether to wire hover at
// all, so it must NOT mean "is a tooltip currently showing" (it is always
// false at setup, and hover would never be wired).
//
// Forced by $AETHER_UI_TOOLTIP, else on under sommelier, where native tooltips
// don't display. Same rule as GTK4.
int aether_ui_vg_tooltip_drawn_impl(void) {
    const char* force = getenv("AETHER_UI_TOOLTIP");
    if (force && strcmp(force, "drawn") == 0) return 1;
    if (force && strcmp(force, "native") == 0) return 0;
    return getenv("SOMMELIER_VERSION") != NULL;
}

// Sheet — modal NSWindow attached to primary window via beginSheet:.
static NSMutableArray<NSWindow*>* sheet_windows = nil;

int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    if (!sheet_windows) sheet_windows = [NSMutableArray array];
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindow* sheet = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:NSWindowStyleMaskTitled |
                                                            NSWindowStyleMaskClosable
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
    [sheet setTitle:[NSString stringWithUTF8String:title ? title : ""]];
    [sheet_windows addObject:sheet];
    int idx = (int)[sheet_windows count];
    // Register under the widget registry too, so callers can reference by handle.
    return register_widget_typed((__bridge void*)[sheet contentView], AUI_SHEET) * 0 + idx;
}

void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    if (!sheet_windows || handle < 1 || handle > (int)[sheet_windows count]) return;
    NSWindow* sheet = sheet_windows[handle - 1];
    NSView* root = (__bridge NSView*)aether_ui_get_widget(root_handle);
    if (root) [sheet setContentView:root];
}

// The window a sheet should attach to: the app's KEY window (the one the user
// is in — matters with multiple windows), falling back to the primary. A sheet
// belongs to whichever window raised it, not always the main one.
static NSWindow* aeui_sheet_parent_window(void) {
    NSWindow* key = [NSApp keyWindow];
    if (key == primary_window) return key;
    if (extra_windows && [extra_windows containsObject:key]) return key;
    return primary_window;
}

void aether_ui_sheet_present_impl(int handle) {
    if (aeui_is_headless()) return;  // sheet tracking needs an interactive runloop
    if (!sheet_windows || handle < 1 || handle > (int)[sheet_windows count]) return;
    NSWindow* sheet = sheet_windows[handle - 1];
    NSWindow* parent = aeui_sheet_parent_window();
    if (parent) {
        [parent beginSheet:sheet completionHandler:^(NSModalResponse r) { (void)r; }];
    } else {
        [sheet makeKeyAndOrderFront:nil];
    }
}

void aether_ui_sheet_dismiss_impl(int handle) {
    if (!sheet_windows || handle < 1 || handle > (int)[sheet_windows count]) return;
    NSWindow* sheet = sheet_windows[handle - 1];
    if (primary_window && [sheet sheetParent]) {
        [primary_window endSheet:sheet];
    } else {
        [sheet close];
    }
}

// NSImageScaling covers original, contain and stretch, but has no COVER:
// fill the box, keep the aspect ratio, crop whatever overflows. That is the
// one a thumbnail grid actually wants, so it is drawn here rather than
// declared unavailable.
@interface AetherImageView : NSImageView
@property (nonatomic) int fillMode;
@end
@implementation AetherImageView
- (void)drawRect:(NSRect)dirty {
    NSImage* img = [self image];
    NSSize is = img ? [img size] : NSZeroSize;
    if (self.fillMode != 2 || !img || is.width <= 0 || is.height <= 0) {
        [super drawRect:dirty];
        return;
    }
    NSRect b = [self bounds];
    CGFloat sx = b.size.width / is.width, sy = b.size.height / is.height;
    CGFloat sc = sx > sy ? sx : sy;               // MAX: cover, not contain
    NSRect dst = NSMakeRect(
        b.origin.x + (b.size.width  - is.width  * sc) * 0.5,
        b.origin.y + (b.size.height - is.height * sc) * 0.5,
        is.width * sc, is.height * sc);
    [NSGraphicsContext saveGraphicsState];
    [NSBezierPath clipRect:b];                    // crop the overflow
    [img drawInRect:dst fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver fraction:1.0];
    // AppKit applies contentTintColor when IT draws a template image. This
    // path draws the image itself, so the tint would be silently dropped and
    // the widget would report a tint it was not showing. SourceIn keeps the
    // alpha just drawn and replaces its colour, which is the same result by
    // hand.
    NSColor* tint = [self contentTintColor];
    if (tint) {
        [tint set];
        NSRectFillUsingOperation(dst, NSCompositingOperationSourceIn);
    }
    [NSGraphicsContext restoreGraphicsState];
}
@end

void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSImageView class]]) return;
    NSImageView* iv = (NSImageView*)v;
    if (!on) {
        [iv setContentTintColor:nil];
        [[iv image] setTemplate:NO];
        return;
    }
    // contentTintColor only reaches a TEMPLATE image: AppKit draws the alpha
    // shape in the tint and throws the source colours away. Marking it here
    // is what makes the tint visible rather than silently ignored.
    [[iv image] setTemplate:YES];
    [iv setContentTintColor:[NSColor colorWithRed:r green:g blue:b alpha:1.0]];
    [iv setNeedsDisplay:YES];
}

int aether_ui_image_get_tint(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSImageView class]]) return -1;
    NSColor* c = [(NSImageView*)v contentTintColor];
    if (!c) return -1;
    c = [c colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!c) return -1;
    return (((int)([c redComponent]   * 255) & 255) << 16)
         | (((int)([c greenComponent] * 255) & 255) << 8)
         |  ((int)([c blueComponent]  * 255) & 255)
         | 0x1000000;
}

void aether_ui_image_set_fill(int handle, int mode) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSImageView class]]) return;
    NSImageView* iv = (NSImageView*)v;
    if ([iv isKindOfClass:[AetherImageView class]]) {
        ((AetherImageView*)iv).fillMode = mode;
    }
    switch (mode) {
        case 1:  [iv setImageScaling:NSImageScaleProportionallyUpOrDown]; break;
        case 3:  [iv setImageScaling:NSImageScaleAxesIndependently];      break;
        // cover paints in drawRect:, so the built-in scaling stays out of it.
        case 2:  [iv setImageScaling:NSImageScaleNone];                   break;
        default: [iv setImageScaling:NSImageScaleNone];                   break;
    }
    [iv setNeedsDisplay:YES];
}

int aether_ui_image_get_fill(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSImageView class]]) return 0;
    if ([v isKindOfClass:[AetherImageView class]]) {
        int m = ((AetherImageView*)v).fillMode;
        if (m == 2) return 2;
    }
    switch ([(NSImageView*)v imageScaling]) {
        case NSImageScaleProportionallyUpOrDown: return 1;
        // AppKit's own default. It scales DOWN keeping aspect, which is
        // contain, not natural size, so reporting "original" here would
        // misdescribe what is on screen.
        case NSImageScaleProportionallyDown:     return 1;
        case NSImageScaleAxesIndependently:      return 3;
        default:                                 return 0;
    }
}

int aether_ui_image_create(const char* filepath) {
    AetherImageView* iv = [[AetherImageView alloc] init];
    // CRITICAL: state the default rather than inheriting each toolkit's own.
    // GtkPicture defaults to CONTAIN and NSImageView to ProportionallyDown;
    // leaving them implicit had the two backends disagree about what an
    // untouched image does. contain is the safe default: it never distorts
    // and never overflows, which is the whole complaint fill modes answer.
    [iv setImageScaling:NSImageScaleProportionallyUpOrDown];
    if (filepath && *filepath) {
        NSImage* img = [[NSImage alloc] initWithContentsOfFile:
            [NSString stringWithUTF8String:filepath]];
        [iv setImage:img];
    }
    return register_widget_typed((__bridge void*)iv, AUI_IMAGE);
}

// Decode encoded image bytes into an image view (no temp file). NSImage
// initWithData: uses the same ImageIO decoders as the file path — PNG /
// JPEG / GIF / etc. NB written from Linux; verify on the Mac.
int aether_ui_image_from_bytes(const char* data, int length) {
    AetherImageView* iv = [[AetherImageView alloc] init];
    [iv setImageScaling:NSImageScaleProportionallyUpOrDown];
    if (data && length > 0) {
        NSData* d = [NSData dataWithBytes:data length:(NSUInteger)length];
        NSImage* img = [[NSImage alloc] initWithData:d];
        if (img) [iv setImage:img];   // stays empty on decode failure
    }
    return register_widget_typed((__bridge void*)iv, AUI_IMAGE);
}

// NSWorkspace answers for a path that does not exist too: it falls back to
// the extension, which is what a directory listing needs before it stats.
static NSImage* aeui_icon_for_path(const char* path) {
    NSString* p = aeui_path_string(path);
    if (!p) return nil;   // iconForFile: requires a non-nil path
    return [[NSWorkspace sharedWorkspace] iconForFile:p];
}

int aether_ui_file_icon_create(const char* path) {
    AetherImageView* iv = [[AetherImageView alloc] init];
    NSImage* icon = aeui_icon_for_path(path);
    if (icon) [iv setImage:icon];
    return register_widget_typed((__bridge void*)iv, AUI_IMAGE);
}

void aether_ui_file_icon_set(int handle, const char* path) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSImageView class]]) return;
    [(NSImageView*)v setImage:aeui_icon_for_path(path)];
}

int aether_ui_image_has_content(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSImageView class]]) return 0;
    return [(NSImageView*)v image] != nil ? 1 : 0;
}

void aether_ui_image_set_size(int handle, int width, int height) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v) {
        [v setTranslatesAutoresizingMaskIntoConstraints:NO];
        [v.widthAnchor constraintEqualToConstant:width].active = YES;
        [v.heightAnchor constraintEqualToConstant:height].active = YES;
    }
}

// ---------------------------------------------------------------------------
// Canvas — NSView subclass replays command buffer via Core Graphics.
// ---------------------------------------------------------------------------

typedef enum {
    CANVAS_BEGIN_PATH,
    CANVAS_MOVE_TO,
    CANVAS_LINE_TO,
    CANVAS_STROKE,
    CANVAS_FILL_RECT,
    CANVAS_CLEAR,
    CANVAS_ARC,
    CANVAS_CLOSE_PATH,
    CANVAS_FILL,
    CANVAS_FILL_TEXT,
    CANVAS_STROKE_TEXT,   /* SVG stroke on <text>: outline over the fill */
    CANVAS_DRAW_IMAGE,
    CANVAS_FILL_LINEAR,
    CANVAS_FILL_RADIAL,
    CANVAS_CLIP_RECT
} CanvasCmdType;

typedef struct {
    CanvasCmdType type;
    double x, y;
    double r, g, b, a;
    double w, h;
    double a0, a1;   // ARC start/end angle
    char* text;     // FILL_TEXT string (owned)
    unsigned char* pixels;  // DRAW_IMAGE RGBA8888 buffer (owned)
    int iw, ih;     // DRAW_IMAGE pixel dims
    double gx1, gy1, gx2, gy2, gr, gfx, gfy;  // gradient geometry
    double grad_line_width;  // 0 → fill; >0 → stroke at this width
    /* The ELLIPSE a gradientTransform (or a non-square objectBoundingBox)
       produces: semi-axes and tilt. gr keeps the old scalar answer, so a
       command with grx == 0 renders exactly as it always did. */
    double grx, gry, grot;
    char* font_family;       /* owned; raw CSS stack, NULL when unset */
    int n_stops;
    double* stop_off;   // owned: offsets
    double* stop_rgba;  // owned: n_stops*4 colour comps
} CanvasCmd;

typedef struct {
    CanvasCmd* cmds;
    int count;
    int capacity;
    int widget_handle;
    AeClosure* on_move;    // pointer-move hook (canvas-local x,y); null = none
    AeClosure* on_click;   // press   (canvas-local x,y)
    AeClosure* on_release; // release (canvas-local x,y) — completes a drag
    AeClosure* on_key;     // key-down (key name: "Left", "a", "space", …)
    AeClosure* on_key_release; // key-up (same key names; driver-driven for now)
    AeClosure* on_resize;  // allocation change (w,h) — vg re-maps its viewBox
    int last_w, last_h;    // on_resize fires on CHANGE only, never per-frame
    double* paint_clip_rects;
    int paint_clip_count;
    int paint_clip_capacity;
    // Last-paint metrics for GET /canvas/{id}/debug — the compositor
    // instrumentation. `area` is the summed clip-rect area when the paint
    // was clipped, else the whole allocation, matching gtk4 and win32.
    int last_paint_w, last_paint_h;
    int last_paint_area, last_paint_count;
    // Cumulative full/clipped repaint counts and the last clipped paint's
    // summed area, matching gtk4 and win32 so the Stage-2.5 clip regression
    // reads the same numbers on all three backends.
    int paint_full_count, paint_clip_count_total, last_clip_area;
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
}

static void canvas_apply_paint_clip(CGContextRef cg, CanvasState* cs) {
    if (!cg || !cs || cs->paint_clip_count <= 0) return;
    CGContextBeginPath(cg);
    for (int i = 0; i < cs->paint_clip_count; i++) {
        double* r = &cs->paint_clip_rects[i * 4];
        if (r[2] > 0.0 && r[3] > 0.0) {
            CGContextAddRect(cg, CGRectMake(r[0], r[1], r[2], r[3]));
        }
    }
    CGContextClip(cg);
}

// Map an NSEvent key-down to the key NAME the DSL speaks. These are GDK's
// names, because GTK4 was the reference backend and the vg apps (and the
// driver's POST /canvas/{id}/key?name=) already spell them that way: "Left",
// "Return", "space", "a". Emitting AppKit-flavoured names instead would mean
// every canvas app needed a per-platform key table.
static void aeui_key_name_for_event(NSEvent* ev, char* out, int outsize) {
    switch ([ev keyCode]) {
        case 123: snprintf(out, outsize, "Left");      return;
        case 124: snprintf(out, outsize, "Right");     return;
        case 126: snprintf(out, outsize, "Up");        return;
        case 125: snprintf(out, outsize, "Down");      return;
        case 36:
        case 76:  snprintf(out, outsize, "Return");    return;
        case 53:  snprintf(out, outsize, "Escape");    return;
        case 48:  snprintf(out, outsize, "Tab");       return;
        case 49:  snprintf(out, outsize, "space");     return;
        case 51:  snprintf(out, outsize, "BackSpace"); return;
        case 117: snprintf(out, outsize, "Delete");    return;
        case 115: snprintf(out, outsize, "Home");      return;
        case 119: snprintf(out, outsize, "End");       return;
        case 116: snprintf(out, outsize, "Page_Up");   return;
        case 121: snprintf(out, outsize, "Page_Down"); return;
        default: break;
    }
    NSString* chars = [ev charactersIgnoringModifiers];
    if (chars && [chars length] > 0) {
        snprintf(out, outsize, "%s", [chars UTF8String]);
    } else {
        out[0] = '\0';
    }
}

@interface AetherCanvasView : NSView
@property (assign) int canvasId;
@end

// Replay a canvas command buffer into ANY CGContext. Factored out of
// drawRect: so the same code path serves both the on-screen view and the
// headless canvas_write_png bitmap — a screenshot that took a different route
// through the drawing code would be evidence of nothing.
//
// The context must already be in canvas coordinates (y down, origin top-left);
// the view gets that from isFlipped, the bitmap gets an explicit transform.
// Replay a RANGE [start, end) of the command buffer. Stage 3 (per-frame
// surfaces) needs this: frames emit their commands sequentially into the
// shared buffer, so one frame's drawing is a contiguous slice, and rendering
// that slice alone offscreen is what lets an unchanged frame be blitted
// instead of re-drawn. canvas_replay below is this with the full range, so
// every existing caller is unaffected.
/* Defined below, next to aeui_metrics_font so the drawing and measuring
   fonts stay one resolver. */
static NSFont* aeui_resolve_font(double size, const char* stack);
static void canvas_replay_range(CGContextRef cg, CanvasState* cs,
                                int start, int end) {
    if (!cg || !cs) return;
    if (start < 0) start = 0;
    if (end > cs->count) end = cs->count;
    {
    for (int i = start; i < end; i++) {
        CanvasCmd* c = &cs->cmds[i];
        switch (c->type) {
            case CANVAS_BEGIN_PATH:
                CGContextBeginPath(cg);
                break;
            case CANVAS_MOVE_TO:
                CGContextMoveToPoint(cg, c->x, c->y);
                break;
            case CANVAS_LINE_TO:
                CGContextAddLineToPoint(cg, c->x, c->y);
                break;
            case CANVAS_STROKE: {
                CGContextSetRGBStrokeColor(cg, c->r, c->g, c->b, c->a);
                CGContextSetLineWidth(cg, c->x);  // line_width stored in x
                // cap/join ride in iw/ih (0=butt/miter 1=round 2=square/
                // bevel — ui.canvas_stroke_cj's contract). These were
                // HARDCODED round, which the stroker suite's cross-renderer
                // pixel gate caught the first time macOS pixels were real:
                // a round-capped native stroke vs a butt/miter geometric
                // outline agreed on only 77% of samples.
                CGLineCap lc = kCGLineCapButt;
                if (c->iw == 1) lc = kCGLineCapRound;
                if (c->iw == 2) lc = kCGLineCapSquare;
                CGLineJoin lj = kCGLineJoinMiter;
                if (c->ih == 1) lj = kCGLineJoinRound;
                if (c->ih == 2) lj = kCGLineJoinBevel;
                CGContextSetLineCap(cg, lc);
                CGContextSetLineJoin(cg, lj);
                CGContextStrokePath(cg);
                break;
            }
            case CANVAS_CLIP_RECT:
                // Intersects the current clip and persists for the rest of the
                // frame (SVG viewport overflow:hidden). The context is fresh
                // each drawRect:, so there is nothing to restore.
                CGContextClipToRect(cg, CGRectMake(c->x, c->y, c->w, c->h));
                break;
            case CANVAS_FILL_RECT:
                CGContextSetRGBFillColor(cg, c->r, c->g, c->b, c->a);
                CGContextFillRect(cg, CGRectMake(c->x, c->y, c->w, c->h));
                break;
            case CANVAS_ARC:
                // CGContextAddArc appends to the current path. w = radius,
                // a0/a1 = start/end angle. clockwise=0 to match cairo's
                // positive-angle direction on a flipped (isFlipped) view.
                CGContextAddArc(cg, c->x, c->y, c->w, c->a0, c->a1, 0);
                break;
            case CANVAS_CLOSE_PATH:
                CGContextClosePath(cg);
                break;
            case CANVAS_FILL:
                CGContextSetRGBFillColor(cg, c->r, c->g, c->b, c->a);
                /* SVG fill-rule; iw carries 1 for evenodd. CoreGraphics has
                   a separate entry point rather than a mode flag. */
                if (c->iw) CGContextEOFillPath(cg);
                else       CGContextFillPath(cg);
                break;
            case CANVAS_STROKE_TEXT: {
                /* SVG stroke on <text>. Was a no-op here, so a glyph whose
                   visible colour comes from its STROKE rendered as the bare
                   fill -- bloglines.svg is <text stroke="white"> with no
                   fill, which takes SVG's default BLACK and came out a solid
                   black B where librsvg draws a white one.

                   AppKit has no "stroke this string" call, but the text
                   attributes do: a POSITIVE NSStrokeWidthAttributeName means
                   stroke-only (negative would mean fill AND stroke). It is
                   expressed as a PERCENTAGE OF FONT SIZE, not points, hence
                   the conversion. Emitted after the fill for the same run, so
                   the outline sits on top -- matching GTK4's
                   cairo_text_path + stroke. */
                if (c->text && c->w > 0) {
                    NSString* s2 = [NSString stringWithUTF8String:c->text];
                    NSColor* col = [NSColor colorWithRed:c->r green:c->g
                                                    blue:c->b alpha:c->a];
                    NSFont* font = aeui_resolve_font(c->w, c->font_family);
                    double pct = (c->h / c->w) * 100.0;
                    if (pct <= 0.0) pct = 1.0;
                    NSDictionary* attrs = @{
                        NSFontAttributeName: font,
                        NSStrokeColorAttributeName: col,
                        NSForegroundColorAttributeName: [NSColor clearColor],
                        NSStrokeWidthAttributeName: @(pct)
                    };
                    CGFloat ascent = [font ascender];
                    [s2 drawAtPoint:NSMakePoint(c->x, c->y - ascent)
                        withAttributes:attrs];
                }
                break;
            }
            case CANVAS_FILL_TEXT: {
                if (c->text) {
                    NSString* s = [NSString stringWithUTF8String:c->text];
                    NSColor* col = [NSColor colorWithRed:c->r green:c->g
                                                    blue:c->b alpha:c->a];
                    NSFont* font = aeui_resolve_font(c->w, c->font_family);
                    NSDictionary* attrs = @{
                        NSFontAttributeName: font,
                        NSForegroundColorAttributeName: col
                    };
                    // cairo's text origin is the baseline; NSString draws
                    // from the top-left, so offset up by the ascender.
                    CGFloat ascent = [font ascender];
                    [s drawAtPoint:NSMakePoint(c->x, c->y - ascent)
                        withAttributes:attrs];
                }
                break;
            }
            case CANVAS_DRAW_IMAGE: {
                if (c->pixels && c->iw > 0 && c->ih > 0) {
                    // RGBA8888, non-premultiplied — CoreGraphics can
                    // consume that directly via kCGImageAlphaLast.
                    CGColorSpaceRef cs2 = CGColorSpaceCreateDeviceRGB();
                    CGDataProviderRef prov = CGDataProviderCreateWithData(
                        NULL, c->pixels, c->iw * c->ih * 4, NULL);
                    CGImageRef img = CGImageCreate(
                        c->iw, c->ih, 8, 32, c->iw * 4, cs2,
                        kCGImageAlphaLast | kCGBitmapByteOrderDefault,
                        prov, NULL, false, kCGRenderingIntentDefault);
                    if (img) {
                        // Dest extent: w/h carry the SCALED size when the
                        // command came from draw_image_scaled (CGContext-
                        // DrawImage scales source to dest natively); zero
                        // means an unscaled draw_image — use pixel dims.
                        // This is what un-stubbed the "AppKit blits 1:1 for
                        // now" limitation: ebiten's set_scale presents a
                        // small logical framebuffer upscaled to the canvas,
                        // which rendered postage-stamp sized here.
                        double ddw = c->w > 0 ? c->w : (double)c->iw;
                        double ddh = c->h > 0 ? c->h : (double)c->ih;
                        // The view isFlipped (top-left origin), so draw
                        // into a rect at (x,y). CGContextDrawImage uses a
                        // bottom-left origin; flip the y within the rect.
                        CGContextSaveGState(cg);
                        CGContextTranslateCTM(cg, c->x, c->y + ddh);
                        CGContextScaleCTM(cg, 1.0, -1.0);
                        CGContextDrawImage(cg,
                            CGRectMake(0, 0, ddw, ddh), img);
                        CGContextRestoreGState(cg);
                        CGImageRelease(img);
                    }
                    CGDataProviderRelease(prov);
                    CGColorSpaceRelease(cs2);
                }
                break;
            }
            case CANVAS_FILL_LINEAR:
            case CANVAS_FILL_RADIAL: {
                if (c->n_stops > 0) {
                    CGColorSpaceRef gcs = CGColorSpaceCreateDeviceRGB();
                    CGFloat* comps = (CGFloat*)malloc(sizeof(CGFloat) * c->n_stops * 4);
                    CGFloat* locs  = (CGFloat*)malloc(sizeof(CGFloat) * c->n_stops);
                    for (int si = 0; si < c->n_stops; si++) {
                        comps[si*4+0] = c->stop_rgba[si*4+0];
                        comps[si*4+1] = c->stop_rgba[si*4+1];
                        comps[si*4+2] = c->stop_rgba[si*4+2];
                        comps[si*4+3] = c->stop_rgba[si*4+3];
                        locs[si] = c->stop_off[si];
                    }
                    CGGradientRef grad = CGGradientCreateWithColorComponents(
                        gcs, comps, locs, c->n_stops);
                    if (grad) {
                        // Clip to the current path (or its stroked
                        // outline for a gradient stroke), then draw.
                        CGContextSaveGState(cg);
                        if (c->grad_line_width > 0) {
                            CGContextSetLineWidth(cg, c->grad_line_width);
                            CGContextSetLineCap(cg, kCGLineCapRound);
                            CGContextSetLineJoin(cg, kCGLineJoinRound);
                            CGContextReplacePathWithStrokedPath(cg);
                        }
                        CGContextClip(cg);  // uses current path as clip
                        if (c->type == CANVAS_FILL_LINEAR) {
                            CGContextDrawLinearGradient(cg, grad,
                                CGPointMake(c->gx1, c->gy1),
                                CGPointMake(c->gx2, c->gy2),
                                kCGGradientDrawsBeforeStartLocation |
                                kCGGradientDrawsAfterEndLocation);
                        } else if (c->grx > 0 && c->gry > 0 &&
                                   (fabs(c->grx - c->gry) > 0.01 ||
                                    fabs(c->grot) > 0.01)) {
                            /* A TRUE ELLIPSE. CGContextDrawRadialGradient
                               only draws circles, but the CTM is ours to
                               bend: translate to the centre, rotate by the
                               tilt, scale the axes, then draw a UNIT circle
                               in that space. Already inside a
                               SaveGState/RestoreGState pair, so the CTM
                               change cannot leak.

                               The focal point is transformed the same way --
                               per axis, de-rotated first -- because it lives
                               in the same space. Collapsing it to one
                               distance is only right for a circle and cost
                               intertwingly.svg +2.90 when GTK4 landed. */
                            double fdx = c->gfx - c->gx1;
                            double fdy = c->gfy - c->gy1;
                            if (fabs(c->grot) > 0.01) {
                                double a = -c->grot * M_PI / 180.0;
                                double ca = cos(a), sa = sin(a);
                                double tx = fdx * ca - fdy * sa;
                                double ty = fdx * sa + fdy * ca;
                                fdx = tx; fdy = ty;
                            }
                            CGContextTranslateCTM(cg, c->gx1, c->gy1);
                            if (fabs(c->grot) > 0.01)
                                CGContextRotateCTM(cg, c->grot * M_PI / 180.0);
                            CGContextScaleCTM(cg, c->grx, c->gry);
                            CGContextDrawRadialGradient(cg, grad,
                                CGPointMake(fdx / c->grx, fdy / c->gry), 0,
                                CGPointMake(0, 0), 1.0,
                                kCGGradientDrawsBeforeStartLocation |
                                kCGGradientDrawsAfterEndLocation);
                        } else {
                            CGContextDrawRadialGradient(cg, grad,
                                CGPointMake(c->gfx, c->gfy), 0,
                                CGPointMake(c->gx1, c->gy1), c->gr,
                                kCGGradientDrawsBeforeStartLocation |
                                kCGGradientDrawsAfterEndLocation);
                        }
                        CGContextRestoreGState(cg);
                        CGGradientRelease(grad);
                    }
                    free(comps);
                    free(locs);
                    CGColorSpaceRelease(gcs);
                }
                break;
            }
            case CANVAS_CLEAR:
                break;
        }
    }
    }
}

static void canvas_replay(CGContextRef cg, CanvasState* cs) {
    if (!cs) return;
    canvas_replay_range(cg, cs, 0, cs->count);
}

// How many commands the buffer currently holds. Bracket a piece of drawing
// with two reads and the difference is that drawing's contiguous slice.
int aether_ui_canvas_cmd_count_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->count : -1;
}

// Render command range [start, end) offscreen and copy it out as tightly
// packed, NON-premultiplied RGBA8 (w*h*4). Returns bytes written, 0 on error.
// The win32/macOS half of Stage 3 (Stage 2.5b).
//
// CoreGraphics gives us premultiplied alpha (kCGImageAlphaPremultipliedLast),
// but canvas_draw_image expects straight RGBA, so this unpremultiplies on the
// way out. Skipping that makes translucent content -- exactly what
// frames_demo uses -- come out too dark.
//
// Commands carry ABSOLUTE canvas coordinates while a per-frame surface is
// frame-local, so the render is offset by (ox, oy). Note the y-flip is
// applied FIRST (bitmap contexts are y-up, the command buffer is y-down),
// then the frame origin is translated in the flipped space.
int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end,
                                            double ox, double oy,
                                            int width, int height,
                                            unsigned char* out, int out_len) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !out || width <= 0 || height <= 0) return 0;
    int need = width * height * 4;
    if (out_len < need) return 0;

    CGColorSpaceRef cspace = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(
        NULL, (size_t)width, (size_t)height, 8, 0, cspace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cspace);
    if (!cg) return 0;

    CGContextTranslateCTM(cg, 0, height);
    CGContextScaleCTM(cg, 1.0, -1.0);
    CGContextTranslateCTM(cg, -ox, -oy);

    canvas_replay_range(cg, cs, start, end);

    unsigned char* data = (unsigned char*)CGBitmapContextGetData(cg);
    size_t stride = CGBitmapContextGetBytesPerRow(cg);
    if (!data) { CGContextRelease(cg); return 0; }
    for (int y = 0; y < height; y++) {
        unsigned char* row = data + (size_t)y * stride;
        unsigned char* dst = out + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            unsigned int r = row[x * 4 + 0];
            unsigned int g = row[x * 4 + 1];
            unsigned int b = row[x * 4 + 2];
            unsigned int a = row[x * 4 + 3];
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
    CGContextRelease(cg);
    return need;
}

@implementation AetherCanvasView
- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGContextRef cg = [[NSGraphicsContext currentContext] CGContext];
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (cs) {
        NSRect b = [self bounds];
        int pw = (int)(b.size.width + 0.5), ph = (int)(b.size.height + 0.5);
        double a = 0.0;
        for (int i = 0; i < cs->paint_clip_count; i++) {
            double* r = &cs->paint_clip_rects[i * 4];
            if (r[2] > 0.0 && r[3] > 0.0) a += r[2] * r[3];
        }
        cs->last_paint_w = pw;
        cs->last_paint_h = ph;
        cs->last_paint_count = cs->count;
        if (cs->paint_clip_count > 0) {
            cs->last_clip_area = (int)(a + 0.5);
            cs->last_paint_area = cs->last_clip_area;
            cs->paint_clip_count_total++;
        } else {
            cs->last_clip_area = 0;
            cs->last_paint_area = pw * ph;
            cs->paint_full_count++;
        }
    }
    if (cs && cs->paint_clip_count > 0) {
        CGContextSaveGState(cg);
        canvas_apply_paint_clip(cg, cs);
        canvas_replay(cg, cs);
        CGContextRestoreGState(cg);
        cs->paint_clip_count = 0;
    } else {
        canvas_replay(cg, cs);
    }
}

// Pointer-move hover: a tracking area over the whole view delivers mouseMoved:;
// we forward the view-local point (already top-left origin since isFlipped) to
// the canvas's on_move closure. updateTrackingAreas keeps it sized to the view.
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* ta in [self.trackingAreas copy]) {
        [self removeTrackingArea:ta];
    }
    NSTrackingArea* ta = [[NSTrackingArea alloc]
        initWithRect:[self bounds]
             options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:ta];
}

- (void)mouseMoved:(NSEvent*)event {
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (!cs || !cs->on_move || !cs->on_move->fn) return;
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    ((void(*)(void*, double, double))cs->on_move->fn)(cs->on_move->env, p.x, p.y);
}

// The view is isFlipped, so its coords are already canvas coords (top-left
// origin) — no y-flip needed on any of these.
- (void)mouseDown:(NSEvent*)event {
    CanvasState* cs = get_canvas_state(self.canvasId);
    // Take key focus so keyDown: starts arriving (a canvas game needs this).
    [[self window] makeFirstResponder:self];
    if (!cs || !cs->on_click || !cs->on_click->fn) return;
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    ((void(*)(void*, double, double))cs->on_click->fn)(cs->on_click->env, p.x, p.y);
}

- (void)mouseUp:(NSEvent*)event {
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (!cs || !cs->on_release || !cs->on_release->fn) return;
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    ((void(*)(void*, double, double))cs->on_release->fn)(cs->on_release->env, p.x, p.y);
}

- (void)mouseDragged:(NSEvent*)event {
    // A drag is a move with the button held — vg hit-tests press→drag→release
    // as one gesture, and without this the middle of every swipe is missing.
    [self mouseMoved:event];
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent*)event {
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (!cs || !cs->on_key || !cs->on_key->fn) {
        [super keyDown:event];
        return;
    }
    char name[32];
    aeui_key_name_for_event(event, name, sizeof(name));
    ((void(*)(void*, const char*))cs->on_key->fn)(cs->on_key->env, name);
}

- (void)setFrameSize:(NSSize)newSize {
    NSSize old = [self frame].size;
    [super setFrameSize:newSize];
    if ((int)lround(old.width) == (int)lround(newSize.width)
        && (int)lround(old.height) == (int)lround(newSize.height)) return;
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (!cs || !cs->on_resize || !cs->on_resize->fn) return;
    int w = (int)lround(newSize.width), h = (int)lround(newSize.height);
    if (w == cs->last_w && h == cs->last_h) return;   // change only
    cs->last_w = w;
    cs->last_h = h;
    AeClosure* c = cs->on_resize;
    // Deferred: the closure re-flushes the whole vg scene (mutating the
    // command buffer we may be mid-draw on).
    dispatch_async(dispatch_get_main_queue(), ^{
        ((void(*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)w, (intptr_t)h);
    });
}
@end

int aether_ui_canvas_create_impl(int width, int height) {
    AetherCanvasView* v = [[AetherCanvasView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    [v setTranslatesAutoresizingMaskIntoConstraints:NO];
    // The given size is the canvas's NATURAL size, not a cage. GTK's drawing
    // area expands to fill its parent and re-maps the vg viewBox on resize;
    // required constraints here would pin the canvas forever and on_resize
    // would never fire. Low priority = "this size unless there's room for more".
    // Apps that hit-test against a fixed viewBox must unmap px→viewBox with
    // vg.scene_px_to_vb_x/y (as grand_perspective does), NOT assume the canvas
    // stays its requested size.
    NSLayoutConstraint* wc = [v.widthAnchor constraintEqualToConstant:width];
    NSLayoutConstraint* hc = [v.heightAnchor constraintEqualToConstant:height];
    // Priority 150 — deliberately BELOW a button's content-hugging (200). Both
    // a canvas and a button will grow to absorb a stack's leftover space; the
    // one with the lower resistance wins it. A canvas is the natural
    // slack-taker (its vg scene rescales to fill), so it must out-compete
    // buttons — otherwise the leftover lands in the button row and Reset /
    // Shuffle / Solve balloon to 146px tall while the canvas sits at its
    // requested size (the rubiks_cube bug). The calculator has no canvas, so
    // its buttons still stretch into a grid, unaffected.
    wc.priority = 150;
    hc.priority = 150;
    wc.active = YES;
    hc.active = YES;
    // ...and it must actively WANT the slack, not merely tolerate it. GTK's
    // drawing area sets hexpand/vexpand TRUE, so a vg scene fills the window
    // below the toolbar. Without this the canvas sits at its natural height and
    // everything below the fold is unreachable — a click the spec aims at the
    // treemap lands outside the view entirely.
    [v setContentHuggingPriority:1
                  forOrientation:NSLayoutConstraintOrientationHorizontal];
    [v setContentHuggingPriority:1
                  forOrientation:NSLayoutConstraintOrientationVertical];

    if (canvas_state_count >= canvas_state_capacity) {
        canvas_state_capacity = canvas_state_capacity == 0 ? 16 : canvas_state_capacity * 2;
        canvas_states = realloc(canvas_states, sizeof(CanvasState) * canvas_state_capacity);
    }
    CanvasState* cs = &canvas_states[canvas_state_count];
    memset(cs, 0, sizeof(*cs));
    canvas_state_count++;
    int canvas_id = canvas_state_count;

    v.canvasId = canvas_id;
    cs->widget_handle = register_widget_typed((__bridge void*)v, AUI_CANVAS);
    return canvas_id;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->widget_handle : 0;
}

// A canvas re-maps its vg viewBox when its allocation changes; on_resize
// delivers the new (w, h) so the scene can rescale. The canvas already fills
// (see canvas_create), so vg apps that hit-test must unmap px→viewBox rather
// than assume a fixed canvas size.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_resize = (AeClosure*)boxed_closure;
}

void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_click = (AeClosure*)boxed_closure;
}

void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_move = (AeClosure*)boxed_closure;
    // Ensure the view has a tracking area now (updateTrackingAreas also runs on
    // layout). The window must accept mouse-moved events for mouseMoved: to fire.
    NSView* v = (__bridge NSView*)aether_ui_get_widget(cs->widget_handle);
    if (v) {
        [v updateTrackingAreas];
        if (v.window) { [v.window setAcceptsMouseMovedEvents:YES]; }
    }
}

// Keyboard input on a canvas. No-op stub for now — the AppKit bridge would
// route NSView keyDown: → key-name string into the closure (mirrors the GTK4
// GtkEventControllerKey path). The Linux backend is the reference impl.
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key = (AeClosure*)boxed_closure;
}

// Key-up on a canvas. Stored (and driver-drivable via POST /canvas/{id}/keyup)
// like on_key; the real NSView keyUp: bridge lands with the keyDown: one.
void aether_ui_canvas_on_key_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key_release = (AeClosure*)boxed_closure;
}

/* canvas scroll: GTK4-only for now (a zoom-capable canvas needs a real
   scroll controller). Stubbed so the ABI stays uniform across backends. */
/* Gesture probe: GTK4-only diagnostic. Stubbed for ABI uniformity. */
void aether_ui_canvas_gesture_probe_impl(int canvas_id, void* boxed_closure) {
    (void)canvas_id; (void)boxed_closure;
}

void aether_ui_canvas_on_scroll_impl(int canvas_id, void* boxed_closure) {
    (void)canvas_id; (void)boxed_closure;
}

void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_release = (AeClosure*)boxed_closure;
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

// Viewport clip — no-op on AppKit for now (GTK-verified feature; AppKit can
// add a CGContextClip path later).
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y,
                                     double w, double h) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_CLIP_RECT, .x = x, .y = y, .w = w, .h = h });
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
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs) cs->paint_clip_count = 0;
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
        .iw = font_flags,
        .r = r, .g = g, .b = b, .a = a,
        .font_family = (font_family && font_family[0]) ? strdup(font_family) : NULL,
        .text = text ? strdup(text) : NULL
    });
}

// Stroke (outline) text — STUB. AppKit outline is CGContextSetTextDrawingMode
// (kCGTextStroke) or a CGPath from CTFont; deferred to when we're next on the
// Mac mini. No-op keeps the ABI linkable (the GTK4 backend is real).
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text,
                                        double x, double y, double font_size,
                                        double line_width, int font_flags,
                                        const char* font_family,
                                        double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_STROKE_TEXT, .x = x, .y = y, .w = font_size,
        .h = line_width, .iw = font_flags,
        .r = r, .g = g, .b = b, .a = a,
        .font_family = (font_family && font_family[0]) ? strdup(font_family) : NULL,
        .text = text ? strdup(text) : NULL
    });
}

// ---------------------------------------------------------------------------
// Text metrics (CoreText).
//
// The load-bearing invariant: these MUST measure the same font that
// CANVAS_FILL_TEXT draws with ([NSFont systemFontOfSize:], see drawRect:).
// vg centres and ellipsizes text using these numbers, so if the measuring
// font and the drawing font ever diverge, every centred label drifts by a
// few pixels and nothing in the code will say why.
//
// Headless-safe by construction: NSFont metrics need no window, no canvas,
// and no display — which is the whole reason GTK4 measures with a 1x1 scratch
// cairo surface rather than through a widget.
// ---------------------------------------------------------------------------
/* Resolve a CSS font stack with CoreText/AppKit's own matcher -- the point of
   carrying the raw string rather than a pre-resolved face: answering "best
   available for this list" is a platform question.

   font-family is a PRIORITISED LIST, so walk it in order and take the first
   entry AppKit can instantiate. Generic CSS families map to the system's
   equivalents. Falls back to systemFontOfSize:, which is what this backend
   used for every glyph before the stack was carried at all.

   Used by BOTH the drawing path and aeui_metrics_font below, deliberately:
   the comment there warns that if the measuring font and the drawing font
   diverge, every centred label drifts by a few pixels and nothing in the
   code says why. One resolver, both callers. */
static NSFont* aeui_resolve_font(double size, const char* stack) {
    CGFloat sz = (size > 0 ? size : 16.0);
    if (stack && stack[0]) {
        NSString* all = [NSString stringWithUTF8String:stack];
        for (NSString* raw in [all componentsSeparatedByString:@","]) {
            NSString* name = [raw stringByTrimmingCharactersInSet:
                [NSCharacterSet characterSetWithCharactersInString:@" \t'\""]];
            if ([name length] == 0) continue;
            NSString* lower = [name lowercaseString];
            if ([lower isEqualToString:@"serif"])
                return [NSFont fontWithName:@"Times New Roman" size:sz]
                       ?: [NSFont fontWithName:@"Times" size:sz]
                       ?: [NSFont systemFontOfSize:sz];
            if ([lower isEqualToString:@"sans-serif"])
                return [NSFont fontWithName:@"Helvetica" size:sz]
                       ?: [NSFont systemFontOfSize:sz];
            if ([lower isEqualToString:@"monospace"])
                return [NSFont fontWithName:@"Menlo" size:sz]
                       ?: [NSFont fontWithName:@"Courier" size:sz]
                       ?: [NSFont userFixedPitchFontOfSize:sz];
            if ([lower isEqualToString:@"cursive"])
                return [NSFont fontWithName:@"Apple Chancery" size:sz]
                       ?: [NSFont systemFontOfSize:sz];
            if ([lower isEqualToString:@"fantasy"])
                return [NSFont fontWithName:@"Papyrus" size:sz]
                       ?: [NSFont systemFontOfSize:sz];
            NSFont* f = [NSFont fontWithName:name size:sz];
            if (f) return f;
        }
    }
    return [NSFont systemFontOfSize:sz];
}

static NSFont* aeui_metrics_font(double size) {
    return [NSFont systemFontOfSize:(size > 0 ? size : 16.0)];
}

double aether_ui_text_measure(double size, const char* text) {
    if (!text || !text[0]) return 0.0;
    NSString* s = [NSString stringWithUTF8String:text];
    if (!s) return 0.0;
    NSAttributedString* as = [[NSAttributedString alloc]
        initWithString:s
            attributes:@{ NSFontAttributeName: aeui_metrics_font(size) }];
    CTLineRef line = CTLineCreateWithAttributedString(
        (__bridge CFAttributedStringRef)as);
    if (!line) return 0.0;
    // Typographic bounds width IS the pen advance — the same quantity cairo's
    // x_advance reports, and the one you position a sibling glyph run by.
    // (The *image* bounds would be tighter and would misplace trailing space.)
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    return w;
}

double aether_ui_font_ascent(double size) {
    return (double)[aeui_metrics_font(size) ascender];
}

double aether_ui_font_descent(double size) {
    // AppKit's descender is NEGATIVE (below the baseline); cairo's descent —
    // and therefore this ABI — is a positive distance. Flip it.
    return (double)-[aeui_metrics_font(size) descender];
}

double aether_ui_font_height(double size) {
    NSFont* f = aeui_metrics_font(size);
    return (double)([f ascender] - [f descender] + [f leading]);
}

void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_DRAW_IMAGE, .x = x, .y = y,
        .pixels = owned, .iw = iw, .ih = ih
    });
}

// Scaled draw — REAL now (was a 1:1 stub that ignored dw/dh, which made
// every upscaled presentation — the Ebiten port's set_scale framebuffer,
// video_frame's fitted region — render at source-pixel size on this
// backend only). The command carries the dest extent in w/h and the
// executor hands CGContextDrawImage a dest rect of that size; CG scales
// natively, same as GTK4's cairo path and win32's StretchBlt.
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y,
                                       double dw, double dh, int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_DRAW_IMAGE, .x = x, .y = y,
        .w = dw, .h = dh,
        .pixels = owned, .iw = iw, .ih = ih
    });
}

extern double floatarr_get_unchecked(void* arr, int i);

static void macos_copy_stops(CanvasCmd* c, int n_stops,
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
    (void)extend; // spreadMethod not yet honored on the CoreGraphics backend
    CanvasCmd cmd = { .type = CANVAS_FILL_LINEAR,
                      .gx1 = x1, .gy1 = y1, .gx2 = x2, .gy2 = y2,
                      .grad_line_width = line_width, .iw = cap, .ih = join };
    macos_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width, int extend,
        int cap, int join, double rx, double ry, double rot_deg) {
    (void)extend; // spreadMethod not yet honored on the CoreGraphics backend
    CanvasCmd cmd = { .type = CANVAS_FILL_RADIAL,
                      .gx1 = cx, .gy1 = cy, .gr = radius, .gfx = fx, .gfy = fy,
                      .grad_line_width = line_width, .iw = cap, .ih = join,
                      .grx = rx, .gry = ry, .grot = rot_deg };
    macos_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_clear_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    for (int i = 0; i < cs->count; i++) {
        CanvasCmd* c = &cs->cmds[i];
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
    NSView* v = (__bridge NSView*)aether_ui_get_widget(cs->widget_handle);
    if (v) [v setNeedsDisplay:YES];
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    NSView* v = (__bridge NSView*)aether_ui_get_widget(cs->widget_handle);
    if (v) [v setNeedsDisplay:YES];
}

// canvas_write_png — off-screen PNG render of the command buffer.
//
// Fully headless: a CGBitmapContext needs no window, no display and no run
// loop, so this works under CI exactly as it does live. That is house rule #4
// (everything renderable must render via canvas_write_png) and it is what lets
// a spec screenshot a vg scene on a box with no screen.
int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py,
                                     int width, int height) {
    // Replay the command buffer into a CGBitmapContext and read one pixel
    // — the same headless route canvas_write_png takes (and the same
    // contract as GTK4's cairo replay). Was a -1 stub, which made every
    // pixel probe read as ink and let colour-comparison specs pass
    // vacuously on this backend.
    if (px < 0 || py < 0 || px >= width || py >= height) return -1;
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return -1;
    __block int result = -1;
    void (^work)(void) = ^{
        CGColorSpaceRef cspace = CGColorSpaceCreateDeviceRGB();
        unsigned char* buf = calloc((size_t)width * (size_t)height, 4);
        if (!buf) { CGColorSpaceRelease(cspace); return; }
        CGContextRef cg = CGBitmapContextCreate(
            buf, (size_t)width, (size_t)height, 8, (size_t)width * 4, cspace,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cspace);
        if (!cg) { free(buf); return; }
        // Canvas coords are y-down; the bitmap is y-up (see canvas_write_png).
        CGContextTranslateCTM(cg, 0, height);
        CGContextScaleCTM(cg, 1.0, -1.0);
        canvas_replay(cg, cs);
        CGContextRelease(cg);
        // RGBA8 big-endian: byte order in memory is R,G,B,A.
        unsigned char* p8 = buf + ((size_t)py * (size_t)width + (size_t)px) * 4;
        result = ((int)p8[3] << 24) | ((int)p8[0] << 16)
               | ((int)p8[1] << 8) | (int)p8[2];
        free(buf);
    };
    if ([NSThread isMainThread]) work();
    else dispatch_sync(dispatch_get_main_queue(), work);
    return result;
}


// Which backend is this binary running? Lets pure-.ae code (the goldens
// gallery picks its tests/goldens/<backend>/ directory) branch without
// per-platform builds or environment guesswork.
const char* aether_ui_backend_name_impl(void) { return "macos"; }

int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !path || width <= 0 || height <= 0) return 0;

    CGColorSpaceRef cspace = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(
        NULL, (size_t)width, (size_t)height, 8, 0, cspace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cspace);
    if (!cg) return 0;

    // The command buffer is authored in canvas coordinates (y down from the
    // top-left); a bitmap context is y-up. The view gets the flip for free
    // from isFlipped — here it has to be applied by hand, or every scene
    // comes out mirrored.
    CGContextTranslateCTM(cg, 0, height);
    CGContextScaleCTM(cg, 1.0, -1.0);

    /* CANVAS_FILL_TEXT draws with -[NSString drawAtPoint:withAttributes:],
       which is AppKit and renders into +[NSGraphicsContext currentContext]
       -- NOT into whatever CGContextRef it is handed. Headless there is no
       focused view, so currentContext was nil and every glyph silently went
       nowhere: text-only SVGs came out completely blank while shapes in the
       same file drew fine (measured on a rect+text repro -- rect 19200 ink
       on both backends, text 776 on GTK4 and 0 here).

       Push a context backed by this bitmap for the duration of the replay.
       `flipped:YES` because the CTM above is already flipped to canvas
       coordinates; without it AppKit would draw the glyphs upside down. */
    NSGraphicsContext* prev = [NSGraphicsContext currentContext];
    NSGraphicsContext* nsctx =
        [NSGraphicsContext graphicsContextWithCGContext:cg flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:nsctx];

    canvas_replay(cg, cs);

    [NSGraphicsContext restoreGraphicsState];
    [NSGraphicsContext setCurrentContext:prev];

    CGImageRef img = CGBitmapContextCreateImage(cg);
    CGContextRelease(cg);
    if (!img) return 0;

    NSString* p = [NSString stringWithUTF8String:path];
    NSURL* url = [NSURL fileURLWithPath:p];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFStringRef)@"public.png", 1, NULL);
    if (!dest) { CGImageRelease(img); return 0; }
    CGImageDestinationAddImage(dest, img, NULL);
    BOOL ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(img);
    return ok ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Events — hover (NSTrackingArea), click + double-click (gesture recognizers)
// ---------------------------------------------------------------------------

@interface AetherHoverView : NSView
@property (assign) AeClosure* closure;
@property (strong) NSTrackingArea* trackingArea;
@end

@implementation AetherHoverView
- (void)updateTrackingAreas {
    if (self.trackingArea) {
        [self removeTrackingArea:self.trackingArea];
    }
    self.trackingArea = [[NSTrackingArea alloc]
        initWithRect:[self bounds]
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:self.trackingArea];
    [super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, intptr_t))self.closure->fn)(self.closure->env, (intptr_t)1);
    }
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, intptr_t))self.closure->fn)(self.closure->env, (intptr_t)0);
    }
}
@end

// NSTrackingArea attached to an existing view via a helper object.
@interface AetherHoverMonitor : NSObject
@property (assign) AeClosure* closure;
@property (weak) NSView* view;
@property (strong) NSTrackingArea* trackingArea;
- (void)attach;
@end

@implementation AetherHoverMonitor
- (void)attach {
    self.trackingArea = [[NSTrackingArea alloc]
        initWithRect:[self.view bounds]
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self.view addTrackingArea:self.trackingArea];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, intptr_t))self.closure->fn)(self.closure->env, (intptr_t)1);
    }
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*, intptr_t))self.closure->fn)(self.closure->env, (intptr_t)0);
    }
}
@end

void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    AetherHoverMonitor* m = [[AetherHoverMonitor alloc] init];
    m.closure = (AeClosure*)boxed_closure;
    m.view = v;
    [m attach];
    retain_target(m);
}

@interface AetherClickRecognizer : NSClickGestureRecognizer
@property (assign) AeClosure* closure;
- (void)clicked:(NSClickGestureRecognizer*)r;
@end

@implementation AetherClickRecognizer
- (void)clicked:(NSClickGestureRecognizer*)r {
    (void)r;
    if (self.closure && self.closure->fn) {
        ((void(*)(void*))self.closure->fn)(self.closure->env);
    }
}
@end

void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    // Also record it by handle so the driver can fire it (see widget_clicks).
    if (handle >= 1 && handle <= widget_count) {
        widget_clicks[handle - 1] = (AeClosure*)boxed_closure;
    }
    AetherClickRecognizer* rec = [[AetherClickRecognizer alloc] init];
    rec.closure = (AeClosure*)boxed_closure;
    [rec setTarget:rec];
    [rec setAction:@selector(clicked:)];
    rec.numberOfClicksRequired = 1;
    [v addGestureRecognizer:rec];
    retain_target(rec);
}

void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    AetherClickRecognizer* rec = [[AetherClickRecognizer alloc] init];
    rec.closure = (AeClosure*)boxed_closure;
    [rec setTarget:rec];
    [rec setAction:@selector(clicked:)];
    rec.numberOfClicksRequired = 2;
    [v addGestureRecognizer:rec];
    retain_target(rec);
    // Stash the closure for the driver's headless fire path.
    objc_setAssociatedObject(v, "aeui_dblclick",
        [NSValue valueWithPointer:boxed_closure], OBJC_ASSOCIATION_RETAIN);
}

int aether_ui_fire_double_click(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    NSValue* nv = objc_getAssociatedObject(v, "aeui_dblclick");
    if (!nv) return 0;
    AeClosure* c = (AeClosure*)[nv pointerValue];
    if (!c || !c->fn) return 0;
    ((void(*)(void*))c->fn)(c->env);
    return 1;
}

// Row drag-reorder. Native AppKit drag is a follow-up; store the drop closure
// so the reorder works via the driver's /widget/{id}/drop (model reorder is
// shared). (void)index — the row's index rides in the Aether closure.
void aether_ui_row_drag_reorder_impl(int row_handle, int index,
                                     void* on_drop_closure) {
    (void)index;
    NSView* v = (__bridge NSView*)aether_ui_get_widget(row_handle);
    if (!v) return;
    objc_setAssociatedObject(v, "aeui_rowdrop",
        [NSValue valueWithPointer:on_drop_closure], OBJC_ASSOCIATION_RETAIN);
}

int aether_ui_fire_row_drop(int row_handle, int src_index) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(row_handle);
    if (!v) return 0;
    NSValue* nv = objc_getAssociatedObject(v, "aeui_rowdrop");
    if (!nv) return 0;
    AeClosure* c = (AeClosure*)[nv pointerValue];
    if (!c || !c->fn) return 0;
    // The driver's /widget/{id}/drop route calls in on the HTTP server thread,
    // and this closure runs listbox_move → a full re-render (AppKit mutations).
    // Marshal to the main thread or AppKit aborts the process.
    void (^fire)(void) = ^{
        ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)src_index);
    };
    if ([NSThread isMainThread]) fire();
    else dispatch_sync(dispatch_get_main_queue(), fire);
    return 1;
}

// vlist native scroll — store the on_scroll(dy) closure on the container. The
// driver fires a step headlessly (fire_scroll); a real scrollWheel: gesture on
// the container is the AppKit follow-up. Fire marshals to the main thread (the
// closure re-renders via AppKit), like the row-drop path.
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(container_handle);
    if (!v) return;
    objc_setAssociatedObject(v, "aeui_vscroll",
        [NSValue valueWithPointer:on_scroll], OBJC_ASSOCIATION_RETAIN);
}

int aether_ui_fire_scroll(int container_handle, int dy) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(container_handle);
    if (!v) return 0;
    NSValue* nv = objc_getAssociatedObject(v, "aeui_vscroll");
    if (!nv) return 0;
    AeClosure* c = (AeClosure*)[nv pointerValue];
    if (!c || !c->fn) return 0;
    void (^fire)(void) = ^{
        ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)dy);
    };
    if ([NSThread isMainThread]) fire();
    else dispatch_sync(dispatch_get_main_queue(), fire);
    return 1;
}

void aether_ui_animate_opacity_impl(int handle, double target, int duration_ms) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = (double)duration_ms / 1000.0;
        [[v animator] setAlphaValue:target];
    } completionHandler:nil];
}

// ---------------------------------------------------------------------------
// Widget manipulation — remove / clear children.
// ---------------------------------------------------------------------------
// Retire a view and its whole subtree from the registry.
//
// The registry holds a STRONG (ARC) reference, so removeFromSuperview alone
// does not make a widget go away: the slot keeps the view alive and the driver
// keeps reporting it. A spec that removes a row then asserts it is gone from
// /widgets would fail forever, and the app would leak every row it ever built.
//
// GTK gets this for free (the widget is destroyed and its slot reads NULL);
// win32 gets it from IsWindow(). On AppKit it has to be done by hand. Slots
// are never reused — handles stay monotonic — the entry just goes dead and
// reports type "null".
static void unregister_view_tree(NSView* v) {
    if (!v) return;
    for (NSView* c in [[v subviews] copy]) {
        unregister_view_tree(c);
    }
    int h = handle_for_view(v);
    if (h < 1) return;
    widgets[h - 1] = nil;
    free(widget_classes[h - 1]);
    widget_classes[h - 1] = NULL;
    widget_clicks[h - 1] = NULL;
    widget_weights[h - 1] = 0;
}

void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    NSView* parent = (__bridge NSView*)aether_ui_get_widget(parent_handle);
    NSView* child = (__bridge NSView*)aether_ui_get_widget(child_handle);
    if (!parent || !child) return;
    if ([parent isKindOfClass:[NSStackView class]]) {
        [(NSStackView*)parent removeArrangedSubview:child];
    }
    [child removeFromSuperview];
    unregister_view_tree(child);
}

void aether_ui_clear_children_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    if ([v isKindOfClass:[NSStackView class]]) {
        NSStackView* s = (NSStackView*)v;
        for (NSView* sub in [[s arrangedSubviews] copy]) {
            [s removeArrangedSubview:sub];
            [sub removeFromSuperview];
            unregister_view_tree(sub);
        }
    } else {
        for (NSView* sub in [[v subviews] copy]) {
            [sub removeFromSuperview];
            unregister_view_tree(sub);
        }
    }
}

// ---------------------------------------------------------------------------
// Widget tree
// ---------------------------------------------------------------------------

// Mark `v` and every ancestor as wanting slack, and retract any equal-height
// row chain that this makes wrong.
//
// Retroactive by necessity: the DSL adds a container to its parent before
// populating it, so a row is already chained to its siblings by the time the
// canvas inside it announces that it expands.
static void aeui_mark_expand(NSView* v, int bits) {
    if (!v || !bits) return;

    int h = handle_for_view(v);
    if (h >= 1 && h <= widget_count) widget_expand[h - 1] |= bits;

    if (bits & AEUI_EXPAND_H) {
        [v setContentHuggingPriority:1
              forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    if (bits & AEUI_EXPAND_V) {
        [v setContentHuggingPriority:1
              forOrientation:NSLayoutConstraintOrientationVertical];
        // A row that expands must not stay pinned to the height of its inert
        // siblings — that is what flattened the treemap to the toolbar's height.
        NSView* p = [v superview];
        if (p) {
            NSMutableArray* drop = [NSMutableArray array];
            for (NSLayoutConstraint* c in [p constraints]) {
                if (![[c identifier] isEqualToString:@"aeui-roweq"]) continue;
                if (c.firstItem == v || c.secondItem == v) [drop addObject:c];
            }
            if ([drop count]) [p removeConstraints:drop];
        }
    }
    aeui_mark_expand([v superview], bits);   // carry it to the root
}

/* set_child — subview a widget into another (the chrome-drawn button
   face). NSButton keeps its title (text hook unaffected); the child view
   covers the face. */
void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) {
    NSView* p = (__bridge NSView*)aether_ui_get_widget(parent_handle);
    NSView* c = (__bridge NSView*)aether_ui_get_widget(child_handle);
    if (!p || !c) return;
    [c removeFromSuperview];
    [c setTranslatesAutoresizingMaskIntoConstraints:YES];
    [c setFrame:[p bounds]];
    [c setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [p addSubview:c];
}

void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    NSView* parent = (__bridge NSView*)aether_ui_get_widget(parent_handle);
    NSView* child = (__bridge NSView*)aether_ui_get_widget(child_handle);
    if (!parent || !child) return;

    if ([parent isKindOfClass:[NSSplitView class]]) {
        // Exactly two panes, in declaration order; a third is silently dropped
        // (GTK's GtkPaned does the same).
        NSSplitView* sv = (NSSplitView*)parent;
        if ([[sv subviews] count] >= 2) return;
        [child setTranslatesAutoresizingMaskIntoConstraints:YES];
        [sv addSubview:child];
        return;
    }

    if ([parent isKindOfClass:[AetherWrapView class]]) {
        // Flow layout positions children by frame, so they must not be
        // driven by Auto Layout.
        [child setTranslatesAutoresizingMaskIntoConstraints:YES];
        [parent addSubview:child];
        [parent setNeedsLayout:YES];
        [parent invalidateIntrinsicContentSize];
        return;
    }

    if ([parent isKindOfClass:[NSStackView class]]) {
        NSStackView* sv = (NSStackView*)parent;
        [sv addArrangedSubview:child];

        // Propagate "wants slack" from the child up the ancestor chain.
        //
        // GTK computes expand transitively: a box holding an expanding child
        // expands too, so a canvas nested three levels down still fills the
        // window. AppKit has no such rule — hugging is strictly per-view.
        //
        // It has to be RETROACTIVE, because the DSL assembles top-down: a
        // container is added to its parent BEFORE its own children arrive. So
        // by the time the canvas shows up, its row is already in the vstack
        // (and already height-chained to its siblings). aeui_mark_expand walks
        // the chain that exists now and retracts those chains.
        int ce = (child_handle >= 1 && child_handle <= widget_count)
                 ? widget_expand[child_handle - 1] : 0;
        if (ce) aeui_mark_expand(sv, ce);

        if ([sv orientation] == NSUserInterfaceLayoutOrientationVertical) {
            // In a vertical stack, arranged subviews only take their intrinsic
            // width by default. Pin container-like children to the parent's
            // leading/trailing anchors so nested hstacks (e.g. calculator rows)
            // fill the full width — matches GTK4 box behaviour.
            int ct = get_widget_type(child_handle);
            if (ct == AUI_HSTACK || ct == AUI_VSTACK || ct == AUI_ZSTACK ||
                ct == AUI_SCROLLVIEW || ct == AUI_FORM_SECTION ||
                ct == AUI_DIVIDER || ct == AUI_PROGRESSBAR ||
                ct == AUI_TEXTAREA || ct == AUI_NAVSTACK ||
                ct == AUI_SPLITVIEW || ct == AUI_WRAP || ct == AUI_CANVAS ||
                ct == AUI_TABS) {
                // Stretch to the full width WHEN YOU CAN, but never force the
                // parent to your width.
                //
                // A required equality on both edges looks right and is a trap:
                // a table's header row holds buttons with required fixed column
                // widths (220 + 90), so the row cannot stretch — and the
                // equality then propagates that 318px all the way up through the
                // vstack to the window, which could not be widened even by
                // /window/resize. table_demo asked for 520 and was capped at 318.
                //
                // leading == pins the left edge; trailing <= lets the row be
                // narrower than the parent; trailing == at high-but-not-required
                // priority makes it stretch whenever nothing forbids it (which is
                // what gives the calculator its full-width button rows).
                [child.leadingAnchor constraintEqualToAnchor:sv.leadingAnchor].active = YES;
                [child.trailingAnchor constraintLessThanOrEqualToAnchor:sv.trailingAnchor].active = YES;
                NSLayoutConstraint* stretch =
                    [child.trailingAnchor constraintEqualToAnchor:sv.trailingAnchor];
                stretch.priority = NSLayoutPriorityDefaultHigh;
                stretch.active = YES;
            }
            if (ct == AUI_SPLITVIEW) {
                // NSSplitView has NO intrinsic content size — it lays its panes
                // out by frame. Dropped into a stack it would collapse to
                // nothing, so it has to be told to absorb the slack (GtkPaned
                // gets this from hexpand/vexpand being TRUE).
                [child setContentHuggingPriority:1
                                  forOrientation:NSLayoutConstraintOrientationVertical];
                [child setContentCompressionResistancePriority:1
                                                forOrientation:NSLayoutConstraintOrientationVertical];
            }
            // Vertical peers: chain equal-height among hstack siblings in the
            // same vstack — with Fill distribution this gives grid-like row
            // heights (calculator) without affecting mixed vstacks whose
            // slack is absorbed by spacer().
            // Equal-height chain across hstack siblings — this is what gives
            // the calculator its grid-like button rows.
            //
            // But it must NOT apply to a row that wants to expand. Grand
            // Perspective is a toolbar row + a treemap-canvas row + a status
            // row in one vstack; equalising them pins the canvas to the height
            // of the toolbar (three rows of 207px in a 656px window) and the
            // whole treemap below the fold becomes unclickable. Rows that
            // absorb slack size themselves; only inert rows get chained.
            if (ct == AUI_HSTACK
                && !(widget_expand[child_handle - 1] & AEUI_EXPAND_V)) {
                for (NSView* sib in [sv arrangedSubviews]) {
                    if (sib == child) break;
                    int sh = handle_for_view(sib);
                    if (sh < 1 || get_widget_type(sh) != AUI_HSTACK) continue;
                    if (widget_expand[sh - 1] & AEUI_EXPAND_V) continue;
                    NSLayoutConstraint* eq =
                        [child.heightAnchor constraintEqualToAnchor:sib.heightAnchor];
                    // Tagged so it can be retracted if either row LATER gains an
                    // expanding child — see aeui_mark_expand.
                    [eq setIdentifier:@"aeui-roweq"];
                    eq.active = YES;
                    break;
                }
            }
        } else {
            // Horizontal stack: constrain all button children to equal width.
            // NSStackViewDistributionFill with multiple low-hugging siblings
            // lets autolayout give the slack to one child; an explicit
            // width-equality chain forces grid-like button rows (calculator)
            // without affecting label+textfield or button+spacer rows.
            //
            // Tagged, and skipped for any button that already carries an
            // explicit width: a table header sets per-column widths (220, 90),
            // and an equal-width chain on top of those is unsatisfiable —
            // Auto Layout breaks one of the required constraints and both
            // columns come out the same wrong size. set_width also RETRACTS
            // this chain, because the width may be applied after the attach.
            if ([child isKindOfClass:[NSButton class]]
                && !aeui_has_explicit_width(child)) {
                for (NSView* sib in [sv arrangedSubviews]) {
                    if (sib == child) break;
                    if (![sib isKindOfClass:[NSButton class]]) continue;
                    if (aeui_has_explicit_width(sib)) continue;
                    NSLayoutConstraint* eq =
                        [child.widthAnchor constraintEqualToAnchor:sib.widthAnchor];
                    [eq setIdentifier:@"aeui-btneq"];
                    eq.active = YES;
                    break;
                }
            }
        }
    } else if ([parent isKindOfClass:[NSScrollView class]]) {
        NSScrollView* sv = (NSScrollView*)parent;
        [sv setDocumentView:child];
        // CRITICAL: a document view with translatesAutoresizingMaskIntoConstraints
        // NO and no constraints of its own gets a ZERO frame, so the content
        // is not merely misplaced, it has no size at all. Pin it to the clip
        // view: top-left for position, width for a vertical scroller. Height
        // is deliberately left to the content, which is what makes it scroll.
        [child setTranslatesAutoresizingMaskIntoConstraints:NO];
        NSClipView* clip = [sv contentView];
        [child.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
        [child.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
        [child.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    } else if ([parent isKindOfClass:[NSBox class]]) {
        // shouldn't happen — section exposes its inner stack via handle+1
        [(NSBox*)parent setContentView:child];
    } else {
        [parent addSubview:child];
        // For zstack-style containers: pin child to parent bounds
        [child setTranslatesAutoresizingMaskIntoConstraints:NO];
        int t = get_widget_type(parent_handle);
        if (t == AUI_ZSTACK || t == AUI_NAVSTACK) {
            [child.leadingAnchor constraintEqualToAnchor:parent.leadingAnchor].active = YES;
            [child.trailingAnchor constraintEqualToAnchor:parent.trailingAnchor].active = YES;
            [child.topAnchor constraintEqualToAnchor:parent.topAnchor].active = YES;
            [child.bottomAnchor constraintEqualToAnchor:parent.bottomAnchor].active = YES;
        }
    }
}

void aether_ui_widget_set_hidden(int handle, int hidden) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v) [v setHidden:hidden != 0];
}

// ---------------------------------------------------------------------------
// AetherUIDriver — AppKit hooks for the SHARED HTTP test server.
//
// This backend used to carry its own hand-rolled HTTP server (~370 lines of
// sockets, parser and JSON emitter). It had drifted badly from the GTK4 one:
// no enabled/geometry/classes fields, no /focus, /window/key, /window/resize,
// no tray or notification routes, no URL-decoding — and it read NSView state
// straight off the server thread.
//
// It is gone. macOS now fills in AetherDriverHooks and lets
// aether_ui_test_server.c do the HTTP work, exactly as Win32 does. Route
// parity is now structural instead of a thing to remember: a route added to
// the shared server lands on both backends at once.
//
// Threading: introspection hooks are called on the HTTP thread (read-only
// queries AppKit tolerates). Every MUTATION goes through dispatch_action,
// which bounces to the main queue with dispatch_sync and blocks.
// ---------------------------------------------------------------------------

#include "aether_ui_test_server.h"

// Seal bookkeeping now lives in the shared server — one source of truth for
// "may automation touch this widget".
void aether_ui_seal_widget_impl(int handle) {
    aether_ui_test_server_seal_widget(handle);
}

static void seal_subtree_recursive(NSView* v) {
    if (!v) return;
    int h = handle_for_view(v);
    if (h > 0) aether_ui_seal_widget_impl(h);
    for (NSView* child in [v subviews]) {
        seal_subtree_recursive(child);
    }
}

void aether_ui_seal_subtree_impl(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v) seal_subtree_recursive(v);
}

static const char* widget_type_name_for(int handle) {
    switch (get_widget_type(handle)) {
        case AUI_TEXT: return "text";
        case AUI_BUTTON: return "button";
        case AUI_TOGGLE: return "toggle";
        case AUI_SLIDER: return "slider";
        case AUI_PICKER: return "picker";
        case AUI_TEXTFIELD: return "textfield";
        case AUI_SECUREFIELD: return "securefield";
        case AUI_TEXTAREA: return "textarea";
        case AUI_TEXTAREA_INNER: return "textarea_inner";
        case AUI_PROGRESSBAR: return "progressbar";
        case AUI_DIVIDER: return "divider";
        case AUI_SCROLLVIEW: return "scrollview";
        case AUI_VSTACK: return "vstack";
        case AUI_HSTACK: return "hstack";
        case AUI_ZSTACK: return "zstack";
        case AUI_SPACER: return "spacer";
        case AUI_CANVAS: return "canvas";
        case AUI_IMAGE: return "image";
        case AUI_FORM_SECTION: return "form_section";
        case AUI_FORM_SECTION_INNER: return "form_section_inner";
        case AUI_NAVSTACK: return "navstack";
        case AUI_BANNER: return "banner";
        case AUI_SPLITVIEW: return "splitview";
        case AUI_WRAP: return "wrap";
        case AUI_SCRIM: return "scrim";
        case AUI_TABS: return "tabs";
        default: return "widget";
    }
}

// ---------------------------------------------------------------------------
// Introspection hooks — HTTP thread.
// ---------------------------------------------------------------------------
static int hook_widget_count(void) { return widget_count; }

static const char* hook_widget_type(int handle) {
    if (handle < 1 || handle > widget_count) return "null";
    // A retired slot (see unregister_view_tree) must read as null, not as its
    // stale type — otherwise a removed row keeps answering "listbox_row".
    if (!aether_ui_get_widget(handle)) return "null";
    return widget_type_name_for(handle);
}

static void hook_widget_text_into(int handle, char* buf, int bufsize) {
    buf[0] = '\0';
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return;
    NSString* s = nil;
    if (get_widget_type(handle) == AUI_PICKER
        && [v isKindOfClass:[NSPopUpButton class]]) {
        // A picker's text tracks its SELECTION (parity with GTK, where the
        // dropdown reports the chosen item) — not the control's own title.
        s = [(NSPopUpButton*)v titleOfSelectedItem];
    } else if ([v isKindOfClass:[NSButton class]]) {
        s = [(NSButton*)v title];
    } else if ([v isKindOfClass:[NSTextField class]]) {
        s = [(NSTextField*)v stringValue];
    } else if ([v isKindOfClass:[NSTextView class]]) {
        s = [(NSTextView*)v string];
    }
    if (s) snprintf(buf, bufsize, "%s", [s UTF8String]);
}

static int hook_widget_visible(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    // The widget's OWN flag, not "is it on screen" — parity with GTK's
    // gtk_widget_get_visible and the win32 WS_VISIBLE read. A headless run
    // never maps the window, and testing ancestry would zero the whole app.
    return [v isHidden] ? 0 : 1;
}

static int hook_widget_parent(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    // Walk up to the nearest REGISTERED ancestor: AppKit interposes untracked
    // clip/content views inside a scrollview, and a raw superview would report
    // 0 (orphan) for anything inside one.
    for (NSView* p = [v superview]; p; p = [p superview]) {
        int h = handle_for_view(p);
        if (h > 0) return h;
    }
    return 0;
}

// ── Stylesheet-walk ABI (ui.apply_styles) ───────────────────────────
int aether_ui_widget_count_impl(void) { return widget_count; }

const char* aether_ui_widget_kind_impl(int handle) {
    const char* t = hook_widget_type(handle);
    return (t && strcmp(t, "null") != 0) ? t : "";
}

int aether_ui_widget_parent_impl(int handle) {
    return hook_widget_parent(handle);
}

const char* aether_ui_widget_classes_impl(int handle) {
    if (handle < 1 || handle > widget_count) return "";
    const char* c = widget_classes[handle - 1];
    return c ? c : "";
}

static int aeui_styled_readback(int handle, const char* key) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return -1;
    NSNumber* n = objc_getAssociatedObject(v, key);
    if (!n) return -1;
    int packed = [n intValue];
    return (packed & 0x1000000) ? (packed & 0xFFFFFF) : -1;
}

int aether_ui_styled_bg_impl(int handle) {
    // For a scroll view, answer from the property that actually PAINTS rather
    // than from the stashed request. The two used to disagree: the stash said
    // the colour had been set while the scroll view drew its own default over
    // the top, so a readback could report a theme that was not on screen.
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[NSScrollView class]]) {
        NSScrollView* sv = (NSScrollView*)v;
        if (![sv drawsBackground]) return -1;
        NSColor* c = [[sv backgroundColor]
            colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (!c) return -1;
        int packed = (((int)([c redComponent]   * 255) & 255) << 16)
                   | (((int)([c greenComponent] * 255) & 255) << 8)
                   |  ((int)([c blueComponent]  * 255) & 255);
        return packed;
    }
    return aeui_styled_readback(handle, "aeui_styled_bg");
}

int aether_ui_styled_fg_impl(int handle) {
    return aeui_styled_readback(handle, "aeui_styled_fg");
}

static int hook_toggle_active(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || get_widget_type(handle) != AUI_TOGGLE
        || ![v isKindOfClass:[NSButton class]]) return 0;
    return [(NSButton*)v state] == NSControlStateValueOn ? 1 : 0;
}

static double hook_slider_value(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSSlider class]]) return 0.0;
    return [(NSSlider*)v doubleValue];
}

static double hook_progressbar_fraction(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[NSProgressIndicator class]]) return 0.0;
    return [(NSProgressIndicator*)v doubleValue];
}

static int hook_widget_enabled(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    // Non-controls (stacks, labels) have no enabled state — report them
    // enabled, which is what GTK's get_sensitive does for a plain box.
    if (![v isKindOfClass:[NSControl class]]) return 1;
    return [(NSControl*)v isEnabled] ? 1 : 0;
}

// Window-local rect in TOP-LEFT coordinates. AppKit's y grows upward from the
// bottom of the content view, while GTK and Win32 both report y growing down
// from the top — so the flip here is what makes a geometry assertion in a spec
// mean the same thing on all three backends.
// Geometry read — MUST run on the main thread. alignmentRectForFrame: (and
// some convertRect: paths) touch the Auto Layout engine, and AppKit aborts the
// process if the layout engine is touched from a background thread after the
// main thread has used it ("Modifications to the layout engine must not be
// performed from a background thread"). The driver's introspection hooks run on
// the HTTP server thread, so an app with live/dynamic layout (LisMusic — an
// NSBox divider whose alignmentRectInsets forces a layout pass) crashes here
// unless the read is marshalled onto the main queue.
static int hook_widget_rect(int handle, int* x, int* y, int* w, int* hgt) {
    __block int rc = -1;
    __block int rx = 0, ry = 0, rw = 0, rh = 0;
    void (^compute)(void) = ^{
        NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
        if (!v) return;
        NSView* content = [[v window] contentView];
        if (!content) content = [primary_window contentView];
        if (!content) return;

        // Report the ALIGNMENT rect, not the raw frame. AppKit controls draw
        // outside their layout box (an NSTextField's frame is ~2px wider per
        // side than what Auto Layout positions), so reporting frames makes
        // adjacent siblings look like they overlap by 4px. GTK's x/y/w/h is the
        // allocation; the alignment rect is AppKit's word for the same thing.
        NSView* super_v = [v superview];
        NSRect r;
        if (super_v) {
            NSRect ar = [v alignmentRectForFrame:[v frame]];   // superview coords
            r = [super_v convertRect:ar toView:content];
        } else {
            r = [v convertRect:[v bounds] toView:content];
        }
        CGFloat ch = [content bounds].size.height;
        rx = (int)lround(r.origin.x);
        ry = (int)lround(ch - (r.origin.y + r.size.height));  // bottom-left → top-left
        rw = (int)lround(r.size.width);
        rh = (int)lround(r.size.height);
        rc = 0;
    };
    if ([NSThread isMainThread]) compute();
    else dispatch_sync(dispatch_get_main_queue(), compute);
    if (rc == 0) { *x = rx; *y = ry; *w = rw; *hgt = rh; }
    return rc;
}

static void hook_widget_classes_into(int handle, char* buf, int bufsize) {
    buf[0] = '\0';
    if (handle < 1 || handle > widget_count) return;
    const char* c = widget_classes[handle - 1];
    if (c) snprintf(buf, bufsize, "%s", c);
}

static void hook_widget_a11y(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    aether_ui_a11y_get_impl(handle, role, rolesz, name, namesz, desc, descsz);
}

static int hook_focused_widget(void) {
    NSWindow* win = primary_window;
    if (!win) return 0;
    id fr = [win firstResponder];
    if (![fr isKindOfClass:[NSView class]]) return 0;
    NSView* v = (NSView*)fr;
    // An NSTextField being edited hands first-responder to the window's FIELD
    // EDITOR (a shared NSTextView), not to the field itself — so a naive read
    // reports an untracked view and focus looks lost. Resolve back through the
    // field editor's delegate, then walk up to the nearest tracked ancestor.
    if ([v isKindOfClass:[NSTextView class]]
        && [(NSTextView*)v isFieldEditor]) {
        id del = [(NSTextView*)v delegate];
        if ([del isKindOfClass:[NSView class]]) v = (NSView*)del;
    }
    for (NSView* p = v; p; p = [p superview]) {
        int h = handle_for_view(p);
        if (h > 0) return h;
    }
    return 0;
}

// Public focus getter (DSL / shortcut predicates).
int aether_ui_focused_widget(void) { return hook_focused_widget(); }

// ---------------------------------------------------------------------------
// Tab order.
//
// AppKit's own key-view loop is NOT usable here: by default macOS only Tabs
// between text fields, and buttons join the loop only when the user has "Full
// Keyboard Access" switched on system-wide. Tab order would then depend on a
// setting outside the app, and the same spec would pass or fail on two Macs.
//
// So we walk the widget registry instead. Registry order IS build order (the
// handle is handed out at construction), which is precisely GTK's default
// focus chain — the contract the DSL documents. macOS therefore matches GTK
// exactly here, where win32 could not (it inherits the native dialog order).
// ---------------------------------------------------------------------------
static int aeui_is_focusable(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v || [v isHidden]) return 0;
    if ([v isKindOfClass:[NSTextField class]]) {
        NSTextField* tf = (NSTextField*)v;   // a label is a non-editable field
        return ([tf isEditable] && [tf isEnabled]) ? 1 : 0;
    }
    if ([v isKindOfClass:[NSTextView class]]) {
        return [(NSTextView*)v isEditable] ? 1 : 0;
    }
    if ([v isKindOfClass:[NSControl class]]) {
        return [(NSControl*)v isEnabled] ? 1 : 0;
    }
    return 0;
}

static int hook_focused_widget(void);

static int aeui_tab_move(int backward) {
    if (!primary_window) return 0;
    int cur = hook_focused_widget();
    // Scan outward from the current widget, wrapping around the registry.
    for (int step = 1; step <= widget_count; step++) {
        int idx = backward ? (cur - step) : (cur + step);
        while (idx < 1) idx += widget_count;
        while (idx > widget_count) idx -= widget_count;
        if (!aeui_is_focusable(idx)) continue;
        aether_ui_focus_impl(idx);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Mutation — marshalled to the main queue.
// ---------------------------------------------------------------------------
static void driver_perform(AetherDriverActionCtx* ctx) {
    // Actions with no widget subject, handled before the registry lookup.
    switch (ctx->action) {
        case AETHER_DRV_SET_STATE: {
            switch (aether_ui_state_type(ctx->handle)) {
                case 1: aether_ui_state_set_i(ctx->handle, atoi(ctx->sval)); break;
                case 2: aether_ui_state_set_b(ctx->handle,
                            (strcmp(ctx->sval, "true") == 0 || atoi(ctx->sval) != 0)); break;
                case 3: aether_ui_state_set_s(ctx->handle, ctx->sval); break;
                default: aether_ui_state_set(ctx->handle, ctx->dval);
            }
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_WIN_RESIZE: {
            NSWindow* win = primary_window;
            if (win) {
                // Retract the start-size floor — an explicit resize outranks
                // the size the app was born with, in both directions.
                aeui_win_floor_w.active = NO;
                aeui_win_floor_h.active = NO;
                // Pin the content to the requested size (strong, sub-required)
                // so the layout is BOUNDED — otherwise it re-adopts its fitting
                // size on the next pass and a resize to less than that is undone.
                // A bounded width is also what lets weight min-clamp resolve.
                NSView* host = [win contentView];
                if (host) {
                    aeui_win_cap_w.active = NO;
                    aeui_win_cap_h.active = NO;
                    aeui_win_cap_w = [host.widthAnchor constraintEqualToConstant:ctx->ival];
                    aeui_win_cap_h = [host.heightAnchor constraintEqualToConstant:ctx->ival2];
                    aeui_win_cap_w.priority = 800;
                    aeui_win_cap_h.priority = 800;
                    aeui_win_cap_w.active = YES;
                    aeui_win_cap_h.active = YES;
                }
                // Size the CONTENT area (the GTK route's semantics), not the
                // frame — a spec asserting w=600 means 600px of app, not 600
                // minus the titlebar.
                NSRect f = [win frame];
                NSRect target = [win frameRectForContentRect:
                    NSMakeRect(0, 0, ctx->ival, ctx->ival2)];
                // Keep the top-left pinned as the window grows downward.
                f.origin.y += f.size.height - target.size.height;
                f.size = target.size;
                [win setFrame:f display:YES];
                [[win contentView] layoutSubtreeIfNeeded];
            }
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_WIN_KEY: {
            // Registered accelerators win first; only then the built-ins.
            char canonical[64];
            combo_normalize(ctx->sval, canonical, sizeof(canonical));
            ctx->retval = shortcut_dispatch(canonical) ? 1 : 0;
            if (!ctx->retval) {
                // Same fall-through the real key monitor takes.
                char kname[64];
                int kmods = aeui_combo_split(canonical, kname, sizeof(kname));
                ctx->retval = aether_ui_window_key_deliver(kname, kmods);
            }
            if (!ctx->retval && primary_window) {
                if (strcmp(canonical, "tab") == 0) {
                    ctx->retval = aeui_tab_move(0);
                } else if (strcmp(canonical, "shift+tab") == 0) {
                    ctx->retval = aeui_tab_move(1);
                } else if (strcmp(canonical, "escape") == 0) {
                    ctx->retval = aeui_escape_overlays() ? 1 : 0;
                }
            }
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_SHUTDOWN: {
            // Exit by the same path a user-close takes so the port is
            // released cleanly; a signal-kill leaves a zombie holding 9222
            // and the next spec in the matrix interrogates the previous app.
            [NSApp terminate:nil];
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_PICK: {
            // A real AppKit hit-test at window-local (top-left) coords. This
            // is the proof that a modal scrim blocks input by z-order rather
            // than by an honour system.
            ctx->retval = 0;
            int want_scrim = 0;
            NSView* content = [primary_window contentView];
            if (content) {
                CGFloat ch = [content bounds].size.height;
                NSPoint p = NSMakePoint(ctx->ival, ch - ctx->ival2);  // flip back
                NSView* hit = [content hitTest:p];
                for (NSView* v = hit; v; v = [v superview]) {
                    int h = handle_for_view(v);
                    if (h > 0) {
                        if (get_widget_type(h) == AUI_SCRIM) { want_scrim = 1; }
                        else { ctx->retval = h; }
                        break;
                    }
                }
            }
            ctx->ival2 = want_scrim;   // out-param: on_scrim
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_SPLIT_POS: {
            if (ctx->ival >= 0) aether_ui_split_set_position_impl(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_split_position_impl(ctx->handle);
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_TAB_SELECT: {
            // Real NSTabView — switches the page and fires on_change via the
            // delegate. tabs_selected reports the resulting index.
            aether_ui_tabs_select(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_tabs_selected(ctx->handle);
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_CTX_MENU: {
            ctx->retval = aeui_ctx_menu_map(ctx->handle);
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_CTX_ACTIVATE: {
            ctx->retval = aeui_ctx_menu_activate(ctx->handle, ctx->ival);
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_CANVAS_CLICK:
        case AETHER_DRV_CANVAS_MOVE:
        case AETHER_DRV_CANVAS_RELEASE:
        case AETHER_DRV_CANVAS_KEY:
        case AETHER_DRV_CANVAS_KEYUP: {
            CanvasState* cs = get_canvas_state(ctx->handle);
            AeClosure* c = NULL;
            if (cs) {
                c = (ctx->action == AETHER_DRV_CANVAS_CLICK)   ? cs->on_click
                  : (ctx->action == AETHER_DRV_CANVAS_MOVE)    ? cs->on_move
                  : (ctx->action == AETHER_DRV_CANVAS_RELEASE) ? cs->on_release
                  : (ctx->action == AETHER_DRV_CANVAS_KEYUP)   ? cs->on_key_release
                                                               : cs->on_key;
            }
            if (!c || !c->fn) { ctx->result = 3; return; }  // 404: unwired, not missed
            if (ctx->action == AETHER_DRV_CANVAS_KEY
                || ctx->action == AETHER_DRV_CANVAS_KEYUP) {
                ((void(*)(void*, const char*))c->fn)(c->env, ctx->sval);
            } else {
                ((void(*)(void*, double, double))c->fn)(c->env, ctx->dval, ctx->dval2);
            }
            ctx->result = 0;
            return;
        }
        default: break;
    }

    NSView* v = (__bridge NSView*)aether_ui_get_widget(ctx->handle);
    if (!v) {
        // hover(0) = "the pointer is over NOTHING": clear every hover.
        if (ctx->action == AETHER_DRV_HOVER && ctx->handle == 0) {
            int n0 = aether_ui_widget_count_impl();
            for (int i = 1; i <= n0; i++) {
                NSView* pv = (__bridge NSView*)aether_ui_get_widget(i);
                if (pv) objc_setAssociatedObject(pv, "aeui-hovered", nil,
                                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            ctx->retval = 1; ctx->result = 0; return;
        }
        ctx->result = 3; return;
    }
    if (ctx->action == AETHER_DRV_FOCUS) {
        aether_ui_focus_impl(ctx->handle);
        ctx->result = 0;
        return;
    }
    if (ctx->handle == aether_ui_test_server_banner_handle()) { ctx->result = 2; return; }
    if (aether_ui_test_server_is_sealed(ctx->handle)) { ctx->result = 1; return; }

    switch (ctx->action) {
        case AETHER_DRV_HOVER: {
            // AppKit has no settable "hovered" flag, so record it on the
            // view and let the styled paths consult it — the same shape the
            // hover COLOURS already use (see aether_ui_set_state_style).
            int n = aether_ui_widget_count_impl();
            for (int i = 1; i <= n; i++) {
                NSView* pv = (__bridge NSView*)aether_ui_get_widget(i);
                if (pv) objc_setAssociatedObject(pv, "aeui-hovered", nil,
                                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            NSView* hv = (__bridge NSView*)aether_ui_get_widget(ctx->handle);
            if (hv) {
                objc_setAssociatedObject(hv, "aeui-hovered", @(1),
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                [hv setNeedsDisplay:YES];
                ctx->retval = 1;
            }
            break;
        }
        case AETHER_DRV_PRESS:
        case AETHER_DRV_RELEASE: {
            NSView* pv2 = (__bridge NSView*)aether_ui_get_widget(ctx->handle);
            if (pv2) {
                objc_setAssociatedObject(pv2, "aeui-pressed",
                    ctx->action == AETHER_DRV_PRESS ? @(1) : nil,
                    OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                [pv2 setNeedsDisplay:YES];
                ctx->retval = 1;
            }
            break;
        }
        case AETHER_DRV_CLICK:
            if ([v isKindOfClass:[NSButton class]]) {
                [(NSButton*)v performClick:nil];
            } else {
                // Any widget carrying an on_click closure — listbox rows are
                // plain containers, and a click on one must still fire.
                AeClosure* c = widget_click_closure(ctx->handle);
                if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
            }
            break;
        case AETHER_DRV_SET_TEXT: {
            NSString* s = [NSString stringWithUTF8String:ctx->sval];
            if ([v isKindOfClass:[NSTextField class]]) {
                // Route through the toolkit setter so a two-way bind_value
                // field mirrors driver input into its state (setStringValue
                // alone fires neither controlTextDidChange nor the write-back).
                aether_ui_textfield_set_text(ctx->handle, ctx->sval);
                // A programmatic setStringValue fires neither
                // controlTextDidChange nor the action — and these fields wire
                // on_change through the DELEGATE, not target/action, so the
                // sendAction: this used to do was a no-op against a null
                // action and driver-typed text never reached the app's
                // closure. (GTK4 notifies natively; win32 invokes the closure
                // explicitly for the same reason.) Invoke the delegate's
                // closure directly, same as a user keystroke would. The
                // bind_value state write-back already happened inside
                // set_text, so only the closure is owed here.
                if (get_widget_type(ctx->handle) != AUI_TEXT) {
                    id d = [(NSTextField*)v delegate];
                    if (d && [d isKindOfClass:[AetherTextFieldDelegate class]]) {
                        AetherTextFieldDelegate* td = (AetherTextFieldDelegate*)d;
                        if (td.closure && td.closure->fn) {
                            ((void(*)(void*, const char*))td.closure->fn)(
                                td.closure->env,
                                aether_ui_textfield_get_text(ctx->handle));
                        }
                    }
                }
            } else if ([v isKindOfClass:[NSTextView class]]) {
                [(NSTextView*)v setString:s];
                // Programmatic setString: does not post
                // NSTextDidChangeNotification either — same debt as the
                // textfield above, same repayment (win32 covers WK_TEXTAREA
                // in its fix for the same reason).
                id d = [(NSTextView*)v delegate];
                if (d && [d isKindOfClass:[AetherTextViewDelegate class]]) {
                    AetherTextViewDelegate* td = (AetherTextViewDelegate*)d;
                    if (td.closure && td.closure->fn) {
                        const char* cs = [[(NSTextView*)v string] UTF8String];
                        ((void(*)(void*, const char*))td.closure->fn)(
                            td.closure->env, cs ? cs : "");
                    }
                }
            }
            break;
        }
        case AETHER_DRV_TOGGLE:
            if ([v isKindOfClass:[NSButton class]]) {
                NSButton* b = (NSButton*)v;
                [b setState:([b state] == NSControlStateValueOn)
                             ? NSControlStateValueOff : NSControlStateValueOn];
                [b sendAction:[b action] to:[b target]];
            }
            break;
        case AETHER_DRV_SET_VALUE:
            if ([v isKindOfClass:[NSSlider class]]) {
                aether_ui_slider_set_value(ctx->handle, ctx->dval);
                [(NSSlider*)v sendAction:[(NSSlider*)v action]
                                      to:[(NSSlider*)v target]];
            } else if ([v isKindOfClass:[NSProgressIndicator class]]) {
                aether_ui_progressbar_set_fraction(ctx->handle, ctx->dval);
            } else if (get_widget_type(ctx->handle) == AUI_PICKER) {
                // set_selected fires the change callback (GTK does the same).
                aether_ui_picker_set_selected(ctx->handle, (int)ctx->dval);
            }
            break;
        default:
            break;
    }
    ctx->result = 0;
}

static void hook_dispatch_action(AetherDriverActionCtx* ctx) {
    if ([NSThread isMainThread]) {
        driver_perform(ctx);
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ driver_perform(ctx); });
    }
    ctx->done = 1;
}

static int hook_widget_children(int handle, int* out, int max) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return -1;
    NSArray* subs = [v isKindOfClass:[NSStackView class]]
        ? [(NSStackView*)v arrangedSubviews] : [v subviews];
    int n = 0;
    for (NSView* c in subs) {
        if (n >= max) break;
        int ch = handle_for_view(c);
        if (ch > 0) {
            if (out) out[n] = ch;
            n++;
        }
    }
    return n;
}

// Capture the window as a PNG — from the PRESENTATION layer where possible.
//
// This matters more than it looks. -[NSView animator] setAlphaValue: sets the
// MODEL value immediately and lets CoreAnimation interpolate the PRESENTATION
// layer toward it. cacheDisplayInRect: draws the model, so a screenshot taken
// mid-tween already shows the final value: every animation looks like an
// instant snap to the driver, and a spec that exists to prove a fade actually
// tweens can never pass. The presentation layer is what is on screen, and what
// a screenshot should mean.
//
// Falls back to cacheDisplayInRect: when there's no layer (or nothing has been
// committed yet — a headless run that never displays), which is the correct
// answer in exactly the cases where no animation can be in flight anyway.
static int hook_widget_hovered(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    return objc_getAssociatedObject(v, "aeui-hovered") ? 1 : 0;
}
static int hook_widget_pressed(int handle) {
    NSView* v = (__bridge NSView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    return objc_getAssociatedObject(v, "aeui-pressed") ? 1 : 0;
}

static int hook_screenshot_png(unsigned char** out_data, size_t* out_len) {
    __block NSData* png = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSWindow* win = primary_window;
        if (!win) return;
        NSView* v = [win contentView];
        if (!v) return;
        NSRect bounds = [v bounds];
        if (bounds.size.width <= 0 || bounds.size.height <= 0) return;

        CALayer* pres = [[v layer] presentationLayer];
        if (pres) {
            int w = (int)lround(bounds.size.width);
            int h = (int)lround(bounds.size.height);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef cg = CGBitmapContextCreate(
                NULL, (size_t)w, (size_t)h, 8, 0, cs,
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(cs);
            if (cg) {
                [pres renderInContext:cg];
                CGImageRef img = CGBitmapContextCreateImage(cg);
                CGContextRelease(cg);
                if (img) {
                    NSBitmapImageRep* rep =
                        [[NSBitmapImageRep alloc] initWithCGImage:img];
                    png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}];
                    CGImageRelease(img);
                }
            }
        }
        if (!png) {
            NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:bounds];
            if (!rep) return;
            [v cacheDisplayInRect:bounds toBitmapImageRep:rep];
            png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        }
    });
    if (!png || [png length] == 0) return -1;
    *out_len = (size_t)[png length];
    *out_data = (unsigned char*)malloc(*out_len);
    if (!*out_data) return -1;
    memcpy(*out_data, [png bytes], *out_len);
    return 0;
}

// Last-paint metrics for GET /canvas/{id}/debug. 0 on success, non-zero
// when the canvas id is unknown (the shared server maps that to 404; a
// NULL hook would be 501 "not wired on this backend").
static int hook_canvas_debug(int canvas_id, int* area, int* commands,
                             int* w, int* h) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    if (area) *area = cs->last_paint_area;
    if (commands) *commands = cs->last_paint_count;
    if (w) *w = cs->last_paint_w;
    if (h) *h = cs->last_paint_h;
    return 0;
}

static int hook_canvas_paint_counters(int canvas_id, int* full_paints,
                                      int* clip_paints, int* last_clip_area) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    if (full_paints) *full_paints = cs->paint_full_count;
    if (clip_paints) *clip_paints = cs->paint_clip_count_total;
    if (last_clip_area) *last_clip_area = cs->last_clip_area;
    return 0;
}

static const AetherDriverHooks macos_driver_hooks = {
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

// Inject the "Under Remote Control" banner as the first arranged subview.
// It is a REAL registered widget, so it shows up in /widgets with
// "banner":true and it shifts every other widget's y — which is exactly why
// the overlay/geometry specs read x/y/w/h from the driver instead of
// hardcoding pixel coordinates.
static void inject_remote_control_banner(int root_handle) {
    NSView* root = (__bridge NSView*)aether_ui_get_widget(root_handle);
    if (!root || ![root isKindOfClass:[NSStackView class]]) return;

    NSTextField* banner = [NSTextField labelWithString:@"Under Remote Control"];
    [banner setEditable:NO];
    [banner setBordered:NO];
    [banner setSelectable:NO];
    [banner setTextColor:[NSColor whiteColor]];
    [banner setFont:[NSFont boldSystemFontOfSize:12]];
    [banner setAlignment:NSTextAlignmentCenter];
    [banner setWantsLayer:YES];
    banner.layer.backgroundColor =
        [[NSColor colorWithRed:0.8 green:0.2 blue:0.2 alpha:1.0] CGColor];
    [banner setTranslatesAutoresizingMaskIntoConstraints:NO];
    [banner.heightAnchor constraintEqualToConstant:24].active = YES;

    int bh = register_widget_typed((__bridge void*)banner, AUI_BANNER);
    [(NSStackView*)root insertArrangedSubview:banner atIndex:0];
    [banner.leadingAnchor constraintEqualToAnchor:root.leadingAnchor].active = YES;
    [banner.trailingAnchor constraintEqualToAnchor:root.trailingAnchor].active = YES;

    aether_ui_test_server_set_banner(bh);
    aether_ui_seal_widget_impl(bh);   // the banner is not automatable
}

static int test_server_started = 0;
void aether_ui_enable_test_server_impl(int port, int root_handle) {
    // Idempotent: the env auto-start and an explicit app call must not stack
    // a second banner (it shows up as a duplicate "Under Remote Control").
    if (test_server_started) return;
    test_server_started = 1;

    inject_remote_control_banner(root_handle);
    aether_ui_test_server_start(port, &macos_driver_hooks);
}

// ---------------------------------------------------------------------------
// Menus (NSMenu / NSMenuItem).
// Minimal native implementation — the app menu is mutated via NSApp's
// mainMenu; context menus use -[NSMenu popUpMenuPositioningItem:].
// ---------------------------------------------------------------------------

typedef struct {
    NSMenu*     menu;
    NSString*   label;
    int         is_bar;
} MacMenuEntry;

static MacMenuEntry* mac_menus = NULL;
static int           mac_menu_count = 0;
static int           mac_menu_capacity = 0;

static int register_mac_menu(NSMenu* menu, NSString* label, int is_bar) {
    if (mac_menu_count >= mac_menu_capacity) {
        mac_menu_capacity = mac_menu_capacity == 0 ? 8 : mac_menu_capacity * 2;
        mac_menus = (MacMenuEntry*)realloc(mac_menus,
            sizeof(MacMenuEntry) * mac_menu_capacity);
    }
    mac_menus[mac_menu_count].menu = menu;
    mac_menus[mac_menu_count].label = [label copy];
    mac_menus[mac_menu_count].is_bar = is_bar;
    mac_menu_count++;
    return mac_menu_count;
}

// Target/action plumbing for menu items. Each menu item stores its boxed
// closure pointer; the shared target invokes it on click. Declared up with the
// context-menu code (which needs it first) — this is its implementation, and
// it serves both the menubar and per-widget right-click menus.
@implementation AetherMenuTarget
- (void)fire:(id)sender {
    AeClosure* c = (AeClosure*)(intptr_t)[[sender representedObject] longLongValue];
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
}
@end

int aether_ui_menu_bar_create(void) {
    if (!g_menu_target) g_menu_target = [[AetherMenuTarget alloc] init];
    return register_mac_menu([[NSMenu alloc] initWithTitle:@""], @"", 1);
}

int aether_ui_menu_create(const char* label) {
    if (!g_menu_target) g_menu_target = [[AetherMenuTarget alloc] init];
    NSString* ns_label = [NSString stringWithUTF8String:(label ? label : "Menu")];
    return register_mac_menu([[NSMenu alloc] initWithTitle:ns_label], ns_label, 0);
}

void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure) {
    if (menu_handle < 1 || menu_handle > mac_menu_count) return;
    NSMenu* m = mac_menus[menu_handle - 1].menu;
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithUTF8String:(label ? label : "")]
        action:@selector(fire:) keyEquivalent:@""];
    [item setTarget:g_menu_target];
    [item setRepresentedObject:[NSNumber numberWithLongLong:(intptr_t)boxed_closure]];
    [m addItem:item];
    // Side-store for the AetherUIDriver /tray/{id}/menu/activate route.
    aether_ui_menu_item_record(menu_handle, label, boxed_closure);
}

void aether_ui_menu_add_separator(int menu_handle) {
    if (menu_handle < 1 || menu_handle > mac_menu_count) return;
    [mac_menus[menu_handle - 1].menu addItem:[NSMenuItem separatorItem]];
}

void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    if (bar_handle < 1 || bar_handle > mac_menu_count) return;
    if (menu_handle < 1 || menu_handle > mac_menu_count) return;
    NSMenu* bar = mac_menus[bar_handle - 1].menu;
    NSMenu* sub = mac_menus[menu_handle - 1].menu;
    NSString* sub_label = mac_menus[menu_handle - 1].label;
    NSMenuItem* host = [[NSMenuItem alloc] initWithTitle:sub_label
                                                  action:nil keyEquivalent:@""];
    [host setSubmenu:sub];
    [bar addItem:host];
}

void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    (void)app_handle;
    if (bar_handle < 1 || bar_handle > mac_menu_count) return;
    [NSApp setMainMenu:mac_menus[bar_handle - 1].menu];
}

// Per-window menu bar: macOS has ONE OS-level menu bar (the top-of-screen
// strip), reflecting the active app/window — there is no per-window menu bar
// widget as on GTK4/win32. Best-effort: set this bar as the main menu (it
// applies while any of our windows is active). A fully window-specific bar
// would require swapping the main menu on window-focus changes.
// TODO(multi-window): swap the main menu in windowDidBecomeKey per window.
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) {
    (void)win_handle;
    if (bar_handle < 1 || bar_handle > mac_menu_count) return;
    [NSApp setMainMenu:mac_menus[bar_handle - 1].menu];
}

void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    if (menu_handle < 1 || menu_handle > mac_menu_count) return;
    // popUpMenuPositioningItem tracks the menu in its own loop until
    // dismissed. With no NSApp run-loop active (widget smoke tests,
    // any headless caller) the loop can fail to dismiss and block the
    // caller. Respect AETHER_UI_HEADLESS unconditionally.
    if (aeui_is_headless()) return;
    NSView* anchor = (__bridge NSView*)aether_ui_get_widget(anchor_widget);
    NSMenu* m = mac_menus[menu_handle - 1].menu;
    NSPoint loc = [NSEvent mouseLocation];
    // `inView:` requires the view to be attached to a window; otherwise
    // Cocoa throws NSInternalInconsistencyException. When the anchor
    // has no window, fall back to screen-space positioning with
    // `inView:nil`, which popUpMenuPositioningItem explicitly documents
    // as supported.
    NSView* viewArg = (anchor && anchor.window) ? anchor : nil;
    if (viewArg) {
        loc = [viewArg.window convertPointFromScreen:loc];
    }
    [m popUpMenuPositioningItem:nil atLocation:loc inView:viewArg];
}

// ---------------------------------------------------------------------------
// Grid layout (NSGridView).
// ---------------------------------------------------------------------------
int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    NSGridView* grid = [NSGridView gridViewWithNumberOfColumns:cols rows:0];
    grid.rowSpacing = row_spacing;
    grid.columnSpacing = col_spacing;
    return aether_ui_register_widget((__bridge_retained void*)grid);
}

void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span) {
    NSGridView* grid = (__bridge NSGridView*)aether_ui_get_widget(grid_handle);
    NSView* child = (__bridge NSView*)aether_ui_get_widget(child_handle);
    if (!grid || !child) return;
    // Extend rows/cols if needed. NSGridView's row-append selector is
    // `addRowWithViews:` — `addRow:` does not exist.
    while (grid.numberOfRows <= row) [grid addRowWithViews:@[]];
    NSGridCell* cell = [grid cellAtColumnIndex:col rowIndex:row];
    [cell setContentView:child];
    if (row_span > 1 || col_span > 1) {
        [grid mergeCellsInHorizontalRange:NSMakeRange(col, col_span)
                             verticalRange:NSMakeRange(row, row_span)];
    }
}

// ---------------------------------------------------------------------------
// Reverse lookup — aether_ui_handle_for_widget.
// Backend-specific: this is a linear scan over the widget registry. The
// hash-backed O(1) version ships in the Win32 backend; porting to AppKit
// is straightforward future work (NSView* maps cleanly to the same hash).
// ---------------------------------------------------------------------------
int aether_ui_handle_for_widget(void* widget) {
    if (!widget) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == widget) return i + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// System tray (Group 7) — registry-only stub.
//
// Real implementation should use NSStatusItem + NSStatusBar.system. On
// click of the status item, call aether_ui_tray_emit_click(id). For
// tray_set_menu, set [statusItem setMenu:] with an NSMenu built from
// the menu_handle's items. tray_set_icon_template maps to
// [statusItem.button.image setTemplate:YES] for adaptive light/dark.
//
// Cannot be authored here without a macOS host to test against; the
// registry + driver routes still validate the callback wiring from
// AvnSync v2's perspective.
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
// Desktop notifications (Group 7b) — NSUserNotification.
//
// NSUserNotification is deprecated since 10.14, but it is the only
// notification API that works from an un-bundled CLI tool:
// UNUserNotificationCenter requires a real signed .app bundle, which the
// aether-ui apps aren't. It is exactly what the win32/toast + gtk4/libnotify
// paths are the peers of. The whole section is wrapped in a deprecation
// pragma so the -Werror build stays green.
//
// The registry (aether_ui_notify_register_full) owns the per-notification
// state and the click closure; we register first (mirroring the gtk4 path:
// register -> show), then deliver an NSUserNotification carrying its id in
// userInfo. On activation the delegate resolves that id and calls
// aether_ui_notif_emit_click, which invokes the stored closure on the main
// thread (delegate callbacks are delivered on the main thread).
// ---------------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@interface AeUINotifDelegate : NSObject <NSUserNotificationCenterDelegate>
@end
@implementation AeUINotifDelegate
// AppKit suppresses a notification when our process is frontmost; force it
// to show so behaviour matches libnotify/toast (which always surface).
- (BOOL)userNotificationCenter:(NSUserNotificationCenter*)center
     shouldPresentNotification:(NSUserNotification*)notification {
    (void)center; (void)notification;
    return YES;
}
- (void)userNotificationCenter:(NSUserNotificationCenter*)center
       didActivateNotification:(NSUserNotification*)notification {
    (void)center;
    NSNumber* aeid = [notification.userInfo objectForKey:@"aeid"];
    if (aeid) aether_ui_notif_emit_click((int)[aeid intValue]);
}
@end

// NSUserNotificationCenter.delegate is a weak reference, so the delegate must
// be owned elsewhere or ARC frees it immediately. This static strong ref
// keeps the single shared delegate alive for the process lifetime.
static AeUINotifDelegate* g_ae_notif_delegate = nil;

// Deliver one NSUserNotification for a registry id, on the main thread
// (NSUserNotificationCenter is main-thread-only). Convert every C string to
// an NSString HERE, on the calling thread, before the async hop: the caller's
// `const char*`s may be freed the moment we return, so the block must capture
// ARC-retained NSStrings (which copy the bytes), never the raw pointers.
// Fire-and-forget: async is fine because the id was already assigned
// synchronously by the caller.
static void aeui_deliver_notification(int notif_id, const char* title,
                                      const char* body, const char* icon_path,
                                      const char* tag) {
    NSString* ns_title = title ? [NSString stringWithUTF8String:title] : @"";
    NSString* ns_body  = body  ? [NSString stringWithUTF8String:body]  : @"";
    NSString* ns_icon  = (icon_path && *icon_path)
                             ? [NSString stringWithUTF8String:icon_path] : nil;
    NSString* ns_tag   = (tag && *tag)
                             ? [NSString stringWithUTF8String:tag] : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSUserNotificationCenter* center =
            [NSUserNotificationCenter defaultUserNotificationCenter];
        if (!g_ae_notif_delegate) {
            g_ae_notif_delegate = [[AeUINotifDelegate alloc] init];
            center.delegate = g_ae_notif_delegate;
        }
        NSUserNotification* un = [[NSUserNotification alloc] init];
        un.title = ns_title;
        un.informativeText = ns_body;
        if (ns_icon) {
            NSImage* img = [[NSImage alloc] initWithContentsOfFile:ns_icon];
            if (img) un.contentImage = img;
        }
        // A same-tag notification replaces the prior one via the identifier,
        // mirroring the registry's tag-based slot reuse.
        if (ns_tag) un.identifier = ns_tag;
        un.userInfo = @{ @"aeid": [NSNumber numberWithInt:notif_id] };
        [center deliverNotification:un];
    });
}

int aether_ui_notify_impl(const char* title, const char* body) {
    int id = aether_ui_notify_register(title, body);
    if (id > 0) aeui_deliver_notification(id, title, body, NULL, NULL);
    return id;
}
int aether_ui_notify_full_impl(const char* title, const char* body,
                                const char* icon_path, const char* tag,
                                void* boxed_click) {
    int id = aether_ui_notify_register_full(title, body, icon_path, tag, boxed_click);
    if (id > 0) aeui_deliver_notification(id, title, body, icon_path, tag);
    return id;
}
int aether_ui_notify_request_permission_impl(void) {
    // NSUserNotification needs no runtime permission grant (unlike
    // UNUserNotificationCenter); report granted so app code doesn't block.
    return aether_ui_notify_request_permission();
}

#pragma clang diagnostic pop

// app_run_headless on macOS: park until killed. A future native
// NSStatusItem implementation should call [NSApp run] here when
// the tray is registered, since AppKit needs the main runloop to
// deliver click events to the status item.
void aether_ui_app_run_headless_impl(void) {
    aether_ui_park_until_killed();
}

#endif // __APPLE__

// Stage 3 per-frame surfaces: NOT IMPLEMENTED on this backend yet.
// Returning 0 is the gate -- ui.frames reads it as "cannot cache here" and
// falls back to drawing every frame inline, which is exactly today's correct
// behaviour. Deliberately not stubbed with something plausible-but-wrong:
// a backend that half-caches would leave stale pixels on screen.
// Porting means reusing this file's existing offscreen replay (the one
// write_png_impl already uses) and copying out non-premultiplied RGBA8.
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
