// Aether UI — Android backend (android.widget Views over JNI)
// ===========================================================================
//
// The fifth native implementation of the shared backend ABI
// (backend/aether_ui_backend.h), beside GTK4, AppKit, Win32 and UIKit. The
// design is docs/design/android-backend.md: real android.widget Views, made
// and driven from C through JNI; a small Java shim (backend/android/) that
// owns the Activity and carries listener callbacks back; the app's Aether
// code, libaether, this file and the shared driver linked into one shared
// library (`ae build --target=aarch64-linux-android --emit=lib`); an APK
// assembled without Gradle (tools/android-apk.sh).
//
// It is a sibling of aether_ui_uikit.m (the other mobile backend) and follows
// its shape where Android allows: a single window per activity, a main()
// that returns into the platform's loop instead of blocking in its own, and
// shared semantics kept above the ABI (ui/module.ae,
// aether_ui_system_extras.c, the shared AetherUIDriver server) so this file
// translates and never interprets.
//
// THREADS. Android owns the UI thread's loop. The app's main() runs from
// AetherActivity.onCreate on that thread, builds its widget tree, and
// returns. Anything that must reach the UI thread from elsewhere -- a
// driver request, a std.worker completion, a timer -- arrives through a pipe
// registered with the UI thread's ALooper (aeui_android_post /
// aeui_android_run_sync); timers are timerfds on the same looper. Every JNI
// call that touches a View therefore happens on the UI thread.
//
// STATUS: STAGE 2, PASS A -- 168 of the 327 ABI functions the UIKit backend
// exports are implemented for real; the other 159 are STUBS. Stage 1 proved
// the chain (`--emit=lib`, JNI, the looper bridge, packaging, the driver over
// `adb forward`) with examples/counter; pass A brought the widget set that
// most apps are made of: inputs, containers, images, styling, sizing,
// accessibility, events and bindings. Passes B and C replace the rest, with
// the spec matrix as the ratchet, until none remain (the backend-parity
// rule).
//
// Real:
//   registry   register_widget get_widget handle_for_widget backend_name_impl
//              widget_kind_impl widget_parent_impl widget_window_impl
//   lifecycle  app_create app_set_body app_run_raw app_run_headless_impl
//              surface_container_new_impl surface_run_impl
//              surface_note_interactive_impl surface_diag_count_impl
//              register_deferred_flush_impl surface_flush_deferred_impl
//   layout     vstack_create hstack_create spacer_create divider_create
//              widget_add_child_ctx remove_child_impl clear_children_impl
//              grid_create grid_place grid_set_uniform form_create
//              form_section_create scrollview_create
//   sizing     set_width set_height set_width_impl set_height_impl
//              set_min_width_impl set_min_height_impl get_width_impl
//              get_height_impl get_min_width_impl get_min_height_impl
//              match_parent_width match_parent_height set_margin
//              set_margin_ctx set_edge_insets set_alignment set_distribution
//              set_rtl
//   text       text_create text_wrapped_create text_set_string
//              text_set_anchor text_set_truncate text_get_wrap
//              text_get_anchor text_get_truncate text_measure font_ascent
//              font_descent font_height
//   button     button_create button_create_plain button_set_label
//              button_set_flat button_set_flat_ctx button_set_disclosure
//              button_set_disclosure_ctx set_onclick_ctx
//   inputs     textfield_create textfield_set_text textfield_get_text
//              securefield_create textarea_create textarea_set_text
//              textarea_get_text placeholder_impl toggle_create
//              toggle_set_active toggle_get_active toggle_set_group
//              slider_create slider_set_value slider_get_value
//              picker_create picker_add_item picker_set_selected
//              picker_get_selected progressbar_create
//              progressbar_set_fraction
//   image      image_create image_from_bytes image_set_size image_set_fill
//              image_get_fill image_set_tint image_get_tint
//              image_has_content
//   style      set_bg_color set_bg_color_ctx set_bg_gradient set_border
//              set_corner_radius set_corner_radius_ctx set_opacity
//              set_opacity_ctx set_text_color set_text_color_ctx
//              set_font_size set_font_size_ctx set_font_bold
//              set_font_bold_ctx set_font_family set_state_style
//              set_tooltip set_tooltip_ctx set_enabled set_enabled_ctx
//              widget_set_hidden, and the readbacks styled_bg_impl
//              styled_fg_impl styled_opacity_impl styled_border_impl
//              styled_weight_impl styled_font_family_impl state_style_impl
//   a11y       a11y_set_role_impl a11y_set_label_impl
//              a11y_set_description_impl a11y_get_impl
//   events     on_click_impl on_double_click_impl on_hover_impl
//              on_layout_impl set_focusable_impl focus_impl focused_widget
//              modifiers_impl
//   state      state_create state_create_s state_create_i state_create_b
//              state_get state_get_s state_get_i state_get_b state_type
//              state_set state_set_s state_set_i state_set_b
//              state_create_list state_get_list state_set_list
//              state_list_rev state_bind_text bind_text_impl
//              bind_enabled_impl bind_hidden_impl bind_value
//   driver     enable_test_server_impl enable_test_server_ctx
//              window_count_impl window_is_open_impl window_title_impl;
//              hooks for enumeration, children, geometry (dp), enabled,
//              text, toggle/slider/progress values, focus, a11y, hovered and
//              pressed, click, set_text, toggle, set_value, focus, hover,
//              press/release, state, the "Under Remote Control" banner, and
//              GET /screenshot (the decor view drawn to a PNG)
//   loop       worker_poster_install_impl on_ui_thread_impl
//              timer_create_impl timer_cancel_impl
//
// Where Android has no counterpart, the closest faithful behaviour is
// implemented and the comment at the function says so: hover is real only
// under a pointer (mouse, trackpad, hovering stylus; a finger never hovers),
// modifiers_impl is 0 (no pollable modifier state, as on UIKit), the
// disclosure triangle is drawn as a path (no system chevron).
//
// STUBS (each calls aeui_android_unimplemented(__func__), which logs the
// name once through liblog, and returns a neutral value: 0, 0.0, NULL, or a
// fresh empty string). The full list is generated into the STUBS section at
// the end of this file; by family:
//   alert      alert_impl
//   app        app_quit_impl
//   canvas     canvas_arc_impl canvas_begin_path_impl canvas_clear_impl
//              canvas_clip_rect_impl canvas_close_path_impl
//              canvas_cmd_count_impl canvas_create_impl
//              canvas_draw_image_borrowed_impl canvas_draw_image_impl
//              canvas_draw_image_impl_ptr
//              canvas_draw_image_scaled_borrowed_impl
//              canvas_draw_image_scaled_impl canvas_draw_image_scaled_impl_ptr
//              canvas_fill_impl canvas_fill_linear_gradient_impl
//              canvas_fill_radial_gradient_impl canvas_fill_rect_impl
//              canvas_fill_text_impl canvas_gesture_probe_impl
//              canvas_get_widget canvas_group_begin_impl canvas_group_end_impl
//              canvas_line_to_impl canvas_move_to_impl canvas_on_click_impl
//              canvas_on_key_impl canvas_on_key_release_impl
//              canvas_on_move_impl canvas_on_release_impl canvas_on_resize_impl
//              canvas_on_scroll_impl canvas_painted_pixels_impl
//              canvas_read_pixel_impl canvas_redraw_impl
//              canvas_render_range_rgba_impl canvas_reset_clip_impl
//              canvas_set_clip_rects_impl canvas_stroke_impl
//              canvas_stroke_text_impl canvas_write_png_impl
//   clipboard  clipboard_read_impl clipboard_write_impl
//   close      close_window_by_handle_impl
//   context    context_menu_item_accel_impl context_menu_item_impl
//   dark       dark_mode_check
//   file       file_icon_create file_icon_set file_open file_pick_folder
//              file_save
//   fire       fire_appearance fire_double_click fire_redo fire_row_drop
//              fire_scroll fire_undo
//   gpuview    gpuview_available_impl gpuview_create_impl gpuview_get_widget
//              gpuview_on_realize_impl gpuview_on_render_impl
//              gpuview_on_resize_impl gpuview_read_pixel_impl
//              gpuview_request_render_impl
//   menu       menu_add_item menu_add_separator menu_bar_add_menu
//              menu_bar_attach menu_bar_attach_window menu_bar_create
//              menu_create menu_item_set_label menu_popup
//   native     native_list_available_impl native_list_create_impl
//              native_list_first_visible_impl native_list_scroll_to_impl
//              native_list_set_count_impl native_list_set_row_builder_impl
//              native_view_available_impl native_view_create_impl
//              native_view_get_widget native_view_handle_impl
//              native_view_kind_impl native_view_on_realize_impl
//              native_view_on_resize_impl
//   navstack   navstack_create navstack_depth navstack_pop navstack_push
//   notify     notify_full_impl notify_impl notify_request_permission_impl
//   open       open_url_impl
//   overlay    overlay_close_impl overlay_count_impl overlay_exit_played_impl
//              overlay_is_exiting_impl overlay_is_live_impl
//              overlay_is_modal_impl overlay_material_effective_impl
//              overlay_open_impl overlay_set_material_impl
//              overlay_set_on_dismiss_impl overlay_set_transition_impl
//   row        row_drag_reorder_impl
//   seal       seal_subtree_impl seal_widget_impl
//   sheet      sheet_create_impl sheet_dismiss_impl sheet_present_impl
//              sheet_set_body_impl
//   shortcut   shortcut_chord_impl shortcut_impl shortcut_when_impl
//   split      split_position_impl split_set_position_impl
//   splitview  splitview_create
//   tab        tab_add
//   tabs       tabs_count tabs_create tabs_select tabs_selected
//              tabs_set_on_change
//   toast      toast_impl
//   tray       tray_create_impl tray_seal_impl tray_set_icon_for_state_impl
//              tray_set_icon_template_impl tray_set_menu_impl
//              tray_set_tooltip_impl
//   vg         vg_tooltip_drawn_impl vg_tooltip_hide_impl vg_tooltip_show_impl
//   vlist      vlist_attach_scroll_impl
//   watch      watch_appearance_impl
//   widget     widget_add_css_class_impl widget_apply_css_impl
//              widget_classes_impl widget_count_impl widget_drag_payload_impl
//              widget_draggable_file_impl widget_remove_css_class_impl
//              widget_set_child_impl widget_weight_impl
//   window     window_close_impl window_create_impl window_file_drop_deliver
//              window_key_deliver window_on_file_drop_impl window_on_key_impl
//              window_set_body_impl window_set_title_impl window_show_impl
//   wrap       wrap_create
//   zstack     zstack_create
//
// Limitations beyond the stubs: there is one window; Back finishes the
// activity, which ends the program as closing a desktop window does; a
// container that is not yet real (zstack, wrap, tabs, navstack, splitview)
// takes no children, which the log names.
// ===========================================================================

#include <jni.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "aether_ui_backend.h"
#include "aether_ui_test_server.h"    // AetherDriverHooks + aether_ui_test_server_start
#include "aether_ui_system_extras.h"  // aether_ui_state_notify (observers live above the ABI)

#define AEUI_TAG "aether-ui"
#define AEUI_LOGI(...) __android_log_print(ANDROID_LOG_INFO, AEUI_TAG, __VA_ARGS__)
#define AEUI_LOGW(...) __android_log_print(ANDROID_LOG_WARN, AEUI_TAG, __VA_ARGS__)
#define AEUI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, AEUI_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Closure struct -- must match Aether codegen's _AeClosure layout (identical
// to every other backend). A boxed closure is called by casting fn to the
// arity the call site needs and passing env first.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

static void aeui_call0(AeClosure* c) {
    if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
}

// ---------------------------------------------------------------------------
// Stubs report themselves once each. A stub is reached on every /widgets
// request or every frame in some apps, so logging each call would bury the
// log; once per name says what an app needs from the next pass.
// ---------------------------------------------------------------------------
static pthread_mutex_t aeui_unimpl_lock = PTHREAD_MUTEX_INITIALIZER;
static const char* aeui_unimpl_seen[512];
static int aeui_unimpl_count = 0;

static void aeui_android_unimplemented(const char* fn) {
    pthread_mutex_lock(&aeui_unimpl_lock);
    for (int i = 0; i < aeui_unimpl_count; i++) {
        if (aeui_unimpl_seen[i] == fn) { pthread_mutex_unlock(&aeui_unimpl_lock); return; }
    }
    if (aeui_unimpl_count < (int)(sizeof(aeui_unimpl_seen) / sizeof(aeui_unimpl_seen[0])))
        aeui_unimpl_seen[aeui_unimpl_count++] = fn;
    pthread_mutex_unlock(&aeui_unimpl_lock);
    AEUI_LOGW("unimplemented on Android (stage 1 stub): %s", fn);
}

// The neutral string a stub returns. Fresh each time: some string-returning
// ABI functions hand ownership to the caller, which frees it, so a literal
// would be a crash there; a leaked empty string is the safe side.
static char* aeui_empty_string(void) {
    return strdup("");
}

// ---------------------------------------------------------------------------
// stdout/stderr -> logcat. An Android app's standard streams go nowhere;
// the driver's "listening on" line, Aether's println and every backend's
// fprintf(stderr, ...) diagnostics should land where `adb logcat` shows them.
// ---------------------------------------------------------------------------
static int aeui_stdio_pipe[2] = { -1, -1 };

static void* aeui_stdio_pump(void* arg) {
    (void)arg;
    char buf[1024];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(aeui_stdio_pipe[0], buf + used, sizeof(buf) - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = '\0';
        char* start = buf;
        char* nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            __android_log_write(ANDROID_LOG_INFO, AEUI_TAG "-stdio", start);
            start = nl + 1;
        }
        used = strlen(start);
        if (used == sizeof(buf) - 1) {   // a line longer than the buffer
            __android_log_write(ANDROID_LOG_INFO, AEUI_TAG "-stdio", start);
            used = 0;
        } else {
            memmove(buf, start, used);
        }
    }
    return NULL;
}

static void aeui_redirect_stdio(void) {
    if (aeui_stdio_pipe[0] >= 0) return;
    if (pipe(aeui_stdio_pipe) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(aeui_stdio_pipe[1], STDOUT_FILENO);
    dup2(aeui_stdio_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, NULL, aeui_stdio_pump, NULL) == 0) pthread_detach(t);
}

// ===========================================================================
// JNI plumbing
// ===========================================================================
static JavaVM* g_vm = NULL;
static jobject g_activity = NULL;     // global ref: the Context every View needs
static jobject g_host = NULL;         // global ref: the FrameLayout the body mounts in
static float g_density = 1.0f;

// Classes and members, resolved once in JNI_OnLoad (where the app's class
// loader is in effect, so the shim's own classes are findable).
static struct {
    jclass View, ViewGroup, LinearLayout, LinearLayoutParams, FrameLayoutParams,
           TextView, Button, Activity, String, CharSequence, Listener, A11y;
    jmethodID View_init, View_setLayoutParams, View_setPadding,
              View_getVisibility, View_isEnabled, View_performClick,
              View_setBackgroundColor, View_setOnClickListener,
              View_getLocationInWindow, View_getWidth, View_getHeight,
              View_getParent;
    jmethodID ViewGroup_addView, ViewGroup_removeView;
    jmethodID LinearLayout_init, LinearLayout_setOrientation;
    jmethodID LinearLayoutParams_init, MarginLayoutParams_setMargins;
    jmethodID FrameLayoutParams_init;
    jmethodID TextView_init, TextView_setText, TextView_getText, TextView_setAllCaps;
    jmethodID Button_init;
    jmethodID Activity_setTitle;
    jmethodID String_initBytes, String_getBytes;
    jmethodID Object_toString;
    jmethodID Listener_init, Listener_initDouble, A11y_init;
    // GET /screenshot: the decor view drawn into a Bitmap, compressed to PNG.
    jclass Bitmap, Canvas, ByteArrayOutputStream;
    jmethodID Activity_getWindow, Window_getDecorView, View_draw;
    jmethodID Bitmap_createBitmap, Bitmap_compress, Bitmap_recycle;
    jmethodID Canvas_init, Canvas_drawColor;
    jmethodID BAOS_init, BAOS_toByteArray;
    jobject ARGB_8888, PNG;   // global refs to the enum constants
} J;

static JNIEnv* aeui_env(void) {
    JNIEnv* env = NULL;
    if (!g_vm) return NULL;
    jint r = (*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
    }
    return env;
}

