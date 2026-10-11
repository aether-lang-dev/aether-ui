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
// STATUS: STAGE 2 COMPLETE (pass C, 2026-10-07) -- all 330 ABI functions
// the UIKit backend exports are here: 324 implemented for real and 6
// documented no-ops (the tray: there is no status-area tray on Android, as
// on iOS). There are NO STUBS. Stage 1 proved the chain (`--emit=lib`, JNI,
// the looper bridge, packaging, the driver over `adb forward`) with
// examples/counter; pass A brought the widget set most apps are made of
// (inputs, containers, images, styling, sizing, accessibility, events and
// bindings); pass B everything around the widgets (containers of pages,
// extra windows and sheets, overlays, menus, the keyboard, dialogs and
// pickers, notifications, the clipboard, appearance, CSS classes, native
// lists and views); pass C the canvas (a command buffer replayed through
// android.graphics.Canvas into a retained Bitmap, see the canvas section),
// the GPU view (a SurfaceView with an EGL / OpenGL ES context) and
// fire_double_click.
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
//              frame_clock_start_impl frame_clock_stop_impl
//              frame_clock_name_impl frame_clock_hz_impl (Choreographer)
//
// Real, pass B (104, + the 6 tray no-ops):
//   containers zstack_create wrap_create tabs_create tab_add tabs_select
//              tabs_selected tabs_count tabs_set_on_change navstack_create
//              navstack_push navstack_pop navstack_depth splitview_create
//              split_position_impl split_set_position_impl
//              widget_set_child_impl widget_weight_impl
//   windows    window_create_impl window_set_body_impl window_show_impl
//              window_close_impl close_window_by_handle_impl
//              window_set_title_impl app_quit_impl (and window_count_impl,
//              window_is_open_impl, window_title_impl, widget_window_impl
//              over the extra windows)
//   sheet      sheet_create_impl sheet_set_body_impl sheet_present_impl
//              sheet_dismiss_impl
//   overlay    overlay_open_impl overlay_close_impl overlay_count_impl
//              overlay_is_live_impl overlay_is_modal_impl
//              overlay_is_exiting_impl overlay_exit_played_impl
//              overlay_set_on_dismiss_impl overlay_set_transition_impl
//              overlay_set_material_impl overlay_material_effective_impl
//              toast_impl vg_tooltip_show_impl vg_tooltip_hide_impl
//              vg_tooltip_drawn_impl
//   menus      menu_bar_create menu_create menu_add_item menu_add_separator
//              menu_item_set_label menu_bar_add_menu menu_bar_attach
//              menu_bar_attach_window menu_popup context_menu_item_impl
//              context_menu_item_accel_impl
//   keys       shortcut_impl shortcut_when_impl shortcut_chord_impl
//              window_on_key_impl window_key_deliver
//   system     alert_impl file_open file_save file_pick_folder
//              clipboard_read_impl clipboard_write_impl open_url_impl
//              dark_mode_check watch_appearance_impl fire_appearance
//              notify_impl notify_full_impl notify_request_permission_impl
//              file_icon_create file_icon_set
//   lists      native_list_available_impl native_list_create_impl
//              native_list_set_row_builder_impl native_list_set_count_impl
//              native_list_scroll_to_impl native_list_first_visible_impl
//              vlist_attach_scroll_impl fire_scroll row_drag_reorder_impl
//              fire_row_drop
//   native     native_view_available_impl native_view_kind_impl
//              native_view_create_impl native_view_get_widget
//              native_view_handle_impl native_view_on_realize_impl
//              native_view_on_resize_impl
//   widget     widget_add_css_class_impl widget_remove_css_class_impl
//              widget_classes_impl widget_apply_css_impl widget_count_impl
//              widget_draggable_file_impl widget_drag_payload_impl
//              window_on_file_drop_impl window_file_drop_deliver
//              seal_widget_impl seal_subtree_impl fire_undo fire_redo
//   driver     window resize, key, pick, split position, tab select,
//              context menu, menu activate (side-store and native), and the
//              classes hook
//   tray       tray_create_impl tray_set_menu_impl tray_set_tooltip_impl
//              tray_set_icon_template_impl tray_set_icon_for_state_impl
//              tray_seal_impl -- documented no-ops (see the tray section)
//
// Where Android has no counterpart, the closest faithful behaviour is
// implemented and the comment at the function says so: hover is real only
// under a pointer (mouse, trackpad, hovering stylus; a finger never hovers),
// modifiers_impl is 0 (no pollable modifier state, as on UIKit), the
// disclosure triangle is drawn as a path (no system chevron).
//
// Real, pass C (52):
//   canvas     canvas_create_impl canvas_get_widget canvas_begin_path_impl
//              canvas_move_to_impl canvas_line_to_impl canvas_arc_impl
//              canvas_close_path_impl canvas_stroke_impl canvas_fill_impl
//              canvas_fill_rect_impl canvas_clip_rect_impl canvas_clip_path_impl
//              canvas_set_clip_rects_impl canvas_reset_clip_impl
//              canvas_group_begin_impl canvas_group_end_impl canvas_image_smoothing_impl
//              canvas_fill_text_impl canvas_stroke_text_impl
//              canvas_draw_image_impl canvas_draw_image_impl_ptr
//              canvas_draw_image_scaled_impl canvas_draw_image_scaled_impl_ptr
//              canvas_draw_image_borrowed_impl
//              canvas_draw_image_scaled_borrowed_impl
//              canvas_fill_linear_gradient_impl canvas_fill_radial_gradient_impl
//              canvas_clear_impl canvas_redraw_impl canvas_cmd_count_impl
//              canvas_read_pixel_impl canvas_write_png_impl
//              canvas_render_range_rgba_impl canvas_painted_pixels_impl
//              canvas_on_click_impl canvas_on_move_impl canvas_on_release_impl
//              canvas_on_right_click_impl canvas_on_double_click_impl
//              canvas_on_key_impl canvas_on_key_release_impl
//              canvas_on_scroll_impl canvas_on_resize_impl
//              canvas_gesture_probe_impl                         (41)
//   fields     textfield_on_submit_impl                          (1)
//   sliders    vslider_create                                    (1)
//   gpuview    gpuview_available_impl gpuview_create_impl gpuview_get_widget
//              gpuview_on_realize_impl gpuview_on_render_impl
//              gpuview_on_resize_impl gpuview_request_render_impl
//              gpuview_read_pixel_impl                           (8)
//   fire       fire_double_click                                 (1)
//   driver     the canvas events (click, move, release, key, keyup,
//              scroll), /canvas/{id}/debug and its paint counters
//
// Limitations: Back finishes the activity, which ends the program as
// closing a desktop window does (in an extra window or a sheet, Back closes
// that dialog). The window size an app asks for is not applied (the
// activity is the screen), so a layout written for a wider desktop window
// is narrower here.
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
#include <limits.h>
#include <android/bitmap.h>   // the image decoder below reads a Bitmap's pixels
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
    AUI_FORM_SECTION, AUI_FORM_SECTION_INNER, AUI_BANNER,
    AUI_SCRIM,          // a modal overlay's scrim (registered, as on AppKit)
    AUI_LIST,           // a native list (ListView); reported as a vstack, as AppKit's is
    AUI_NATIVE_VIEW,    // a native view (SurfaceView)
    AUI_GPUVIEW         // a GPU view (SurfaceView + EGL); "widget", as GTK4's GtkGLArea reports
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
    AeClosure* submit;         // textfield/securefield on_submit(text)
    AeClosure** clicks; int nclicks;
    AeClosure** hovers; int nhovers;
    AeClosure* dbl;
    AeClosure* layout_cb; int layout_w, layout_h;
    int bound_state;           // bind_value: the string state this field writes back
    double smin, smax;         // slider range
    double sval; int sval_steps; // the last value set from code, and the step it sits at
    char** items; int nitems; int selected;   // picker
    int radio_leader;          // toggle group
    int fill, tint;            // image: fill mode, tint (-1 none)

    // --- pass B ---
    jobject content;           // global ref: the ViewGroup children go in, when not the view
                               // (a tab set's page host); NULL = the view itself
    char* classes;             // CSS classes, space-separated (NULL none)
    int weight;                // widget_weight: a share of the stack's slack, 0 none
    AeClosure* scroll_cb;      // vlist_attach_scroll: on_scroll(dy)
    AeClosure* row_drop;       // row_drag_reorder: on_drop(src)
    AeClosure* wdrag;          // on_drag(phase, x, y)
    int row_index;             //   ... and this row's index
    char* drag_path;           // widget_draggable_file
    int face;                  // set_child on a non-container: the face widget laid over it
    struct AeuiCtxItem* ctx; int nctx;   // context_menu_item
    int split_pos;             // splitview: pane 1's size (dp), -1 none yet
    int split_vertical;
    int nav_depth;             // navstack: pages pushed above the root
} AeuiWidget;

typedef struct AeuiCtxItem { char* label; char* accel; AeClosure* closure; } AeuiCtxItem;

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
    w->split_pos = -1;
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
    return aeui_is_linear(type) || type == AUI_GRID || type == AUI_SCROLLVIEW ||
           type == AUI_ZSTACK || type == AUI_WRAP || type == AUI_TABS ||
           type == AUI_NAVSTACK || type == AUI_SPLITVIEW;
}

// The ViewGroup a container's children go in: the view itself, or the host
// inside it (a tab set's page frame).
static jobject aeui_content_of(AeuiWidget* w) {
    return w->content ? w->content : w->view;
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
    if (w->content) { (*env)->DeleteGlobalRef(env, w->content); w->content = NULL; }
    free(w->classes); w->classes = NULL;
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
        case AUI_SCRIM:       return "scrim";
        case AUI_LIST:        return "vstack";
        case AUI_NATIVE_VIEW: return "native_view";
        default:              return "widget";
    }
}

// "" for a dead or unknown handle (the ABI's word), so a stylesheet walk
// over every handle skips what is gone.
const char* aether_ui_widget_kind_impl(int handle) {
    if (!view_of(handle)) return "";
    return aeui_kind_name(get_widget_type(handle));
}

int aether_ui_widget_parent_impl(int handle) {
    AeuiWidget* w = widget_at(handle);
    return w ? w->parent : 0;
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
JMETHOD(M_View_getMeasuredWidth, C_View, "getMeasuredWidth", "()I");
JMETHOD(M_View_getMeasuredHeight, C_View, "getMeasuredHeight", "()I");
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
JMETHOD(M_MP_init, C_MarginParams, "<init>", "(II)V");
JCLASS(C_AbsListParams, "android/widget/AbsListView$LayoutParams");
JMETHOD(M_ALP_init, C_AbsListParams, "<init>", "(II)V");
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
static void aeui_apply_window_size(JNIEnv* env);
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
    aeui_apply_window_size(env);   // a size the driver set survives a re-mount
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
        } else if (c->weight > 0) {
            // widget_weight: a share of the slack in proportion to n, on top
            // of a stated size, which is then the floor it cannot go below
            // (weightclamp: a 260-wide weighted label keeps 260 and more).
            main_sz = main_fixed > 0 ? aeui_dp(main_fixed) : 0;
            weight = (float)c->weight;
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
    } else if (p->type == AUI_SPLITVIEW) {
        // Two panes either side of the divider: the first is the split
        // position long (or an even share until one is set), the second
        // takes the rest; across, both fill.
        int vertical = p->split_vertical;
        int first = 1;
        for (int i = 0; i < widget_count; i++)
            if (widgets[i].parent == c->parent && widgets[i].view && i + 1 < handle) { first = 0; break; }
        int main_sz = 0;
        float weight = 1.0f;
        if (first && p->split_pos >= 0) { main_sz = aeui_dp(p->split_pos); weight = 0.0f; }
        lp = JNEW(M_LLP_init, (jint)(vertical ? LP_MATCH_PARENT : main_sz),
                  (jint)(vertical ? main_sz : LP_MATCH_PARENT), (jfloat)weight);
        if (!lp) return;
        JV(lp, M_MP_setMargins, ml, mt, mr, mb);
    } else if (p->type == AUI_ZSTACK || p->type == AUI_TABS || p->type == AUI_NAVSTACK) {
        // Layered: every child fills the frame (a zstack's layers, a tab
        // set's pages, a navigation stack's pages), unless it stated a size.
        lp = JNEW(M_FP_init, (jint)(c->fixed_w > 0 ? aeui_dp(c->fixed_w) : LP_MATCH_PARENT),
                  (jint)(c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_MATCH_PARENT));
        if (!lp) return;
        JV(lp, M_MP_setMargins, ml, mt, mr, mb);
    } else if (p->type == AUI_WRAP) {
        lp = JNEW(M_MP_init, (jint)(c->fixed_w > 0 ? aeui_dp(c->fixed_w) : LP_WRAP_CONTENT),
                  (jint)(c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_WRAP_CONTENT));
        if (!lp) return;
        JV(lp, M_MP_setMargins, ml, mt, mr, mb);
    } else if (p->type == AUI_LIST) {
        // A ListView's rows carry ITS params (it casts to them).
        lp = JNEW(M_ALP_init, (jint)LP_MATCH_PARENT,
                  (jint)(c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_WRAP_CONTENT));
        if (!lp) return;
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
    jobject host = aeui_content_of(p);
    int k = 0;
    if (!(*env)->IsInstanceOf(env, host, jcls(env, &C_ViewGroup))) {
        // A leaf with a face laid over it (set_child on a button): its one
        // registered child lives in the overlay, not inside it.
        for (int j = 0; j < widget_count && k < max; j++)
            if (widgets[j].parent == parent && widgets[j].view) out[k++] = j + 1;
        return k;
    }
    int n = JI(host, M_VG_getChildCount);
    for (int i = 0; i < n && k < max; i++) {
        jobject v = JO(host, M_VG_getChildAt, (jint)i);
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
static void aeui_mount_face(JNIEnv* env, int handle);
static int aeui_opacity_transition_ms(int handle);
static jobject aeui_listener3(JNIEnv* env, int handle, int kind, int arg);
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
    // A split's first pane goes before the divider, the second after it.
    if (p->type == AUI_SPLITVIEW && p->child_count == 0) index = 0;
    p->child_count++;
    // The derived params go on the View first; addView then keeps them
    // (ViewGroup.addView(child, index) uses the child's own LayoutParams).
    aeui_apply_lp(env, child_handle);
    JV(aeui_content_of(p), M_VG_addViewIdx, c->view, (jint)index);
    if (index >= 0) aeui_restack(env, parent_handle);
    if (c->face) aeui_mount_face(env, child_handle);
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
// The shim's AetherScroll and AetherHost (bound in JNI_OnLoad): the scroll
// area that reports its minimum when asked for its natural height, and the
// window frame that grows to its body's natural height rather than letting
// LinearLayout squash what overflows (see AetherHost.java).
static jclass g_scroll_class = NULL, g_host_class = NULL;
static jclass g_vseek_class = NULL;   // AetherVSeekBar (ui.vslider)
static jmethodID g_vseek_init = NULL;
static jmethodID g_scroll_init = NULL, g_host_init = NULL;

int aether_ui_scrollview_create(void) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject sv = NULL;
    if (g_activity && g_scroll_init) {
        sv = (*env)->NewObject(env, g_scroll_class, g_scroll_init, g_activity);
        if (aeui_check(env, "new AetherScroll")) sv = NULL;
    } else if (g_activity) {
        sv = JNEW(M_SV_init, g_activity);
    }
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
       AEUI_EV_SELECT = 5, AEUI_EV_HOVER = 6, AEUI_EV_LAYOUT = 7, AEUI_EV_DOUBLE = 8,
       AEUI_EV_TAB = 9, AEUI_EV_MENU = 10, AEUI_EV_CONTEXT = 11, AEUI_EV_SCRIM = 12,
       AEUI_EV_DISMISS = 13, AEUI_EV_DRAG = 14, AEUI_EV_WHEEL = 15, AEUI_EV_MENU_CLOSED = 16,
       AEUI_EV_SURFACE = 17, AEUI_EV_MENU_OPEN = 18, AEUI_EV_ROW_DRAG = 19, AEUI_EV_ROW_DROP = 20,
       AEUI_EV_FILE_DRAG = 21, AEUI_EV_FILE_DROP = 22, AEUI_EV_SUBMIT = 23,
       AEUI_EV_WDRAG = 24 };

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
JMETHOD(M_TV_setOnEditorActionListener, C_TextView, "setOnEditorActionListener", "(Landroid/widget/TextView$OnEditorActionListener;)V");
JMETHOD(M_TV_setImeOptions, C_TextView, "setImeOptions", "(I)V");
JMETHOD(M_TV_onEditorAction, C_TextView, "onEditorAction", "(I)V");
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
    if ((w->type == AUI_BUTTON || w->type == AUI_TOGGLE) && w->text_override) {
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

// on_submit: the field's editor action (the keyboard's Done, or Enter),
// relayed by AetherListener as AEUI_EV_SUBMIT.
#define AEUI_IME_ACTION_DONE 6
void aether_ui_textfield_on_submit_impl(int handle, void* boxed_closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !boxed_closure) return;
    if (w->type != AUI_TEXTFIELD && w->type != AUI_SECUREFIELD) return;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    w->submit = (AeClosure*)boxed_closure;
    jobject l = aeui_listener(env, handle, AEUI_EV_SUBMIT);
    if (l) {
        JV(w->view, M_TV_setImeOptions, (jint)AEUI_IME_ACTION_DONE);
        JV(w->view, M_TV_setOnEditorActionListener, l);
        (*env)->DeleteLocalRef(env, l);
    }
    aeui_unframe(env);
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

// Selection in UTF-16 units: EditText.setSelection / TextView's getters.
JMETHOD(M_ET_setSelection2, C_EditText, "setSelection", "(II)V");
JMETHOD(M_TV_length, C_TextView, "length", "()I");
JMETHOD(M_TV_getSelectionStart, C_TextView, "getSelectionStart", "()I");
JMETHOD(M_TV_getSelectionEnd, C_TextView, "getSelectionEnd", "()I");

void aether_ui_textfield_select_impl(int handle, int start, int end) {
    AeuiWidget* w = live_widget(handle);
    if (!w || (w->type != AUI_TEXTFIELD && w->type != AUI_SECUREFIELD)) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    int n = JI(w->view, M_TV_length);
    if (start < 0 || start > n) start = n;
    if (end < 0 || end > n) end = n;
    JV(w->view, M_ET_setSelection2, (jint)start, (jint)end);
    aeui_unframe(env);
}

int aether_ui_textfield_selection_impl(int handle, int* start, int* end) {
    AeuiWidget* w = live_widget(handle);
    if (!w || (w->type != AUI_TEXTFIELD && w->type != AUI_SECUREFIELD)) return 0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    *start = JI(w->view, M_TV_getSelectionStart);
    *end = JI(w->view, M_TV_getSelectionEnd);
    aeui_unframe(env);
    return 1;
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

static int make_slider(double min_val, double max_val, double initial,
                       void* boxed_closure, int vertical) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    int h = 0;
    jobject sb = NULL;
    if (vertical) {
        // ui.vslider: AetherVSeekBar, a SeekBar a quarter turn round.
        if (g_activity && g_vseek_init) {
            sb = (*env)->NewObject(env, g_vseek_class, g_vseek_init, g_activity);
            if (aeui_check(env, "new AetherVSeekBar")) sb = NULL;
        }
    } else if (g_activity) {
        sb = JNEW(M_SB_init, g_activity);
    }
    if (sb) {
        h = register_widget_typed(env, sb, AUI_SLIDER);
        AeuiWidget* w = widget_at(h);
        if (w) {
            w->smin = min_val;
            w->smax = max_val;
            w->sval = initial;
            w->sval_steps = slider_steps_for(w, initial);
            w->change = (AeClosure*)boxed_closure;
            // A slider takes the row's width, as GTK's scale does (hexpand);
            // a vertical one the column's height.
            if (vertical) w->own_vexp = w->vexp = 1;
            else w->own_hexp = w->hexp = 1;
            JV(sb, M_PB_setMax, (jint)SLIDER_STEPS);
            JV(sb, M_PB_setProgress, (jint)slider_steps_for(w, initial));
        }
        jobject l = aeui_listener(env, h, AEUI_EV_SEEK);
        if (l) JV(sb, M_SB_setOnSeekBarChangeListener, l);
    }
    aeui_unframe(env);
    return h;
}

int aether_ui_slider_create(double min_val, double max_val, double initial,
                            void* boxed_closure) {
    return make_slider(min_val, max_val, initial, boxed_closure, 0);
}
int aether_ui_vslider_create(double min_val, double max_val, double initial,
                             void* boxed_closure) {
    return make_slider(min_val, max_val, initial, boxed_closure, 1);
}

// Programmatic: no on_change (the SeekBar reports it as not from the user,
// and the listener only forwards a person's drag).
void aether_ui_slider_set_value(int handle, double value) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_SLIDER) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    int steps = slider_steps_for(w, value);
    JV(w->view, M_PB_setProgress, (jint)steps);
    // The exact value, for the getter while the thumb stays on its step: a
    // SeekBar holds SLIDER_STEPS positions, so over a 100,000-row range a
    // set of 90,000 read back as 90,002 (the datagrid's scrollbar). Win32
    // keeps the set value the same way.
    w->sval = value;
    w->sval_steps = steps;
    aeui_unframe(env);
}

double aether_ui_slider_get_value(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_SLIDER) return 0.0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0.0;
    int steps = JI(w->view, M_PB_getProgress);
    aeui_unframe(env);
    if (steps == w->sval_steps && w->sval_steps >= 0) return w->sval;
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

// The same decoder to pixels (aether_ui_backend.h): BitmapFactory's Bitmap,
// read through AndroidBitmap_lockPixels (premultiplied RGBA_8888, R G B A in
// memory on every ABI), copied out row by row and unpremultiplied.
unsigned char* aether_ui_image_decode_rgba_impl(const unsigned char* data, int length,
                                                int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!data || length <= 0) return NULL;
    JNIEnv* env = aeui_frame(8);
    if (!env) return NULL;
    unsigned char* out = NULL;
    jbyteArray bytes = (*env)->NewByteArray(env, length);
    if (bytes) {
        (*env)->SetByteArrayRegion(env, bytes, 0, length, (const jbyte*)data);
        jobject bmp = JSO(M_BF_decodeByteArray, bytes, (jint)0, (jint)length);
        AndroidBitmapInfo info;
        void* px = NULL;
        if (bmp && AndroidBitmap_getInfo(env, bmp, &info) == ANDROID_BITMAP_RESULT_SUCCESS &&
            info.format == ANDROID_BITMAP_FORMAT_RGBA_8888 &&
            info.width > 0 && info.height > 0 &&
            (size_t)info.width * (size_t)info.height <= (size_t)(INT_MAX / 4) &&
            AndroidBitmap_lockPixels(env, bmp, &px) == ANDROID_BITMAP_RESULT_SUCCESS && px) {
            size_t w = info.width, h = info.height;
            out = (unsigned char*)malloc(w * h * 4);
            if (out) {
                for (size_t y = 0; y < h; y++)
                    memcpy(out + y * w * 4, (const unsigned char*)px + y * info.stride, w * 4);
                aether_ui_rgba_unpremultiply(out, w * h);
                if (out_w) *out_w = (int)w;
                if (out_h) *out_h = (int)h;
            }
            AndroidBitmap_unlockPixels(env, bmp);
        }
    }
    aeui_unframe(env);
    return out;
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
JMETHOD(M_View_requestFocusFromTouch, C_View, "requestFocusFromTouch", "()Z");
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

JMETHOD(M_View_animate, C_View, "animate", "()Landroid/view/ViewPropertyAnimator;");
JCLASS(C_VPA, "android/view/ViewPropertyAnimator");
JMETHOD(M_VPA_alpha, C_VPA, "alpha", "(F)Landroid/view/ViewPropertyAnimator;");
JMETHOD(M_VPA_translationY, C_VPA, "translationY", "(F)Landroid/view/ViewPropertyAnimator;");
JMETHOD(M_VPA_scaleX, C_VPA, "scaleX", "(F)Landroid/view/ViewPropertyAnimator;");
JMETHOD(M_VPA_scaleY, C_VPA, "scaleY", "(F)Landroid/view/ViewPropertyAnimator;");
JMETHOD(M_VPA_setDuration, C_VPA, "setDuration", "(J)Landroid/view/ViewPropertyAnimator;");
JMETHOD(M_VPA_start, C_VPA, "start", "()V");

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
    // Width 0 is a border explicitly CLEARED, recorded as such (a re-theme
    // removing one), not as never-set: the flag bit keeps it >= 0.
    w->styled_border = (((int)(width > 0 ? width : 0) & 0x3F) << 24) | 0x40000000 | aeui_rgb(r, g, b);
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
    int ms = aeui_opacity_transition_ms(handle);
    jobject anim = ms > 0 ? JO(w->view, M_View_animate) : NULL;
    if (anim) {
        // A transition declared through apply_css ("transition: opacity
        // 300ms ...", ui.transition's carrier): tween to it.
        JO(anim, M_VPA_setDuration, (jlong)ms);
        JO(anim, M_VPA_alpha, (jfloat)opacity);
        JV(anim, M_VPA_start);
    } else {
        JV(w->view, M_View_setAlpha, (jfloat)opacity);
    }
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
    if (!w) return;
    // Recorded on any widget (a stylesheet's container rule colours the
    // container itself; the readback says it was applied), painted where
    // there is text to paint.
    w->styled_fg = aeui_rgb(r, g, b);
    if (!aeui_is_textview(w->type)) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    JV(w->view, M_TV_setTextColor, (jint)aeui_argb(r, g, b, 1.0));
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
    // A widget not focusable by touch (a button, a checkbox) is still
    // focusable by keyboard, and focus() is the keyboard's kind: it takes
    // focus the way a key press would, leaving touch mode (as GTK4 grabs
    // focus on any focusable widget).
    if (!JZ(w->view, M_View_requestFocus)) JZ(w->view, M_View_requestFocusFromTouch);
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

// on_drag: the view's own touch stream. The listener keeps where the touch
// went down and sends the press point, then offsets from it, packed as two
// signed 16-bit pixel counts (nativeEvent carries two ints); here they become
// dp, the unit width() and the driver's geometry use.
static void aeui_drag_call(AeClosure* c, int phase, double x, double y) {
    if (c && c->fn)
        ((void(*)(void*, intptr_t, double, double))c->fn)(c->env, (intptr_t)phase, x, y);
}

void aether_ui_on_drag_impl(int handle, void* boxed_closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !boxed_closure) return;
    int armed = w->wdrag != NULL;
    w->wdrag = (AeClosure*)boxed_closure;
    if (armed) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    jobject l = aeui_listener(env, handle, AEUI_EV_WDRAG);
    if (l) JV(w->view, M_View_setOnTouchListener, l);
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

// The driver's /widget/{id}/double_click and a programmatic double-click:
// the closure a double tap runs, on the UI thread. 1 if there was one.
static void aeui_fire_double_run(void* arg) {
    int* io = (int*)arg;
    AeuiWidget* w = live_widget(io[0]);
    if (!w || !w->dbl || !w->dbl->fn) return;
    aeui_call0(w->dbl);
    io[1] = 1;
}
int aether_ui_fire_double_click(int handle) {
    int io[2] = { handle, 0 };
    aeui_android_run_sync(aeui_fire_double_run, io);
    return io[1];
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

// The frame clock (aether_ui_backend.h, "One-shot timer and the frame
// clock"): Choreographer, through the NDK's AChoreographer -- the same
// android.view.Choreographer instance the UI thread's Views animate on,
// reached without a Java hop. A frame callback is one-shot, so each frame
// posts the next while the clock runs. frameTimeNanos is CLOCK_MONOTONIC
// (System.nanoTime), the vsync the frame is for. A callback cannot be
// unposted: stop bumps a generation, and a callback from an older one
// neither dispatches nor reposts. Android stops vsync for a stopped
// activity; the shared fallback drives frames from a timer meanwhile.
#include <android/choreographer.h>

static int aeui_frame_running = 0;
static intptr_t aeui_frame_gen = 0;

static double aeui_mono_ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void aeui_frame_cb(int64_t frame_time_nanos, void* data) {
    if (!aeui_frame_running || (intptr_t)data != aeui_frame_gen) return;
    // Post the next one first: a subscriber that stops the clock in this
    // frame bumps the generation, which makes that post a no-op.
    AChoreographer* ch = AChoreographer_getInstance();
    if (ch) AChoreographer_postFrameCallback64(ch, aeui_frame_cb, (void*)aeui_frame_gen);
    aether_ui_frame_dispatch((double)frame_time_nanos / 1e6, aeui_mono_ms_now());
}

int aether_ui_frame_clock_start_impl(void) {
    if (aeui_frame_running) return 1;
    if (!aeui_on_ui_thread()) return 0;   // AChoreographer is per looper thread
    AChoreographer* ch = AChoreographer_getInstance();
    if (!ch) return 0;
    aeui_frame_gen++;
    aeui_frame_running = 1;
    AChoreographer_postFrameCallback64(ch, aeui_frame_cb, (void*)aeui_frame_gen);
    return 1;
}

void aether_ui_frame_clock_stop_impl(void) {
    if (!aeui_frame_running) return;
    aeui_frame_running = 0;
    aeui_frame_gen++;
}

const char* aether_ui_frame_clock_name_impl(void) { return "choreographer"; }

// The display's refresh rate, from the Java side (Display.getRefreshRate is
// not in the NDK before API 30's AChoreographer refresh callbacks). Read once:
// it is only the fallback timer's rate.
static int aeui_display_refresh_hz(void) {
    static int cached = 0;
    if (cached) return cached;
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    float rate = 0.0f;
    if (g_activity) {
        jclass ac = (*env)->GetObjectClass(env, g_activity);
        jmethodID gwm = (*env)->GetMethodID(env, ac, "getWindowManager", "()Landroid/view/WindowManager;");
        jobject wm = gwm ? (*env)->CallObjectMethod(env, g_activity, gwm) : NULL;
        aeui_check(env, "getWindowManager");
        jclass wmc = wm ? (*env)->GetObjectClass(env, wm) : NULL;
        jmethodID gdd = wmc ? (*env)->GetMethodID(env, wmc, "getDefaultDisplay", "()Landroid/view/Display;") : NULL;
        jobject d = gdd ? (*env)->CallObjectMethod(env, wm, gdd) : NULL;
        aeui_check(env, "getDefaultDisplay");
        jclass dc = d ? (*env)->GetObjectClass(env, d) : NULL;
        jmethodID grr = dc ? (*env)->GetMethodID(env, dc, "getRefreshRate", "()F") : NULL;
        if (grr) rate = (*env)->CallFloatMethod(env, d, grr);
        aeui_check(env, "getRefreshRate");
    }
    aeui_unframe(env);
    cached = rate >= 1.0f ? (int)(rate + 0.5f) : 60;
    return cached;
}

int aether_ui_frame_clock_hz_impl(void) {
    int hz = aeui_display_refresh_hz();
    return hz >= 1 ? hz : 60;
}


// ===========================================================================
// STAGE 2, PASS B -- containers, windows, overlays, menus, keys, system.
// Everything an app does around its widgets: the containers that hold pages
// (tabs, navstack, splitview, zstack, wrap), extra windows and sheets,
// in-window overlays, menus and context menus, the keyboard (shortcuts,
// chords, the any-key handler), dialogs, notifications, the clipboard, URLs,
// pickers, appearance, CSS classes, native lists and native views.
//
// Where Android differs from the desktop the ABI was written against, the
// mapping is stated at the function, in the manner of the UIKit backend:
//
//   windows    An activity is the app's one full window (handle 1). An extra
//              window is an android.app.Dialog -- a real platform window of
//              its own, with a title, its own view tree and a close path
//              (Back, or a tap outside) -- which is what a secondary window
//              is on a phone. Sheets are dialogs too. UIKit collapses every
//              window into one; a Dialog keeps each one observable (title,
//              live flag, the widgets in it), which the multi-window specs
//              hold every backend to.
//   menu bar   The action bar's options menu (overflow): each menu is a
//              submenu, each item a MenuItem. A Dialog has no action bar,
//              so a window-2 menu bar is a row of menu titles at the top of
//              the dialog, each opening its menu as a PopupMenu.
//   menus      menu_popup and context menus are PopupMenus anchored to the
//              widget; a context menu opens on a long press, or a
//              secondary click under a mouse (OnContextClickListener).
//   keys       No window-level accelerator table: AetherActivity's
//              dispatchKeyEvent hands every hardware-keyboard key here
//              first, where shortcuts and chords are matched (and consume
//              the key) and the any-key handlers run (and do not).
//              "Primary" is Ctrl, as on GTK4 and Win32: Ctrl+Z/C/V are
//              Android's own keyboard accelerators.
//   pickers    The ABI's file dialogs are synchronous; the Storage Access
//              Framework's are activities that answer later. A desktop modal
//              is a nested event loop, and so is this one
//              (AetherActivity.pick). The answer is a path C can open:
//              /proc/self/fd/N for a document, the content:// URI of a tree
//              for a folder.
//   tray       There is no status-area tray on Android: the tray family is
//              a set of documented no-ops, exactly as on UIKit.
// ===========================================================================

static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

static int aeui_animations_off(void) {
    const char* v = getenv("AETHER_UI_NO_ANIMATION");
    return v && v[0] && v[0] != '0';
}

// The platform's API level (Build.VERSION.SDK_INT).
JCLASS(C_BuildVersion, "android/os/Build$VERSION");
JSFIELD(F_BV_SDK_INT, C_BuildVersion, "SDK_INT", "I");
static int aeui_sdk_int(JNIEnv* env) {
    static int sdk = 0;
    if (!sdk) sdk = JSFI(F_BV_SDK_INT);
    return sdk;
}

// A listener carrying a payload of its own (AetherListener(int, int, int)).
static jmethodID g_listener_init3 = NULL;
static jobject aeui_listener3(JNIEnv* env, int handle, int kind, int arg) {
    if (!g_listener_init3) return NULL;
    jobject l = (*env)->NewObject(env, J.Listener, g_listener_init3, (jint)handle, (jint)kind, (jint)arg);
    if (aeui_check(env, "new AetherListener(3)")) return NULL;
    return l;
}

// Work for later on the UI thread: a one-shot timerfd on the looper (a
// toast's lifetime, an exit tween's end).
typedef struct { void (*fn)(void*); void* arg; } AeuiAfter;
static int aeui_after_cb(int fd, int events, void* data) {
    (void)events;
    AeuiAfter* a = (AeuiAfter*)data;
    uint64_t x;
    if (read(fd, &x, sizeof(x)) < 0) { /* drained either way */ }
    ALooper_removeFd(g_looper, fd);
    a->fn(a->arg);
    close(fd);   // after fn: a timer fn arms cannot be handed this fd number
    free(a);
    return 0;
}
static void aeui_after(int ms, void (*fn)(void*), void* arg) {
    AeuiAfter* a = (AeuiAfter*)malloc(sizeof(AeuiAfter));
    if (!a) return;
    a->fn = fn; a->arg = arg;
    int fd = g_looper ? timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC) : -1;
    if (fd < 0) { fn(arg); free(a); return; }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (ms < 1) ms = 1;
    its.it_value.tv_sec = ms / 1000;
    its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
    timerfd_settime(fd, 0, &its, NULL);
    ALooper_addFd(g_looper, fd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, aeui_after_cb, a);
}

