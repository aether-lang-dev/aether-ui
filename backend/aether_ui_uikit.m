// Aether UI — iOS / iPadOS UIKit backend for Aether
// ===========================================================================
//
// The fourth native implementation of the shared backend ABI
// (backend/aether_ui_backend.h), alongside GTK4 (Linux/FreeBSD), AppKit
// (macOS) and Win32 (Windows). The Aether module.ae is platform-agnostic —
// only the backend changes.
//
// It is a SIBLING of aether_ui_macos.m, not a fork of it: UIKit is not AppKit.
// NSView→UIView, NSWindow→UIWindow+rootViewController, NSApplication's run
// loop→UIApplicationMain, NSStackView→UIStackView (the arranged-subview and
// Auto-Layout model carry over almost 1:1). The lossy desktop idioms (hover,
// right-click menus, resizable multi-window) are re-expressed through the iPad
// affordances — pointer interactions, UIMenu, scenes — behind a
// UIUserInterfaceIdiomPad branch as those sections get ported.
//
// STATUS (pass 1): the lifecycle, widget registry, stack layout and the core
// widgets (text, button, textfield/securefield, toggle, slider) are REAL. The
// rest of the 287-function ABI is present as honest, compiling `// TODO(ios)`
// stubs at the foot of the file, so the backend links and is gated by the iOS
// SDK compile check in ci.sh. Each later pass moves a section out of the stub
// block into a real implementation, exactly as the Win32/AppKit backends grew.
//
// Build (simulator, from ci.sh Phase 1e):
//   clang -fobjc-arc -target arm64-apple-ios-simulator -isysroot <SimSDK> \
//         -c backend/aether_ui_uikit.m -Ibackend
//   link: -framework UIKit -framework Foundation -framework QuartzCore \
//         -framework CoreText -framework ImageIO
// ===========================================================================

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "aether_ui_backend.h"

// ---------------------------------------------------------------------------
// Closure struct — must match Aether codegen's _AeClosure layout (identical to
// the AppKit/Win32 backends). A boxed closure is called by casting its fn to
// the arity the call site needs and passing env first.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

// AETHER_UI_HEADLESS — set by CI / smoke tests. Suppresses anything that would
// spin a modal/user-input loop with no user present.
static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

// Widget type tags — mirror of the AppKit backend's, used at creation for
// readability and by the (later) driver type-reporting. Pass 1 does not store
// them; kept so widget factories read the same as their AppKit siblings.
enum {
    AUI_UNKNOWN = 0,
    AUI_TEXT, AUI_BUTTON, AUI_TOGGLE, AUI_SLIDER, AUI_PICKER,
    AUI_TEXTFIELD, AUI_SECUREFIELD, AUI_TEXTAREA,
    AUI_PROGRESSBAR, AUI_DIVIDER, AUI_SCROLLVIEW,
    AUI_VSTACK, AUI_HSTACK, AUI_ZSTACK, AUI_SPACER,
    AUI_CANVAS, AUI_IMAGE
};

// ---------------------------------------------------------------------------
// Widget registry — flat strong array of UIView*, 1-based handles (as AppKit).
// ---------------------------------------------------------------------------
static UIView* __strong *widgets = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

static int register_widget_typed(void* widget, int type) {
    (void)type;  // pass 1 does not track types (driver type-report is stubbed)
    if (widget_count >= widget_capacity) {
        int new_cap = widget_capacity == 0 ? 64 : widget_capacity * 2;
        UIView* __strong *nw = (__strong UIView**)calloc(new_cap, sizeof(UIView*));
        if (widgets) {
            for (int i = 0; i < widget_count; i++) nw[i] = widgets[i];
            free(widgets);
        }
        widgets = nw;
        widget_capacity = new_cap;
    }
    widgets[widget_count] = (__bridge UIView*)widget;
    widget_count++;
    return widget_count;
}

int aether_ui_register_widget(void* widget) {
    return register_widget_typed(widget, AUI_UNKNOWN);
}

void* aether_ui_get_widget(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return (__bridge void*)widgets[handle - 1];
}

int aether_ui_handle_for_widget(void* widget) {
    if (!widget) return 0;
    UIView* v = (__bridge UIView*)widget;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == v) return i + 1;
    }
    return 0;
}

const char* aether_ui_backend_name_impl(void) { return "uikit"; }

// ---------------------------------------------------------------------------
// Target trampolines — a UIControl's target is an unretained pointer, so each
// closure-bearing target is parked in this array for the life of the app
// (mirrors the AppKit backend's retain_target).
// ---------------------------------------------------------------------------
static NSMutableArray* g_targets = nil;
static void retain_target(id t) {
    if (!g_targets) g_targets = [NSMutableArray array];
    if (t) [g_targets addObject:t];
}

@interface AeuiButtonTarget : NSObject
@property (nonatomic, assign) AeClosure* closure;
@end
@implementation AeuiButtonTarget
- (void)fire {
    if (self.closure && self.closure->fn)
        ((void(*)(void*))self.closure->fn)(self.closure->env);
}
@end