// A pending Java exception would make the next JNI call undefined; report it
// with the call that raised it and carry on.
static int aeui_check(JNIEnv* env, const char* what) {
    if ((*env)->ExceptionCheck(env)) {
        AEUI_LOGE("Java exception in %s", what);
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

static jclass aeui_find_class(JNIEnv* env, const char* name) {
    jclass local = (*env)->FindClass(env, name);
    if (!local) { aeui_check(env, name); AEUI_LOGE("class %s not found", name); return NULL; }
    jclass global = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return global;
}

// UTF-8 C string -> java.lang.String, through new String(bytes, "UTF-8").
// NewStringUTF takes MODIFIED UTF-8, which mangles anything outside the BMP
// (an emoji in a label), so it is not used for user text.
static jstring aeui_jstring(JNIEnv* env, const char* s) {
    if (!s) s = "";
    jsize n = (jsize)strlen(s);
    jbyteArray bytes = (*env)->NewByteArray(env, n);
    if (!bytes) { aeui_check(env, "NewByteArray"); return NULL; }
    (*env)->SetByteArrayRegion(env, bytes, 0, n, (const jbyte*)s);
    jstring charset = (*env)->NewStringUTF(env, "UTF-8");
    jstring str = (jstring)(*env)->NewObject(env, J.String, J.String_initBytes, bytes, charset);
    aeui_check(env, "new String(bytes, UTF-8)");
    (*env)->DeleteLocalRef(env, bytes);
    (*env)->DeleteLocalRef(env, charset);
    return str;
}

// java CharSequence -> UTF-8 into buf (truncated to bufsize-1).
static void aeui_charseq_into(JNIEnv* env, jobject cs, char* buf, int bufsize) {
    buf[0] = '\0';
    if (!cs || bufsize <= 0) return;
    jstring str = (jstring)(*env)->CallObjectMethod(env, cs, J.Object_toString);
    if (aeui_check(env, "CharSequence.toString") || !str) return;
    jstring charset = (*env)->NewStringUTF(env, "UTF-8");
    jbyteArray bytes = (jbyteArray)(*env)->CallObjectMethod(env, str, J.String_getBytes, charset);
    if (!aeui_check(env, "String.getBytes") && bytes) {
        jsize n = (*env)->GetArrayLength(env, bytes);
        if (n > bufsize - 1) n = bufsize - 1;
        (*env)->GetByteArrayRegion(env, bytes, 0, n, (jbyte*)buf);
        buf[n] = '\0';
        (*env)->DeleteLocalRef(env, bytes);
    }
    (*env)->DeleteLocalRef(env, charset);
    (*env)->DeleteLocalRef(env, str);
}

static int aeui_dp(int v) {
    return (int)(v * g_density + (v >= 0 ? 0.5f : -0.5f));
}

// Device pixels -> dp, the unit every other backend reports in (points on
// AppKit/UIKit, logical pixels on GTK4/Win32): what the driver, get_width
// and on_layout answer with, so a spec's numbers mean one thing everywhere.
static int aeui_px_to_dp(double px) {
    return (int)floor(px / g_density + 0.5);
}

// java CharSequence -> a malloc'd UTF-8 copy (never NULL).
static char* aeui_charseq_dup(JNIEnv* env, jobject cs) {
    if (!cs) return strdup("");
    jstring str = (jstring)(*env)->CallObjectMethod(env, cs, J.Object_toString);
    if (aeui_check(env, "CharSequence.toString") || !str) return strdup("");
    jstring charset = (*env)->NewStringUTF(env, "UTF-8");
    jbyteArray bytes = (jbyteArray)(*env)->CallObjectMethod(env, str, J.String_getBytes, charset);
    char* out = NULL;
    if (!aeui_check(env, "String.getBytes") && bytes) {
        jsize n = (*env)->GetArrayLength(env, bytes);
        out = (char*)malloc((size_t)n + 1);
        if (out) {
            (*env)->GetByteArrayRegion(env, bytes, 0, n, (jbyte*)out);
            out[n] = '\0';
        }
        (*env)->DeleteLocalRef(env, bytes);
    }
    (*env)->DeleteLocalRef(env, charset);
    (*env)->DeleteLocalRef(env, str);
    return out ? out : strdup("");
}

// ---------------------------------------------------------------------------
// Lazily resolved classes and members. Stage 1 resolved everything in
// JNI_OnLoad (the J struct above); the widget set needs a few hundred more,
// so each is declared where it is used and resolved on first use. Framework
// classes resolve from any thread with a Java frame on the stack (the UI
// thread always has one); the shim's own classes are resolved in JNI_OnLoad,
// the only place the app's class loader is guaranteed to be in effect.
//
// A member that does not resolve is logged once and the call is skipped
// rather than made through a NULL id, which would abort the process.
// ---------------------------------------------------------------------------
typedef struct { const char* name; jclass cls; } AeuiJClass;
enum { JK_METHOD, JK_STATIC, JK_FIELD, JK_SFIELD };
typedef struct { AeuiJClass* c; const char* name; const char* sig; int kind; void* id; int failed; } AeuiJMember;

static jclass jcls(JNIEnv* env, AeuiJClass* c) {
    if (!c->cls) c->cls = aeui_find_class(env, c->name);
    return c->cls;
}

static void* jmem(JNIEnv* env, AeuiJMember* m) {
    if (m->id || m->failed) return m->id;
    jclass c = jcls(env, m->c);
    if (!c) { m->failed = 1; return NULL; }
    switch (m->kind) {
        case JK_METHOD: m->id = (void*)(*env)->GetMethodID(env, c, m->name, m->sig); break;
        case JK_STATIC: m->id = (void*)(*env)->GetStaticMethodID(env, c, m->name, m->sig); break;
        case JK_FIELD:  m->id = (void*)(*env)->GetFieldID(env, c, m->name, m->sig); break;
        default:        m->id = (void*)(*env)->GetStaticFieldID(env, c, m->name, m->sig); break;
    }
    if (!m->id) {
        aeui_check(env, m->name);
        AEUI_LOGE("JNI: no %s.%s %s", m->c->name, m->name, m->sig);
        m->failed = 1;
    }
    return m->id;
}

#define JCLASS(var, path)            static AeuiJClass var = { path, NULL }
#define JMETHOD(var, cls, name, sig) static AeuiJMember var = { &cls, name, sig, JK_METHOD, NULL, 0 }
#define JSTATIC(var, cls, name, sig) static AeuiJMember var = { &cls, name, sig, JK_STATIC, NULL, 0 }
#define JFIELD(var, cls, name, sig)  static AeuiJMember var = { &cls, name, sig, JK_FIELD, NULL, 0 }
#define JSFIELD(var, cls, name, sig) static AeuiJMember var = { &cls, name, sig, JK_SFIELD, NULL, 0 }

// Calls through a member: skipped (zero result) when the object or the
// member is missing, and any Java exception is reported and cleared.
#define JV(o, m, ...) do { jobject o_ = (o); jmethodID id_ = (jmethodID)jmem(env, &(m)); \
    if (o_ && id_) { (*env)->CallVoidMethod(env, o_, id_, ##__VA_ARGS__); aeui_check(env, (m).name); } } while (0)
#define JZ(o, m, ...) ({ jobject o_ = (o); jmethodID id_ = (jmethodID)jmem(env, &(m)); jboolean r_ = JNI_FALSE; \
    if (o_ && id_) { r_ = (*env)->CallBooleanMethod(env, o_, id_, ##__VA_ARGS__); aeui_check(env, (m).name); } r_; })
#define JI(o, m, ...) ({ jobject o_ = (o); jmethodID id_ = (jmethodID)jmem(env, &(m)); jint r_ = 0; \
    if (o_ && id_) { r_ = (*env)->CallIntMethod(env, o_, id_, ##__VA_ARGS__); aeui_check(env, (m).name); } r_; })
#define JF(o, m, ...) ({ jobject o_ = (o); jmethodID id_ = (jmethodID)jmem(env, &(m)); jfloat r_ = 0; \
    if (o_ && id_) { r_ = (*env)->CallFloatMethod(env, o_, id_, ##__VA_ARGS__); aeui_check(env, (m).name); } r_; })
#define JO(o, m, ...) ({ jobject o_ = (o); jmethodID id_ = (jmethodID)jmem(env, &(m)); jobject r_ = NULL; \
    if (o_ && id_) { r_ = (*env)->CallObjectMethod(env, o_, id_, ##__VA_ARGS__); if (aeui_check(env, (m).name)) r_ = NULL; } r_; })
#define JSO(m, ...) ({ jmethodID id_ = (jmethodID)jmem(env, &(m)); jobject r_ = NULL; \
    if (id_) { r_ = (*env)->CallStaticObjectMethod(env, (m).c->cls, id_, ##__VA_ARGS__); if (aeui_check(env, (m).name)) r_ = NULL; } r_; })
#define JNEW(m, ...) ({ jmethodID id_ = (jmethodID)jmem(env, &(m)); jobject r_ = NULL; \
    if (id_) { r_ = (*env)->NewObject(env, (m).c->cls, id_, ##__VA_ARGS__); if (aeui_check(env, (m).name)) r_ = NULL; } r_; })
#define JSFO(f) ({ jfieldID id_ = (jfieldID)jmem(env, &(f)); \
    id_ ? (*env)->GetStaticObjectField(env, (f).c->cls, id_) : NULL; })
#define JSFI(f) ({ jfieldID id_ = (jfieldID)jmem(env, &(f)); \
    id_ ? (*env)->GetStaticIntField(env, (f).c->cls, id_) : 0; })
#define JSETI(o, f, v) do { jobject o_ = (o); jfieldID id_ = (jfieldID)jmem(env, &(f)); \
    if (o_ && id_) (*env)->SetIntField(env, o_, id_, (jint)(v)); } while (0)
#define JGETF(o, f) ({ jobject o_ = (o); jfieldID id_ = (jfieldID)jmem(env, &(f)); \
    (o_ && id_) ? (*env)->GetFloatField(env, o_, id_) : 0.0f; })

// A JNI local frame around a unit of work: every local reference made inside
// is released at aeui_unframe, so a driver request walking hundreds of
// widgets cannot exhaust the local reference table.
static JNIEnv* aeui_frame(int capacity) {
    JNIEnv* env = aeui_env();
    if (!env) return NULL;
    if ((*env)->PushLocalFrame(env, capacity) != 0) { aeui_check(env, "PushLocalFrame"); return NULL; }
    return env;
}
static void aeui_unframe(JNIEnv* env) {
    (*env)->PopLocalFrame(env, NULL);
}

// The framework's public resource ids (android.R.attr.state_pressed, ...),
// read from android.R rather than written in as numbers.
static int aeui_android_r(JNIEnv* env, const char* group, const char* name) {
    char cls[64];
    snprintf(cls, sizeof(cls), "android/R$%s", group);
    jclass c = (*env)->FindClass(env, cls);
    if (!c) { aeui_check(env, cls); return 0; }
    jfieldID f = (*env)->GetStaticFieldID(env, c, name, "I");
    int v = 0;
    if (f) v = (*env)->GetStaticIntField(env, c, f);
    else aeui_check(env, name);
    (*env)->DeleteLocalRef(env, c);
    return v;
}

// ===========================================================================
// The looper bridge: work posted from any thread runs on the UI thread.
// ===========================================================================
typedef struct {
    void (*fn)(void*);
    void* arg;
    sem_t* done;     // run_sync: posted when fn has returned; NULL = async (job is freed)
} AeuiJob;

static ALooper* g_looper = NULL;
static int g_bridge[2] = { -1, -1 };
static pthread_t g_ui_thread;
static int g_ui_thread_known = 0;

static int aeui_on_ui_thread(void) {
    return g_ui_thread_known && pthread_equal(pthread_self(), g_ui_thread);
}

static int aeui_bridge_cb(int fd, int events, void* data) {
    (void)events; (void)data;
    AeuiJob* job;
    for (;;) {
        ssize_t n = read(fd, &job, sizeof(job));
        if (n != (ssize_t)sizeof(job)) break;   // drained (fd is non-blocking)
        job->fn(job->arg);
        if (job->done) sem_post(job->done);
        else free(job);
    }
    return 1;   // keep the fd registered
}

static int aeui_bridge_install(void) {
    if (g_looper) return 1;
    g_looper = ALooper_forThread();
    if (!g_looper) { AEUI_LOGE("no ALooper on the UI thread"); return 0; }
    ALooper_acquire(g_looper);
    if (pipe(g_bridge) != 0) { AEUI_LOGE("bridge pipe: %s", strerror(errno)); return 0; }
    fcntl(g_bridge[0], F_SETFL, fcntl(g_bridge[0], F_GETFL) | O_NONBLOCK);
    ALooper_addFd(g_looper, g_bridge[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                  aeui_bridge_cb, NULL);
    g_ui_thread = pthread_self();
    g_ui_thread_known = 1;
    return 1;
}

// A pointer is 8 bytes, far under PIPE_BUF, so each write is atomic and
// jobs from concurrent posters never interleave.
static int aeui_bridge_write(AeuiJob* job) {
    for (;;) {
        ssize_t n = write(g_bridge[1], &job, sizeof(job));
        if (n == (ssize_t)sizeof(job)) return 1;
        if (n < 0 && errno == EINTR) continue;
        AEUI_LOGE("bridge write failed: %s", strerror(errno));
        return 0;
    }
}

static void aeui_android_post(void (*fn)(void*), void* arg) {
    AeuiJob* job = (AeuiJob*)malloc(sizeof(AeuiJob));
    if (!job) return;
    job->fn = fn; job->arg = arg; job->done = NULL;
    if (!aeui_bridge_write(job)) free(job);
}

// Run fn(arg) on the UI thread and block until it has returned.
static void aeui_android_run_sync(void (*fn)(void*), void* arg) {
    if (aeui_on_ui_thread() || g_bridge[1] < 0) { fn(arg); return; }
    sem_t done;
    sem_init(&done, 0, 0);
    AeuiJob job = { fn, arg, &done };
    if (aeui_bridge_write(&job)) {
        while (sem_wait(&done) != 0 && errno == EINTR) { }
    }
    sem_destroy(&done);
}


// ===========================================================================
// Widget registry -- JNI global references to Views, 1-based handles. The
// record beside each View holds what the layout, the styling and the driver
// need without a JNI round trip: the type tag, the registered parent, the
// size and expansion the DSL asked for, the style that was APPLIED (the
// styled_* readbacks report it), the closures the View's listeners run.
// ===========================================================================
enum {
    AUI_UNKNOWN = 0,
    AUI_TEXT, AUI_BUTTON, AUI_TOGGLE, AUI_SLIDER, AUI_PICKER,
    AUI_TEXTFIELD, AUI_SECUREFIELD, AUI_TEXTAREA,
    AUI_PROGRESSBAR, AUI_DIVIDER, AUI_SCROLLVIEW,
    AUI_VSTACK, AUI_HSTACK, AUI_ZSTACK, AUI_SPACER,
    AUI_CANVAS, AUI_IMAGE,
    AUI_TABS, AUI_NAVSTACK, AUI_SPLITVIEW, AUI_WRAP, AUI_GRID,
    AUI_FORM_SECTION, AUI_FORM_SECTION_INNER, AUI_BANNER
};

// Which Java listeners a View already carries (one of each per View).
enum { LST_CLICK = 1, LST_HOVER = 2, LST_LAYOUT = 4, LST_DOUBLE = 8, LST_TEXT = 16 };

typedef struct {
    jobject view;      // global ref, NULL once retired
    int type;
    int parent;        // registered parent handle, 0 = none
    int child_count;
    int spacing;       // stacks: the DSL's spacing (dp)

    // --- layout (dp) ---
    int lead;                  // the stack's spacing before this child (0 = first)
    int fixed_w, fixed_h;      // set_width/set_height/image_set_size, 0 = none
    int min_w, min_h;          // set_min_width/height floors, 0 = none
    int has_margin, mt, mr, mb, ml;   // set_margin on a leaf: outer margins
    int own_hexp, own_vexp;    // this widget asks for the slack (match_parent, a scroll area)
    int hexp, vexp;            // ... or something inside it does (GTK's expand propagation)
    int align;                 // stacks: 0 default, 1 start, 2 center, 3 end, 4 fill
    int distribution;          // stacks: 0 fill, 1 fill equally
    int row, col, rspan, cspan;          // a grid cell
    int grid_cols, grid_rsp, grid_csp, grid_uniform;   // a grid

    // --- text ---
    int wrap, anchor, truncate;
    char* text_override;       // a caption the display replaced (disclosure)
    char* placeholder;

    // --- style (what was APPLIED; the styled_* readbacks) ---
    int bg_set; unsigned int bg_argb;
    int grad_set; unsigned int grad1, grad2; int grad_vertical;
    double radius;
    double border_w; unsigned int border_argb;
    int hover_bg, active_bg;   // packed 0xRRGGBB, -1 none
    int flat;
    int bg_captured; jobject orig_bg;   // the theme's background, for restores
    int styled_bg, styled_fg, styled_opacity, styled_border;
    int bold;                  // -1 never set, else 0/1
    char* family;
    double font_size;          // 0 = theme default

    // --- accessibility ---
    char* role; char* a11y_name; char* a11y_desc;
    jobject a11y;              // global ref: the AetherA11y delegate, once made

    // --- behaviour ---
    int disabled;              // set_enabled(0) on THIS widget
    int listeners;
    AeClosure* change;         // textfield/toggle/slider/picker/textarea on_change
    AeClosure** clicks; int nclicks;
    AeClosure** hovers; int nhovers;
    AeClosure* dbl;
    AeClosure* layout_cb; int layout_w, layout_h;
    int bound_state;           // bind_value: the string state this field writes back
    double smin, smax;         // slider range
    char** items; int nitems; int selected;   // picker
    int radio_leader;          // toggle group
    int fill, tint;            // image: fill mode, tint (-1 none)
} AeuiWidget;

static AeuiWidget* widgets = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

static int register_widget_typed(JNIEnv* env, jobject view, int type) {
    if (!view) return 0;
    if (widget_count >= widget_capacity) {
        int cap = widget_capacity == 0 ? 64 : widget_capacity * 2;
        AeuiWidget* nw = (AeuiWidget*)realloc(widgets, sizeof(AeuiWidget) * (size_t)cap);
        if (!nw) return 0;
        widgets = nw;
        widget_capacity = cap;
    }
    AeuiWidget* w = &widgets[widget_count];
    memset(w, 0, sizeof(*w));
    w->view = (*env)->NewGlobalRef(env, view);
    w->type = type;
    w->rspan = w->cspan = 1;
    w->hover_bg = w->active_bg = -1;
    w->styled_bg = w->styled_fg = w->styled_opacity = w->styled_border = -1;
    w->bold = -1;
    w->tint = -1;
    w->fill = 1;   // contain, stated explicitly as on every backend
    widget_count++;
    return widget_count;
}

static AeuiWidget* widget_at(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return &widgets[handle - 1];
}

static AeuiWidget* live_widget(int handle) {
    AeuiWidget* w = widget_at(handle);
    return (w && w->view) ? w : NULL;
}

static jobject view_of(int handle) {
    AeuiWidget* w = widget_at(handle);
    return w ? w->view : NULL;
}

static int get_widget_type(int handle) {
    AeuiWidget* w = widget_at(handle);
    return w ? w->type : AUI_UNKNOWN;
}

static int aeui_is_textview(int type) {   // a TextView underneath
    switch (type) {
        case AUI_TEXT: case AUI_BUTTON: case AUI_TOGGLE: case AUI_TEXTFIELD:
        case AUI_SECUREFIELD: case AUI_TEXTAREA: case AUI_BANNER: return 1;
        default: return 0;
    }
}

static int aeui_is_edit(int type) {
    return type == AUI_TEXTFIELD || type == AUI_SECUREFIELD || type == AUI_TEXTAREA;
}

static int aeui_is_linear(int type) {
    return type == AUI_VSTACK || type == AUI_HSTACK ||
           type == AUI_FORM_SECTION || type == AUI_FORM_SECTION_INNER;
}

static int aeui_is_container(int type) {
    return aeui_is_linear(type) || type == AUI_GRID || type == AUI_SCROLLVIEW;
}

static void closure_push(AeClosure*** arr, int* n, AeClosure* c) {
    AeClosure** na = (AeClosure**)realloc(*arr, sizeof(AeClosure*) * (size_t)(*n + 1));
    if (!na) return;
    na[(*n)++] = c;
    *arr = na;
}

// Retire one widget and everything registered under it: the slot self-NULLs
// and the global reference goes, as every backend's handle does when its
// view is destroyed.
static void aeui_retire_tree(JNIEnv* env, int handle) {
    for (int i = 0; i < widget_count; i++)
        if (widgets[i].parent == handle && widgets[i].view) aeui_retire_tree(env, i + 1);
    AeuiWidget* w = widget_at(handle);
    if (!w || !w->view) return;
    (*env)->DeleteGlobalRef(env, w->view);
    w->view = NULL;
    if (w->orig_bg) { (*env)->DeleteGlobalRef(env, w->orig_bg); w->orig_bg = NULL; }
    if (w->a11y) { (*env)->DeleteGlobalRef(env, w->a11y); w->a11y = NULL; }
    w->parent = 0;
}

// The activity is finishing: every View goes with it, so every slot self-NULLs.
static void aeui_retire_all(JNIEnv* env) {
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i].view) {
            (*env)->DeleteGlobalRef(env, widgets[i].view);
            widgets[i].view = NULL;
        }
    }
}

int aether_ui_register_widget(void* widget) {
    JNIEnv* env = aeui_env();
    return env ? register_widget_typed(env, (jobject)widget, AUI_UNKNOWN) : 0;
}

void* aether_ui_get_widget(int handle) {
    return (void*)view_of(handle);
}

int aether_ui_handle_for_widget(void* widget) {
    if (!widget) return 0;
    JNIEnv* env = aeui_env();
    if (!env) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i].view && (*env)->IsSameObject(env, widgets[i].view, (jobject)widget))
            return i + 1;
    }
    return 0;
}

const char* aether_ui_backend_name_impl(void) { return "android"; }

// The kind vocabulary the desktop backends report. A grid answers "widget"
// as GtkGrid and NSGridView do (the scroll-area spec finds its document view
// by that name); a form is a vstack and its sections are AppKit's
// form_section / form_section_inner.
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
        case AUI_FORM_SECTION:       return "form_section";
        case AUI_FORM_SECTION_INNER: return "form_section_inner";
        case AUI_BANNER:      return "banner";
        default:              return "widget";
    }
}

const char* aether_ui_widget_kind_impl(int handle) {
    return aeui_kind_name(get_widget_type(handle));
}

int aether_ui_widget_parent_impl(int handle) {
    AeuiWidget* w = widget_at(handle);
    return w ? w->parent : 0;
}

// One window per activity: every live widget is in window 1.
int aether_ui_widget_window_impl(int widget_handle) {
    return view_of(widget_handle) ? 1 : 0;
}

// Classes the layout engine and the widgets below need.
JCLASS(C_View, "android/view/View");
JCLASS(C_ViewGroup, "android/view/ViewGroup");
JCLASS(C_LinearLayout, "android/widget/LinearLayout");
JCLASS(C_LLParams, "android/widget/LinearLayout$LayoutParams");
JCLASS(C_MarginParams, "android/view/ViewGroup$MarginLayoutParams");
JCLASS(C_VGParams, "android/view/ViewGroup$LayoutParams");
JCLASS(C_FrameParams, "android/widget/FrameLayout$LayoutParams");
JCLASS(C_GridLayout, "android/widget/GridLayout");
JCLASS(C_GridParams, "android/widget/GridLayout$LayoutParams");
JCLASS(C_ScrollView, "android/widget/ScrollView");
JCLASS(C_TextView, "android/widget/TextView");

JMETHOD(M_View_setLayoutParams, C_View, "setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V");
JMETHOD(M_View_setPadding, C_View, "setPadding", "(IIII)V");
JMETHOD(M_View_setMinimumWidth, C_View, "setMinimumWidth", "(I)V");
JMETHOD(M_View_setMinimumHeight, C_View, "setMinimumHeight", "(I)V");
JMETHOD(M_View_getParent, C_View, "getParent", "()Landroid/view/ViewParent;");
JMETHOD(M_View_getWidth, C_View, "getWidth", "()I");
JMETHOD(M_View_getHeight, C_View, "getHeight", "()I");
JMETHOD(M_View_getLocationInWindow, C_View, "getLocationInWindow", "([I)V");
JMETHOD(M_VG_addView, C_ViewGroup, "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
JMETHOD(M_VG_addViewIdx, C_ViewGroup, "addView", "(Landroid/view/View;I)V");
JMETHOD(M_VG_removeView, C_ViewGroup, "removeView", "(Landroid/view/View;)V");
JMETHOD(M_VG_getChildCount, C_ViewGroup, "getChildCount", "()I");
JMETHOD(M_VG_getChildAt, C_ViewGroup, "getChildAt", "(I)Landroid/view/View;");
JMETHOD(M_LL_init, C_LinearLayout, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_LL_setOrientation, C_LinearLayout, "setOrientation", "(I)V");
JMETHOD(M_LLP_init, C_LLParams, "<init>", "(IIF)V");
JFIELD(F_LLP_gravity, C_LLParams, "gravity", "I");
JFIELD(F_VGP_width, C_VGParams, "width", "I");
JFIELD(F_VGP_height, C_VGParams, "height", "I");
JMETHOD(M_MP_setMargins, C_MarginParams, "setMargins", "(IIII)V");
JMETHOD(M_MP_setMarginStart, C_MarginParams, "setMarginStart", "(I)V");
JMETHOD(M_MP_setMarginEnd, C_MarginParams, "setMarginEnd", "(I)V");
JMETHOD(M_FP_init, C_FrameParams, "<init>", "(II)V");
JMETHOD(M_GL_init, C_GridLayout, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_GL_setColumnCount, C_GridLayout, "setColumnCount", "(I)V");
JMETHOD(M_GL_getColumnCount, C_GridLayout, "getColumnCount", "()I");
JMETHOD(M_GL_setUseDefaultMargins, C_GridLayout, "setUseDefaultMargins", "(Z)V");
JSTATIC(M_GL_spec, C_GridLayout, "spec", "(II)Landroid/widget/GridLayout$Spec;");
JSTATIC(M_GL_specW, C_GridLayout, "spec", "(IILandroid/widget/GridLayout$Alignment;F)Landroid/widget/GridLayout$Spec;");
JSFIELD(F_GL_FILL, C_GridLayout, "FILL", "Landroid/widget/GridLayout$Alignment;");
JMETHOD(M_GLP_init, C_GridParams, "<init>", "(Landroid/widget/GridLayout$Spec;Landroid/widget/GridLayout$Spec;)V");
JMETHOD(M_SV_init, C_ScrollView, "<init>", "(Landroid/content/Context;)V");

// ===========================================================================
// App lifecycle and surfaces
// ===========================================================================
static int g_root_handle = 0;
static char* g_title = NULL;
static int g_started = 0;    // main() has run in this process
static int g_mounted = 0;    // the body is in the host

int aether_ui_app_create(const char* title, int width, int height) {
    (void)width; (void)height;   // the activity is the screen; no window size to ask for
    aether_ui_worker_poster_install_impl();
    free(g_title);
    g_title = strdup(title ? title : "");
    return 1;
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    (void)app_handle;
    g_root_handle = root_handle;
}

// Put the body in the activity's host layout and title the activity. Also
// the re-mount path when the system recreates the activity in a live process.
static void aeui_mount_body(JNIEnv* env) {
    jobject root = view_of(g_root_handle);
    if (!root || !g_host) return;
    jobject parent = (*env)->CallObjectMethod(env, root, J.View_getParent);
    aeui_check(env, "View.getParent");
    if (parent) {
        if ((*env)->IsInstanceOf(env, parent, J.ViewGroup))
            (*env)->CallVoidMethod(env, parent, J.ViewGroup_removeView, root);
        aeui_check(env, "ViewGroup.removeView");
        (*env)->DeleteLocalRef(env, parent);
    }
    // The window's content inset, as the desktop backends pad the window
    // body. A MARGIN around the body rather than its padding: padding is the
    // body's own, for margin() and edge_insets() to set.
    jobject lp = (*env)->NewObject(env, J.FrameLayoutParams, J.FrameLayoutParams_init,
                                   (jint)-1 /* MATCH_PARENT */, (jint)-1);
    int pad = aeui_dp(16);
    if (lp) {
        (*env)->CallVoidMethod(env, lp, J.MarginLayoutParams_setMargins, pad, pad, pad, pad);
        aeui_check(env, "setMargins");
    }
    (*env)->CallVoidMethod(env, g_host, J.ViewGroup_addView, root, lp);
    aeui_check(env, "host.addView");
    if (lp) (*env)->DeleteLocalRef(env, lp);
    if (g_title && g_activity) {
        jstring t = aeui_jstring(env, g_title);
        (*env)->CallVoidMethod(env, g_activity, J.Activity_setTitle, t);
        aeui_check(env, "Activity.setTitle");
        if (t) (*env)->DeleteLocalRef(env, t);
    }
    g_mounted = 1;
}

// Desktop backends block here in their loop. Android's loop is the
// platform's, already running on this thread: mount the body and return, so
// main() returns into it. The app lives on in the Views and the closures
// their listeners carry.
void aether_ui_app_run_raw(int app_handle) {
    (void)app_handle;
    JNIEnv* env = aeui_env();
    if (env) aeui_mount_body(env);
}

void aether_ui_app_run_headless_impl(void) { /* no UI loop under headless */ }

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

static void surface_add(int container_handle, int kind) {
    if (surface_count >= surface_capacity) {
        int cap = surface_capacity == 0 ? 4 : surface_capacity * 2;
        SurfaceEntry* ns = (SurfaceEntry*)realloc(surfaces, sizeof(SurfaceEntry) * (size_t)cap);
        if (!ns) return;
        surfaces = ns;
        surface_capacity = cap;
    }
    SurfaceEntry* s = &surfaces[surface_count++];
    s->container_handle = container_handle;
    s->kind = kind;
    s->app_handle = 0;
    s->diag_count = 0;
}

int aether_ui_surface_container_new_impl(int kind) {
    int container = aether_ui_vstack_create(0);
    if (container) surface_add(container, kind);
    return container;
}

static AeClosure** deferred_flushes = NULL;
static int deferred_flush_count = 0;
static int deferred_flush_capacity = 0;

void aether_ui_register_deferred_flush_impl(void* boxed_closure) {
    if (!boxed_closure) return;
    if (deferred_flush_count >= deferred_flush_capacity) {
        int cap = deferred_flush_capacity == 0 ? 4 : deferred_flush_capacity * 2;
        AeClosure** nd = (AeClosure**)realloc(deferred_flushes, sizeof(AeClosure*) * (size_t)cap);
        if (!nd) return;
        deferred_flushes = nd;
        deferred_flush_capacity = cap;
    }
    deferred_flushes[deferred_flush_count++] = (AeClosure*)boxed_closure;
}

void aether_ui_surface_flush_deferred_impl(void) {
    for (int i = 0; i < deferred_flush_count; i++) aeui_call0(deferred_flushes[i]);
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
    // Arm the driver when AETHER_UI_TEST_PORT is set, as every backend's
    // window(){} path does. On Android the variable arrives as an intent
    // extra (AetherActivity.exportEnvironment).
    const char* port_env = getenv("AETHER_UI_TEST_PORT");
    if (port_env) {
        int port = atoi(port_env);
        if (port > 0) aether_ui_enable_test_server_impl(port, container_handle);
    }
    aether_ui_app_run_raw(app);
}

int aether_ui_surface_note_interactive_impl(int container_handle) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s || s->kind == AUI_SURFACE_WINDOW) return 0;
    s->diag_count++;
    return 1;
}

int aether_ui_surface_diag_count_impl(int container_handle) {
    SurfaceEntry* s = surface_for_container(container_handle);
    return s ? s->diag_count : 0;
}

int aether_ui_window_count_impl(void) { return 1; }
int aether_ui_window_is_open_impl(int win_handle) { return win_handle == 1 && g_mounted ? 1 : 0; }
const char* aether_ui_window_title_impl(int win_handle) {
    (void)win_handle;
    return g_title ? g_title : "";
}

// ===========================================================================
// The layout engine. Every container is a real Android layout -- vstack and
// hstack a LinearLayout, grid a GridLayout, scrollview a ScrollView -- and a
// child's place in it is its LayoutParams, which are DERIVED, in one place
// (aeui_apply_lp), from what the DSL asked of the child and of its parent:
// fixed sizes, expansion, the parent's spacing, alignment and distribution,
// a grid cell. Every setter that changes one of those inputs re-derives,
// whenever it is called: the DSL assembles top-down (a container is in its
// parent before its children arrive), so nothing can be decided once at
// attach time.
//
// Units are dp. Expansion follows GTK4's model, the one the DSL was written
// against: a scroll area, a progress bar and a match_parent axis ask for the
// slack, and a container whose child asks for it asks too (hexp/vexp, the
// propagated value), unless a fixed size on that axis says otherwise. A
// spacer takes its stack's main-axis slack.
// ===========================================================================
enum { LL_HORIZONTAL = 0, LL_VERTICAL = 1 };
enum { LP_MATCH_PARENT = -1, LP_WRAP_CONTENT = -2 };
enum { GRAV_START = 0x00800003, GRAV_END = 0x00800005, GRAV_TOP = 0x30, GRAV_BOTTOM = 0x50,
       GRAV_CENTER_H = 0x01, GRAV_CENTER_V = 0x10, GRAV_CENTER = 0x11 };

static int aeui_is_vertical(int type) {
    return type != AUI_HSTACK;   // vstack, form section and its inner box
}

static void aeui_apply_lp(JNIEnv* env, int handle) {
    AeuiWidget* c = live_widget(handle);
    if (!c || !c->parent) return;
    AeuiWidget* p = live_widget(c->parent);
    if (!p) return;
    jobject lp = NULL;
    int ml = c->has_margin ? aeui_dp(c->ml) : 0, mr = c->has_margin ? aeui_dp(c->mr) : 0;
    int mt = c->has_margin ? aeui_dp(c->mt) : 0, mb = c->has_margin ? aeui_dp(c->mb) : 0;

    if (aeui_is_linear(p->type)) {
        int vertical = aeui_is_vertical(p->type);
        int main_fixed = vertical ? c->fixed_h : c->fixed_w;
        int cross_fixed = vertical ? c->fixed_w : c->fixed_h;
        int main_exp = vertical ? c->vexp : c->hexp;
        int cross_exp = vertical ? c->hexp : c->vexp;
        int main_sz, cross_sz, gravity = -1;
        float weight = 0.0f;
        if (c->type == AUI_SPACER) {
            main_sz = 0; weight = 1.0f;
        } else if (c->type == AUI_DIVIDER) {
            main_sz = aeui_dp(1);
        } else if (main_fixed > 0) {
            main_sz = aeui_dp(main_fixed);
        } else if (p->distribution == 1 || main_exp) {
            main_sz = 0; weight = 1.0f;   // equal shares / the slack
        } else {
            main_sz = LP_WRAP_CONTENT;
        }
        if (cross_fixed > 0) {
            cross_sz = aeui_dp(cross_fixed);
        } else if (c->type == AUI_SPACER || c->type == AUI_DIVIDER || cross_exp) {
            // Never WRAP_CONTENT for a bare View on the cross axis: one asked
            // to wrap measures to all the space offered, so a spacer in an
            // hstack stretched the row to the bottom of the screen.
            cross_sz = LP_MATCH_PARENT;
        } else if (vertical) {
            // A vstack's children fill its width unless it was aligned (as a
            // UIStackView / NSStackView with fill alignment does).
            cross_sz = (p->align == 0 || p->align == 4) ? LP_MATCH_PARENT : LP_WRAP_CONTENT;
        } else {
            cross_sz = p->align == 4 ? LP_MATCH_PARENT : LP_WRAP_CONTENT;
        }
        if (p->align >= 1 && p->align <= 3)
            gravity = vertical ? (p->align == 1 ? GRAV_START : p->align == 2 ? GRAV_CENTER_H : GRAV_END)
                               : (p->align == 1 ? GRAV_TOP : p->align == 2 ? GRAV_CENTER_V : GRAV_BOTTOM);
        lp = JNEW(M_LLP_init, (jint)(vertical ? cross_sz : main_sz),
                  (jint)(vertical ? main_sz : cross_sz), (jfloat)weight);
        if (!lp) return;
        int lead = aeui_dp(c->lead);
        JV(lp, M_MP_setMargins, ml, mt + (vertical ? lead : 0), mr, mb);
        // The leading edge in reading order, so an RTL row puts its spacing
        // (and its first child) on the right.
        JV(lp, M_MP_setMarginStart, ml + (vertical ? 0 : lead));
        JV(lp, M_MP_setMarginEnd, mr);
        if (gravity >= 0) JSETI(lp, F_LLP_gravity, gravity);
    } else if (p->type == AUI_GRID) {
        int cols = p->grid_cols > 0 ? p->grid_cols : 1;
        int cspan = c->cspan < 1 ? 1 : c->cspan, rspan = c->rspan < 1 ? 1 : c->rspan;
        if (c->col + cspan > cols) {
            int have = JI(p->view, M_GL_getColumnCount);
            if (have < c->col + cspan) JV(p->view, M_GL_setColumnCount, (jint)(c->col + cspan));
        }
        jobject rs, cs;
        int wsz, hsz;
        if (p->grid_uniform) {
            // A keypad: every column one share of the width (weight on a
            // zero-width column), every cell filling its track.
            jobject fill = JSFO(F_GL_FILL);
            rs = JSO(M_GL_specW, (jint)c->row, (jint)rspan, fill, (jfloat)0.0f);
            cs = JSO(M_GL_specW, (jint)c->col, (jint)cspan, fill, (jfloat)1.0f);
            wsz = c->fixed_w > 0 ? aeui_dp(c->fixed_w) : 0;
            hsz = c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_WRAP_CONTENT;
        } else {
            rs = JSO(M_GL_spec, (jint)c->row, (jint)rspan);
            cs = JSO(M_GL_spec, (jint)c->col, (jint)cspan);
            wsz = c->fixed_w > 0 ? aeui_dp(c->fixed_w) : LP_WRAP_CONTENT;
            hsz = c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_WRAP_CONTENT;
        }
        if (!rs || !cs) return;
        lp = JNEW(M_GLP_init, rs, cs);
        if (!lp) return;
        JSETI(lp, F_VGP_width, wsz);
        JSETI(lp, F_VGP_height, hsz);
        // Row and column spacing as the gap BEFORE every row/column but the
        // first, as GtkGrid's and NSGridView's spacing reads.
        JV(lp, M_MP_setMargins, ml, mt + (c->row > 0 ? aeui_dp(p->grid_rsp) : 0), mr, mb);
        JV(lp, M_MP_setMarginStart, ml + (c->col > 0 ? aeui_dp(p->grid_csp) : 0));
        JV(lp, M_MP_setMarginEnd, mr);
    } else if (p->type == AUI_SCROLLVIEW) {
        // The document view: the scroll area's width, its own height.
        lp = JNEW(M_FP_init, (jint)(c->fixed_w > 0 ? aeui_dp(c->fixed_w) : LP_MATCH_PARENT),
                  (jint)(c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_WRAP_CONTENT));
        if (!lp) return;
        JV(lp, M_MP_setMargins, ml, mt, mr, mb);
    } else {
        return;
    }
    JV(c->view, M_View_setLayoutParams, lp);
    (*env)->DeleteLocalRef(env, lp);
}

// Re-derive a widget's expansion from its own request and its children's,
// and walk up while it changes (a scroll area deep in a tree makes every
// container above it take the slack, as GTK's expand propagation does).
static void aeui_update_expand(JNIEnv* env, int handle) {
    while (handle) {
        AeuiWidget* w = live_widget(handle);
        if (!w) return;
        int h = w->own_hexp, v = w->own_vexp;
        for (int i = 0; i < widget_count; i++) {
            AeuiWidget* c = &widgets[i];
            if (c->parent != handle || !c->view) continue;
            if (c->hexp) h = 1;
            if (c->vexp) v = 1;
        }
        // A size the app stated on that axis is a size, not a share.
        if (w->fixed_w > 0) h = 0;
        if (w->fixed_h > 0) v = 0;
        if (h == w->hexp && v == w->vexp) return;
        w->hexp = h;
        w->vexp = v;
        aeui_apply_lp(env, handle);
        handle = w->parent;
    }
}

// The registered children of a container, in the container's own order.
static int aeui_children_in_order(JNIEnv* env, int parent, int* out, int max) {
    AeuiWidget* p = live_widget(parent);
    if (!p) return 0;
    int n = JI(p->view, M_VG_getChildCount), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        jobject v = JO(p->view, M_VG_getChildAt, (jint)i);
        if (!v) continue;
        for (int j = 0; j < widget_count; j++) {
            if (widgets[j].parent == parent && widgets[j].view &&
                (*env)->IsSameObject(env, widgets[j].view, v)) { out[k++] = j + 1; break; }
        }
        (*env)->DeleteLocalRef(env, v);
    }
    return k;
}