// The registered ancestor at the top of a widget's tree.
static int aeui_top_of(int handle) {
    int h = handle, guard = 0;
    while (h && guard++ < 4096) {
        AeuiWidget* w = widget_at(h);
        if (!w || !w->parent) return h;
        h = w->parent;
    }
    return h;
}

// Take a widget's View out of whatever holds it, registered or not (an
// overlay host, a dialog), then retire it and everything under it.
static void aeui_unhost_and_retire(JNIEnv* env, int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    jobject vp = JO(w->view, M_View_getParent);
    if (vp) {
        if ((*env)->IsInstanceOf(env, vp, jcls(env, &C_ViewGroup))) JV(vp, M_VG_removeView, w->view);
        (*env)->DeleteLocalRef(env, vp);
    }
    if (w->parent) aeui_detach(env, handle);
    aeui_retire_tree(env, handle);
}

JCLASS(C_FrameLayout, "android/widget/FrameLayout");
JMETHOD(M_FL_init, C_FrameLayout, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_FP_init3, C_FrameParams, "<init>", "(III)V");
JMETHOD(M_View_setTranslationX, C_View, "setTranslationX", "(F)V");
JMETHOD(M_View_setTranslationY, C_View, "setTranslationY", "(F)V");
JMETHOD(M_View_setOnLongClickListener, C_View, "setOnLongClickListener", "(Landroid/view/View$OnLongClickListener;)V");
JMETHOD(M_View_setOnContextClickListener, C_View, "setOnContextClickListener", "(Landroid/view/View$OnContextClickListener;)V");
JMETHOD(M_View_setOnGenericMotionListener, C_View, "setOnGenericMotionListener", "(Landroid/view/View$OnGenericMotionListener;)V");
JMETHOD(M_View_setOnDragListener, C_View, "setOnDragListener", "(Landroid/view/View$OnDragListener;)V");
JMETHOD(M_View_getVisibility, C_View, "getVisibility", "()I");
JMETHOD(M_View_setSelected, C_View, "setSelected", "(Z)V");
JMETHOD(M_View_measure, C_View, "measure", "(II)V");
JMETHOD(M_View_layout, C_View, "layout", "(IIII)V");
JMETHOD(M_View_getLeft, C_View, "getLeft", "()I");
JMETHOD(M_View_getTop, C_View, "getTop", "()I");
JMETHOD(M_View_getRight, C_View, "getRight", "()I");
JMETHOD(M_View_getBottom, C_View, "getBottom", "()I");
JMETHOD(M_View_setRenderEffect, C_View, "setRenderEffect", "(Landroid/graphics/RenderEffect;)V");
JMETHOD(M_View_focusSearch, C_View, "focusSearch", "(I)Landroid/view/View;");
JMETHOD(M_VG_getOverlay, C_ViewGroup, "getOverlay", "()Landroid/view/ViewGroupOverlay;");
JCLASS(C_VGOverlay, "android/view/ViewGroupOverlay");
JMETHOD(M_VGO_add, C_VGOverlay, "add", "(Landroid/view/View;)V");

static jobject aeui_frame_layout(JNIEnv* env) {
    return g_activity ? JNEW(M_FL_init, g_activity) : NULL;
}

// ===========================================================================
// Containers: zstack, wrap, tabs, navstack, splitview
// ===========================================================================

// zstack -- a FrameLayout: children layered, each filling it (GtkOverlay's
// children, UIKit's pinned subviews).
int aether_ui_zstack_create(void) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    jobject fl = aeui_frame_layout(env);
    int h = fl ? register_widget_typed(env, fl, AUI_ZSTACK) : 0;
    aeui_unframe(env);
    return h;
}

// wrap -- AetherWrap, a flow layout: children left to right, wrapping when
// the width runs out, 8 dp apart both ways (UIKit's gaps, GtkFlowBox's
// look). The shim's one layout class: android.widget has no flow layout.
static jclass g_wrap_class = NULL;
static jmethodID g_wrap_init = NULL;
int aether_ui_wrap_create(void) {
    JNIEnv* env = aeui_frame(8);
    if (!env || !g_activity || !g_wrap_init) { if (env) aeui_unframe(env); return 0; }
    jobject v = (*env)->NewObject(env, g_wrap_class, g_wrap_init, g_activity,
                                  (jint)aeui_dp(8), (jint)aeui_dp(8));
    int h = 0;
    if (!aeui_check(env, "new AetherWrap") && v) {
        h = register_widget_typed(env, v, AUI_WRAP);
        AeuiWidget* w = widget_at(h);
        if (w) w->own_hexp = w->hexp = 1;   // it flows across the width it is given
    }
    aeui_unframe(env);
    return h;
}

// --- Tabs: a strip of tab buttons over a frame of pages ---------------------
// The tabs widget is a vertical LinearLayout: the strip (a horizontal
// LinearLayout of Buttons, the selected one marked with View.setSelected and
// a rule under it) and the page frame. Each page is a registered vstack in
// the frame; the selected one is VISIBLE, the others GONE, so every page's
// widgets stay registered and findable (GtkStack keeps its pages the same
// way). The strip's buttons are chrome, not widgets, as GtkStackSwitcher's
// and NSTabView's are.
typedef struct {
    int handle;
    jobject strip;           // global ref: the button row
    int* pages; int npages;
    int selected;
    AeClosure* on_change;
} AeuiTabs;
static AeuiTabs* tabsets = NULL;
static int ntabsets = 0;

static AeuiTabs* tabs_of(int handle) {
    for (int i = 0; i < ntabsets; i++) if (tabsets[i].handle == handle) return &tabsets[i];
    return NULL;
}

int aether_ui_tabs_create(void* boxed_closure) {
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return 0; }
    int h = 0;
    jobject root = JNEW(M_LL_init, g_activity);
    jobject strip = JNEW(M_LL_init, g_activity);
    jobject frame = aeui_frame_layout(env);
    AeuiTabs* nt = (AeuiTabs*)realloc(tabsets, sizeof(AeuiTabs) * (size_t)(ntabsets + 1));
    if (root && strip && frame && nt) {
        tabsets = nt;
        JV(root, M_LL_setOrientation, (jint)LL_VERTICAL);
        JV(strip, M_LL_setOrientation, (jint)LL_HORIZONTAL);
        jobject slp = JNEW(M_LLP_init, (jint)LP_MATCH_PARENT, (jint)LP_WRAP_CONTENT, (jfloat)0.0f);
        JV(root, M_VG_addView, strip, slp);
        jobject flp = JNEW(M_LLP_init, (jint)LP_MATCH_PARENT, (jint)LP_WRAP_CONTENT, (jfloat)1.0f);
        if (flp) JV(flp, M_MP_setMargins, 0, aeui_dp(6), 0, 0);
        JV(root, M_VG_addView, frame, flp);
        h = register_widget_typed(env, root, AUI_TABS);
        AeuiWidget* w = widget_at(h);
        if (w) w->content = (*env)->NewGlobalRef(env, frame);
        AeuiTabs* t = &tabsets[ntabsets++];
        memset(t, 0, sizeof(*t));
        t->handle = h;
        t->strip = (*env)->NewGlobalRef(env, strip);
        t->on_change = (AeClosure*)boxed_closure;
    }
    aeui_unframe(env);
    return h;
}

static void aeui_tabs_show(JNIEnv* env, AeuiTabs* t) {
    for (int i = 0; i < t->npages; i++) {
        AeuiWidget* pw = live_widget(t->pages[i]);
        if (pw) JV(pw->view, M_View_setVisibility, (jint)(i == t->selected ? 0 : 8));
        jobject b = JO(t->strip, M_VG_getChildAt, (jint)i);
        if (b) {
            JV(b, M_View_setSelected, i == t->selected ? JNI_TRUE : JNI_FALSE);
            JV(b, M_View_setAlpha, (jfloat)(i == t->selected ? 1.0f : 0.6f));
        }
    }
}

// The page's container comes back, so the tab's block builds into it.
int aether_ui_tab_add(int tabs_handle, const char* title) {
    AeuiTabs* t = tabs_of(tabs_handle);
    if (!t || !live_widget(tabs_handle)) return 0;
    int page = aether_ui_vstack_create(8);
    JNIEnv* env = aeui_frame(16);
    if (!env) return page;
    int* np = (int*)realloc(t->pages, sizeof(int) * (size_t)(t->npages + 1));
    if (np && page) {
        t->pages = np;
        int index = t->npages;
        t->pages[t->npages++] = page;
        jobject b = make_button(env, title);
        if (b) {
            jobject l = aeui_listener3(env, tabs_handle, AEUI_EV_TAB, index);
            if (l) JV(b, M_View_setOnClickListener, l);
            jobject blp = JNEW(M_LLP_init, (jint)LP_WRAP_CONTENT, (jint)LP_WRAP_CONTENT, (jfloat)0.0f);
            JV(t->strip, M_VG_addView, b, blp);
        }
        aeui_attach(env, tabs_handle, page, -1);
        aeui_tabs_show(env, t);
    }
    aeui_unframe(env);
    return page;
}

// Selecting runs on_change with the index, whoever selected (a tap, the app,
// the driver), as every backend's tab switch does.
void aether_ui_tabs_select(int tabs_handle, int index) {
    AeuiTabs* t = tabs_of(tabs_handle);
    if (!t || index < 0 || index >= t->npages) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    t->selected = index;
    aeui_tabs_show(env, t);
    aeui_unframe(env);
    AeClosure* c = t->on_change;
    if (c && c->fn) ((void (*)(void*, intptr_t))c->fn)(c->env, (intptr_t)index);
}
int aether_ui_tabs_selected(int tabs_handle) {
    AeuiTabs* t = tabs_of(tabs_handle);
    return t ? t->selected : -1;
}
int aether_ui_tabs_count(int tabs_handle) {
    AeuiTabs* t = tabs_of(tabs_handle);
    return t ? t->npages : 0;
}
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    AeuiTabs* t = tabs_of(tabs_handle);
    if (t) t->on_change = (AeClosure*)boxed_closure;
}

// --- Navstack: a stack of pages, the top one showing ------------------------
// A FrameLayout holding every pushed page; push hides the page below
// (GONE) and shows the new one, pop retires the top page -- its widgets
// leave the registry, so the driver's census shrinks -- and shows the one
// under it again (the desktop backends hold one page and cannot go back to
// it; a phone's back stack can). Depth counts the pages pushed. A title is a real text widget above the page's body
// (the desktop backends' title bar), so a spec can find it.
int aether_ui_navstack_create(void) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    jobject fl = aeui_frame_layout(env);
    int h = fl ? register_widget_typed(env, fl, AUI_NAVSTACK) : 0;
    aeui_unframe(env);
    return h;
}

static int aeui_nav_pages(JNIEnv* env, int handle, int* out, int max) {
    return aeui_children_in_order(env, handle, out, max);
}

void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    AeuiWidget* nav = live_widget(handle);
    if (!nav || nav->type != AUI_NAVSTACK || !live_widget(body_handle)) return;
    int page = body_handle;
    if (title && title[0]) {
        page = aether_ui_vstack_create(0);
        int bar = aether_ui_text_create(title);
        aether_ui_set_font_bold(bar, 1);
        aether_ui_widget_add_child_ctx((void*)(intptr_t)page, bar);
        aether_ui_widget_add_child_ctx((void*)(intptr_t)page, body_handle);
    }
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    int pages[256];
    int n = aeui_nav_pages(env, handle, pages, 256);
    for (int i = 0; i < n; i++) {
        AeuiWidget* pw = live_widget(pages[i]);
        if (pw) JV(pw->view, M_View_setVisibility, (jint)8);
    }
    aeui_attach(env, handle, page, -1);
    nav = widget_at(handle);
    nav->nav_depth++;
    aeui_unframe(env);
}

void aether_ui_navstack_pop(int handle) {
    AeuiWidget* nav = live_widget(handle);
    if (!nav || nav->type != AUI_NAVSTACK) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    int pages[256];
    int n = aeui_nav_pages(env, handle, pages, 256);
    if (n >= 1 && nav->nav_depth > 0) {   // at the root: nothing to pop
        aeui_detach(env, pages[n - 1]);
        aeui_retire_tree(env, pages[n - 1]);
        AeuiWidget* below = n >= 2 ? live_widget(pages[n - 2]) : NULL;
        if (below) JV(below->view, M_View_setVisibility, (jint)0);
        nav = widget_at(handle);
        nav->nav_depth--;
    }
    aeui_unframe(env);
}

int aether_ui_navstack_depth(int handle) {
    AeuiWidget* nav = live_widget(handle);
    return (nav && nav->type == AUI_NAVSTACK) ? nav->nav_depth : 0;
}

// --- Splitview: two panes and a draggable divider -------------------------
// A LinearLayout [pane 1, divider, pane 2]; vertical=1 stacks the panes (the
// GTK sense). The position is pane 1's size in dp from the start edge; until
// one is set the panes share the space evenly. The divider is a real drag
// handle: a touch (or a mouse) dragging it moves the position, as GtkPaned's
// and NSSplitView's do.
typedef struct { int handle; int start_raw; int start_pos; } AeuiSplitDrag;
static AeuiSplitDrag g_split_drag = { 0, 0, 0 };

int aether_ui_splitview_create(int vertical) {
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return 0; }
    int h = 0;
    jobject ll = JNEW(M_LL_init, g_activity);
    jobject div = (*env)->NewObject(env, J.View, J.View_init, g_activity);
    aeui_check(env, "split divider");
    if (ll && div) {
        JV(ll, M_LL_setOrientation, (jint)(vertical ? LL_VERTICAL : LL_HORIZONTAL));
        h = register_widget_typed(env, ll, AUI_SPLITVIEW);
        AeuiWidget* w = widget_at(h);
        w->split_vertical = vertical ? 1 : 0;
        w->own_hexp = w->hexp = 1;
        (*env)->CallVoidMethod(env, div, J.View_setBackgroundColor, (jint)0x33000000);
        aeui_check(env, "divider colour");
        // 1 dp of line, inside 8 dp of touch target (Material's minimum
        // drag handle reads as the hairline the desktop draws).
        int t = aeui_dp(8);
        jobject dlp = JNEW(M_LLP_init, (jint)(vertical ? LP_MATCH_PARENT : t),
                           (jint)(vertical ? t : LP_MATCH_PARENT), (jfloat)0.0f);
        JV(ll, M_VG_addView, div, dlp);
        jobject l = aeui_listener3(env, h, AEUI_EV_DRAG, vertical ? 1 : 0);
        if (l) JV(div, M_View_setOnTouchListener, l);
    }
    aeui_unframe(env);
    return h;
}

void aether_ui_split_set_position_impl(int handle, int px) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_SPLITVIEW) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    w->split_pos = px < 0 ? 0 : px;
    aeui_relayout_children(env, handle);
    // Settle now, so a position read straight after (the driver's, the
    // app's) sees the layout it asked for.
    JV(w->view, M_View_requestLayout);
    aeui_unframe(env);
}

// The first pane's laid-out size along the split (dp), or the position asked
// for before the first layout; -1 = not a split.
int aether_ui_split_position_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_SPLITVIEW) return -1;
    return w->split_pos >= 0 ? w->split_pos : 0;
}

static void aeui_split_drag(JNIEnv* env, int handle, int action, int raw_px) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    if (action == 0 /* ACTION_DOWN */) {
        int kids[2];
        int n = aeui_children_in_order(env, handle, kids, 2);
        int pos = w->split_pos;
        if (pos < 0 && n > 0) {
            AeuiWidget* first = widget_at(kids[0]);
            int x, y, ww, hh;
            pos = (first && aeui_rect_dp(env, first->view, &x, &y, &ww, &hh) == 0)
                ? (w->split_vertical ? hh : ww) : 0;
        }
        g_split_drag.handle = handle;
        g_split_drag.start_raw = raw_px;
        g_split_drag.start_pos = pos < 0 ? 0 : pos;
    } else if (g_split_drag.handle == handle && (action == 2 /* MOVE */ || action == 1 /* UP */)) {
        int pos = g_split_drag.start_pos + aeui_px_to_dp(raw_px - g_split_drag.start_raw);
        aether_ui_split_set_position_impl(handle, pos < 0 ? 0 : pos);
        if (action == 1) g_split_drag.handle = 0;
    } else if (action == 3 /* CANCEL */) {
        g_split_drag.handle = 0;
    }
}

// --- set_child: one widget inside another -----------------------------------
// Into a container: it becomes the only child (the other registered
// children retire, as UIKit's set_child unregisters them), filling it.
// Onto a leaf -- a Button cannot hold Views -- the child is a FACE laid over
// it: hosted in the parent's own parent's ViewGroupOverlay and kept on the
// leaf's bounds as it lays out (AppKit puts a subview over the button face
// the same way).
static void aeui_place_face(JNIEnv* env, int handle) {
    AeuiWidget* w = live_widget(handle);
    AeuiWidget* f = w ? live_widget(w->face) : NULL;
    if (!f) return;
    int l = JI(w->view, M_View_getLeft), t = JI(w->view, M_View_getTop);
    int r = JI(w->view, M_View_getRight), b = JI(w->view, M_View_getBottom);
    if (r <= l || b <= t) return;
    if (f->type == AUI_CANVAS) {
        // A canvas face draws a scene of its natural size (a chrome face
        // has no on_resize to re-map it), so it sits at that size centred
        // on the widget, the way a GtkButton centres a child smaller than
        // itself, rather than in its top-left corner.
        JV(f->view, M_View_measure, (jint)(0x80000000 | (r - l)), (jint)(0x80000000 | (b - t)));
        int fw = JI(f->view, M_View_getMeasuredWidth), fh = JI(f->view, M_View_getMeasuredHeight);
        int x = l + ((r - l) - fw) / 2, y = t + ((b - t) - fh) / 2;
        JV(f->view, M_View_layout, (jint)x, (jint)y, (jint)(x + fw), (jint)(y + fh));
        return;
    }
    JV(f->view, M_View_measure, (jint)(0x40000000 | (r - l)), (jint)(0x40000000 | (b - t)));
    JV(f->view, M_View_layout, (jint)l, (jint)t, (jint)r, (jint)b);
}

// A face goes in the overlay of the ViewGroup its widget is in. The DSL
// assembles top-down, so a face is often set before its widget has been
// put anywhere (ui.btn makes the chrome face first, then adds the button):
// then it is mounted when the widget is attached (aeui_attach).
static void aeui_mount_face(JNIEnv* env, int handle) {
    AeuiWidget* w = live_widget(handle);
    AeuiWidget* f = w ? live_widget(w->face) : NULL;
    if (!f) return;
    jobject vp = JO(w->view, M_View_getParent);
    if (!vp || !(*env)->IsInstanceOf(env, vp, jcls(env, &C_ViewGroup))) return;
    jobject ov = JO(vp, M_VG_getOverlay);
    if (ov) JV(ov, M_VGO_add, f->view);   // out of any earlier overlay first
    aeui_place_face(env, handle);
}

void aether_ui_widget_set_child_impl(int parent_handle, int child_handle) {
    AeuiWidget* p = live_widget(parent_handle);
    AeuiWidget* c = live_widget(child_handle);
    if (!p || !c || parent_handle == child_handle) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    if ((*env)->IsInstanceOf(env, aeui_content_of(p), jcls(env, &C_ViewGroup)) && aeui_is_container(p->type)) {
        for (int i = 0; i < widget_count; i++) {
            if (widgets[i].parent == parent_handle && widgets[i].view && i + 1 != child_handle) {
                aeui_detach(env, i + 1);
                aeui_retire_tree(env, i + 1);
            }
        }
        c = widget_at(child_handle);
        if (c->parent != parent_handle) aeui_attach(env, parent_handle, child_handle, -1);
        aeui_match_parent(child_handle, 0);
        aeui_match_parent(child_handle, 1);
    } else {
        if (c->parent) aeui_detach(env, child_handle);
        c = widget_at(child_handle);
        c->parent = parent_handle;
        p = widget_at(parent_handle);
        p->face = child_handle;
        // The face IS the widget's look now, as gtk_button_set_child
        // replaces the label: the caption leaves the screen but stays the
        // widget's text for the driver and its description for TalkBack
        // (the disclosure's arrangement).
        if ((p->type == AUI_BUTTON || p->type == AUI_TOGGLE) && !p->text_override) {
            jobject cs = (*env)->CallObjectMethod(env, p->view, J.TextView_getText);
            p->text_override = aeui_check(env, "getText") ? strdup("") : aeui_charseq_dup(env, cs);
            jstring d = aeui_jstring(env, p->text_override);
            if (!p->a11y_name) JV(p->view, M_View_setContentDescription, d);
            set_text_on(env, p->view, "");
        }
        if (!(p->listeners & LST_LAYOUT)) {
            jobject l = aeui_listener(env, parent_handle, AEUI_EV_LAYOUT);
            if (l) { JV(p->view, M_View_addOnLayoutChangeListener, l); p->listeners |= LST_LAYOUT; }
        }
        aeui_mount_face(env, parent_handle);
    }
    aeui_unframe(env);
}

// ===========================================================================
// Windows. Handle 1 is the activity. 2.. are extra windows, each an
// android.app.Dialog whose content is a frame (the overlay host) holding the
// body; the same record serves sheets, which are dialogs as well.
// ===========================================================================
JCLASS(C_Dialog, "android/app/Dialog");
JCLASS(C_Window, "android/view/Window");
JMETHOD(M_Dlg_init, C_Dialog, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_Dlg_setTitle, C_Dialog, "setTitle", "(Ljava/lang/CharSequence;)V");
JMETHOD(M_Dlg_setContentView, C_Dialog, "setContentView", "(Landroid/view/View;)V");
JMETHOD(M_Dlg_show, C_Dialog, "show", "()V");
JMETHOD(M_Dlg_dismiss, C_Dialog, "dismiss", "()V");
JMETHOD(M_Dlg_getWindow, C_Dialog, "getWindow", "()Landroid/view/Window;");
JMETHOD(M_Dlg_setOnDismissListener, C_Dialog, "setOnDismissListener",
        "(Landroid/content/DialogInterface$OnDismissListener;)V");
JMETHOD(M_Win_setLayout, C_Window, "setLayout", "(II)V");
JMETHOD(M_Act_setTitle, C_Activity, "setTitle", "(Ljava/lang/CharSequence;)V");
JMETHOD(M_Act_invalidateOptionsMenu, C_Activity, "invalidateOptionsMenu", "()V");
JMETHOD(M_Act_finishAndRemoveTask, C_Activity, "finishAndRemoveTask", "()V");

enum { WREC_WINDOW = 1, WREC_SHEET = 2 };

typedef struct {
    int kind;            // WREC_WINDOW / WREC_SHEET
    jobject dialog;      // global ref (Dialog), NULL until made
    jobject host;        // global ref: the dialog's content frame
    jobject menubar;     // global ref: a window's menu-bar row, once attached
    int root;            // the body's handle
    char* title;
    int w, h;            // dp asked for
    int live;            // shown and not closed
} AeuiWinRec;
static AeuiWinRec* wrecs = NULL;   // extra windows: id = index + 2
static int nwrecs = 0;
static AeuiWinRec* sheetrecs = NULL;   // sheets: id = index + 1
static int nsheetrecs = 0;

static AeuiWinRec* win_rec(int win_handle) {
    int i = win_handle - 2;
    return (i >= 0 && i < nwrecs) ? &wrecs[i] : NULL;
}

static int g_win_w = 0, g_win_h = 0;   // the primary window's size, once the driver set one

static AeuiWinRec* aeui_wrec_new(AeuiWinRec** arr, int* n, int kind, const char* title, int w, int h) {
    AeuiWinRec* na = (AeuiWinRec*)realloc(*arr, sizeof(AeuiWinRec) * (size_t)(*n + 1));
    if (!na) return NULL;
    *arr = na;
    AeuiWinRec* r = &na[(*n)++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->title = strdup(title ? title : "");
    r->w = w; r->h = h;
    return r;
}

// Make the record's Dialog (once): a titled platform window whose content is
// a FrameLayout, the host its body and its overlays mount in.
static int aeui_wrec_realize(JNIEnv* env, AeuiWinRec* r, int id) {
    if (r->dialog) return 1;
    if (!g_activity) return 0;
    jobject d = JNEW(M_Dlg_init, g_activity);
    jobject host = NULL;
    if (g_host_init) {
        host = (*env)->NewObject(env, g_host_class, g_host_init, g_activity);
        if (aeui_check(env, "new AetherHost")) host = NULL;
    }
    if (!host) host = aeui_frame_layout(env);
    if (!d || !host) return 0;
    jstring t = aeui_jstring(env, r->title);
    JV(d, M_Dlg_setTitle, t);
    JV(d, M_Dlg_setContentView, host);
    jobject l = aeui_listener3(env, id, AEUI_EV_DISMISS, r->kind);
    if (l) JV(d, M_Dlg_setOnDismissListener, l);
    r->dialog = (*env)->NewGlobalRef(env, d);
    r->host = (*env)->NewGlobalRef(env, host);
    return 1;
}

// The body in the host, inset as the activity's body is.
static void aeui_wrec_mount(JNIEnv* env, AeuiWinRec* r) {
    AeuiWidget* b = live_widget(r->root);
    if (!b || !r->host) return;
    jobject vp = JO(b->view, M_View_getParent);
    if (vp) {
        if ((*env)->IsSameObject(env, vp, r->host)) return;
        if ((*env)->IsInstanceOf(env, vp, jcls(env, &C_ViewGroup))) JV(vp, M_VG_removeView, b->view);
    }
    jobject lp = JNEW(M_FP_init, (jint)LP_MATCH_PARENT, (jint)LP_MATCH_PARENT);
    int pad = aeui_dp(16);
    if (lp) JV(lp, M_MP_setMargins, pad, pad, pad, pad);
    JV(r->host, M_VG_addView, b->view, lp);
}

static void aeui_wrec_size(JNIEnv* env, AeuiWinRec* r) {
    if (!r->dialog || r->w <= 0 || r->h <= 0) return;
    jobject win = JO(r->dialog, M_Dlg_getWindow);
    if (win) JV(win, M_Win_setLayout, (jint)aeui_dp(r->w), (jint)aeui_dp(r->h));
}

// Close: the body's widgets leave the registry (the census falls back), and
// the dialog goes. Its own dismiss event then finds the record closed.
static void aeui_wrec_close(JNIEnv* env, AeuiWinRec* r) {
    if (!r->live && !r->root) return;
    int was_live = r->live;
    r->live = 0;
    if (r->root) {
        aeui_unhost_and_retire(env, r->root);
        r->root = 0;
    }
    if (was_live && r->dialog) JV(r->dialog, M_Dlg_dismiss);
}

int aether_ui_window_create_impl(const char* title, int width, int height) {
    AeuiWinRec* r = aeui_wrec_new(&wrecs, &nwrecs, WREC_WINDOW, title, width, height);
    return r ? nwrecs + 1 : 0;
}

void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    if (win_handle <= 1) {
        // The activity's body: what app_set_body sets, mounted now if the
        // app is already running.
        g_root_handle = root_handle;
        JNIEnv* env = aeui_frame(16);
        if (env) { if (g_mounted) aeui_mount_body(env); aeui_unframe(env); }
        return;
    }
    AeuiWinRec* r = win_rec(win_handle);
    if (!r) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    r->root = root_handle;
    if (aeui_wrec_realize(env, r, win_handle)) aeui_wrec_mount(env, r);
    aeui_unframe(env);
}

void aether_ui_window_show_impl(int win_handle) {
    AeuiWinRec* r = win_rec(win_handle);
    if (!r) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    if (aeui_wrec_realize(env, r, win_handle)) {
        aeui_wrec_mount(env, r);
        JV(r->dialog, M_Dlg_show);
        aeui_wrec_size(env, r);
        r->live = 1;
    }
    aeui_unframe(env);
}

// The primary window closing ends the app, as on the desktop; an extra
// window closes alone.
void aether_ui_window_close_impl(int win_handle) {
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    if (win_handle == 1) {
        if (g_activity) JV(g_activity, M_Act_finishAndRemoveTask);
    } else {
        AeuiWinRec* r = win_rec(win_handle);
        if (r) aeui_wrec_close(env, r);
    }
    aeui_unframe(env);
}

void aether_ui_close_window_by_handle_impl(int win_handle) {
    aether_ui_window_close_impl(win_handle);
}

void aether_ui_window_set_title_impl(int win_handle, const char* title) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    jstring t = aeui_jstring(env, title ? title : "");
    if (win_handle <= 1) {
        free(g_title);
        g_title = strdup(title ? title : "");
        if (g_activity) JV(g_activity, M_Act_setTitle, t);
    } else {
        AeuiWinRec* r = win_rec(win_handle);
        if (r) {
            free(r->title);
            r->title = strdup(title ? title : "");
            if (r->dialog) JV(r->dialog, M_Dlg_setTitle, t);
        }
    }
    aeui_unframe(env);
}

// Which window a widget is in: the one whose body its tree hangs from, or
// whose overlay it is; 0 when it is in none (built and not yet placed).
static int aeui_overlay_window_of(int handle);
int aether_ui_widget_window_impl(int widget_handle) {
    if (!view_of(widget_handle)) return 0;
    int top = aeui_top_of(widget_handle);
    if (top == g_root_handle) return 1;
    for (int i = 0; i < nwrecs; i++)
        if (wrecs[i].root && wrecs[i].root == top) return wrecs[i].live ? i + 2 : 0;
    int ow = aeui_overlay_window_of(top);
    if (ow) return ow;
    if (top == aether_ui_test_server_banner_handle()) return 1;
    return 0;
}

// The dialog went (Back, a tap outside, dismiss): a window's widgets retire,
// as a closed desktop window's do; a sheet just stops showing -- the app's
// sheet_dismiss is what retires its body.
static void aeui_dialog_gone(JNIEnv* env, int id, int kind) {
    if (kind == WREC_WINDOW) {
        AeuiWinRec* r = win_rec(id);
        if (r && r->live) aeui_wrec_close(env, r);
    } else {
        int i = id - 1;
        if (i >= 0 && i < nsheetrecs) sheetrecs[i].live = 0;
    }
}

// --- Sheets: a dialog over the window, presented and dismissed --------------
int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    AeuiWinRec* r = aeui_wrec_new(&sheetrecs, &nsheetrecs, WREC_SHEET, title, width, height);
    return r ? nsheetrecs : 0;
}

void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    int i = handle - 1;
    if (i < 0 || i >= nsheetrecs) return;
    sheetrecs[i].root = root_handle;
}

// Headless there is no one to see it: the body is registered (the DSL built
// it) and nothing is shown, as AppKit's sheet does headless.
void aether_ui_sheet_present_impl(int handle) {
    int i = handle - 1;
    if (i < 0 || i >= nsheetrecs || aeui_is_headless()) return;
    AeuiWinRec* r = &sheetrecs[i];
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    if (aeui_wrec_realize(env, r, handle)) {
        aeui_wrec_mount(env, r);
        JV(r->dialog, M_Dlg_show);
        aeui_wrec_size(env, r);
        r->live = 1;
    }
    aeui_unframe(env);
}

// The body retires at once, so the driver never sees a dismissed sheet's
// widgets, then the dialog goes.
void aether_ui_sheet_dismiss_impl(int handle) {
    int i = handle - 1;
    if (i < 0 || i >= nsheetrecs) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    AeuiWinRec* r = &sheetrecs[i];
    int was_live = r->live;
    r->live = 0;
    if (r->root) { aeui_unhost_and_retire(env, r->root); r->root = 0; }
    if (was_live && r->dialog) JV(r->dialog, M_Dlg_dismiss);
    aeui_unframe(env);
}

