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
// STATUS: ~237 of the 287 ABI functions are REAL; the rest are honest,
// compiling `// TODO(ios)` stubs at the foot of the file, so the backend links
// and is gated by the iOS SDK compile+link+RENDER check in ci.sh (Phase 1e,
// which pixel-checks the canvas natively via Mac Catalyst). Each pass moves a
// section out of the stub block into a real implementation, as the Win32/AppKit
// backends grew. What's left is mostly the heavier subsystems (overlays/sheets,
// navstack, splitview, grid/wrap, menus, notifications, file pickers,
// drag-drop, CSS engine) plus the genuinely iOS-N/A ones (tray/menu-bar).
//   pass 1 — lifecycle, widget registry, stack layout, core widgets (text,
//            button, textfield/securefield, toggle, slider).
//   pass 2 — visibility/enablement, text getters+truncation, accessibility
//            (UIAccessibility), image (UIImageView), progress bar
//            (UIProgressView), text area (UITextView), scroll view
//            (UIScrollView), picker (UIButton + UIMenu).
//   pass 3 — the CANVAS: a UIView replaying a Core Graphics command buffer
//            (paths, fills, strokes, arcs, gradients, images, clips, group
//            opacity, text), touch→click/move/release, on_resize, text metrics,
//            and the headless offscreen render/read-pixel/write-PNG paths. The
//            CG/CoreText executor is lifted verbatim from the AppKit backend
//            (identical APIs), so every vg app — rubiks_cube, boing, the clocks
//            — renders on iOS from the same .ae source as desktop.
//   pass 4 — timers (NSTimer, drives vg.live's refresh loop), a real headless
//            run loop so the driver server stays alive and serves (matching the
//            other backends), a one-shot headless canvas snapshot facility
//            (AETHER_UI_SNAPSHOT_DIR) for windowless PNG capture, and the
//            AetherUIDriver test server (enable_test_server + a hooks table) so
//            the backend is driver-capable — it binds and serves the canvas
//            pixel routes under Mac Catalyst.
//   pass 6 — styling (colour/corner/opacity/border/gradient/fonts), layout
//            setters (align/distribution/size/margins/match-parent/weight/RTL),
//            CSS classes + widget introspection, system (open-URL/dark-mode/
//            clipboard); the reactive STATE subsystem (typed cells, observers,
//            bind_text/enabled/hidden + two-way bind_value); events (tap/
//            double-tap/hover), zstack, focus/sealing; and the single-window
//            model + alert (UIAlertController) + toast + key/file-drop delivery;
//            container subsystems (tabs/navstack/splitview); in-window overlays
//            (toast/modal/tooltip, real blur scrim, enter/exit tweens) + sheets;
//            grid, form/section, and drawn vg tooltips.
//   pass 5 — modern scene lifecycle (UIWindowSceneDelegate + a scene config in
//            the app delegate): a UIWindow created without a UIWindowScene
//            paints but never composites on iOS 13+/iPad/Catalyst, and this is
//            the seam iPad multi-window / Stage Manager plugs into. Activated by
//            a UIApplicationSceneManifest in Info.plist (packaging emits it);
//            without it UIKit falls back to the legacy app-delegate window path.
//
// Proven on Mac Catalyst (natively, headless): the analog_clock and svg_tetris
// vg apps render correctly through this backend + a from-source Catalyst build
// of libaether, captured via AETHER_UI_SNAPSHOT_DIR — same .ae source as desktop.
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
#import <objc/runtime.h>          // objc_setAssociatedObject (a11y strings)
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "aether_ui_backend.h"
#include "aether_ui_test_server.h"   // AetherDriverHooks + aether_ui_test_server_start

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
    AUI_CANVAS, AUI_IMAGE,
    AUI_TABS, AUI_NAVSTACK, AUI_SPLITVIEW, AUI_WRAP, AUI_GRID, AUI_FORM
};

// ---------------------------------------------------------------------------
// Widget registry — flat strong array of UIView*, 1-based handles (as AppKit).
// Parallel arrays hold the type tag and a malloc'd space-separated CSS class
// list (NULL until a class is added), mirroring the AppKit backend so the
// driver can report kind/classes and apps can style by class.
// ---------------------------------------------------------------------------
static UIView* __strong *widgets = NULL;
static int* widget_types = NULL;
static char** widget_classes = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

static int register_widget_typed(void* widget, int type) {
    if (widget_count >= widget_capacity) {
        int new_cap = widget_capacity == 0 ? 64 : widget_capacity * 2;
        UIView* __strong *nw = (__strong UIView**)calloc(new_cap, sizeof(UIView*));
        int* nt = (int*)calloc(new_cap, sizeof(int));
        char** nc = (char**)calloc(new_cap, sizeof(char*));
        if (widgets) {
            for (int i = 0; i < widget_count; i++) {
                nw[i] = widgets[i];
                nt[i] = widget_types[i];
                nc[i] = widget_classes[i];
            }
            free(widgets); free(widget_types); free(widget_classes);
        }
        widgets = nw; widget_types = nt; widget_classes = nc;
        widget_capacity = new_cap;
    }
    widgets[widget_count] = (__bridge UIView*)widget;
    widget_types[widget_count] = type;
    widget_classes[widget_count] = NULL;
    widget_count++;
    return widget_count;
}

static int get_widget_type(int handle) {
    if (handle < 1 || handle > widget_count) return AUI_UNKNOWN;
    return widget_types[handle - 1];
}

// Kind name mirrors the GTK4/AppKit widget_type_name() vocabulary so the driver
// reports the same strings on every backend.
static const char* aeui_kind_name(int type) {
    switch (type) {
        case AUI_TEXT:        return "text";
        case AUI_BUTTON:      return "button";
        case AUI_TOGGLE:      return "toggle";
        case AUI_SLIDER:      return "slider";
        case AUI_PICKER:      return "picker";
        case AUI_TEXTFIELD:   return "textfield";
        case AUI_SECUREFIELD: return "securefield";
        case AUI_TEXTAREA:    return "textarea";
        case AUI_PROGRESSBAR: return "progressbar";
        case AUI_DIVIDER:     return "divider";
        case AUI_SCROLLVIEW:  return "scrollview";
        case AUI_VSTACK:      return "vstack";
        case AUI_HSTACK:      return "hstack";
        case AUI_ZSTACK:      return "zstack";
        case AUI_SPACER:      return "spacer";
        case AUI_CANVAS:      return "canvas";
        case AUI_IMAGE:       return "image";
        case AUI_TABS:        return "tabs";
        case AUI_NAVSTACK:    return "navstack";
        case AUI_SPLITVIEW:   return "splitview";
        case AUI_WRAP:        return "wrap";
        case AUI_GRID:        return "grid";
        case AUI_FORM:        return "form";
        default:              return "unknown";
    }
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

// The root view controller hosting the app body — shared by the scene path
// (modern, iOS 13+/iPad/Catalyst) and the legacy app-delegate path.
static UIViewController* aeui_build_root_vc(void) {
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
    return vc;
}

// Modern scene lifecycle. A UIWindow created without a UIWindowScene paints but
// never COMPOSITES on iOS 13+/iPad/Catalyst — the window has to belong to the
// scene UIKit hands us here. This is also the seam iPad multi-window / Stage
// Manager plugs into. Activated by the UIApplicationSceneManifest in Info.plist
// (the packaging step emits it); without that manifest UIKit uses the legacy
// app-delegate path below instead.
API_AVAILABLE(ios(13.0))
@interface AetherSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow* window;
@end
@implementation AetherSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
      options:(UISceneConnectionOptions*)connectionOptions {
    (void)session; (void)connectionOptions;
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = aeui_build_root_vc();
    [self.window makeKeyAndVisible];
}
@end

@interface AetherAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow* window;
@end
@implementation AetherAppDelegate
- (BOOL)application:(UIApplication*)application
        didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    (void)application; (void)launchOptions;
    // Legacy fallback: only used when no scene manifest is present (the scene
    // path handles the window otherwise). A pre-scenes runtime lands here.
    if (@available(iOS 13.0, *)) return YES;
    CGRect bounds = CGRectMake(0, 0,
                               g_want_w > 0 ? g_want_w : 390,
                               g_want_h > 0 ? g_want_h : 844);
    self.window = [[UIWindow alloc] initWithFrame:bounds];
    self.window.rootViewController = aeui_build_root_vc();
    [self.window makeKeyAndVisible];
    return YES;
}
// Route new scene sessions to AetherSceneDelegate in code, so no per-app
// delegate-class wiring is needed in the Info.plist manifest.
- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)connectingSceneSession
    options:(UISceneConnectionOptions*)options API_AVAILABLE(ios(13.0)) {
    (void)application; (void)options;
    UISceneConfiguration* cfg = [UISceneConfiguration
        configurationWithName:@"Default"
        sessionRole:connectingSceneSession.role];
    cfg.delegateClass = [AetherSceneDelegate class];
    return cfg;
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

// Headless canvas snapshot — defined in the canvas section (needs canvas_states
// in scope). Writes every canvas to <dir>/canvas_<id>.png through the same
// offscreen write_png path the goldens use.
static void aeui_snapshot_canvases(const char* dir);
static void aeui_prime_canvases(void);

void aether_ui_app_run_raw(int app_handle) {
    (void)app_handle;
    // Headless: there is no host to run a UIApplication against, but the process
    // must STAY ALIVE so the driver test server (started before this in
    // surface_run_impl) can serve — the widget tree and canvas command buffer
    // are already built by the window(){} block body. A bare CFRunLoop keeps us
    // alive AND services the main queue, which is required because the server
    // thread's read_pixel/redraw routes marshal to main via dispatch. This is
    // what lets the UIKit backend be driven headlessly (e.g. under Mac Catalyst)
    // exactly as the AppKit/GTK4/win32 backends are. /shutdown exits the process.
    if (aeui_is_headless()) {
        // One-shot headless render: dump every canvas to PNG and exit, so an
        // app's scene can be captured with no window, driver or display —
        // house rule #4 (everything renderable renders via write_png).
        const char* snap = getenv("AETHER_UI_SNAPSHOT_DIR");
        if (snap && snap[0]) {
            // Give each canvas its allocation (on_resize never fires with no
            // window, so the vg viewBox→px map would stay degenerate), then let
            // refresh timers run so an ANIMATED scene (vg.live scene_set_
            // refreshing) emits a frame into the command buffer before capture.
            aeui_prime_canvases();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
            aeui_snapshot_canvases(snap);
            return;
        }
        CFRunLoopRun();
        return;
    }
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
    } else if (get_widget_type(parent_handle) == AUI_ZSTACK) {
        // zstack: children overlap, each pinned to fill the parent (z-order =
        // insertion order, last on top).
        child.translatesAutoresizingMaskIntoConstraints = NO;
        [parent addSubview:child];
        [NSLayoutConstraint activateConstraints:@[
            [child.topAnchor constraintEqualToAnchor:parent.topAnchor],
            [child.leadingAnchor constraintEqualToAnchor:parent.leadingAnchor],
            [child.trailingAnchor constraintEqualToAnchor:parent.trailingAnchor],
            [child.bottomAnchor constraintEqualToAnchor:parent.bottomAnchor],
        ]];
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
// Pass 2 — static content, basic controls, visibility and accessibility.
// Ported out of the stub block below into real UIKit implementations, mirroring
// the corresponding AppKit sections of aether_ui_macos.m.
// ===========================================================================

// --- Widget visibility / enablement -----------------------------------------
void aether_ui_widget_set_hidden(int handle, int hidden) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) v.hidden = (hidden != 0);
}

void aether_ui_set_enabled(int handle, int enabled) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    BOOL on = (enabled != 0);
    if ([v isKindOfClass:[UIControl class]]) ((UIControl*)v).enabled = on;
    v.userInteractionEnabled = on;
    v.alpha = on ? 1.0 : 0.4;   // the AppKit backend dims a disabled control too
}

void aether_ui_set_enabled_ctx(void* ctx, int enabled) {
    aether_ui_set_enabled((int)(intptr_t)ctx, enabled);
}

// --- Text getters / truncation ----------------------------------------------
int aether_ui_text_get_wrap(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UILabel class]])
        return ((UILabel*)v).numberOfLines == 0 ? 1 : 0;
    return 0;
}

int aether_ui_text_get_anchor(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UILabel class]]) return 0;
    switch (((UILabel*)v).textAlignment) {
        case NSTextAlignmentCenter: return 1;
        case NSTextAlignmentRight:  return 2;
        default:                    return 0;
    }
}

void aether_ui_text_set_truncate(int handle, int mode) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UILabel class]]) return;
    UILabel* l = (UILabel*)v;
    NSLineBreakMode lb;
    if (mode == 1)      lb = NSLineBreakByTruncatingHead;
    else if (mode == 2) lb = NSLineBreakByTruncatingMiddle;
    else if (mode == 3) lb = NSLineBreakByTruncatingTail;
    else lb = (l.numberOfLines == 0) ? NSLineBreakByWordWrapping
                                     : NSLineBreakByClipping;
    l.lineBreakMode = lb;
    if (mode != 0 && l.numberOfLines != 0) l.numberOfLines = 1;
}

int aether_ui_text_get_truncate(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UILabel class]]) return 0;
    switch (((UILabel*)v).lineBreakMode) {
        case NSLineBreakByTruncatingHead:   return 1;
        case NSLineBreakByTruncatingMiddle: return 2;
        case NSLineBreakByTruncatingTail:   return 3;
        default:                            return 0;
    }
}

const char* aether_ui_placeholder_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UITextField class]]) {
        NSString* p = ((UITextField*)v).placeholder;
        if (p) return p.UTF8String;
    }
    return "";
}