// A stack's spacing goes before every child but the first. After an insert
// at the front (the driver's banner) or a removal, which child is first can
// change, so the leads are re-derived from the real child order.
static void aeui_restack(JNIEnv* env, int parent) {
    AeuiWidget* p = live_widget(parent);
    if (!p || !aeui_is_linear(p->type)) return;
    int kids[512];
    int n = aeui_children_in_order(env, parent, kids, 512);
    for (int i = 0; i < n; i++) {
        AeuiWidget* c = widget_at(kids[i]);
        int lead = i > 0 ? p->spacing : 0;
        if (c && c->lead != lead) { c->lead = lead; aeui_apply_lp(env, kids[i]); }
    }
}

static void aeui_apply_enabled(JNIEnv* env, int handle, int parent_on);
static void aeui_apply_background(JNIEnv* env, int handle);

static int aeui_parent_enabled(int handle) {
    AeuiWidget* w = widget_at(handle);
    for (int p = w ? w->parent : 0; p; ) {
        AeuiWidget* pw = widget_at(p);
        if (!pw) break;
        if (pw->disabled) return 0;
        p = pw->parent;
    }
    return 1;
}

// Take a child out of the container it is in (a grid_place of a widget the
// DSL already put somewhere else moves it, as every backend's reparent does).
static void aeui_detach(JNIEnv* env, int child) {
    AeuiWidget* c = live_widget(child);
    if (!c) return;
    jobject vp = JO(c->view, M_View_getParent);
    if (vp) {
        if ((*env)->IsInstanceOf(env, vp, jcls(env, &C_ViewGroup)))
            JV(vp, M_VG_removeView, c->view);
        (*env)->DeleteLocalRef(env, vp);
    }
    int old = c->parent;
    c->parent = 0;
    c->lead = 0;
    AeuiWidget* op = widget_at(old);
    if (op && op->child_count > 0) op->child_count--;
    if (old) { aeui_restack(env, old); aeui_update_expand(env, old); }
}

static int make_stack(int orientation, int spacing, int type) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject ll = g_activity ? JNEW(M_LL_init, g_activity) : NULL;
    if (ll) {
        JV(ll, M_LL_setOrientation, (jint)orientation);
        h = register_widget_typed(env, ll, type);
        AeuiWidget* w = widget_at(h);
        if (w) w->spacing = spacing;
    }
    aeui_unframe(env);
    return h;
}

int aether_ui_vstack_create(int spacing) { return make_stack(LL_VERTICAL, spacing, AUI_VSTACK); }
int aether_ui_hstack_create(int spacing) { return make_stack(LL_HORIZONTAL, spacing, AUI_HSTACK); }

static int make_plain_view(int type, int background_argb, int set_bg) {
    JNIEnv* env = aeui_env();
    if (!env || !g_activity) return 0;
    jobject v = (*env)->NewObject(env, J.View, J.View_init, g_activity);
    if (aeui_check(env, "new View") || !v) return 0;
    if (set_bg) {
        (*env)->CallVoidMethod(env, v, J.View_setBackgroundColor, (jint)background_argb);
        aeui_check(env, "View.setBackgroundColor");
    }
    int h = register_widget_typed(env, v, type);
    (*env)->DeleteLocalRef(env, v);
    return h;
}

int aether_ui_spacer_create(void) { return make_plain_view(AUI_SPACER, 0, 0); }

// A hairline in the platform's separator grey (Material's 12% black).
int aether_ui_divider_create(void) { return make_plain_view(AUI_DIVIDER, (int)0x1F000000, 1); }

void aether_ui_grid_place(int grid_handle, int child_handle, int row, int col,
                          int row_span, int col_span);

// The next empty cell of a grid, row by row (GtkGrid's attach order for a
// child built inside the grid's block).
static void aeui_grid_next_cell(int grid, int* out_row, int* out_col) {
    AeuiWidget* g = widget_at(grid);
    int cols = (g && g->grid_cols > 0) ? g->grid_cols : 1;
    for (int r = 0; r < 4096; r++) {
        for (int col = 0; col < cols; col++) {
            int taken = 0;
            for (int i = 0; i < widget_count && !taken; i++) {
                AeuiWidget* c = &widgets[i];
                if (c->parent != grid || !c->view) continue;
                if (r >= c->row && r < c->row + c->rspan && col >= c->col && col < c->col + c->cspan)
                    taken = 1;
            }
            if (!taken) { *out_row = r; *out_col = col; return; }
        }
    }
    *out_row = 0; *out_col = 0;
}

static void aeui_attach(JNIEnv* env, int parent_handle, int child_handle, int index) {
    AeuiWidget* p = live_widget(parent_handle);
    AeuiWidget* c = live_widget(child_handle);
    if (!p || !c) return;
    if (c->parent) aeui_detach(env, child_handle);
    if (p->type == AUI_SCROLLVIEW) {
        // One document view: a second replaces the first, as
        // gtk_scrolled_window_set_child and NSScrollView's documentView do.
        for (int i = 0; i < widget_count; i++)
            if (widgets[i].parent == parent_handle && widgets[i].view) aeui_detach(env, i + 1);
    }
    c->parent = parent_handle;
    if (c->type == AUI_SPACER && aeui_is_linear(p->type)) {
        // A spacer takes the slack of ITS stack's axis only.
        c->own_vexp = c->vexp = aeui_is_vertical(p->type);
        c->own_hexp = c->hexp = !aeui_is_vertical(p->type);
    }
    c->lead = (aeui_is_linear(p->type) && p->child_count > 0 && index < 0) ? p->spacing : 0;
    p->child_count++;
    // The derived params go on the View first; addView then keeps them
    // (ViewGroup.addView(child, index) uses the child's own LayoutParams).
    aeui_apply_lp(env, child_handle);
    JV(p->view, M_VG_addViewIdx, c->view, (jint)index);
    if (index >= 0) aeui_restack(env, parent_handle);
    aeui_update_expand(env, child_handle);
    aeui_update_expand(env, parent_handle);
    if (p->disabled || !aeui_parent_enabled(parent_handle)) aeui_apply_enabled(env, child_handle, 0);
}

void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    AeuiWidget* p = live_widget(parent_handle);
    if (!p || !live_widget(child_handle)) return;
    if (p->type == AUI_GRID) {
        int r, c;
        aeui_grid_next_cell(parent_handle, &r, &c);
        aether_ui_grid_place(parent_handle, child_handle, r, c, 1, 1);
        return;
    }
    if (!aeui_is_container(p->type)) {
        // zstack, wrap, tabs, navstack, splitview: a later pass.
        AEUI_LOGW("add_child: parent %d (%s) is not a container on Android yet", parent_handle,
                  aeui_kind_name(p->type));
        return;
    }
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    aeui_attach(env, parent_handle, child_handle, -1);
    aeui_unframe(env);
}

// Retired, not just detached: the View and everything under it leave the
// registry, as on the other backends.
void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    AeuiWidget* c = live_widget(child_handle);
    if (!c) return;
    if (parent_handle && c->parent != parent_handle) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    aeui_detach(env, child_handle);
    aeui_retire_tree(env, child_handle);
    aeui_unframe(env);
}

void aether_ui_clear_children_impl(int handle) {
    if (!live_widget(handle)) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i].parent == handle && widgets[i].view) {
            aeui_detach(env, i + 1);
            aeui_retire_tree(env, i + 1);
        }
    }
    aeui_unframe(env);
}

// --- Grid -- a GridLayout -------------------------------------------------
int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject g = g_activity ? JNEW(M_GL_init, g_activity) : NULL;
    if (g) {
        JV(g, M_GL_setColumnCount, (jint)(cols > 0 ? cols : 1));
        JV(g, M_GL_setUseDefaultMargins, JNI_FALSE);
        h = register_widget_typed(env, g, AUI_GRID);
        AeuiWidget* w = widget_at(h);
        if (w) {
            w->grid_cols = cols > 0 ? cols : 1;
            w->grid_rsp = row_spacing;
            w->grid_csp = col_spacing;
        }
    }
    aeui_unframe(env);
    return h;
}

void aether_ui_grid_place(int grid_handle, int child_handle, int row, int col,
                          int row_span, int col_span) {
    AeuiWidget* g = live_widget(grid_handle);
    AeuiWidget* c = live_widget(child_handle);
    if (!g || !c || g->type != AUI_GRID || row < 0 || col < 0) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    if (c->parent) aeui_detach(env, child_handle);
    c = widget_at(child_handle);
    c->row = row;
    c->col = col;
    c->rspan = row_span > 0 ? row_span : 1;
    c->cspan = col_span > 0 ? col_span : 1;
    aeui_attach(env, grid_handle, child_handle, -1);
    aeui_unframe(env);
}

// A keypad: every column one width (weighted equal shares of the grid's
// width) and every cell filling its track.
void aether_ui_grid_set_uniform(int grid_handle, int on) {
    AeuiWidget* g = live_widget(grid_handle);
    if (!g || g->type != AUI_GRID) return;
    g->grid_uniform = on ? 1 : 0;
    JNIEnv* env = aeui_frame(64);
    if (!env) return;
    for (int i = 0; i < widget_count; i++)
        if (widgets[i].parent == grid_handle && widgets[i].view) aeui_apply_lp(env, i + 1);
    aeui_unframe(env);
}

// --- Form and section -----------------------------------------------------
// A form is a vstack with the desktop's gutters (16 between groups, 20
// around), reported as one, as GTK4 and AppKit report it. A section is a
// titled group: its title is the section's own header (not a widget the
// driver finds, as GtkFrame's and NSBox's titles are not), and the DSL puts
// the section's children in the box registered right after it (handle + 1).
int aether_ui_form_create(void) {
    int h = make_stack(LL_VERTICAL, 16, AUI_VSTACK);
    AeuiWidget* w = live_widget(h);
    JNIEnv* env = aeui_env();
    if (w && env) {
        int pad = aeui_dp(20);
        JV(w->view, M_View_setPadding, pad, pad, pad, pad);
    }
    return h;
}

JMETHOD(M_TV_init, C_TextView, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_TV_setTypeface2, C_TextView, "setTypeface", "(Landroid/graphics/Typeface;I)V");

int aether_ui_form_section_create(const char* title) {
    int section = make_stack(LL_VERTICAL, 4, AUI_FORM_SECTION);
    int inner = make_stack(LL_VERTICAL, 8, AUI_FORM_SECTION_INNER);   // handle + 1
    JNIEnv* env = aeui_frame(16);
    if (!env) return section;
    AeuiWidget* s = live_widget(section);
    AeuiWidget* in = live_widget(inner);
    if (s && in) {
        if (title && title[0] && g_activity) {
            jobject tv = JNEW(M_TV_init, g_activity);
            if (tv) {
                jstring t = aeui_jstring(env, title);
                (*env)->CallVoidMethod(env, tv, J.TextView_setText, t);
                aeui_check(env, "section title");
                JV(tv, M_TV_setTypeface2, (jobject)NULL, (jint)1 /* Typeface.BOLD */);
                jobject lp = JNEW(M_LLP_init, (jint)LP_MATCH_PARENT, (jint)LP_WRAP_CONTENT, (jfloat)0.0f);
                JV(s->view, M_VG_addView, tv, lp);
            }
        }
        int pad = aeui_dp(8);
        JV(in->view, M_View_setPadding, pad, pad, pad, pad);
        aeui_attach(env, section, inner, -1);
        // The header is not registered, so the inner box is the section's
        // first REGISTERED child; it still sits below the title.
        AeuiWidget* inw = widget_at(inner);
        if (inw && title && title[0]) { inw->lead = 4; aeui_apply_lp(env, inner); }
    }
    aeui_unframe(env);
    return section;
}

// --- Scroll view -- a vertical ScrollView ----------------------------------
// It takes the slack in both directions, as GTK4's scrolled window does
// (hexpand + vexpand), so a scroll area in a window fills what is left of it.
int aether_ui_scrollview_create(void) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject sv = g_activity ? JNEW(M_SV_init, g_activity) : NULL;
    if (sv) {
        h = register_widget_typed(env, sv, AUI_SCROLLVIEW);
        AeuiWidget* w = widget_at(h);
        if (w) w->own_hexp = w->own_vexp = w->hexp = w->vexp = 1;
    }
    aeui_unframe(env);
    return h;
}

// ===========================================================================
// Listeners. One AetherListener per (View, kind of event), carrying the
// widget's HANDLE: everything an event means -- which closures run, with
// what, whether a programmatic change counts -- is decided here in
// native_event, against the registry, never in Java.
// ===========================================================================
enum { AEUI_EV_CLICK = 1, AEUI_EV_TEXT = 2, AEUI_EV_CHECK = 3, AEUI_EV_SEEK = 4,
       AEUI_EV_SELECT = 5, AEUI_EV_HOVER = 6, AEUI_EV_LAYOUT = 7, AEUI_EV_DOUBLE = 8 };

static jobject aeui_listener(JNIEnv* env, int handle, int kind) {
    jobject l = (*env)->NewObject(env, J.Listener, J.Listener_init, (jint)handle, (jint)kind);
    if (aeui_check(env, "new AetherListener")) return NULL;
    return l;
}

JMETHOD(M_View_setOnClickListener, C_View, "setOnClickListener", "(Landroid/view/View$OnClickListener;)V");
JMETHOD(M_View_setOnHoverListener, C_View, "setOnHoverListener", "(Landroid/view/View$OnHoverListener;)V");
JMETHOD(M_View_setOnTouchListener, C_View, "setOnTouchListener", "(Landroid/view/View$OnTouchListener;)V");
JMETHOD(M_View_addOnLayoutChangeListener, C_View, "addOnLayoutChangeListener", "(Landroid/view/View$OnLayoutChangeListener;)V");
JMETHOD(M_View_setClickable, C_View, "setClickable", "(Z)V");

// A programmatic change in progress: the listeners it trips are not the
// user's doing, so on_change closures stay quiet (as AppKit's and UIKit's
// setters send no action). Bindings still see it.
static int g_programmatic = 0;
// bind_value seeding a field from its state: no write-back into that state.
static int g_seeding = 0;

// Every click closure on a View runs, in the order registered: a button's
// own closure and any on_click added to it later (GTK4 stacks a gesture on
// the button the same way).
static void aeui_add_click(JNIEnv* env, int handle, AeClosure* closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !closure) return;
    closure_push(&w->clicks, &w->nclicks, closure);
    if (!(w->listeners & LST_CLICK)) {
        jobject l = aeui_listener(env, handle, AEUI_EV_CLICK);
        if (l) {
            JV(w->view, M_View_setOnClickListener, l);
            (*env)->DeleteLocalRef(env, l);
            w->listeners |= LST_CLICK;
        }
    }
}