@interface AeuiToggleTarget : NSObject
@property (nonatomic, assign) AeClosure* closure;
@end
@implementation AeuiToggleTarget
- (void)changed:(UISwitch*)sw {
    if (self.closure && self.closure->fn)
        ((void(*)(void*, intptr_t))self.closure->fn)(
            self.closure->env, (intptr_t)(sw.isOn ? 1 : 0));
}
@end

@interface AeuiSliderTarget : NSObject
@property (nonatomic, assign) AeClosure* closure;
@end
@implementation AeuiSliderTarget
- (void)changed:(UISlider*)s {
    if (self.closure && self.closure->fn)
        ((void(*)(void*, double))self.closure->fn)(self.closure->env, (double)s.value);
}
@end

@interface AeuiFieldTarget : NSObject
@property (nonatomic, assign) AeClosure* closure;
@end
@implementation AeuiFieldTarget
- (void)changed:(UITextField*)f {
    if (self.closure && self.closure->fn) {
        const char* cs = f.text ? f.text.UTF8String : "";
        ((void(*)(void*, const char*))self.closure->fn)(self.closure->env, cs);
    }
}
@end

// ---------------------------------------------------------------------------
// App lifecycle — one UIWindow + a root UIViewController hosting the body view.
// ---------------------------------------------------------------------------
static int g_root_handle = 0;
static int g_want_w = 0;
static int g_want_h = 0;

@interface AetherAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow* window;
@end
@implementation AetherAppDelegate
- (BOOL)application:(UIApplication*)application
        didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    (void)application; (void)launchOptions;
    CGRect bounds = CGRectMake(0, 0,
                               g_want_w > 0 ? g_want_w : 390,
                               g_want_h > 0 ? g_want_h : 844);
    self.window = [[UIWindow alloc] initWithFrame:bounds];
    UIViewController* vc = [[UIViewController alloc] init];
    vc.view.backgroundColor = [UIColor systemBackgroundColor];

    UIView* root = (__bridge UIView*)aether_ui_get_widget(g_root_handle);
    if (root) {
        root.translatesAutoresizingMaskIntoConstraints = NO;
        [vc.view addSubview:root];
        UILayoutGuide* g = vc.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [root.topAnchor constraintEqualToAnchor:g.topAnchor],
            [root.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
            [root.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
            [root.bottomAnchor constraintLessThanOrEqualToAnchor:g.bottomAnchor],
        ]];
    }
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int aether_ui_app_create(const char* title, int width, int height) {
    (void)title;  // iOS has no window title chrome
    g_want_w = width;
    g_want_h = height;
    return 1;
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    (void)app_handle;
    g_root_handle = root_handle;
}

void aether_ui_app_run_raw(int app_handle) {
    (void)app_handle;
    // Under headless CI there is no host to run a UIApplication against, and the
    // compile gate never reaches here — so return rather than block.
    if (aeui_is_headless()) return;
    @autoreleasepool {
        char arg0[] = "aether-ui";
        char* argv[] = { arg0, NULL };
        UIApplicationMain(1, argv, nil,
                          NSStringFromClass([AetherAppDelegate class]));
    }
}

void aether_ui_app_run_headless_impl(void) { /* no UI loop under headless */ }

// ---------------------------------------------------------------------------
// Surface table — platform-agnostic handle bookkeeping (mirrors AppKit/GTK4).
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
    for (int i = 0; i < surface_count; i++)
        if (surfaces[i].container_handle == container_handle) return &surfaces[i];
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

// Deferred-flush registry — mirrors AppKit/GTK4 so the AeVG deferred-colour
// path flushes identically.
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
        if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
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
    // Auto-start the driver test server when AETHER_UI_TEST_PORT is set, as the
    // AppKit/Win32/GTK4 run loops do — the window(){} block form owns the loop.
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
// Stack layout — UIStackView is the direct UIKit analogue of NSStackView.
// alignment=Fill makes arranged children fill the cross axis (what the AppKit
// backend pins by hand); distribution=Fill lets spacers absorb slack.
// ---------------------------------------------------------------------------
static UIStackView* make_stack(UILayoutConstraintAxis axis, int spacing) {
    UIStackView* stack = [[UIStackView alloc] init];
    stack.axis = axis;
    stack.spacing = (CGFloat)spacing;
    stack.alignment = UIStackViewAlignmentFill;
    stack.distribution = UIStackViewDistributionFill;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    return stack;
}

int aether_ui_vstack_create(int spacing) {
    return register_widget_typed(
        (__bridge void*)make_stack(UILayoutConstraintAxisVertical, spacing),
        AUI_VSTACK);
}

int aether_ui_hstack_create(int spacing) {
    return register_widget_typed(
        (__bridge void*)make_stack(UILayoutConstraintAxisHorizontal, spacing),
        AUI_HSTACK);
}