// --- Accessibility — stored per-view + mapped onto UIAccessibility ----------
static const char kA11yRole;
static const char kA11yName;
static const char kA11yDesc;

static void a11y_store(UIView* v, const void* key, const char* s) {
    objc_setAssociatedObject(v, key,
        s ? [NSString stringWithUTF8String:s] : nil,
        OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static void a11y_copy(char* dst, int sz, NSString* s) {
    if (sz <= 0) return;
    const char* c = s ? s.UTF8String : "";
    if (!c) c = "";
    strncpy(dst, c, (size_t)sz - 1);
    dst[sz - 1] = '\0';
}

void aether_ui_a11y_set_role_impl(int handle, const char* role) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) a11y_store(v, &kA11yRole, role);
}

void aether_ui_a11y_set_label_impl(int handle, const char* name) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    a11y_store(v, &kA11yName, name);
    v.isAccessibilityElement = YES;
    v.accessibilityLabel = name ? [NSString stringWithUTF8String:name] : nil;
}

void aether_ui_a11y_set_description_impl(int handle, const char* desc) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    a11y_store(v, &kA11yDesc, desc);
    v.accessibilityHint = desc ? [NSString stringWithUTF8String:desc] : nil;
}

void aether_ui_a11y_get_impl(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    a11y_copy(role, rolesz, v ? objc_getAssociatedObject(v, &kA11yRole) : nil);
    a11y_copy(name, namesz, v ? objc_getAssociatedObject(v, &kA11yName) : nil);
    a11y_copy(desc, descsz, v ? objc_getAssociatedObject(v, &kA11yDesc) : nil);
}

// --- Image — UIImageView ----------------------------------------------------
static const char kImgTint;

static UIViewContentMode image_mode_for(int mode) {
    switch (mode) {
        case 1:  return UIViewContentModeScaleAspectFit;  // proportional up/down
        case 3:  return UIViewContentModeScaleToFill;     // axes independently
        default: return UIViewContentModeCenter;          // none
    }
}

static int image_register(UIImage* img) {
    UIImageView* iv = [[UIImageView alloc] initWithImage:img];
    iv.translatesAutoresizingMaskIntoConstraints = NO;
    iv.contentMode = UIViewContentModeCenter;
    iv.clipsToBounds = YES;
    return register_widget_typed((__bridge void*)iv, AUI_IMAGE);
}

int aether_ui_image_create(const char* filepath) {
    UIImage* img = (filepath && *filepath)
        ? [UIImage imageWithContentsOfFile:[NSString stringWithUTF8String:filepath]]
        : nil;
    return image_register(img);
}

int aether_ui_image_from_bytes(const char* data, int length) {
    UIImage* img = nil;
    if (data && length > 0) {
        NSData* d = [NSData dataWithBytes:data length:(NSUInteger)length];
        img = [UIImage imageWithData:d];
    }
    return image_register(img);
}

void aether_ui_image_set_fill(int handle, int mode) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UIImageView class]])
        ((UIImageView*)v).contentMode = image_mode_for(mode);
}

int aether_ui_image_get_fill(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UIImageView class]]) return 0;
    switch (((UIImageView*)v).contentMode) {
        case UIViewContentModeScaleAspectFit: return 1;
        case UIViewContentModeScaleToFill:    return 3;
        default:                              return 2;
    }
}

void aether_ui_image_set_size(int handle, int width, int height) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    if (width > 0)  [v.widthAnchor  constraintEqualToConstant:width].active  = YES;
    if (height > 0) [v.heightAnchor constraintEqualToConstant:height].active = YES;
}

void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UIImageView class]]) return;
    UIImageView* iv = (UIImageView*)v;
    objc_setAssociatedObject(iv, &kImgTint, @(on != 0),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (on) {
        iv.image = [iv.image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
        iv.tintColor = [UIColor colorWithRed:r green:g blue:b alpha:1.0];
    } else {
        iv.image = [iv.image imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
    }
}

int aether_ui_image_get_tint(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UIImageView class]]) return 0;
    NSNumber* n = objc_getAssociatedObject(v, &kImgTint);
    return (n && n.boolValue) ? 1 : 0;
}

int aether_ui_image_has_content(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UIImageView class]])
        return ((UIImageView*)v).image != nil ? 1 : 0;
    return 0;
}

// --- Progress bar — UIProgressView ------------------------------------------
int aether_ui_progressbar_create(double fraction) {
    UIProgressView* p = [[UIProgressView alloc]
        initWithProgressViewStyle:UIProgressViewStyleDefault];
    p.translatesAutoresizingMaskIntoConstraints = NO;
    p.progress = (float)fraction;
    return register_widget_typed((__bridge void*)p, AUI_PROGRESSBAR);
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UIProgressView class]])
        ((UIProgressView*)v).progress = (float)fraction;
}

// --- Text area — UITextView -------------------------------------------------
@interface AeuiTextViewDelegate : NSObject <UITextViewDelegate>
@property (nonatomic, assign) AeClosure* closure;
@end
@implementation AeuiTextViewDelegate
- (void)textViewDidChange:(UITextView*)tv {
    if (self.closure && self.closure->fn) {
        const char* cs = tv.text ? tv.text.UTF8String : "";
        ((void(*)(void*, const char*))self.closure->fn)(self.closure->env, cs);
    }
}
@end

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    (void)placeholder;  // UITextView has no native placeholder (pass 3)
    UITextView* tv = [[UITextView alloc] init];
    tv.translatesAutoresizingMaskIntoConstraints = NO;
    tv.editable = YES;
    tv.font = [UIFont systemFontOfSize:UIFont.systemFontSize];
    if (boxed_closure) {
        AeuiTextViewDelegate* d = [[AeuiTextViewDelegate alloc] init];
        d.closure = (AeClosure*)boxed_closure;
        tv.delegate = d;
        retain_target(d);
    }
    return register_widget_typed((__bridge void*)tv, AUI_TEXTAREA);
}

void aether_ui_textarea_set_text(int handle, const char* text) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UITextView class]])
        ((UITextView*)v).text = [NSString stringWithUTF8String:text ? text : ""];
}

char* aether_ui_textarea_get_text(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UITextView class]]) {
        UITextView* tv = (UITextView*)v;
        const char* cs = tv.text ? tv.text.UTF8String : "";
        return strdup(cs ? cs : "");   // caller owns (char*, not const char*)
    }
    return strdup("");
}

// --- Scroll view — UIScrollView ---------------------------------------------
// A bare scroller for now; content sizing/inner stack is a pass-3 refinement.
int aether_ui_scrollview_create(void) {
    UIScrollView* sv = [[UIScrollView alloc] init];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    return register_widget_typed((__bridge void*)sv, AUI_SCROLLVIEW);
}

// --- Picker — a UIButton driving a UIMenu (iPad-friendly; iOS 14+) ----------
@interface AeuiPicker : UIButton
@property (nonatomic, strong) NSMutableArray<NSString*>* items;
@property (nonatomic, assign) int selectedIndex;
@property (nonatomic, assign) AeClosure* closure;
- (void)rebuildMenu;
@end
@implementation AeuiPicker
- (void)rebuildMenu {
    NSMutableArray<UIAction*>* actions = [NSMutableArray array];
    for (NSUInteger i = 0; i < self.items.count; i++) {
        NSUInteger idx = i;
        UIAction* a = [UIAction actionWithTitle:self.items[i] image:nil
                                     identifier:nil
                                        handler:^(__kindof UIAction* action) {
            (void)action;
            self.selectedIndex = (int)idx;
            [self setTitle:self.items[idx] forState:UIControlStateNormal];
            if (self.closure && self.closure->fn)
                ((void(*)(void*, intptr_t))self.closure->fn)(
                    self.closure->env, (intptr_t)idx);
            [self rebuildMenu];
        }];
        a.state = (self.selectedIndex == (int)i) ? UIMenuElementStateOn
                                            : UIMenuElementStateOff;
        [actions addObject:a];
    }
    self.menu = [UIMenu menuWithTitle:@"" children:actions];
    self.showsMenuAsPrimaryAction = YES;
}
@end

int aether_ui_picker_create(void* boxed_closure) {
    AeuiPicker* p = [AeuiPicker buttonWithType:UIButtonTypeSystem];
    p.translatesAutoresizingMaskIntoConstraints = NO;
    p.items = [NSMutableArray array];
    p.selectedIndex = 0;
    p.closure = (AeClosure*)boxed_closure;
    [p rebuildMenu];
    return register_widget_typed((__bridge void*)p, AUI_PICKER);
}

void aether_ui_picker_add_item(int handle, const char* item) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[AeuiPicker class]]) return;
    AeuiPicker* p = (AeuiPicker*)v;
    [p.items addObject:[NSString stringWithUTF8String:item ? item : ""]];
    if (p.items.count == 1)
        [p setTitle:p.items[0] forState:UIControlStateNormal];
    [p rebuildMenu];
}

void aether_ui_picker_set_selected(int handle, int index) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[AeuiPicker class]]) return;
    AeuiPicker* p = (AeuiPicker*)v;
    if (index < 0 || index >= (int)p.items.count) return;
    p.selectedIndex = index;
    [p setTitle:p.items[index] forState:UIControlStateNormal];
    [p rebuildMenu];
}

int aether_ui_picker_get_selected(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[AeuiPicker class]]) return ((AeuiPicker*)v).selectedIndex;
    return 0;
}

// ===========================================================================
// Pass 3 — the canvas: a UIView that replays a command buffer via Core
// Graphics, the iOS counterpart of aether_ui_macos.m's NSView canvas. Core
// Graphics and Core Text are identical on macOS and iOS, so the command
// executor, gradients, images and offscreen PNG/pixel readback are lifted
// verbatim; only the view/event plumbing, font resolution and the
// current-graphics-context handling for text are UIKit-specific. This is
// what makes the vg apps (rubiks_cube, boing, the clocks, ...) render on iOS
// from the same .ae source as desktop. UIView is already top-left/y-down, so
// no isFlipped is needed; the command buffer's y-down coords replay directly.
// ===========================================================================

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
    CANVAS_CLIP_RECT,
    /* True group opacity: composite everything between BEGIN and END
       into one transparency layer, then paint that layer ONCE at the
       group alpha, so overlapping children do not double-darken.
       GTK4 has had it via cairo_push_group since the feature landed and
       win32 gained it later; this backend's pair stayed empty stubs,
       which is the same defect the win32 comment records for mememe.svg
       (a <g opacity="0.5"> of three overlapping strokes).
       Appended at the END: the values are positional. */
    CANVAS_GROUP_BEGIN, CANVAS_GROUP_END,
    CANVAS_RESET_CLIP
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
    int grad_extend;         // SVG spreadMethod: 0=pad, 1=reflect, 2=repeat
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
    AeClosure* on_scroll;  // wheel / two-finger scroll (dx,dy); see scrollWheel:
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
    int created_w, created_h;  // natural size at creation (for headless snapshot)
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

/* SVG spreadMethod on CoreGraphics.
   CGGradient has no reflect/repeat: kCGGradientDrawsBefore/AfterEndLocation is
   pad and nothing else, so `extend` was dropped here and every gradient
   rendered as pad -- reflect and repeat silently wrong, while GTK4 (cairo
   CAIRO_EXTEND_*) and win32 both honoured them.

   The fix is to stop asking the gradient to tile and hand it a stop list that
   ALREADY covers the tiles. Given `reps` copies either side of the original
   band, build stops across [-reps, 1+reps] in gradient-parameter space and
   renormalise to 0..1; the caller then stretches the axis (or the radius) by
   the same factor, so tile k lands exactly where the k-th repeat belongs.
   Reflect mirrors odd tiles, which is the whole difference between the two
   modes. Offsets are clamped and kept non-decreasing: CGGradient requires a
   monotonic location array and silently misdraws otherwise.

   Returns the new stop count, or 0 to fall back to the plain path. */
static int aeui_expand_gradient_stops(const CanvasCmd* c, int reps, int reflect,
                                      CGFloat** out_comps, CGFloat** out_locs) {
    int n = c->n_stops;
    if (n <= 0 || reps < 1) return 0;
    int tiles = 2 * reps + 1;
    int total = tiles * n;
    CGFloat* comps = (CGFloat*)malloc(sizeof(CGFloat) * total * 4);
    CGFloat* locs  = (CGFloat*)malloc(sizeof(CGFloat) * total);
    if (!comps || !locs) { free(comps); free(locs); return 0; }
    double span = (double)tiles;
    int w = 0;
    double prev = 0.0;
    for (int t = -reps; t <= reps; t++) {
        int mirrored = reflect && ((t & 1) != 0);
        for (int k = 0; k < n; k++) {
            int si = mirrored ? (n - 1 - k) : k;
            double off = c->stop_off[si];
            if (mirrored) off = 1.0 - off;
            double u = ((double)(t + reps) + off) / span;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
            if (u < prev) u = prev;      // CGGradient needs non-decreasing
            prev = u;
            locs[w] = (CGFloat)u;
            comps[w*4+0] = c->stop_rgba[si*4+0];
            comps[w*4+1] = c->stop_rgba[si*4+1];
            comps[w*4+2] = c->stop_rgba[si*4+2];
            comps[w*4+3] = c->stop_rgba[si*4+3];
            w++;
        }
    }
    *out_comps = comps; *out_locs = locs;
    return w;
}