// ===========================================================================
// Text (TextView) and button (Button)
// ===========================================================================
JMETHOD(M_TV_setSingleLine, C_TextView, "setSingleLine", "(Z)V");
JMETHOD(M_TV_setGravity, C_TextView, "setGravity", "(I)V");
JMETHOD(M_TV_setEllipsize, C_TextView, "setEllipsize", "(Landroid/text/TextUtils$TruncateAt;)V");
JMETHOD(M_TV_setHint, C_TextView, "setHint", "(Ljava/lang/CharSequence;)V");
JMETHOD(M_TV_setInputType, C_TextView, "setInputType", "(I)V");
JMETHOD(M_TV_setMinLines, C_TextView, "setMinLines", "(I)V");
JMETHOD(M_TV_setTextColor, C_TextView, "setTextColor", "(I)V");
JMETHOD(M_TV_setTextSize, C_TextView, "setTextSize", "(IF)V");
JMETHOD(M_TV_setTypeface, C_TextView, "setTypeface", "(Landroid/graphics/Typeface;)V");
JMETHOD(M_TV_getCurrentTextColor, C_TextView, "getCurrentTextColor", "()I");
JMETHOD(M_TV_addTextChangedListener, C_TextView, "addTextChangedListener", "(Landroid/text/TextWatcher;)V");
JMETHOD(M_TV_setCompoundDrawablesRelative, C_TextView, "setCompoundDrawablesRelative",
        "(Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;)V");
JCLASS(C_TruncateAt, "android/text/TextUtils$TruncateAt");
JSFIELD(F_TA_START, C_TruncateAt, "START", "Landroid/text/TextUtils$TruncateAt;");
JSFIELD(F_TA_MIDDLE, C_TruncateAt, "MIDDLE", "Landroid/text/TextUtils$TruncateAt;");
JSFIELD(F_TA_END, C_TruncateAt, "END", "Landroid/text/TextUtils$TruncateAt;");

static void set_text_on(JNIEnv* env, jobject view, const char* text) {
    jstring s = aeui_jstring(env, text);
    (*env)->CallVoidMethod(env, view, J.TextView_setText, s);
    aeui_check(env, "TextView.setText");
    if (s) (*env)->DeleteLocalRef(env, s);
}

// A label's gravity: its anchor across, centred down (a label beside a
// taller control sits on its middle, as GTK's yalign 0.5 puts it).
static void aeui_apply_anchor(JNIEnv* env, AeuiWidget* w) {
    int across = w->anchor == 1 ? GRAV_CENTER_H : w->anchor == 2 ? GRAV_END : GRAV_START;
    JV(w->view, M_TV_setGravity, (jint)(across | GRAV_CENTER_V));
}

// Line handling, from the three things that decide it: wrapping (a wrapped
// label is multi-line), truncation (an ellipsis needs one line: Android, like
// UIKit, cannot put a head or middle ellipsis in wrapped text) and neither
// (one line, clipped -- a desktop label does not wrap unless asked).
static void aeui_apply_lines(JNIEnv* env, AeuiWidget* w) {
    if (w->truncate) {
        AeuiJMember* f = w->truncate == 1 ? &F_TA_START : w->truncate == 2 ? &F_TA_MIDDLE : &F_TA_END;
        jobject at = JSFO(*f);
        JV(w->view, M_TV_setSingleLine, JNI_TRUE);
        JV(w->view, M_TV_setEllipsize, at);
        if (at) (*env)->DeleteLocalRef(env, at);
    } else {
        JV(w->view, M_TV_setEllipsize, (jobject)NULL);
        JV(w->view, M_TV_setSingleLine, w->wrap ? JNI_FALSE : JNI_TRUE);
    }
}

static int make_text(const char* text, int wrap_width) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject tv = g_activity ? JNEW(M_TV_init, g_activity) : NULL;
    if (tv) {
        set_text_on(env, tv, text);
        h = register_widget_typed(env, tv, AUI_TEXT);
        AeuiWidget* w = widget_at(h);
        if (w) {
            w->wrap = wrap_width >= 0;
            if (wrap_width > 0) w->fixed_w = wrap_width;
            aeui_apply_lines(env, w);
            aeui_apply_anchor(env, w);
        }
    }
    aeui_unframe(env);
    return h;
}

int aether_ui_text_create(const char* text) { return make_text(text, -1); }

// Wraps at wrap_width (dp) on word boundaries: the label is that wide and as
// tall as its lines, as UIKit's preferredMaxLayoutWidth + width pin does.
int aether_ui_text_wrapped_create(const char* text, int wrap_width_px) {
    return make_text(text, wrap_width_px > 0 ? wrap_width_px : 0);
}

void aether_ui_text_set_anchor(int handle, int anchor) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_TEXT) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->anchor = (anchor >= 0 && anchor <= 2) ? anchor : 0;
    aeui_apply_anchor(env, w);
    aeui_unframe(env);
}

// All three ellipsis modes are native (TextUtils.TruncateAt), so the mode
// asked for is the mode applied; a truncated label is one line (wrap off).
void aether_ui_text_set_truncate(int handle, int mode) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_TEXT) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->truncate = (mode >= 1 && mode <= 3) ? mode : 0;
    if (w->truncate) w->wrap = 0;
    aeui_apply_lines(env, w);
    aeui_unframe(env);
}

int aether_ui_text_get_wrap(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->type == AUI_TEXT) ? w->wrap : 0;
}
int aether_ui_text_get_anchor(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->type == AUI_TEXT) ? w->anchor : 0;
}
int aether_ui_text_get_truncate(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->type == AUI_TEXT) ? w->truncate : 0;
}

static void aeui_set_edit_text(JNIEnv* env, AeuiWidget* w, const char* text);

// A label, a button, or a field's content (as UIKit's set_text reaches a
// UITextField). A button showing a disclosure keeps its caption off screen
// for the driver and a screen reader; that caption is what changes.
void aether_ui_text_set_string(int handle, const char* text) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    if (w->type == AUI_BUTTON && w->text_override) {
        free(w->text_override);
        w->text_override = strdup(text ? text : "");
    } else if (aeui_is_edit(w->type)) {
        aeui_set_edit_text(env, w, text);
    } else if (w->type == AUI_TEXT || w->type == AUI_BUTTON || w->type == AUI_TOGGLE) {
        set_text_on(env, w->view, text);
    }
    aeui_unframe(env);
}

static jobject make_button(JNIEnv* env, const char* label) {
    jobject b = (*env)->NewObject(env, J.Button, J.Button_init, g_activity);
    if (aeui_check(env, "new Button") || !b) return NULL;
    // The platform theme upper-cases button text; every other backend shows
    // the label as written.
    (*env)->CallVoidMethod(env, b, J.TextView_setAllCaps, JNI_FALSE);
    aeui_check(env, "setAllCaps");
    set_text_on(env, b, label);
    return b;
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject b = g_activity ? make_button(env, label) : NULL;
    if (b) {
        h = register_widget_typed(env, b, AUI_BUTTON);
        if (boxed_closure) aeui_add_click(env, h, (AeClosure*)boxed_closure);
    }
    aeui_unframe(env);
    return h;
}

int aether_ui_button_create_plain(const char* label) {
    return aether_ui_button_create(label, NULL);
}

void aether_ui_button_set_label(int handle, const char* label) {
    if (get_widget_type(handle) != AUI_BUTTON) return;
    aether_ui_text_set_string(handle, label);
}

// onclick() inside a widget's block: any View takes a click (a button's own
// closure is kept; this one runs after it).
void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    if (!live_widget(handle) || !boxed_closure) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    aeui_add_click(env, handle, (AeClosure*)boxed_closure);
    aeui_unframe(env);
}

// ===========================================================================
// Text fields -- EditText. One line for textfield/securefield, several for
// textarea; the placeholder is the platform's hint, on all three.
// ===========================================================================
JCLASS(C_EditText, "android/widget/EditText");
JMETHOD(M_ET_init, C_EditText, "<init>", "(Landroid/content/Context;)V");
enum { IT_TEXT = 0x1, IT_PASSWORD = 0x81, IT_MULTILINE = 0x20001 };

static void aeui_watch_text(JNIEnv* env, int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || (w->listeners & LST_TEXT)) return;
    jobject l = aeui_listener(env, handle, AEUI_EV_TEXT);
    if (!l) return;
    JV(w->view, M_TV_addTextChangedListener, l);
    (*env)->DeleteLocalRef(env, l);
    w->listeners |= LST_TEXT;
}

static int make_edit(const char* placeholder, void* boxed_closure, int type) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject et = g_activity ? JNEW(M_ET_init, g_activity) : NULL;
    if (et) {
        if (type == AUI_TEXTAREA) {
            JV(et, M_TV_setInputType, (jint)IT_MULTILINE);
            JV(et, M_TV_setSingleLine, JNI_FALSE);
            JV(et, M_TV_setMinLines, (jint)4);
            JV(et, M_TV_setGravity, (jint)(GRAV_TOP | GRAV_START));
        } else {
            JV(et, M_TV_setSingleLine, JNI_TRUE);
            JV(et, M_TV_setInputType, (jint)(type == AUI_SECUREFIELD ? IT_PASSWORD : IT_TEXT));
        }
        if (placeholder && *placeholder) {
            jstring hint = aeui_jstring(env, placeholder);
            JV(et, M_TV_setHint, hint);
        }
        h = register_widget_typed(env, et, type);
        AeuiWidget* w = widget_at(h);
        if (w) {
            w->change = (AeClosure*)boxed_closure;
            if (placeholder && *placeholder) w->placeholder = strdup(placeholder);
        }
        aeui_watch_text(env, h);
    }
    aeui_unframe(env);
    return h;
}

int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    return make_edit(placeholder, boxed_closure, AUI_TEXTFIELD);
}
int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    return make_edit(placeholder, boxed_closure, AUI_SECUREFIELD);
}
int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    return make_edit(placeholder, boxed_closure, AUI_TEXTAREA);
}

const char* aether_ui_placeholder_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->placeholder) ? w->placeholder : "";
}

// A programmatic set: on_change stays quiet (g_programmatic), a two-way
// bound field still writes back (AppKit's set_text mirrors into the state
// the same way; the compare-first in the VALUE binding stops the echo).
static void aeui_set_edit_text(JNIEnv* env, AeuiWidget* w, const char* text) {
    g_programmatic++;
    set_text_on(env, w->view, text);
    g_programmatic--;
}

static char* aeui_edit_text_dup(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_edit(w->type)) return strdup("");
    JNIEnv* env = aeui_frame(8);
    if (!env) return strdup("");
    jobject cs = (*env)->CallObjectMethod(env, w->view, J.TextView_getText);
    char* out = aeui_check(env, "getText") ? strdup("") : aeui_charseq_dup(env, cs);
    aeui_unframe(env);
    return out;
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_edit(w->type)) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    aeui_set_edit_text(env, w, text);
    aeui_unframe(env);
}

// The caller owns the buffer (the @heap extern), as on every backend.
const char* aether_ui_textfield_get_text(int handle) { return aeui_edit_text_dup(handle); }

void aether_ui_textarea_set_text(int handle, const char* text) {
    aether_ui_textfield_set_text(handle, text);
}
char* aether_ui_textarea_get_text(int handle) { return aeui_edit_text_dup(handle); }

// ===========================================================================
// Toggle -- CheckBox, labelled (GTK's check button, AppKit's checkbox).
// Closure fires 0/1. In a group it draws as a radio button and turning one
// member on turns the rest off.
// ===========================================================================
JCLASS(C_CheckBox, "android/widget/CheckBox");
JCLASS(C_CompoundButton, "android/widget/CompoundButton");
JMETHOD(M_CB_init, C_CheckBox, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_CPB_setChecked, C_CompoundButton, "setChecked", "(Z)V");
JMETHOD(M_CPB_isChecked, C_CompoundButton, "isChecked", "()Z");
JMETHOD(M_CPB_setOnCheckedChangeListener, C_CompoundButton, "setOnCheckedChangeListener",
        "(Landroid/widget/CompoundButton$OnCheckedChangeListener;)V");
JMETHOD(M_CPB_setButtonDrawable, C_CompoundButton, "setButtonDrawable", "(I)V");

int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject cb = g_activity ? JNEW(M_CB_init, g_activity) : NULL;
    if (cb) {
        set_text_on(env, cb, label);
        h = register_widget_typed(env, cb, AUI_TOGGLE);
        AeuiWidget* w = widget_at(h);
        if (w) w->change = (AeClosure*)boxed_closure;
        jobject l = aeui_listener(env, h, AEUI_EV_CHECK);
        if (l) JV(cb, M_CPB_setOnCheckedChangeListener, l);
    }
    aeui_unframe(env);
    return h;
}

void aether_ui_toggle_set_active(int handle, int active) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_TOGGLE) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    g_programmatic++;
    JV(w->view, M_CPB_setChecked, active ? JNI_TRUE : JNI_FALSE);
    g_programmatic--;
    aeui_unframe(env);
}

int aether_ui_toggle_get_active(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_TOGGLE) return 0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    int on = JZ(w->view, M_CPB_isChecked) ? 1 : 0;
    aeui_unframe(env);
    return on;
}

// Chain each member to the group's first (b->a, c->a, or c->b->a: one
// leader per group, as AppKit's radio leader handle works).
void aether_ui_toggle_set_group(int handle, int group_with) {
    AeuiWidget* a = live_widget(handle);
    AeuiWidget* b = live_widget(group_with);
    if (!a || !b || a->type != AUI_TOGGLE || b->type != AUI_TOGGLE) return;
    int leader = b->radio_leader ? b->radio_leader : group_with;
    b->radio_leader = leader;
    a->radio_leader = leader;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    int radio = aeui_android_r(env, "drawable", "btn_radio");
    if (radio) {
        JV(a->view, M_CPB_setButtonDrawable, (jint)radio);
        JV(b->view, M_CPB_setButtonDrawable, (jint)radio);
    }
    aeui_unframe(env);
}

// ===========================================================================
// Slider -- SeekBar. A SeekBar counts integer steps from 0, so the range is
// mapped onto SLIDER_STEPS of them and the value read back is the value the
// thumb is actually at (the step nearest what was asked).
// ===========================================================================
#define SLIDER_STEPS 10000
JCLASS(C_SeekBar, "android/widget/SeekBar");
JCLASS(C_ProgressBar, "android/widget/ProgressBar");
JMETHOD(M_SB_init, C_SeekBar, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_SB_setOnSeekBarChangeListener, C_SeekBar, "setOnSeekBarChangeListener",
        "(Landroid/widget/SeekBar$OnSeekBarChangeListener;)V");
JMETHOD(M_PB_init3, C_ProgressBar, "<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V");
JMETHOD(M_PB_setMax, C_ProgressBar, "setMax", "(I)V");
JMETHOD(M_PB_setProgress, C_ProgressBar, "setProgress", "(I)V");
JMETHOD(M_PB_getProgress, C_ProgressBar, "getProgress", "()I");
JMETHOD(M_PB_setIndeterminate, C_ProgressBar, "setIndeterminate", "(Z)V");

static int slider_steps_for(AeuiWidget* w, double value) {
    double span = w->smax - w->smin;
    if (span <= 0) return 0;
    double t = (value - w->smin) / span;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (int)floor(t * SLIDER_STEPS + 0.5);
}

static double slider_value_at(AeuiWidget* w, int steps) {
    return w->smin + (w->smax - w->smin) * (double)steps / SLIDER_STEPS;
}

int aether_ui_slider_create(double min_val, double max_val, double initial,
                            void* boxed_closure) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject sb = g_activity ? JNEW(M_SB_init, g_activity) : NULL;
    if (sb) {
        h = register_widget_typed(env, sb, AUI_SLIDER);
        AeuiWidget* w = widget_at(h);
        if (w) {
            w->smin = min_val;
            w->smax = max_val;
            w->change = (AeClosure*)boxed_closure;
            // A slider takes the row's width, as GTK's scale does (hexpand).
            w->own_hexp = w->hexp = 1;
            JV(sb, M_PB_setMax, (jint)SLIDER_STEPS);
            JV(sb, M_PB_setProgress, (jint)slider_steps_for(w, initial));
        }
        jobject l = aeui_listener(env, h, AEUI_EV_SEEK);
        if (l) JV(sb, M_SB_setOnSeekBarChangeListener, l);
    }
    aeui_unframe(env);
    return h;
}

// Programmatic: no on_change (the SeekBar reports it as not from the user,
// and the listener only forwards a person's drag).
void aether_ui_slider_set_value(int handle, double value) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_SLIDER) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_PB_setProgress, (jint)slider_steps_for(w, value));
    aeui_unframe(env);
}

double aether_ui_slider_get_value(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_SLIDER) return 0.0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0.0;
    int steps = JI(w->view, M_PB_getProgress);
    aeui_unframe(env);
    return slider_value_at(w, steps);
}

// ===========================================================================
// Progress bar -- a horizontal ProgressBar over 0..PROGRESS_STEPS.
// ===========================================================================
#define PROGRESS_STEPS 10000

static int progress_steps(double fraction) {
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    return (int)floor(fraction * PROGRESS_STEPS + 0.5);
}

int aether_ui_progressbar_create(double fraction) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    int style = aeui_android_r(env, "attr", "progressBarStyleHorizontal");
    jobject pb = g_activity ? JNEW(M_PB_init3, g_activity, (jobject)NULL, (jint)style) : NULL;
    if (pb) {
        JV(pb, M_PB_setIndeterminate, JNI_FALSE);
        JV(pb, M_PB_setMax, (jint)PROGRESS_STEPS);
        JV(pb, M_PB_setProgress, (jint)progress_steps(fraction));
        h = register_widget_typed(env, pb, AUI_PROGRESSBAR);
        AeuiWidget* w = widget_at(h);
        if (w) w->own_hexp = w->hexp = 1;   // GTK's bar is hexpand too
    }
    aeui_unframe(env);
    return h;
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_PROGRESSBAR) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_PB_setProgress, (jint)progress_steps(fraction));
    aeui_unframe(env);
}

static double aeui_progress_fraction(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_PROGRESSBAR) return 0.0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0.0;
    int steps = JI(w->view, M_PB_getProgress);
    aeui_unframe(env);
    return (double)steps / PROGRESS_STEPS;
}

// ===========================================================================
// Picker -- a Spinner over an ArrayAdapter. The items are kept here too, so
// the driver reads the chosen item's text without asking the adapter.
// ===========================================================================
JCLASS(C_Spinner, "android/widget/Spinner");
JCLASS(C_AdapterView, "android/widget/AdapterView");
JCLASS(C_ArrayAdapter, "android/widget/ArrayAdapter");
JMETHOD(M_SP_init, C_Spinner, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_SP_setAdapter, C_Spinner, "setAdapter", "(Landroid/widget/SpinnerAdapter;)V");
JMETHOD(M_AV_setSelection, C_AdapterView, "setSelection", "(I)V");
JMETHOD(M_AV_getAdapter, C_AdapterView, "getAdapter", "()Landroid/widget/Adapter;");
JMETHOD(M_AV_setOnItemSelectedListener, C_AdapterView, "setOnItemSelectedListener",
        "(Landroid/widget/AdapterView$OnItemSelectedListener;)V");
JMETHOD(M_AA_init, C_ArrayAdapter, "<init>", "(Landroid/content/Context;I)V");
JMETHOD(M_AA_add, C_ArrayAdapter, "add", "(Ljava/lang/Object;)V");
JMETHOD(M_AA_setDropDownViewResource, C_ArrayAdapter, "setDropDownViewResource", "(I)V");

int aether_ui_picker_create(void* boxed_closure) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject sp = g_activity ? JNEW(M_SP_init, g_activity) : NULL;
    if (sp) {
        jobject ad = JNEW(M_AA_init, g_activity,
                          (jint)aeui_android_r(env, "layout", "simple_spinner_item"));
        if (ad) {
            JV(ad, M_AA_setDropDownViewResource,
               (jint)aeui_android_r(env, "layout", "simple_spinner_dropdown_item"));
            JV(sp, M_SP_setAdapter, ad);
        }
        h = register_widget_typed(env, sp, AUI_PICKER);
        AeuiWidget* w = widget_at(h);
        if (w) w->change = (AeClosure*)boxed_closure;
        jobject l = aeui_listener(env, h, AEUI_EV_SELECT);
        if (l) JV(sp, M_AV_setOnItemSelectedListener, l);
    }
    aeui_unframe(env);
    return h;
}

void aether_ui_picker_add_item(int handle, const char* item) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_PICKER) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    char** ni = (char**)realloc(w->items, sizeof(char*) * (size_t)(w->nitems + 1));
    if (ni) {
        w->items = ni;
        w->items[w->nitems++] = strdup(item ? item : "");
        jobject ad = JO(w->view, M_AV_getAdapter);
        jstring s = aeui_jstring(env, item);
        if (ad && s) JV(ad, M_AA_add, s);
    }
    aeui_unframe(env);
}

static void aeui_fire_picker(AeuiWidget* w, int index) {
    AeClosure* c = w->change;
    if (c && c->fn) ((void (*)(void*, intptr_t))c->fn)(c->env, (intptr_t)index);
}

// A programmatic selection fires the change callback, as it does on GTK4,
// AppKit and UIKit. The Spinner reports the selection later, from its next
// layout; that report matches `selected` and is not fired twice.
void aether_ui_picker_set_selected(int handle, int index) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_PICKER || index < 0 || index >= w->nitems) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->selected = index;
    JV(w->view, M_AV_setSelection, (jint)index);
    aeui_unframe(env);
    aeui_fire_picker(widget_at(handle), index);
}

int aether_ui_picker_get_selected(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->type == AUI_PICKER) ? w->selected : 0;
}

// ===========================================================================
// Image -- ImageView over a Bitmap decoded by the platform (BitmapFactory:
// PNG, JPEG, GIF, BMP, WebP, HEIF). A relative path resolves against the
// app's working directory, which native_start makes the extracted copy of
// the files packaged with the app (tools/android-apk.sh), so the paths a
// desktop app opens from its checkout open here too.
// ===========================================================================
JCLASS(C_ImageView, "android/widget/ImageView");
JCLASS(C_ScaleType, "android/widget/ImageView$ScaleType");
JCLASS(C_BitmapFactory, "android/graphics/BitmapFactory");
JCLASS(C_ColorStateList, "android/content/res/ColorStateList");
JMETHOD(M_IV_init, C_ImageView, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_IV_setImageBitmap, C_ImageView, "setImageBitmap", "(Landroid/graphics/Bitmap;)V");
JMETHOD(M_IV_setScaleType, C_ImageView, "setScaleType", "(Landroid/widget/ImageView$ScaleType;)V");
JMETHOD(M_IV_getDrawable, C_ImageView, "getDrawable", "()Landroid/graphics/drawable/Drawable;");
JMETHOD(M_IV_setImageTintList, C_ImageView, "setImageTintList", "(Landroid/content/res/ColorStateList;)V");
JSFIELD(F_ST_CENTER, C_ScaleType, "CENTER", "Landroid/widget/ImageView$ScaleType;");
JSFIELD(F_ST_FIT_CENTER, C_ScaleType, "FIT_CENTER", "Landroid/widget/ImageView$ScaleType;");
JSFIELD(F_ST_CENTER_CROP, C_ScaleType, "CENTER_CROP", "Landroid/widget/ImageView$ScaleType;");
JSFIELD(F_ST_FIT_XY, C_ScaleType, "FIT_XY", "Landroid/widget/ImageView$ScaleType;");
JSTATIC(M_BF_decodeByteArray, C_BitmapFactory, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
JSTATIC(M_CSL_valueOf, C_ColorStateList, "valueOf", "(I)Landroid/content/res/ColorStateList;");

// Every mode has a ScaleType, so each is applied as asked: original = the
// picture at its own size, centred and clipped (CENTER); contain =
// FIT_CENTER; cover = CENTER_CROP; stretch = FIT_XY.
static void aeui_apply_fill(JNIEnv* env, AeuiWidget* w) {
    AeuiJMember* f = w->fill == 0 ? &F_ST_CENTER : w->fill == 2 ? &F_ST_CENTER_CROP
                   : w->fill == 3 ? &F_ST_FIT_XY : &F_ST_FIT_CENTER;
    jobject st = JSFO(*f);
    if (st) JV(w->view, M_IV_setScaleType, st);
}

static int image_register(const unsigned char* data, int length) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject iv = g_activity ? JNEW(M_IV_init, g_activity) : NULL;
    if (iv) {
        if (data && length > 0) {
            jbyteArray bytes = (*env)->NewByteArray(env, length);
            if (bytes) {
                (*env)->SetByteArrayRegion(env, bytes, 0, length, (const jbyte*)data);
                jobject bmp = JSO(M_BF_decodeByteArray, bytes, (jint)0, (jint)length);
                // A picture that will not decode leaves the widget empty, so
                // the tree stays stable (has_image reports it).
                if (bmp) JV(iv, M_IV_setImageBitmap, bmp);
            }
        }
        h = register_widget_typed(env, iv, AUI_IMAGE);
        AeuiWidget* w = widget_at(h);
        if (w) aeui_apply_fill(env, w);
    }
    aeui_unframe(env);
    return h;
}