int aether_ui_spacer_create(void) {
    UIView* v = [[UIView alloc] init];
    v.translatesAutoresizingMaskIntoConstraints = NO;
    // Low hugging + low compression resistance → the spacer soaks up slack.
    [v setContentHuggingPriority:UILayoutPriorityDefaultLow - 1
                         forAxis:UILayoutConstraintAxisHorizontal];
    [v setContentHuggingPriority:UILayoutPriorityDefaultLow - 1
                         forAxis:UILayoutConstraintAxisVertical];
    [v setContentCompressionResistancePriority:UILayoutPriorityDefaultLow - 1
                                       forAxis:UILayoutConstraintAxisHorizontal];
    [v setContentCompressionResistancePriority:UILayoutPriorityDefaultLow - 1
                                       forAxis:UILayoutConstraintAxisVertical];
    return register_widget_typed((__bridge void*)v, AUI_SPACER);
}

int aether_ui_divider_create(void) {
    UIView* v = [[UIView alloc] init];
    v.translatesAutoresizingMaskIntoConstraints = NO;
    v.backgroundColor = [UIColor separatorColor];
    [v.heightAnchor constraintEqualToConstant:1.0].active = YES;
    return register_widget_typed((__bridge void*)v, AUI_DIVIDER);
}

void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    UIView* parent = (__bridge UIView*)aether_ui_get_widget(parent_handle);
    UIView* child = (__bridge UIView*)aether_ui_get_widget(child_handle);
    if (!parent || !child) return;
    if ([parent isKindOfClass:[UIStackView class]]) {
        [(UIStackView*)parent addArrangedSubview:child];
    } else {
        [parent addSubview:child];
    }
}

void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    (void)parent_handle;
    UIView* child = (__bridge UIView*)aether_ui_get_widget(child_handle);
    if (child) [child removeFromSuperview];  // also removes it as arranged
}

void aether_ui_clear_children_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    NSArray<UIView*>* kids = [v.subviews copy];
    for (UIView* k in kids) [k removeFromSuperview];
}

// ---------------------------------------------------------------------------
// Text — UILabel.
// ---------------------------------------------------------------------------
int aether_ui_text_create(const char* text) {
    UILabel* label = [[UILabel alloc] init];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.text = [NSString stringWithUTF8String:text ? text : ""];
    label.numberOfLines = 1;
    return register_widget_typed((__bridge void*)label, AUI_TEXT);
}

int aether_ui_text_wrapped_create(const char* text, int wrap_width_px) {
    UILabel* label = [[UILabel alloc] init];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.text = [NSString stringWithUTF8String:text ? text : ""];
    label.numberOfLines = 0;
    if (wrap_width_px > 0) {
        label.preferredMaxLayoutWidth = (CGFloat)wrap_width_px;
        [label.widthAnchor constraintEqualToConstant:(CGFloat)wrap_width_px].active = YES;
    }
    return register_widget_typed((__bridge void*)label, AUI_TEXT);
}

void aether_ui_text_set_anchor(int handle, int anchor) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UILabel class]]) return;
    NSTextAlignment a = anchor == 1 ? NSTextAlignmentCenter
                      : anchor == 2 ? NSTextAlignmentRight
                                    : NSTextAlignmentLeft;
    ((UILabel*)v).textAlignment = a;
}

// ---------------------------------------------------------------------------
// Button — UIButton (system). Closure fires on .TouchUpInside, no args.
// ---------------------------------------------------------------------------
static UIButton* make_button(const char* label) {
    UIButton* btn = [UIButton buttonWithType:UIButtonTypeSystem];
    btn.translatesAutoresizingMaskIntoConstraints = NO;
    [btn setTitle:[NSString stringWithUTF8String:label ? label : ""]
         forState:UIControlStateNormal];
    return btn;
}

static void wire_button(UIButton* btn, void* boxed_closure) {
    if (!boxed_closure) return;
    AeuiButtonTarget* t = [[AeuiButtonTarget alloc] init];
    t.closure = (AeClosure*)boxed_closure;
    [btn addTarget:t action:@selector(fire)
        forControlEvents:UIControlEventTouchUpInside];
    retain_target(t);
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    UIButton* btn = make_button(label);
    wire_button(btn, boxed_closure);
    return register_widget_typed((__bridge void*)btn, AUI_BUTTON);
}

int aether_ui_button_create_plain(const char* label) {
    return register_widget_typed((__bridge void*)make_button(label), AUI_BUTTON);
}

void aether_ui_button_set_label(int handle, const char* label) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UIButton class]])
        [(UIButton*)v setTitle:[NSString stringWithUTF8String:label ? label : ""]
                      forState:UIControlStateNormal];
}

void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    if ([v isKindOfClass:[UIButton class]])
        wire_button((UIButton*)v, boxed_closure);
    // Non-button widgets get a tap gesture in a later pass (aether_ui_on_click).
}