// Resolve a CSS font stack to a UIFont — the UIKit twin of the AppKit
// aeui_resolve_font. font-family is a prioritised list: walk it and take the
// first face UIKit can instantiate, mapping the generic CSS families.
static UIFont* aeui_resolve_font(double size, const char* stack) {
    CGFloat sz = (size > 0 ? size : 16.0);
    if (stack && stack[0]) {
        NSString* all = [NSString stringWithUTF8String:stack];
        for (NSString* raw in [all componentsSeparatedByString:@","]) {
            NSString* name = [raw stringByTrimmingCharactersInSet:
                [NSCharacterSet characterSetWithCharactersInString:@" \t'\""]];
            if ([name length] == 0) continue;
            NSString* lower = [name lowercaseString];
            if ([lower isEqualToString:@"serif"])
                return [UIFont fontWithName:@"TimesNewRomanPSMT" size:sz]
                       ?: [UIFont fontWithName:@"Times New Roman" size:sz]
                       ?: [UIFont systemFontOfSize:sz];
            if ([lower isEqualToString:@"sans-serif"])
                return [UIFont fontWithName:@"Helvetica" size:sz]
                       ?: [UIFont systemFontOfSize:sz];
            if ([lower isEqualToString:@"monospace"])
                return [UIFont fontWithName:@"Menlo" size:sz]
                       ?: [UIFont fontWithName:@"Courier" size:sz]
                       ?: [UIFont monospacedSystemFontOfSize:sz weight:UIFontWeightRegular];
            if ([lower isEqualToString:@"cursive"] ||
                [lower isEqualToString:@"fantasy"])
                return [UIFont systemFontOfSize:sz];
            UIFont* f = [UIFont fontWithName:name size:sz];
            if (f) return f;
        }
    }
    return [UIFont systemFontOfSize:sz];
}

static UIFont* aeui_metrics_font(double size) {
    return [UIFont systemFontOfSize:(size > 0 ? size : 16.0)];
}

@interface AetherCanvasView : UIView
@property (assign) int canvasId;
@property (assign) CGSize aeuiLastSize;
@end

static void canvas_replay_range(CGContextRef cg, CanvasState* cs,
                                int start, int end) {
    if (!cg || !cs) return;
    if (start < 0) start = 0;
    if (end > cs->count) end = cs->count;
    {
    /* INVARIANT: every CGContextSaveGState here has exactly one matching
       restore, and CANVAS_RESET_CLIP restores/re-saves in place. CoreGraphics
       has no reset-clip call, so the pairing IS the mechanism. */
    CGContextSaveGState(cg);
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
            case CANVAS_GROUP_BEGIN: {
                /* CoreGraphics applies a transparency layer's alpha from the
                   gstate AT BEGIN, but the alpha only arrives with the END
                   command. The buffer is fully built before replay, so look
                   ahead for the matching END and set it now. Depth-counted:
                   a nested group's END must not be mistaken for this one's. */
                double ga = 1.0;
                int depth = 1;
                for (int j = i + 1; j < end; j++) {
                    if (cs->cmds[j].type == CANVAS_GROUP_BEGIN) depth++;
                    else if (cs->cmds[j].type == CANVAS_GROUP_END) {
                        depth--;
                        if (depth == 0) { ga = cs->cmds[j].x; break; }
                    }
                }
                CGContextSaveGState(cg);
                CGContextSetAlpha(cg, ga);
                CGContextBeginTransparencyLayer(cg, NULL);
                CGContextSaveGState(cg);
                break;
            }
            case CANVAS_GROUP_END:
                /* The alpha was applied at BEGIN; ending the layer composites
                   it once, which is the whole point -- painting each child at
                   the group alpha instead makes overlaps double-darken. */
                CGContextRestoreGState(cg);
                CGContextEndTransparencyLayer(cg);
                CGContextRestoreGState(cg);
                break;
            case CANVAS_CLIP_RECT:
                // Intersects the current clip and persists until the scope
                // ends or CANVAS_RESET_CLIP drops it (SVG overflow:hidden).
                CGContextClipToRect(cg, CGRectMake(c->x, c->y, c->w, c->h));
                break;
            case CANVAS_RESET_CLIP:
                // Drop every clip added since this compositing scope began.
                // CoreGraphics cannot widen a clip, so the saved baseline is
                // the only way back; re-save so the next reset still works.
                CGContextRestoreGState(cg);
                CGContextSaveGState(cg);
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
                    UIColor* col = [UIColor colorWithRed:c->r green:c->g
                                                    blue:c->b alpha:c->a];
                    UIFont* font = aeui_resolve_font(c->w, c->font_family);
                    double pct = (c->h / c->w) * 100.0;
                    if (pct <= 0.0) pct = 1.0;
                    NSDictionary* attrs = @{
                        NSFontAttributeName: font,
                        NSStrokeColorAttributeName: col,
                        NSForegroundColorAttributeName: [UIColor clearColor],
                        NSStrokeWidthAttributeName: @(pct)
                    };
                    CGFloat ascent = [font ascender];
                    [s2 drawAtPoint:CGPointMake(c->x, c->y - ascent)
                        withAttributes:attrs];
                }
                break;
            }
            case CANVAS_FILL_TEXT: {
                if (c->text) {
                    NSString* s = [NSString stringWithUTF8String:c->text];
                    UIColor* col = [UIColor colorWithRed:c->r green:c->g
                                                    blue:c->b alpha:c->a];
                    UIFont* font = aeui_resolve_font(c->w, c->font_family);
                    NSDictionary* attrs = @{
                        NSFontAttributeName: font,
                        NSForegroundColorAttributeName: col
                    };
                    // cairo's text origin is the baseline; NSString draws
                    // from the top-left, so offset up by the ascender.
                    CGFloat ascent = [font ascender];
                    [s drawAtPoint:CGPointMake(c->x, c->y - ascent)
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
                // Axis/radius actually drawn: stretched when the stops were
                // pre-tiled for reflect/repeat, the originals otherwise.
                double gx1e = c->gx1, gy1e = c->gy1;
                double gx2e = c->gx2, gy2e = c->gy2;
                double gre  = c->gr;
                if (c->n_stops > 0) {
                    CGColorSpaceRef gcs = CGColorSpaceCreateDeviceRGB();
                    CGFloat* comps = NULL; CGFloat* locs = NULL;
                    int ncomp = 0;
                    /* reflect/repeat: pre-tile the stops and stretch the axis
                       by the same factor (see aeui_expand_gradient_stops).
                       `reps` only has to cover what is visible; the clip is
                       already the path being filled, so its bounding box in
                       gradient-parameter units is the honest bound. Clamped:
                       a degenerate axis would otherwise ask for a vast stop
                       array, and past a few hundred tiles nothing is
                       distinguishable anyway. */
                    if (c->grad_extend == 1 || c->grad_extend == 2) {
                        CGRect cb = CGContextGetClipBoundingBox(cg);
                        double reach;
                        if (c->type == CANVAS_FILL_LINEAR) {
                            double ax = c->gx2 - c->gx1, ay = c->gy2 - c->gy1;
                            double len2 = ax*ax + ay*ay;
                            reach = 2.0;
                            if (len2 > 1e-9) {
                                double worst = 0.0;
                                double xs[2] = { CGRectGetMinX(cb), CGRectGetMaxX(cb) };
                                double ys[2] = { CGRectGetMinY(cb), CGRectGetMaxY(cb) };
                                for (int qi = 0; qi < 2; qi++)
                                    for (int qj = 0; qj < 2; qj++) {
                                        double t = ((xs[qi] - c->gx1) * ax
                                                  + (ys[qj] - c->gy1) * ay) / len2;
                                        double d = (t < 0.0) ? -t : (t > 1.0 ? t - 1.0 : 0.0);
                                        if (d > worst) worst = d;
                                    }
                                reach = worst;
                            }
                        } else {
                            double r = c->gr > 0.0 ? c->gr : 1.0;
                            double dx = fmax(fabs(CGRectGetMinX(cb) - c->gx1),
                                             fabs(CGRectGetMaxX(cb) - c->gx1));
                            double dy = fmax(fabs(CGRectGetMinY(cb) - c->gy1),
                                             fabs(CGRectGetMaxY(cb) - c->gy1));
                            reach = sqrt(dx*dx + dy*dy) / r;
                        }
                        int reps = (int)ceil(reach) + 1;
                        if (reps < 1) reps = 1;
                        if (reps > 64) reps = 64;
                        ncomp = aeui_expand_gradient_stops(c, reps,
                                    c->grad_extend == 1, &comps, &locs);
                        if (ncomp > 0) {
                            double span = 2.0 * reps + 1.0;
                            if (c->type == CANVAS_FILL_LINEAR) {
                                double ax = c->gx2 - c->gx1, ay = c->gy2 - c->gy1;
                                gx1e = c->gx1 - ax * reps;
                                gy1e = c->gy1 - ay * reps;
                                gx2e = c->gx1 + ax * (reps + 1);
                                gy2e = c->gy1 + ay * (reps + 1);
                            } else {
                                gre = c->gr * span;
                            }
                        }
                    }
                    if (ncomp <= 0) {
                        comps = (CGFloat*)malloc(sizeof(CGFloat) * c->n_stops * 4);
                        locs  = (CGFloat*)malloc(sizeof(CGFloat) * c->n_stops);
                        for (int si = 0; si < c->n_stops; si++) {
                            comps[si*4+0] = c->stop_rgba[si*4+0];
                            comps[si*4+1] = c->stop_rgba[si*4+1];
                            comps[si*4+2] = c->stop_rgba[si*4+2];
                            comps[si*4+3] = c->stop_rgba[si*4+3];
                            locs[si] = c->stop_off[si];
                        }
                        ncomp = c->n_stops;
                    }
                    CGGradientRef grad = CGGradientCreateWithColorComponents(
                        gcs, comps, locs, ncomp);
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
                                CGPointMake(gx1e, gy1e),
                                CGPointMake(gx2e, gy2e),
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
                                CGPointMake(0, 0),
                                (c->gr > 0.0) ? (gre / c->gr) : 1.0,
                                kCGGradientDrawsBeforeStartLocation |
                                kCGGradientDrawsAfterEndLocation);
                        } else {
                            CGContextDrawRadialGradient(cg, grad,
                                CGPointMake(c->gfx, c->gfy), 0,
                                CGPointMake(c->gx1, c->gy1), gre,
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
    CGContextRestoreGState(cg);
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

    UIGraphicsPushContext(cg);
    canvas_replay_range(cg, cs, start, end);
    UIGraphicsPopContext();

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
- (void)drawRect:(CGRect)rect {
    (void)rect;
    CGContextRef cg = UIGraphicsGetCurrentContext();
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (!cg) return;
    if (cs) {
        CGRect b = self.bounds;
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
    // The drawRect context is already the current UIKit context, so text draws
    // correctly without an explicit push here.
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

// Touch → the same press/move/release closures the AppKit mouse path drives.
// locationInView is already canvas coords (top-left origin, y down).
- (void)fireTouch:(NSSet<UITouch*>*)touches closure:(AeClosure*)c {
    if (!c || !c->fn) return;
    UITouch* t = touches.anyObject;
    if (!t) return;
    CGPoint p = [t locationInView:self];
    ((void(*)(void*, double, double))c->fn)(c->env, p.x, p.y);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (cs) [self fireTouch:touches closure:cs->on_click];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (cs) [self fireTouch:touches closure:cs->on_move];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (cs) [self fireTouch:touches closure:cs->on_release];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (cs) [self fireTouch:touches closure:cs->on_release];
}

// on_resize on a real allocation change — the vg scene re-maps its viewBox.
- (void)layoutSubviews {
    [super layoutSubviews];
    CGSize sz = self.bounds.size;
    if ((int)lround(sz.width) == (int)lround(self.aeuiLastSize.width)
        && (int)lround(sz.height) == (int)lround(self.aeuiLastSize.height)) return;
    self.aeuiLastSize = sz;
    CanvasState* cs = get_canvas_state(self.canvasId);
    if (!cs || !cs->on_resize || !cs->on_resize->fn) return;
    int w = (int)lround(sz.width), h = (int)lround(sz.height);
    if (w == cs->last_w && h == cs->last_h) return;
    cs->last_w = w; cs->last_h = h;
    AeClosure* c = cs->on_resize;
    dispatch_async(dispatch_get_main_queue(), ^{
        ((void(*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)w, (intptr_t)h);
    });
}
@end

int aether_ui_canvas_create_impl(int width, int height) {
    AetherCanvasView* v = [[AetherCanvasView alloc]
        initWithFrame:CGRectMake(0, 0, width, height)];
    v.translatesAutoresizingMaskIntoConstraints = NO;
    v.contentMode = UIViewContentModeRedraw;   // re-run drawRect on resize
    v.aeuiLastSize = CGSizeZero;
    // Natural size, not a cage: low-priority size constraints + low hugging so
    // the canvas is the slack-taker (its vg scene rescales to fill), exactly as
    // the AppKit backend arranges (priority 150 / hugging 1).
    NSLayoutConstraint* wc = [v.widthAnchor constraintEqualToConstant:width];
    NSLayoutConstraint* hc = [v.heightAnchor constraintEqualToConstant:height];
    wc.priority = 150; hc.priority = 150;
    wc.active = YES;   hc.active = YES;
    [v setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisHorizontal];
    [v setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisVertical];

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
    cs->created_w = width;
    cs->created_h = height;
    return canvas_id;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->widget_handle : 0;
}

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
    cs->on_move = (AeClosure*)boxed_closure;  // delivered by touchesMoved:
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
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_scroll = (AeClosure*)boxed_closure;
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
void aether_ui_canvas_group_begin_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_GROUP_BEGIN });
}
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    // Alpha rides in x, matching the GTK4 record (cairo_paint_with_alpha(c->x)).
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_GROUP_END, .x = alpha });
}

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

double aether_ui_text_measure(double size, const char* text) {
    if (!text || !text[0]) return 0.0;
    NSString* s = [NSString stringWithUTF8String:text];
    if (!s) return 0.0;
    return (double)[s sizeWithAttributes:@{
        NSFontAttributeName: aeui_metrics_font(size)
    }].width;
}
double aether_ui_font_ascent(double size)  { return (double)[aeui_metrics_font(size) ascender]; }
double aether_ui_font_descent(double size) { return (double)-[aeui_metrics_font(size) descender]; }
double aether_ui_font_height(double size)  { return (double)[aeui_metrics_font(size) lineHeight]; }

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
    CanvasCmd cmd = { .type = CANVAS_FILL_LINEAR,
                      .gx1 = x1, .gy1 = y1, .gx2 = x2, .gy2 = y2,
                      .grad_line_width = line_width, .grad_extend = extend,
                      .iw = cap, .ih = join };
    macos_copy_stops(&cmd, n_stops, offsets, rgba);
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
    UIView* v = (__bridge UIView*)aether_ui_get_widget(cs->widget_handle);
    if (v) [v setNeedsDisplay];
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    UIView* v = (__bridge UIView*)aether_ui_get_widget(cs->widget_handle);
    if (v) [v setNeedsDisplay];
}

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
        // Let CoreGraphics own the backing store (NULL data, auto stride): iOS
        // rejects CGBitmapContextCreate with a caller-supplied buffer for this
        // pixel format and returns NULL, unlike macOS. The write_png and
        // render_range paths already use this form; read back via GetData.
        CGColorSpaceRef cspace = CGColorSpaceCreateDeviceRGB();
        CGContextRef cg = CGBitmapContextCreate(
            NULL, (size_t)width, (size_t)height, 8, 0, cspace,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cspace);
        if (!cg) return;
        // Canvas coords are y-down; the bitmap is y-up (see canvas_write_png).
        CGContextTranslateCTM(cg, 0, height);
        CGContextScaleCTM(cg, 1.0, -1.0);
        UIGraphicsPushContext(cg);
        canvas_replay(cg, cs);
        UIGraphicsPopContext();
        unsigned char* data = (unsigned char*)CGBitmapContextGetData(cg);
        size_t stride = CGBitmapContextGetBytesPerRow(cg);
        if (data) {
            // RGBA8 big-endian: byte order in memory is R,G,B,A.
            unsigned char* p8 = data + (size_t)py * stride + (size_t)px * 4;
            result = ((int)p8[3] << 24) | ((int)p8[0] << 16)
                   | ((int)p8[1] << 8) | (int)p8[2];
        }
        CGContextRelease(cg);
    };
    if ([NSThread isMainThread]) work();
    else dispatch_sync(dispatch_get_main_queue(), work);
    return result;
}

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
    // Command buffer is y-down (top-left); a bitmap context is y-up — flip.
    CGContextTranslateCTM(cg, 0, height);
    CGContextScaleCTM(cg, 1.0, -1.0);
    // Text draws into the CURRENT UIKit context, so back it with this bitmap for
    // the replay (the AppKit backend does the same via NSGraphicsContext).
    UIGraphicsPushContext(cg);
    canvas_replay(cg, cs);
    UIGraphicsPopContext();
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