int aether_ui_image_create(const char* filepath) {
    unsigned char* data = NULL;
    long len = 0;
    FILE* f = (filepath && *filepath) ? fopen(filepath, "rb") : NULL;
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
            data = (unsigned char*)malloc((size_t)len);
            if (data && fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); data = NULL; }
        }
        fclose(f);
    } else if (filepath && *filepath) {
        AEUI_LOGW("image: cannot open %s (%s)", filepath, strerror(errno));
    }
    int h = image_register(data, data ? (int)len : 0);
    free(data);
    return h;
}

int aether_ui_image_from_bytes(const char* data, int length) {
    return image_register((const unsigned char*)data, length);
}

void aether_ui_image_set_fill(int handle, int mode) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_IMAGE) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->fill = (mode >= 0 && mode <= 3) ? mode : 1;
    aeui_apply_fill(env, w);
    aeui_unframe(env);
}

int aether_ui_image_get_fill(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->type == AUI_IMAGE) ? w->fill : 0;
}

// The box the picture fills, in dp (the size request GTK4 makes).
void aether_ui_image_set_size(int handle, int width, int height) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    w->fixed_w = width > 0 ? width : 0;
    w->fixed_h = height > 0 ? height : 0;
    aeui_apply_lp(env, handle);
    aeui_update_expand(env, handle);
    aeui_unframe(env);
}

// ImageView tints any picture through its tint list (SRC_IN over the
// picture's alpha: a template's shape in the tint colour), so the tint asked
// for is the tint applied, and the getter reports it.
void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_IMAGE) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    if (on) {
        int rgb = (((int)(r * 255) & 255) << 16) | (((int)(g * 255) & 255) << 8) | ((int)(b * 255) & 255);
        jobject csl = JSO(M_CSL_valueOf, (jint)(0xFF000000u | (unsigned)rgb));
        JV(w->view, M_IV_setImageTintList, csl);
        w->tint = 0x1000000 | rgb;
    } else {
        JV(w->view, M_IV_setImageTintList, (jobject)NULL);
        w->tint = -1;
    }
    aeui_unframe(env);
}

int aether_ui_image_get_tint(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->type == AUI_IMAGE) ? w->tint : -1;
}

int aether_ui_image_has_content(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_IMAGE) return 0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    jobject d = JO(w->view, M_IV_getDrawable);
    aeui_unframe(env);
    return d ? 1 : 0;
}

// ===========================================================================
// Styling. A View's look is its background Drawable, so every background
// input -- colour, gradient, border, corner radius, the hover and pressed
// colours, flatness -- is kept on the record and the Drawable is rebuilt
// from all of them whenever one changes (aeui_apply_background):
//
//   face    a GradientDrawable in the colour or gradient, with the radius;
//           or, when no colour was asked for, the theme's own background
//           (a button keeps its face under a border)
//   border  a stroked, unfilled GradientDrawable layered over the face
//   states  a StateListDrawable when there is a hover or pressed colour:
//           the platform's own state machinery switches the face (a real
//           press on a touch screen, a real hover under a mouse)
//
// The styled_* readbacks report these values, which are what is painted.
// ===========================================================================
JCLASS(C_Drawable, "android/graphics/drawable/Drawable");
JCLASS(C_GradientDrawable, "android/graphics/drawable/GradientDrawable");
JCLASS(C_GDOrientation, "android/graphics/drawable/GradientDrawable$Orientation");
JCLASS(C_LayerDrawable, "android/graphics/drawable/LayerDrawable");
JCLASS(C_StateListDrawable, "android/graphics/drawable/StateListDrawable");
JMETHOD(M_GD_init, C_GradientDrawable, "<init>", "()V");
JMETHOD(M_GD_setColor, C_GradientDrawable, "setColor", "(I)V");
JMETHOD(M_GD_setColors, C_GradientDrawable, "setColors", "([I)V");
JMETHOD(M_GD_setOrientation, C_GradientDrawable, "setOrientation",
        "(Landroid/graphics/drawable/GradientDrawable$Orientation;)V");
JMETHOD(M_GD_setCornerRadius, C_GradientDrawable, "setCornerRadius", "(F)V");
JMETHOD(M_GD_setStroke, C_GradientDrawable, "setStroke", "(II)V");
JSFIELD(F_GDO_TOP_BOTTOM, C_GDOrientation, "TOP_BOTTOM", "Landroid/graphics/drawable/GradientDrawable$Orientation;");
JSFIELD(F_GDO_LEFT_RIGHT, C_GDOrientation, "LEFT_RIGHT", "Landroid/graphics/drawable/GradientDrawable$Orientation;");
JMETHOD(M_LD_init, C_LayerDrawable, "<init>", "([Landroid/graphics/drawable/Drawable;)V");
JMETHOD(M_SLD_init, C_StateListDrawable, "<init>", "()V");
JMETHOD(M_SLD_addState, C_StateListDrawable, "addState", "([ILandroid/graphics/drawable/Drawable;)V");
JMETHOD(M_View_setBackground, C_View, "setBackground", "(Landroid/graphics/drawable/Drawable;)V");
JMETHOD(M_View_getBackground, C_View, "getBackground", "()Landroid/graphics/drawable/Drawable;");
JMETHOD(M_View_setClipToOutline, C_View, "setClipToOutline", "(Z)V");
JMETHOD(M_View_setAlpha, C_View, "setAlpha", "(F)V");
JMETHOD(M_View_setEnabled, C_View, "setEnabled", "(Z)V");
JMETHOD(M_View_setVisibility, C_View, "setVisibility", "(I)V");
JMETHOD(M_View_setTooltipText, C_View, "setTooltipText", "(Ljava/lang/CharSequence;)V");
JMETHOD(M_View_setLayoutDirection, C_View, "setLayoutDirection", "(I)V");
JMETHOD(M_View_setFocusable, C_View, "setFocusable", "(Z)V");
JMETHOD(M_View_setFocusableInTouchMode, C_View, "setFocusableInTouchMode", "(Z)V");
JMETHOD(M_View_requestFocus, C_View, "requestFocus", "()Z");
JMETHOD(M_View_requestLayout, C_View, "requestLayout", "()V");
JMETHOD(M_View_setContentDescription, C_View, "setContentDescription", "(Ljava/lang/CharSequence;)V");
JMETHOD(M_View_setAccessibilityDelegate, C_View, "setAccessibilityDelegate",
        "(Landroid/view/View$AccessibilityDelegate;)V");
JMETHOD(M_View_setAccessibilityHeading, C_View, "setAccessibilityHeading", "(Z)V");
JMETHOD(M_View_setImportantForAccessibility, C_View, "setImportantForAccessibility", "(I)V");
JMETHOD(M_View_isHovered, C_View, "isHovered", "()Z");
JMETHOD(M_View_isPressed, C_View, "isPressed", "()Z");
JMETHOD(M_View_setHovered, C_View, "setHovered", "(Z)V");
JMETHOD(M_View_setPressed, C_View, "setPressed", "(Z)V");
JMETHOD(M_View_setStateListAnimator, C_View, "setStateListAnimator", "(Landroid/animation/StateListAnimator;)V");
JMETHOD(M_TV_setMinWidth, C_TextView, "setMinWidth", "(I)V");
JMETHOD(M_TV_setCompoundDrawablePadding, C_TextView, "setCompoundDrawablePadding", "(I)V");

static unsigned int aeui_argb(double r, double g, double b, double a) {
    return ((unsigned)((int)(a * 255) & 255) << 24) | ((unsigned)((int)(r * 255) & 255) << 16) |
           ((unsigned)((int)(g * 255) & 255) << 8) | (unsigned)((int)(b * 255) & 255);
}
static int aeui_rgb(double r, double g, double b) {
    return (int)(aeui_argb(r, g, b, 0) & 0xFFFFFF);
}

static jobject aeui_gradient(JNIEnv* env, AeuiWidget* w) {
    jobject gd = JNEW(M_GD_init);
    if (gd && w->radius > 0) JV(gd, M_GD_setCornerRadius, (jfloat)(w->radius * g_density));
    return gd;
}

// The face for one state: 0 rest, 1 hovered, 2 pressed. NULL = none.
static jobject aeui_face(JNIEnv* env, AeuiWidget* w, int which) {
    int colour_set = 0;
    unsigned int argb = 0;
    if (which == 1 && w->hover_bg >= 0) { colour_set = 1; argb = 0xFF000000u | (unsigned)w->hover_bg; }
    else if (which == 2 && w->active_bg >= 0) { colour_set = 1; argb = 0xFF000000u | (unsigned)w->active_bg; }
    else if (which != 0 && w->flat && !w->bg_set && !w->grad_set) {
        // A flat button's face appears under the pointer and the finger,
        // in the platform's 12% ink, as GTK4's flat button lights up.
        colour_set = 1; argb = 0x1F000000u;
    }
    jobject face = NULL;
    if (colour_set || (which == 0 && (w->bg_set || w->grad_set))) {
        face = aeui_gradient(env, w);
        if (face) {
            if (!colour_set && w->grad_set) {
                jintArray cols = (*env)->NewIntArray(env, 2);
                if (cols) {
                    jint cv[2] = { (jint)w->grad1, (jint)w->grad2 };
                    (*env)->SetIntArrayRegion(env, cols, 0, 2, cv);
                    JV(face, M_GD_setColors, cols);
                }
                jobject o = w->grad_vertical ? JSFO(F_GDO_TOP_BOTTOM) : JSFO(F_GDO_LEFT_RIGHT);
                if (o) JV(face, M_GD_setOrientation, o);
            } else {
                JV(face, M_GD_setColor, (jint)(colour_set ? argb : w->bg_argb));
            }
        }
    } else if (which == 0 && w->orig_bg && !w->flat) {
        face = (*env)->NewLocalRef(env, w->orig_bg);
    } else if (which == 0 && w->radius > 0) {
        face = aeui_gradient(env, w);   // clear, but it gives the outline its corners
        if (face) JV(face, M_GD_setColor, (jint)0);
    }
    if (w->border_w > 0 && (face || which == 0)) {
        jobject border = aeui_gradient(env, w);
        if (border) {
            JV(border, M_GD_setColor, (jint)0);
            int bw = (int)floor(w->border_w * g_density + 0.5);
            JV(border, M_GD_setStroke, (jint)(bw < 1 ? 1 : bw), (jint)w->border_argb);
            jobjectArray layers = (*env)->NewObjectArray(env, face ? 2 : 1, jcls(env, &C_Drawable), NULL);
            if (layers) {
                int i = 0;
                if (face) (*env)->SetObjectArrayElement(env, layers, i++, face);
                (*env)->SetObjectArrayElement(env, layers, i, border);
                jobject ld = JNEW(M_LD_init, layers);
                if (ld) face = ld;
            }
        }
    }
    return face;
}

static jintArray aeui_state_set(JNIEnv* env, int attr) {
    jintArray a = (*env)->NewIntArray(env, attr ? 1 : 0);
    if (a && attr) { jint v = attr; (*env)->SetIntArrayRegion(env, a, 0, 1, &v); }
    return a;
}

static void aeui_apply_background(JNIEnv* env, int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    if (!w->bg_captured) {
        jobject orig = JO(w->view, M_View_getBackground);
        w->orig_bg = orig ? (*env)->NewGlobalRef(env, orig) : NULL;
        w->bg_captured = 1;
    }
    int owned = w->bg_set || w->grad_set || w->border_w > 0 || w->hover_bg >= 0 ||
                w->active_bg >= 0 || w->flat || w->radius > 0;
    if (!owned) {
        JV(w->view, M_View_setBackground, w->orig_bg);
        JV(w->view, M_View_setClipToOutline, JNI_FALSE);
        return;
    }
    jobject base = aeui_face(env, w, 0);
    jobject hover = aeui_face(env, w, 1);
    jobject press = aeui_face(env, w, 2);
    jobject bg = base;
    if (hover || press) {
        jobject sld = JNEW(M_SLD_init);
        if (sld) {
            if (press) JV(sld, M_SLD_addState, aeui_state_set(env, aeui_android_r(env, "attr", "state_pressed")), press);
            if (hover) JV(sld, M_SLD_addState, aeui_state_set(env, aeui_android_r(env, "attr", "state_hovered")), hover);
            if (base) JV(sld, M_SLD_addState, aeui_state_set(env, 0), base);
            bg = sld;
        }
    }
    JV(w->view, M_View_setBackground, bg);
    // Corners clip what is inside too, as UIKit's clipsToBounds does.
    JV(w->view, M_View_setClipToOutline, w->radius > 0 ? JNI_TRUE : JNI_FALSE);
}

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    w->bg_set = 1;
    w->grad_set = 0;
    w->bg_argb = aeui_argb(r, g, b, a);
    w->styled_bg = aeui_rgb(r, g, b);
    aeui_apply_background(env, handle);
    aeui_unframe(env);
}
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aether_ui_set_bg_color((int)(intptr_t)ctx, r, g, b, a);
}

void aether_ui_set_bg_gradient(int handle, double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    w->grad_set = 1;
    w->bg_set = 0;
    w->grad1 = aeui_argb(r1, g1, b1, 1.0);
    w->grad2 = aeui_argb(r2, g2, b2, 1.0);
    w->grad_vertical = vertical ? 1 : 0;
    aeui_apply_background(env, handle);
    aeui_unframe(env);
}

void aether_ui_set_corner_radius(int handle, double radius) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    w->radius = radius > 0 ? radius : 0;
    aeui_apply_background(env, handle);
    aeui_unframe(env);
}
void aether_ui_set_corner_radius_ctx(void* ctx, double radius) {
    aether_ui_set_corner_radius((int)(intptr_t)ctx, radius);
}

// Width 0 clears the border, and the readback with it.
void aether_ui_set_border(int handle, double width, double r, double g, double b) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    w->border_w = width > 0 ? width : 0;
    w->border_argb = aeui_argb(r, g, b, 1.0);
    w->styled_border = width > 0
        ? (((int)width & 0x3F) << 24) | 0x40000000 | aeui_rgb(r, g, b) : -1;
    aeui_apply_background(env, handle);
    aeui_unframe(env);
}

// Hover (state 0) and pressed (state 1) backgrounds. The per-state text
// colour the ABI carries is not applied on any backend yet, so neither here.
void aether_ui_set_state_style(int handle, int state, double br, double bg_, double bb,
                               double fr, double fg_, double fb) {
    (void)fr; (void)fg_; (void)fb;
    AeuiWidget* w = live_widget(handle);
    if (!w || br < 0.0) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    if (state == 1) w->active_bg = aeui_rgb(br, bg_, bb);
    else w->hover_bg = aeui_rgb(br, bg_, bb);
    aeui_apply_background(env, handle);
    aeui_unframe(env);
}

int aether_ui_state_style_impl(int handle, int state) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return -1;
    return state == 1 ? w->active_bg : w->hover_bg;
}

void aether_ui_set_opacity(int handle, double opacity) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    if (opacity < 0) opacity = 0;
    if (opacity > 1) opacity = 1;
    JV(w->view, M_View_setAlpha, (jfloat)opacity);
    w->styled_opacity = (int)(opacity * 100 + 0.5);
    aeui_unframe(env);
}
void aether_ui_set_opacity_ctx(void* ctx, double opacity) {
    aether_ui_set_opacity((int)(intptr_t)ctx, opacity);
}

// The colour PAINTED now: with a pressed or hover colour installed, the
// StateListDrawable shows it while the View is in that state (pressed beats
// hovered, as its state order says), and that is what the readback reports.
int aether_ui_styled_bg_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return -1;
    if (w->active_bg < 0 && w->hover_bg < 0) return w->styled_bg;
    JNIEnv* env = aeui_frame(4);
    if (!env) return w->styled_bg;
    int out = w->styled_bg;
    if (w->active_bg >= 0 && JZ(w->view, M_View_isPressed)) out = w->active_bg;
    else if (w->hover_bg >= 0 && JZ(w->view, M_View_isHovered)) out = w->hover_bg;
    aeui_unframe(env);
    return out;
}
int aether_ui_styled_fg_impl(int handle) { AeuiWidget* w = live_widget(handle); return w ? w->styled_fg : -1; }
int aether_ui_styled_opacity_impl(int handle) { AeuiWidget* w = live_widget(handle); return w ? w->styled_opacity : -1; }
int aether_ui_styled_border_impl(int handle) { AeuiWidget* w = live_widget(handle); return w ? w->styled_border : -1; }
const char* aether_ui_styled_weight_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->bold < 0) return "";
    return w->bold ? "bold" : "normal";
}
const char* aether_ui_styled_font_family_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->family) ? w->family : "";
}

// --- Flat and disclosure buttons -------------------------------------------

// No face of its own until a finger or the pointer is on it, and none of
// the raised button's press elevation. 0 puts the theme's face back.
void aether_ui_button_set_flat(int handle, int on) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_BUTTON) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    w->flat = on ? 1 : 0;
    if (on) {
        JV(w->view, M_View_setStateListAnimator, (jobject)NULL);
        JV(w->view, M_TV_setMinWidth, (jint)0);
        JV(w->view, M_View_setMinimumWidth, (jint)aeui_dp(w->min_w));
    }
    aeui_apply_background(env, handle);
    aeui_unframe(env);
}
void aether_ui_button_set_flat_ctx(void* ctx, int on) {
    aether_ui_button_set_flat((int)(intptr_t)ctx, on);
}

JCLASS(C_Path, "android/graphics/Path");
JCLASS(C_PathShape, "android/graphics/drawable/shapes/PathShape");
JCLASS(C_ShapeDrawable, "android/graphics/drawable/ShapeDrawable");
JCLASS(C_Paint, "android/graphics/Paint");
JMETHOD(M_Path_init, C_Path, "<init>", "()V");
JMETHOD(M_Path_moveTo, C_Path, "moveTo", "(FF)V");
JMETHOD(M_Path_lineTo, C_Path, "lineTo", "(FF)V");
JMETHOD(M_Path_close, C_Path, "close", "()V");
JMETHOD(M_PS_init, C_PathShape, "<init>", "(Landroid/graphics/Path;FF)V");
JMETHOD(M_SD_init, C_ShapeDrawable, "<init>", "(Landroid/graphics/drawable/shapes/Shape;)V");
JMETHOD(M_SD_getPaint, C_ShapeDrawable, "getPaint", "()Landroid/graphics/Paint;");
JMETHOD(M_Drawable_setBounds, C_Drawable, "setBounds", "(IIII)V");
JMETHOD(M_Paint_init, C_Paint, "<init>", "(I)V");
JMETHOD(M_Paint_setColor, C_Paint, "setColor", "(I)V");
JMETHOD(M_Paint_setTextSize, C_Paint, "setTextSize", "(F)V");
JMETHOD(M_Paint_measureText, C_Paint, "measureText", "(Ljava/lang/String;)F");
JMETHOD(M_Paint_getFontMetrics, C_Paint, "getFontMetrics", "()Landroid/graphics/Paint$FontMetrics;");
JCLASS(C_FontMetrics, "android/graphics/Paint$FontMetrics");
JFIELD(F_FM_ascent, C_FontMetrics, "ascent", "F");
JFIELD(F_FM_descent, C_FontMetrics, "descent", "F");
JFIELD(F_FM_leading, C_FontMetrics, "leading", "F");

// The disclosure: a triangle at the head of the row, pointing right when
// collapsed and down when expanded, drawn as a path in the button's own text
// colour (Android has no system chevron to borrow, so it is drawn the way
// the Win32 backend draws its own, in the row's ink). The caption leaves the
// screen but stays the widget's text for the driver and its content
// description for TalkBack.
void aether_ui_button_set_disclosure(int handle, int expanded) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_BUTTON) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    if (!w->text_override) {
        jobject cs = (*env)->CallObjectMethod(env, w->view, J.TextView_getText);
        w->text_override = aeui_check(env, "getText") ? strdup("") : aeui_charseq_dup(env, cs);
        jstring d = aeui_jstring(env, w->text_override);
        if (!w->a11y_name) JV(w->view, M_View_setContentDescription, d);
        set_text_on(env, w->view, "");
    }
    jobject path = JNEW(M_Path_init);
    if (path) {
        if (expanded) {
            JV(path, M_Path_moveTo, (jfloat)0.15f, (jfloat)0.3f);
            JV(path, M_Path_lineTo, (jfloat)0.85f, (jfloat)0.3f);
            JV(path, M_Path_lineTo, (jfloat)0.5f, (jfloat)0.8f);
        } else {
            JV(path, M_Path_moveTo, (jfloat)0.3f, (jfloat)0.15f);
            JV(path, M_Path_lineTo, (jfloat)0.8f, (jfloat)0.5f);
            JV(path, M_Path_lineTo, (jfloat)0.3f, (jfloat)0.85f);
        }
        JV(path, M_Path_close);
        jobject shape = JNEW(M_PS_init, path, (jfloat)1.0f, (jfloat)1.0f);
        jobject sd = shape ? JNEW(M_SD_init, shape) : NULL;
        if (sd) {
            jobject paint = JO(sd, M_SD_getPaint);
            JV(paint, M_Paint_setColor, JI(w->view, M_TV_getCurrentTextColor));
            int sz = aeui_dp(16);
            JV(sd, M_Drawable_setBounds, (jint)0, (jint)0, (jint)sz, (jint)sz);
            JV(w->view, M_TV_setCompoundDrawablesRelative, sd, (jobject)NULL, (jobject)NULL, (jobject)NULL);
            JV(w->view, M_TV_setCompoundDrawablePadding, (jint)0);
        }
    }
    aeui_unframe(env);
}
void aether_ui_button_set_disclosure_ctx(void* ctx, int expanded) {
    aether_ui_button_set_disclosure((int)(intptr_t)ctx, expanded);
}

// --- Text colour and fonts -------------------------------------------------
JCLASS(C_Typeface, "android/graphics/Typeface");
JSTATIC(M_TF_createFamily, C_Typeface, "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;");
JSTATIC(M_TF_defaultFromStyle, C_Typeface, "defaultFromStyle", "(I)Landroid/graphics/Typeface;");

// The family string goes to the platform verbatim, as on every backend:
// Android resolves the generic CSS families ("monospace", "serif",
// "sans-serif") and its own named ones ("sans-serif-condensed", ...).
static void aeui_apply_typeface(JNIEnv* env, AeuiWidget* w) {
    int style = w->bold == 1 ? 1 : 0;   // Typeface.BOLD / NORMAL
    jobject tf = NULL;
    if (w->family) {
        jstring fam = aeui_jstring(env, w->family);
        tf = JSO(M_TF_createFamily, fam, (jint)style);
    } else {
        tf = JSO(M_TF_defaultFromStyle, (jint)style);
    }
    if (tf) JV(w->view, M_TV_setTypeface, tf);
}

void aether_ui_set_text_color(int handle, double r, double g, double b) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_textview(w->type)) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_TV_setTextColor, (jint)aeui_argb(r, g, b, 1.0));
    w->styled_fg = aeui_rgb(r, g, b);
    aeui_unframe(env);
}
void aether_ui_set_text_color_ctx(void* ctx, double r, double g, double b) {
    aether_ui_set_text_color((int)(intptr_t)ctx, r, g, b);
}