// ---------------------------------------------------------------------------
// Text fields — UITextField, closure fires the changed text on EditingChanged.
// ---------------------------------------------------------------------------
static UITextField* make_field(const char* placeholder, void* boxed_closure,
                               BOOL secure) {
    UITextField* f = [[UITextField alloc] init];
    f.translatesAutoresizingMaskIntoConstraints = NO;
    f.borderStyle = UITextBorderStyleRoundedRect;
    f.secureTextEntry = secure;
    if (placeholder && *placeholder)
        f.placeholder = [NSString stringWithUTF8String:placeholder];
    if (boxed_closure) {
        AeuiFieldTarget* t = [[AeuiFieldTarget alloc] init];
        t.closure = (AeClosure*)boxed_closure;
        [f addTarget:t action:@selector(changed:)
            forControlEvents:UIControlEventEditingChanged];
        retain_target(t);
    }
    return f;
}

int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    return register_widget_typed(
        (__bridge void*)make_field(placeholder, boxed_closure, NO), AUI_TEXTFIELD);
}

int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    return register_widget_typed(
        (__bridge void*)make_field(placeholder, boxed_closure, YES), AUI_SECUREFIELD);
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UITextField class]])
        ((UITextField*)v).text = [NSString stringWithUTF8String:text ? text : ""];
}

const char* aether_ui_textfield_get_text(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UITextField class]]) {
        UITextField* f = (UITextField*)v;
        return f.text ? f.text.UTF8String : "";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Toggle — UISwitch. Closure fires 0/1 as intptr_t. (UISwitch carries no
// title; a labelled row is composed at the DSL level, as on iOS generally.)
// ---------------------------------------------------------------------------
int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    (void)label;
    UISwitch* sw = [[UISwitch alloc] init];
    sw.translatesAutoresizingMaskIntoConstraints = NO;
    if (boxed_closure) {
        AeuiToggleTarget* t = [[AeuiToggleTarget alloc] init];
        t.closure = (AeClosure*)boxed_closure;
        [sw addTarget:t action:@selector(changed:)
            forControlEvents:UIControlEventValueChanged];
        retain_target(t);
    }
    return register_widget_typed((__bridge void*)sw, AUI_TOGGLE);
}

void aether_ui_toggle_set_active(int handle, int active) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UISwitch class]])
        [(UISwitch*)v setOn:(active != 0)];
}

int aether_ui_toggle_get_active(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UISwitch class]]) return ((UISwitch*)v).isOn ? 1 : 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Slider — UISlider. Closure fires the value as a double on change.
// ---------------------------------------------------------------------------
int aether_ui_slider_create(double min_val, double max_val, double initial,
                            void* boxed_closure) {
    UISlider* s = [[UISlider alloc] init];
    s.translatesAutoresizingMaskIntoConstraints = NO;
    s.minimumValue = (float)min_val;
    s.maximumValue = (float)max_val;
    s.value = (float)initial;
    if (boxed_closure) {
        AeuiSliderTarget* t = [[AeuiSliderTarget alloc] init];
        t.closure = (AeClosure*)boxed_closure;
        [s addTarget:t action:@selector(changed:)
            forControlEvents:UIControlEventValueChanged];
        retain_target(t);
    }
    return register_widget_typed((__bridge void*)s, AUI_SLIDER);
}

void aether_ui_slider_set_value(int handle, double value) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UISlider class]]) ((UISlider*)v).value = (float)value;
}

double aether_ui_slider_get_value(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UISlider class]]) return (double)((UISlider*)v).value;
    return 0.0;
}

// ===========================================================================
// UNIMPLEMENTED — pass 1 stubs.
//
// Every remaining ABI entry the app can call, defined so the backend links.
// Each returns a safe default and is a TODO for a later pass that ports the
// corresponding AppKit section of aether_ui_macos.m to UIKit. They are
// grouped only by return type; the real work moves them OUT of this block
// into proper sections above, exactly as the Win32/AppKit backends grew.
// ===========================================================================