// Headless one-shot snapshot: write every canvas to <dir>/canvas_<id>.png at
// its natural (creation) size, through the offscreen write_png path. Lets an
// app's rendered scene be captured with no window/driver/display — used by
// aether_ui_app_run_raw when AETHER_UI_SNAPSHOT_DIR is set.
// Deliver each canvas its natural size through on_resize, and lay the view out
// at that size, so a vg scene that maps its viewBox to the allocation has a
// non-degenerate one before the first headless frame.
static void aeui_prime_canvases(void) {
    for (int id = 1; id <= canvas_state_count; id++) {
        CanvasState* cs = get_canvas_state(id);
        if (!cs) continue;
        int w = cs->created_w > 0 ? cs->created_w : 512;
        int h = cs->created_h > 0 ? cs->created_h : 512;
        UIView* v = (__bridge UIView*)aether_ui_get_widget(cs->widget_handle);
        if (v) { v.frame = CGRectMake(0, 0, w, h); [v layoutIfNeeded]; }
        if (cs->on_resize && cs->on_resize->fn)
            ((void(*)(void*, intptr_t, intptr_t))cs->on_resize->fn)(
                cs->on_resize->env, (intptr_t)w, (intptr_t)h);
    }
}

static void aeui_snapshot_canvases(const char* dir) {
    for (int id = 1; id <= canvas_state_count; id++) {
        CanvasState* cs = get_canvas_state(id);
        if (!cs) continue;
        int w = cs->created_w > 0 ? cs->created_w : 512;
        int h = cs->created_h > 0 ? cs->created_h : 512;
        char path[1024];
        snprintf(path, sizeof(path), "%s/canvas_%d.png", dir, id);
        aether_ui_canvas_write_png_impl(id, path, w, h);
    }
}

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

// --- Timers — NSTimer on the main run loop, firing the closure each tick ----
// This is what drives vg.live's refresh loop (scene_set_refreshing) and any
// app animation; without it a refreshing scene never repaints. 1-based ids into
// a keep-alive array; cancel invalidates in place.
static NSMutableArray<NSTimer*>* g_timers = nil;

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (!boxed_closure) return 0;
    if (!g_timers) g_timers = [NSMutableArray array];
    AeClosure* c = (AeClosure*)boxed_closure;
    NSTimeInterval iv = (interval_ms > 0 ? interval_ms : 1) / 1000.0;
    NSTimer* t = [NSTimer scheduledTimerWithTimeInterval:iv repeats:YES
        block:^(NSTimer* timer) {
            (void)timer;
            if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
        }];
    [g_timers addObject:t];
    return (int)g_timers.count;   // 1-based id
}

void aether_ui_timer_cancel_impl(int timer_id) {
    if (!g_timers || timer_id < 1 || timer_id > (int)g_timers.count) return;
    NSTimer* t = g_timers[timer_id - 1];
    if ((id)t != [NSNull null]) [t invalidate];
}

// --- Native list (NSTableView-backed on AppKit) -----------------------------
// The AppKit backend added a native windowed list; the UIKit equivalent is a
// UITableView/UICollectionView with cell reuse, a later pass. Report NOT
// available so apps fall back to the portable stack/scroll vlist, and stub the
// rest so the ABI links.
int aether_ui_native_list_available_impl(void) { return 0; }
int aether_ui_native_list_create_impl(int horizontal, int window_rows) {
    (void)horizontal; (void)window_rows; return 0;
}
void aether_ui_native_list_set_row_builder_impl(int handle, void* builder) {
    (void)handle; (void)builder;
}
void aether_ui_native_list_set_count_impl(int handle, int count) {
    (void)handle; (void)count;
}
void aether_ui_native_list_scroll_to_impl(int handle, int index) {
    (void)handle; (void)index;
}
int aether_ui_native_list_first_visible_impl(int handle) { (void)handle; return 0; }


// ===========================================================================
// Pass 6 — styling, layout setters, CSS classes, widget introspection, system.
// UIView.layer for visual styling; UIStackView for alignment/distribution;
// per-widget CSS-class + type from the registry; UIPasteboard/UIApplication/
// UITraitCollection for system bits. Mirrors the AppKit backend section-for-
// section where a direct UIKit equivalent exists.
// ===========================================================================

static UIColor* aeui_color(double r, double g, double b, double a) {
    return [UIColor colorWithRed:r green:g blue:b alpha:a];
}

// Current font of a text-bearing view (label/button/field/textview), or nil.
static UIFont* aeui_view_font(UIView* v) {
    if ([v isKindOfClass:[UILabel class]])     return ((UILabel*)v).font;
    if ([v isKindOfClass:[UIButton class]])    return ((UIButton*)v).titleLabel.font;
    if ([v isKindOfClass:[UITextField class]]) return ((UITextField*)v).font;
    if ([v isKindOfClass:[UITextView class]])  return ((UITextView*)v).font;
    return nil;
}
static void aeui_set_view_font(UIView* v, UIFont* f) {
    if (!f) return;
    if ([v isKindOfClass:[UILabel class]])     ((UILabel*)v).font = f;
    else if ([v isKindOfClass:[UIButton class]])    ((UIButton*)v).titleLabel.font = f;
    else if ([v isKindOfClass:[UITextField class]]) ((UITextField*)v).font = f;
    else if ([v isKindOfClass:[UITextView class]])  ((UITextView*)v).font = f;
}

// --- Visual styling (UIView.layer) ------------------------------------------
void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) v.backgroundColor = aeui_color(r, g, b, a);
}
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aether_ui_set_bg_color((int)(intptr_t)ctx, r, g, b, a);
}
void aether_ui_set_corner_radius(int handle, double radius) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    v.layer.cornerRadius = radius;
    v.clipsToBounds = YES;
}
void aether_ui_set_corner_radius_ctx(void* ctx, double radius) {
    aether_ui_set_corner_radius((int)(intptr_t)ctx, radius);
}
void aether_ui_set_opacity(int handle, double opacity) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) v.alpha = opacity;
}
void aether_ui_set_opacity_ctx(void* ctx, double opacity) {
    aether_ui_set_opacity((int)(intptr_t)ctx, opacity);
}
void aether_ui_set_border(int handle, double width, double r, double g, double b) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    v.layer.borderWidth = width;
    if (width > 0.0) v.layer.borderColor = aeui_color(r, g, b, 1.0).CGColor;
}
void aether_ui_set_bg_gradient(int handle, double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    CAGradientLayer* grad = [CAGradientLayer layer];
    grad.frame = v.bounds;
    grad.colors = @[ (id)aeui_color(r1, g1, b1, 1.0).CGColor,
                     (id)aeui_color(r2, g2, b2, 1.0).CGColor ];
    if (vertical) { grad.startPoint = CGPointMake(0.5, 0.0); grad.endPoint = CGPointMake(0.5, 1.0); }
    else          { grad.startPoint = CGPointMake(0.0, 0.5); grad.endPoint = CGPointMake(1.0, 0.5); }
    // NOTE: CALayer has no autoresizingMask on iOS; the gradient is sized to the
    // view's current bounds and does not track later resizes (pass-N refinement
    // would update grad.frame in a layoutSubviews override).
    [v.layer insertSublayer:grad atIndex:0];
}

// --- Text colour + fonts ----------------------------------------------------
void aether_ui_set_text_color(int handle, double r, double g, double b) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    UIColor* c = aeui_color(r, g, b, 1.0);
    if ([v isKindOfClass:[UILabel class]])          ((UILabel*)v).textColor = c;
    else if ([v isKindOfClass:[UIButton class]])    [(UIButton*)v setTitleColor:c forState:UIControlStateNormal];
    else if ([v isKindOfClass:[UITextField class]]) ((UITextField*)v).textColor = c;
    else if ([v isKindOfClass:[UITextView class]])  ((UITextView*)v).textColor = c;
}
void aether_ui_set_text_color_ctx(void* ctx, double r, double g, double b) {
    aether_ui_set_text_color((int)(intptr_t)ctx, r, g, b);
}
void aether_ui_set_font_size(int handle, double size) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    UIFont* f = aeui_view_font(v);
    if (f) aeui_set_view_font(v, [f fontWithSize:size]);
}
void aether_ui_set_font_size_ctx(void* ctx, double size) {
    aether_ui_set_font_size((int)(intptr_t)ctx, size);
}
void aether_ui_set_font_bold(int handle, int bold) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    UIFont* f = aeui_view_font(v);
    if (!f) return;
    UIFontDescriptor* d = f.fontDescriptor;
    UIFontDescriptorSymbolicTraits tr = d.symbolicTraits;
    if (bold) tr |= UIFontDescriptorTraitBold; else tr &= ~UIFontDescriptorTraitBold;
    UIFontDescriptor* nd = [d fontDescriptorWithSymbolicTraits:tr];
    if (nd) aeui_set_view_font(v, [UIFont fontWithDescriptor:nd size:f.pointSize]);
}
void aether_ui_set_font_bold_ctx(void* ctx, int bold) {
    aether_ui_set_font_bold((int)(intptr_t)ctx, bold);
}
void aether_ui_set_font_family(int handle, const char* family) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    UIFont* f = aeui_view_font(v);
    if (!f || !family || !family[0]) return;
    UIFont* nf = [UIFont fontWithName:[NSString stringWithUTF8String:family] size:f.pointSize];
    if (nf) aeui_set_view_font(v, nf);
}
void aether_ui_text_set_string(int handle, const char* text) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    NSString* s = [NSString stringWithUTF8String:text ? text : ""];
    if ([v isKindOfClass:[UILabel class]])          ((UILabel*)v).text = s;
    else if ([v isKindOfClass:[UIButton class]])    [(UIButton*)v setTitle:s forState:UIControlStateNormal];
    else if ([v isKindOfClass:[UITextField class]]) ((UITextField*)v).text = s;
    else if ([v isKindOfClass:[UITextView class]])  ((UITextView*)v).text = s;
}