// --- The window count and its records, for /windows ------------------------
int aether_ui_window_count_impl(void) { return 1 + nwrecs; }
int aether_ui_window_is_open_impl(int win_handle) {
    if (win_handle == 1) return g_mounted ? 1 : 0;
    AeuiWinRec* r = win_rec(win_handle);
    return r ? r->live : 0;
}
const char* aether_ui_window_title_impl(int win_handle) {
    if (win_handle == 1) return g_title ? g_title : "";
    AeuiWinRec* r = win_rec(win_handle);
    return (r && r->title) ? r->title : "";
}

// The primary window's size, as the driver's /window/resize sets it: the
// activity fills the screen, so a "window size" is the size of its content
// -- the body (what a desktop window's content view is, edge to edge) is
// given exactly that size, inside the activity's inset, and the layout runs
// at it, wider than the screen if asked, as an app in a resizable (freeform
// / desktop) window is. Extra windows are sized as dialogs.
static void aeui_apply_window_size(JNIEnv* env) {
    jobject root = view_of(g_root_handle);
    if (!root || g_win_w <= 0 || g_win_h <= 0) return;
    int pad = aeui_dp(16);
    jobject lp = JNEW(M_FP_init, (jint)aeui_dp(g_win_w), (jint)aeui_dp(g_win_h));
    if (!lp) return;
    JV(lp, M_MP_setMargins, pad, pad, pad, pad);
    JV(root, M_View_setLayoutParams, lp);
}

static void aeui_window_resize(JNIEnv* env, int win_handle, int w, int h) {
    if (win_handle <= 1) {
        g_win_w = w; g_win_h = h;
        aeui_apply_window_size(env);
    } else {
        AeuiWinRec* r = win_rec(win_handle);
        if (r) { r->w = w; r->h = h; aeui_wrec_size(env, r); }
    }
}

// ===========================================================================
// Overlays -- toasts, modals, tooltips: drawn INSIDE the window, above its
// body, in the window's host frame (the activity's, or a dialog's). The table
// is append-only, as on every backend: a closed overlay stays listed with
// live 0, so "it expired" and "it never opened" read differently.
// ===========================================================================
JCLASS(C_RenderEffect, "android/graphics/RenderEffect");
JCLASS(C_TileMode, "android/graphics/Shader$TileMode");
JSTATIC(M_RE_blur, C_RenderEffect, "createBlurEffect",
        "(FFLandroid/graphics/Shader$TileMode;)Landroid/graphics/RenderEffect;");
JSFIELD(F_TM_CLAMP, C_TileMode, "CLAMP", "Landroid/graphics/Shader$TileMode;");

typedef struct {
    int window;          // the window it is drawn in
    int content;         // the content widget
    int scrim;           // the scrim widget (modal), 0 none
    AeClosure* on_dismiss;
    int modal, live, exiting, exit_played, trans_ms;
    char* trans_kind;
    char* material;      // effective: "dim" | "blur" | "tint"
} AeuiOverlay;
static AeuiOverlay* overlays = NULL;
static int overlay_count = 0;

static AeuiOverlay* overlay_at(int h) {
    return (h >= 1 && h <= overlay_count) ? &overlays[h - 1] : NULL;
}

static int aeui_overlay_window_of(int handle) {
    for (int i = 0; i < overlay_count; i++)
        if (overlays[i].live && (overlays[i].content == handle || overlays[i].scrim == handle))
            return overlays[i].window;
    return 0;
}

static jobject aeui_window_host(int win_handle) {
    if (win_handle <= 1) return g_host;
    AeuiWinRec* r = win_rec(win_handle);
    return (r && r->live) ? r->host : NULL;
}

// A real backdrop blur where the platform has one: from API 31 a View can
// carry a RenderEffect, so a "blur" scrim blurs the window's body behind it
// (and the dim on top stays faint). Below 31 it degrades to "tint" and
// material_effective says so.
static int aeui_any_blur_live(int window) {
    for (int i = 0; i < overlay_count; i++)
        if (overlays[i].live && overlays[i].window == window && overlays[i].material &&
            strcmp(overlays[i].material, "blur") == 0) return 1;
    return 0;
}
static void aeui_apply_blur(JNIEnv* env, int window) {
    if (aeui_sdk_int(env) < 31) return;
    int root = window <= 1 ? g_root_handle : (win_rec(window) ? win_rec(window)->root : 0);
    AeuiWidget* r = live_widget(root);
    if (!r) return;
    jobject fx = NULL;
    if (aeui_any_blur_live(window)) {
        jobject clamp = JSFO(F_TM_CLAMP);
        float radius = 12.0f * g_density;
        fx = JSO(M_RE_blur, (jfloat)radius, (jfloat)radius, clamp);
    }
    JV(r->view, M_View_setRenderEffect, fx);
}

static unsigned int aeui_scrim_argb(const char* material) {
    if (material && strcmp(material, "blur") == 0) return 0x14000000u;   // a faint veil over the frost
    if (material && strcmp(material, "tint") == 0) return 0x59F2F2FAu;
    return 0x73000000u;                                                  // 45% black
}

int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal) {
    if (win_handle < 1) win_handle = 1;
    AeuiWidget* c = live_widget(content_handle);
    JNIEnv* env = aeui_frame(32);
    if (!env) return 0;
    jobject host = aeui_window_host(win_handle);
    if (!c || !host) { aeui_unframe(env); return 0; }
    AeuiOverlay* no = (AeuiOverlay*)realloc(overlays, sizeof(AeuiOverlay) * (size_t)(overlay_count + 1));
    if (!no) { aeui_unframe(env); return 0; }
    overlays = no;
    int handle = overlay_count + 1;
    AeuiOverlay* e = &overlays[overlay_count++];
    memset(e, 0, sizeof(*e));
    e->window = win_handle;
    e->content = content_handle;
    e->modal = modal ? 1 : 0;
    e->live = 1;
    e->material = strdup("dim");

    // Scrim first, so it is below the content: a full-window View that takes
    // every tap (input blocking is z-order, as on the other backends), and
    // whose tap is the one path that runs on_dismiss.
    if (modal) {
        jobject sv = (*env)->NewObject(env, J.View, J.View_init, g_activity);
        if (!aeui_check(env, "new scrim") && sv) {
            (*env)->CallVoidMethod(env, sv, J.View_setBackgroundColor, (jint)aeui_scrim_argb("dim"));
            aeui_check(env, "scrim colour");
            jobject l = aeui_listener3(env, handle, AEUI_EV_SCRIM, 0);
            if (l) JV(sv, M_View_setOnClickListener, l);
            jobject lp = JNEW(M_FP_init, (jint)LP_MATCH_PARENT, (jint)LP_MATCH_PARENT);
            JV(host, M_VG_addView, sv, lp);
            e = overlay_at(handle);
            e->scrim = register_widget_typed(env, sv, AUI_SCRIM);
        }
    }
    // The content: detached from wherever it was built, placed by anchor
    // (h + v*4: 0 start, 1 centre, 2 end), dx/dy signed insets from the
    // start/top (positive) or end/bottom (negative), an offset when centred.
    c = widget_at(content_handle);
    if (c->parent) aeui_detach(env, content_handle);
    c = widget_at(content_handle);
    jobject vp = JO(c->view, M_View_getParent);
    if (vp && (*env)->IsInstanceOf(env, vp, jcls(env, &C_ViewGroup))) JV(vp, M_VG_removeView, c->view);
    int h = anchor & 3, v = (anchor >> 2) & 3;
    int grav = (h == 0 ? GRAV_START : h == 2 ? GRAV_END : GRAV_CENTER_H) |
               (v == 0 ? GRAV_TOP : v == 2 ? GRAV_BOTTOM : GRAV_CENTER_V);
    jobject lp = JNEW(M_FP_init3, (jint)(c->fixed_w > 0 ? aeui_dp(c->fixed_w) : LP_WRAP_CONTENT),
                      (jint)(c->fixed_h > 0 ? aeui_dp(c->fixed_h) : LP_WRAP_CONTENT), (jint)grav);
    if (lp) {
        int ml = (h == 0 && dx > 0) ? aeui_dp(dx) : 0, mr = (h == 2 && dx < 0) ? aeui_dp(-dx) : 0;
        int mt = (v == 0 && dy > 0) ? aeui_dp(dy) : 0, mb = (v == 2 && dy < 0) ? aeui_dp(-dy) : 0;
        JV(lp, M_MP_setMargins, ml, mt, mr, mb);
        JV(lp, M_MP_setMarginStart, ml);
        JV(lp, M_MP_setMarginEnd, mr);
    }
    JV(c->view, M_View_setTranslationX, (jfloat)(h == 1 ? aeui_dp(dx) : 0));
    JV(c->view, M_View_setTranslationY, (jfloat)(v == 1 ? aeui_dp(dy) : 0));
    JV(c->view, M_View_setAlpha, (jfloat)1.0f);
    JV(host, M_VG_addView, c->view, lp);
    aeui_unframe(env);
    return handle;
}

static void aeui_overlay_finalize(void* arg) {
    int h = (int)(intptr_t)arg;
    AeuiOverlay* e = overlay_at(h);
    if (!e || !e->live) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    e->live = 0;
    e->exiting = 0;
    // UNREGISTERED, not just removed, as every backend's close does: a
    // closed dialog's buttons must not stay clickable through the driver.
    int content = e->content, scrim = e->scrim;
    if (scrim) aeui_unhost_and_retire(env, scrim);
    if (content) aeui_unhost_and_retire(env, content);
    e = overlay_at(h);
    if (e->material && strcmp(e->material, "blur") == 0) aeui_apply_blur(env, e->window);
    aeui_unframe(env);
}

// Instant, unless the overlay has a transition and animation is on: then the
// exit plays (a ViewPropertyAnimator: fade, plus the slide or scale the kind
// names) and the overlay is removed when it ends; is_exiting reads 1 until
// then, exit_played stays 1 for good.
void aether_ui_overlay_close_impl(int overlay_handle) {
    AeuiOverlay* e = overlay_at(overlay_handle);
    if (!e || !e->live || e->exiting) return;
    if (e->trans_ms > 0 && !aeui_animations_off()) {
        JNIEnv* env = aeui_frame(16);
        if (!env) return;
        e->exiting = 1;
        e->exit_played = 1;
        int ids[2] = { e->content, e->scrim };
        for (int k = 0; k < 2; k++) {
            AeuiWidget* w = live_widget(ids[k]);
            if (!w) continue;
            jobject a = JO(w->view, M_View_animate);
            if (!a) continue;
            JO(a, M_VPA_setDuration, (jlong)e->trans_ms);
            JO(a, M_VPA_alpha, (jfloat)0.0f);
            if (k == 0 && e->trans_kind) {
                int hh = JI(w->view, M_View_getHeight);
                if (strcmp(e->trans_kind, "slide-up") == 0) JO(a, M_VPA_translationY, (jfloat)-hh);
                else if (strcmp(e->trans_kind, "slide-down") == 0) JO(a, M_VPA_translationY, (jfloat)hh);
                else if (strcmp(e->trans_kind, "scale") == 0) {
                    JO(a, M_VPA_scaleX, (jfloat)0.0f);
                    JO(a, M_VPA_scaleY, (jfloat)0.0f);
                }
            }
            JV(a, M_VPA_start);
        }
        aeui_unframe(env);
        aeui_after(e->trans_ms, aeui_overlay_finalize, (void*)(intptr_t)overlay_handle);
        return;
    }
    aeui_overlay_finalize((void*)(intptr_t)overlay_handle);
}

void aether_ui_overlay_set_on_dismiss_impl(int h, void* boxed_closure) {
    AeuiOverlay* e = overlay_at(h);
    if (e) e->on_dismiss = (AeClosure*)boxed_closure;
}
int aether_ui_overlay_is_live_impl(int h)     { AeuiOverlay* e = overlay_at(h); return e ? e->live : 0; }
int aether_ui_overlay_count_impl(void)        { return overlay_count; }
int aether_ui_overlay_is_modal_impl(int h)    { AeuiOverlay* e = overlay_at(h); return e ? e->modal : 0; }
int aether_ui_overlay_is_exiting_impl(int h)  { AeuiOverlay* e = overlay_at(h); return e ? e->exiting : 0; }
int aether_ui_overlay_exit_played_impl(int h) { AeuiOverlay* e = overlay_at(h); return e ? e->exit_played : 0; }

void aether_ui_overlay_set_transition_impl(int h, const char* kind, int ms) {
    AeuiOverlay* e = overlay_at(h);
    if (!e) return;
    free(e->trans_kind);
    e->trans_kind = (kind && *kind) ? strdup(kind) : NULL;
    e->trans_ms = ms > 0 ? ms : 0;
}

// "blur" is real from API 31 (RenderEffect on the body) and reported as
// such; below it, and for "tint", the scrim is a light tint; else dim.
void aether_ui_overlay_set_material_impl(int h, const char* kind) {
    AeuiOverlay* e = overlay_at(h);
    if (!e) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    const char* eff = "dim";
    if (kind && strcmp(kind, "blur") == 0) eff = aeui_sdk_int(env) >= 31 ? "blur" : "tint";
    else if (kind && strcmp(kind, "tint") == 0) eff = "tint";
    int was_blur = e->material && strcmp(e->material, "blur") == 0;
    free(e->material);
    e->material = strdup(eff);
    AeuiWidget* s = live_widget(e->scrim);
    if (s) {
        (*env)->CallVoidMethod(env, s->view, J.View_setBackgroundColor, (jint)aeui_scrim_argb(eff));
        aeui_check(env, "scrim material");
    }
    if (e->live && (was_blur || strcmp(eff, "blur") == 0)) aeui_apply_blur(env, e->window);
    aeui_unframe(env);
}
const char* aether_ui_overlay_material_effective_impl(int h) {
    AeuiOverlay* e = overlay_at(h);
    return (e && e->material) ? e->material : "dim";
}

// Escape closes the topmost live overlay (1 if one closed).
static int aeui_escape_overlays(void) {
    for (int i = overlay_count - 1; i >= 0; i--) {
        if (overlays[i].live && !overlays[i].exiting) {
            aether_ui_overlay_close_impl(i + 1);
            return 1;
        }
    }
    return 0;
}

static void aeui_scrim_tapped(int overlay_handle) {
    AeuiOverlay* e = overlay_at(overlay_handle);
    if (!e || !e->live) return;
    AeClosure* c = e->on_dismiss;
    if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
    aether_ui_overlay_close_impl(overlay_handle);
}

// A label in the overlay layer: what the toast and the drawn tooltip show.
static int aeui_overlay_label(JNIEnv* env, const char* text, unsigned int bg, float size) {
    jobject tv = g_activity ? JNEW(M_TV_init, g_activity) : NULL;
    if (!tv) return 0;
    set_text_on(env, tv, text ? text : "");
    JV(tv, M_TV_setTextColor, (jint)0xFFFFFFFF);
    JV(tv, M_TV_setTextSize, (jint)1, (jfloat)size);
    int ph = aeui_dp(12), pv = aeui_dp(8);
    JV(tv, M_View_setPadding, ph, pv, ph, pv);
    jobject gd = JNEW(M_GD_init);
    if (gd) {
        JV(gd, M_GD_setColor, (jint)bg);
        JV(gd, M_GD_setCornerRadius, (jfloat)(8.0f * g_density));
        JV(tv, M_View_setBackground, gd);
    }
    return register_widget_typed(env, tv, AUI_TEXT);
}

// --- Toast -------------------------------------------------------------------
// A registered label in the overlay layer, bottom-centre and 24 dp up, that
// closes itself after ms (AppKit's toast; Android's own Toast is a system
// window the app cannot see into, so the driver could not find it).
static void aeui_toast_expire(void* arg) { aether_ui_overlay_close_impl((int)(intptr_t)arg); }

int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    JNIEnv* env = aeui_frame(16);
    if (!env) return 0;
    int content = aeui_overlay_label(env, text, 0xF2262629u, 14.0f);
    aeui_unframe(env);
    if (!content) return 0;
    int h = aether_ui_overlay_open_impl(win_handle, content, 9, 0, -24, 0);
    if (!h) {
        JNIEnv* e2 = aeui_frame(8);
        if (e2) { aeui_retire_tree(e2, content); aeui_unframe(e2); }
        return 0;
    }
    if (ms > 0) aeui_after(ms, aeui_toast_expire, (void*)(intptr_t)h);
    return h;
}

// --- Drawn vg tooltip: one overlay label at the pointer ---------------------
static int g_vg_tooltip = 0;
extern int aether_ui_canvas_get_widget(int canvas_id);
int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text, double cx, double cy) {
    if (g_vg_tooltip && aether_ui_overlay_is_live_impl(g_vg_tooltip)) {
        AeuiOverlay* e = overlay_at(g_vg_tooltip);
        if (e) e->trans_ms = 0;
        aether_ui_overlay_close_impl(g_vg_tooltip);
    }
    // The point is the canvas's; the overlay layer is the window's.
    int ox = 0, oy = 0;
    AeuiWidget* cw = live_widget(aether_ui_canvas_get_widget(canvas_id));
    JNIEnv* env = aeui_frame(16);
    if (!env) return 0;
    if (cw) {
        int ww, hh;
        int hx = 0, hy = 0, hw, hhh;
        aeui_rect_dp(env, cw->view, &ox, &oy, &ww, &hh);
        if (g_host) aeui_rect_dp(env, g_host, &hx, &hy, &hw, &hhh);
        ox -= hx; oy -= hy;
    }
    int content = aeui_overlay_label(env, text, 0xD9000000u, 13.0f);
    aeui_unframe(env);
    if (!content) return 0;
    g_vg_tooltip = aether_ui_overlay_open_impl(1, content, 0, ox + (int)cx + 12, oy + (int)cy + 12, 0);
    return g_vg_tooltip;
}
void aether_ui_vg_tooltip_hide_impl(void) {
    if (g_vg_tooltip) { aether_ui_overlay_close_impl(g_vg_tooltip); g_vg_tooltip = 0; }
}
// Whether vg scenes take the DRAWN tooltip path (the question GTK4, AppKit
// and Win32 answer here; it is not "is one showing"). On Android it is the
// default: a vg shape is a region of one View, and the platform's tooltip
// (View.setTooltipText) belongs to a whole View, so drawing it is the only
// way a shape's tooltip shows at all. AETHER_UI_TOOLTIP=native turns it off.
int aether_ui_vg_tooltip_drawn_impl(void) {
    const char* force = getenv("AETHER_UI_TOOLTIP");
    return !(force && strcmp(force, "native") == 0);
}

// --- Alert -------------------------------------------------------------------
// An AlertDialog with an OK button. Shown, not waited for: the ABI returns
// nothing, and Android's dialogs are modeless to the caller (UIKit's alert
// is presented the same way). Headless it is recorded, as everywhere.
JCLASS(C_ADBuilder, "android/app/AlertDialog$Builder");
JMETHOD(M_ADB_init, C_ADBuilder, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_ADB_setTitle, C_ADBuilder, "setTitle", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;");
JMETHOD(M_ADB_setMessage, C_ADBuilder, "setMessage", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;");
JMETHOD(M_ADB_setPositiveButton, C_ADBuilder, "setPositiveButton",
        "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;");
JMETHOD(M_ADB_show, C_ADBuilder, "show", "()Landroid/app/AlertDialog;");

void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) {
        free(aether_ui_prompt_headless(AEUI_PROMPT_ALERT, title, message));
        return;
    }
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return; }
    jobject b = JNEW(M_ADB_init, g_activity);
    if (b) {
        JO(b, M_ADB_setTitle, aeui_jstring(env, title ? title : ""));
        JO(b, M_ADB_setMessage, aeui_jstring(env, message ? message : ""));
        JO(b, M_ADB_setPositiveButton, aeui_jstring(env, "OK"), (jobject)NULL);
        JO(b, M_ADB_show);
    }
    aeui_unframe(env);
}

// --- File pickers --------------------------------------------------------
// Headless: the shared scripted-answer queue, exactly as every backend.
// Otherwise the Storage Access Framework picker as a modal
// (AetherActivity.pick, the nested loop described there). Off the UI thread
// the call waits for the UI thread to run it.
typedef struct { int kind; const char* title; const char* detail; char* out; } AeuiPick;
static jmethodID g_activity_pick = NULL;

static void aeui_pick_run(void* arg) {
    AeuiPick* p = (AeuiPick*)arg;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    if (g_activity && g_activity_pick) {
        jstring t = aeui_jstring(env, p->title ? p->title : "");
        jstring d = aeui_jstring(env, p->detail ? p->detail : "");
        jobject r = (*env)->CallObjectMethod(env, g_activity, g_activity_pick, (jint)p->kind, t, d);
        if (!aeui_check(env, "AetherActivity.pick") && r) p->out = aeui_charseq_dup(env, r);
    }
    aeui_unframe(env);
}

static char* aeui_pick(int prompt_kind, int kind, const char* title, const char* detail) {
    if (aeui_is_headless()) return aether_ui_prompt_headless(prompt_kind, title, detail);
    AeuiPick p = { kind, title, detail, NULL };
    aeui_android_run_sync(aeui_pick_run, &p);
    return p.out ? p.out : strdup("");
}

char* aether_ui_file_open(const char* title, const char* start_dir) {
    return aeui_pick(AEUI_PROMPT_OPEN, 0, title, start_dir);
}
char* aether_ui_file_save(const char* title, const char* default_name) {
    return aeui_pick(AEUI_PROMPT_SAVE, 1, title, default_name);
}
char* aether_ui_file_pick_folder(const char* title, const char* start_dir) {
    return aeui_pick(AEUI_PROMPT_FOLDER, 2, title, start_dir);
}

// ===========================================================================
// System: clipboard, URLs, appearance, quitting
// ===========================================================================
JCLASS(C_Context2, "android/content/Context");
JCLASS(C_ClipboardManager, "android/content/ClipboardManager");
JCLASS(C_ClipData, "android/content/ClipData");
JCLASS(C_ClipItem, "android/content/ClipData$Item");
JMETHOD(M_Ctx_getSystemService, C_Context2, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
JMETHOD(M_Ctx_getResources, C_Context2, "getResources", "()Landroid/content/res/Resources;");
JMETHOD(M_Ctx_startActivity, C_Context2, "startActivity", "(Landroid/content/Intent;)V");
JMETHOD(M_Ctx_getPackageManager, C_Context2, "getPackageManager", "()Landroid/content/pm/PackageManager;");
JMETHOD(M_Ctx_checkSelfPermission, C_Context2, "checkSelfPermission", "(Ljava/lang/String;)I");
JMETHOD(M_CM_setPrimaryClip, C_ClipboardManager, "setPrimaryClip", "(Landroid/content/ClipData;)V");
JMETHOD(M_CM_getPrimaryClip, C_ClipboardManager, "getPrimaryClip", "()Landroid/content/ClipData;");
JSTATIC(M_CD_newPlainText, C_ClipData, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
JMETHOD(M_CD_getItemCount, C_ClipData, "getItemCount", "()I");
JMETHOD(M_CD_getItemAt, C_ClipData, "getItemAt", "(I)Landroid/content/ClipData$Item;");
JMETHOD(M_CI_coerceToText, C_ClipItem, "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;");

static jobject aeui_service(JNIEnv* env, const char* name) {
    if (!g_activity) return NULL;
    return JO(g_activity, M_Ctx_getSystemService, aeui_jstring(env, name));
}

typedef struct { const char* text; char* out; } AeuiClip;
static void aeui_clip_write_run(void* arg) {
    AeuiClip* c = (AeuiClip*)arg;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    jobject cm = aeui_service(env, "clipboard");
    jobject cd = JSO(M_CD_newPlainText, aeui_jstring(env, "aether-ui"), aeui_jstring(env, c->text ? c->text : ""));
    if (cm && cd) JV(cm, M_CM_setPrimaryClip, cd);
    aeui_unframe(env);
}
static void aeui_clip_read_run(void* arg) {
    AeuiClip* c = (AeuiClip*)arg;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    jobject cm = aeui_service(env, "clipboard");
    jobject cd = cm ? JO(cm, M_CM_getPrimaryClip) : NULL;
    if (cd && JI(cd, M_CD_getItemCount) > 0) {
        jobject it = JO(cd, M_CD_getItemAt, (jint)0);
        jobject cs = it ? JO(it, M_CI_coerceToText, g_activity) : NULL;
        if (cs) c->out = aeui_charseq_dup(env, cs);
    }
    aeui_unframe(env);
}

// ClipboardManager, on the UI thread (the service is the app's own).
void aether_ui_clipboard_write_impl(const char* text) {
    AeuiClip c = { text, NULL };
    aeui_android_run_sync(aeui_clip_write_run, &c);
}
char* aether_ui_clipboard_read_impl(void) {
    AeuiClip c = { NULL, NULL };
    aeui_android_run_sync(aeui_clip_read_run, &c);
    return c.out ? c.out : strdup("");
}

// An ACTION_VIEW intent: the browser for a web URL, whatever the user's
// apps handle for anything else. Headless it is recorded, never opened.
JCLASS(C_Intent, "android/content/Intent");
JCLASS(C_Uri, "android/net/Uri");
JMETHOD(M_Intent_init2, C_Intent, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
JMETHOD(M_Intent_addFlags, C_Intent, "addFlags", "(I)Landroid/content/Intent;");
JSTATIC(M_Uri_parse, C_Uri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");

typedef struct { const char* url; } AeuiUrl;
static void aeui_open_url_run(void* arg) {
    const char* url = ((AeuiUrl*)arg)->url;
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return; }
    jobject uri = JSO(M_Uri_parse, aeui_jstring(env, url));
    jobject in = uri ? JNEW(M_Intent_init2, aeui_jstring(env, "android.intent.action.VIEW"), uri) : NULL;
    if (in) {
        JO(in, M_Intent_addFlags, (jint)0x10000000 /* FLAG_ACTIVITY_NEW_TASK */);
        // No app for the scheme raises ActivityNotFoundException, which the
        // call reports and clears: nothing opens, as on a desktop with no
        // handler.
        JV(g_activity, M_Ctx_startActivity, in);
    }
    aeui_unframe(env);
}
void aether_ui_open_url_impl(const char* url) {
    if (!url) return;
    if (aeui_is_headless()) { aether_ui_opened_url_record(url); return; }
    AeuiUrl u = { url };
    aeui_android_run_sync(aeui_open_url_run, &u);
}

// Dark mode is the configuration's night bit (Configuration.uiMode), which
// follows the system theme; the driver's override comes first.
JCLASS(C_Resources, "android/content/res/Resources");
JCLASS(C_Configuration, "android/content/res/Configuration");
JMETHOD(M_Res_getConfiguration, C_Resources, "getConfiguration", "()Landroid/content/res/Configuration;");
JFIELD(F_Cfg_uiMode, C_Configuration, "uiMode", "I");

static int aeui_system_dark(void) {
    JNIEnv* env = aeui_frame(8);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return 0; }
    int dark = 0;
    jobject res = JO(g_activity, M_Ctx_getResources);
    jobject cfg = res ? JO(res, M_Res_getConfiguration) : NULL;
    jfieldID f = cfg ? (jfieldID)jmem(env, &F_Cfg_uiMode) : NULL;
    if (f) dark = (((*env)->GetIntField(env, cfg, f)) & 0x30) == 0x20;   // UI_MODE_NIGHT_YES
    aeui_unframe(env);
    return dark;
}

int aether_ui_dark_mode_check(void) {
    int ov = aether_ui_appearance_override_get();
    if (ov >= 0) return ov;
    return aeui_system_dark();
}

// The OS path: the manifest claims uiMode changes, so a theme switch arrives
// as a configuration change (native_lifecycle) instead of a restart; when
// the night bit flipped, the registered closures run with the new value.
static int g_appearance_watched = 0;
static int g_appearance_last = -1;
void aether_ui_watch_appearance_impl(void) {
    if (g_appearance_watched) return;
    g_appearance_watched = 1;
    g_appearance_last = aeui_system_dark();
}
static void aeui_appearance_config_changed(void) {
    if (!g_appearance_watched) return;
    int dark = aeui_system_dark();
    if (dark == g_appearance_last) return;
    g_appearance_last = dark;
    if (aether_ui_appearance_override_get() < 0) aether_ui_appearance_invoke(dark);
}

// The driver's path (already on the UI thread: the server runs every
// request there): override, then the closures, as AppKit does.
int aether_ui_fire_appearance(int dark) {
    aether_ui_appearance_override_set(dark ? 1 : 0);
    aether_ui_appearance_invoke(dark ? 1 : 0);
    return 1;
}

// Undo and redo through the shared stack; the driver calls these on the UI
// thread, where the edit closures belong.
int aether_ui_fire_undo(void) { return aether_ui_undo_step_impl(); }
int aether_ui_fire_redo(void) { return aether_ui_redo_step_impl(); }

// An app quitting itself: the activity finishes and leaves the recents list,
// and its destroy ends the process (native_lifecycle), which is the
// desktop's "the loop returned and main ended".
static void aeui_quit_run(void* arg) {
    (void)arg;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    if (g_activity) JV(g_activity, M_Act_finishAndRemoveTask);
    aeui_unframe(env);
}
void aether_ui_app_quit_impl(void) {
    aether_ui_request_quit();
    if (g_bridge[1] >= 0) aeui_android_post(aeui_quit_run, NULL);
}

// ===========================================================================
// Menus. A menu is a record of (label, closure) items, also recorded in the
// shared side-store the driver's /menus and /menu/{h}/activate read. A menu
// BAR is a record of menus. Shown as:
//   - window 1's bar: the action bar's options menu, rebuilt from the
//     records whenever the activity asks (AetherActivity.onPrepareOptionsMenu
//     -> nativeOptionsMenu); each menu a SubMenu, each item a MenuItem whose
//     id names (menu, item); separators start a new group, with the group
//     dividers on;
//   - a dialog window's bar: a row of the menus' titles at its top, each
//     opening its menu as a PopupMenu;
//   - menu_popup: a PopupMenu anchored to the widget.
// ===========================================================================
JCLASS(C_PopupMenu, "android/widget/PopupMenu");
JCLASS(C_Menu, "android/view/Menu");
JCLASS(C_MenuItem, "android/view/MenuItem");
JMETHOD(M_PM_init, C_PopupMenu, "<init>", "(Landroid/content/Context;Landroid/view/View;)V");
JMETHOD(M_PM_getMenu, C_PopupMenu, "getMenu", "()Landroid/view/Menu;");
JMETHOD(M_PM_setOnMenuItemClickListener, C_PopupMenu, "setOnMenuItemClickListener",
        "(Landroid/widget/PopupMenu$OnMenuItemClickListener;)V");
JMETHOD(M_PM_show, C_PopupMenu, "show", "()V");
JMETHOD(M_Menu_add, C_Menu, "add", "(IIILjava/lang/CharSequence;)Landroid/view/MenuItem;");
JMETHOD(M_Menu_addSubMenu, C_Menu, "addSubMenu", "(IIILjava/lang/CharSequence;)Landroid/view/SubMenu;");
JMETHOD(M_Menu_setGroupDividerEnabled, C_Menu, "setGroupDividerEnabled", "(Z)V");
JMETHOD(M_Menu_size, C_Menu, "size", "()I");
JMETHOD(M_Menu_getItem, C_Menu, "getItem", "(I)Landroid/view/MenuItem;");
JMETHOD(M_Menu_performIdentifierAction, C_Menu, "performIdentifierAction", "(II)Z");
JMETHOD(M_MI_getItemId, C_MenuItem, "getItemId", "()I");
JMETHOD(M_MI_getSubMenu, C_MenuItem, "getSubMenu", "()Landroid/view/SubMenu;");
JMETHOD(M_MI_getTitle, C_MenuItem, "getTitle", "()Ljava/lang/CharSequence;");
JMETHOD(M_MI_setAlphabeticShortcut, C_MenuItem, "setAlphabeticShortcut", "(CI)Landroid/view/MenuItem;");

#define AEUI_MENU_ID(menu, index) ((menu) * 1000 + (index) + 1)

typedef struct { char* label; AeClosure* closure; int is_sep; } AeuiMenuItem;
typedef struct {
    char* label;
    AeuiMenuItem* items; int count;
    int is_bar;
    int* menus; int nmenus;      // a bar's menus
} AeuiMenuRec;
static AeuiMenuRec* menurecs = NULL;
static int nmenurecs = 0;
static int g_window1_bar = 0;    // the bar the activity shows
static jobject g_options_menu = NULL;   // global ref: the activity's options Menu, once built

static AeuiMenuRec* menu_rec(int h) { return (h >= 1 && h <= nmenurecs) ? &menurecs[h - 1] : NULL; }

static int aeui_menu_new(const char* label, int is_bar) {
    AeuiMenuRec* nm = (AeuiMenuRec*)realloc(menurecs, sizeof(AeuiMenuRec) * (size_t)(nmenurecs + 1));
    if (!nm) return 0;
    menurecs = nm;
    AeuiMenuRec* m = &menurecs[nmenurecs++];
    memset(m, 0, sizeof(*m));
    m->label = strdup(label ? label : "");
    m->is_bar = is_bar;
    return nmenurecs;
}

static void aeui_menu_push(int h, const char* label, AeClosure* c, int is_sep) {
    AeuiMenuRec* m = menu_rec(h);
    if (!m) return;
    AeuiMenuItem* ni = (AeuiMenuItem*)realloc(m->items, sizeof(AeuiMenuItem) * (size_t)(m->count + 1));
    if (!ni) return;
    m->items = ni;
    m->items[m->count].label = strdup(label ? label : "");
    m->items[m->count].closure = c;
    m->items[m->count].is_sep = is_sep;
    m->count++;
}

static void aeui_refresh_options_menu(void) {
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    if (g_activity) JV(g_activity, M_Act_invalidateOptionsMenu);
    aeui_unframe(env);
}

int aether_ui_menu_create(const char* label) { return aeui_menu_new(label, 0); }
int aether_ui_menu_bar_create(void) { return aeui_menu_new("", 1); }

void aether_ui_menu_add_item(int menu_handle, const char* label, void* boxed_closure) {
    if (!menu_rec(menu_handle)) return;
    aeui_menu_push(menu_handle, label, (AeClosure*)boxed_closure, 0);
    aether_ui_menu_item_record(menu_handle, label ? label : "", boxed_closure);
    aeui_refresh_options_menu();
}

void aether_ui_menu_add_separator(int menu_handle) {
    aeui_menu_push(menu_handle, "", NULL, 1);
    aeui_refresh_options_menu();
}

void aether_ui_menu_item_set_label(int menu_handle, const char* old_label, const char* new_label) {
    if (!old_label || !new_label) return;
    aether_ui_menu_item_relabel(menu_handle, old_label, new_label);
    AeuiMenuRec* m = menu_rec(menu_handle);
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        if (m->items[i].is_sep || strcmp(m->items[i].label, old_label) != 0) continue;
        free(m->items[i].label);
        m->items[i].label = strdup(new_label);
        break;
    }
    aeui_refresh_options_menu();
}

void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    AeuiMenuRec* b = menu_rec(bar_handle);
    if (!b || !menu_rec(menu_handle)) return;
    int* nm = (int*)realloc(b->menus, sizeof(int) * (size_t)(b->nmenus + 1));
    if (!nm) return;
    b->menus = nm;
    b->menus[b->nmenus++] = menu_handle;
    aeui_refresh_options_menu();
}