// Sizes are dp, like every other length here, so text_measure (which
// measures at size dp) agrees with what a label set to that size draws.
// (sp would scale with the user's font-size setting; the DSL's sizes are
// the app's layout units, as points are on AppKit and UIKit.)
void aether_ui_set_font_size(int handle, double size) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_textview(w->type) || size <= 0) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->font_size = size;
    JV(w->view, M_TV_setTextSize, (jint)1 /* COMPLEX_UNIT_DIP */, (jfloat)size);
    aeui_unframe(env);
}
void aether_ui_set_font_size_ctx(void* ctx, double size) {
    aether_ui_set_font_size((int)(intptr_t)ctx, size);
}

void aether_ui_set_font_bold(int handle, int bold) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_textview(w->type)) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    w->bold = bold ? 1 : 0;
    aeui_apply_typeface(env, w);
    aeui_unframe(env);
}
void aether_ui_set_font_bold_ctx(void* ctx, int bold) {
    aether_ui_set_font_bold((int)(intptr_t)ctx, bold);
}

void aether_ui_set_font_family(int handle, const char* family) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_textview(w->type) || !family || !family[0]) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    free(w->family);
    w->family = strdup(family);
    aeui_apply_typeface(env, w);
    aeui_unframe(env);
}

// --- Text metrics: the platform's text measurer, at size dp, in dp ---------
static jobject aeui_metrics_paint(JNIEnv* env, double size) {
    jobject p = JNEW(M_Paint_init, (jint)1 /* ANTI_ALIAS_FLAG */);
    if (p) JV(p, M_Paint_setTextSize, (jfloat)(size * g_density));
    return p;
}

double aether_ui_text_measure(double size, const char* text) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0.0;
    double out = 0.0;
    jobject p = aeui_metrics_paint(env, size);
    jstring s = aeui_jstring(env, text);
    if (p && s) out = JF(p, M_Paint_measureText, s) / g_density;
    aeui_unframe(env);
    return out;
}

static double aeui_font_metric(double size, int which) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0.0;
    double out = 0.0;
    jobject p = aeui_metrics_paint(env, size);
    jobject fm = p ? JO(p, M_Paint_getFontMetrics) : NULL;
    if (fm) {
        double ascent = -JGETF(fm, F_FM_ascent), descent = JGETF(fm, F_FM_descent);
        double leading = JGETF(fm, F_FM_leading);
        out = (which == 0 ? ascent : which == 1 ? descent : ascent + descent + leading) / g_density;
    }
    aeui_unframe(env);
    return out;
}

double aether_ui_font_ascent(double size)  { return aeui_font_metric(size, 0); }
double aether_ui_font_descent(double size) { return aeui_font_metric(size, 1); }
double aether_ui_font_height(double size)  { return aeui_font_metric(size, 2); }

// --- Stacks: alignment and distribution --------------------------------------
// The DSL passes NSLayoutAttribute values (ALIGN_* in ui/module.ae), mapped
// as GTK4 maps them: a vstack aligns its children across (5 leading, 9
// centre, 6 trailing, 7 fill), an hstack down (3 top, 12/10 centre, 4
// bottom, 8/7 fill).
static void aeui_relayout_children(JNIEnv* env, int handle) {
    for (int i = 0; i < widget_count; i++)
        if (widgets[i].parent == handle && widgets[i].view) aeui_apply_lp(env, i + 1);
}

void aether_ui_set_alignment(int handle, int alignment) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_linear(w->type)) return;
    int a;
    if (aeui_is_vertical(w->type))
        a = alignment == 5 || alignment == 1 ? 1 : alignment == 9 ? 2 :
            alignment == 6 || alignment == 2 ? 3 : alignment == 7 ? 4 : 0;
    else
        a = alignment == 3 ? 1 : (alignment == 12 || alignment == 10) ? 2 :
            alignment == 4 ? 3 : (alignment == 8 || alignment == 7) ? 4 : 0;
    w->align = a;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    aeui_relayout_children(env, handle);
    aeui_unframe(env);
}

// 1 = fill equally (every child an equal share of the main axis: weight 1 on
// a zero main size, LinearLayout's own equal split); anything else is fill.
// The proportional and equal-spacing distributions AppKit names have no
// LinearLayout counterpart and lay out as fill.
void aether_ui_set_distribution(int handle, int distribution) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !aeui_is_linear(w->type)) return;
    w->distribution = distribution == 1 ? 1 : 0;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    aeui_relayout_children(env, handle);
    aeui_unframe(env);
}

// --- Sizes ------------------------------------------------------------------
// A stated size is exactly that size on that axis (and no longer a share of
// the slack); 0 clears it. The old set_width/set_height and the #95 _impl
// pair are the same request here, as on GTK4.
static void aeui_set_fixed(int handle, int px, int vertical) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    if (vertical) w->fixed_h = px > 0 ? px : 0;
    else w->fixed_w = px > 0 ? px : 0;
    aeui_apply_lp(env, handle);
    aeui_update_expand(env, handle);
    aeui_unframe(env);
}

void aether_ui_set_width(int handle, int width) { aeui_set_fixed(handle, width, 0); }
void aether_ui_set_height(int handle, int height) { aeui_set_fixed(handle, height, 1); }
void aether_ui_set_width_impl(int handle, int px) { aeui_set_fixed(handle, px, 0); }
void aether_ui_set_height_impl(int handle, int px) { aeui_set_fixed(handle, px, 1); }

// A floor: View's minimum size, which every layout measures against, so the
// widget opens at least this big and can still grow.
void aether_ui_set_min_width_impl(int handle, int px) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->min_w = px > 0 ? px : 0;
    JV(w->view, M_View_setMinimumWidth, (jint)aeui_dp(w->min_w));
    aeui_unframe(env);
}
void aether_ui_set_min_height_impl(int handle, int px) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->min_h = px > 0 ? px : 0;
    JV(w->view, M_View_setMinimumHeight, (jint)aeui_dp(w->min_h));
    aeui_unframe(env);
}
int aether_ui_get_min_width_impl(int handle) { AeuiWidget* w = live_widget(handle); return w ? w->min_w : 0; }
int aether_ui_get_min_height_impl(int handle) { AeuiWidget* w = live_widget(handle); return w ? w->min_h : 0; }

// What the widget actually got, in dp, from its rounded EDGES (aether-ui
// #101: rounding each size alone makes a row of children add up to more
// than their parent). 0 before the first layout pass.
static int aeui_rect_dp(JNIEnv* env, jobject v, int* x, int* y, int* w, int* h) {
    jintArray loc = (*env)->NewIntArray(env, 2);
    if (!loc) { aeui_check(env, "NewIntArray"); return 1; }
    JV(v, M_View_getLocationInWindow, loc);
    jint xy[2] = { 0, 0 };
    (*env)->GetIntArrayRegion(env, loc, 0, 2, xy);
    (*env)->DeleteLocalRef(env, loc);
    int pw = JI(v, M_View_getWidth), ph = JI(v, M_View_getHeight);
    int x0 = aeui_px_to_dp(xy[0]), y0 = aeui_px_to_dp(xy[1]);
    *x = x0;
    *y = y0;
    *w = aeui_px_to_dp(xy[0] + pw) - x0;
    *h = aeui_px_to_dp(xy[1] + ph) - y0;
    return 0;
}

static int aeui_extent(int handle, int vertical) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return 0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    int x = 0, y = 0, ww = 0, hh = 0;
    aeui_rect_dp(env, w->view, &x, &y, &ww, &hh);
    aeui_unframe(env);
    return vertical ? hh : ww;
}
int aether_ui_get_width_impl(int handle) { return aeui_extent(handle, 0); }
int aether_ui_get_height_impl(int handle) { return aeui_extent(handle, 1); }

// Take the slack on that axis (GTK4's hexpand/vexpand + fill).
static void aeui_match_parent(int handle, int vertical) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    if (vertical) w->own_vexp = 1; else w->own_hexp = 1;
    aeui_update_expand(env, handle);
    aeui_apply_lp(env, handle);
    aeui_unframe(env);
}
void aether_ui_match_parent_width(int handle) { aeui_match_parent(handle, 0); }
void aether_ui_match_parent_height(int handle) { aeui_match_parent(handle, 1); }

// --- Margins and insets ----------------------------------------------------
// A container's margin is the space inside its edge, before its children
// (NSStackView's edgeInsets on AppKit, layoutMargins on UIKit, and what
// GTK4's outer margin looks like around a box's content). A leaf's is the
// space outside it, in its parent (GTK4's widget margin): a button has a
// padding of its own, which is its look, not its spacing.
static void aeui_margin(int handle, int top, int right, int bottom, int left) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    if (aeui_is_container(w->type)) {
        JV(w->view, M_View_setPadding, (jint)aeui_dp(left), (jint)aeui_dp(top),
           (jint)aeui_dp(right), (jint)aeui_dp(bottom));
    } else {
        w->has_margin = 1;
        w->mt = top; w->mr = right; w->mb = bottom; w->ml = left;
        aeui_apply_lp(env, handle);
    }
    aeui_unframe(env);
}
void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    aeui_margin(handle, top, right, bottom, left);
}
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aeui_margin((int)(intptr_t)ctx, top, right, bottom, left);
}

// Padding, on any widget (GTK4's CSS padding).
void aether_ui_set_edge_insets(int handle, double top, double right, double bottom, double left) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_View_setPadding, (jint)aeui_dp((int)left), (jint)aeui_dp((int)top),
       (jint)aeui_dp((int)right), (jint)aeui_dp((int)bottom));
    aeui_unframe(env);
}

// --- Direction, tooltip, focusability --------------------------------------
// A View's layout direction: an hstack lays its children right to left, and
// it is inherited by what is inside, as GTK's widget direction is.
void aether_ui_set_rtl(int handle, int on) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_View_setLayoutDirection, (jint)(on ? 1 : 0));   // LAYOUT_DIRECTION_RTL / LTR
    aeui_unframe(env);
}

// The platform tooltip (API 26): shown on a long press, and on hover under
// a mouse.
void aether_ui_set_tooltip(int handle, const char* text) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    jstring s = (text && *text) ? aeui_jstring(env, text) : NULL;
    JV(w->view, M_View_setTooltipText, s);
    aeui_unframe(env);
}
void aether_ui_set_tooltip_ctx(void* ctx, const char* text) {
    aether_ui_set_tooltip((int)(intptr_t)ctx, text);
}

// A container (a list row) becomes focusable in touch mode too, or
// focus() could never reach it on a phone; a control keeps the platform's
// touch-mode rule, since one focusable in touch mode takes two taps to press.
void aether_ui_set_focusable_impl(int handle, int on) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_View_setFocusable, on ? JNI_TRUE : JNI_FALSE);
    if (aeui_is_container(w->type) || w->type == AUI_TEXT || w->type == AUI_IMAGE)
        JV(w->view, M_View_setFocusableInTouchMode, on ? JNI_TRUE : JNI_FALSE);
    aeui_unframe(env);
}

JCLASS(C_Activity, "android/app/Activity");
JMETHOD(M_Act_getCurrentFocus, C_Activity, "getCurrentFocus", "()Landroid/view/View;");

void aether_ui_focus_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JZ(w->view, M_View_requestFocus);
    aeui_unframe(env);
}

int aether_ui_focused_widget(void) {
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    jobject f = g_activity ? JO(g_activity, M_Act_getCurrentFocus) : NULL;
    int h = f ? aether_ui_handle_for_widget(f) : 0;
    aeui_unframe(env);
    return h;
}

// --- Enabled and hidden ----------------------------------------------------
// Android's enabled flag is per View, where GTK4's sensitivity is inherited:
// disabling a box disables what is in it. So a widget's View is enabled when
// it AND every ancestor are; the flag the driver reports is the widget's own
// (gtk_widget_get_sensitive's answer).
static void aeui_apply_enabled(JNIEnv* env, int handle, int parent_on) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    int on = parent_on && !w->disabled;
    JV(w->view, M_View_setEnabled, on ? JNI_TRUE : JNI_FALSE);
    for (int i = 0; i < widget_count; i++)
        if (widgets[i].parent == handle && widgets[i].view) aeui_apply_enabled(env, i + 1, on);
}

void aether_ui_set_enabled(int handle, int enabled) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(64);
    if (!env) return;
    w->disabled = enabled ? 0 : 1;
    aeui_apply_enabled(env, handle, aeui_parent_enabled(handle));
    aeui_unframe(env);
}
void aether_ui_set_enabled_ctx(void* ctx, int enabled) {
    aether_ui_set_enabled((int)(intptr_t)ctx, enabled);
}

// Hidden takes no space (View.GONE), as an unmapped GTK widget does.
void aether_ui_widget_set_hidden(int handle, int hidden) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_View_setVisibility, (jint)(hidden ? 8 /* GONE */ : 0 /* VISIBLE */));
    aeui_unframe(env);
}

// ===========================================================================
// Accessibility. The role is the class name TalkBack announces (set through
// an AetherA11y delegate), "heading" is View's heading flag and "none" takes
// the widget out of the accessibility tree; the name is the content
// description; the description is the node's hint text. The readback reports
// the role as set (or the widget's own when unset, as AppKit's auto-role
// does) and the name as set (or the widget's text, as GTK4 and AppKit
// answer for a label or a button).
// ===========================================================================
// Resolved in JNI_OnLoad (C_A11y.cls = J.A11y): an app class, so only the
// app's class loader finds it.
JCLASS(C_A11y, "dev/aether/ui/AetherA11y");
JMETHOD(M_A11y_setClassName, C_A11y, "setClassName", "(Ljava/lang/String;)V");
JMETHOD(M_A11y_setHint, C_A11y, "setHint", "(Ljava/lang/String;)V");

static const char* aeui_role_class(const char* role) {
    static const struct { const char* role; const char* cls; } map[] = {
        {"button", "android.widget.Button"}, {"checkbox", "android.widget.CheckBox"},
        {"radio", "android.widget.RadioButton"}, {"link", "android.widget.Button"},
        {"heading", "android.widget.TextView"}, {"image", "android.widget.ImageView"},
        {"group", "android.view.ViewGroup"}, {"table", "android.widget.GridView"},
        {"list", "android.widget.ListView"}, {"tablist", "android.widget.TabWidget"},
        {"textbox", "android.widget.EditText"}, {"slider", "android.widget.SeekBar"},
        {"progressbar", "android.widget.ProgressBar"}, {"menu", "android.widget.ListView"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(map[i].role, role) == 0) return map[i].cls;
    return NULL;   // a role with no widget class of its own keeps the View's
}

static jobject aeui_a11y_delegate(JNIEnv* env, AeuiWidget* w) {
    if (w->a11y) return w->a11y;
    jobject d = (*env)->NewObject(env, J.A11y, J.A11y_init);
    if (aeui_check(env, "new AetherA11y") || !d) return NULL;
    w->a11y = (*env)->NewGlobalRef(env, d);
    JV(w->view, M_View_setAccessibilityDelegate, w->a11y);
    return w->a11y;
}

void aether_ui_a11y_set_role_impl(int handle, const char* role) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !role) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    free(w->role);
    w->role = strdup(role);
    jobject d = aeui_a11y_delegate(env, w);
    const char* cls = aeui_role_class(role);
    if (d) JV(d, M_A11y_setClassName, cls ? aeui_jstring(env, cls) : (jstring)NULL);
    JV(w->view, M_View_setAccessibilityHeading, strcmp(role, "heading") == 0 ? JNI_TRUE : JNI_FALSE);
    JV(w->view, M_View_setImportantForAccessibility,
       (jint)(strcmp(role, "none") == 0 ? 2 /* NO */ : 0 /* AUTO */));
    aeui_unframe(env);
}

void aether_ui_a11y_set_label_impl(int handle, const char* name) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !name) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    free(w->a11y_name);
    w->a11y_name = strdup(name);
    JV(w->view, M_View_setContentDescription, aeui_jstring(env, name));
    aeui_unframe(env);
}

void aether_ui_a11y_set_description_impl(int handle, const char* desc) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !desc) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    free(w->a11y_desc);
    w->a11y_desc = strdup(desc);
    jobject d = aeui_a11y_delegate(env, w);
    if (d) JV(d, M_A11y_setHint, aeui_jstring(env, desc));
    aeui_unframe(env);
}

static const char* aeui_auto_role(int type) {
    switch (type) {
        case AUI_BUTTON:      return "button";
        case AUI_TOGGLE:      return "checkbox";
        case AUI_TEXTFIELD:
        case AUI_SECUREFIELD:
        case AUI_TEXTAREA:    return "textbox";
        case AUI_SLIDER:      return "slider";
        case AUI_PROGRESSBAR: return "progressbar";
        case AUI_IMAGE:       return "image";
        default:              return "";
    }
}

static void aeui_copy(char* dst, int sz, const char* s) {
    if (!dst || sz <= 0) return;
    snprintf(dst, (size_t)sz, "%s", s ? s : "");
}

void aether_ui_a11y_get_impl(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    aeui_copy(role, rolesz, "");
    aeui_copy(name, namesz, "");
    aeui_copy(desc, descsz, "");
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    aeui_copy(role, rolesz, w->role ? w->role : aeui_auto_role(w->type));
    aeui_copy(desc, descsz, w->a11y_desc);
    if (w->a11y_name) {
        aeui_copy(name, namesz, w->a11y_name);
    } else if (w->text_override) {
        aeui_copy(name, namesz, w->text_override);
    } else if (w->type == AUI_TEXT || w->type == AUI_BUTTON || w->type == AUI_TOGGLE) {
        JNIEnv* env = aeui_frame(4);
        if (!env) return;
        jobject cs = (*env)->CallObjectMethod(env, w->view, J.TextView_getText);
        if (!aeui_check(env, "getText") && cs && name && namesz > 0)
            aeui_charseq_into(env, cs, name, namesz);
        aeui_unframe(env);
    }
}

// ===========================================================================
// Events on any widget.
// ===========================================================================
void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    if (!live_widget(handle) || !boxed_closure) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    aeui_add_click(env, handle, (AeClosure*)boxed_closure);
    aeui_unframe(env);
}

// The platform's double-tap (GestureDetector over the View's touches).
void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !boxed_closure) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->dbl = (AeClosure*)boxed_closure;
    if (!(w->listeners & LST_DOUBLE)) {
        jobject l = (*env)->NewObject(env, J.Listener, J.Listener_initDouble, g_activity, (jint)handle);
        if (!aeui_check(env, "new AetherListener(double)") && l) {
            JV(w->view, M_View_setOnTouchListener, l);
            // A plain container ignores the touch stream unless clickable.
            JV(w->view, M_View_setClickable, JNI_TRUE);
            w->listeners |= LST_DOUBLE;
        }
    }
    aeui_unframe(env);
}

// Enter (1) and leave (0), as GTK4's motion controller reports them. Real
// under a pointer -- a mouse, a trackpad, a stylus hovering -- which is the
// only thing on Android that hovers; a finger on glass never does, so on a
// plain touch screen the closure simply never runs (UIKit's pointer
// interaction is the same).
void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !boxed_closure) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    closure_push(&w->hovers, &w->nhovers, (AeClosure*)boxed_closure);
    if (!(w->listeners & LST_HOVER)) {
        jobject l = aeui_listener(env, handle, AEUI_EV_HOVER);
        if (l) { JV(w->view, M_View_setOnHoverListener, l); w->listeners |= LST_HOVER; }
    }
    aeui_unframe(env);
}

// cb(w, h) in dp after the widget's size settles and CHANGES, deferred off
// the layout pass (the closure may build widgets), as GTK4 defers to idle.
void aether_ui_on_layout_impl(int handle, void* boxed_closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !boxed_closure) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    w->layout_cb = (AeClosure*)boxed_closure;
    w->layout_w = w->layout_h = 0;
    if (!(w->listeners & LST_LAYOUT)) {
        jobject l = aeui_listener(env, handle, AEUI_EV_LAYOUT);
        if (l) { JV(w->view, M_View_addOnLayoutChangeListener, l); w->listeners |= LST_LAYOUT; }
    }
    JV(w->view, M_View_requestLayout);   // the first report, as GTK queues a resize
    aeui_unframe(env);
}

// The modifier keys held right now. Always 0 on Android, and that is the
// honest answer, as on UIKit: a touch carries no modifiers and Android has
// no pollable global modifier state -- a hardware keyboard's modifiers
// arrive on each KeyEvent (getMetaState), which answers a different
// question from "what is held now". A cross-platform app reading modifiers
// in a click handler sees an unmodified click.
int aether_ui_modifiers_impl(void) {
    return 0;
}
// ===========================================================================
// Reactive state -- typed cells and property bindings. The same
// platform-free subsystem every backend carries (lifted from the UIKit
// backend): a set re-applies the bindings on that cell through the widget
// setters (text_set_string, set_enabled, widget_set_hidden, the field's
// text), and observers stay in the DSL (aether_ui_state_notify).
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

enum { AEUI_BIND_TEXT = 0, AEUI_BIND_ENABLED = 1, AEUI_BIND_HIDDEN = 2, AEUI_BIND_VALUE = 3 };

typedef struct {
    int kind, state_handle, widget_handle;
    char* prefix;
    char* suffix;
    int decimals;
    int invert;
} PropBinding;

static StateCell* state_cells = NULL;
static int state_count = 0, state_capacity = 0;
static PropBinding* bindings = NULL;
static int binding_count = 0, binding_capacity = 0;

static StateCell* state_cell(int handle) {
    if (handle < 1 || handle > state_count) return NULL;
    return &state_cells[handle - 1];
}

static int state_create_cell(int type, double num, const char* str) {
    if (state_count >= state_capacity) {
        int cap = state_capacity == 0 ? 32 : state_capacity * 2;
        StateCell* nc = (StateCell*)realloc(state_cells, sizeof(StateCell) * (size_t)cap);
        if (!nc) return 0;
        state_cells = nc;
        state_capacity = cap;
    }
    StateCell* c = &state_cells[state_count];
    c->type = type; c->num = num;
    c->str = str ? strdup(str) : NULL; c->list = NULL; c->rev = 0;
    state_count++;
    return state_count;
}

int aether_ui_state_create(double initial)  { return state_create_cell(AEUI_STATE_FLOAT, initial, NULL); }
int aether_ui_state_create_s(const char* i) { return state_create_cell(AEUI_STATE_STRING, 0.0, i ? i : ""); }
int aether_ui_state_create_i(int initial)   { return state_create_cell(AEUI_STATE_INT, (double)initial, NULL); }
int aether_ui_state_create_b(int initial)   { return state_create_cell(AEUI_STATE_BOOL, initial ? 1.0 : 0.0, NULL); }

double aether_ui_state_get(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_FLOAT) ? c->num : 0.0;
}
const char* aether_ui_state_get_s(int handle) {   // malloc'd -- the extern owns it
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
        case AEUI_STATE_STRING: snprintf(buf, (size_t)n, "%s", c->str ? c->str : ""); break;
        case AEUI_STATE_INT:    snprintf(buf, (size_t)n, "%d", (int)c->num); break;
        case AEUI_STATE_BOOL:   snprintf(buf, (size_t)n, "%s", c->num != 0.0 ? "true" : "false"); break;
        default:
            if (decimals >= 0)              snprintf(buf, (size_t)n, "%.*f", decimals, c->num);
            else if (c->num == (int)c->num) snprintf(buf, (size_t)n, "%d", (int)c->num);
            else                            snprintf(buf, (size_t)n, "%.2f", c->num);
    }
}

static int state_truthy(StateCell* c) {
    if (!c) return 0;
    if (c->type == AEUI_STATE_STRING) return c->str && c->str[0];
    return c->num != 0.0;
}

static void apply_binding(PropBinding* b) {
    StateCell* c = state_cell(b->state_handle);
    if (!c) return;
    if (b->kind == AEUI_BIND_VALUE) {
        // Compare first: the field's write-back sets this same cell, and an
        // unconditional set here would bounce between them.
        char* cur = (char*)aether_ui_textfield_get_text(b->widget_handle);
        const char* want = (c->type == AEUI_STATE_STRING && c->str) ? c->str : "";
        if (strcmp(cur, want) != 0) {
            g_seeding++;
            aether_ui_textfield_set_text(b->widget_handle, want);
            g_seeding--;
        }
        free(cur);
        return;
    }
    if (b->kind == AEUI_BIND_TEXT) {
        char val[256];
        state_render_value(c, b->decimals, val, sizeof(val));
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s%s", b->prefix, val, b->suffix);
        aether_ui_text_set_string(b->widget_handle, buf);
        return;
    }
    int on = state_truthy(c);
    if (b->invert) on = !on;
    if (b->kind == AEUI_BIND_ENABLED) aether_ui_set_enabled(b->widget_handle, on);
    else aether_ui_widget_set_hidden(b->widget_handle, on);
}