// --- Stack alignment / distribution -----------------------------------------
// The DSL passes a small enum; clamp into UIKit's ranges. Exact cross-backend
// parity of the numeric values is approximate (AppKit's NSStackView enums
// differ) — refined per-app as specs pin it.
void aether_ui_set_alignment(int handle, int alignment) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UIStackView class]]) {
        if (alignment < 0) alignment = 0;
        if (alignment > (int)UIStackViewAlignmentTrailing) alignment = (int)UIStackViewAlignmentFill;
        ((UIStackView*)v).alignment = (UIStackViewAlignment)alignment;
    } else if (v && [v isKindOfClass:[UILabel class]]) {
        ((UILabel*)v).textAlignment = alignment == 1 ? NSTextAlignmentCenter
                                    : alignment == 2 ? NSTextAlignmentRight
                                                     : NSTextAlignmentLeft;
    }
}
void aether_ui_set_distribution(int handle, int distribution) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && [v isKindOfClass:[UIStackView class]]) {
        if (distribution < 0) distribution = 0;
        if (distribution > (int)UIStackViewDistributionEqualCentering) distribution = 0;
        ((UIStackView*)v).distribution = (UIStackViewDistribution)distribution;
    }
}

// --- Size / margins / match-parent ------------------------------------------
void aether_ui_set_width(int handle, int width) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && width > 0) [v.widthAnchor constraintEqualToConstant:width].active = YES;
}
void aether_ui_set_height(int handle, int height) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && height > 0) [v.heightAnchor constraintEqualToConstant:height].active = YES;
}
static void aeui_apply_margins(UIView* v, int top, int right, int bottom, int left) {
    if (!v) return;
    v.layoutMargins = UIEdgeInsetsMake(top, left, bottom, right);
    if ([v isKindOfClass:[UIStackView class]])
        ((UIStackView*)v).layoutMarginsRelativeArrangement = YES;
}
void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    aeui_apply_margins((__bridge UIView*)aether_ui_get_widget(handle), top, right, bottom, left);
}
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aeui_apply_margins((__bridge UIView*)aether_ui_get_widget((int)(intptr_t)ctx),
                       top, right, bottom, left);
}
void aether_ui_set_edge_insets(int handle, double top, double right, double bottom, double left) {
    aeui_apply_margins((__bridge UIView*)aether_ui_get_widget(handle),
                       (int)top, (int)right, (int)bottom, (int)left);
}
void aether_ui_match_parent_width(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && v.superview) [v.widthAnchor constraintEqualToAnchor:v.superview.widthAnchor].active = YES;
}
void aether_ui_match_parent_height(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v && v.superview) [v.heightAnchor constraintEqualToAnchor:v.superview.heightAnchor].active = YES;
}

// --- RTL, tooltip (→ a11y hint on iOS) --------------------------------------
void aether_ui_set_rtl(int handle, int on) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) v.semanticContentAttribute = on ? UISemanticContentAttributeForceRightToLeft
                                            : UISemanticContentAttributeForceLeftToRight;
}
void aether_ui_set_tooltip(int handle, const char* text) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) v.accessibilityHint = text ? [NSString stringWithUTF8String:text] : nil;
}
void aether_ui_set_tooltip_ctx(void* ctx, const char* text) {
    aether_ui_set_tooltip((int)(intptr_t)ctx, text);
}

// --- Flex weight — a weighted child soaks up slack --------------------------
void aether_ui_widget_weight_impl(int handle, int n) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    UILayoutPriority p = n > 0 ? (UILayoutPriorityDefaultLow - 1) : UILayoutPriorityDefaultHigh;
    [v setContentHuggingPriority:p forAxis:UILayoutConstraintAxisHorizontal];
    [v setContentHuggingPriority:p forAxis:UILayoutConstraintAxisVertical];
}

// --- CSS classes (per-widget, space-separated; drives selectors + the driver)
void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    if (handle < 1 || handle > widget_count || !cls || !cls[0]) return;
    char* cur = widget_classes[handle - 1];
    if (!cur) { widget_classes[handle - 1] = strdup(cls); return; }
    // Already present? (whole-token match)
    size_t clen = strlen(cls);
    const char* p = cur;
    while (*p) {
        while (*p == ' ') p++;
        const char* start = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - start) == clen && strncmp(start, cls, clen) == 0) return;
    }
    size_t n = strlen(cur) + 1 + clen + 1;
    char* joined = (char*)malloc(n);
    snprintf(joined, n, "%s %s", cur, cls);
    free(cur);
    widget_classes[handle - 1] = joined;
}
void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    if (handle < 1 || handle > widget_count || !cls || !cls[0]) return;
    char* cur = widget_classes[handle - 1];
    if (!cur) return;
    size_t clen = strlen(cls);
    char* out = (char*)malloc(strlen(cur) + 1);
    out[0] = '\0';
    const char* p = cur;
    while (*p) {
        while (*p == ' ') p++;
        const char* start = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - start);
        if (tlen == 0) continue;
        if (tlen == clen && strncmp(start, cls, clen) == 0) continue;  // drop it
        if (out[0]) strcat(out, " ");
        strncat(out, start, tlen);
    }
    free(cur);
    widget_classes[handle - 1] = out[0] ? out : (free(out), (char*)NULL);
}
const char* aether_ui_widget_classes_impl(int handle) {
    if (handle < 1 || handle > widget_count) return "";
    char* c = widget_classes[handle - 1];
    return c ? c : "";
}

// --- Widget introspection ---------------------------------------------------
int aether_ui_widget_count_impl(void) { return widget_count; }
const char* aether_ui_widget_kind_impl(int handle) { return aeui_kind_name(get_widget_type(handle)); }
int aether_ui_widget_parent_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || !v.superview) return 0;
    return aether_ui_handle_for_widget((__bridge void*)v.superview);
}

// --- System: URL open, dark mode, clipboard ---------------------------------
void aether_ui_open_url_impl(const char* url) {
    if (!url || !url[0]) return;
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    if (!u) return;
    if ([UIApplication respondsToSelector:@selector(sharedApplication)]) {
        UIApplication* app = [UIApplication sharedApplication];
        if (app) [app openURL:u options:@{} completionHandler:nil];
    }
}
int aether_ui_dark_mode_check(void) {
    if (@available(iOS 13.0, *)) {
        return UITraitCollection.currentTraitCollection.userInterfaceStyle
               == UIUserInterfaceStyleDark ? 1 : 0;
    }
    return 0;
}
char* aether_ui_clipboard_read_impl(void) {
    NSString* s = UIPasteboard.generalPasteboard.string;
    return strdup(s ? s.UTF8String : "");
}
void aether_ui_clipboard_write_impl(const char* text) {
    UIPasteboard.generalPasteboard.string =
        [NSString stringWithUTF8String:text ? text : ""];
}


// ===========================================================================
// Pass 6 wave 2 — reactive state (cells, observers, property bindings).
// Platform-agnostic subsystem lifted from the AppKit backend: it only calls the
// widget setters (text_set_string / set_enabled / widget_set_hidden /
// textfield_get/set_text), all of which are real here. Two-way bind_value wires
// UITextField's EditingChanged back into the string cell (a programmatic
// .text set does NOT refire EditingChanged on iOS, so no bounce guard needed).
// ===========================================================================
enum { AEUI_STATE_FLOAT = 0, AEUI_STATE_INT = 1,
       AEUI_STATE_BOOL = 2, AEUI_STATE_STRING = 3, AEUI_STATE_LIST = 4 };
typedef struct {
    int type;
    double num;   // float/int/bool payload
    char* str;    // string payload (owned)
    void* list;   // LIST payload (opaque std.list ptr, NOT owned)
    int rev;      // LIST: bumps on each set
} StateCell;

typedef struct { int state_handle; AeClosure* closure; } StateObserver;
static StateObserver* state_observers = NULL;
static int state_observer_count = 0;
static int state_observer_capacity = 0;

enum { AEUI_BIND_TEXT = 0, AEUI_BIND_ENABLED = 1, AEUI_BIND_HIDDEN = 2,
       AEUI_BIND_VALUE = 3 };
typedef struct {
    int kind, state_handle, widget_handle;
    char* prefix; char* suffix;
    int decimals; int invert;
} PropBinding;

static StateCell* state_cells = NULL;
static int state_count = 0, state_capacity = 0;
static PropBinding* prop_bindings = NULL;
static int prop_binding_count = 0, prop_binding_capacity = 0;

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
    c->type = type; c->num = num;
    c->str = str ? strdup(str) : NULL; c->list = NULL; c->rev = 0;
    state_count++;
    return state_count;
}

int aether_ui_state_create(double initial)   { return state_create_cell(AEUI_STATE_FLOAT, initial, NULL); }
int aether_ui_state_create_s(const char* i)  { return state_create_cell(AEUI_STATE_STRING, 0.0, i ? i : ""); }
int aether_ui_state_create_i(int initial)    { return state_create_cell(AEUI_STATE_INT, (double)initial, NULL); }
int aether_ui_state_create_b(int initial)    { return state_create_cell(AEUI_STATE_BOOL, initial ? 1.0 : 0.0, NULL); }

double aether_ui_state_get(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_FLOAT) ? c->num : 0.0;
}
const char* aether_ui_state_get_s(int handle) {   // malloc'd — extern owns it
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

static void state_render_value(StateCell* c, int decimals, char* buf, int n) {
    if (!c) { buf[0] = '\0'; return; }
    switch (c->type) {
        case AEUI_STATE_STRING: snprintf(buf, n, "%s", c->str ? c->str : ""); break;
        case AEUI_STATE_INT:    snprintf(buf, n, "%d", (int)c->num); break;
        case AEUI_STATE_BOOL:   snprintf(buf, n, "%s", c->num != 0.0 ? "true" : "false"); break;
        default:
            if (decimals >= 0)               snprintf(buf, n, "%.*f", decimals, c->num);
            else if (c->num == (int)c->num)  snprintf(buf, n, "%d", (int)c->num);
            else                             snprintf(buf, n, "%.2f", c->num);
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
        const char* cur = aether_ui_textfield_get_text(b->widget_handle);
        const char* want = (c->type == AEUI_STATE_STRING && c->str) ? c->str : "";
        if (!cur || strcmp(cur, want) != 0)
            aether_ui_textfield_set_text(b->widget_handle, want);
        return;
    }
    if (b->kind == AEUI_BIND_TEXT) {
        char val[256]; state_render_value(c, b->decimals, val, sizeof(val));
        char buf[512]; snprintf(buf, sizeof(buf), "%s%s%s", b->prefix, val, b->suffix);
        aether_ui_text_set_string(b->widget_handle, buf);
    } else {
        int on = state_truthy(c);
        if (b->invert) on = !on;
        if (b->kind == AEUI_BIND_ENABLED) aether_ui_set_enabled(b->widget_handle, on);
        else                              aether_ui_widget_set_hidden(b->widget_handle, on);
    }
}
static void fire_state_observers(int state_handle) {
    int n = state_observer_count;
    for (int i = 0; i < n; i++)
        if (state_observers[i].state_handle == state_handle) {
            AeClosure* c = state_observers[i].closure;
            if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
        }
}
static void update_prop_bindings(int state_handle) {
    for (int i = 0; i < prop_binding_count; i++)
        if (prop_bindings[i].state_handle == state_handle)
            apply_prop_binding(&prop_bindings[i]);
    fire_state_observers(state_handle);
}

void aether_ui_state_on_change(int state_handle, void* boxed_closure) {
    if (state_observer_count >= state_observer_capacity) {
        state_observer_capacity = state_observer_capacity == 0 ? 16 : state_observer_capacity * 2;
        state_observers = realloc(state_observers, sizeof(StateObserver) * state_observer_capacity);
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
    c->list = list_ptr; c->rev++;
    update_prop_bindings(handle);
}
int aether_ui_state_list_rev(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_LIST) ? c->rev : 0;
}

void aether_ui_state_set(int handle, double value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_FLOAT) return;
    c->num = value; update_prop_bindings(handle);
}
void aether_ui_state_set_s(int handle, const char* value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_STRING) return;
    free(c->str); c->str = strdup(value ? value : "");
    update_prop_bindings(handle);
}
void aether_ui_state_set_i(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_INT) return;
    c->num = (double)value; update_prop_bindings(handle);
}
void aether_ui_state_set_b(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_BOOL) return;
    c->num = value ? 1.0 : 0.0; update_prop_bindings(handle);
}

static PropBinding* prop_binding_new(int kind, int state_handle, int widget_handle) {
    if (prop_binding_count >= prop_binding_capacity) {
        prop_binding_capacity = prop_binding_capacity == 0 ? 32 : prop_binding_capacity * 2;
        prop_bindings = realloc(prop_bindings, sizeof(PropBinding) * prop_binding_capacity);
    }
    PropBinding* b = &prop_bindings[prop_binding_count++];
    b->kind = kind; b->state_handle = state_handle; b->widget_handle = widget_handle;
    b->prefix = strdup(""); b->suffix = strdup(""); b->decimals = -1; b->invert = 0;
    return b;
}

void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix) {
    PropBinding* b = prop_binding_new(AEUI_BIND_TEXT, state_handle, text_handle);
    free(b->prefix); free(b->suffix);
    b->prefix = prefix ? strdup(prefix) : strdup("");
    b->suffix = suffix ? strdup(suffix) : strdup("");
    apply_prop_binding(b);
}
void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals) {
    PropBinding* b = prop_binding_new(AEUI_BIND_TEXT, state_handle, widget_handle);
    b->decimals = decimals; apply_prop_binding(b);
}
void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = prop_binding_new(AEUI_BIND_ENABLED, state_handle, widget_handle);
    b->invert = invert; apply_prop_binding(b);
}
void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = prop_binding_new(AEUI_BIND_HIDDEN, state_handle, widget_handle);
    b->invert = invert; apply_prop_binding(b);
}