// Fill a Menu with one menu's items (ids from AEUI_MENU_ID; a separator
// starts the next group, drawn with a divider).
static void aeui_fill_menu(JNIEnv* env, jobject menu, int menu_handle) {
    AeuiMenuRec* m = menu_rec(menu_handle);
    if (!m || !menu) return;
    int group = 0;
    for (int i = 0; i < m->count; i++) {
        if (m->items[i].is_sep) { group++; continue; }
        JO(menu, M_Menu_add, (jint)group, (jint)AEUI_MENU_ID(menu_handle, i), (jint)i,
           aeui_jstring(env, m->items[i].label));
    }
    if (group) JV(menu, M_Menu_setGroupDividerEnabled, JNI_TRUE);
}

// AetherActivity.onCreate/PrepareOptionsMenu: the bar attached to window 1.
static jboolean JNICALL native_options_menu(JNIEnv* env, jclass cls, jobject menu) {
    (void)cls;
    AeuiMenuRec* b = menu_rec(g_window1_bar);
    if (!b || !menu) return JNI_FALSE;
    if ((*env)->PushLocalFrame(env, 64) != 0) return JNI_FALSE;
    if (g_options_menu) (*env)->DeleteGlobalRef(env, g_options_menu);
    g_options_menu = (*env)->NewGlobalRef(env, menu);
    for (int i = 0; i < b->nmenus; i++) {
        AeuiMenuRec* m = menu_rec(b->menus[i]);
        if (!m) continue;
        jobject sub = JO(menu, M_Menu_addSubMenu, (jint)0, (jint)AEUI_MENU_ID(b->menus[i], 998),
                         (jint)i, aeui_jstring(env, m->label));
        aeui_fill_menu(env, sub, b->menus[i]);
    }
    (*env)->PopLocalFrame(env, NULL);
    return b->nmenus > 0 ? JNI_TRUE : JNI_FALSE;
}

void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    (void)app_handle;
    g_window1_bar = bar_handle;
    aeui_refresh_options_menu();
}

// A dialog window has no action bar: its bar is a row of buttons at the top,
// one per menu, each opening its menu (MENU_OPEN).
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle) {
    if (win_handle <= 1) { aether_ui_menu_bar_attach(0, bar_handle); return; }
    AeuiWinRec* r = win_rec(win_handle);
    AeuiMenuRec* b = menu_rec(bar_handle);
    if (!r || !b) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    if (aeui_wrec_realize(env, r, win_handle)) {
        if (r->menubar) {
            JV(r->host, M_VG_removeView, r->menubar);
            (*env)->DeleteGlobalRef(env, r->menubar);
            r->menubar = NULL;
        }
        jobject row = JNEW(M_LL_init, g_activity);
        if (row) {
            JV(row, M_LL_setOrientation, (jint)LL_HORIZONTAL);
            for (int i = 0; i < b->nmenus; i++) {
                AeuiMenuRec* m = menu_rec(b->menus[i]);
                jobject btn = m ? make_button(env, m->label) : NULL;
                if (!btn) continue;
                jobject l = aeui_listener3(env, b->menus[i], AEUI_EV_MENU_OPEN, 0);
                if (l) JV(btn, M_View_setOnClickListener, l);
                jobject blp = JNEW(M_LLP_init, (jint)LP_WRAP_CONTENT, (jint)LP_WRAP_CONTENT, (jfloat)0.0f);
                JV(row, M_VG_addView, btn, blp);
            }
            jobject lp = JNEW(M_FP_init3, (jint)LP_MATCH_PARENT, (jint)LP_WRAP_CONTENT, (jint)GRAV_TOP);
            JV(r->host, M_VG_addView, row, lp);
            r->menubar = (*env)->NewGlobalRef(env, row);
        }
    }
    aeui_unframe(env);
}

static void aeui_popup_menu(JNIEnv* env, int menu_handle, jobject anchor) {
    if (!anchor || !g_activity || !menu_rec(menu_handle)) return;
    jobject pm = JNEW(M_PM_init, g_activity, anchor);
    if (!pm) return;
    aeui_fill_menu(env, JO(pm, M_PM_getMenu), menu_handle);
    jobject l = aeui_listener3(env, menu_handle, AEUI_EV_MENU, 0);
    if (l) JV(pm, M_PM_setOnMenuItemClickListener, l);
    JV(pm, M_PM_show);
}

// The menu as a PopupMenu at the widget (a pull-down from it). Headless
// nothing is shown: there is no one to choose (UIKit does the same).
void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    if (aeui_is_headless()) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    AeuiWidget* a = live_widget(anchor_widget);
    aeui_popup_menu(env, menu_handle, a ? a->view : g_host);
    aeui_unframe(env);
}

// A chosen item: (menu, index) from its id.
static int aeui_menu_fire_id(int id) {
    int menu = (id - 1) / 1000, index = (id - 1) % 1000;
    AeuiMenuRec* m = menu_rec(menu);
    if (!m || index < 0 || index >= m->count || m->items[index].is_sep) return 0;
    AeClosure* c = m->items[index].closure;
    if (c && c->fn) { ((void (*)(void*))c->fn)(c->env); return 1; }
    return 0;
}

// The driver's /menu/{h}/native_activate: through the platform's own
// binding -- the options menu's MenuItem, performed as a tap performs it
// (Menu.performIdentifierAction -> Activity.onOptionsItemSelected). 0 fired,
// 2 no such item; -1 = this menu is not in the options menu (a popup's or a
// dialog's), which has no performable native item: the route says 501.
static int aeui_menu_native_activate(JNIEnv* env, int menu_handle, const char* label) {
    AeuiMenuRec* bar = menu_rec(g_window1_bar);
    int in_bar = 0;
    for (int i = 0; bar && i < bar->nmenus; i++) if (bar->menus[i] == menu_handle) in_bar = 1;
    if (!in_bar || !g_options_menu) return -1;
    int n = JI(g_options_menu, M_Menu_size);
    for (int i = 0; i < n; i++) {
        jobject top = JO(g_options_menu, M_Menu_getItem, (jint)i);
        if (!top || JI(top, M_MI_getItemId) != AEUI_MENU_ID(menu_handle, 998)) continue;
        jobject sub = JO(top, M_MI_getSubMenu);
        int k = sub ? JI(sub, M_Menu_size) : 0;
        for (int j = 0; j < k; j++) {
            jobject it = JO(sub, M_Menu_getItem, (jint)j);
            char t[512];
            aeui_charseq_into(env, it ? JO(it, M_MI_getTitle) : NULL, t, (int)sizeof(t));
            if (strcmp(t, label ? label : "") != 0) continue;
            return JZ(sub, M_Menu_performIdentifierAction, (jint)JI(it, M_MI_getItemId), (jint)0) ? 0 : 4;
        }
    }
    return 2;
}

// --- Context menus -----------------------------------------------------------
// Items kept on the widget; a long press (or a secondary click) opens them
// as a PopupMenu at the widget. An accelerator is shown on the item, as the
// platform shows a menu item's keyboard shortcut.
static int aeui_accel_split(const char* accel, char* key) {
    int meta = 0;
    *key = 0;
    if (!accel) return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", accel);
    for (char* tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control") || !strcasecmp(tok, "Primary")) meta |= 0x1000;
        else if (!strcasecmp(tok, "Shift")) meta |= 0x1;
        else if (!strcasecmp(tok, "Alt")) meta |= 0x2;
        else if (!strcasecmp(tok, "Cmd") || !strcasecmp(tok, "Meta") || !strcasecmp(tok, "Super")) meta |= 0x10000;
        else if (strlen(tok) == 1) *key = tok[0];
    }
    return meta;
}

static void aeui_ctx_add(int handle, const char* label, const char* accel, void* closure) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !label) return;
    AeuiCtxItem* ni = (AeuiCtxItem*)realloc(w->ctx, sizeof(AeuiCtxItem) * (size_t)(w->nctx + 1));
    if (!ni) return;
    w->ctx = ni;
    w->ctx[w->nctx].label = strdup(label);
    w->ctx[w->nctx].accel = accel ? strdup(accel) : NULL;
    w->ctx[w->nctx].closure = (AeClosure*)closure;
    w->nctx++;
    if (w->nctx == 1) {
        JNIEnv* env = aeui_frame(8);
        if (!env) return;
        jobject l = aeui_listener(env, handle, AEUI_EV_CONTEXT);
        if (l) {
            JV(w->view, M_View_setOnLongClickListener, l);
            JV(w->view, M_View_setOnContextClickListener, l);
        }
        aeui_unframe(env);
    }
}

void aether_ui_context_menu_item_impl(int handle, const char* label, void* boxed_closure) {
    aeui_ctx_add(handle, label, NULL, boxed_closure);
}
void aether_ui_context_menu_item_accel_impl(int handle, const char* label, const char* accel,
                                            void* boxed_closure) {
    aeui_ctx_add(handle, label, accel, boxed_closure);
}

static void aeui_ctx_open(JNIEnv* env, int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->nctx == 0 || !g_activity) return;
    jobject pm = JNEW(M_PM_init, g_activity, w->view);
    if (!pm) return;
    jobject menu = JO(pm, M_PM_getMenu);
    for (int i = 0; i < w->nctx; i++) {
        jobject it = JO(menu, M_Menu_add, (jint)0, (jint)(i + 1), (jint)i, aeui_jstring(env, w->ctx[i].label));
        char key;
        int meta = aeui_accel_split(w->ctx[i].accel, &key);
        if (it && key) JO(it, M_MI_setAlphabeticShortcut, (jchar)key, (jint)meta);
    }
    jobject l = aeui_listener3(env, handle, AEUI_EV_MENU, 1);
    if (l) JV(pm, M_PM_setOnMenuItemClickListener, l);
    JV(pm, M_PM_show);
}

static int aeui_ctx_fire(int handle, int index) {
    AeuiWidget* w = live_widget(handle);
    if (!w || index < 0 || index >= w->nctx) return 0;
    AeClosure* c = w->ctx[index].closure;
    if (c && c->fn) { ((void (*)(void*))c->fn)(c->env); return 1; }
    return 0;
}

// ===========================================================================
// Keyboard: shortcuts, chords, the any-key handlers.
//
// Combos are matched in one canonical spelling, "ctrl+alt+shift+cmd+key"
// (modifiers in that order, key lower-cased), which "Ctrl+Shift+R",
// "<Control><Shift>r" and a real keypress all reduce to. "Primary" is Ctrl
// here -- Android's own keyboard accelerators (Ctrl+C/V/Z) -- as on GTK4 and
// Win32. A real key arrives from AetherActivity.dispatchKeyEvent; the
// driver's /window/key arrives as a combo; both take the same path:
// chords, then shortcuts (which consume the key), then the any-key
// handlers (which do not).
// ===========================================================================
#define KM_CTRL 1
#define KM_ALT 2
#define KM_SHIFT 4
#define KM_CMD 8

typedef struct { char combo[64]; AeClosure* closure; AeClosure* enabled; } AeuiShortcut;
static AeuiShortcut* shortcuts = NULL;
static int nshortcuts = 0;
typedef struct { char first[64]; char second[64]; AeClosure* closure; } AeuiChord;
static AeuiChord* chords = NULL;
static int nchords = 0;
static char chord_pending[64] = "";
static double chord_armed_at = 0;
static AeClosure** key_handlers = NULL;
static int nkey_handlers = 0;

static void combo_canonical(int mods, const char* key, char* out, int outsize) {
    char low[32];
    int i = 0;
    for (; key && key[i] && i < (int)sizeof(low) - 1; i++) {
        unsigned char ch = (unsigned char)key[i];
        low[i] = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    }
    low[i] = 0;
    snprintf(out, (size_t)outsize, "%s%s%s%s%s", (mods & KM_CTRL) ? "ctrl+" : "",
             (mods & KM_ALT) ? "alt+" : "", (mods & KM_SHIFT) ? "shift+" : "",
             (mods & KM_CMD) ? "cmd+" : "", low);
}

static int combo_mod(const char* name) {
    if (!strcasecmp(name, "Ctrl") || !strcasecmp(name, "Control") || !strcasecmp(name, "Primary")) return KM_CTRL;
    if (!strcasecmp(name, "Shift")) return KM_SHIFT;
    if (!strcasecmp(name, "Alt") || !strcasecmp(name, "Option")) return KM_ALT;
    if (!strcasecmp(name, "Cmd") || !strcasecmp(name, "Command") || !strcasecmp(name, "Meta") ||
        !strcasecmp(name, "Super")) return KM_CMD;
    return 0;
}

static void combo_normalize(const char* combo, char* out, int outsize) {
    out[0] = 0;
    if (!combo || !*combo) return;
    int mods = 0;
    char key[32] = "";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", combo);
    if (strchr(buf, '<')) {
        const char* p = buf;
        while (*p == '<') {
            const char* close = strchr(p, '>');
            if (!close) break;
            char name[32];
            size_t n = (size_t)(close - p - 1);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, p + 1, n);
            name[n] = 0;
            mods |= combo_mod(name);
            p = close + 1;
        }
        snprintf(key, sizeof(key), "%s", p);
    } else {
        // "Ctrl++" names the plus key: a trailing empty token after '+'.
        size_t L = strlen(buf);
        int plus_key = L >= 2 && buf[L - 1] == '+' && buf[L - 2] == '+';
        if (plus_key) buf[L - 1] = 0;
        for (char* tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
            int m = combo_mod(tok);
            if (m) mods |= m; else snprintf(key, sizeof(key), "%s", tok);
        }
        if (plus_key) snprintf(key, sizeof(key), "+");
    }
    combo_canonical(mods, key, out, outsize);
}

static double aeui_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int shortcut_fire(const char* canonical) {
    for (int i = 0; i < nshortcuts; i++) {
        if (strcmp(shortcuts[i].combo, canonical) != 0) continue;
        AeClosure* en = shortcuts[i].enabled;
        if (en && en->fn && !((int (*)(void*))en->fn)(en->env)) continue;   // inert: keep looking
        AeClosure* c = shortcuts[i].closure;
        if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
        return 1;
    }
    return 0;
}

// Two-key chords: the first combo arms, the second (within 1.5 s) fires.
static int chord_feed(const char* canonical) {
    if (chord_pending[0] && aeui_mono_now() - chord_armed_at > 1.5) chord_pending[0] = 0;
    if (chord_pending[0]) {
        for (int i = 0; i < nchords; i++) {
            if (strcmp(chords[i].first, chord_pending) == 0 && strcmp(chords[i].second, canonical) == 0) {
                chord_pending[0] = 0;
                AeClosure* c = chords[i].closure;
                if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
                return 1;
            }
        }
        chord_pending[0] = 0;
    }
    for (int i = 0; i < nchords; i++) {
        if (strcmp(chords[i].first, canonical) == 0) {
            snprintf(chord_pending, sizeof(chord_pending), "%s", canonical);
            chord_armed_at = aeui_mono_now();
            return 1;
        }
    }
    return 0;
}

static int shortcut_dispatch(const char* canonical) {
    if (!canonical || !*canonical) return 0;
    if (chord_feed(canonical)) return 1;
    return shortcut_fire(canonical);
}

void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure, void* enabled_closure) {
    if (!combo || !boxed_closure) return;
    AeuiShortcut* ns = (AeuiShortcut*)realloc(shortcuts, sizeof(AeuiShortcut) * (size_t)(nshortcuts + 1));
    if (!ns) return;
    shortcuts = ns;
    combo_normalize(combo, shortcuts[nshortcuts].combo, (int)sizeof(shortcuts[0].combo));
    shortcuts[nshortcuts].closure = (AeClosure*)boxed_closure;
    shortcuts[nshortcuts].enabled = (AeClosure*)enabled_closure;
    nshortcuts++;
}
void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) {
    aether_ui_shortcut_when_impl(combo, boxed_closure, NULL);
}
void aether_ui_shortcut_chord_impl(const char* first_combo, const char* second_combo, void* boxed_closure) {
    if (!first_combo || !second_combo || !boxed_closure) return;
    AeuiChord* nc = (AeuiChord*)realloc(chords, sizeof(AeuiChord) * (size_t)(nchords + 1));
    if (!nc) return;
    chords = nc;
    combo_normalize(first_combo, chords[nchords].first, (int)sizeof(chords[0].first));
    combo_normalize(second_combo, chords[nchords].second, (int)sizeof(chords[0].second));
    chords[nchords].closure = (AeClosure*)boxed_closure;
    nchords++;
}

// Every handler registered, in order (a second window_on_key must not
// silence the first: on_key() per widget is built on this).
void aether_ui_window_on_key_impl(void* boxed_closure) {
    if (!boxed_closure) return;
    AeClosure** nk = (AeClosure**)realloc(key_handlers, sizeof(AeClosure*) * (size_t)(nkey_handlers + 1));
    if (!nk) return;
    key_handlers = nk;
    key_handlers[nkey_handlers++] = (AeClosure*)boxed_closure;
}

int aether_ui_window_key_deliver(const char* key_name, int mods) {
    if (!key_name) return 0;
    int fired = 0, n = nkey_handlers;
    for (int i = 0; i < n; i++) {
        AeClosure* c = key_handlers[i];
        if (!c || !c->fn) continue;
        ((void (*)(void*, const char*, intptr_t))c->fn)(c->env, key_name, (intptr_t)mods);
        fired = 1;
    }
    return fired;
}

// The ABI's mods bits (1 shift, 2 ctrl, 4 alt, 8 super) from ours.
static int aeui_abi_mods(int km) {
    return ((km & KM_SHIFT) ? 1 : 0) | ((km & KM_CTRL) ? 2 : 0) | ((km & KM_ALT) ? 4 : 0) | ((km & KM_CMD) ? 8 : 0);
}

// A canonical combo back into (key name, ABI mods), with the key spelled as
// a real keypress spells it ("BackSpace", "Return", "Left"), so a spec drives
// the same strings the app receives from a keyboard.
static int aeui_combo_split(const char* canonical, char* name, int namesize) {
    int km = 0;
    const char* p = canonical;
    for (;;) {
        const char* plus = strchr(p, '+');
        if (!plus || plus == p) break;
        size_t n = (size_t)(plus - p);
        int m = (n == 4 && !strncmp(p, "ctrl", 4)) ? KM_CTRL : (n == 3 && !strncmp(p, "alt", 3)) ? KM_ALT
              : (n == 5 && !strncmp(p, "shift", 5)) ? KM_SHIFT : (n == 3 && !strncmp(p, "cmd", 3)) ? KM_CMD : 0;
        if (!m) break;
        km |= m;
        p = plus + 1;
    }
    static const char* canon[] = {
        "left", "Left", "right", "Right", "up", "Up", "down", "Down", "return", "Return",
        "enter", "Return", "escape", "Escape", "esc", "Escape", "tab", "Tab", "space", "space",
        "backspace", "BackSpace", "delete", "Delete", "home", "Home", "end", "End",
        "page_up", "Page_Up", "page_down", "Page_Down", "insert", "Insert", NULL };
    for (int i = 0; canon[i]; i += 2) {
        if (!strcmp(p, canon[i])) { snprintf(name, (size_t)namesize, "%s", canon[i + 1]); return aeui_abi_mods(km); }
    }
    if (p[0] == 'f' && p[1] >= '1' && p[1] <= '9') { snprintf(name, (size_t)namesize, "F%s", p + 1); return aeui_abi_mods(km); }
    snprintf(name, (size_t)namesize, "%s", p);
    return aeui_abi_mods(km);
}

// Tab / Shift+Tab move focus through the focusable widgets, as a desktop
// window's do (View.focusSearch, the platform's own focus order). A real
// key takes the window out of touch mode, and with it every focusable
// widget (a checkbox, a button) joins the order rather than only those
// focusable by touch (a text field, a canvas that takes keys); the driver's
// Tab is not a real key event, so it leaves touch mode the way a key would
// (requestFocusFromTouch on the focused widget) before searching.
static int aeui_focus_step(JNIEnv* env, int backward) {
    jobject f = g_activity ? JO(g_activity, M_Act_getCurrentFocus) : NULL;
    if (!f) return 0;
    JZ(f, M_View_requestFocusFromTouch);
    jobject next = JO(f, M_View_focusSearch, (jint)(backward ? 1 /* FOCUS_BACKWARD */ : 2 /* FOCUS_FORWARD */));
    return next ? (JZ(next, M_View_requestFocus) ? 1 : 0) : 0;
}

// One key, as a canonical combo plus the name and mods the any-key
// handlers get. Returns 1 when something consumed or handled it.
static int aeui_key_route(const char* canonical, const char* name, int abi_mods, int* consumed) {
    *consumed = 0;
    if (shortcut_dispatch(canonical)) { *consumed = 1; return 1; }
    return aether_ui_window_key_deliver(name, abi_mods);
}

// Android key codes -> the key names the DSL speaks (GDK's, as every
// backend reports them).
static const char* aeui_key_name(int code) {
    switch (code) {
        case 19: return "Up";        case 20: return "Down";
        case 21: return "Left";      case 22: return "Right";
        case 66: case 160: return "Return";
        case 111: return "Escape";   case 61: return "Tab";
        case 62: return "space";     case 67: return "BackSpace";
        case 112: return "Delete";   case 122: return "Home";
        case 123: return "End";      case 92: return "Page_Up";
        case 93: return "Page_Down"; case 124: return "Insert";
        default: return NULL;
    }
}

static jboolean JNICALL native_key(JNIEnv* env, jclass cls, jint code, jint meta, jint unicode, jint repeat) {
    (void)cls; (void)repeat;
    // A modifier on its own is not a key the app hears about.
    if ((code >= 57 && code <= 60) || code == 113 || code == 114 || code == 117 || code == 118 ||
        code == 115 || code == 119) return JNI_FALSE;
    int km = ((meta & 0x1000) ? KM_CTRL : 0) | ((meta & 0x2) ? KM_ALT : 0) |
             ((meta & 0x1) ? KM_SHIFT : 0) | ((meta & 0x10000) ? KM_CMD : 0);
    char name[16] = "", base[16] = "";
    const char* named = aeui_key_name(code);
    if (named) {
        snprintf(name, sizeof(name), "%s", named);
        snprintf(base, sizeof(base), "%s", named);
    } else if (code >= 131 && code <= 142) {
        snprintf(name, sizeof(name), "F%d", code - 130);
        snprintf(base, sizeof(base), "%s", name);
    } else if (code >= 29 && code <= 54) {
        // A letter: the combo names the key (shift stays a modifier, so
        // Ctrl+Shift+C is not Ctrl+C); the handler gets the character typed.
        snprintf(base, sizeof(base), "%c", 'a' + (code - 29));
        if (unicode > 0 && unicode < 128) snprintf(name, sizeof(name), "%c", (char)unicode);
        else snprintf(name, sizeof(name), "%s", base);
    } else if (unicode > 0) {
        // Anything else printable is its character, shift folded into it.
        char u[8] = "";
        if (unicode < 0x80) { u[0] = (char)unicode; }
        else if (unicode < 0x800) { u[0] = (char)(0xC0 | (unicode >> 6)); u[1] = (char)(0x80 | (unicode & 63)); }
        else { u[0] = (char)(0xE0 | (unicode >> 12)); u[1] = (char)(0x80 | ((unicode >> 6) & 63)); u[2] = (char)(0x80 | (unicode & 63)); }
        snprintf(name, sizeof(name), "%s", u);
        snprintf(base, sizeof(base), "%s", u);
        km &= ~KM_SHIFT;
    } else {
        return JNI_FALSE;
    }
    char canonical[64];
    combo_canonical(km, base, canonical, (int)sizeof(canonical));
    int consumed = 0;
    if ((*env)->PushLocalFrame(env, 16) != 0) return JNI_FALSE;
    aeui_key_route(canonical, name, aeui_abi_mods(km), &consumed);
    // Escape with nothing bound to it closes the topmost overlay.
    if (!consumed && code == 111 && aeui_escape_overlays()) consumed = 1;
    (*env)->PopLocalFrame(env, NULL);
    return consumed ? JNI_TRUE : JNI_FALSE;
}

// The driver's /window/key?combo=: the same route, then the window's own
// keys (Escape closes an overlay, Tab moves focus) when nothing took it.
static int aeui_driver_key(JNIEnv* env, const char* combo) {
    char canonical[64], name[64];
    combo_normalize(combo, canonical, (int)sizeof(canonical));
    int mods = aeui_combo_split(canonical, name, (int)sizeof(name));
    int consumed = 0;
    int fired = aeui_key_route(canonical, name, mods, &consumed);
    if (!fired) {
        if (!strcmp(name, "Escape")) fired = aeui_escape_overlays();
        else if (!strcmp(name, "Tab")) fired = aeui_focus_step(env, mods & 1);
    }
    return fired;
}

// ===========================================================================
// Notifications -- NotificationManager on a channel of the app's own
// ("aether-ui", API 26+). A tap brings the app back (a PendingIntent to the
// activity, singleTop) and runs the notification's closure through the
// shared registry, which also keeps what the driver's /notifications lists.
// Headless nothing is posted (no one to see it); the record is made either
// way, as on every backend.
// ===========================================================================
JCLASS(C_NotificationManager, "android/app/NotificationManager");
JCLASS(C_NotificationChannel, "android/app/NotificationChannel");
JCLASS(C_NotifBuilder, "android/app/Notification$Builder");
JCLASS(C_PendingIntent, "android/app/PendingIntent");
JCLASS(C_Object, "java/lang/Object");
JCLASS(C_BitmapFactory2, "android/graphics/BitmapFactory");
JMETHOD(M_NC_init, C_NotificationChannel, "<init>", "(Ljava/lang/String;Ljava/lang/CharSequence;I)V");
JMETHOD(M_NM_createChannel, C_NotificationManager, "createNotificationChannel", "(Landroid/app/NotificationChannel;)V");
JMETHOD(M_NM_notify, C_NotificationManager, "notify", "(Ljava/lang/String;ILandroid/app/Notification;)V");
JMETHOD(M_NB_init, C_NotifBuilder, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
JMETHOD(M_NB_setSmallIcon, C_NotifBuilder, "setSmallIcon", "(I)Landroid/app/Notification$Builder;");
JMETHOD(M_NB_setLargeIcon, C_NotifBuilder, "setLargeIcon", "(Landroid/graphics/Bitmap;)Landroid/app/Notification$Builder;");
JMETHOD(M_NB_setContentTitle, C_NotifBuilder, "setContentTitle", "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;");
JMETHOD(M_NB_setContentText, C_NotifBuilder, "setContentText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;");
JMETHOD(M_NB_setAutoCancel, C_NotifBuilder, "setAutoCancel", "(Z)Landroid/app/Notification$Builder;");
JMETHOD(M_NB_setContentIntent, C_NotifBuilder, "setContentIntent", "(Landroid/app/PendingIntent;)Landroid/app/Notification$Builder;");
JMETHOD(M_NB_build, C_NotifBuilder, "build", "()Landroid/app/Notification;");
JSTATIC(M_PI_getActivity, C_PendingIntent, "getActivity",
        "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;");
JMETHOD(M_Obj_getClass, C_Object, "getClass", "()Ljava/lang/Class;");
JMETHOD(M_Intent_initCls, C_Intent, "<init>", "(Landroid/content/Context;Ljava/lang/Class;)V");
JMETHOD(M_Intent_putExtraI, C_Intent, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;");
JSTATIC(M_BF_decodeFile, C_BitmapFactory2, "decodeFile", "(Ljava/lang/String;)Landroid/graphics/Bitmap;");
JMETHOD(M_Act_requestPermissions, C_Activity, "requestPermissions", "([Ljava/lang/String;I)V");

typedef struct { int id; const char* title; const char* body; const char* icon; const char* tag; } AeuiNotif;

static void aeui_notify_run(void* arg) {
    AeuiNotif* n = (AeuiNotif*)arg;
    JNIEnv* env = aeui_frame(32);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return; }
    jobject nm = aeui_service(env, "notification");
    static int channel_made = 0;
    if (nm && !channel_made) {
        jobject ch = JNEW(M_NC_init, aeui_jstring(env, "aether-ui"),
                          aeui_jstring(env, g_title && *g_title ? g_title : "Notifications"),
                          (jint)3 /* IMPORTANCE_DEFAULT */);
        if (ch) { JV(nm, M_NM_createChannel, ch); channel_made = 1; }
    }
    jobject b = nm ? JNEW(M_NB_init, g_activity, aeui_jstring(env, "aether-ui")) : NULL;
    if (b) {
        JO(b, M_NB_setSmallIcon, (jint)aeui_android_r(env, "drawable", "ic_dialog_info"));
        JO(b, M_NB_setContentTitle, aeui_jstring(env, n->title ? n->title : ""));
        JO(b, M_NB_setContentText, aeui_jstring(env, n->body ? n->body : ""));
        JO(b, M_NB_setAutoCancel, JNI_TRUE);
        if (n->icon && *n->icon) {
            jobject bmp = JSO(M_BF_decodeFile, aeui_jstring(env, n->icon));
            if (bmp) JO(b, M_NB_setLargeIcon, bmp);
        }
        jobject cls = JO(g_activity, M_Obj_getClass);
        jobject in = cls ? JNEW(M_Intent_initCls, g_activity, cls) : NULL;
        if (in) {
            JO(in, M_Intent_putExtraI, aeui_jstring(env, "aeui_notification"), (jint)n->id);
            JO(in, M_Intent_addFlags, (jint)0x20000000 /* FLAG_ACTIVITY_SINGLE_TOP */);
            jobject pi = JSO(M_PI_getActivity, g_activity, (jint)n->id, in,
                             (jint)(0x04000000 | 0x08000000) /* IMMUTABLE | UPDATE_CURRENT */);
            if (pi) JO(b, M_NB_setContentIntent, pi);
        }
        jobject notif = JO(b, M_NB_build);
        // A tag names the slot: a second notification with it replaces the
        // first, as the registry's tag reuse does.
        int has_tag = n->tag && *n->tag;
        if (notif) JV(nm, M_NM_notify, has_tag ? aeui_jstring(env, n->tag) : (jstring)NULL,
                      (jint)(has_tag ? 0 : n->id), notif);
    }
    aeui_unframe(env);
}

static int aeui_post_notification(int id, const char* title, const char* body, const char* icon, const char* tag) {
    if (id <= 0 || aeui_is_headless()) return id;
    AeuiNotif n = { id, title, body, icon, tag };
    aeui_android_run_sync(aeui_notify_run, &n);
    return id;
}

int aether_ui_notify_impl(const char* title, const char* body) {
    int id = aether_ui_notify_register(title, body);
    return aeui_post_notification(id, title, body, NULL, NULL);
}
int aether_ui_notify_full_impl(const char* title, const char* body, const char* icon_path,
                               const char* tag, void* boxed_click) {
    int id = aether_ui_notify_register_full(title, body, icon_path ? icon_path : "",
                                            tag ? tag : "", boxed_click);
    return aeui_post_notification(id, title, body, icon_path, tag);
}

static void JNICALL native_notification_tap(JNIEnv* env, jclass cls, jint id) {
    (void)env; (void)cls;
    aether_ui_notif_emit_click((int)id);
}

// POST_NOTIFICATIONS is a runtime permission from API 33: 1 when granted;
// otherwise the system's permission dialog is asked for and 0 returned (the
// grant, if the user gives it, arrives later; the next call answers 1).
// Below 33, and headless, it is granted as the shared registry records.
typedef struct { int granted; } AeuiPerm;
static void aeui_perm_run(void* arg) {
    AeuiPerm* p = (AeuiPerm*)arg;
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return; }
    jstring perm = aeui_jstring(env, "android.permission.POST_NOTIFICATIONS");
    if (JI(g_activity, M_Ctx_checkSelfPermission, perm) == 0) {
        p->granted = 1;
    } else {
        jobjectArray arr = (*env)->NewObjectArray(env, 1, J.String, perm);
        if (arr) JV(g_activity, M_Act_requestPermissions, arr, (jint)0xAE02);
        p->granted = 0;
    }
    aeui_unframe(env);
}
int aether_ui_notify_request_permission_impl(void) {
    JNIEnv* env = aeui_env();
    if (aeui_is_headless() || !env || aeui_sdk_int(env) < 33) return aether_ui_notify_request_permission();
    AeuiPerm p = { 0 };
    aeui_android_run_sync(aeui_perm_run, &p);
    return p.granted;
}