static void update_bindings(int state_handle) {
    for (int i = 0; i < binding_count; i++)
        if (bindings[i].state_handle == state_handle) apply_binding(&bindings[i]);
    aether_ui_state_notify(state_handle);
}

int aether_ui_state_create_list(void* list_ptr) {
    int h = state_create_cell(AEUI_STATE_LIST, 0.0, NULL);
    StateCell* c = state_cell(h);
    if (c) c->list = list_ptr;
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
    update_bindings(handle);
}
int aether_ui_state_list_rev(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_LIST) ? c->rev : 0;
}

void aether_ui_state_set(int handle, double value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_FLOAT) return;
    c->num = value; update_bindings(handle);
}
void aether_ui_state_set_s(int handle, const char* value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_STRING) return;
    free(c->str); c->str = strdup(value ? value : "");
    update_bindings(handle);
}
void aether_ui_state_set_i(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_INT) return;
    c->num = (double)value; update_bindings(handle);
}
void aether_ui_state_set_b(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_BOOL) return;
    c->num = value ? 1.0 : 0.0; update_bindings(handle);
}

static PropBinding* binding_new(int kind, int state_handle, int widget_handle) {
    if (binding_count >= binding_capacity) {
        int cap = binding_capacity == 0 ? 32 : binding_capacity * 2;
        PropBinding* nb = (PropBinding*)realloc(bindings, sizeof(PropBinding) * (size_t)cap);
        if (!nb) return NULL;
        bindings = nb;
        binding_capacity = cap;
    }
    PropBinding* b = &bindings[binding_count++];
    b->kind = kind;
    b->state_handle = state_handle;
    b->widget_handle = widget_handle;
    b->prefix = strdup("");
    b->suffix = strdup("");
    b->decimals = -1;
    b->invert = 0;
    return b;
}

void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix) {
    PropBinding* b = binding_new(AEUI_BIND_TEXT, state_handle, text_handle);
    if (!b) return;
    free(b->prefix);
    free(b->suffix);
    b->prefix = strdup(prefix ? prefix : "");
    b->suffix = strdup(suffix ? suffix : "");
    apply_binding(b);
}

void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals) {
    PropBinding* b = binding_new(AEUI_BIND_TEXT, state_handle, widget_handle);
    if (!b) return;
    b->decimals = decimals;
    apply_binding(b);
}

void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = binding_new(AEUI_BIND_ENABLED, state_handle, widget_handle);
    if (!b) return;
    b->invert = invert;
    apply_binding(b);
}

void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = binding_new(AEUI_BIND_HIDDEN, state_handle, widget_handle);
    if (!b) return;
    b->invert = invert;
    apply_binding(b);
}

// Two-way: string state <-> a field (textfield, securefield or textarea --
// all EditText here). State -> field is a VALUE binding; field -> state is
// the field's TextWatcher writing the cell (native_event), for a keystroke,
// the driver's set_text and a programmatic set alike, as on AppKit.
void aether_ui_bind_value(int state_handle, int widget_handle) {
    AeuiWidget* w = live_widget(widget_handle);
    PropBinding* b = binding_new(AEUI_BIND_VALUE, state_handle, widget_handle);
    if (!b) return;
    if (w && aeui_is_edit(w->type)) w->bound_state = state_handle;
    apply_binding(b);   // seed the field from the state's initial value
}
// ===========================================================================
// Background work and timers, through the looper.
// ===========================================================================
static int aeui_worker_poster_installed = 0;

static void aeui_worker_deliver_job(void* job) { aether_worker_deliver(job); }

static void aeui_worker_post(void* env, void* job) {
    (void)env;
    aeui_android_post(aeui_worker_deliver_job, job);
}

void aether_ui_worker_poster_install_impl(void) {
    if (aeui_worker_poster_installed || !g_looper) return;
    AetherUiWorkerClosure poster;
    poster.fn = (void (*)(void))aeui_worker_post;
    poster.env = NULL;
    aether_worker_set_main_poster(poster);
    aeui_worker_poster_installed = 1;
}

int aether_ui_on_ui_thread_impl(void) { return aeui_on_ui_thread(); }

// A timer is a timerfd on the UI looper: the kernel keeps time, the looper
// wakes the UI thread, the closure runs there. 1-based ids; a cancelled slot
// keeps its id so ids are never reused.
typedef struct { int fd; AeClosure* closure; } AeuiTimer;
static AeuiTimer* timers = NULL;
static int timer_count = 0, timer_capacity = 0;

static int aeui_timer_cb(int fd, int events, void* data) {
    (void)events;
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) return 1;
    int id = (int)(intptr_t)data;
    if (id < 1 || id > timer_count || timers[id - 1].fd != fd) return 0;
    aeui_call0(timers[id - 1].closure);   // once per wakeup, however many periods passed
    return 1;
}

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (!boxed_closure || !g_looper) return 0;
    if (interval_ms < 1) interval_ms = 1;
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) { AEUI_LOGE("timerfd_create: %s", strerror(errno)); return 0; }
    struct itimerspec its;
    its.it_interval.tv_sec = interval_ms / 1000;
    its.it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    its.it_value = its.it_interval;
    timerfd_settime(fd, 0, &its, NULL);
    if (timer_count >= timer_capacity) {
        int cap = timer_capacity == 0 ? 8 : timer_capacity * 2;
        AeuiTimer* nt = (AeuiTimer*)realloc(timers, sizeof(AeuiTimer) * (size_t)cap);
        if (!nt) { close(fd); return 0; }
        timers = nt;
        timer_capacity = cap;
    }
    timers[timer_count].fd = fd;
    timers[timer_count].closure = (AeClosure*)boxed_closure;
    timer_count++;
    ALooper_addFd(g_looper, fd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                  aeui_timer_cb, (void*)(intptr_t)timer_count);
    return timer_count;
}

void aether_ui_timer_cancel_impl(int timer_id) {
    if (timer_id < 1 || timer_id > timer_count) return;
    AeuiTimer* t = &timers[timer_id - 1];
    if (t->fd < 0) return;
    if (g_looper) ALooper_removeFd(g_looper, t->fd);
    close(t->fd);
    t->fd = -1;
    t->closure = NULL;
}


// ===========================================================================
// AetherUIDriver hooks. The whole request is serviced on the UI thread
// (run_on_ui_thread), so every hook below runs there and may touch Views.
// ===========================================================================
static int hook_widget_count(void) { return widget_count; }

static const char* hook_widget_type(int handle) {
    AeuiWidget* w = widget_at(handle);
    if (!w || !w->view) return "null";
    return aeui_kind_name(w->type);
}

// What is on screen, read back from the View: a label's or a button's text,
// a field's content, a toggle's label, a picker's chosen item. A caption the
// display replaced (a disclosure) is still the widget's text.
static void hook_widget_text_into(int handle, char* buf, int bufsize) {
    buf[0] = '\0';
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    if (w->text_override) { snprintf(buf, (size_t)bufsize, "%s", w->text_override); return; }
    if (w->type == AUI_PICKER) {
        if (w->selected >= 0 && w->selected < w->nitems)
            snprintf(buf, (size_t)bufsize, "%s", w->items[w->selected]);
        return;
    }
    if (!aeui_is_textview(w->type)) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    jobject cs = (*env)->CallObjectMethod(env, w->view, J.TextView_getText);
    if (!aeui_check(env, "TextView.getText") && cs) aeui_charseq_into(env, cs, buf, bufsize);
    aeui_unframe(env);
}

// The widget's OWN flag, as GTK's gtk_widget_get_visible reads it.
static int hook_widget_visible(int handle) {
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (!v || !env) return 0;
    jint vis = (*env)->CallIntMethod(env, v, J.View_getVisibility);
    aeui_check(env, "View.getVisibility");
    return vis == 0;   // View.VISIBLE
}

// The widget's own enabled flag (gtk_widget_get_sensitive): a child of a
// disabled box is disabled on screen but reports its own setting.
static int hook_widget_enabled(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && !w->disabled) ? 1 : 0;
}

static int hook_widget_parent(int handle) { return aether_ui_widget_parent_impl(handle); }

static int hook_widget_children(int handle, int* out_handles, int max) {
    if (!live_widget(handle)) return -1;
    JNIEnv* env = aeui_frame(64);
    if (!env) return 0;
    int kids[1024];
    int n = aeui_children_in_order(env, handle, kids, 1024);
    aeui_unframe(env);
    if (!out_handles) return n;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out_handles[i] = kids[i];
    return n;
}

// Window-local, in dp: the logical units AppKit's and UIKit's points and
// GTK4's logical pixels are, so a size the app set (dp) reads back as set.
static int hook_widget_rect(int handle, int* x, int* y, int* w, int* hgt) {
    jobject v = view_of(handle);
    if (!v) return 1;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 1;
    int rc = aeui_rect_dp(env, v, x, y, w, hgt);
    aeui_unframe(env);
    return rc;
}

static int hook_toggle_active(int handle) { return aether_ui_toggle_get_active(handle); }
static double hook_slider_value(int handle) { return aether_ui_slider_get_value(handle); }
static double hook_progressbar_fraction(int handle) { return aeui_progress_fraction(handle); }
static int hook_focused_widget(void) { return aether_ui_focused_widget(); }

static void hook_widget_a11y(int handle, char* role, int rolesz,
                             char* name, int namesz, char* desc, int descsz) {
    aether_ui_a11y_get_impl(handle, role, rolesz, name, namesz, desc, descsz);
}

// The View's own hovered / pressed state: what the platform's state
// machinery (and the StateListDrawable a hover or pressed colour installs)
// is showing.
static int aeui_view_state(int handle, int pressed) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return 0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    int on = (pressed ? JZ(w->view, M_View_isPressed) : JZ(w->view, M_View_isHovered)) ? 1 : 0;
    aeui_unframe(env);
    return on;
}
static int hook_widget_hovered(int handle) { return aeui_view_state(handle, 0); }
static int hook_widget_pressed(int handle) { return aeui_view_state(handle, 1); }

// GET /screenshot. The activity's decor view (action bar, title and body)
// drawn into a Bitmap through the same View.draw the compositor uses, then
// Bitmap.compress to PNG. It needs no window on screen, so it answers with
// the phone locked or the app in the background, as the desktop backends'
// offscreen captures do.
static int hook_screenshot_png(unsigned char** out_data, size_t* out_len) {
    JNIEnv* env = aeui_env();
    if (!env || !g_activity || !J.Bitmap) return 1;
    if ((*env)->PushLocalFrame(env, 16) != 0) return 1;
    int rc = 1;
    jobject window = (*env)->CallObjectMethod(env, g_activity, J.Activity_getWindow);
    jobject decor = window ? (*env)->CallObjectMethod(env, window, J.Window_getDecorView) : NULL;
    if (aeui_check(env, "getDecorView") || !decor) goto done;
    jint w = (*env)->CallIntMethod(env, decor, J.View_getWidth);
    jint h = (*env)->CallIntMethod(env, decor, J.View_getHeight);
    if (w <= 0 || h <= 0) goto done;   // not laid out yet
    jobject bmp = (*env)->CallStaticObjectMethod(env, J.Bitmap, J.Bitmap_createBitmap,
                                                 w, h, J.ARGB_8888);
    if (aeui_check(env, "Bitmap.createBitmap") || !bmp) goto done;
    jobject canvas = (*env)->NewObject(env, J.Canvas, J.Canvas_init, bmp);
    if (aeui_check(env, "new Canvas") || !canvas) goto done;
    (*env)->CallVoidMethod(env, canvas, J.Canvas_drawColor, (jint)0xFFFFFFFF);
    (*env)->CallVoidMethod(env, decor, J.View_draw, canvas);
    if (aeui_check(env, "View.draw")) goto done;
    jobject baos = (*env)->NewObject(env, J.ByteArrayOutputStream, J.BAOS_init);
    jboolean ok = (*env)->CallBooleanMethod(env, bmp, J.Bitmap_compress, J.PNG, (jint)100, baos);
    (*env)->CallVoidMethod(env, bmp, J.Bitmap_recycle);
    if (aeui_check(env, "Bitmap.compress") || !ok) goto done;
    jbyteArray bytes = (jbyteArray)(*env)->CallObjectMethod(env, baos, J.BAOS_toByteArray);
    if (aeui_check(env, "toByteArray") || !bytes) goto done;
    jsize n = (*env)->GetArrayLength(env, bytes);
    unsigned char* data = (unsigned char*)malloc((size_t)n);
    if (!data) goto done;
    (*env)->GetByteArrayRegion(env, bytes, 0, n, (jbyte*)data);
    *out_data = data;
    *out_len = (size_t)n;
    rc = 0;
done:
    (*env)->PopLocalFrame(env, NULL);
    return rc;
}

JMETHOD(M_View_performClick, C_View, "performClick", "()Z");

// One widget hovered at a time: the pointer is over at most one thing.
static void aeui_driver_hover(JNIEnv* env, int handle) {
    for (int i = 0; i < widget_count; i++) {
        if (!widgets[i].view || i + 1 == handle) continue;
        if (JZ(widgets[i].view, M_View_isHovered)) JV(widgets[i].view, M_View_setHovered, JNI_FALSE);
    }
    AeuiWidget* w = live_widget(handle);
    if (w) JV(w->view, M_View_setHovered, JNI_TRUE);
}

static void driver_perform(AetherDriverActionCtx* ctx) {
    switch (ctx->action) {
        case AETHER_DRV_SET_STATE:
            switch (aether_ui_state_type(ctx->handle)) {
                case AEUI_STATE_INT:  aether_ui_state_set_i(ctx->handle, atoi(ctx->sval)); break;
                case AEUI_STATE_BOOL: aether_ui_state_set_b(ctx->handle,
                                          strcmp(ctx->sval, "true") == 0 || atoi(ctx->sval) != 0); break;
                case AEUI_STATE_STRING: aether_ui_state_set_s(ctx->handle, ctx->sval); break;
                default: aether_ui_state_set(ctx->handle, ctx->dval);
            }
            ctx->result = 0;
            return;
        case AETHER_DRV_SHUTDOWN:
            // The end of a test run: the process exits and the port with it,
            // as the UIKit backend does (Android has no programmatic quit for
            // an app either).
            fflush(stdout);
            fflush(stderr);
            exit(0);
        case AETHER_DRV_CLICK:
        case AETHER_DRV_SET_TEXT:
        case AETHER_DRV_TOGGLE:
        case AETHER_DRV_SET_VALUE:
        case AETHER_DRV_FOCUS:
        case AETHER_DRV_HOVER:
        case AETHER_DRV_PRESS:
        case AETHER_DRV_RELEASE:
            break;   // handled below, against the widget
        default:
            // The rest (canvas, split, tabs, menus, window) arrive with the
            // passes that bring those widgets: 404 honestly rather than pretend.
            ctx->result = 3;
            return;
    }

    JNIEnv* env = aeui_frame(32);
    if (!env) { ctx->result = 3; return; }
    AeuiWidget* w = live_widget(ctx->handle);
    if (!w) {
        // hover(0) = "the pointer is over NOTHING": clear every hover.
        if (ctx->action == AETHER_DRV_HOVER && ctx->handle == 0) {
            aeui_driver_hover(env, 0);
            ctx->retval = 1;
            ctx->result = 0;
        } else {
            ctx->result = 3;
        }
        aeui_unframe(env);
        return;
    }
    if (ctx->action == AETHER_DRV_FOCUS) {
        aether_ui_focus_impl(ctx->handle);
        ctx->result = 0;
        aeui_unframe(env);
        return;
    }
    if (ctx->handle == aether_ui_test_server_banner_handle()) { ctx->result = 2; aeui_unframe(env); return; }
    if (aether_ui_test_server_is_sealed(ctx->handle)) { ctx->result = 1; aeui_unframe(env); return; }

    switch (ctx->action) {
        case AETHER_DRV_CLICK:
        case AETHER_DRV_TOGGLE:
            // performClick runs what a tap runs: the click listener (and the
            // click sound / accessibility event with it), and a CheckBox's
            // own toggle, which fires its change listener.
            if (ctx->action == AETHER_DRV_CLICK || w->type == AUI_TOGGLE)
                JZ(w->view, M_View_performClick);
            break;
        case AETHER_DRV_SET_TEXT:
            if (aeui_is_edit(w->type)) {
                // Not a programmatic set: the TextWatcher runs the field's
                // on_change and a bound field's write-back, as typing does.
                set_text_on(env, w->view, ctx->sval);
            } else if (w->type == AUI_TEXT || w->type == AUI_BUTTON) {
                aether_ui_text_set_string(ctx->handle, ctx->sval);
            }
            break;
        case AETHER_DRV_SET_VALUE:
            if (w->type == AUI_SLIDER) {
                // Move the thumb, then fire the change a drag would have.
                aether_ui_slider_set_value(ctx->handle, ctx->dval);
                AeClosure* c = w->change;
                if (c && c->fn)
                    ((void (*)(void*, double))c->fn)(c->env, aether_ui_slider_get_value(ctx->handle));
            } else if (w->type == AUI_PROGRESSBAR) {
                aether_ui_progressbar_set_fraction(ctx->handle, ctx->dval);
            } else if (w->type == AUI_PICKER) {
                aether_ui_picker_set_selected(ctx->handle, (int)ctx->dval);   // fires on_change
            }
            break;
        case AETHER_DRV_HOVER:
            aeui_driver_hover(env, ctx->handle);
            ctx->retval = 1;
            break;
        case AETHER_DRV_PRESS:
        case AETHER_DRV_RELEASE:
            JV(w->view, M_View_setPressed, ctx->action == AETHER_DRV_PRESS ? JNI_TRUE : JNI_FALSE);
            ctx->retval = 1;
            break;
        default:
            break;
    }
    ctx->result = 0;
    aeui_unframe(env);
}

static void driver_perform_trampoline(void* arg) {
    driver_perform((AetherDriverActionCtx*)arg);
}

static void hook_dispatch_action(AetherDriverActionCtx* ctx) {
    aeui_android_run_sync(driver_perform_trampoline, ctx);
    ctx->done = 1;
}

static void hook_run_on_ui_thread(void (*fn)(void*), void* arg) {
    aeui_android_run_sync(fn, arg);
}

static const AetherDriverHooks android_driver_hooks = {
    .widget_count         = hook_widget_count,
    .widget_type          = hook_widget_type,
    .widget_text_into     = hook_widget_text_into,
    .widget_visible       = hook_widget_visible,
    .widget_hovered       = hook_widget_hovered,
    .widget_pressed       = hook_widget_pressed,
    .widget_parent        = hook_widget_parent,
    .toggle_active        = hook_toggle_active,
    .slider_value         = hook_slider_value,
    .progressbar_fraction = hook_progressbar_fraction,
    .dispatch_action      = hook_dispatch_action,
    .widget_children      = hook_widget_children,
    .widget_enabled       = hook_widget_enabled,
    .widget_rect          = hook_widget_rect,
    .widget_classes_into  = NULL,
    .focused_widget       = hook_focused_widget,
    .widget_a11y          = hook_widget_a11y,
    .screenshot_png       = hook_screenshot_png,
    .canvas_debug         = NULL,
    .canvas_paint_counters = NULL,
    .run_on_ui_thread     = hook_run_on_ui_thread,
};

// The "Under Remote Control" banner, first in the window's body: a REAL
// registered widget, as on AppKit, so it is in /widgets with "banner":true
// and it moves everything below it (which is why specs read geometry from
// the driver instead of hardcoding it).
static void aeui_inject_banner(int root_handle) {
    AeuiWidget* root = live_widget(root_handle);
    if (!root || !aeui_is_linear(root->type) || aether_ui_test_server_banner_handle()) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    jobject tv = g_activity ? JNEW(M_TV_init, g_activity) : NULL;
    if (tv) {
        set_text_on(env, tv, "Under Remote Control");
        int bh = register_widget_typed(env, tv, AUI_BANNER);
        AeuiWidget* b = widget_at(bh);
        if (b) {
            b->fixed_h = 24;
            b->own_hexp = b->hexp = 1;
            b->bold = 1;
            b->anchor = 1;
            JV(tv, M_TV_setTextColor, (jint)0xFFFFFFFF);
            JV(tv, M_TV_setTextSize, (jint)1, (jfloat)12.0f);
            aeui_apply_typeface(env, b);
            aeui_apply_anchor(env, b);
            (*env)->CallVoidMethod(env, tv, J.View_setBackgroundColor, (jint)aeui_argb(0.8, 0.2, 0.2, 1.0));
            aeui_check(env, "banner background");
            aeui_attach(env, root_handle, bh, 0);
            aether_ui_test_server_set_banner(bh);
        }
    }
    aeui_unframe(env);
}

static int android_test_server_started = 0;

void aether_ui_enable_test_server_impl(int port, int root_handle) {
    if (android_test_server_started) return;   // idempotent (env + explicit call)
    android_test_server_started = 1;
    AEUI_LOGI("AetherUIDriver: starting on 127.0.0.1:%d (adb forward tcp:%d tcp:%d)",
              port, port, port);
    aeui_inject_banner(root_handle ? root_handle : g_root_handle);
    aether_ui_test_server_start(port, &android_driver_hooks);
}

void aether_ui_enable_test_server_ctx(int port, void* ctx) {
    aether_ui_enable_test_server_impl(port, (int)(intptr_t)ctx);
}

// ===========================================================================
// The Java side's natives (registered in JNI_OnLoad)
// ===========================================================================

// The app's entry point. `ae build --emit=lib` drops an Aether program's
// main(), so tools/android-apk.sh compiles the app with main() renamed to
// aeui_app_main(); --emit=lib exports it as aether_aeui_app_main. Weak, so a
// library built without it still loads and says why it shows nothing.
extern void aether_aeui_app_main(void) __attribute__((weak));

JCLASS(C_Context, "android/content/Context");
JCLASS(C_File, "java/io/File");
JMETHOD(M_Ctx_getFilesDir, C_Context, "getFilesDir", "()Ljava/io/File;");
JMETHOD(M_Ctx_getAssets, C_Context, "getAssets", "()Landroid/content/res/AssetManager;");
JMETHOD(M_File_getAbsolutePath, C_File, "getAbsolutePath", "()Ljava/lang/String;");

static void aeui_mkdirs(char* path) {   // mkdir -p of path's directories
    for (char* p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(path, 0700);
        *p = '/';
    }
}