// Two-way: string state ⇄ UITextField. State→field is a VALUE binding; field→
// state is an EditingChanged target writing back into the cell.
@interface AeuiValueBindTarget : NSObject
@property (nonatomic, assign) int stateHandle;
@end
@implementation AeuiValueBindTarget
- (void)changed:(UITextField*)f {
    aether_ui_state_set_s(self.stateHandle, f.text ? f.text.UTF8String : "");
}
@end
void aether_ui_bind_value(int state_handle, int widget_handle) {
    PropBinding* b = prop_binding_new(AEUI_BIND_VALUE, state_handle, widget_handle);
    UIView* v = (__bridge UIView*)aether_ui_get_widget(widget_handle);
    if (v && [v isKindOfClass:[UITextField class]]) {
        AeuiValueBindTarget* t = [[AeuiValueBindTarget alloc] init];
        t.stateHandle = state_handle;
        [(UITextField*)v addTarget:t action:@selector(changed:)
            forControlEvents:UIControlEventEditingChanged];
        retain_target(t);
    }
    apply_prop_binding(b);  // seed the field from the state's initial value
}


// ===========================================================================
// Pass 6 wave 3 — events (tap/double-tap/hover), zstack, focus, sealing, misc.
// ===========================================================================
static const char kDblClosure;
static const char kSealed;

@interface AeuiTapTarget : NSObject
@property (nonatomic, assign) AeClosure* closure;
@end
@implementation AeuiTapTarget
- (void)fire {
    if (self.closure && self.closure->fn)
        ((void(*)(void*))self.closure->fn)(self.closure->env);
}
- (void)hover:(UIHoverGestureRecognizer*)g {
    if (g.state == UIGestureRecognizerStateBegan && self.closure && self.closure->fn)
        ((void(*)(void*))self.closure->fn)(self.closure->env);
}
@end

void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    AeuiTapTarget* t = [[AeuiTapTarget alloc] init];
    t.closure = (AeClosure*)boxed_closure;
    UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc]
        initWithTarget:t action:@selector(fire)];
    v.userInteractionEnabled = YES;
    [v addGestureRecognizer:tap];
    retain_target(t);
}

void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    AeuiTapTarget* t = [[AeuiTapTarget alloc] init];
    t.closure = (AeClosure*)boxed_closure;
    UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc]
        initWithTarget:t action:@selector(fire)];
    tap.numberOfTapsRequired = 2;
    v.userInteractionEnabled = YES;
    [v addGestureRecognizer:tap];
    retain_target(t);
    // Also addressable by handle so the driver / fire_double_click can invoke it.
    objc_setAssociatedObject(v, &kDblClosure, [NSValue valueWithPointer:boxed_closure],
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// Driver / programmatic double-click: fire the stored closure. 1 if fired.
int aether_ui_fire_double_click(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    NSValue* nv = objc_getAssociatedObject(v, &kDblClosure);
    if (!nv) return 0;
    AeClosure* c = (AeClosure*)nv.pointerValue;
    if (c && c->fn) { ((void(*)(void*))c->fn)(c->env); return 1; }
    return 0;
}

// Pointer hover (iPad, iOS 13.4+ trackpad/Magic-Keyboard). No-op on plain touch.
void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || !boxed_closure) return;
    if (@available(iOS 13.0, *)) {
        AeuiTapTarget* t = [[AeuiTapTarget alloc] init];
        t.closure = (AeClosure*)boxed_closure;
        UIHoverGestureRecognizer* h = [[UIHoverGestureRecognizer alloc]
            initWithTarget:t action:@selector(hover:)];
        [v addGestureRecognizer:h];
        retain_target(t);
    }
}

// --- zstack — a plain UIView; children overlap-fill (pinned in add_child) ----
int aether_ui_zstack_create(void) {
    UIView* v = [[UIView alloc] init];
    v.translatesAutoresizingMaskIntoConstraints = NO;
    return register_widget_typed((__bridge void*)v, AUI_ZSTACK);
}

// --- focus / first responder ------------------------------------------------
void aether_ui_focus_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) [v becomeFirstResponder];
}
int aether_ui_focused_widget(void) {
    for (int i = 0; i < widget_count; i++)
        if (widgets[i] && [widgets[i] isFirstResponder]) return i + 1;
    return 0;
}
void aether_ui_set_focusable_impl(int handle, int on) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) v.userInteractionEnabled = (on != 0);
}

// --- window handle (single window on iOS; primary == 1) ---------------------
int aether_ui_widget_window_impl(int widget_handle) {
    return (widget_handle >= 1 && widget_handle <= widget_count) ? 1 : 0;
}

// --- driver server ctx wrapper ----------------------------------------------
void aether_ui_enable_test_server_ctx(int port, void* ctx) {
    aether_ui_enable_test_server_impl(port, (int)(intptr_t)ctx);
}

// --- sealing (driver "not automatable" marker) ------------------------------
void aether_ui_seal_widget_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (v) objc_setAssociatedObject(v, &kSealed, @(1), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
void aether_ui_seal_subtree_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return;
    aether_ui_seal_widget_impl(handle);
    for (UIView* k in v.subviews) {
        int kh = aether_ui_handle_for_widget((__bridge void*)k);
        if (kh) aether_ui_seal_subtree_impl(kh);
    }
}

// --- single-child container set (navstack pages, etc.) ----------------------
void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) {
    UIView* parent = (__bridge UIView*)aether_ui_get_widget(parent_handle);
    UIView* child = (__bridge UIView*)aether_ui_get_widget(child_handle);
    if (!parent || !child) return;
    for (UIView* k in [parent.subviews copy]) [k removeFromSuperview];
    child.translatesAutoresizingMaskIntoConstraints = NO;
    [parent addSubview:child];
    [NSLayoutConstraint activateConstraints:@[
        [child.topAnchor constraintEqualToAnchor:parent.topAnchor],
        [child.leadingAnchor constraintEqualToAnchor:parent.leadingAnchor],
        [child.trailingAnchor constraintEqualToAnchor:parent.trailingAnchor],
        [child.bottomAnchor constraintEqualToAnchor:parent.bottomAnchor],
    ]];
}


// ===========================================================================
// Pass 6 wave 4 — the single-window model, alert, toast, key/file-drop delivery.
// iOS has one foreground window (per scene); the multi-window ABI collapses to
// that. alert/toast present on the active scene's key window (no-op headless).
// ===========================================================================
static UIWindow* aeui_key_window(void) {
    if (@available(iOS 13.0, *)) {
        for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
            if (![s isKindOfClass:[UIWindowScene class]]) continue;
            for (UIWindow* w in ((UIWindowScene*)s).windows)
                if (w.isKeyWindow) return w;
        }
        for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:[UIWindowScene class]])
                for (UIWindow* w in ((UIWindowScene*)s).windows) return w;
    }
    return nil;
}
static UIViewController* aeui_top_vc(void) {
    UIViewController* vc = aeui_key_window().rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    return vc;
}

// --- Single-window handle model (primary == 1) ------------------------------
int aether_ui_window_create_impl(const char* title, int width, int height) {
    (void)title; (void)width; (void)height;
    return 1;   // iOS: one window; secondary windows are scenes (a later pass)
}
int aether_ui_window_count_impl(void) { return 1; }
int aether_ui_window_is_open_impl(int win_handle) { return win_handle >= 1 ? 1 : 0; }
void aether_ui_window_show_impl(int win_handle) { (void)win_handle; }
void aether_ui_window_close_impl(int win_handle) { (void)win_handle; }
void aether_ui_close_window_by_handle_impl(int win_handle) { (void)win_handle; }
void aether_ui_window_set_title_impl(int win_handle, const char* title) {
    (void)win_handle; (void)title;   // iOS windows carry no title chrome
}
const char* aether_ui_window_title_impl(int win_handle) { (void)win_handle; return ""; }
void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    (void)win_handle;
    // Primary window body → the app root. Takes effect at scene/window build;
    // if the window is already live, reinstall as the root VC's child.
    g_root_handle = root_handle;
    UIViewController* vc = aeui_key_window().rootViewController;
    if (vc && root_handle >= 1) {
        int host = aether_ui_handle_for_widget((__bridge void*)vc.view);
        if (host) aether_ui_widget_set_child_impl(host, root_handle);
    }
}

// --- Window key + file-drop: store the closure, driver delivers it ----------
static AeClosure* g_window_key_closure = NULL;
static AeClosure* g_window_file_drop_closure = NULL;

void aether_ui_window_on_key_impl(void* boxed_closure) {
    g_window_key_closure = (AeClosure*)boxed_closure;   // hardware-keyboard pass wires live delivery
}
int aether_ui_window_key_deliver(const char* key_name, int mods) {
    if (!g_window_key_closure || !g_window_key_closure->fn) return 0;
    ((void(*)(void*, const char*, int))g_window_key_closure->fn)(
        g_window_key_closure->env, key_name ? key_name : "", mods);
    return 1;
}
void aether_ui_window_on_file_drop_impl(void* boxed_closure) {
    g_window_file_drop_closure = (AeClosure*)boxed_closure;
}
int aether_ui_window_file_drop_deliver(const char* paths) {
    if (!g_window_file_drop_closure || !g_window_file_drop_closure->fn) return 0;
    ((void(*)(void*, const char*))g_window_file_drop_closure->fn)(
        g_window_file_drop_closure->env, paths ? paths : "");
    return 1;
}

// --- Alert (UIAlertController) ----------------------------------------------
void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) return;          // no modal loop without a user
    UIViewController* top = aeui_top_vc();
    if (!top) return;
    UIAlertController* a = [UIAlertController
        alertControllerWithTitle:[NSString stringWithUTF8String:title ? title : ""]
                         message:[NSString stringWithUTF8String:message ? message : ""]
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK"
                                          style:UIAlertActionStyleDefault handler:nil]];
    [top presentViewController:a animated:YES completion:nil];
}

// --- Toast — transient label overlay on the key window, auto-dismiss --------
int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    (void)win_handle;
    if (aeui_is_headless()) return 1;
    UIWindow* w = aeui_key_window();
    if (!w) return 0;
    UILabel* toast = [[UILabel alloc] init];
    toast.text = [NSString stringWithUTF8String:text ? text : ""];
    toast.textColor = [UIColor whiteColor];
    toast.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.8];
    toast.textAlignment = NSTextAlignmentCenter;
    toast.layer.cornerRadius = 8.0;
    toast.clipsToBounds = YES;
    toast.translatesAutoresizingMaskIntoConstraints = NO;
    [w addSubview:toast];
    [NSLayoutConstraint activateConstraints:@[
        [toast.centerXAnchor constraintEqualToAnchor:w.centerXAnchor],
        [toast.bottomAnchor constraintEqualToAnchor:w.safeAreaLayoutGuide.bottomAnchor constant:-40],
        [toast.heightAnchor constraintEqualToConstant:40],
        [toast.leadingAnchor constraintGreaterThanOrEqualToAnchor:w.leadingAnchor constant:20],
    ]];
    double secs = (ms > 0 ? ms : 2000) / 1000.0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [UIView animateWithDuration:0.3 animations:^{ toast.alpha = 0.0; }
                         completion:^(BOOL f){ (void)f; [toast removeFromSuperview]; }];
    });
    return 1;
}


// ===========================================================================
// Pass 6 wave 5 — container subsystems: tabs, navstack, splitview.
// ===========================================================================

// --- Tabs — UISegmentedControl strip over a swapped content host -----------
// The tabs widget is a vertical stack [segmented ; contentHost]. Each page is a
// vstack; selecting swaps the single child of contentHost. Matches the AppKit
// NSTabView model (tab_add returns the page vstack to attach children to).
typedef struct {
    int tabs_handle;
    int content_host;          // widget handle of the content UIView
    UISegmentedControl* seg;   // the strip (unretained; held by the view tree)
    int* pages;                // page widget handles
    int page_count, page_cap;
    int selected;
    AeClosure* on_change;
} AeuiTabsState;
static AeuiTabsState* tabs_states = NULL;
static int tabs_state_count = 0, tabs_state_cap = 0;

static AeuiTabsState* tabs_state_for(int handle) {
    for (int i = 0; i < tabs_state_count; i++)
        if (tabs_states[i].tabs_handle == handle) return &tabs_states[i];
    return NULL;
}

@interface AeuiTabsTarget : NSObject
@property (nonatomic, assign) int tabsHandle;
@end
@implementation AeuiTabsTarget
- (void)changed:(UISegmentedControl*)s {
    aether_ui_tabs_select(self.tabsHandle, (int)s.selectedSegmentIndex);
}
@end

int aether_ui_tabs_create(void* boxed_closure) {
    UIStackView* root = [[UIStackView alloc] init];
    root.axis = UILayoutConstraintAxisVertical;
    root.spacing = 6;
    root.alignment = UIStackViewAlignmentFill;
    root.translatesAutoresizingMaskIntoConstraints = NO;
    int handle = register_widget_typed((__bridge void*)root, AUI_TABS);

    UISegmentedControl* seg = [[UISegmentedControl alloc] init];
    seg.translatesAutoresizingMaskIntoConstraints = NO;
    [root addArrangedSubview:seg];
    UIView* host = [[UIView alloc] init];
    host.translatesAutoresizingMaskIntoConstraints = NO;
    int host_h = register_widget_typed((__bridge void*)host, AUI_ZSTACK);  // fill-child host
    [root addArrangedSubview:host];

    AeuiTabsTarget* t = [[AeuiTabsTarget alloc] init];
    t.tabsHandle = handle;
    [seg addTarget:t action:@selector(changed:) forControlEvents:UIControlEventValueChanged];
    retain_target(t);

    if (tabs_state_count >= tabs_state_cap) {
        tabs_state_cap = tabs_state_cap == 0 ? 8 : tabs_state_cap * 2;
        tabs_states = realloc(tabs_states, sizeof(AeuiTabsState) * tabs_state_cap);
    }
    AeuiTabsState* ts = &tabs_states[tabs_state_count++];
    ts->tabs_handle = handle;
    ts->content_host = host_h;
    ts->seg = seg;
    ts->pages = NULL; ts->page_count = 0; ts->page_cap = 0;
    ts->selected = 0;
    ts->on_change = (AeClosure*)boxed_closure;
    return handle;
}