// ===========================================================================
// CSS classes, weight, inline CSS, the census
// ===========================================================================

// Classes are the widget's own list (space-separated, no duplicates), which
// the stylesheet layer above the ABI matches and the driver reports.
static int aeui_has_class(const char* list, const char* cls) {
    size_t n = strlen(cls);
    for (const char* p = list; p && *p; ) {
        while (*p == ' ') p++;
        const char* e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, cls, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !cls || !*cls || strchr(cls, ' ') || aeui_has_class(w->classes, cls)) return;
    size_t old = w->classes ? strlen(w->classes) : 0;
    char* nc = (char*)malloc(old + strlen(cls) + 2);
    if (!nc) return;
    if (old) { memcpy(nc, w->classes, old); nc[old] = ' '; strcpy(nc + old + 1, cls); }
    else strcpy(nc, cls);
    free(w->classes);
    w->classes = nc;
}

void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    AeuiWidget* w = live_widget(handle);
    if (!w || !cls || !w->classes || !aeui_has_class(w->classes, cls)) return;
    char* out = (char*)malloc(strlen(w->classes) + 1);
    if (!out) return;
    out[0] = 0;
    char* dup = strdup(w->classes);
    char* save = NULL;
    for (char* t = strtok_r(dup, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        if (!strcmp(t, cls)) continue;
        if (out[0]) strcat(out, " ");
        strcat(out, t);
    }
    free(dup);
    free(w->classes);
    w->classes = out[0] ? out : (free(out), (char*)NULL);
}

const char* aether_ui_widget_classes_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->classes) ? w->classes : "";
}

static void hook_widget_classes_into(int handle, char* buf, int bufsize) {
    buf[0] = 0;
    AeuiWidget* w = live_widget(handle);
    if (w && w->classes) snprintf(buf, (size_t)bufsize, "%s", w->classes);
}

int aether_ui_widget_count_impl(void) { return widget_count; }

// A share of the stack's slack in proportion to n (a stated size stays as
// the floor: aeui_apply_lp); 0 takes the widget back to its natural size.
void aether_ui_widget_weight_impl(int handle, int n) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    w->weight = n > 0 ? n : 0;
    aeui_apply_lp(env, handle);
    aeui_unframe(env);
}

// "prop: value; ..." through the typed setters -- there is no stylesheet
// engine under Android Views, so a declaration is the setter it names (the
// UIKit backend's reading). Transitions travel this way too: "transition:
// opacity 300ms" makes the next set_opacity tween (ViewPropertyAnimator)
// instead of snapping, as AppKit honours it.
static int aeui_css_color(const char* v, double* r, double* g, double* b) {
    unsigned int rr, gg, bb;
    if (!v) return 0;
    if (v[0] == '#' && strlen(v) == 7 && sscanf(v + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3) {
        *r = rr / 255.0; *g = gg / 255.0; *b = bb / 255.0; return 1;
    }
    if (v[0] == '#' && strlen(v) == 4 && sscanf(v + 1, "%1x%1x%1x", &rr, &gg, &bb) == 3) {
        *r = rr / 15.0; *g = gg / 15.0; *b = bb / 15.0; return 1;
    }
    static const struct { const char* n; double r, g, b; } names[] = {
        { "white", 1, 1, 1 }, { "black", 0, 0, 0 }, { "red", 1, 0, 0 },
        { "green", 0, 0.5, 0 }, { "blue", 0, 0, 1 }, { "gray", 0.5, 0.5, 0.5 }, { "grey", 0.5, 0.5, 0.5 } };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(v, names[i].n)) { *r = names[i].r; *g = names[i].g; *b = names[i].b; return 1; }
    return 0;
}

static char* aeui_trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int* g_opacity_ms = NULL;   // by handle: a declared opacity transition (ms), 0 none
static int g_opacity_cap = 0;

static void aeui_apply_css_decl(int handle, const char* prop, const char* val) {
    double r, g, b;
    if ((!strcmp(prop, "background") || !strcmp(prop, "background-color")) && aeui_css_color(val, &r, &g, &b))
        aether_ui_set_bg_color(handle, r, g, b, 1.0);
    else if (!strcmp(prop, "color") && aeui_css_color(val, &r, &g, &b))
        aether_ui_set_text_color(handle, r, g, b);
    else if (!strcmp(prop, "opacity"))
        aether_ui_set_opacity(handle, atof(val));
    else if (!strcmp(prop, "border-radius") || !strcmp(prop, "corner-radius"))
        aether_ui_set_corner_radius(handle, atof(val));
    else if (!strcmp(prop, "font-size"))
        aether_ui_set_font_size(handle, atof(val));
    else if (!strcmp(prop, "font-weight"))
        aether_ui_set_font_bold(handle, strstr(val, "bold") ? 1 : 0);
    else if (!strcmp(prop, "font-family"))
        aether_ui_set_font_family(handle, val);
    else if (!strcmp(prop, "border")) {
        const char* hash = strchr(val, '#');
        if (hash && aeui_css_color(hash, &r, &g, &b)) aether_ui_set_border(handle, atof(val), r, g, b);
    } else if (!strcmp(prop, "transition") && strstr(val, "opacity")) {
        int ms = 0;
        const char* d = val + strcspn(val, "0123456789");
        if (*d) ms = atoi(d);
        if (handle > g_opacity_cap) {
            int cap = g_opacity_cap ? g_opacity_cap : 64;
            while (cap < handle) cap *= 2;
            int* n = (int*)realloc(g_opacity_ms, sizeof(int) * (size_t)cap);
            if (!n) return;
            memset(n + g_opacity_cap, 0, sizeof(int) * (size_t)(cap - g_opacity_cap));
            g_opacity_ms = n; g_opacity_cap = cap;
        }
        g_opacity_ms[handle - 1] = ms;
    }
}

static int aeui_opacity_transition_ms(int handle) {
    return (handle >= 1 && handle <= g_opacity_cap && !aeui_animations_off()) ? g_opacity_ms[handle - 1] : 0;
}

void aether_ui_widget_apply_css_impl(int handle, const char* property_css) {
    if (!property_css || !live_widget(handle)) return;
    char* dup = strdup(property_css);
    char* save = NULL;
    for (char* decl = strtok_r(dup, ";", &save); decl; decl = strtok_r(NULL, ";", &save)) {
        char* colon = strchr(decl, ':');
        if (!colon) continue;
        *colon = 0;
        aeui_apply_css_decl(handle, aeui_trim(decl), aeui_trim(colon + 1));
    }
    free(dup);
}

// ===========================================================================
// Sealing (the driver's "not automatable" mark), recursively by registry.
// ===========================================================================
void aether_ui_seal_widget_impl(int handle) {
    if (live_widget(handle)) aether_ui_test_server_seal_widget(handle);
}
void aether_ui_seal_subtree_impl(int handle) {
    if (!live_widget(handle)) return;
    aether_ui_seal_widget_impl(handle);
    for (int i = 0; i < widget_count; i++)
        if (widgets[i].parent == handle && widgets[i].view) aether_ui_seal_subtree_impl(i + 1);
}

// ===========================================================================
// Drag and drop, scrolling hooks
// ===========================================================================
JCLASS(C_ShadowBuilder, "android/view/View$DragShadowBuilder");
JMETHOD(M_SB2_init, C_ShadowBuilder, "<init>", "(Landroid/view/View;)V");
JMETHOD(M_View_startDragAndDrop, C_View, "startDragAndDrop",
        "(Landroid/content/ClipData;Landroid/view/View$DragShadowBuilder;Ljava/lang/Object;I)Z");

// A drag of plain text (a row's index, a file's path) with the view as its
// shadow. DRAG_FLAG_GLOBAL (256) lets a file path leave the app.
static void aeui_start_drag(JNIEnv* env, int handle, const char* label, const char* text, int global) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    jobject cd = JSO(M_CD_newPlainText, aeui_jstring(env, label), aeui_jstring(env, text));
    jobject sb = JNEW(M_SB2_init, w->view);
    if (cd && sb) JZ(w->view, M_View_startDragAndDrop, cd, sb, (jobject)NULL, (jint)(global ? 256 : 0));
}

// A reorderable row: a long press lifts it (its index travels as the
// drag's text), and a row it is dropped on runs its on_drop(src) -- the
// platform's own drag and drop, inside the list.
void aether_ui_row_drag_reorder_impl(int row_handle, int index, void* on_drop_closure) {
    AeuiWidget* w = live_widget(row_handle);
    if (!w) return;
    w->row_drop = (AeClosure*)on_drop_closure;
    w->row_index = index;
    JNIEnv* env = aeui_frame(8);
    if (!env) return;
    jobject drag = aeui_listener(env, row_handle, AEUI_EV_ROW_DRAG);
    jobject drop = aeui_listener(env, row_handle, AEUI_EV_ROW_DROP);
    if (drag) JV(w->view, M_View_setOnLongClickListener, drag);
    if (drop) JV(w->view, M_View_setOnDragListener, drop);
    aeui_unframe(env);
}

int aether_ui_fire_row_drop(int row_handle, int src_index) {
    AeuiWidget* w = live_widget(row_handle);
    AeClosure* c = w ? w->row_drop : NULL;
    if (!c || !c->fn) return 0;
    ((void (*)(void*, intptr_t))c->fn)(c->env, (intptr_t)src_index);
    return 1;
}

// A drag SOURCE carrying a file path: a long press drags the path out (to
// another app in split screen), as plain text -- Android shares files
// between apps as content URIs through a provider, which an app without one
// cannot mint, so the path is what travels. The driver reads the payload.
void aether_ui_widget_draggable_file_impl(int handle, const char* path) {
    AeuiWidget* w = live_widget(handle);
    if (!w) return;
    free(w->drag_path);
    w->drag_path = (path && *path) ? strdup(path) : NULL;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    jobject l = w->drag_path ? aeui_listener(env, handle, AEUI_EV_FILE_DRAG) : NULL;
    JV(w->view, M_View_setOnLongClickListener, l);
    aeui_unframe(env);
}
const char* aether_ui_widget_drag_payload_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    return (w && w->drag_path) ? w->drag_path : "";
}

// Files dropped on the window: the activity's host frame takes drops (from
// another app in split screen, or a desktop-mode file manager), and the
// listener turns each item into a path (see AetherListener.onDrag).
static AeClosure* g_file_drop = NULL;
void aether_ui_window_on_file_drop_impl(void* boxed_closure) {
    g_file_drop = (AeClosure*)boxed_closure;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    jobject l = g_host ? aeui_listener(env, 0, AEUI_EV_FILE_DROP) : NULL;
    if (l) JV(g_host, M_View_setOnDragListener, l);
    aeui_unframe(env);
}
int aether_ui_window_file_drop_deliver(const char* paths) {
    AeClosure* c = g_file_drop;
    if (!c || !c->fn) return 0;
    ((void (*)(void*, const char*))c->fn)(c->env, paths ? paths : "");
    return 1;
}

// A composed vlist's native scroll: a mouse wheel or a trackpad's two-finger
// scroll over the container, in rows (AetherListener WHEEL). A native list
// (below) scrolls itself.
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll) {
    AeuiWidget* w = live_widget(container_handle);
    if (!w) return;
    w->scroll_cb = (AeClosure*)on_scroll;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    jobject l = aeui_listener(env, container_handle, AEUI_EV_WHEEL);
    if (l) JV(w->view, M_View_setOnGenericMotionListener, l);
    aeui_unframe(env);
}
int aether_ui_fire_scroll(int container_handle, int dy) {
    AeuiWidget* w = live_widget(container_handle);
    AeClosure* c = w ? w->scroll_cb : NULL;
    if (!c || !c->fn) return 0;
    ((void (*)(void*, intptr_t))c->fn)(c->env, (intptr_t)dy);
    return 1;
}

// ===========================================================================
// Native list -- a ListView over AetherListAdapter. The ListView asks for
// the rows its viewport shows; each is a registered container the app's row
// closure fills (so the driver sees rows exactly as in a composed list), and
// a row it scraps leaves the registry. Rows are 24 dp, the viewport
// window_rows of them, as AppKit's table sizes itself. Vertical only: a
// horizontal list keeps the DSL's composed path, as on AppKit.
// ===========================================================================
JCLASS(C_ListView, "android/widget/ListView");
JMETHOD(M_LV_init, C_ListView, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_LV_setAdapter, C_ListView, "setAdapter", "(Landroid/widget/ListAdapter;)V");
JMETHOD(M_LV_setRecyclerListener, C_ListView, "setRecyclerListener", "(Landroid/widget/AbsListView$RecyclerListener;)V");
JMETHOD(M_LV_setSelectionFromTop, C_ListView, "setSelectionFromTop", "(II)V");
JMETHOD(M_LV_getFirstVisiblePosition, C_ListView, "getFirstVisiblePosition", "()I");
JMETHOD(M_LV_setDividerHeight, C_ListView, "setDividerHeight", "(I)V");
static jclass g_adapter_class = NULL;
static jmethodID g_adapter_init = NULL, g_adapter_setCount = NULL;

typedef struct { int handle; jobject adapter; AeClosure* builder; int count; } AeuiList;
static AeuiList* lists = NULL;
static int nlists = 0;
static AeuiList* list_of(int handle) {
    for (int i = 0; i < nlists; i++) if (lists[i].handle == handle) return &lists[i];
    return NULL;
}

int aether_ui_native_list_available_impl(void) { return g_adapter_init ? 1 : 0; }

int aether_ui_native_list_create_impl(int horizontal, int window_rows) {
    if (horizontal || !g_adapter_init) return 0;
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return 0; }
    int h = 0;
    jobject lv = JNEW(M_LV_init, g_activity);
    AeuiList* nl = (AeuiList*)realloc(lists, sizeof(AeuiList) * (size_t)(nlists + 1));
    if (lv && nl) {
        lists = nl;
        h = register_widget_typed(env, lv, AUI_LIST);
        jobject ad = (*env)->NewObject(env, g_adapter_class, g_adapter_init, (jint)h);
        if (!aeui_check(env, "new AetherListAdapter") && ad) {
            JV(lv, M_LV_setDividerHeight, (jint)0);
            JV(lv, M_LV_setAdapter, ad);
            JV(lv, M_LV_setRecyclerListener, ad);
            AeuiList* l = &lists[nlists++];
            l->handle = h;
            l->adapter = (*env)->NewGlobalRef(env, ad);
            l->builder = NULL;
            l->count = 0;
            AeuiWidget* w = widget_at(h);
            w->fixed_h = 24 * (window_rows > 0 ? window_rows : 10);
            w->own_hexp = w->hexp = 1;
        }
    }
    aeui_unframe(env);
    return h;
}

void aether_ui_native_list_set_row_builder_impl(int handle, void* builder) {
    AeuiList* l = list_of(handle);
    if (l) l->builder = (AeClosure*)builder;
}

void aether_ui_native_list_set_count_impl(int handle, int count) {
    AeuiList* l = list_of(handle);
    if (!l) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    l->count = count < 0 ? 0 : count;
    (*env)->CallVoidMethod(env, l->adapter, g_adapter_setCount, (jint)l->count);
    aeui_check(env, "AetherListAdapter.setCount");
    aeui_unframe(env);
}

// Lay the list out now at the size it has, so the rows for its new position
// are realized (and the old ones scrapped) before the caller reads it back.
static void aeui_list_settle(JNIEnv* env, AeuiWidget* w) {
    int l = JI(w->view, M_View_getLeft), t = JI(w->view, M_View_getTop);
    int r = JI(w->view, M_View_getRight), b = JI(w->view, M_View_getBottom);
    if (r <= l || b <= t) return;
    JV(w->view, M_View_measure, (jint)(0x40000000 | (r - l)), (jint)(0x40000000 | (b - t)));
    JV(w->view, M_View_layout, (jint)l, (jint)t, (jint)r, (jint)b);
}

// The row at the top of the viewport (vlist_scroll_to names the first row).
void aether_ui_native_list_scroll_to_impl(int handle, int index) {
    AeuiList* lst = list_of(handle);
    AeuiWidget* w = live_widget(handle);
    if (!lst || !w || lst->count <= 0) return;
    JNIEnv* env = aeui_frame(16);
    if (!env) return;
    int i = index < 0 ? 0 : index >= lst->count ? lst->count - 1 : index;
    JV(w->view, M_LV_setSelectionFromTop, (jint)i, (jint)0);
    aeui_list_settle(env, w);
    aeui_unframe(env);
}

int aether_ui_native_list_first_visible_impl(int handle) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_LIST) return 0;
    JNIEnv* env = aeui_frame(4);
    if (!env) return 0;
    int f = JI(w->view, M_LV_getFirstVisiblePosition);
    aeui_unframe(env);
    return f;
}

// AetherListAdapter.getView: build row `position` into a fresh container.
static jobject JNICALL native_list_row(JNIEnv* env, jclass cls, jint handle, jint position) {
    (void)cls;
    AeuiList* l = list_of(handle);
    if (!l || !live_widget(handle)) return NULL;
    int row = aether_ui_vstack_create(0);
    AeuiWidget* rw = live_widget(row);
    if (!rw) return NULL;
    rw->parent = handle;   // a row of the list, for the driver and for retiring
    aeui_apply_lp(env, row);
    AeClosure* b = l->builder;
    if (b && b->fn) ((void (*)(void*, intptr_t, intptr_t))b->fn)(b->env, (intptr_t)position, (intptr_t)row);
    rw = live_widget(row);
    return rw ? (*env)->NewLocalRef(env, rw->view) : NULL;
}

// A row the list let go of: out of the registry with everything in it.
static void JNICALL native_list_scrap(JNIEnv* env, jclass cls, jint handle, jobject view) {
    (void)cls; (void)handle;
    int row = aether_ui_handle_for_widget(view);
    if (row) aeui_retire_tree(env, row);
}

// ===========================================================================
// Native view -- a SurfaceView. Its handle is the ANativeWindow* of its
// Surface (kind 5, ANativeWindow), what an engine makes a Vulkan surface
// (VK_KHR_android_surface) or an EGL window surface from. Realize and resize
// report the Surface's size in pixels, from SurfaceHolder.Callback.
// ===========================================================================
#include <android/native_window_jni.h>
JCLASS(C_SurfaceView, "android/view/SurfaceView");
JCLASS(C_SurfaceHolder, "android/view/SurfaceHolder");
JMETHOD(M_SV2_init, C_SurfaceView, "<init>", "(Landroid/content/Context;)V");
JMETHOD(M_SV2_getHolder, C_SurfaceView, "getHolder", "()Landroid/view/SurfaceHolder;");
JMETHOD(M_SH_addCallback, C_SurfaceHolder, "addCallback", "(Landroid/view/SurfaceHolder$Callback;)V");
JMETHOD(M_SH_getSurface, C_SurfaceHolder, "getSurface", "()Landroid/view/Surface;");

typedef struct {
    int widget;
    ANativeWindow* window;
    AeClosure* on_realize; AeClosure* on_resize;
    int realized, w, h;
} AeuiNativeView;
static AeuiNativeView* nviews = NULL;
static int nnviews = 0;
static AeuiNativeView* nview_at(int id) { return (id >= 1 && id <= nnviews) ? &nviews[id - 1] : NULL; }

int aether_ui_native_view_available_impl(void) { return 1; }
int aether_ui_native_view_kind_impl(void) { return 5; }

int aether_ui_native_view_create_impl(int width, int height) {
    JNIEnv* env = aeui_frame(16);
    if (!env || !g_activity) { if (env) aeui_unframe(env); return 0; }
    int id = 0;
    jobject sv = JNEW(M_SV2_init, g_activity);
    AeuiNativeView* nn = (AeuiNativeView*)realloc(nviews, sizeof(AeuiNativeView) * (size_t)(nnviews + 1));
    if (sv && nn) {
        nviews = nn;
        int h = register_widget_typed(env, sv, AUI_NATIVE_VIEW);
        AeuiNativeView* v = &nviews[nnviews++];
        memset(v, 0, sizeof(*v));
        v->widget = h;
        id = nnviews;
        // The size asked for is where the panel starts, a floor, not a cage:
        // fill_width/fill_height grow it with the window, as an engine's
        // viewport does.
        if (width > 0) aether_ui_set_min_width_impl(h, width);
        if (height > 0) aether_ui_set_min_height_impl(h, height);
        jobject holder = JO(sv, M_SV2_getHolder);
        jobject l = aeui_listener(env, h, AEUI_EV_SURFACE);
        if (holder && l) JV(holder, M_SH_addCallback, l);
    }
    aeui_unframe(env);
    return id;
}

int aether_ui_native_view_get_widget(int view_id) {
    AeuiNativeView* v = nview_at(view_id);
    return v ? v->widget : 0;
}
void* aether_ui_native_view_handle_impl(int view_id) {
    AeuiNativeView* v = nview_at(view_id);
    return v ? (void*)v->window : NULL;
}
void aether_ui_native_view_on_realize_impl(int view_id, void* boxed_closure) {
    AeuiNativeView* v = nview_at(view_id);
    if (!v) return;
    v->on_realize = (AeClosure*)boxed_closure;
    // Already real: report it now, as a realize hook installed late expects.
    if (v->realized && v->on_realize && v->on_realize->fn)
        ((void (*)(void*, intptr_t, intptr_t))v->on_realize->fn)(v->on_realize->env, (intptr_t)v->w, (intptr_t)v->h);
}
void aether_ui_native_view_on_resize_impl(int view_id, void* boxed_closure) {
    AeuiNativeView* v = nview_at(view_id);
    if (v) v->on_resize = (AeClosure*)boxed_closure;
}

static void aeui_native_view_surface(JNIEnv* env, int widget, int w, int h) {
    AeuiNativeView* v = NULL;
    for (int i = 0; i < nnviews; i++) if (nviews[i].widget == widget) v = &nviews[i];
    AeuiWidget* sw = live_widget(widget);
    if (!v || !sw) return;
    if (w <= 0 || h <= 0) {   // the Surface is going: its window with it
        if (v->window) { ANativeWindow_release(v->window); v->window = NULL; }
        return;
    }
    if (!v->window) {
        jobject holder = JO(sw->view, M_SV2_getHolder);
        jobject surface = holder ? JO(holder, M_SH_getSurface) : NULL;
        if (surface) v->window = ANativeWindow_fromSurface(env, surface);
    }
    int first = !v->realized;
    if (!first && w == v->w && h == v->h) return;
    v->realized = 1;
    v->w = w; v->h = h;
    AeClosure* c = first ? v->on_realize : v->on_resize;
    if (c && c->fn) ((void (*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)w, (intptr_t)h);
}

// ===========================================================================
// Canvas -- a command buffer replayed through android.graphics.Canvas.
//
// The same shape as every other backend's canvas: the drawing calls append
// commands to a buffer, and the buffer is REPLAYED onto a platform drawing
// surface whenever something needs pixels. Here the surface is an
// android.graphics.Canvas (Skia) over a Bitmap, which is Android's
// counterpart of cairo on GTK4 and Core Graphics on AppKit/UIKit: the
// backend translates each command into the platform's own path, paint,
// shader and text calls and never rasterises anything itself.
//
// Three consumers replay the buffer, all through canvas_replay_range:
//   * the View (dev.aether.ui.AetherCanvas). onDraw replays into a Bitmap
//     the size of the View in device pixels, scaled by the density so the
//     canvas's units are dp like every other length here, and shows it.
//     That Bitmap is the RETAINED paint surface (GTK4's paint_surface):
//     painted_pixels samples it, and a dirty-region paint (set_clip_rects)
//     redraws only its clip;
//   * read_pixel, write_png and render_range_rgba, which replay offscreen at
//     one pixel per canvas unit, the size the caller names -- what GTK4,
//     AppKit and UIKit do -- so a spec's pixel coordinates mean the same on
//     all of them;
//   * the headless snapshot path the others have is not needed: a phone
//     always has the activity's window.
//
// Text is drawn with the platform's text renderer through a Paint, in the
// typeface the CSS font-family stack resolves to (Typeface.create walks the
// system font map, as fontconfig and CoreText do on the desktop); with no
// family it is Typeface.DEFAULT, the face text_measure and the font metrics
// report for text widgets, so a vg label anchored by measured width lands
// where the measurement says.
//
// Input arrives through AetherCanvas's natives: the touch stream (press =
// on_click, move = on_move, release = on_release, as UIKit's touches map),
// a mouse's hover and wheel, hardware keys while the canvas has focus, and
// size changes (on_resize). The gesture probe sees the same stream.
// ===========================================================================
#include <android/bitmap.h>

typedef enum {
    CANVAS_BEGIN_PATH, CANVAS_MOVE_TO, CANVAS_LINE_TO, CANVAS_STROKE, CANVAS_FILL_RECT,
    CANVAS_CLEAR, CANVAS_ARC, CANVAS_CLOSE_PATH, CANVAS_FILL, CANVAS_FILL_TEXT,
    CANVAS_STROKE_TEXT, CANVAS_DRAW_IMAGE, CANVAS_FILL_LINEAR, CANVAS_FILL_RADIAL,
    CANVAS_CLIP_RECT, CANVAS_GROUP_BEGIN, CANVAS_GROUP_END, CANVAS_RESET_CLIP,
    CANVAS_CLIP_PATH,  // clip to the current path (iw = even-odd), consuming it
    /* Image smoothing for the DRAW_IMAGEs after it: x = 1 smoothed (the
       default), 0 nearest (vg image_rendering "pixelated"). State, like the
       web canvas's imageSmoothingEnabled; appended, the values are positional. */
    CANVAS_IMAGE_SMOOTHING
} CanvasCmdType;

typedef struct {
    CanvasCmdType type;
    double x, y;            // points; STROKE line width in x; GROUP_END alpha in x
    double r, g, b, a;      // colour
    double w, h;            // rect size; ARC radius in w; text size in w, stroke width in h;
                            // DRAW_IMAGE destination extent (0 = the pixel size)
    double a0, a1;          // ARC start/end angle (radians)
    char* text;             // FILL_TEXT / STROKE_TEXT (owned)
    char* font_family;      // the raw CSS stack (owned), NULL = none declared
    unsigned char* pixels;  // DRAW_IMAGE RGBA8888, straight alpha
    int pixels_borrowed;    // 1 = the caller's, valid until the next clear
    jobject bitmap;         // global ref: the owned pixels as a Bitmap, made on first replay
    int iw, ih;             // DRAW_IMAGE pixel size; STROKE/gradient cap & join; FILL even-odd;
                            // text font flags (iw: bit0 mono, bit1 bold, bit2 italic)
    double gx1, gy1, gx2, gy2, gr, gfx, gfy;   // gradient geometry
    double grad_line_width; // 0 = fill the path, > 0 = stroke it this wide
    int grad_extend;        // spreadMethod: 0 pad, 1 reflect, 2 repeat
    double grx, gry, grot;  // a radial gradient's ellipse (grx == 0: a circle of gr)
    int n_stops;
    double* stop_off;       // owned
    double* stop_rgba;      // owned, 4 per stop, 0..1
} CanvasCmd;

typedef struct {
    CanvasCmd* cmds;
    int count, capacity;
    int widget_handle;
    AeClosure* on_click;      // press (x, y) in canvas units
    AeClosure* on_move;       // pointer move (x, y)
    AeClosure* on_release;    // release (x, y)
    AeClosure* on_right_click;  // secondary click / long press (x, y)
    AeClosure* on_double_click; // double tap (x, y)
    AeClosure* on_key;        // key down (name)
    AeClosure* on_key_release;
    AeClosure* on_resize;     // (w, h) on a change of size
    AeClosure* on_scroll;     // wheel (dx, dy), dy < 0 away from the user
    AeClosure* probe;         // gesture probe (kind, a, b, mods)
    int last_w, last_h;       // the size on_resize last reported (dp), -1 none yet
    int created_w, created_h;
    // Dirty-region paint (set_clip_rects) and the paint metrics the driver's
    // /canvas/{id}/debug reports, as GTK4, AppKit and UIKit keep them.
    double* paint_clip_rects; int paint_clip_count, paint_clip_capacity;
    int last_paint_w, last_paint_h, last_paint_area, last_paint_count;
    int paint_full_count, paint_clip_count_total, last_clip_area;
    // The retained paint surface: what the View shows, in device pixels.
    jobject paint_bmp, paint_canvas;
    int paint_w, paint_h;
    // read_pixel's replay cache (GTK4's): one rendered buffer per
    // (generation, command count, size). The generation moves on clear;
    // within one the buffer only grows, so the pair names its content. A
    // golden signature reads 1536 pixels; without this each would replay
    // the whole scene.
    unsigned long gen, cache_gen;
    int cache_count, cache_w, cache_h;
    unsigned char* cache_px;  // premultiplied RGBA, cache_w * cache_h * 4
    // The gesture probe's view of the touch stream.
    int g_down; double g_x0, g_y0;
    int g_two; double g_span0, g_ang0;
    int keys;                 // focusable for keys (a key handler is set)
} CanvasState;

static CanvasState* canvas_states = NULL;
static int canvas_state_count = 0;
static int canvas_state_capacity = 0;

extern double floatarr_get_raw(void* arr, int i);

static jclass g_canvas_class = NULL;
static jmethodID g_canvas_init = NULL;

static CanvasState* get_canvas_state(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_state_count) return NULL;
    return &canvas_states[canvas_id - 1];
}

static void canvas_add_cmd(int canvas_id, CanvasCmd cmd) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) {
        free(cmd.text); free(cmd.font_family);
        if (!cmd.pixels_borrowed) free(cmd.pixels);
        free(cmd.stop_off); free(cmd.stop_rgba);
        return;
    }
    if (cs->count >= cs->capacity) {
        int cap = cs->capacity == 0 ? 64 : cs->capacity * 2;
        CanvasCmd* nc = (CanvasCmd*)realloc(cs->cmds, sizeof(CanvasCmd) * (size_t)cap);
        if (!nc) return;
        cs->cmds = nc;
        cs->capacity = cap;
    }
    cs->cmds[cs->count++] = cmd;
}

// --- The platform's drawing API -----------------------------------------------
JCLASS(C_GCanvas, "android/graphics/Canvas");
JCLASS(C_GPath, "android/graphics/Path");
JCLASS(C_GPaint, "android/graphics/Paint");
JCLASS(C_FillType, "android/graphics/Path$FillType");
JCLASS(C_PathDir, "android/graphics/Path$Direction");
JCLASS(C_PaintStyle, "android/graphics/Paint$Style");
JCLASS(C_PaintCap, "android/graphics/Paint$Cap");
JCLASS(C_PaintJoin, "android/graphics/Paint$Join");
JCLASS(C_Shader, "android/graphics/Shader");
JCLASS(C_LinearGradient, "android/graphics/LinearGradient");
JCLASS(C_RadialGradient, "android/graphics/RadialGradient");
JCLASS(C_Matrix, "android/graphics/Matrix");
JCLASS(C_RectF, "android/graphics/RectF");
JCLASS(C_GBitmap, "android/graphics/Bitmap");
JCLASS(C_PorterDuffMode, "android/graphics/PorterDuff$Mode");
JCLASS(C_Typeface2, "android/graphics/Typeface");
JCLASS(C_FileOutputStream, "java/io/FileOutputStream");