void aether_ui_a11y_get_impl(int handle, char* role, int rolesz, char* name, int namesz, char* desc, int descsz) { }  // TODO(ios)
void aether_ui_a11y_set_description_impl(int handle, const char* desc) { }  // TODO(ios)
void aether_ui_a11y_set_label_impl(int handle, const char* name) { }  // TODO(ios)
void aether_ui_a11y_set_role_impl(int handle, const char* role) { }  // TODO(ios)
void aether_ui_alert_impl(const char* title, const char* message) { }  // TODO(ios)
void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert) { }  // TODO(ios)
void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert) { }  // TODO(ios)
void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals) { }  // TODO(ios)
void aether_ui_bind_value(int state_handle, int widget_handle) { }  // TODO(ios)
void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius, double start_angle, double end_angle) { }  // TODO(ios)
void aether_ui_canvas_begin_path_impl(int canvas_id) { }  // TODO(ios)
void aether_ui_canvas_clear_impl(int canvas_id) { }  // TODO(ios)
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y, double w, double h) { }  // TODO(ios)
void aether_ui_canvas_close_path_impl(int canvas_id) { }  // TODO(ios)
int aether_ui_canvas_cmd_count_impl(int canvas_id) { return 0; }  // TODO(ios)
int aether_ui_canvas_create_impl(int width, int height) { return 0; }  // TODO(ios)
void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y, int iw, int ih, const unsigned char* rgba, int byte_len) { }  // TODO(ios)
void aether_ui_canvas_draw_image_impl_ptr(int canvas_id, double x, double y, int iw, int ih, const unsigned char* rgba, int byte_len) { }  // TODO(ios)
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y, double dw, double dh, int iw, int ih, const unsigned char* rgba, int byte_len) { }  // TODO(ios)
void aether_ui_canvas_draw_image_scaled_impl_ptr(int canvas_id, double x, double y, double dw, double dh, int iw, int ih, const unsigned char* rgba, int byte_len) { }  // TODO(ios)
void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a, int even_odd) { }  // TODO(ios)
void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id, double x1, double y1, double x2, double y2, int n_stops, void* offsets, void* rgba, double line_width, int extend, int cap, int join) { }  // TODO(ios)
void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id, double cx, double cy, double radius, double fx, double fy, int n_stops, void* offsets, void* rgba, double line_width, int extend, int cap, int join, double rx, double ry, double rot_deg) { }  // TODO(ios)
void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y, double w, double h, double r, double g, double b, double a) { }  // TODO(ios)
void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text, double x, double y, double font_size, int font_flags, const char* font_family, double r, double g, double b, double a) { }  // TODO(ios)
void aether_ui_canvas_gesture_probe_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
int aether_ui_canvas_get_widget(int canvas_id) { return 0; }  // TODO(ios)
void aether_ui_canvas_group_begin_impl(int canvas_id) { }  // TODO(ios)
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) { }  // TODO(ios)
void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) { }  // TODO(ios)
void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) { }  // TODO(ios)
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
void aether_ui_canvas_on_key_release_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
void aether_ui_canvas_on_scroll_impl(int canvas_id, void* boxed_closure) { }  // TODO(ios)
int aether_ui_canvas_painted_pixels_impl(int canvas_id) { return 0; }  // TODO(ios)
int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py, int width, int height) { return 0; }  // TODO(ios)
void aether_ui_canvas_redraw_impl(int canvas_id) { }  // TODO(ios)
int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end, double ox, double oy, int width, int height, unsigned char* out, int out_len) { return 0; }  // TODO(ios)
void aether_ui_canvas_reset_clip_impl(int canvas_id) { }  // TODO(ios)
void aether_ui_canvas_set_clip_rects_impl(int canvas_id, void* rects, int n) { }  // TODO(ios)
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b, double a, double line_width, int cap, int join) { }  // TODO(ios)
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text, double x, double y, double font_size, double line_width, int font_flags, const char* font_family, double r, double g, double b, double a) { }  // TODO(ios)
int aether_ui_canvas_write_png_impl(int canvas_id, const char* path, int width, int height) { return 0; }  // TODO(ios)
char* aether_ui_clipboard_read_impl(void) { return (void*)0; }  // TODO(ios)
void aether_ui_clipboard_write_impl(const char* text) { }  // TODO(ios)
void aether_ui_close_window_by_handle_impl(int win_handle) { }  // TODO(ios)
void aether_ui_context_menu_item_accel_impl(int handle, const char* label, const char* accel, void* boxed_closure) { }  // TODO(ios)
void aether_ui_context_menu_item_impl(int handle, const char* label, void* boxed_closure) { }  // TODO(ios)
int aether_ui_dark_mode_check(void) { return 0; }  // TODO(ios)
void aether_ui_enable_test_server_ctx(int port, void* ctx) { }  // TODO(ios)
void aether_ui_enable_test_server_impl(int port, int root_handle) { }  // TODO(ios)
int aether_ui_file_icon_create(const char* path) { return 0; }  // TODO(ios)
void aether_ui_file_icon_set(int handle, const char* path) { }  // TODO(ios)
char* aether_ui_file_open(const char* title, const char* start_dir) { return (void*)0; }  // TODO(ios)
char* aether_ui_file_pick_folder(const char* title, const char* start_dir) { return (void*)0; }  // TODO(ios)
char* aether_ui_file_save(const char* title, const char* default_name) { return (void*)0; }  // TODO(ios)
int aether_ui_fire_appearance(int dark) { return 0; }  // TODO(ios)
int aether_ui_fire_double_click(int handle) { return 0; }  // TODO(ios)
int aether_ui_fire_redo(void) { return 0; }  // TODO(ios)
int aether_ui_fire_row_drop(int row_handle, int src_index) { return 0; }  // TODO(ios)
int aether_ui_fire_scroll(int container_handle, int dy) { return 0; }  // TODO(ios)
int aether_ui_fire_undo(void) { return 0; }  // TODO(ios)
void aether_ui_focus_impl(int handle) { }  // TODO(ios)
int aether_ui_focused_widget(void) { return 0; }  // TODO(ios)
double aether_ui_font_ascent(double size) { return 0.0; }  // TODO(ios)
double aether_ui_font_descent(double size) { return 0.0; }  // TODO(ios)
double aether_ui_font_height(double size) { return 0.0; }  // TODO(ios)
int aether_ui_form_create(void) { return 0; }  // TODO(ios)
int aether_ui_form_section_create(const char* title) { return 0; }  // TODO(ios)
int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) { return 0; }  // TODO(ios)
void aether_ui_grid_place(int grid_handle, int child_handle, int row, int col, int row_span, int col_span) { }  // TODO(ios)
int aether_ui_image_create(const char* filepath) { return 0; }  // TODO(ios)
int aether_ui_image_from_bytes(const char* data, int length) { return 0; }  // TODO(ios)
int aether_ui_image_get_fill(int handle) { return 0; }  // TODO(ios)
int aether_ui_image_get_tint(int handle) { return 0; }  // TODO(ios)
int aether_ui_image_has_content(int handle) { return 0; }  // TODO(ios)
void aether_ui_image_set_fill(int handle, int mode) { }  // TODO(ios)
void aether_ui_image_set_size(int handle, int width, int height) { }  // TODO(ios)
void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) { }  // TODO(ios)
void aether_ui_match_parent_height(int handle) { }  // TODO(ios)
void aether_ui_match_parent_width(int handle) { }  // TODO(ios)
void aether_ui_menu_add_item(int menu_handle, const char* label, void* boxed_closure) { }  // TODO(ios)
void aether_ui_menu_add_separator(int menu_handle) { }  // TODO(ios)
void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) { }  // TODO(ios)
void aether_ui_menu_bar_attach(int app_handle, int bar_handle) { }  // TODO(ios)
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) { }  // TODO(ios)
int aether_ui_menu_bar_create(void) { return 0; }  // TODO(ios)
int aether_ui_menu_create(const char* label) { return 0; }  // TODO(ios)
void aether_ui_menu_popup(int menu_handle, int anchor_widget) { }  // TODO(ios)
int aether_ui_navstack_create(void) { return 0; }  // TODO(ios)
int aether_ui_navstack_depth(int handle) { return 0; }  // TODO(ios)
void aether_ui_navstack_pop(int handle) { }  // TODO(ios)
void aether_ui_navstack_push(int handle, const char* title, int body_handle) { }  // TODO(ios)
int aether_ui_notify_full_impl(const char* title, const char* body, const char* icon_path, const char* tag, void* boxed_click) { return 0; }  // TODO(ios)
int aether_ui_notify_impl(const char* title, const char* body) { return 0; }  // TODO(ios)
int aether_ui_notify_request_permission_impl(void) { return 0; }  // TODO(ios)
void aether_ui_on_click_impl(int handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_on_double_click_impl(int handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_on_hover_impl(int handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_on_layout_impl(int handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_open_url_impl(const char* url) { }  // TODO(ios)
void aether_ui_overlay_close_impl(int overlay_handle) { }  // TODO(ios)
int aether_ui_overlay_count_impl(void) { return 0; }  // TODO(ios)
int aether_ui_overlay_exit_played_impl(int overlay_handle) { return 0; }  // TODO(ios)
int aether_ui_overlay_is_exiting_impl(int overlay_handle) { return 0; }  // TODO(ios)
int aether_ui_overlay_is_live_impl(int overlay_handle) { return 0; }  // TODO(ios)
int aether_ui_overlay_is_modal_impl(int overlay_handle) { return 0; }  // TODO(ios)
const char* aether_ui_overlay_material_effective_impl(int overlay_handle) { return ""; }  // TODO(ios)
int aether_ui_overlay_open_impl(int win_handle, int content_handle, int anchor, int dx, int dy, int modal) { return 0; }  // TODO(ios)
void aether_ui_overlay_set_material_impl(int overlay_handle, const char* kind) { }  // TODO(ios)
void aether_ui_overlay_set_on_dismiss_impl(int overlay_handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_overlay_set_transition_impl(int overlay_handle, const char* kind, int ms) { }  // TODO(ios)
void aether_ui_picker_add_item(int handle, const char* item) { }  // TODO(ios)
int aether_ui_picker_create(void* boxed_closure) { return 0; }  // TODO(ios)
int aether_ui_picker_get_selected(int handle) { return 0; }  // TODO(ios)
void aether_ui_picker_set_selected(int handle, int index) { }  // TODO(ios)
const char* aether_ui_placeholder_impl(int handle) { return ""; }  // TODO(ios)
int aether_ui_progressbar_create(double fraction) { return 0; }  // TODO(ios)
void aether_ui_progressbar_set_fraction(int handle, double fraction) { }  // TODO(ios)
void aether_ui_row_drag_reorder_impl(int row_handle, int index, void* on_drop_closure) { }  // TODO(ios)
int aether_ui_scrollview_create(void) { return 0; }  // TODO(ios)
void aether_ui_seal_subtree_impl(int handle) { }  // TODO(ios)
void aether_ui_seal_widget_impl(int handle) { }  // TODO(ios)
void aether_ui_set_alignment(int handle, int alignment) { }  // TODO(ios)
void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) { }  // TODO(ios)
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) { }  // TODO(ios)
void aether_ui_set_bg_gradient(int handle, double r1, double g1, double b1, double r2, double g2, double b2, int vertical) { }  // TODO(ios)
void aether_ui_set_border(int handle, double width, double r, double g, double b) { }  // TODO(ios)
void aether_ui_set_corner_radius(int handle, double radius) { }  // TODO(ios)
void aether_ui_set_corner_radius_ctx(void* ctx, double radius) { }  // TODO(ios)
void aether_ui_set_distribution(int handle, int distribution) { }  // TODO(ios)
void aether_ui_set_edge_insets(int handle, double top, double right, double bottom, double left) { }  // TODO(ios)
void aether_ui_set_enabled(int handle, int enabled) { }  // TODO(ios)
void aether_ui_set_enabled_ctx(void* ctx, int enabled) { }  // TODO(ios)
void aether_ui_set_focusable_impl(int handle, int on) { }  // TODO(ios)
void aether_ui_set_font_bold(int handle, int bold) { }  // TODO(ios)
void aether_ui_set_font_bold_ctx(void* ctx, int bold) { }  // TODO(ios)
void aether_ui_set_font_family(int handle, const char* family) { }  // TODO(ios)
void aether_ui_set_font_size(int handle, double size) { }  // TODO(ios)
void aether_ui_set_font_size_ctx(void* ctx, double size) { }  // TODO(ios)
void aether_ui_set_height(int handle, int height) { }  // TODO(ios)
void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) { }  // TODO(ios)
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) { }  // TODO(ios)
void aether_ui_set_opacity(int handle, double opacity) { }  // TODO(ios)
void aether_ui_set_opacity_ctx(void* ctx, double opacity) { }  // TODO(ios)
void aether_ui_set_rtl(int handle, int on) { }  // TODO(ios)
void aether_ui_set_state_style(int handle, int state, double br, double bg_, double bb, double fr, double fg_, double fb) { }  // TODO(ios)
void aether_ui_set_text_color(int handle, double r, double g, double b) { }  // TODO(ios)
void aether_ui_set_text_color_ctx(void* ctx, double r, double g, double b) { }  // TODO(ios)
void aether_ui_set_tooltip(int handle, const char* text) { }  // TODO(ios)
void aether_ui_set_tooltip_ctx(void* ctx, const char* text) { }  // TODO(ios)
void aether_ui_set_width(int handle, int width) { }  // TODO(ios)
int aether_ui_sheet_create_impl(const char* title, int width, int height) { return 0; }  // TODO(ios)
void aether_ui_sheet_dismiss_impl(int handle) { }  // TODO(ios)
void aether_ui_sheet_present_impl(int handle) { }  // TODO(ios)
void aether_ui_sheet_set_body_impl(int handle, int root_handle) { }  // TODO(ios)
void aether_ui_shortcut_chord_impl(const char* first_combo, const char* second_combo, void* boxed_closure) { }  // TODO(ios)
void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) { }  // TODO(ios)
void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure, void* enabled_closure) { }  // TODO(ios)
int aether_ui_split_position_impl(int handle) { return 0; }  // TODO(ios)
void aether_ui_split_set_position_impl(int handle, int px) { }  // TODO(ios)
int aether_ui_splitview_create(int vertical) { return 0; }  // TODO(ios)
void aether_ui_state_bind_text(int state_handle, int text_handle, const char* prefix, const char* suffix) { }  // TODO(ios)
int aether_ui_state_create(double initial) { return 0; }  // TODO(ios)
int aether_ui_state_create_b(int initial) { return 0; }  // TODO(ios)
int aether_ui_state_create_i(int initial) { return 0; }  // TODO(ios)
int aether_ui_state_create_list(void* list_ptr) { return 0; }  // TODO(ios)
int aether_ui_state_create_s(const char* initial) { return 0; }  // TODO(ios)
double aether_ui_state_get(int handle) { return 0.0; }  // TODO(ios)
int aether_ui_state_get_b(int handle) { return 0; }  // TODO(ios)
int aether_ui_state_get_i(int handle) { return 0; }  // TODO(ios)
void* aether_ui_state_get_list(int handle) { return (void*)0; }  // TODO(ios)
const char* aether_ui_state_get_s(int handle) { return ""; }  // TODO(ios)
int aether_ui_state_list_rev(int handle) { return 0; }  // TODO(ios)
void aether_ui_state_on_change(int state_handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_state_set(int handle, double value) { }  // TODO(ios)
void aether_ui_state_set_b(int handle, int value) { }  // TODO(ios)
void aether_ui_state_set_i(int handle, int value) { }  // TODO(ios)
void aether_ui_state_set_list(int handle, void* list_ptr) { }  // TODO(ios)
void aether_ui_state_set_s(int handle, const char* value) { }  // TODO(ios)
int aether_ui_state_style_impl(int handle, int state) { return 0; }  // TODO(ios)
int aether_ui_state_type(int handle) { return 0; }  // TODO(ios)
int aether_ui_styled_bg_impl(int handle) { return 0; }  // TODO(ios)
int aether_ui_styled_border_impl(int handle) { return 0; }  // TODO(ios)
int aether_ui_styled_fg_impl(int handle) { return 0; }  // TODO(ios)
const char* aether_ui_styled_font_family_impl(int handle) { return ""; }  // TODO(ios)
int aether_ui_styled_opacity_impl(int handle) { return 0; }  // TODO(ios)
const char* aether_ui_styled_weight_impl(int handle) { return ""; }  // TODO(ios)
int aether_ui_tab_add(int tabs_handle, const char* title) { return 0; }  // TODO(ios)
int aether_ui_tabs_count(int tabs_handle) { return 0; }  // TODO(ios)
int aether_ui_tabs_create(void* boxed_closure) { return 0; }  // TODO(ios)
void aether_ui_tabs_select(int tabs_handle, int index) { }  // TODO(ios)
int aether_ui_tabs_selected(int tabs_handle) { return 0; }  // TODO(ios)
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) { }  // TODO(ios)
int aether_ui_text_get_anchor(int handle) { return 0; }  // TODO(ios)
int aether_ui_text_get_truncate(int handle) { return 0; }  // TODO(ios)
int aether_ui_text_get_wrap(int handle) { return 0; }  // TODO(ios)
double aether_ui_text_measure(double size, const char* text) { return 0.0; }  // TODO(ios)
void aether_ui_text_set_string(int handle, const char* text) { }  // TODO(ios)
void aether_ui_text_set_truncate(int handle, int mode) { }  // TODO(ios)
int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) { return 0; }  // TODO(ios)
char* aether_ui_textarea_get_text(int handle) { return (void*)0; }  // TODO(ios)
void aether_ui_textarea_set_text(int handle, const char* text) { }  // TODO(ios)
void aether_ui_timer_cancel_impl(int timer_id) { }  // TODO(ios)
int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) { return 0; }  // TODO(ios)
int aether_ui_toast_impl(int win_handle, const char* text, int ms) { return 0; }  // TODO(ios)
void aether_ui_toggle_set_group(int handle, int group_with) { }  // TODO(ios)
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) { return 0; }  // TODO(ios)
void aether_ui_tray_seal_impl(int tray_id) { }  // TODO(ios)
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle, const char* icon_clean, const char* icon_busy, const char* icon_alert) { }  // TODO(ios)
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) { }  // TODO(ios)
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) { }  // TODO(ios)
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) { }  // TODO(ios)
int aether_ui_vg_tooltip_drawn_impl(void) { return 0; }  // TODO(ios)
void aether_ui_vg_tooltip_hide_impl(void) { }  // TODO(ios)
int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text, double cx, double cy) { return 0; }  // TODO(ios)
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) { }  // TODO(ios)
void aether_ui_watch_appearance_impl(void) { }  // TODO(ios)
void aether_ui_widget_add_css_class_impl(int handle, const char* cls) { }  // TODO(ios)
void aether_ui_widget_apply_css_impl(int handle, const char* property_css) { }  // TODO(ios)
const char* aether_ui_widget_classes_impl(int handle) { return ""; }  // TODO(ios)
int aether_ui_widget_count_impl(void) { return 0; }  // TODO(ios)
const char* aether_ui_widget_drag_payload_impl(int handle) { return ""; }  // TODO(ios)
void aether_ui_widget_draggable_file_impl(int handle, const char* path) { }  // TODO(ios)
const char* aether_ui_widget_kind_impl(int handle) { return ""; }  // TODO(ios)
int aether_ui_widget_parent_impl(int handle) { return 0; }  // TODO(ios)
void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) { }  // TODO(ios)
void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) { }  // TODO(ios)
void aether_ui_widget_set_hidden(int handle, int hidden) { }  // TODO(ios)
void aether_ui_widget_weight_impl(int handle, int n) { }  // TODO(ios)
int aether_ui_widget_window_impl(int widget_handle) { return 0; }  // TODO(ios)
void aether_ui_window_close_impl(int win_handle) { }  // TODO(ios)
int aether_ui_window_count_impl(void) { return 0; }  // TODO(ios)
int aether_ui_window_create_impl(const char* title, int width, int height) { return 0; }  // TODO(ios)
int aether_ui_window_file_drop_deliver(const char* paths) { return 0; }  // TODO(ios)
int aether_ui_window_is_open_impl(int win_handle) { return 0; }  // TODO(ios)
int aether_ui_window_key_deliver(const char* key_name, int mods) { return 0; }  // TODO(ios)
void aether_ui_window_on_file_drop_impl(void* boxed_closure) { }  // TODO(ios)
void aether_ui_window_on_key_impl(void* boxed_closure) { }  // TODO(ios)
void aether_ui_window_set_body_impl(int win_handle, int root_handle) { }  // TODO(ios)
void aether_ui_window_set_title_impl(int win_handle, const char* title) { }  // TODO(ios)
void aether_ui_window_show_impl(int win_handle) { }  // TODO(ios)
const char* aether_ui_window_title_impl(int win_handle) { return ""; }  // TODO(ios)
int aether_ui_wrap_create(void) { return 0; }  // TODO(ios)
int aether_ui_zstack_create(void) { return 0; }  // TODO(ios)