int aether_ui_tab_add(int tabs_handle, const char* title) {
    AeuiTabsState* ts = tabs_state_for(tabs_handle);
    if (!ts) return 0;
    int page = aether_ui_vstack_create(8);
    if (ts->page_count >= ts->page_cap) {
        ts->page_cap = ts->page_cap == 0 ? 4 : ts->page_cap * 2;
        ts->pages = realloc(ts->pages, sizeof(int) * ts->page_cap);
    }
    ts->pages[ts->page_count] = page;
    [ts->seg insertSegmentWithTitle:[NSString stringWithUTF8String:title ? title : ""]
                            atIndex:ts->page_count animated:NO];
    ts->page_count++;
    if (ts->page_count == 1) {   // first page becomes visible
        ts->seg.selectedSegmentIndex = 0;
        aether_ui_tabs_select(tabs_handle, 0);
    }
    return page;
}

void aether_ui_tabs_select(int tabs_handle, int index) {
    AeuiTabsState* ts = tabs_state_for(tabs_handle);
    if (!ts || index < 0 || index >= ts->page_count) return;
    ts->selected = index;
    ts->seg.selectedSegmentIndex = index;
    aether_ui_widget_set_child_impl(ts->content_host, ts->pages[index]);
    if (ts->on_change && ts->on_change->fn)
        ((void(*)(void*, intptr_t))ts->on_change->fn)(ts->on_change->env, (intptr_t)index);
}
int aether_ui_tabs_selected(int tabs_handle) {
    AeuiTabsState* ts = tabs_state_for(tabs_handle);
    return ts ? ts->selected : 0;
}
int aether_ui_tabs_count(int tabs_handle) {
    AeuiTabsState* ts = tabs_state_for(tabs_handle);
    return ts ? ts->page_count : 0;
}
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    AeuiTabsState* ts = tabs_state_for(tabs_handle);
    if (ts) ts->on_change = (AeClosure*)boxed_closure;
}

// --- Navstack — one visible page; title bar + body; depth tracked -----------
// Matches the AppKit model: the container holds exactly one page at a time
// (push replaces it), and depth is what the driver observes. A title becomes a
// real text widget above the body (no per-platform title chrome).
static const char kNavDepth;

int aether_ui_navstack_create(void) {
    UIView* v = [[UIView alloc] init];
    v.translatesAutoresizingMaskIntoConstraints = NO;
    return register_widget_typed((__bridge void*)v, AUI_NAVSTACK);
}
void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    UIView* container = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!container || body_handle < 1) return;
    int page = body_handle;
    if (title && title[0]) {
        int wrap = aether_ui_vstack_create(0);
        int bar = aether_ui_text_create(title);
        aether_ui_widget_add_child_ctx((void*)(intptr_t)wrap, bar);
        aether_ui_widget_add_child_ctx((void*)(intptr_t)wrap, body_handle);
        page = wrap;
    }
    aether_ui_widget_set_child_impl(handle, page);   // one page at a time
    NSNumber* d = objc_getAssociatedObject(container, &kNavDepth);
    objc_setAssociatedObject(container, &kNavDepth, @((d ? d.intValue : 0) + 1),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
void aether_ui_navstack_pop(int handle) {
    UIView* container = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!container) return;
    NSNumber* d = objc_getAssociatedObject(container, &kNavDepth);
    int depth = d ? d.intValue : 0;
    if (depth <= 0) return;   // root: no-op
    for (UIView* k in [container.subviews copy]) [k removeFromSuperview];
    objc_setAssociatedObject(container, &kNavDepth, @(depth - 1),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
int aether_ui_navstack_depth(int handle) {
    UIView* container = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!container) return 0;
    NSNumber* d = objc_getAssociatedObject(container, &kNavDepth);
    return d ? d.intValue : 0;
}

// --- Splitview — two panes, first sized to the divider position -------------
// UIKit has no draggable NSSplitView; a horizontal/vertical stack with the
// first pane pinned to a position constraint gives a real two-pane split (drag
// is a later refinement). vertical follows the GTK/ABI sense: vertical=1 stacks
// the panes vertically (axis vertical). The position constraint is stored on
// the view for split_position/set_position.
static const char kSplitConstraint;
static const char kSplitVertical;

int aether_ui_splitview_create(int vertical) {
    UIStackView* sv = [[UIStackView alloc] init];
    sv.axis = vertical ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    sv.distribution = UIStackViewDistributionFill;
    sv.alignment = UIStackViewAlignmentFill;
    sv.spacing = 1;
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    int h = register_widget_typed((__bridge void*)sv, AUI_SPLITVIEW);
    objc_setAssociatedObject(sv, &kSplitVertical, @(vertical),
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return h;
}
void aether_ui_split_set_position_impl(int handle, int px) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v || ![v isKindOfClass:[UIStackView class]]) return;
    UIStackView* sv = (UIStackView*)v;
    if (sv.arrangedSubviews.count < 1) return;
    UIView* first = sv.arrangedSubviews[0];
    NSNumber* vert = objc_getAssociatedObject(v, &kSplitVertical);
    NSLayoutConstraint* c = objc_getAssociatedObject(v, &kSplitConstraint);
    if (c) c.active = NO;
    c = (vert && vert.boolValue)
        ? [first.heightAnchor constraintEqualToConstant:px]
        : [first.widthAnchor constraintEqualToConstant:px];
    c.active = YES;
    objc_setAssociatedObject(v, &kSplitConstraint, c, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
int aether_ui_split_position_impl(int handle) {
    UIView* v = (__bridge UIView*)aether_ui_get_widget(handle);
    if (!v) return 0;
    NSLayoutConstraint* c = objc_getAssociatedObject(v, &kSplitConstraint);
    return c ? (int)c.constant : 0;
}


// ===========================================================================
// Pass 6 wave 6 — in-window overlays (toast / modal / tooltip / dropdown).
// Drawn inside the key window, never a compositor popup. The table is
// append-only and monotonic (close flips `live` to 0, never removes) so
// GET /overlays lists every overlay ever opened — a toast must be observably
// DEAD, not absent (matches AppKit/GTK4). Headless (no window) still tracks the
// entry state so specs can observe it; views are added only when a window
// exists. Modal scrim uses a real UIBlurEffect for "blur", a dim fill otherwise.
// ===========================================================================
static int aeui_animations_off(void) {
    const char* v = getenv("AETHER_UI_NO_ANIMATION");
    return v && v[0] && v[0] != '0';
}

typedef struct {
    UIView* __unsafe_unretained content;   // owned by the window tree
    UIView* __unsafe_unretained scrim;
    AeClosure* on_dismiss;
    int modal, live, exiting, exit_played, trans_ms;
    char* trans_kind;   // "fade"/"slide-up"/…; NULL = fade
    char* material;     // "dim" | "blur" | "tint"
} OverlayEntry;
static OverlayEntry* overlays = NULL;
static int overlay_count = 0, overlay_capacity = 0;
static OverlayEntry* overlay_at(int handle) {
    if (handle < 1 || handle > overlay_count) return NULL;
    return &overlays[handle - 1];
}

@interface AeuiScrimTarget : NSObject
@property (nonatomic, assign) int overlayHandle;
@end
@implementation AeuiScrimTarget
- (void)tap {
    OverlayEntry* e = overlay_at(self.overlayHandle);
    if (!e || !e->live) return;
    if (e->on_dismiss && e->on_dismiss->fn)   // scrim click is the ONLY dismiss path
        ((void(*)(void*))e->on_dismiss->fn)(e->on_dismiss->env);
    aether_ui_overlay_close_impl(self.overlayHandle);
}
@end

int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal) {
    (void)win_handle;
    if (overlay_count >= overlay_capacity) {
        overlay_capacity = overlay_capacity == 0 ? 8 : overlay_capacity * 2;
        overlays = realloc(overlays, sizeof(OverlayEntry) * overlay_capacity);
    }
    OverlayEntry* e = &overlays[overlay_count];
    memset(e, 0, sizeof(*e));
    e->modal = modal; e->live = 1; e->trans_ms = 0;
    e->material = strdup("dim");
    int handle = ++overlay_count;   // 1-based

    UIWindow* w = aeui_key_window();
    UIView* content = (__bridge UIView*)aether_ui_get_widget(content_handle);
    if (w && content) {
        if (modal) {
            UIView* scrim;
            if (strcmp(e->material, "blur") == 0) {
                scrim = [[UIVisualEffectView alloc]
                    initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterial]];
            } else {
                scrim = [[UIView alloc] init];
                scrim.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.4];
            }
            scrim.translatesAutoresizingMaskIntoConstraints = NO;
            [w addSubview:scrim];
            [NSLayoutConstraint activateConstraints:@[
                [scrim.topAnchor constraintEqualToAnchor:w.topAnchor],
                [scrim.leadingAnchor constraintEqualToAnchor:w.leadingAnchor],
                [scrim.trailingAnchor constraintEqualToAnchor:w.trailingAnchor],
                [scrim.bottomAnchor constraintEqualToAnchor:w.bottomAnchor],
            ]];
            AeuiScrimTarget* t = [[AeuiScrimTarget alloc] init];
            t.overlayHandle = handle;
            [scrim addGestureRecognizer:
                [[UITapGestureRecognizer alloc] initWithTarget:t action:@selector(tap)]];
            scrim.userInteractionEnabled = YES;
            retain_target(t);
            e->scrim = scrim;
        }
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [w addSubview:content];
        UILayoutGuide* g = w.safeAreaLayoutGuide;
        if (anchor == 0) {   // absolute top-left placement (tooltips at a point)
            [content.leadingAnchor constraintEqualToAnchor:w.leadingAnchor constant:dx].active = YES;
            [content.topAnchor constraintEqualToAnchor:w.topAnchor constant:dy].active = YES;
        } else {             // centred, with offset (toasts/modals)
            [content.centerXAnchor constraintEqualToAnchor:g.centerXAnchor constant:dx].active = YES;
            [content.centerYAnchor constraintEqualToAnchor:g.centerYAnchor constant:dy].active = YES;
        }
        if (!aeui_animations_off()) {   // enter fade
            content.alpha = 0.0; if (e->scrim) e->scrim.alpha = 0.0;
            [UIView animateWithDuration:0.15 animations:^{
                UIView* c = (__bridge UIView*)aether_ui_get_widget(content_handle);
                c.alpha = 1.0;
                OverlayEntry* ee = overlay_at(handle);
                if (ee && ee->scrim) ee->scrim.alpha = 1.0;
            }];
        }
    }
    return handle;
}

void aether_ui_overlay_close_impl(int overlay_handle) {
    OverlayEntry* e = overlay_at(overlay_handle);
    if (!e || !e->live) return;
    int h = overlay_handle;
    void (^finalize)(void) = ^{
        OverlayEntry* ee = overlay_at(h);
        if (!ee) return;
        if (ee->scrim)   [ee->scrim removeFromSuperview];
        if (ee->content) [ee->content removeFromSuperview];
        ee->live = 0; ee->exiting = 0;
    };
    if (e->trans_ms > 0 && !aeui_animations_off() && e->content) {
        e->exiting = 1; e->exit_played = 1;
        [UIView animateWithDuration:(e->trans_ms / 1000.0) animations:^{
            OverlayEntry* ee = overlay_at(h);
            if (ee) { if (ee->content) ee->content.alpha = 0.0;
                      if (ee->scrim)   ee->scrim.alpha = 0.0; }
        } completion:^(BOOL done){ (void)done; finalize(); }];
    } else {
        finalize();
    }
}

int  aether_ui_overlay_count_impl(void) { return overlay_count; }
int  aether_ui_overlay_is_live_impl(int h)    { OverlayEntry* e = overlay_at(h); return e ? e->live : 0; }
int  aether_ui_overlay_is_modal_impl(int h)   { OverlayEntry* e = overlay_at(h); return e ? e->modal : 0; }
int  aether_ui_overlay_is_exiting_impl(int h) { OverlayEntry* e = overlay_at(h); return e ? e->exiting : 0; }
int  aether_ui_overlay_exit_played_impl(int h){ OverlayEntry* e = overlay_at(h); return e ? e->exit_played : 0; }
void aether_ui_overlay_set_on_dismiss_impl(int h, void* boxed_closure) {
    OverlayEntry* e = overlay_at(h); if (e) e->on_dismiss = (AeClosure*)boxed_closure;
}
void aether_ui_overlay_set_transition_impl(int h, const char* kind, int ms) {
    OverlayEntry* e = overlay_at(h); if (!e) return;
    free(e->trans_kind); e->trans_kind = kind ? strdup(kind) : NULL;
    e->trans_ms = ms;
}
void aether_ui_overlay_set_material_impl(int h, const char* kind) {
    OverlayEntry* e = overlay_at(h); if (!e) return;
    free(e->material); e->material = strdup(kind ? kind : "dim");
}
const char* aether_ui_overlay_material_effective_impl(int h) {
    OverlayEntry* e = overlay_at(h);
    return (e && e->material) ? e->material : "dim";
}