JMETHOD(M_GC_init, C_GCanvas, "<init>", "(Landroid/graphics/Bitmap;)V");
JMETHOD(M_GC_setDensity, C_GCanvas, "setDensity", "(I)V");
JMETHOD(M_GC_save, C_GCanvas, "save", "()I");
JMETHOD(M_GC_restoreToCount, C_GCanvas, "restoreToCount", "(I)V");
JMETHOD(M_GC_saveLayerAlpha, C_GCanvas, "saveLayerAlpha", "(Landroid/graphics/RectF;I)I");
JMETHOD(M_GC_clipRect, C_GCanvas, "clipRect", "(FFFF)Z");
JMETHOD(M_GC_clipPath, C_GCanvas, "clipPath", "(Landroid/graphics/Path;)Z");
JMETHOD(M_GC_translate, C_GCanvas, "translate", "(FF)V");
JMETHOD(M_GC_scale, C_GCanvas, "scale", "(FF)V");
JMETHOD(M_GC_drawPath, C_GCanvas, "drawPath", "(Landroid/graphics/Path;Landroid/graphics/Paint;)V");
JMETHOD(M_GC_drawRect, C_GCanvas, "drawRect", "(FFFFLandroid/graphics/Paint;)V");
JMETHOD(M_GC_drawText, C_GCanvas, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
JMETHOD(M_GC_drawBitmapRect, C_GCanvas, "drawBitmap",
        "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;Landroid/graphics/RectF;Landroid/graphics/Paint;)V");
JMETHOD(M_GC_drawBitmapAt, C_GCanvas, "drawBitmap", "(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V");
JMETHOD(M_GC_drawColorMode, C_GCanvas, "drawColor", "(ILandroid/graphics/PorterDuff$Mode;)V");
JMETHOD(M_GP_init, C_GPath, "<init>", "()V");
JMETHOD(M_GP_reset, C_GPath, "reset", "()V");
JMETHOD(M_GP_moveTo, C_GPath, "moveTo", "(FF)V");
JMETHOD(M_GP_lineTo, C_GPath, "lineTo", "(FF)V");
JMETHOD(M_GP_close, C_GPath, "close", "()V");
JMETHOD(M_GP_arcTo, C_GPath, "arcTo", "(FFFFFFZ)V");
JMETHOD(M_GP_setFillType, C_GPath, "setFillType", "(Landroid/graphics/Path$FillType;)V");
JMETHOD(M_GP_addRect, C_GPath, "addRect", "(FFFFLandroid/graphics/Path$Direction;)V");
JSFIELD(F_FT_WINDING, C_FillType, "WINDING", "Landroid/graphics/Path$FillType;");
JSFIELD(F_FT_EVEN_ODD, C_FillType, "EVEN_ODD", "Landroid/graphics/Path$FillType;");
JSFIELD(F_PD_CW, C_PathDir, "CW", "Landroid/graphics/Path$Direction;");
JMETHOD(M_GPt_init, C_GPaint, "<init>", "(I)V");
JMETHOD(M_GPt_setColor, C_GPaint, "setColor", "(I)V");
JMETHOD(M_GPt_setStyle, C_GPaint, "setStyle", "(Landroid/graphics/Paint$Style;)V");
JMETHOD(M_GPt_setStrokeWidth, C_GPaint, "setStrokeWidth", "(F)V");
JMETHOD(M_GPt_setStrokeCap, C_GPaint, "setStrokeCap", "(Landroid/graphics/Paint$Cap;)V");
JMETHOD(M_GPt_setStrokeJoin, C_GPaint, "setStrokeJoin", "(Landroid/graphics/Paint$Join;)V");
JMETHOD(M_GPt_setStrokeMiter, C_GPaint, "setStrokeMiter", "(F)V");
JMETHOD(M_GPt_setFilterBitmap, C_GPaint, "setFilterBitmap", "(Z)V");
JMETHOD(M_GPt_setShader, C_GPaint, "setShader", "(Landroid/graphics/Shader;)Landroid/graphics/Shader;");
JMETHOD(M_GPt_setTypeface, C_GPaint, "setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
JMETHOD(M_GPt_setTextSize, C_GPaint, "setTextSize", "(F)V");
JSFIELD(F_PS_FILL, C_PaintStyle, "FILL", "Landroid/graphics/Paint$Style;");
JSFIELD(F_PS_STROKE, C_PaintStyle, "STROKE", "Landroid/graphics/Paint$Style;");
JSFIELD(F_PC_BUTT, C_PaintCap, "BUTT", "Landroid/graphics/Paint$Cap;");
JSFIELD(F_PC_ROUND, C_PaintCap, "ROUND", "Landroid/graphics/Paint$Cap;");
JSFIELD(F_PC_SQUARE, C_PaintCap, "SQUARE", "Landroid/graphics/Paint$Cap;");
JSFIELD(F_PJ_MITER, C_PaintJoin, "MITER", "Landroid/graphics/Paint$Join;");
JSFIELD(F_PJ_ROUND, C_PaintJoin, "ROUND", "Landroid/graphics/Paint$Join;");
JSFIELD(F_PJ_BEVEL, C_PaintJoin, "BEVEL", "Landroid/graphics/Paint$Join;");
JSFIELD(F_TM_MIRROR, C_TileMode, "MIRROR", "Landroid/graphics/Shader$TileMode;");
JSFIELD(F_TM_REPEAT, C_TileMode, "REPEAT", "Landroid/graphics/Shader$TileMode;");
JSFIELD(F_PDM_CLEAR, C_PorterDuffMode, "CLEAR", "Landroid/graphics/PorterDuff$Mode;");
JMETHOD(M_LG_init, C_LinearGradient, "<init>", "(FFFF[I[FLandroid/graphics/Shader$TileMode;)V");
JMETHOD(M_RG_init, C_RadialGradient, "<init>", "(FFF[I[FLandroid/graphics/Shader$TileMode;)V");
JMETHOD(M_RG_init2pt, C_RadialGradient, "<init>", "(FFFFFF[J[FLandroid/graphics/Shader$TileMode;)V");
JMETHOD(M_Shader_setLocalMatrix, C_Shader, "setLocalMatrix", "(Landroid/graphics/Matrix;)V");
JMETHOD(M_Mx_init, C_Matrix, "<init>", "()V");
JMETHOD(M_Mx_setTranslate, C_Matrix, "setTranslate", "(FF)V");
JMETHOD(M_Mx_preRotate, C_Matrix, "preRotate", "(F)Z");
JMETHOD(M_Mx_preScale, C_Matrix, "preScale", "(FF)Z");
JMETHOD(M_RF_init, C_RectF, "<init>", "(FFFF)V");
JMETHOD(M_GB_setDensity, C_GBitmap, "setDensity", "(I)V");
JMETHOD(M_GB_eraseColor, C_GBitmap, "eraseColor", "(I)V");
JMETHOD(M_GB_recycle, C_GBitmap, "recycle", "()V");
JMETHOD(M_GB_compress, C_GBitmap, "compress",
        "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z");
JSTATIC(M_TF_create2, C_Typeface2, "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;");
JSTATIC(M_TF_default2, C_Typeface2, "defaultFromStyle", "(I)Landroid/graphics/Typeface;");
JMETHOD(M_Obj_equals, C_Typeface2, "equals", "(Ljava/lang/Object;)Z");
JMETHOD(M_FOS_init, C_FileOutputStream, "<init>", "(Ljava/lang/String;)V");
JMETHOD(M_FOS_close, C_FileOutputStream, "close", "()V");
JMETHOD(M_View_invalidate, C_View, "invalidate", "()V");
JMETHOD(M_View_postInvalidate, C_View, "postInvalidate", "()V");

// A colour component 0..1 as the 0..255 the platform's 8-bit ARGB takes,
// rounded (cairo and Core Graphics round too; truncating darkens by one).
static unsigned int cv_c8(double v) {
    if (!(v > 0.0)) return 0;
    if (v >= 1.0) return 255;
    return (unsigned int)lrint(v * 255.0);
}
static jint cv_argb(double r, double g, double b, double a) {
    return (jint)((cv_c8(a) << 24) | (cv_c8(r) << 16) | (cv_c8(g) << 8) | cv_c8(b));
}

// A Bitmap of w x h, ARGB_8888 (premultiplied), cleared, and with no
// density: drawBitmap must not rescale it for a screen it was not made for.
static jobject cv_new_bitmap(JNIEnv* env, int w, int h) {
    jobject bmp = (*env)->CallStaticObjectMethod(env, J.Bitmap, J.Bitmap_createBitmap,
                                                 (jint)w, (jint)h, J.ARGB_8888);
    if (aeui_check(env, "Bitmap.createBitmap") || !bmp) return NULL;
    JV(bmp, M_GB_setDensity, (jint)0 /* DENSITY_NONE */);
    return bmp;
}

static jobject cv_new_canvas(JNIEnv* env, jobject bmp) {
    jobject c = JNEW(M_GC_init, bmp);
    if (c) JV(c, M_GC_setDensity, (jint)0);
    return c;
}

// An image command's pixels as a Bitmap: straight RGBA in, the platform's
// premultiplied RGBA_8888 out (byte order R, G, B, A on every Android ABI).
static jobject cv_image_bitmap(JNIEnv* env, const CanvasCmd* c) {
    jobject bmp = cv_new_bitmap(env, c->iw, c->ih);
    if (!bmp) return NULL;
    AndroidBitmapInfo info;
    void* px = NULL;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_lockPixels(env, bmp, &px) != ANDROID_BITMAP_RESULT_SUCCESS || !px) {
        (*env)->DeleteLocalRef(env, bmp);
        return NULL;
    }
    for (int y = 0; y < c->ih; y++) {
        const unsigned char* s = c->pixels + (size_t)y * (size_t)c->iw * 4;
        unsigned char* d = (unsigned char*)px + (size_t)y * info.stride;
        for (int x = 0; x < c->iw; x++, s += 4, d += 4) {
            unsigned int a = s[3];
            d[0] = (unsigned char)((s[0] * a + 127) / 255);
            d[1] = (unsigned char)((s[1] * a + 127) / 255);
            d[2] = (unsigned char)((s[2] * a + 127) / 255);
            d[3] = (unsigned char)a;
        }
    }
    AndroidBitmap_unlockPixels(env, bmp);
    return bmp;
}

// --- Fonts: the CSS stack through the platform's font map -----------------------
// font_flags: bit0 monospace, bit1 bold, bit2 italic. Typeface.create(name,
// style) answers the system font map's family for a name it knows and the
// default family for one it does not, so a name whose answer IS the default
// is passed over and the next in the stack tried, as fontconfig and CoreText
// skip a face that is not installed. The generic families are the platform's
// own aliases ("serif", "sans-serif", "monospace", "cursive", ...). The
// result is cached per (stack, style): a scene draws the same few faces over
// and over.
typedef struct { char* key; jobject tf; } CvFont;
static CvFont cv_fonts[64];
static int cv_nfonts = 0;

static int cv_is_generic(const char* n) {
    static const char* gen[] = { "serif", "sans-serif", "monospace", "cursive", "fantasy",
                                 "system-ui", "serif-monospace", "casual", NULL };
    for (int i = 0; gen[i]; i++) if (strcasecmp(n, gen[i]) == 0) return 1;
    return 0;
}

static jobject cv_typeface(JNIEnv* env, int flags, const char* family) {
    int style = ((flags & 2) ? 1 : 0) | ((flags & 4) ? 2 : 0);   // Typeface.BOLD / ITALIC
    char key[300];
    snprintf(key, sizeof(key), "%d|%s", flags, family ? family : "");
    for (int i = 0; i < cv_nfonts; i++)
        if (strcmp(cv_fonts[i].key, key) == 0) return cv_fonts[i].tf;
    jobject fallback = JSO(M_TF_default2, (jint)style);
    jobject chosen = NULL;
    if (family && family[0]) {
        const char* p = family;
        while (*p && !chosen) {
            const char* comma = strchr(p, ',');
            size_t n = comma ? (size_t)(comma - p) : strlen(p);
            char name[128];
            size_t k = 0;
            for (size_t i = 0; i < n && k < sizeof(name) - 1; i++) {
                char ch = p[i];
                if (ch == '"' || ch == '\'') continue;
                if (k == 0 && (ch == ' ' || ch == '\t')) continue;
                name[k++] = ch;
            }
            while (k > 0 && (name[k - 1] == ' ' || name[k - 1] == '\t')) k--;
            name[k] = '\0';
            if (k > 0) {
                jstring js = aeui_jstring(env, name);
                jobject tf = js ? JSO(M_TF_create2, js, (jint)style) : NULL;
                if (tf && (cv_is_generic(name) || !fallback || !JZ(tf, M_Obj_equals, fallback)))
                    chosen = tf;
            }
            p = comma ? comma + 1 : p + n;
        }
    }
    if (!chosen && (flags & 1)) {
        jstring js = aeui_jstring(env, "monospace");
        chosen = js ? JSO(M_TF_create2, js, (jint)style) : NULL;
    }
    if (!chosen) chosen = fallback;
    if (!chosen) return NULL;
    jobject g = (*env)->NewGlobalRef(env, chosen);
    if (cv_nfonts < (int)(sizeof(cv_fonts) / sizeof(cv_fonts[0]))) {
        cv_fonts[cv_nfonts].key = strdup(key);
        cv_fonts[cv_nfonts].tf = g;
        cv_nfonts++;
    }
    return g;
}

// --- The replay -----------------------------------------------------------------
// One replay's working objects. The path is the CURRENT path, which is not
// part of the canvas's saved state on Android any more than on cairo, so it
// lives here; `cur` says whether it has a current point (cairo's notion,
// which Skia lacks: a lineTo on an empty Skia path starts from (0, 0), a
// cairo line_to with no current point is a move_to).
typedef struct {
    JNIEnv* env;
    jobject canvas, path, paint;
    jobject s_fill, s_stroke;
    int cur;
    int bases[64]; int nbases;   // save counts RESET_CLIP restores to
    int layers[64]; int nlayers; // saveLayerAlpha counts GROUP_END restores to
    int smooth;                  // CANVAS_IMAGE_SMOOTHING in force (1 = filtered)
} CvReplay;

static void cv_path_reset(CvReplay* r) {
    JNIEnv* env = r->env;
    JV(r->path, M_GP_reset);
    r->cur = 0;
}

static jobject cv_cap(JNIEnv* env, int cap) {
    return JSFO(*(cap == 1 ? &F_PC_ROUND : cap == 2 ? &F_PC_SQUARE : &F_PC_BUTT));
}
static jobject cv_join(JNIEnv* env, int join) {
    return JSFO(*(join == 1 ? &F_PJ_ROUND : join == 2 ? &F_PJ_BEVEL : &F_PJ_MITER));
}

static void cv_set_stroke(CvReplay* r, double width, int cap, int join) {
    JNIEnv* env = r->env;
    JV(r->paint, M_GPt_setStyle, r->s_stroke);
    JV(r->paint, M_GPt_setStrokeWidth, (jfloat)width);
    jobject c = cv_cap(env, cap), j = cv_join(env, join);
    JV(r->paint, M_GPt_setStrokeCap, c);
    JV(r->paint, M_GPt_setStrokeJoin, j);
    if (c) (*env)->DeleteLocalRef(env, c);
    if (j) (*env)->DeleteLocalRef(env, j);
}

// The fill rule rides on the path in Skia; cairo's default (and every fill
// that does not ask) is nonzero.
static void cv_fill_type(CvReplay* r, int even_odd) {
    JNIEnv* env = r->env;
    jobject ft = JSFO(*(even_odd ? &F_FT_EVEN_ODD : &F_FT_WINDING));
    if (ft) { JV(r->path, M_GP_setFillType, ft); (*env)->DeleteLocalRef(env, ft); }
}

// cairo_arc: a line from the current point (if there is one) to the arc's
// start, then the arc in the positive-angle direction, end angle advanced by
// 2pi until it is not before the start. Skia's arcTo treats a sweep of 360
// or more modulo 360 (a full circle could vanish), so the sweep goes in
// pieces of at most 180 degrees.
static void cv_arc(CvReplay* r, const CanvasCmd* c) {
    JNIEnv* env = r->env;
    double a0 = c->a0, a1 = c->a1, rad = c->w;
    if (rad <= 0.0) return;
    while (a1 < a0) a1 += 2.0 * M_PI;
    double sweep = (a1 - a0) * 180.0 / M_PI;
    double start = a0 * 180.0 / M_PI;
    int pieces = (int)ceil(sweep / 180.0);
    if (pieces < 1) pieces = 1;
    double step = sweep / pieces;
    float l = (float)(c->x - rad), t = (float)(c->y - rad);
    float rt = (float)(c->x + rad), b = (float)(c->y + rad);
    for (int i = 0; i < pieces; i++) {
        JV(r->path, M_GP_arcTo, l, t, rt, b, (jfloat)(start + step * i), (jfloat)step,
           (r->cur || i > 0) ? JNI_FALSE : JNI_TRUE);
    }
    r->cur = 1;
}

// A gradient's shader: the stops as the platform's colour and position
// arrays (at least two; a single stop is a flat colour, as cairo paints
// it), spreadMethod as the tile mode (pad CLAMP, reflect MIRROR, repeat
// REPEAT -- the same three cairo's extend has), and for a radial its focal
// point (two-point conical, API 31+: the start circle at the focal point
// with radius 0, the end circle the gradient's own, which is cairo's radial
// exactly) and its ellipse (a unit circle under a local matrix, as GTK4
// maps one with the pattern matrix).
static jobject cv_shader(CvReplay* r, const CanvasCmd* c) {
    JNIEnv* env = r->env;
    int n = c->n_stops;
    if (n <= 0) return NULL;
    int m = n < 2 ? 2 : n;
    jintArray colors = (*env)->NewIntArray(env, m);
    jlongArray lcolors = (*env)->NewLongArray(env, m);
    jfloatArray pos = (*env)->NewFloatArray(env, m);
    if (!colors || !lcolors || !pos) return NULL;
    jint ci[m];
    jlong cl[m];
    jfloat pf[m];
    float prev = 0.0f;
    for (int i = 0; i < m; i++) {
        int si = i < n ? i : n - 1;
        const double* s = &c->stop_rgba[si * 4];
        ci[i] = cv_argb(s[0], s[1], s[2], s[3]);
        // Color.pack(int): an sRGB colour long is the ARGB int in the high word.
        cl[i] = (jlong)((uint64_t)(uint32_t)ci[i] << 32);
        float o = (float)(n < 2 ? (double)i : c->stop_off[si]);
        if (o < 0.0f) o = 0.0f;
        if (o > 1.0f) o = 1.0f;
        if (o < prev) o = prev;   // positions must not go backwards
        prev = o;
        pf[i] = o;
    }
    (*env)->SetIntArrayRegion(env, colors, 0, m, ci);
    (*env)->SetLongArrayRegion(env, lcolors, 0, m, cl);
    (*env)->SetFloatArrayRegion(env, pos, 0, m, pf);
    jobject tile = JSFO(*(c->grad_extend == 1 ? &F_TM_MIRROR : c->grad_extend == 2 ? &F_TM_REPEAT : &F_TM_CLAMP));
    jobject sh = NULL;
    if (c->type == CANVAS_FILL_LINEAR) {
        sh = JNEW(M_LG_init, (jfloat)c->gx1, (jfloat)c->gy1, (jfloat)c->gx2, (jfloat)c->gy2,
                  colors, pos, tile);
    } else {
        int ellipse = c->grx > 0.0 && c->gry > 0.0 &&
                      (fabs(c->grx - c->gry) > 0.01 || fabs(c->grot) > 0.01);
        int two_point = aeui_sdk_int(env) >= 31;
        if (ellipse) {
            // The focal offset in the ellipse's own (unit-circle) space:
            // de-rotated, then scaled per axis.
            double fdx = c->gfx - c->gx1, fdy = c->gfy - c->gy1;
            if (fabs(c->grot) > 0.01) {
                double ang = -c->grot * M_PI / 180.0, ca = cos(ang), sa = sin(ang);
                double tx = fdx * ca - fdy * sa, ty = fdx * sa + fdy * ca;
                fdx = tx; fdy = ty;
            }
            double fx = fdx / c->grx, fy = fdy / c->gry;
            if (two_point && (fabs(fx) > 1e-6 || fabs(fy) > 1e-6))
                sh = JNEW(M_RG_init2pt, (jfloat)fx, (jfloat)fy, (jfloat)0.0f,
                          (jfloat)0.0f, (jfloat)0.0f, (jfloat)1.0f, lcolors, pos, tile);
            else
                sh = JNEW(M_RG_init, (jfloat)0.0f, (jfloat)0.0f, (jfloat)1.0f, colors, pos, tile);
            jobject mx = sh ? JNEW(M_Mx_init) : NULL;
            if (mx) {
                JV(mx, M_Mx_setTranslate, (jfloat)c->gx1, (jfloat)c->gy1);
                if (fabs(c->grot) > 0.01) JZ(mx, M_Mx_preRotate, (jfloat)c->grot);
                JZ(mx, M_Mx_preScale, (jfloat)c->grx, (jfloat)c->gry);
                JV(sh, M_Shader_setLocalMatrix, mx);
                (*env)->DeleteLocalRef(env, mx);
            }
        } else {
            double rad = c->gr;
            if (rad <= 0.0) rad = c->grx > 0.0 ? c->grx : 0.0;
            if (rad > 0.0) {
                int focal = fabs(c->gfx - c->gx1) > 1e-6 || fabs(c->gfy - c->gy1) > 1e-6;
                if (focal && two_point)
                    sh = JNEW(M_RG_init2pt, (jfloat)c->gfx, (jfloat)c->gfy, (jfloat)0.0f,
                              (jfloat)c->gx1, (jfloat)c->gy1, (jfloat)rad, lcolors, pos, tile);
                else   // below API 31 the focal point has nowhere to go: centred
                    sh = JNEW(M_RG_init, (jfloat)c->gx1, (jfloat)c->gy1, (jfloat)rad, colors, pos, tile);
            }
        }
    }
    if (tile) (*env)->DeleteLocalRef(env, tile);
    (*env)->DeleteLocalRef(env, colors);
    (*env)->DeleteLocalRef(env, lcolors);
    (*env)->DeleteLocalRef(env, pos);
    return sh;
}

static void cv_draw_text(CvReplay* r, const CanvasCmd* c, int stroke) {
    JNIEnv* env = r->env;
    if (!c->text || c->w <= 0.0) return;
    if (stroke && c->h <= 0.0) return;
    jobject tf = cv_typeface(env, c->iw, c->font_family);
    jstring s = aeui_jstring(env, c->text);
    if (!s) return;
    if (tf) { jobject old = JO(r->paint, M_GPt_setTypeface, tf); if (old) (*env)->DeleteLocalRef(env, old); }
    JV(r->paint, M_GPt_setTextSize, (jfloat)c->w);
    JV(r->paint, M_GPt_setColor, cv_argb(c->r, c->g, c->b, c->a));
    if (stroke) cv_set_stroke(r, c->h, 1, 1);   // round, as GTK4's text outline
    else JV(r->paint, M_GPt_setStyle, r->s_fill);
    JV(r->canvas, M_GC_drawText, s, (jfloat)c->x, (jfloat)c->y, r->paint);
    (*env)->DeleteLocalRef(env, s);
}

static void cv_draw_image(CvReplay* r, CanvasCmd* c) {
    JNIEnv* env = r->env;
    if (!c->pixels || c->iw <= 0 || c->ih <= 0) return;
    jobject bmp = NULL;
    int local = 0;
    if (c->bitmap) {
        bmp = c->bitmap;
    } else {
        bmp = cv_image_bitmap(env, c);
        if (!bmp) return;
        local = 1;
        // Owned pixels never change, so their Bitmap is kept until the
        // clear; a borrowed buffer is the caller's to rewrite, so it is
        // read afresh each replay.
        if (!c->pixels_borrowed) { c->bitmap = (*env)->NewGlobalRef(env, bmp); }
    }
    double dw = c->w > 0.0 ? c->w : (double)c->iw;
    double dh = c->h > 0.0 ? c->h : (double)c->ih;
    jobject dst = JNEW(M_RF_init, (jfloat)c->x, (jfloat)c->y, (jfloat)(c->x + dw), (jfloat)(c->y + dh));
    jobject p = JNEW(M_GPt_init, (jint)(1 | 2) /* ANTI_ALIAS | FILTER_BITMAP */);
    // FILTER_BITMAP is the smoothing; without it Skia samples nearest
    // (image_rendering "pixelated"). Cleared explicitly: since Android 9 a
    // Paint's constructor ORs FILTER_BITMAP into whatever flags it is given,
    // so leaving the bit out of them still filtered.
    if (p && !r->smooth) JV(p, M_GPt_setFilterBitmap, (jboolean)JNI_FALSE);
    if (dst) JV(r->canvas, M_GC_drawBitmapRect, bmp, (jobject)NULL, dst, p);
    if (dst) (*env)->DeleteLocalRef(env, dst);
    if (p) (*env)->DeleteLocalRef(env, p);
    if (local) (*env)->DeleteLocalRef(env, bmp);
}

// Replay [start, end) of the buffer onto `canvas`. The caller has set the
// canvas's transform (density, an origin) and its clip; everything this
// adds is undone before it returns.
static void canvas_replay_range(JNIEnv* env, jobject canvas, CanvasState* cs, int start, int end) {
    if (!cs || !canvas) return;
    if (start < 0) start = 0;
    if (end > cs->count) end = cs->count;
    if ((*env)->PushLocalFrame(env, 64) != 0) { aeui_check(env, "PushLocalFrame"); return; }
    CvReplay r;
    memset(&r, 0, sizeof(r));
    r.env = env;
    r.canvas = canvas;
    // Smoothing in force where this range begins: a partial replay starts
    // after the command that set it.
    r.smooth = 1;
    for (int j = start - 1; j >= 0; j--) {
        if (cs->cmds[j].type == CANVAS_IMAGE_SMOOTHING) { r.smooth = cs->cmds[j].x != 0.0; break; }
    }
    r.path = JNEW(M_GP_init);
    r.paint = JNEW(M_GPt_init, (jint)1 /* ANTI_ALIAS_FLAG */);
    r.s_fill = JSFO(F_PS_FILL);
    r.s_stroke = JSFO(F_PS_STROKE);
    if (!r.path || !r.paint || !r.s_fill || !r.s_stroke) { (*env)->PopLocalFrame(env, NULL); return; }
    // cairo's and Core Graphics' miter limit; Skia's own default is 4, which
    // bevels joins the others mitre.
    JV(r.paint, M_GPt_setStrokeMiter, (jfloat)10.0f);
    int outer = JI(canvas, M_GC_save);
    r.bases[r.nbases++] = JI(canvas, M_GC_save);
    for (int i = start; i < end; i++) {
        CanvasCmd* c = &cs->cmds[i];
        switch (c->type) {
            case CANVAS_BEGIN_PATH:
                cv_path_reset(&r);
                break;
            case CANVAS_MOVE_TO:
                JV(r.path, M_GP_moveTo, (jfloat)c->x, (jfloat)c->y);
                r.cur = 1;
                break;
            case CANVAS_LINE_TO:
                if (r.cur) JV(r.path, M_GP_lineTo, (jfloat)c->x, (jfloat)c->y);
                else JV(r.path, M_GP_moveTo, (jfloat)c->x, (jfloat)c->y);
                r.cur = 1;
                break;
            case CANVAS_ARC:
                cv_arc(&r, c);
                break;
            case CANVAS_CLOSE_PATH:
                JV(r.path, M_GP_close);
                break;
            case CANVAS_STROKE:
                // A zero width strokes nothing on cairo; Skia's is a hairline.
                if (c->x > 0.0) {
                    JV(r.paint, M_GPt_setColor, cv_argb(c->r, c->g, c->b, c->a));
                    cv_set_stroke(&r, c->x, c->iw, c->ih);
                    JV(canvas, M_GC_drawPath, r.path, r.paint);
                }
                cv_path_reset(&r);
                break;
            case CANVAS_FILL:
                cv_fill_type(&r, c->iw);
                JV(r.paint, M_GPt_setColor, cv_argb(c->r, c->g, c->b, c->a));
                JV(r.paint, M_GPt_setStyle, r.s_fill);
                JV(canvas, M_GC_drawPath, r.path, r.paint);
                cv_path_reset(&r);
                cv_fill_type(&r, 0);
                break;
            case CANVAS_FILL_RECT:
                JV(r.paint, M_GPt_setColor, cv_argb(c->r, c->g, c->b, c->a));
                JV(r.paint, M_GPt_setStyle, r.s_fill);
                JV(canvas, M_GC_drawRect, (jfloat)fmin(c->x, c->x + c->w), (jfloat)fmin(c->y, c->y + c->h),
                   (jfloat)fmax(c->x, c->x + c->w), (jfloat)fmax(c->y, c->y + c->h), r.paint);
                break;
            case CANVAS_CLIP_RECT:
                // Intersects, and holds until the scope ends or RESET_CLIP.
                JZ(canvas, M_GC_clipRect, (jfloat)c->x, (jfloat)c->y,
                   (jfloat)(c->x + c->w), (jfloat)(c->y + c->h));
                cv_path_reset(&r);
                break;
            case CANVAS_CLIP_PATH:
                // vg clip-path: intersect with the current path. Scoped by the
                // enclosing group, whose END restores past the save it made.
                cv_fill_type(&r, c->iw);
                JZ(canvas, M_GC_clipPath, r.path);
                cv_path_reset(&r);
                cv_fill_type(&r, 0);
                break;
            case CANVAS_RESET_CLIP:
                // Back to this compositing scope's baseline: the clip a
                // platform canvas cannot widen is dropped by restoring.
                JV(canvas, M_GC_restoreToCount, (jint)r.bases[r.nbases - 1]);
                r.bases[r.nbases - 1] = JI(canvas, M_GC_save);
                cv_path_reset(&r);
                break;
            case CANVAS_GROUP_BEGIN: {
                // True group opacity: everything up to the matching END is
                // composited into one layer, painted once at the group alpha
                // (cairo_push_group / paint_with_alpha). The alpha arrives
                // with END; the buffer is complete, so look ahead for it.
                double ga = 1.0;
                int depth = 1;
                for (int j = i + 1; j < end; j++) {
                    if (cs->cmds[j].type == CANVAS_GROUP_BEGIN) depth++;
                    else if (cs->cmds[j].type == CANVAS_GROUP_END && --depth == 0) { ga = cs->cmds[j].x; break; }
                }
                if (r.nlayers >= 64 || r.nbases >= 64) break;
                r.layers[r.nlayers++] = JI(canvas, M_GC_saveLayerAlpha, (jobject)NULL, (jint)cv_c8(ga));
                r.bases[r.nbases++] = JI(canvas, M_GC_save);
                break;
            }
            case CANVAS_GROUP_END:
                if (r.nlayers <= 0) break;
                JV(canvas, M_GC_restoreToCount, (jint)r.layers[--r.nlayers]);
                r.nbases--;
                break;
            case CANVAS_FILL_TEXT:
                cv_draw_text(&r, c, 0);
                cv_path_reset(&r);
                break;
            case CANVAS_STROKE_TEXT:
                cv_draw_text(&r, c, 1);
                cv_path_reset(&r);
                break;
            case CANVAS_IMAGE_SMOOTHING:
                r.smooth = c->x != 0.0;
                break;
            case CANVAS_DRAW_IMAGE:
                cv_draw_image(&r, c);
                break;
            case CANVAS_FILL_LINEAR:
            case CANVAS_FILL_RADIAL: {
                jobject sh = cv_shader(&r, c);
                if (sh) {
                    jobject old = JO(r.paint, M_GPt_setShader, sh);
                    if (old) (*env)->DeleteLocalRef(env, old);
                    JV(r.paint, M_GPt_setColor, (jint)0xFF000000);
                    if (c->grad_line_width > 0.0) {
                        // The command's own cap and join: it is the only
                        // thing dispatched for a gradient stroke.
                        cv_set_stroke(&r, c->grad_line_width, c->iw, c->ih);
                    } else {
                        cv_fill_type(&r, 0);
                        JV(r.paint, M_GPt_setStyle, r.s_fill);
                    }
                    JV(canvas, M_GC_drawPath, r.path, r.paint);
                    old = JO(r.paint, M_GPt_setShader, (jobject)NULL);
                    if (old) (*env)->DeleteLocalRef(env, old);
                    (*env)->DeleteLocalRef(env, sh);
                }
                cv_path_reset(&r);
                break;
            }
            case CANVAS_CLEAR:
                break;
        }
    }
    JV(canvas, M_GC_restoreToCount, (jint)outer);
    (*env)->PopLocalFrame(env, NULL);
}

// --- Offscreen replay: read_pixel, write_png, render_range_rgba -------------------
// One pixel per canvas unit, at the size the caller names, onto a cleared
// (transparent) Bitmap -- what the other backends' offscreen replays give.
static jobject cv_render_offscreen(JNIEnv* env, CanvasState* cs, int start, int end,
                                   double ox, double oy, int w, int h) {
    jobject bmp = cv_new_bitmap(env, w, h);
    if (!bmp) return NULL;
    jobject cv = cv_new_canvas(env, bmp);
    if (!cv) { (*env)->DeleteLocalRef(env, bmp); return NULL; }
    if (ox != 0.0 || oy != 0.0) JV(cv, M_GC_translate, (jfloat)-ox, (jfloat)-oy);
    canvas_replay_range(env, cv, cs, start, end);
    (*env)->DeleteLocalRef(env, cv);
    return bmp;
}

// The rendered pixels as premultiplied RGBA bytes, rows packed (malloc'd).
static unsigned char* cv_bitmap_bytes(JNIEnv* env, jobject bmp, int w, int h) {
    AndroidBitmapInfo info;
    void* px = NULL;
    if (AndroidBitmap_getInfo(env, bmp, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        (int)info.width != w || (int)info.height != h ||
        AndroidBitmap_lockPixels(env, bmp, &px) != ANDROID_BITMAP_RESULT_SUCCESS || !px)
        return NULL;
    unsigned char* out = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
    if (out)
        for (int y = 0; y < h; y++)
            memcpy(out + (size_t)y * (size_t)w * 4, (unsigned char*)px + (size_t)y * info.stride, (size_t)w * 4);
    AndroidBitmap_unlockPixels(env, bmp);
    return out;
}

static void cv_cache_drop(CanvasState* cs) {
    free(cs->cache_px);
    cs->cache_px = NULL;
    cs->cache_count = -1;
}

typedef struct { int canvas_id, px, py, w, h, result; } CvPixelReq;

static void cv_read_pixel_run(void* arg) {
    CvPixelReq* q = (CvPixelReq*)arg;
    CanvasState* cs = get_canvas_state(q->canvas_id);
    if (!cs) return;
    if (!cs->cache_px || cs->cache_gen != cs->gen || cs->cache_count != cs->count ||
        cs->cache_w != q->w || cs->cache_h != q->h) {
        cv_cache_drop(cs);
        JNIEnv* env = aeui_frame(16);
        if (!env) return;
        jobject bmp = cv_render_offscreen(env, cs, 0, cs->count, 0, 0, q->w, q->h);
        if (bmp) {
            cs->cache_px = cv_bitmap_bytes(env, bmp, q->w, q->h);
            JV(bmp, M_GB_recycle);
        }
        aeui_unframe(env);
        if (!cs->cache_px) return;
        cs->cache_gen = cs->gen;
        cs->cache_count = cs->count;
        cs->cache_w = q->w;
        cs->cache_h = q->h;
    }
    const unsigned char* p = cs->cache_px + ((size_t)q->py * (size_t)q->w + (size_t)q->px) * 4;
    // 0xAARRGGBB, premultiplied, as cairo's ARGB32 and Core Graphics' read.
    q->result = (int)(((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2]);
}

int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py, int width, int height) {
    if (px < 0 || py < 0 || px >= width || py >= height) return -1;
    if (!get_canvas_state(canvas_id)) return -1;
    CvPixelReq q = { canvas_id, px, py, width, height, -1 };
    aeui_android_run_sync(cv_read_pixel_run, &q);
    return q.result;
}

typedef struct { int canvas_id; const char* path; int w, h, ok; } CvPngReq;

static void cv_write_png_run(void* arg) {
    CvPngReq* q = (CvPngReq*)arg;
    CanvasState* cs = get_canvas_state(q->canvas_id);
    JNIEnv* env = cs ? aeui_frame(16) : NULL;
    if (!env) return;
    jobject bmp = cv_render_offscreen(env, cs, 0, cs->count, 0, 0, q->w, q->h);
    jstring path = bmp ? aeui_jstring(env, q->path) : NULL;
    jobject out = path ? JNEW(M_FOS_init, path) : NULL;
    if (out) {
        q->ok = JZ(bmp, M_GB_compress, J.PNG, (jint)100, out) ? 1 : 0;
        JV(out, M_FOS_close);
    }
    if (bmp) JV(bmp, M_GB_recycle);
    aeui_unframe(env);
}

int aether_ui_canvas_write_png_impl(int canvas_id, const char* path, int width, int height) {
    if (!get_canvas_state(canvas_id) || !path || width <= 0 || height <= 0) return 0;
    CvPngReq q = { canvas_id, path, width, height, 0 };
    aeui_android_run_sync(cv_write_png_run, &q);
    return q.ok;
}

typedef struct { int canvas_id, start, end; double ox, oy; int w, h; unsigned char* out; int result; } CvRangeReq;

static void cv_render_range_run(void* arg) {
    CvRangeReq* q = (CvRangeReq*)arg;
    CanvasState* cs = get_canvas_state(q->canvas_id);
    JNIEnv* env = cs ? aeui_frame(16) : NULL;
    if (!env) return;
    jobject bmp = cv_render_offscreen(env, cs, q->start, q->end, q->ox, q->oy, q->w, q->h);
    unsigned char* px = bmp ? cv_bitmap_bytes(env, bmp, q->w, q->h) : NULL;
    if (bmp) JV(bmp, M_GB_recycle);
    aeui_unframe(env);
    if (!px) return;
    // Straight (non-premultiplied) RGBA out: the form draw_image takes back.
    int n = q->w * q->h;
    for (int i = 0; i < n; i++) {
        unsigned int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], a = px[i * 4 + 3];
        if (a != 0 && a != 255) {
            r = (r * 255 + a / 2) / a; if (r > 255) r = 255;
            g = (g * 255 + a / 2) / a; if (g > 255) g = 255;
            b = (b * 255 + a / 2) / a; if (b > 255) b = 255;
        }
        q->out[i * 4] = (unsigned char)r;
        q->out[i * 4 + 1] = (unsigned char)g;
        q->out[i * 4 + 2] = (unsigned char)b;
        q->out[i * 4 + 3] = (unsigned char)a;
    }
    free(px);
    q->result = n * 4;
}

int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end,
                                            double ox, double oy, int width, int height,
                                            unsigned char* out, int out_len) {
    if (!get_canvas_state(canvas_id) || !out || width <= 0 || height <= 0) return 0;
    if (out_len < width * height * 4) return 0;
    CvRangeReq q = { canvas_id, start, end, ox, oy, width, height, out, 0 };
    aeui_android_run_sync(cv_render_range_run, &q);
    return q.result;
}