// The app's own files. A desktop app opens paths relative to where it runs
// ("examples/imagefill_demo/swatch.png"); an Android process runs in "/".
// tools/android-apk.sh packs the files beside the app's source into the APK
// as assets, with a manifest (aeui-files.txt) since the NDK cannot list an
// asset tree; they are copied out to <files>/app once per install, and that
// directory becomes the working directory, so the same relative paths open.
static void aeui_install_files(JNIEnv* env, jobject activity) {
    if ((*env)->PushLocalFrame(env, 16) != 0) return;
    char root[512] = "";
    jobject dir = JO(activity, M_Ctx_getFilesDir);
    jobject path = dir ? JO(dir, M_File_getAbsolutePath) : NULL;
    if (path) aeui_charseq_into(env, path, root, (int)sizeof(root) - 8);
    if (root[0]) {
        strcat(root, "/app");
        mkdir(root, 0700);
        jobject jam = JO(activity, M_Ctx_getAssets);
        AAssetManager* am = jam ? AAssetManager_fromJava(env, jam) : NULL;
        AAsset* list = am ? AAssetManager_open(am, "aeui-files.txt", AASSET_MODE_BUFFER) : NULL;
        if (list) {
            off_t n = AAsset_getLength(list);
            char* names = (char*)malloc((size_t)n + 1);
            if (names && AAsset_read(list, names, (size_t)n) == (int)n) {
                names[n] = '\0';
                int copied = 0;
                for (char* line = strtok(names, "\n"); line; line = strtok(NULL, "\n")) {
                    if (!*line || strstr(line, "..")) continue;
                    char dest[1024];
                    snprintf(dest, sizeof(dest), "%s/%s", root, line);
                    AAsset* a = AAssetManager_open(am, line, AASSET_MODE_STREAMING);
                    if (!a) continue;
                    struct stat st;
                    off_t len = AAsset_getLength(a);
                    if (stat(dest, &st) != 0 || st.st_size != len) {
                        aeui_mkdirs(dest);
                        FILE* f = fopen(dest, "wb");
                        if (f) {
                            char buf[16384];
                            int r;
                            while ((r = AAsset_read(a, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)r, f);
                            fclose(f);
                            copied++;
                        }
                    }
                    AAsset_close(a);
                }
                if (copied) AEUI_LOGI("installed %d app file(s) under %s", copied, root);
            }
            free(names);
            AAsset_close(list);
        }
        if (chdir(root) != 0) AEUI_LOGW("chdir %s: %s", root, strerror(errno));
    }
    (*env)->PopLocalFrame(env, NULL);
}

static void JNICALL native_start(JNIEnv* env, jclass cls, jobject activity,
                                 jobject host, jfloat density) {
    (void)cls;
    if (g_activity) (*env)->DeleteGlobalRef(env, g_activity);
    if (g_host) (*env)->DeleteGlobalRef(env, g_host);
    g_activity = (*env)->NewGlobalRef(env, activity);
    g_host = (*env)->NewGlobalRef(env, host);
    g_density = density > 0 ? density : 1.0f;
    if (g_started) {
        // The system recreated the activity in a live process: the program
        // already ran, so put its body in the new host instead of running
        // main() twice.
        aeui_mount_body(env);
        return;
    }
    g_started = 1;
    aeui_redirect_stdio();
    if (!aeui_bridge_install()) return;
    aeui_install_files(env, activity);
    if (!aether_aeui_app_main) {
        AEUI_LOGE("no aether_aeui_app_main in libapp.so: build it with tools/android-apk.sh");
        return;
    }
    AEUI_LOGI("running the app's main() (density %.2f)", (double)g_density);
    aether_aeui_app_main();
    AEUI_LOGI("main() returned; %d widgets, body %s", widget_count,
              g_mounted ? "mounted" : "NOT mounted");
}

static void JNICALL native_lifecycle(JNIEnv* env, jclass cls, jint event, jboolean finishing) {
    (void)cls;
    switch (event) {
        case 1: AEUI_LOGI("lifecycle: pause"); break;
        case 2: AEUI_LOGI("lifecycle: resume"); break;
        case 3: AEUI_LOGI("lifecycle: configuration change"); break;
        case 4:
            AEUI_LOGI("lifecycle: destroy (finishing=%d)", finishing ? 1 : 0);
            if (finishing) {
                // The window closed: on the desktop that returns from the
                // run loop and the program ends. Retire every View and end
                // the process the same way, so the next launch is a fresh
                // run of main() rather than a half-alive one.
                aeui_retire_all(env);
                fflush(stdout);
                fflush(stderr);
                exit(0);
            }
            g_mounted = 0;   // the host is going; a recreated activity re-mounts
            break;
        default: break;
    }
}

// on_layout's report, after the layout pass that produced it.
typedef struct { int handle, w, h; } AeuiLayoutFire;

static void aeui_layout_fire(void* arg) {
    AeuiLayoutFire* f = (AeuiLayoutFire*)arg;
    AeuiWidget* w = live_widget(f->handle);
    AeClosure* c = w ? w->layout_cb : NULL;
    if (c && c->fn) ((void (*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)f->w, (intptr_t)f->h);
    free(f);
}

// A radio group: one member on. A member switched on turns the others off
// (each reports its change, as GTK's group toggles do); the last member on
// cannot be switched off by tapping it, as a radio button cannot.
static int aeui_radio_settle(JNIEnv* env, int handle, int on) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !w->radio_leader) return 0;
    int leader = w->radio_leader;
    if (on) {
        for (int i = 0; i < widget_count; i++) {
            AeuiWidget* o = &widgets[i];
            if (i + 1 == handle || !o->view || o->radio_leader != leader) continue;
            if (JZ(o->view, M_CPB_isChecked)) JV(o->view, M_CPB_setChecked, JNI_FALSE);
        }
        return 0;
    }
    for (int i = 0; i < widget_count; i++) {
        AeuiWidget* o = &widgets[i];
        if (i + 1 == handle || !o->view || o->radio_leader != leader) continue;
        if (JZ(o->view, M_CPB_isChecked)) return 0;   // another member holds it
    }
    g_programmatic++;
    JV(w->view, M_CPB_setChecked, JNI_TRUE);
    g_programmatic--;
    return 1;   // swallowed: nothing changed
}

// Every View event, from AetherListener: what it means is decided here.
static void JNICALL native_event(JNIEnv* env, jclass cls, jint handle, jint kind,
                                 jint a, jint b, jstring s) {
    (void)cls;
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    switch (kind) {
        case AEUI_EV_CLICK: {
            int n = w->nclicks;   // a closure may add more; run the ones there were
            for (int i = 0; i < n; i++) {
                AeuiWidget* cw = live_widget(handle);
                if (!cw) break;
                aeui_call0(cw->clicks[i]);
            }
            break;
        }
        case AEUI_EV_TEXT: {
            char* text = aeui_charseq_dup(env, s);
            if (w->bound_state && !g_seeding) aether_ui_state_set_s(w->bound_state, text);
            w = live_widget(handle);
            if (w && !g_programmatic && w->change && w->change->fn)
                ((void (*)(void*, const char*))w->change->fn)(w->change->env, text);
            free(text);
            break;
        }
        case AEUI_EV_CHECK:
            if (aeui_radio_settle(env, handle, a)) break;
            w = live_widget(handle);
            if (w && !g_programmatic && w->change && w->change->fn)
                ((void (*)(void*, intptr_t))w->change->fn)(w->change->env, (intptr_t)(a ? 1 : 0));
            break;
        case AEUI_EV_SEEK:
            if (w->change && w->change->fn)
                ((void (*)(void*, double))w->change->fn)(w->change->env, slider_value_at(w, a));
            break;
        case AEUI_EV_SELECT:
            // The Spinner reports every selection, including the one a
            // programmatic set (already fired) or its first layout makes.
            if (a == w->selected || a < 0 || a >= w->nitems) break;
            w->selected = a;
            aeui_fire_picker(w, a);
            break;
        case AEUI_EV_HOVER: {
            int n = w->nhovers;
            for (int i = 0; i < n; i++) {
                AeuiWidget* hw = live_widget(handle);
                AeClosure* c = hw ? hw->hovers[i] : NULL;
                if (c && c->fn) ((void (*)(void*, intptr_t))c->fn)(c->env, (intptr_t)a);
            }
            break;
        }
        case AEUI_EV_LAYOUT: {
            int wd = aeui_px_to_dp(a), ht = aeui_px_to_dp(b);
            if (!w->layout_cb || (wd == w->layout_w && ht == w->layout_h)) break;
            w->layout_w = wd;
            w->layout_h = ht;
            AeuiLayoutFire* f = (AeuiLayoutFire*)malloc(sizeof(AeuiLayoutFire));
            if (f) { f->handle = handle; f->w = wd; f->h = ht; aeui_android_post(aeui_layout_fire, f); }
            break;
        }
        case AEUI_EV_DOUBLE:
            aeui_call0(w->dbl);
            break;
        default:
            break;
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_vm = vm;
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    J.View               = aeui_find_class(env, "android/view/View");
    J.ViewGroup          = aeui_find_class(env, "android/view/ViewGroup");
    J.LinearLayout       = aeui_find_class(env, "android/widget/LinearLayout");
    J.LinearLayoutParams = aeui_find_class(env, "android/widget/LinearLayout$LayoutParams");
    J.FrameLayoutParams  = aeui_find_class(env, "android/widget/FrameLayout$LayoutParams");
    J.TextView           = aeui_find_class(env, "android/widget/TextView");
    J.Button             = aeui_find_class(env, "android/widget/Button");
    J.Activity           = aeui_find_class(env, "android/app/Activity");
    J.String             = aeui_find_class(env, "java/lang/String");
    J.CharSequence       = aeui_find_class(env, "java/lang/CharSequence");
    // The shim's own classes: only this thread, in JNI_OnLoad, is sure to
    // see the app's class loader.
    J.Listener           = aeui_find_class(env, "dev/aether/ui/AetherListener");
    J.A11y               = aeui_find_class(env, "dev/aether/ui/AetherA11y");
    jclass shim          = aeui_find_class(env, "dev/aether/ui/AetherActivity");
    jclass margin        = aeui_find_class(env, "android/view/ViewGroup$MarginLayoutParams");
    jclass object        = aeui_find_class(env, "java/lang/Object");
    if (!J.View || !J.ViewGroup || !J.LinearLayout || !J.LinearLayoutParams ||
        !J.FrameLayoutParams || !J.TextView || !J.Button || !J.Activity ||
        !J.String || !J.CharSequence || !J.Listener || !J.A11y || !shim || !margin || !object)
        return JNI_ERR;
    C_A11y.cls = J.A11y;

#define M(cls, name, sig) (*env)->GetMethodID(env, cls, name, sig)
    J.View_init                = M(J.View, "<init>", "(Landroid/content/Context;)V");
    J.View_setLayoutParams     = M(J.View, "setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V");
    J.View_setPadding          = M(J.View, "setPadding", "(IIII)V");
    J.View_getVisibility       = M(J.View, "getVisibility", "()I");
    J.View_isEnabled           = M(J.View, "isEnabled", "()Z");
    J.View_performClick        = M(J.View, "performClick", "()Z");
    J.View_setBackgroundColor  = M(J.View, "setBackgroundColor", "(I)V");
    J.View_setOnClickListener  = M(J.View, "setOnClickListener", "(Landroid/view/View$OnClickListener;)V");
    J.View_getLocationInWindow = M(J.View, "getLocationInWindow", "([I)V");
    J.View_getWidth            = M(J.View, "getWidth", "()I");
    J.View_getHeight           = M(J.View, "getHeight", "()I");
    J.View_getParent           = M(J.View, "getParent", "()Landroid/view/ViewParent;");
    J.ViewGroup_addView        = M(J.ViewGroup, "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    J.ViewGroup_removeView     = M(J.ViewGroup, "removeView", "(Landroid/view/View;)V");
    J.LinearLayout_init        = M(J.LinearLayout, "<init>", "(Landroid/content/Context;)V");
    J.LinearLayout_setOrientation = M(J.LinearLayout, "setOrientation", "(I)V");
    J.LinearLayoutParams_init  = M(J.LinearLayoutParams, "<init>", "(IIF)V");
    J.MarginLayoutParams_setMargins = M(margin, "setMargins", "(IIII)V");
    J.FrameLayoutParams_init   = M(J.FrameLayoutParams, "<init>", "(II)V");
    J.TextView_init            = M(J.TextView, "<init>", "(Landroid/content/Context;)V");
    J.TextView_setText         = M(J.TextView, "setText", "(Ljava/lang/CharSequence;)V");
    J.TextView_getText         = M(J.TextView, "getText", "()Ljava/lang/CharSequence;");
    J.TextView_setAllCaps      = M(J.TextView, "setAllCaps", "(Z)V");
    J.Button_init              = M(J.Button, "<init>", "(Landroid/content/Context;)V");
    J.Activity_setTitle        = M(J.Activity, "setTitle", "(Ljava/lang/CharSequence;)V");
    J.String_initBytes         = M(J.String, "<init>", "([BLjava/lang/String;)V");
    J.String_getBytes          = M(J.String, "getBytes", "(Ljava/lang/String;)[B");
    J.Object_toString          = M(object, "toString", "()Ljava/lang/String;");
    J.Listener_init            = M(J.Listener, "<init>", "(II)V");
    J.Listener_initDouble      = M(J.Listener, "<init>", "(Landroid/content/Context;I)V");
    J.A11y_init                = M(J.A11y, "<init>", "()V");
    J.Bitmap                = aeui_find_class(env, "android/graphics/Bitmap");
    J.Canvas                = aeui_find_class(env, "android/graphics/Canvas");
    J.ByteArrayOutputStream = aeui_find_class(env, "java/io/ByteArrayOutputStream");
    jclass window_cls       = aeui_find_class(env, "android/view/Window");
    jclass config_cls       = aeui_find_class(env, "android/graphics/Bitmap$Config");
    jclass format_cls       = aeui_find_class(env, "android/graphics/Bitmap$CompressFormat");
    if (!J.Bitmap || !J.Canvas || !J.ByteArrayOutputStream || !window_cls ||
        !config_cls || !format_cls)
        return JNI_ERR;
    J.Activity_getWindow    = M(J.Activity, "getWindow", "()Landroid/view/Window;");
    J.Window_getDecorView   = M(window_cls, "getDecorView", "()Landroid/view/View;");
    J.View_draw             = M(J.View, "draw", "(Landroid/graphics/Canvas;)V");
    J.Bitmap_createBitmap   = (*env)->GetStaticMethodID(env, J.Bitmap, "createBitmap",
                                  "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    J.Bitmap_compress       = M(J.Bitmap, "compress",
                                  "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z");
    J.Bitmap_recycle        = M(J.Bitmap, "recycle", "()V");
    J.Canvas_init           = M(J.Canvas, "<init>", "(Landroid/graphics/Bitmap;)V");
    J.Canvas_drawColor      = M(J.Canvas, "drawColor", "(I)V");
    J.BAOS_init             = M(J.ByteArrayOutputStream, "<init>", "()V");
    J.BAOS_toByteArray      = M(J.ByteArrayOutputStream, "toByteArray", "()[B");
    {
        jfieldID f_argb = (*env)->GetStaticFieldID(env, config_cls, "ARGB_8888",
                                                   "Landroid/graphics/Bitmap$Config;");
        jfieldID f_png = (*env)->GetStaticFieldID(env, format_cls, "PNG",
                                                  "Landroid/graphics/Bitmap$CompressFormat;");
        if (f_argb && f_png) {
            jobject argb = (*env)->GetStaticObjectField(env, config_cls, f_argb);
            jobject png = (*env)->GetStaticObjectField(env, format_cls, f_png);
            J.ARGB_8888 = (*env)->NewGlobalRef(env, argb);
            J.PNG = (*env)->NewGlobalRef(env, png);
            (*env)->DeleteLocalRef(env, argb);
            (*env)->DeleteLocalRef(env, png);
        }
    }
    (*env)->DeleteGlobalRef(env, window_cls);
    (*env)->DeleteGlobalRef(env, config_cls);
    (*env)->DeleteGlobalRef(env, format_cls);
#undef M
    if (aeui_check(env, "JNI_OnLoad method lookup")) return JNI_ERR;

    static const JNINativeMethod natives[] = {
        { "nativeStart", "(Landroid/app/Activity;Landroid/view/ViewGroup;F)V", (void*)native_start },
        { "nativeLifecycle", "(IZ)V", (void*)native_lifecycle },
        { "nativeEvent", "(IIIILjava/lang/String;)V", (void*)native_event },
    };
    if ((*env)->RegisterNatives(env, shim, natives,
                                (jint)(sizeof(natives) / sizeof(natives[0]))) != JNI_OK) {
        aeui_check(env, "RegisterNatives");
        return JNI_ERR;
    }
    (*env)->DeleteGlobalRef(env, margin);
    (*env)->DeleteGlobalRef(env, object);
    (*env)->DeleteGlobalRef(env, shim);
    return JNI_VERSION_1_6;
}

// ===========================================================================
// STUBS -- every remaining ABI function, so the backend links. Each is
// replaced by a real implementation in a stage-2 pass; the list in STATUS at
// the top is the same set.
// ===========================================================================
void aether_ui_alert_impl(const char* title, const char* message) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_app_quit_impl(void) { aeui_android_unimplemented(__func__); }
void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius, double start_angle, double end_angle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_begin_path_impl(int canvas_id) { aeui_android_unimplemented(__func__); }
void aether_ui_canvas_clear_impl(int canvas_id) { aeui_android_unimplemented(__func__); }
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y, double w, double h) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_close_path_impl(int canvas_id) { aeui_android_unimplemented(__func__); }
int aether_ui_canvas_cmd_count_impl(int canvas_id) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_canvas_create_impl(int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_canvas_draw_image_borrowed_impl(int canvas_id, double x, double y, int iw, int ih, const unsigned char* rgba, int byte_len) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y, int iw, int ih, const unsigned char* rgba, int byte_len) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_draw_image_impl_ptr(int canvas_id, double x, double y, int iw, int ih, const unsigned char* rgba, int byte_len) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_draw_image_scaled_borrowed_impl(int canvas_id, double x, double y, double dw, double dh, int iw, int ih, const unsigned char* rgba, int byte_len) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y, double dw, double dh, int iw, int ih, const unsigned char* rgba, int byte_len) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_draw_image_scaled_impl_ptr(int canvas_id, double x, double y, double dw, double dh, int iw, int ih, const unsigned char* rgba, int byte_len) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a, int even_odd) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id, double x1, double y1, double x2, double y2, int n_stops, void* offsets, void* rgba, double line_width, int extend, int cap, int join) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id, double cx, double cy, double radius, double fx, double fy, int n_stops, void* offsets, void* rgba, double line_width, int extend, int cap, int join, double rx, double ry, double rot_deg) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y, double w, double h, double r, double g, double b, double a) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text, double x, double y, double font_size, int font_flags, const char* font_family, double r, double g, double b, double a) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_gesture_probe_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_canvas_get_widget(int canvas_id) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_canvas_group_begin_impl(int canvas_id) { aeui_android_unimplemented(__func__); }
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_key_release_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_on_scroll_impl(int canvas_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_canvas_painted_pixels_impl(int canvas_id) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py, int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_canvas_redraw_impl(int canvas_id) { aeui_android_unimplemented(__func__); }
int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end, double ox, double oy, int width, int height, unsigned char* out, int out_len) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_canvas_reset_clip_impl(int canvas_id) { aeui_android_unimplemented(__func__); }
void aether_ui_canvas_set_clip_rects_impl(int canvas_id, void* rects, int n) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b, double a, double line_width, int cap, int join) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text, double x, double y, double font_size, double line_width, int font_flags, const char* font_family, double r, double g, double b, double a) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_canvas_write_png_impl(int canvas_id, const char* path, int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
char* aether_ui_clipboard_read_impl(void) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
void aether_ui_clipboard_write_impl(const char* text) { aeui_android_unimplemented(__func__); }
void aether_ui_close_window_by_handle_impl(int win_handle) { aeui_android_unimplemented(__func__); }
void aether_ui_context_menu_item_accel_impl(int handle, const char* label, const char* accel, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_context_menu_item_impl(int handle, const char* label, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_dark_mode_check(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_file_icon_create(const char* path) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_file_icon_set(int handle, const char* path) { aeui_android_unimplemented(__func__); }
char* aether_ui_file_open(const char* title, const char* start_dir) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
char* aether_ui_file_pick_folder(const char* title, const char* start_dir) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
char* aether_ui_file_save(const char* title, const char* default_name) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
int aether_ui_fire_appearance(int dark) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_fire_double_click(int handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_fire_redo(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_fire_row_drop(int row_handle, int src_index) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_fire_scroll(int container_handle, int dy) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_fire_undo(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_gpuview_available_impl(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_gpuview_create_impl(int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_gpuview_get_widget(int gpu_id) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_gpuview_on_realize_impl(int gpu_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_gpuview_on_render_impl(int gpu_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_gpuview_on_resize_impl(int gpu_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_gpuview_read_pixel_impl(int gpu_id, int px, int py) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_gpuview_request_render_impl(int gpu_id) { aeui_android_unimplemented(__func__); }
void aether_ui_menu_add_item(int menu_handle, const char* label, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_menu_add_separator(int menu_handle) { aeui_android_unimplemented(__func__); }
void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_menu_bar_create(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_menu_create(const char* label) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_menu_item_set_label(int menu_handle, const char* old_label, const char* new_label) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_native_list_available_impl(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_native_list_create_impl(int horizontal, int window_rows) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_native_list_first_visible_impl(int handle) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_native_list_scroll_to_impl(int handle, int index) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_native_list_set_count_impl(int handle, int count) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_native_list_set_row_builder_impl(int handle, void* builder) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_native_view_available_impl(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_native_view_create_impl(int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_native_view_get_widget(int view_id) {
    aeui_android_unimplemented(__func__); return 0;
}
void* aether_ui_native_view_handle_impl(int view_id) {
    aeui_android_unimplemented(__func__); return NULL;
}
int aether_ui_native_view_kind_impl(void) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_native_view_on_realize_impl(int view_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_native_view_on_resize_impl(int view_id, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_navstack_create(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_navstack_depth(int handle) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_navstack_pop(int handle) { aeui_android_unimplemented(__func__); }
void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_notify_full_impl(const char* title, const char* body, const char* icon_path, const char* tag, void* boxed_click) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_notify_impl(const char* title, const char* body) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_notify_request_permission_impl(void) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_open_url_impl(const char* url) { aeui_android_unimplemented(__func__); }
void aether_ui_overlay_close_impl(int overlay_handle) { aeui_android_unimplemented(__func__); }
int aether_ui_overlay_count_impl(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_overlay_exit_played_impl(int h) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_overlay_is_exiting_impl(int h) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_overlay_is_live_impl(int h) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_overlay_is_modal_impl(int h) { aeui_android_unimplemented(__func__); return 0; }
const char* aether_ui_overlay_material_effective_impl(int h) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
int aether_ui_overlay_open_impl(int win_handle, int content_handle, int anchor, int dx, int dy, int modal) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_overlay_set_material_impl(int h, const char* kind) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_overlay_set_on_dismiss_impl(int h, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_overlay_set_transition_impl(int h, const char* kind, int ms) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_row_drag_reorder_impl(int row_handle, int index, void* on_drop_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_seal_subtree_impl(int handle) { aeui_android_unimplemented(__func__); }
void aether_ui_seal_widget_impl(int handle) { aeui_android_unimplemented(__func__); }
int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_sheet_dismiss_impl(int handle) { aeui_android_unimplemented(__func__); }
void aether_ui_sheet_present_impl(int handle) { aeui_android_unimplemented(__func__); }
void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_shortcut_chord_impl(const char* first_combo, const char* second_combo, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure, void* enabled_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_split_position_impl(int handle) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_split_set_position_impl(int handle, int px) { aeui_android_unimplemented(__func__); }
int aether_ui_splitview_create(int vertical) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_tab_add(int tabs_handle, const char* title) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_tabs_count(int tabs_handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_tabs_create(void* boxed_closure) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_tabs_select(int tabs_handle, int index) { aeui_android_unimplemented(__func__); }
int aether_ui_tabs_selected(int tabs_handle) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_tray_seal_impl(int tray_id) { aeui_android_unimplemented(__func__); }
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle, const char* icon_clean, const char* icon_busy, const char* icon_alert) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_vg_tooltip_drawn_impl(void) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_vg_tooltip_hide_impl(void) { aeui_android_unimplemented(__func__); }
int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text, double cx, double cy) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_watch_appearance_impl(void) { aeui_android_unimplemented(__func__); }
void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_widget_apply_css_impl(int handle, const char* property_css) {
    aeui_android_unimplemented(__func__);
}
const char* aether_ui_widget_classes_impl(int handle) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
int aether_ui_widget_count_impl(void) { aeui_android_unimplemented(__func__); return 0; }
const char* aether_ui_widget_drag_payload_impl(int handle) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
void aether_ui_widget_draggable_file_impl(int handle, const char* path) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_widget_weight_impl(int handle, int n) { aeui_android_unimplemented(__func__); }
void aether_ui_window_close_impl(int win_handle) { aeui_android_unimplemented(__func__); }
int aether_ui_window_create_impl(const char* title, int width, int height) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_window_file_drop_deliver(const char* paths) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_window_key_deliver(const char* key_name, int mods) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_window_on_file_drop_impl(void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_window_on_key_impl(void* boxed_closure) { aeui_android_unimplemented(__func__); }
void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_window_set_title_impl(int win_handle, const char* title) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_window_show_impl(int win_handle) { aeui_android_unimplemented(__func__); }
int aether_ui_wrap_create(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_zstack_create(void) { aeui_android_unimplemented(__func__); return 0; }