// --- Sheets — a presented UIViewController hosting a body view --------------
typedef struct { int body_handle; UIViewController* __unsafe_unretained vc; } SheetEntry;
static SheetEntry* sheets = NULL;
static int sheet_count = 0, sheet_cap = 0;

int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    (void)title; (void)width; (void)height;
    if (sheet_count >= sheet_cap) {
        sheet_cap = sheet_cap == 0 ? 4 : sheet_cap * 2;
        sheets = realloc(sheets, sizeof(SheetEntry) * sheet_cap);
    }
    sheets[sheet_count].body_handle = 0;
    sheets[sheet_count].vc = nil;
    return ++sheet_count;   // 1-based
}
void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    if (handle < 1 || handle > sheet_count) return;
    sheets[handle - 1].body_handle = root_handle;
}
void aether_ui_sheet_present_impl(int handle) {
    if (handle < 1 || handle > sheet_count || aeui_is_headless()) return;
    UIViewController* top = aeui_top_vc();
    UIView* body = (__bridge UIView*)aether_ui_get_widget(sheets[handle - 1].body_handle);
    if (!top || !body) return;
    UIViewController* vc = [[UIViewController alloc] init];
    vc.view.backgroundColor = [UIColor systemBackgroundColor];
    body.translatesAutoresizingMaskIntoConstraints = NO;
    [vc.view addSubview:body];
    UILayoutGuide* g = vc.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [body.topAnchor constraintEqualToAnchor:g.topAnchor],
        [body.leadingAnchor constraintEqualToAnchor:g.leadingAnchor],
        [body.trailingAnchor constraintEqualToAnchor:g.trailingAnchor],
        [body.bottomAnchor constraintLessThanOrEqualToAnchor:g.bottomAnchor],
    ]];
    sheets[handle - 1].vc = vc;
    [top presentViewController:vc animated:YES completion:nil];
}
void aether_ui_sheet_dismiss_impl(int handle) {
    if (handle < 1 || handle > sheet_count) return;
    UIViewController* vc = sheets[handle - 1].vc;
    if (vc) [vc dismissViewControllerAnimated:YES completion:nil];
}


// ===========================================================================
// Pass 6 wave 7 — grid, form/section, drawn vg tooltips.
// ===========================================================================

// --- Grid — a vertical stack of horizontal row-stacks ------------------------
static const char kGridCols;
static const char kGridColSpacing;

int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    UIStackView* g = [[UIStackView alloc] init];
    g.axis = UILayoutConstraintAxisVertical;
    g.spacing = row_spacing;
    g.alignment = UIStackViewAlignmentFill;
    g.translatesAutoresizingMaskIntoConstraints = NO;
    int h = register_widget_typed((__bridge void*)g, AUI_GRID);
    objc_setAssociatedObject(g, &kGridCols, @(cols > 0 ? cols : 1), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(g, &kGridColSpacing, @(col_spacing), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return h;
}

void aether_ui_grid_place(int grid_handle, int child_handle, int row, int col,
                          int row_span, int col_span) {
    (void)row_span; (void)col_span;   // spans are a later refinement
    UIStackView* g = (__bridge UIStackView*)aether_ui_get_widget(grid_handle);
    UIView* child = (__bridge UIView*)aether_ui_get_widget(child_handle);
    if (!g || !child || ![g isKindOfClass:[UIStackView class]] || row < 0 || col < 0) return;
    NSNumber* csn = objc_getAssociatedObject(g, &kGridColSpacing);
    CGFloat colSpacing = csn ? csn.doubleValue : 0;
    // Ensure `row` row-stacks exist.
    while ((int)g.arrangedSubviews.count <= row) {
        UIStackView* r = [[UIStackView alloc] init];
        r.axis = UILayoutConstraintAxisHorizontal;
        r.spacing = colSpacing;
        r.alignment = UIStackViewAlignmentFill;
        r.distribution = UIStackViewDistributionFillEqually;
        r.translatesAutoresizingMaskIntoConstraints = NO;
        [g addArrangedSubview:r];
    }
    UIStackView* rowStack = (UIStackView*)g.arrangedSubviews[row];
    // Pad the row with empty cells up to `col`, then drop the child in.
    while ((int)rowStack.arrangedSubviews.count < col) {
        UIView* pad = [[UIView alloc] init];
        pad.translatesAutoresizingMaskIntoConstraints = NO;
        [rowStack addArrangedSubview:pad];
    }
    child.translatesAutoresizingMaskIntoConstraints = NO;
    if (col < (int)rowStack.arrangedSubviews.count) {
        UIView* existing = rowStack.arrangedSubviews[col];
        [rowStack insertArrangedSubview:child atIndex:col];
        [existing removeFromSuperview];   // replace the placeholder/old cell
    } else {
        [rowStack addArrangedSubview:child];
    }
}

// --- Form / section — grouped vstacks; a section carries a title header ------
int aether_ui_form_create(void) {
    UIStackView* f = [[UIStackView alloc] init];
    f.axis = UILayoutConstraintAxisVertical;
    f.spacing = 16;
    f.alignment = UIStackViewAlignmentFill;
    f.translatesAutoresizingMaskIntoConstraints = NO;
    return register_widget_typed((__bridge void*)f, AUI_FORM);
}
int aether_ui_form_section_create(const char* title) {
    int section = aether_ui_vstack_create(8);
    if (title && title[0]) {
        int header = aether_ui_text_create(title);   // real text widget the driver sees
        aether_ui_set_font_bold(header, 1);
        aether_ui_widget_add_child_ctx((void*)(intptr_t)section, header);
    }
    // Mark it a form section for the driver's kind reporting.
    if (section >= 1 && section <= widget_count) widget_types[section - 1] = AUI_FORM;
    return section;
}

// --- Drawn vg tooltip — a single reused overlay following the pointer --------
static int g_vg_tooltip_overlay = 0;   // 0 = none live
int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text, double cx, double cy) {
    (void)canvas_id;
    if (g_vg_tooltip_overlay && aether_ui_overlay_is_live_impl(g_vg_tooltip_overlay))
        aether_ui_overlay_close_impl(g_vg_tooltip_overlay);
    UILabel* label = [[UILabel alloc] init];
    label.text = [NSString stringWithUTF8String:text ? text : ""];
    label.textColor = [UIColor whiteColor];
    label.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.85];
    label.font = [UIFont systemFontOfSize:13];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.layer.cornerRadius = 4.0;
    label.clipsToBounds = YES;
    int content = register_widget_typed((__bridge void*)label, AUI_TEXT);
    // anchor 0 = absolute placement at (cx, cy), non-modal.
    g_vg_tooltip_overlay = aether_ui_overlay_open_impl(0, content, 0, (int)cx, (int)cy, 0);
    return g_vg_tooltip_overlay;
}
void aether_ui_vg_tooltip_hide_impl(void) {
    if (g_vg_tooltip_overlay) {
        aether_ui_overlay_close_impl(g_vg_tooltip_overlay);
        g_vg_tooltip_overlay = 0;
    }
}
int aether_ui_vg_tooltip_drawn_impl(void) {
    return (g_vg_tooltip_overlay && aether_ui_overlay_is_live_impl(g_vg_tooltip_overlay)) ? 1 : 0;
}

// ===========================================================================
// UNIMPLEMENTED — later-pass stubs.
//
// Every remaining ABI entry the app can call, defined so the backend links.
// Each returns a safe default and is a TODO for a later pass. The real work
// moves them OUT of this block into proper sections above, as passes 1-3 did.
// ===========================================================================

void aether_ui_context_menu_item_accel_impl(int handle, const char* label, const char* accel, void* boxed_closure) { }  // TODO(ios)
void aether_ui_context_menu_item_impl(int handle, const char* label, void* boxed_closure) { }  // TODO(ios)
// --- AetherUIDriver hooks ---------------------------------------------------
// Enough for the server to run and serve the canvas pixel routes (which call
// aether_ui_canvas_read_pixel_impl directly, no hook) plus the cheap scalar
// queries. The rest stay NULL — the shared server treats a NULL hook as 501 and
// omits the field, so a partial table is safe. A full driver (widget geometry,
// dispatch_action) is a later pass. Every hook here reads only plain state, no
// off-main UIView mutation.
static int hook_widget_count(void) { return widget_count; }
static const char* hook_widget_type(int handle) { return aeui_kind_name(get_widget_type(handle)); }
static int hook_toggle_active(int handle) { return aether_ui_toggle_get_active(handle); }
static double hook_slider_value(int handle) { return aether_ui_slider_get_value(handle); }
static void hook_widget_a11y(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    aether_ui_a11y_get_impl(handle, role, rolesz, name, namesz, desc, descsz);
}
static int hook_canvas_debug(int canvas_id, int* area, int* commands,
                             int* w, int* h) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    if (area) *area = cs->last_paint_area;
    if (commands) *commands = cs->count;   // immediate-mode: current buffer size
    if (w) *w = cs->created_w;
    if (h) *h = cs->created_h;
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

static const AetherDriverHooks uikit_driver_hooks = {
    .widget_count          = hook_widget_count,
    .widget_type           = hook_widget_type,
    .toggle_active         = hook_toggle_active,
    .slider_value          = hook_slider_value,
    .widget_a11y           = hook_widget_a11y,
    .canvas_debug          = hook_canvas_debug,
    .canvas_paint_counters = hook_canvas_paint_counters,
};

static int uikit_test_server_started = 0;
void aether_ui_enable_test_server_impl(int port, int root_handle) {
    (void)root_handle;   // banner injection is a later pass
    if (uikit_test_server_started) return;   // idempotent (env + explicit call)
    uikit_test_server_started = 1;
    aether_ui_test_server_start(port, &uikit_driver_hooks);
}
int aether_ui_file_icon_create(const char* path) { return 0; }  // TODO(ios)
void aether_ui_file_icon_set(int handle, const char* path) { }  // TODO(ios)
char* aether_ui_file_open(const char* title, const char* start_dir) { return (void*)0; }  // TODO(ios)
char* aether_ui_file_pick_folder(const char* title, const char* start_dir) { return (void*)0; }  // TODO(ios)
char* aether_ui_file_save(const char* title, const char* default_name) { return (void*)0; }  // TODO(ios)
int aether_ui_fire_appearance(int dark) { return 0; }  // TODO(ios)
int aether_ui_fire_redo(void) { return 0; }  // TODO(ios)
int aether_ui_fire_row_drop(int row_handle, int src_index) { return 0; }  // TODO(ios)
int aether_ui_fire_scroll(int container_handle, int dy) { return 0; }  // TODO(ios)
int aether_ui_fire_undo(void) { return 0; }  // TODO(ios)
void aether_ui_menu_add_item(int menu_handle, const char* label, void* boxed_closure) { }  // TODO(ios)
void aether_ui_menu_add_separator(int menu_handle) { }  // TODO(ios)
void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) { }  // TODO(ios)
void aether_ui_menu_bar_attach(int app_handle, int bar_handle) { }  // TODO(ios)
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) { }  // TODO(ios)
int aether_ui_menu_bar_create(void) { return 0; }  // TODO(ios)
int aether_ui_menu_create(const char* label) { return 0; }  // TODO(ios)
void aether_ui_menu_popup(int menu_handle, int anchor_widget) { }  // TODO(ios)
int aether_ui_notify_full_impl(const char* title, const char* body, const char* icon_path, const char* tag, void* boxed_click) { return 0; }  // TODO(ios)
int aether_ui_notify_impl(const char* title, const char* body) { return 0; }  // TODO(ios)
int aether_ui_notify_request_permission_impl(void) { return 0; }  // TODO(ios)
void aether_ui_on_layout_impl(int handle, void* boxed_closure) { }  // TODO(ios)
void aether_ui_row_drag_reorder_impl(int row_handle, int index, void* on_drop_closure) { }  // TODO(ios)
void aether_ui_set_state_style(int handle, int state, double br, double bg_, double bb, double fr, double fg_, double fb) { }  // TODO(ios)
void aether_ui_shortcut_chord_impl(const char* first_combo, const char* second_combo, void* boxed_closure) { }  // TODO(ios)
void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) { }  // TODO(ios)
void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure, void* enabled_closure) { }  // TODO(ios)
int aether_ui_state_style_impl(int handle, int state) { return 0; }  // TODO(ios)
int aether_ui_styled_bg_impl(int handle) { return 0; }  // TODO(ios)
int aether_ui_styled_border_impl(int handle) { return 0; }  // TODO(ios)
int aether_ui_styled_fg_impl(int handle) { return 0; }  // TODO(ios)
const char* aether_ui_styled_font_family_impl(int handle) { return ""; }  // TODO(ios)
int aether_ui_styled_opacity_impl(int handle) { return 0; }  // TODO(ios)
const char* aether_ui_styled_weight_impl(int handle) { return ""; }  // TODO(ios)
void aether_ui_toggle_set_group(int handle, int group_with) { }  // TODO(ios)
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) { return 0; }  // TODO(ios)
void aether_ui_tray_seal_impl(int tray_id) { }  // TODO(ios)
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle, const char* icon_clean, const char* icon_busy, const char* icon_alert) { }  // TODO(ios)
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) { }  // TODO(ios)
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) { }  // TODO(ios)
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) { }  // TODO(ios)
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) { }  // TODO(ios)
void aether_ui_watch_appearance_impl(void) { }  // TODO(ios)
void aether_ui_widget_apply_css_impl(int handle, const char* property_css) { }  // TODO(ios)
const char* aether_ui_widget_drag_payload_impl(int handle) { return ""; }  // TODO(ios)
void aether_ui_widget_draggable_file_impl(int handle, const char* path) { }  // TODO(ios)
int aether_ui_wrap_create(void) { return 0; }  // TODO(ios)