int aether_ui_canvas_cmd_count_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->count : -1;
}

// Pixels of the retained paint surface that differ from its top-left (the
// background), sampled every 4 canvas units each way as GTK4 samples its
// own, so the count means the same there and here. -1 before the first
// paint (nothing to sample), 0 = the app painted nothing.
static void cv_painted_run(void* arg) {
    int* io = (int*)arg;
    CanvasState* cs = get_canvas_state(io[0]);
    io[1] = -1;
    if (!cs || !cs->paint_bmp) return;
    JNIEnv* env = aeui_frame(4);
    if (!env) return;
    AndroidBitmapInfo info;
    void* px = NULL;
    if (AndroidBitmap_getInfo(env, cs->paint_bmp, &info) == ANDROID_BITMAP_RESULT_SUCCESS &&
        AndroidBitmap_lockPixels(env, cs->paint_bmp, &px) == ANDROID_BITMAP_RESULT_SUCCESS && px) {
        int step = (int)lrint(4.0 * g_density);
        if (step < 1) step = 1;
        uint32_t bg = *(uint32_t*)px;
        int differing = 0;
        for (uint32_t y = 0; y < info.height; y += (uint32_t)step) {
            const uint32_t* row = (const uint32_t*)((unsigned char*)px + (size_t)y * info.stride);
            for (uint32_t x = 0; x < info.width; x += (uint32_t)step)
                if (row[x] != bg) differing++;
        }
        AndroidBitmap_unlockPixels(env, cs->paint_bmp);
        io[1] = differing;
    }
    aeui_unframe(env);
}

int aether_ui_canvas_painted_pixels_impl(int canvas_id) {
    int io[2] = { canvas_id, -1 };
    aeui_android_run_sync(cv_painted_run, io);
    return io[1];
}

// --- The View ---------------------------------------------------------------------
static void cv_invalidate(CanvasState* cs) {
    AeuiWidget* w = cs ? live_widget(cs->widget_handle) : NULL;
    if (!w) return;
    JNIEnv* env = aeui_env();
    if (!env) return;
    if (aeui_on_ui_thread()) JV(w->view, M_View_invalidate);
    else JV(w->view, M_View_postInvalidate);
}

// on_resize on a CHANGE of size, never per frame, and synchronously, as
// GTK4 fires it from its draw func: the closure re-maps the scene and
// re-flushes the buffer, and the paint in progress replays the new one.
// The closure takes (w, h) as doubles (vg.live's |rw: float, rh: float|),
// as GTK4 and Win32 call it.
static void cv_note_size(int canvas_id, int wpx, int hpx) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || wpx <= 0 || hpx <= 0) return;
    int w = aeui_px_to_dp(wpx), h = aeui_px_to_dp(hpx);
    if (w == cs->last_w && h == cs->last_h) return;
    AeClosure* c = cs->on_resize;
    if (!c || !c->fn) return;   // reported once a hook exists to hear it
    cs->last_w = w;
    cs->last_h = h;
    ((void (*)(void*, double, double))c->fn)(c->env, (double)w, (double)h);
}

// onDraw: replay into the retained surface at device resolution and show it.
static void JNICALL native_canvas_draw(JNIEnv* env, jclass cls, jint canvas_id, jobject vcanvas,
                                       jint wpx, jint hpx) {
    (void)cls;
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || wpx <= 0 || hpx <= 0) return;
    if ((*env)->PushLocalFrame(env, 32) != 0) return;
    cv_note_size(canvas_id, wpx, hpx);
    cs = get_canvas_state(canvas_id);   // the closure may have made canvases (realloc)
    int fresh = 0;
    if (!cs->paint_bmp || cs->paint_w != wpx || cs->paint_h != hpx) {
        if (cs->paint_canvas) (*env)->DeleteGlobalRef(env, cs->paint_canvas);
        if (cs->paint_bmp) { JV(cs->paint_bmp, M_GB_recycle); (*env)->DeleteGlobalRef(env, cs->paint_bmp); }
        cs->paint_bmp = cs->paint_canvas = NULL;
        jobject bmp = cv_new_bitmap(env, wpx, hpx);
        jobject cv = bmp ? cv_new_canvas(env, bmp) : NULL;
        if (!cv) { (*env)->PopLocalFrame(env, NULL); return; }
        cs->paint_bmp = (*env)->NewGlobalRef(env, bmp);
        cs->paint_canvas = (*env)->NewGlobalRef(env, cv);
        cs->paint_w = wpx;
        cs->paint_h = hpx;
        fresh = 1;
    }
    // The paint metrics, in canvas units, as GTK4/AppKit/UIKit report them.
    int clipped = cs->paint_clip_count > 0 && !fresh;
    double area = 0.0;
    for (int i = 0; i < cs->paint_clip_count; i++) {
        double* r = &cs->paint_clip_rects[i * 4];
        if (r[2] > 0.0 && r[3] > 0.0) area += r[2] * r[3];
    }
    cs->last_paint_w = aeui_px_to_dp(wpx);
    cs->last_paint_h = aeui_px_to_dp(hpx);
    cs->last_paint_count = cs->count;
    if (clipped) {
        cs->last_clip_area = (int)(area + 0.5);
        cs->last_paint_area = cs->last_clip_area;
        cs->paint_clip_count_total++;
    } else {
        cs->last_clip_area = 0;
        cs->last_paint_area = cs->last_paint_w * cs->last_paint_h;
        cs->paint_full_count++;
    }
    jobject pc = cs->paint_canvas;
    int saved = JI(pc, M_GC_save);
    JV(pc, M_GC_scale, (jfloat)g_density, (jfloat)g_density);
    if (clipped) {
        // Only the dirty region is cleared and redrawn; the rest of the
        // retained surface keeps what the last paint put there.
        jobject clip = JNEW(M_GP_init);
        jobject dir = JSFO(F_PD_CW);
        for (int i = 0; clip && dir && i < cs->paint_clip_count; i++) {
            double* r = &cs->paint_clip_rects[i * 4];
            if (r[2] > 0.0 && r[3] > 0.0)
                JV(clip, M_GP_addRect, (jfloat)r[0], (jfloat)r[1], (jfloat)(r[0] + r[2]), (jfloat)(r[1] + r[3]), dir);
        }
        if (clip) JZ(pc, M_GC_clipPath, clip);
        jobject mode = JSFO(F_PDM_CLEAR);
        if (mode) JV(pc, M_GC_drawColorMode, (jint)0, mode);
    } else {
        JV(cs->paint_bmp, M_GB_eraseColor, (jint)0);
    }
    canvas_replay_range(env, pc, cs, 0, cs->count);
    JV(pc, M_GC_restoreToCount, (jint)saved);
    cs->paint_clip_count = 0;
    JV(vcanvas, M_GC_drawBitmapAt, cs->paint_bmp, (jfloat)0.0f, (jfloat)0.0f, (jobject)NULL);
    (*env)->PopLocalFrame(env, NULL);
}

// The ABI's modifier bits as GDK reports them to the gesture probe (SHIFT 1,
// CONTROL 4, MOD1/alt 8, SUPER 1<<26), from Android's meta state.
static int cv_gdk_mods(int meta) {
    return ((meta & 0x1) ? 1 : 0) | ((meta & 0x1000) ? 4 : 0) | ((meta & 0x2) ? 8 : 0) |
           ((meta & 0x10000) ? (1 << 26) : 0);
}

static void cv_probe(CanvasState* cs, const char* kind, double a, double b, int meta) {
    AeClosure* p = cs->probe;
    if (p && p->fn)
        ((void (*)(void*, const char*, double, double, intptr_t))p->fn)(p->env, kind, a, b, (intptr_t)cv_gdk_mods(meta));
}

static void cv_xy(AeClosure* c, double x, double y) {
    if (c && c->fn) ((void (*)(void*, double, double))c->fn)(c->env, x, y);
}

// A key's name as the DSL speaks it (GDK's): the named keys, F1..F12, and
// otherwise the character typed (shift folded in). Modifiers alone are not
// keys the app hears.
static int cv_key_name(int code, int unicode, char* out, int n) {
    const char* named = aeui_key_name(code);
    if (named) { snprintf(out, (size_t)n, "%s", named); return 1; }
    if (code >= 131 && code <= 142) { snprintf(out, (size_t)n, "F%d", code - 130); return 1; }
    if (unicode <= 0) return 0;
    char u[8] = "";
    if (unicode < 0x80) { u[0] = (char)unicode; }
    else if (unicode < 0x800) { u[0] = (char)(0xC0 | (unicode >> 6)); u[1] = (char)(0x80 | (unicode & 63)); }
    else { u[0] = (char)(0xE0 | (unicode >> 12)); u[1] = (char)(0x80 | ((unicode >> 6) & 63)); u[2] = (char)(0x80 | (unicode & 63)); }
    snprintf(out, (size_t)n, "%s", u);
    return 1;
}

enum { AEUI_CV_DOWN = 1, AEUI_CV_MOVE = 2, AEUI_CV_UP = 3, AEUI_CV_CANCEL = 4,
       AEUI_CV_POINTER_DOWN = 5, AEUI_CV_POINTER_UP = 6, AEUI_CV_HOVER = 7,
       AEUI_CV_SCROLL = 8, AEUI_CV_SIZE = 9, AEUI_CV_KEY_DOWN = 10, AEUI_CV_KEY_UP = 11,
       AEUI_CV_DOUBLE_TAP = 12, AEUI_CV_SECONDARY = 13 };

static jboolean JNICALL native_canvas_event(JNIEnv* env, jclass cls, jint canvas_id, jint kind,
                                            jint count, jfloat x0, jfloat y0, jfloat x1, jfloat y1,
                                            jint meta) {
    (void)cls; (void)env;
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return JNI_FALSE;
    // Pixels in, canvas units (dp) out.
    double x = x0 / g_density, y = y0 / g_density;
    int pointer = cs->on_click || cs->on_move || cs->on_release || cs->probe
                  || cs->on_right_click || cs->on_double_click;
    switch (kind) {
        case AEUI_CV_SIZE:
            cv_note_size(canvas_id, (int)x0, (int)y0);
            return JNI_TRUE;
        case AEUI_CV_DOWN:
            cs->g_down = 1; cs->g_x0 = x; cs->g_y0 = y; cs->g_two = 0;
            cv_probe(cs, "drag-begin", x, y, meta);
            cv_xy(cs->on_click, x, y);
            return pointer ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_POINTER_DOWN:
        case AEUI_CV_MOVE:
            if (count >= 2) {
                double dx = (x1 - x0) / g_density, dy = (y1 - y0) / g_density;
                double span = sqrt(dx * dx + dy * dy), ang = atan2(dy, dx);
                if (!cs->g_two) {
                    cs->g_two = 1; cs->g_span0 = span; cs->g_ang0 = ang;
                } else if (kind == AEUI_CV_MOVE) {
                    if (cs->g_span0 > 0.0) cv_probe(cs, "zoom", span / cs->g_span0, 0.0, meta);
                    cv_probe(cs, "rotate", ang - cs->g_ang0, 0.0, meta);
                }
                return pointer ? JNI_TRUE : JNI_FALSE;
            }
            if (kind == AEUI_CV_MOVE) {
                if (cs->g_down) cv_probe(cs, "drag-update", x - cs->g_x0, y - cs->g_y0, meta);
                cv_xy(cs->on_move, x, y);
            }
            return pointer ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_POINTER_UP:
            if (count <= 2) cs->g_two = 0;
            return pointer ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_UP:
        case AEUI_CV_CANCEL:
            if (cs->g_down) cv_probe(cs, "drag-end", x - cs->g_x0, y - cs->g_y0, meta);
            cs->g_down = 0; cs->g_two = 0;
            cv_xy(cs->on_release, x, y);
            return pointer ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_HOVER:
            cv_xy(cs->on_move, x, y);
            return cs->on_move ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_DOUBLE_TAP:
            // GestureDetector's double tap (the platform's timing): the
            // touch double click.
            cv_xy(cs->on_double_click, x, y);
            return cs->on_double_click ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_SECONDARY:
            // A right click: the mouse's secondary button released, or a
            // long press (its touch spelling).
            cv_xy(cs->on_right_click, x, y);
            return cs->on_right_click ? JNI_TRUE : JNI_FALSE;
        case AEUI_CV_SCROLL: {
            // AXIS_VSCROLL is positive AWAY from the user, the DSL's dy is
            // negative away (the zoom-in direction); AXIS_HSCROLL is
            // positive to the right, as the DSL's dx is.
            double dx = x0, dy = -y0;
            cv_probe(cs, "scroll", dx, dy, meta);
            cv_xy(cs->on_scroll, dx, dy);
            return (cs->on_scroll || cs->probe) ? JNI_TRUE : JNI_FALSE;
        }
        case AEUI_CV_KEY_DOWN:
        case AEUI_CV_KEY_UP: {
            AeClosure* c = kind == AEUI_CV_KEY_DOWN ? cs->on_key : cs->on_key_release;
            char name[16];
            if (!c || !c->fn || !cv_key_name(count, (int)x0, name, (int)sizeof(name))) return JNI_FALSE;
            ((void (*)(void*, const char*))c->fn)(c->env, name);
            return JNI_TRUE;
        }
        default:
            return JNI_FALSE;
    }
}

int aether_ui_canvas_create_impl(int width, int height) {
    JNIEnv* env = aeui_frame(8);
    if (!env) return 0;
    if (!g_activity || !g_canvas_init) { aeui_unframe(env); return 0; }
    if (canvas_state_count >= canvas_state_capacity) {
        int cap = canvas_state_capacity == 0 ? 16 : canvas_state_capacity * 2;
        CanvasState* nc = (CanvasState*)realloc(canvas_states, sizeof(CanvasState) * (size_t)cap);
        if (!nc) { aeui_unframe(env); return 0; }
        canvas_states = nc;
        canvas_state_capacity = cap;
    }
    int canvas_id = canvas_state_count + 1;
    jobject v = (*env)->NewObject(env, g_canvas_class, g_canvas_init, g_activity, (jint)canvas_id);
    if (aeui_check(env, "new AetherCanvas") || !v) { aeui_unframe(env); return 0; }
    int h = register_widget_typed(env, v, AUI_CANVAS);
    if (!h) { aeui_unframe(env); return 0; }
    CanvasState* cs = &canvas_states[canvas_state_count++];
    memset(cs, 0, sizeof(*cs));
    cs->widget_handle = h;
    cs->created_w = width;
    cs->created_h = height;
    cs->last_w = cs->last_h = -1;
    cs->cache_count = -1;
    // The size asked for is the canvas's NATURAL size, and it takes the
    // slack both ways, as GTK4's drawing area (hexpand + vexpand) and
    // AppKit's low-priority constraints do: a vg scene fills the window and
    // re-maps its viewBox through on_resize. A pinned size (width/height)
    // still holds it exactly.
    AeuiWidget* w = widget_at(h);
    w->own_hexp = w->own_vexp = w->hexp = w->vexp = 1;
    JV(v, M_View_setMinimumWidth, (jint)aeui_dp(width > 0 ? width : 0));
    JV(v, M_View_setMinimumHeight, (jint)aeui_dp(height > 0 ? height : 0));
    aeui_unframe(env);
    return canvas_id;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->widget_handle : 0;
}

void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    cs->on_resize = (AeClosure*)boxed_closure;
    // Already laid out: the size it has now is news to this hook (GTK4
    // seeds its last size to -1 so the next paint reports it).
    cs->last_w = cs->last_h = -1;
    cv_invalidate(cs);
}
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->on_click = (AeClosure*)boxed_closure;
}
void aether_ui_canvas_on_right_click_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->on_right_click = (AeClosure*)boxed_closure;
}
void aether_ui_canvas_on_double_click_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->on_double_click = (AeClosure*)boxed_closure;
}
void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->on_move = (AeClosure*)boxed_closure;
}
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->on_release = (AeClosure*)boxed_closure;
}
void aether_ui_canvas_on_scroll_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->on_scroll = (AeClosure*)boxed_closure;
}
void aether_ui_canvas_gesture_probe_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs && boxed_closure) cs->probe = (AeClosure*)boxed_closure;
}

// Keys reach a View only while it has focus, so a canvas that listens for
// them becomes focusable (in touch mode too, so a tap focuses it, as a click
// focuses GTK4's): one that does not stays out of the focus order.
static void cv_want_keys(CanvasState* cs) {
    if (cs->keys) return;
    AeuiWidget* w = live_widget(cs->widget_handle);
    JNIEnv* env = w ? aeui_frame(4) : NULL;
    if (!env) return;
    JV(w->view, M_View_setFocusable, JNI_TRUE);
    JV(w->view, M_View_setFocusableInTouchMode, JNI_TRUE);
    cs->keys = 1;
    aeui_unframe(env);
}
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key = (AeClosure*)boxed_closure;
    cv_want_keys(cs);
}
void aether_ui_canvas_on_key_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key_release = (AeClosure*)boxed_closure;
    cv_want_keys(cs);
}

// --- Recording ------------------------------------------------------------------
void aether_ui_canvas_begin_path_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_BEGIN_PATH });
}
void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_MOVE_TO, .x = x, .y = y });
}
void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_LINE_TO, .x = x, .y = y });
}
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b, double a,
                                  double line_width, int cap, int join) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_STROKE, .r = r, .g = g, .b = b, .a = a,
                                           .x = line_width, .iw = cap, .ih = join });
}
void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y, double w, double h,
                                     double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_FILL_RECT, .x = x, .y = y, .w = w, .h = h,
                                           .r = r, .g = g, .b = b, .a = a });
}
void aether_ui_canvas_image_smoothing_impl(int canvas_id, int on) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_IMAGE_SMOOTHING, .x = on ? 1.0 : 0.0 });
}

void aether_ui_canvas_group_begin_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_GROUP_BEGIN });
}
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_GROUP_END, .x = alpha });
}
void aether_ui_canvas_clip_path_impl(int canvas_id, int even_odd) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_CLIP_PATH, .iw = even_odd ? 1 : 0 });
}
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y, double w, double h) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_CLIP_RECT, .x = x, .y = y, .w = w, .h = h });
}
void aether_ui_canvas_reset_clip_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_RESET_CLIP });
}
void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                               double start_angle, double end_angle) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_ARC, .x = cx, .y = cy, .w = radius,
                                           .a0 = start_angle, .a1 = end_angle });
}
void aether_ui_canvas_close_path_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_CLOSE_PATH });
}
void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a, int even_odd) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_FILL, .r = r, .g = g, .b = b, .a = a,
                                           .iw = even_odd });
}
void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text, double x, double y,
                                     double font_size, int font_flags, const char* font_family,
                                     double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL_TEXT, .x = x, .y = y, .w = font_size, .iw = font_flags,
        .r = r, .g = g, .b = b, .a = a,
        .font_family = (font_family && font_family[0]) ? strdup(font_family) : NULL,
        .text = text ? strdup(text) : NULL });
}
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text, double x, double y,
                                       double font_size, double line_width, int font_flags,
                                       const char* font_family, double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_STROKE_TEXT, .x = x, .y = y, .w = font_size, .h = line_width, .iw = font_flags,
        .r = r, .g = g, .b = b, .a = a,
        .font_family = (font_family && font_family[0]) ? strdup(font_family) : NULL,
        .text = text ? strdup(text) : NULL });
}

static void cv_add_image(int canvas_id, double x, double y, double dw, double dh, int iw, int ih,
                         const unsigned char* rgba, int byte_len, int borrowed) {
    if (iw <= 0 || ih <= 0 || !rgba || byte_len < iw * ih * 4) return;
    unsigned char* px = (unsigned char*)rgba;
    if (!borrowed) {
        px = (unsigned char*)malloc((size_t)iw * (size_t)ih * 4);
        if (!px) return;
        memcpy(px, rgba, (size_t)iw * (size_t)ih * 4);
    }
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_DRAW_IMAGE, .x = x, .y = y, .w = dw, .h = dh,
                                           .pixels = px, .pixels_borrowed = borrowed, .iw = iw, .ih = ih });
}
void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y, int iw, int ih,
                                      const unsigned char* rgba, int byte_len) {
    cv_add_image(canvas_id, x, y, 0, 0, iw, ih, rgba, byte_len, 0);
}
void aether_ui_canvas_draw_image_impl_ptr(int canvas_id, double x, double y, int iw, int ih,
                                          const unsigned char* rgba, int byte_len) {
    cv_add_image(canvas_id, x, y, 0, 0, iw, ih, rgba, byte_len, 0);
}
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y, double dw, double dh,
                                             int iw, int ih, const unsigned char* rgba, int byte_len) {
    cv_add_image(canvas_id, x, y, dw, dh, iw, ih, rgba, byte_len, 0);
}
void aether_ui_canvas_draw_image_scaled_impl_ptr(int canvas_id, double x, double y, double dw, double dh,
                                                 int iw, int ih, const unsigned char* rgba, int byte_len) {
    cv_add_image(canvas_id, x, y, dw, dh, iw, ih, rgba, byte_len, 0);
}
// Borrowed: the caller's pixels, valid until the next clear (#102); not
// copied, and never freed here.
void aether_ui_canvas_draw_image_borrowed_impl(int canvas_id, double x, double y, int iw, int ih,
                                               const unsigned char* rgba, int byte_len) {
    cv_add_image(canvas_id, x, y, 0, 0, iw, ih, rgba, byte_len, 1);
}
void aether_ui_canvas_draw_image_scaled_borrowed_impl(int canvas_id, double x, double y, double dw,
                                                      double dh, int iw, int ih,
                                                      const unsigned char* rgba, int byte_len) {
    cv_add_image(canvas_id, x, y, dw, dh, iw, ih, rgba, byte_len, 1);
}

static void cv_copy_stops(CanvasCmd* c, int n_stops, void* offsets, void* rgba) {
    if (n_stops < 0 || !offsets || !rgba) n_stops = 0;
    c->n_stops = n_stops;
    c->stop_off = (double*)malloc(sizeof(double) * (size_t)(n_stops > 0 ? n_stops : 1));
    c->stop_rgba = (double*)malloc(sizeof(double) * (size_t)(n_stops > 0 ? n_stops * 4 : 1));
    if (!c->stop_off || !c->stop_rgba) { c->n_stops = 0; return; }
    for (int i = 0; i < n_stops; i++) {
        c->stop_off[i] = floatarr_get_raw(offsets, i);
        for (int k = 0; k < 4; k++) c->stop_rgba[i * 4 + k] = floatarr_get_raw(rgba, i * 4 + k);
    }
}
void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id, double x1, double y1, double x2, double y2,
                                                int n_stops, void* offsets, void* rgba,
                                                double line_width, int extend, int cap, int join) {
    CanvasCmd cmd = { .type = CANVAS_FILL_LINEAR, .gx1 = x1, .gy1 = y1, .gx2 = x2, .gy2 = y2,
                      .grad_line_width = line_width, .grad_extend = extend, .iw = cap, .ih = join };
    cv_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}
void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id, double cx, double cy, double radius,
                                                double fx, double fy, int n_stops, void* offsets,
                                                void* rgba, double line_width, int extend, int cap,
                                                int join, double rx, double ry, double rot_deg) {
    CanvasCmd cmd = { .type = CANVAS_FILL_RADIAL, .gx1 = cx, .gy1 = cy, .gr = radius, .gfx = fx, .gfy = fy,
                      .grad_line_width = line_width, .grad_extend = extend, .iw = cap, .ih = join,
                      .grx = rx, .gry = ry, .grot = rot_deg };
    cv_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_set_clip_rects_impl(int canvas_id, void* rects, int n) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    if (!rects || n <= 0) { cs->paint_clip_count = 0; return; }
    if (n > cs->paint_clip_capacity) {
        double* nr = (double*)realloc(cs->paint_clip_rects, sizeof(double) * (size_t)n * 4);
        if (!nr) { cs->paint_clip_count = 0; return; }
        cs->paint_clip_rects = nr;
        cs->paint_clip_capacity = n;
    }
    for (int i = 0; i < n * 4; i++) cs->paint_clip_rects[i] = floatarr_get_raw(rects, i);
    cs->paint_clip_count = n;
}

// A new frame: every command goes (with what it owned), the generation
// moves so read_pixel's cache cannot answer for the old scene.
static void cv_clear_run(void* arg) {
    CanvasState* cs = get_canvas_state((int)(intptr_t)arg);
    if (!cs) return;
    JNIEnv* env = aeui_env();
    for (int i = 0; i < cs->count; i++) {
        CanvasCmd* c = &cs->cmds[i];
        free(c->text); c->text = NULL;
        free(c->font_family); c->font_family = NULL;
        if (c->pixels && !c->pixels_borrowed) free(c->pixels);
        c->pixels = NULL;
        if (c->bitmap && env) (*env)->DeleteGlobalRef(env, c->bitmap);
        c->bitmap = NULL;
        free(c->stop_off); c->stop_off = NULL;
        free(c->stop_rgba); c->stop_rgba = NULL;
    }
    cs->count = 0;
    cs->gen++;
    cv_cache_drop(cs);
    cv_invalidate(cs);
}

void aether_ui_canvas_clear_impl(int canvas_id) {
    if (!get_canvas_state(canvas_id)) return;
    // On the UI thread, where the replays run, so a paint never sees a
    // buffer being freed under it.
    aeui_android_run_sync(cv_clear_run, (void*)(intptr_t)canvas_id);
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    cv_invalidate(get_canvas_state(canvas_id));
}

// The driver's canvas events: the same closures the View's input runs.
static int cv_driver_event(AetherDriverActionCtx* ctx) {
    CanvasState* cs = get_canvas_state(ctx->handle);
    if (!cs) return 3;
    AeClosure* c = ctx->action == AETHER_DRV_CANVAS_SCROLL  ? cs->on_scroll
                 : ctx->action == AETHER_DRV_CANVAS_CLICK   ? cs->on_click
                 : ctx->action == AETHER_DRV_CANVAS_RIGHT_CLICK  ? cs->on_right_click
                 : ctx->action == AETHER_DRV_CANVAS_DOUBLE_CLICK ? cs->on_double_click
                 : ctx->action == AETHER_DRV_CANVAS_MOVE    ? cs->on_move
                 : ctx->action == AETHER_DRV_CANVAS_RELEASE ? cs->on_release
                 : ctx->action == AETHER_DRV_CANVAS_KEYUP   ? cs->on_key_release
                                                            : cs->on_key;
    if (!c || !c->fn) return 3;   // 404: nothing wired, not an event missed
    if (ctx->action == AETHER_DRV_CANVAS_KEY || ctx->action == AETHER_DRV_CANVAS_KEYUP)
        ((void (*)(void*, const char*))c->fn)(c->env, ctx->sval);
    else
        ((void (*)(void*, double, double))c->fn)(c->env, ctx->dval, ctx->dval2);
    return 0;
}

static int hook_canvas_debug(int canvas_id, int* area, int* commands, int* w, int* h) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    if (area) *area = cs->last_paint_area;
    if (commands) *commands = cs->count;
    // The canvas's real size once painted; its natural size before that.
    if (w) *w = cs->last_paint_w > 0 ? cs->last_paint_w : cs->created_w;
    if (h) *h = cs->last_paint_h > 0 ? cs->last_paint_h : cs->created_h;
    return 0;
}

static int hook_canvas_paint_counters(int canvas_id, int* full_paints, int* clip_paints,
                                      int* last_clip_area) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return 1;
    if (full_paints) *full_paints = cs->paint_full_count;
    if (clip_paints) *clip_paints = cs->paint_clip_count_total;
    if (last_clip_area) *last_clip_area = cs->last_clip_area;
    return 0;
}

// ===========================================================================
// GPU view (#92) -- a SurfaceView with an OpenGL ES context (EGL).
//
// The contract is canvas's shape with a GL context the backend owns: the
// app's on_render runs with the context current and the backend presents
// afterwards. Android's GL is OpenGL ES, through EGL, onto the SurfaceView's
// Surface (the ANativeWindow pass B's native view hands out); the entry
// points an app calls -- glClearColor, glClear, glViewport, and the rest of
// what ES shares with desktop GL -- are libGLESv3's. ES 3 where the device
// has it, else ES 2.
//
// Frames are drawn on the UI thread, where the app's closures belong: once
// when the Surface arrives (realize, then resize, then a frame, as a GtkGLArea
// draws when it is shown), and then whenever the app asks
// (request_render). With no Surface yet -- the view not laid out, the
// activity in the background -- a frame goes to an offscreen framebuffer of
// the view's size instead, as AppKit's headless path does, so an app that
// animates still runs.
//
// read_pixel draws a fresh frame into that offscreen framebuffer and reads
// it (AppKit's approach): a window surface's back buffer is undefined once
// swapped, so reading the presented frame would answer with whatever the
// driver left there.
//
// gpuview_available says whether EGL gives this device a context of either
// version -- the honest answer for a phone, and for the emulator's GPU
// (SwiftShader or the host's) alike.
// ===========================================================================
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

typedef struct {
    int widget;
    ANativeWindow* window;
    EGLSurface surface;    // the window's, EGL_NO_SURFACE until the Surface exists
    EGLSurface pbuffer;    // 1x1, what makes the context current without one
    EGLContext ctx;
    AeClosure* on_realize; AeClosure* on_render; AeClosure* on_resize;
    int realized, last_w, last_h;
    int surf_w, surf_h;
    int pending;
    double last_render;
    GLuint fbo, tex, depth; int probe_w, probe_h;
} AeuiGpu;

static AeuiGpu* gpus = NULL;
static int ngpus = 0;
static EGLDisplay g_egl_dpy = EGL_NO_DISPLAY;
static EGLConfig g_egl_cfg = NULL;
static int g_egl_version = 0;   // 3, 2, or -1 = no GL here

static AeuiGpu* gpu_at(int id) { return (id >= 1 && id <= ngpus) ? &gpus[id - 1] : NULL; }

static int gpu_egl_init(void) {
    if (g_egl_version) return g_egl_version > 0;
    g_egl_version = -1;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) {
        AEUI_LOGW("gpuview: no EGL display (0x%x)", eglGetError());
        return 0;
    }
    for (int ver = 3; ver >= 2; ver--) {
        const EGLint attrs[] = {
            EGL_RENDERABLE_TYPE, ver == 3 ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 16,
            EGL_NONE };
        EGLConfig cfg = NULL;
        EGLint n = 0;
        if (eglChooseConfig(d, attrs, &cfg, 1, &n) && n > 0) {
            g_egl_dpy = d;
            g_egl_cfg = cfg;
            g_egl_version = ver;
            return 1;
        }
    }
    AEUI_LOGW("gpuview: no RGBA8888 ES2/ES3 EGL config on this device");
    return 0;
}

static double gpu_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// The context, made current on the window surface when there is one, else
// on the 1x1 pbuffer (enough to drive an offscreen framebuffer).
static int gpu_make_current(AeuiGpu* g) {
    if (!gpu_egl_init()) return 0;
    if (g->ctx == EGL_NO_CONTEXT || !g->ctx) {
        const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, g_egl_version, EGL_NONE };
        g->ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, EGL_NO_CONTEXT, ca);
        if (g->ctx == EGL_NO_CONTEXT) { g->ctx = NULL; AEUI_LOGW("gpuview: eglCreateContext 0x%x", eglGetError()); return 0; }
    }
    EGLSurface s = g->surface;
    if (s == EGL_NO_SURFACE || !s) {
        if (g->pbuffer == EGL_NO_SURFACE || !g->pbuffer) {
            const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            g->pbuffer = eglCreatePbufferSurface(g_egl_dpy, g_egl_cfg, pa);
            if (g->pbuffer == EGL_NO_SURFACE) { g->pbuffer = NULL; return 0; }
        }
        s = g->pbuffer;
    }
    return eglMakeCurrent(g_egl_dpy, s, s, g->ctx) ? 1 : 0;
}

static void gpu_fire_wh(AeClosure* c, int w, int h) {
    if (c && c->fn) ((void (*)(void*, intptr_t, intptr_t))c->fn)(c->env, (intptr_t)w, (intptr_t)h);
}

// The view's size in pixels: the Surface's once it exists, else its layout.
static void gpu_size(AeuiGpu* g, int* w, int* h) {
    *w = g->surf_w; *h = g->surf_h;
    if (*w > 0 && *h > 0) return;
    AeuiWidget* sw = live_widget(g->widget);
    JNIEnv* env = sw ? aeui_env() : NULL;
    if (env) { *w = JI(sw->view, M_View_getWidth); *h = JI(sw->view, M_View_getHeight); }
    if (*w <= 0 || *h <= 0) { *w = 1; *h = 1; }
}

// Realize once (on_realize, then on_resize), and resize on a change: with
// the context current, as the contract says, before the frame it precedes.
static void gpu_settle(AeuiGpu* g, int w, int h) {
    if (!g->realized) {
        g->realized = 1;
        g->last_w = w; g->last_h = h;
        AeClosure* c = g->on_realize;
        if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
        gpu_fire_wh(g->on_resize, w, h);
    } else if (w != g->last_w || h != g->last_h) {
        g->last_w = w; g->last_h = h;
        gpu_fire_wh(g->on_resize, w, h);
    }
}

static void gpu_call_render(AeuiGpu* g) {
    double now = gpu_now();
    double dt = g->last_render > 0.0 ? now - g->last_render : 0.0;
    g->last_render = now;
    AeClosure* c = g->on_render;
    if (c && c->fn) ((void (*)(void*, double))c->fn)(c->env, dt);
}

// A frame into the offscreen framebuffer (kept, and resized with the view).
// Leaves it bound; 0 when there is no usable target.
static int gpu_frame_offscreen(AeuiGpu* g, int* out_w, int* out_h) {
    if (!gpu_make_current(g)) return 0;
    int w, h;
    gpu_size(g, &w, &h);
    if (!g->fbo) {
        glGenFramebuffers(1, &g->fbo);
        glGenTextures(1, &g->tex);
        glGenRenderbuffers(1, &g->depth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    if (g->probe_w != w || g->probe_h != h) {
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depth);
        g->probe_w = w; g->probe_h = h;
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }
    glViewport(0, 0, w, h);
    gpu_settle(g, w, h);
    gpu_call_render(g);
    glFinish();
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 1;
}

// A frame onto the Surface, presented.
static void gpu_frame(AeuiGpu* g) {
    if (!g->surface) {
        if (gpu_frame_offscreen(g, NULL, NULL)) glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    if (!gpu_make_current(g)) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The whole Surface, as a GtkGLArea sets it before each render; an
    // on_resize that sets its own runs after this and wins.
    glViewport(0, 0, g->surf_w, g->surf_h);
    gpu_settle(g, g->surf_w, g->surf_h);
    gpu_call_render(g);
    if (!eglSwapBuffers(g_egl_dpy, g->surface))
        AEUI_LOGW("gpuview: eglSwapBuffers 0x%x", eglGetError());
}

static void gpu_render_job(void* arg) {
    AeuiGpu* g = gpu_at((int)(intptr_t)arg);
    if (!g) return;
    g->pending = 0;
    if (live_widget(g->widget)) gpu_frame(g);
}

// The Surface arriving, changing size (w, h in pixels) or going (0 x 0):
// the SurfaceHolder.Callback a native view also listens with.
static void aeui_gpu_surface(JNIEnv* env, int widget, int w, int h) {
    AeuiGpu* g = NULL;
    for (int i = 0; i < ngpus; i++) if (gpus[i].widget == widget) g = &gpus[i];
    AeuiWidget* sw = live_widget(widget);
    if (!g || !sw || !gpu_egl_init()) return;
    if (w <= 0 || h <= 0) {
        if (g->surface) {
            if (g->ctx) eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(g_egl_dpy, g->surface);
            g->surface = NULL;
        }
        if (g->window) { ANativeWindow_release(g->window); g->window = NULL; }
        g->surf_w = g->surf_h = 0;
        return;
    }
    if (!g->window) {
        jobject holder = JO(sw->view, M_SV2_getHolder);
        jobject surface = holder ? JO(holder, M_SH_getSurface) : NULL;
        if (surface) g->window = ANativeWindow_fromSurface(env, surface);
    }
    if (g->window && !g->surface) {
        g->surface = eglCreateWindowSurface(g_egl_dpy, g_egl_cfg, g->window, NULL);
        if (g->surface == EGL_NO_SURFACE) {
            AEUI_LOGW("gpuview: eglCreateWindowSurface 0x%x", eglGetError());
            g->surface = NULL;
        }
    }
    g->surf_w = w;
    g->surf_h = h;
    gpu_frame(g);
}

int aether_ui_gpuview_available_impl(void) {
    return gpu_egl_init();
}

int aether_ui_gpuview_create_impl(int width, int height) {
    if (!gpu_egl_init()) return 0;   // asked first, as the contract says
    JNIEnv* env = aeui_frame(16);
    if (!env) return 0;
    if (!g_activity) { aeui_unframe(env); return 0; }
    int id = 0;
    jobject sv = JNEW(M_SV2_init, g_activity);
    AeuiGpu* ng = sv ? (AeuiGpu*)realloc(gpus, sizeof(AeuiGpu) * (size_t)(ngpus + 1)) : NULL;
    if (ng) {
        gpus = ng;
        int h = register_widget_typed(env, sv, AUI_GPUVIEW);
        AeuiGpu* g = &gpus[ngpus++];
        memset(g, 0, sizeof(*g));
        g->widget = h;
        id = ngpus;
        // Canvas's natural-size contract: the size asked for is where the
        // view starts, and it takes the slack both ways.
        AeuiWidget* w = widget_at(h);
        if (w) w->own_hexp = w->own_vexp = w->hexp = w->vexp = 1;
        JV(sv, M_View_setMinimumWidth, (jint)aeui_dp(width > 0 ? width : 1));
        JV(sv, M_View_setMinimumHeight, (jint)aeui_dp(height > 0 ? height : 1));
        jobject holder = JO(sv, M_SV2_getHolder);
        jobject l = aeui_listener(env, h, AEUI_EV_SURFACE);
        if (holder && l) JV(holder, M_SH_addCallback, l);
    }
    aeui_unframe(env);
    return id;
}

int aether_ui_gpuview_get_widget(int gpu_id) {
    AeuiGpu* g = gpu_at(gpu_id);
    return g ? g->widget : 0;
}
void aether_ui_gpuview_on_realize_impl(int gpu_id, void* boxed_closure) {
    AeuiGpu* g = gpu_at(gpu_id);
    if (g) g->on_realize = (AeClosure*)boxed_closure;
}
void aether_ui_gpuview_on_render_impl(int gpu_id, void* boxed_closure) {
    AeuiGpu* g = gpu_at(gpu_id);
    if (g) g->on_render = (AeClosure*)boxed_closure;
}
void aether_ui_gpuview_on_resize_impl(int gpu_id, void* boxed_closure) {
    AeuiGpu* g = gpu_at(gpu_id);
    if (g) g->on_resize = (AeClosure*)boxed_closure;
}

// One more frame, on the UI thread's next turn; requests that arrive before
// it is drawn are one frame, as gtk_gl_area_queue_render coalesces them.
void aether_ui_gpuview_request_render_impl(int gpu_id) {
    AeuiGpu* g = gpu_at(gpu_id);
    if (!g || g->pending) return;
    g->pending = 1;
    aeui_android_post(gpu_render_job, (void*)(intptr_t)gpu_id);
}

typedef struct { int id, px, py, result; } GpuPixelReq;

static void gpu_read_pixel_run(void* arg) {
    GpuPixelReq* q = (GpuPixelReq*)arg;
    AeuiGpu* g = gpu_at(q->id);
    if (!g || !live_widget(g->widget)) return;
    int w = 0, h = 0;
    if (!gpu_frame_offscreen(g, &w, &h)) return;
    if (q->px < w && q->py < h) {
        unsigned char rgba[4] = { 0, 0, 0, 0 };
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // GL's origin is bottom-left; every read-back in this ABI is top-left.
        glReadPixels(q->px, h - 1 - q->py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        q->result = (int)(((unsigned)rgba[0] << 24) | ((unsigned)rgba[1] << 16) |
                          ((unsigned)rgba[2] << 8) | rgba[3]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int aether_ui_gpuview_read_pixel_impl(int gpu_id, int px, int py) {
    if (!gpu_at(gpu_id) || px < 0 || py < 0) return -1;
    GpuPixelReq q = { gpu_id, px, py, -1 };
    aeui_android_run_sync(gpu_read_pixel_run, &q);
    return q.result;
}

// ===========================================================================
// File icons -- an ImageView showing the icon for a path: an image file is
// its own picture; anything else gets the icon of the app that opens its
// type (PackageManager, as a file manager shows it), else a framework glyph
// for the kind of thing it is. The path need not exist (named by its
// extension, as on every backend); "" is an empty image.
// ===========================================================================
JCLASS(C_MimeTypeMap, "android/webkit/MimeTypeMap");
JCLASS(C_PackageManager, "android/content/pm/PackageManager");
JCLASS(C_ResolveInfo, "android/content/pm/ResolveInfo");
JSTATIC(M_MTM_getSingleton, C_MimeTypeMap, "getSingleton", "()Landroid/webkit/MimeTypeMap;");
JMETHOD(M_MTM_getMime, C_MimeTypeMap, "getMimeTypeFromExtension", "(Ljava/lang/String;)Ljava/lang/String;");
JMETHOD(M_Intent_setDataAndType, C_Intent, "setDataAndType", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;");
JMETHOD(M_PM_resolveActivity, C_PackageManager, "resolveActivity",
        "(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;");
JMETHOD(M_RI_loadIcon, C_ResolveInfo, "loadIcon",
        "(Landroid/content/pm/PackageManager;)Landroid/graphics/drawable/Drawable;");
JMETHOD(M_IV_setImageDrawable, C_ImageView, "setImageDrawable", "(Landroid/graphics/drawable/Drawable;)V");
JMETHOD(M_IV_setImageResource, C_ImageView, "setImageResource", "(I)V");
JSTATIC(M_BF_decodeFile2, C_BitmapFactory, "decodeFile", "(Ljava/lang/String;)Landroid/graphics/Bitmap;");

static void aeui_set_file_icon(JNIEnv* env, AeuiWidget* w, const char* path) {
    JV(w->view, M_IV_setImageDrawable, (jobject)NULL);
    if (!path || !*path) return;
    struct stat st;
    int is_dir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) || path[strlen(path) - 1] == '/';
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '/');
    char ext[32] = "";
    if (!is_dir && dot && (!slash || dot > slash)) {
        snprintf(ext, sizeof(ext), "%s", dot + 1);
        for (char* p = ext; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
    char mime[128] = "";
    if (is_dir) snprintf(mime, sizeof(mime), "inode/directory");
    else if (ext[0]) {
        jobject mtm = JSO(M_MTM_getSingleton);
        jobject m = mtm ? JO(mtm, M_MTM_getMime, aeui_jstring(env, ext)) : NULL;
        if (m) aeui_charseq_into(env, m, mime, (int)sizeof(mime));
    }
    if (!strncmp(mime, "image/", 6)) {
        jobject bmp = JSO(M_BF_decodeFile2, aeui_jstring(env, path));
        if (bmp) { JV(w->view, M_IV_setImageBitmap, bmp); return; }
    }
    if (mime[0] && !is_dir && g_activity) {
        jobject pm = JO(g_activity, M_Ctx_getPackageManager);
        jobject uri = JSO(M_Uri_parse, aeui_jstring(env, "file:///aeui-icon"));
        jobject in = uri ? JNEW(M_Intent_init2, aeui_jstring(env, "android.intent.action.VIEW"), uri) : NULL;
        if (pm && in) {
            JO(in, M_Intent_setDataAndType, uri, aeui_jstring(env, mime));
            jobject ri = JO(pm, M_PM_resolveActivity, in, (jint)0x00010000 /* MATCH_DEFAULT_ONLY */);
            jobject icon = ri ? JO(ri, M_RI_loadIcon, pm) : NULL;
            if (icon) { JV(w->view, M_IV_setImageDrawable, icon); return; }
        }
    }
    // The framework's own glyphs, by kind (it has no folder icon): a
    // listing for a directory, a picture, a play mark for media, a page for
    // anything else.
    const char* glyph = is_dir ? "ic_menu_view"
                      : !strncmp(mime, "image/", 6) ? "ic_menu_gallery"
                      : (!strncmp(mime, "audio/", 6) || !strncmp(mime, "video/", 6)) ? "ic_media_play"
                      : "ic_menu_agenda";
    int res = aeui_android_r(env, "drawable", glyph);
    if (res) JV(w->view, M_IV_setImageResource, (jint)res);
}

int aether_ui_file_icon_create(const char* path) {
    int h = image_register(NULL, 0);
    AeuiWidget* w = live_widget(h);
    JNIEnv* env = aeui_frame(32);
    if (w && env) aeui_set_file_icon(env, w, path);
    if (env) aeui_unframe(env);
    return h;
}

void aether_ui_file_icon_set(int handle, const char* path) {
    AeuiWidget* w = live_widget(handle);
    if (!w || w->type != AUI_IMAGE) return;
    JNIEnv* env = aeui_frame(32);
    if (!env) return;
    aeui_set_file_icon(env, w, path);
    aeui_unframe(env);
}

// ===========================================================================
// System tray -- NOT AVAILABLE on Android. There is no status-area tray: an
// app's persistent presence there is an ongoing notification, which is
// notify's job, not a menu-bearing icon. As on UIKit these are documented
// no-ops so the ABI links; tray_create answers 0 (no tray), and the driver's
// /tray routes find nothing.
// ===========================================================================
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) {
    (void)name; (void)boxed_left_click;
    return 0;
}
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) { (void)tray_id; (void)menu_handle; }
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) { (void)tray_id; (void)text; }
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) { (void)tray_id; (void)is_template; }
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle, const char* icon_clean,
                                            const char* icon_busy, const char* icon_alert) {
    (void)tray_id; (void)state_handle; (void)icon_clean; (void)icon_busy; (void)icon_alert;
}
void aether_ui_tray_seal_impl(int tray_id) { (void)tray_id; }

// ===========================================================================
// Driver: window-level hit testing (/window/pick).
// ===========================================================================
// The deepest visible View under (x, y) window px, as the platform's touch
// dispatch would find it (children last-drawn first).
static jobject aeui_hit(JNIEnv* env, jobject v, int x, int y, int depth) {
    if (!v || depth > 64 || JI(v, M_View_getVisibility) != 0) return NULL;
    int rx, ry, rw, rh;
    jintArray loc = (*env)->NewIntArray(env, 2);
    if (!loc) return NULL;
    JV(v, M_View_getLocationInWindow, loc);
    jint xy[2] = { 0, 0 };
    (*env)->GetIntArrayRegion(env, loc, 0, 2, xy);
    (*env)->DeleteLocalRef(env, loc);
    rx = xy[0]; ry = xy[1];
    rw = JI(v, M_View_getWidth); rh = JI(v, M_View_getHeight);
    if (x < rx || y < ry || x >= rx + rw || y >= ry + rh) return NULL;
    if ((*env)->IsInstanceOf(env, v, jcls(env, &C_ViewGroup))) {
        int n = JI(v, M_VG_getChildCount);
        for (int i = n - 1; i >= 0; i--) {
            jobject c = JO(v, M_VG_getChildAt, (jint)i);
            jobject hit = aeui_hit(env, c, x, y, depth + 1);
            if (c) (*env)->DeleteLocalRef(env, c);
            if (hit) return hit;
        }
    }
    return (*env)->NewLocalRef(env, v);
}

static void aeui_driver_pick(AetherDriverActionCtx* ctx) {
    int px = ctx->ival, py = ctx->ival2;   // window dp, as /widgets reports
    ctx->retval = 0;
    ctx->ival2 = 0;
    JNIEnv* env = aeui_frame(256);
    if (!env) return;
    jobject hit = g_host ? aeui_hit(env, g_host, aeui_dp(px), aeui_dp(py), 0) : NULL;
    for (jobject v = hit; v; ) {
        int h = aether_ui_handle_for_widget(v);
        if (h) {
            if (get_widget_type(h) == AUI_SCRIM) ctx->ival2 = 1;
            else ctx->retval = h;
            break;
        }
        jobject p = JO(v, M_View_getParent);
        v = (p && (*env)->IsInstanceOf(env, p, jcls(env, &C_View))) ? p : NULL;
    }
    aeui_unframe(env);
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
        case AETHER_DRV_WIN_RESIZE: {
            JNIEnv* env = aeui_frame(16);
            if (env) { aeui_window_resize(env, ctx->handle > 1 ? ctx->handle : 1, ctx->ival, ctx->ival2); aeui_unframe(env); }
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_WIN_KEY: {
            JNIEnv* env = aeui_frame(32);
            ctx->retval = env ? aeui_driver_key(env, ctx->sval) : 0;
            if (env) aeui_unframe(env);
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_PICK:
            aeui_driver_pick(ctx);
            ctx->result = 0;
            return;
        case AETHER_DRV_SPLIT_POS:
            if (get_widget_type(ctx->handle) != AUI_SPLITVIEW || !live_widget(ctx->handle)) { ctx->result = 3; return; }
            if (ctx->ival >= 0) aether_ui_split_set_position_impl(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_split_position_impl(ctx->handle);
            ctx->result = 0;
            return;
        case AETHER_DRV_TAB_SELECT:
            if (!tabs_of(ctx->handle)) { ctx->result = 3; return; }
            aether_ui_tabs_select(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_tabs_selected(ctx->handle);
            ctx->result = 0;
            return;
        case AETHER_DRV_CTX_MENU: {
            AeuiWidget* w = live_widget(ctx->handle);
            ctx->retval = (w && w->nctx > 0) ? 1 : 0;
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_CTX_ACTIVATE:
            ctx->retval = aeui_ctx_fire(ctx->handle, ctx->ival);
            ctx->result = 0;
            return;
        case AETHER_DRV_MENU_ACTIVATE:
            // The shared side-store: 0 fired, 3 no such item, 4 no closure.
            ctx->retval = aether_ui_menu_item_invoke(ctx->handle, ctx->sval);
            ctx->result = 0;
            return;
        case AETHER_DRV_MENU_NATIVE_ACTIVATE: {
            JNIEnv* env = aeui_frame(64);
            int r = env ? aeui_menu_native_activate(env, ctx->handle, ctx->sval) : -1;
            if (env) aeui_unframe(env);
            if (r < 0) { ctx->result = 3; return; }
            ctx->retval = r;
            ctx->result = 0;
            return;
        }
        case AETHER_DRV_TRAY_ACTIVATE:
            ctx->result = 3;   // no tray on Android
            return;
        case AETHER_DRV_CLICK:
        case AETHER_DRV_SUBMIT:
        case AETHER_DRV_DRAG:
        case AETHER_DRV_SET_TEXT:
        case AETHER_DRV_TOGGLE:
        case AETHER_DRV_SET_VALUE:
        case AETHER_DRV_FOCUS:
        case AETHER_DRV_HOVER:
        case AETHER_DRV_PRESS:
        case AETHER_DRV_RELEASE:
            break;   // handled below, against the widget
        case AETHER_DRV_CANVAS_CLICK:
        case AETHER_DRV_CANVAS_RIGHT_CLICK:
        case AETHER_DRV_CANVAS_DOUBLE_CLICK:
        case AETHER_DRV_CANVAS_MOVE:
        case AETHER_DRV_CANVAS_RELEASE:
        case AETHER_DRV_CANVAS_KEY:
        case AETHER_DRV_CANVAS_KEYUP:
        case AETHER_DRV_CANVAS_SCROLL:
            ctx->result = cv_driver_event(ctx);
            return;
        default:
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
        case AETHER_DRV_DRAG:
            // A press, two moves and a release through on_drag's closure.
            if (!w->wdrag) { ctx->result = 3; aeui_unframe(env); return; }
            aeui_drag_call(w->wdrag, 0, ctx->dval, ctx->dval2);
            aeui_drag_call(w->wdrag, 1, ctx->ival / 2.0, ctx->ival2 / 2.0);
            aeui_drag_call(w->wdrag, 1, (double)ctx->ival, (double)ctx->ival2);
            aeui_drag_call(w->wdrag, 2, (double)ctx->ival, (double)ctx->ival2);
            break;
        case AETHER_DRV_SUBMIT:
            // The keyboard's Done key: TextView.onEditorAction runs the
            // field's editor-action listener (AetherListener -> on_submit).
            if (w->type != AUI_TEXTFIELD && w->type != AUI_SECUREFIELD) {
                ctx->result = 3; aeui_unframe(env); return;
            }
            JV(w->view, M_TV_onEditorAction, (jint)AEUI_IME_ACTION_DONE);
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
    .widget_classes_into  = hook_widget_classes_into,
    .focused_widget       = hook_focused_widget,
    .widget_a11y          = hook_widget_a11y,
    .screenshot_png       = hook_screenshot_png,
    .canvas_debug         = hook_canvas_debug,
    .canvas_paint_counters = hook_canvas_paint_counters,
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

// The app's entry point. `ae build --emit=lib` (aether >= 0.789, #2489) keeps
// an Aether program's main() as aether_main(argc, argv) -- the executable's
// prologue (argv, sandbox, the actor scheduler with this thread marked as not
// a scheduler thread) and main()'s body, returning with the actors running --
// and aether_main_exit(), the executable's epilogue. A library from an older
// ae has neither: tools/android-apk.sh then renamed main() to aeui_app_main(),
// exported as aether_aeui_app_main, which skipped the prologue. All weak, so a
// library built without any of them still loads and says why it shows nothing.
extern int aether_main(int argc, char** argv) __attribute__((weak));
extern void aether_main_exit(void) __attribute__((weak));
extern void aether_aeui_app_main(void) __attribute__((weak));

JCLASS(C_Context, "android/content/Context");
JCLASS(C_File, "java/io/File");
JMETHOD(M_Ctx_getFilesDir, C_Context, "getFilesDir", "()Ljava/io/File;");
JMETHOD(M_Ctx_getAssets, C_Context, "getAssets", "()Landroid/content/res/AssetManager;");
JMETHOD(M_Ctx_getPackageName, C_Context, "getPackageName", "()Ljava/lang/String;");
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
    if (aether_main) {
        // argv[0] is the package name, where a desktop run has the program's
        // path: one argument, as there, so args_count() agrees across
        // platforms. Static: aether_main keeps the pointer for args_get.
        static char argv0[256] = "app";
        static char* argv[2] = { argv0, NULL };
        if ((*env)->PushLocalFrame(env, 4) == 0) {
            jobject pkg = JO(activity, M_Ctx_getPackageName);
            if (pkg) aeui_charseq_into(env, pkg, argv0, (int)sizeof(argv0));
            (*env)->PopLocalFrame(env, NULL);
        }
        AEUI_LOGI("running the app's main() via aether_main (density %.2f)", (double)g_density);
        int rc = aether_main(1, argv);
        AEUI_LOGI("main() returned %d; %d widgets, body %s", rc, widget_count,
                  g_mounted ? "mounted" : "NOT mounted");
        return;
    }
    if (!aether_aeui_app_main) {
        AEUI_LOGE("no aether_main in libapp.so: build it with tools/android-apk.sh "
                  "and an ae >= 0.789");
        return;
    }
    AEUI_LOGI("running the app's main() as aeui_app_main (an older ae: no "
              "aether_main, so no argv or scheduler prologue) (density %.2f)",
              (double)g_density);
    aether_aeui_app_main();
    AEUI_LOGI("main() returned; %d widgets, body %s", widget_count,
              g_mounted ? "mounted" : "NOT mounted");
}

static void JNICALL native_lifecycle(JNIEnv* env, jclass cls, jint event, jboolean finishing) {
    (void)cls;
    switch (event) {
        case 1: AEUI_LOGI("lifecycle: pause"); break;
        case 2: AEUI_LOGI("lifecycle: resume"); break;
        case 3:
            AEUI_LOGI("lifecycle: configuration change");
            aeui_appearance_config_changed();
            break;
        case 4:
            AEUI_LOGI("lifecycle: destroy (finishing=%d)", finishing ? 1 : 0);
            if (finishing) {
                // The window closed: on the desktop that returns from the
                // run loop and the program ends. Retire every View and end
                // the process the same way, so the next launch is a fresh
                // run of main() rather than a half-alive one.
                aeui_retire_all(env);
                // The executable's epilogue: main() has "returned" (the
                // window closed), so drain the program's actors and join the
                // scheduler before the process ends, as a desktop run does.
                if (aether_main_exit) aether_main_exit();
                fflush(stdout);
                fflush(stderr);
                // _exit, not exit: exit() runs the process's C++ static
                // destructors while Android's render threads (hwuiTask*)
                // are still running, and one of them then locks a mutex a
                // destructor has destroyed -- FORTIFY aborts the process
                // with SIGABRT on every finish. The program's own shutdown
                // is the aether_main_exit above; nothing else is owed.
                _exit(0);
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
    // Events whose handle is not a widget's: a menu's, an overlay's, a
    // window's, the activity's.
    switch (kind) {
        case AEUI_EV_MENU:
            if (b == 1) aeui_ctx_fire(handle, a - 1);   // a context menu: handle = the widget
            else aeui_menu_fire_id(a);                 // the options menu, a popup
            return;
        case AEUI_EV_MENU_OPEN: {
            // A dialog window's menu-bar title: its menu, anchored to the
            // bar it is in.
            if ((*env)->PushLocalFrame(env, 16) != 0) return;
            jobject anchor = NULL;
            for (int i = 0; i < nwrecs && !anchor; i++)
                if (wrecs[i].live && wrecs[i].menubar) anchor = wrecs[i].menubar;
            aeui_popup_menu(env, handle, anchor ? anchor : g_host);
            (*env)->PopLocalFrame(env, NULL);
            return;
        }
        case AEUI_EV_SCRIM:
            aeui_scrim_tapped(handle);
            return;
        case AEUI_EV_DISMISS:
            if ((*env)->PushLocalFrame(env, 32) != 0) return;
            aeui_dialog_gone(env, handle, a);
            (*env)->PopLocalFrame(env, NULL);
            return;
        case AEUI_EV_FILE_DROP: {
            char* paths = aeui_charseq_dup(env, s);
            aether_ui_window_file_drop_deliver(paths);
            free(paths);
            return;
        }
        default:
            break;
    }
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
        case AEUI_EV_SUBMIT: {
            char* text = aeui_charseq_dup(env, s);
            if (w->submit && w->submit->fn)
                ((void (*)(void*, const char*))w->submit->fn)(w->submit->env, text);
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
            if (w->face) aeui_place_face(env, handle);
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
        case AEUI_EV_TAB:
            aether_ui_tabs_select(handle, a);
            break;
        case AEUI_EV_CONTEXT:
            if ((*env)->PushLocalFrame(env, 32) != 0) break;
            aeui_ctx_open(env, handle);
            (*env)->PopLocalFrame(env, NULL);
            break;
        case AEUI_EV_DRAG:
            if ((*env)->PushLocalFrame(env, 32) != 0) break;
            aeui_split_drag(env, handle, a, b);
            (*env)->PopLocalFrame(env, NULL);
            break;
        case AEUI_EV_WHEEL:
            aether_ui_fire_scroll(handle, a);
            break;
        case AEUI_EV_WDRAG: {
            double px = (double)(short)((unsigned)b >> 16), py = (double)(short)(b & 0xFFFF);
            aeui_drag_call(w->wdrag, a, aeui_px_to_dp(px), aeui_px_to_dp(py));
            break;
        }
        case AEUI_EV_SURFACE:
            if ((*env)->PushLocalFrame(env, 16) != 0) break;
            if (w->type == AUI_GPUVIEW) aeui_gpu_surface(env, handle, a, b);
            else aeui_native_view_surface(env, handle, a, b);
            (*env)->PopLocalFrame(env, NULL);
            break;
        case AEUI_EV_ROW_DRAG: {
            if ((*env)->PushLocalFrame(env, 16) != 0) break;
            char idx[16];
            snprintf(idx, sizeof(idx), "%d", w->row_index);
            aeui_start_drag(env, handle, "aeui-row", idx, 0);
            (*env)->PopLocalFrame(env, NULL);
            break;
        }
        case AEUI_EV_ROW_DROP:
            aether_ui_fire_row_drop(handle, a);
            break;
        case AEUI_EV_FILE_DRAG:
            if (!w->drag_path || (*env)->PushLocalFrame(env, 16) != 0) break;
            aeui_start_drag(env, handle, "aeui-file", w->drag_path, 1);
            (*env)->PopLocalFrame(env, NULL);
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
    g_listener_init3           = M(J.Listener, "<init>", "(III)V");
    g_activity_pick            = M(shim, "pick", "(ILjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    // The shim's other classes, while the app's class loader is in effect.
    g_vseek_class = aeui_find_class(env, "dev/aether/ui/AetherVSeekBar");
    if (g_vseek_class) g_vseek_init = M(g_vseek_class, "<init>", "(Landroid/content/Context;)V");
    g_scroll_class = aeui_find_class(env, "dev/aether/ui/AetherScroll");
    if (g_scroll_class) g_scroll_init = M(g_scroll_class, "<init>", "(Landroid/content/Context;)V");
    g_host_class = aeui_find_class(env, "dev/aether/ui/AetherHost");
    if (g_host_class) g_host_init = M(g_host_class, "<init>", "(Landroid/content/Context;)V");
    g_wrap_class = aeui_find_class(env, "dev/aether/ui/AetherWrap");
    if (g_wrap_class) g_wrap_init = M(g_wrap_class, "<init>", "(Landroid/content/Context;II)V");
    g_adapter_class = aeui_find_class(env, "dev/aether/ui/AetherListAdapter");
    if (g_adapter_class) {
        g_adapter_init = M(g_adapter_class, "<init>", "(I)V");
        g_adapter_setCount = M(g_adapter_class, "setCount", "(I)V");
        static const JNINativeMethod list_natives[] = {
            { "nativeListRow", "(II)Landroid/view/View;", (void*)native_list_row },
            { "nativeListScrap", "(ILandroid/view/View;)V", (void*)native_list_scrap },
        };
        if ((*env)->RegisterNatives(env, g_adapter_class, list_natives, 2) != JNI_OK) {
            aeui_check(env, "RegisterNatives(AetherListAdapter)");
            g_adapter_init = NULL;   // no native list: vlist composes its own window
        }
    }
    // The canvas's View and its natives.
    g_canvas_class = aeui_find_class(env, "dev/aether/ui/AetherCanvas");
    if (g_canvas_class) {
        g_canvas_init = M(g_canvas_class, "<init>", "(Landroid/content/Context;I)V");
        static const JNINativeMethod canvas_natives[] = {
            { "nativeCanvasDraw", "(ILandroid/graphics/Canvas;II)V", (void*)native_canvas_draw },
            { "nativeCanvasEvent", "(IIIFFFFI)Z", (void*)native_canvas_event },
        };
        if ((*env)->RegisterNatives(env, g_canvas_class, canvas_natives, 2) != JNI_OK) {
            aeui_check(env, "RegisterNatives(AetherCanvas)");
            g_canvas_init = NULL;   // canvas_create answers 0
        }
    }
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
        { "nativeKey", "(IIII)Z", (void*)native_key },
        { "nativeOptionsMenu", "(Landroid/view/Menu;)Z", (void*)native_options_menu },
        { "nativeNotificationTap", "(I)V", (void*)native_notification_tap },
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
