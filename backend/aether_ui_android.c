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
// STATUS: STAGE 1 (the skeleton) -- 66 of the 327 ABI functions the UIKit
// backend exports are implemented for real; the other 261 are STUBS. Stage 1
// proves the whole chain (`--emit=lib`, JNI, the looper bridge, packaging,
// the driver over `adb forward`) with examples/counter and
// tests/counter/spec_counter.ae. Stage 2 replaces the stubs pass by pass,
// with the spec matrix as the ratchet, until none remain (the
// backend-parity rule).
//
// Real in stage 1:
//   registry   register_widget get_widget handle_for_widget backend_name_impl
//              widget_kind_impl widget_parent_impl widget_window_impl
//   lifecycle  app_create app_set_body app_run_raw app_run_headless_impl
//              surface_container_new_impl surface_run_impl
//              surface_note_interactive_impl surface_diag_count_impl
//              register_deferred_flush_impl surface_flush_deferred_impl
//   layout     vstack_create hstack_create spacer_create divider_create
//              widget_add_child_ctx
//   widgets    text_create text_set_string text_get_wrap text_get_anchor
//              text_get_truncate button_create button_create_plain
//              button_set_label
//   state      state_create state_create_s state_create_i state_create_b
//              state_get state_get_s state_get_i state_get_b state_type
//              state_set state_set_s state_set_i state_set_b
//              state_create_list state_get_list state_set_list
//              state_list_rev state_bind_text bind_text_impl
//   driver     enable_test_server_impl enable_test_server_ctx, and the
//              per-widget readbacks the /widgets route makes on every
//              widget: styled_bg_impl styled_fg_impl styled_opacity_impl
//              styled_border_impl styled_weight_impl
//              styled_font_family_impl state_style_impl placeholder_impl
//              window_count_impl window_is_open_impl window_title_impl
//              (no styling is applied yet, so "none" is the EFFECTIVE
//              answer, not a placeholder); driver hooks for enumeration,
//              children, geometry, enabled, click, set_text, state, and
//              GET /screenshot (the decor view drawn to a PNG)
//   loop       worker_poster_install_impl on_ui_thread_impl
//              timer_create_impl timer_cancel_impl
//
// STUBS (each calls aeui_android_unimplemented(__func__), which logs the
// name once through liblog, and returns a neutral value: 0, 0.0, NULL, or a
// fresh empty string). The full list is generated into the STUBS section at
// the end of this file; by family:
//   a11y       a11y_get_impl a11y_set_description_impl a11y_set_label_impl
//              a11y_set_role_impl
//   alert      alert_impl
//   app        app_quit_impl
//   bind       bind_enabled_impl bind_hidden_impl bind_value
//   button     button_set_disclosure button_set_disclosure_ctx
//              button_set_flat button_set_flat_ctx
//   canvas     canvas_arc_impl canvas_begin_path_impl canvas_clear_impl
//              canvas_clip_rect_impl canvas_close_path_impl
//              canvas_cmd_count_impl canvas_create_impl
//              canvas_draw_image_borrowed_impl canvas_draw_image_impl
//              canvas_draw_image_impl_ptr
//              canvas_draw_image_scaled_borrowed_impl
//              canvas_draw_image_scaled_impl
//              canvas_draw_image_scaled_impl_ptr canvas_fill_impl
//              canvas_fill_linear_gradient_impl
//              canvas_fill_radial_gradient_impl canvas_fill_rect_impl
//              canvas_fill_text_impl canvas_gesture_probe_impl
//              canvas_get_widget canvas_group_begin_impl
//              canvas_group_end_impl canvas_line_to_impl canvas_move_to_impl
//              canvas_on_click_impl canvas_on_key_impl
//              canvas_on_key_release_impl canvas_on_move_impl
//              canvas_on_release_impl canvas_on_resize_impl
//              canvas_on_scroll_impl canvas_painted_pixels_impl
//              canvas_read_pixel_impl canvas_redraw_impl
//              canvas_render_range_rgba_impl canvas_reset_clip_impl
//              canvas_set_clip_rects_impl canvas_stroke_impl
//              canvas_stroke_text_impl canvas_write_png_impl
//   clear      clear_children_impl
//   clipboard  clipboard_read_impl clipboard_write_impl
//   close      close_window_by_handle_impl
//   context    context_menu_item_accel_impl context_menu_item_impl
//   dark       dark_mode_check
//   file       file_icon_create file_icon_set file_open file_pick_folder
//              file_save
//   fire       fire_appearance fire_double_click fire_redo fire_row_drop
//              fire_scroll fire_undo
//   focus      focus_impl
//   focused    focused_widget
//   font       font_ascent font_descent font_height
//   form       form_create form_section_create
//   get        get_height_impl get_min_height_impl get_min_width_impl
//              get_width_impl
//   gpuview    gpuview_available_impl gpuview_create_impl gpuview_get_widget
//              gpuview_on_realize_impl gpuview_on_render_impl
//              gpuview_on_resize_impl gpuview_read_pixel_impl
//              gpuview_request_render_impl
//   grid       grid_create grid_place grid_set_uniform
//   image      image_create image_from_bytes image_get_fill image_get_tint
//              image_has_content image_set_fill image_set_size image_set_tint
//   match      match_parent_height match_parent_width
//   menu       menu_add_item menu_add_separator menu_bar_add_menu
//              menu_bar_attach menu_bar_attach_window menu_bar_create
//              menu_create menu_item_set_label menu_popup
//   modifiers  modifiers_impl
//   native     native_list_available_impl native_list_create_impl
//              native_list_first_visible_impl native_list_scroll_to_impl
//              native_list_set_count_impl native_list_set_row_builder_impl
//              native_view_available_impl native_view_create_impl
//              native_view_get_widget native_view_handle_impl
//              native_view_kind_impl native_view_on_realize_impl
//              native_view_on_resize_impl
//   navstack   navstack_create navstack_depth navstack_pop navstack_push
//   notify     notify_full_impl notify_impl notify_request_permission_impl
//   on         on_click_impl on_double_click_impl on_hover_impl
//              on_layout_impl
//   open       open_url_impl
//   overlay    overlay_close_impl overlay_count_impl overlay_exit_played_impl
//              overlay_is_exiting_impl overlay_is_live_impl
//              overlay_is_modal_impl overlay_material_effective_impl
//              overlay_open_impl overlay_set_material_impl
//              overlay_set_on_dismiss_impl overlay_set_transition_impl
//   picker     picker_add_item picker_create picker_get_selected
//              picker_set_selected
//   progressbar progressbar_create progressbar_set_fraction
//   remove     remove_child_impl
//   row        row_drag_reorder_impl
//   scrollview scrollview_create
//   seal       seal_subtree_impl seal_widget_impl
//   securefield securefield_create
//   set        set_alignment set_bg_color set_bg_color_ctx set_bg_gradient
//              set_border set_corner_radius set_corner_radius_ctx
//              set_distribution set_edge_insets set_enabled set_enabled_ctx
//              set_focusable_impl set_font_bold set_font_bold_ctx
//              set_font_family set_font_size set_font_size_ctx set_height
//              set_height_impl set_margin set_margin_ctx set_min_height_impl
//              set_min_width_impl set_onclick_ctx set_opacity set_opacity_ctx
//              set_rtl set_state_style set_text_color set_text_color_ctx
//              set_tooltip set_tooltip_ctx set_width set_width_impl
//   sheet      sheet_create_impl sheet_dismiss_impl sheet_present_impl
//              sheet_set_body_impl
//   shortcut   shortcut_chord_impl shortcut_impl shortcut_when_impl
//   slider     slider_create slider_get_value slider_set_value
//   split      split_position_impl split_set_position_impl
//   splitview  splitview_create
//   tab        tab_add
//   tabs       tabs_count tabs_create tabs_select tabs_selected
//              tabs_set_on_change
//   text       text_measure text_set_anchor text_set_truncate
//              text_wrapped_create
//   textarea   textarea_create textarea_get_text textarea_set_text
//   textfield  textfield_create textfield_get_text textfield_set_text
//   toast      toast_impl
//   toggle     toggle_create toggle_get_active toggle_set_active
//              toggle_set_group
//   tray       tray_create_impl tray_seal_impl tray_set_icon_for_state_impl
//              tray_set_icon_template_impl tray_set_menu_impl
//              tray_set_tooltip_impl
//   vg         vg_tooltip_drawn_impl vg_tooltip_hide_impl
//              vg_tooltip_show_impl
//   vlist      vlist_attach_scroll_impl
//   watch      watch_appearance_impl
//   widget     widget_add_css_class_impl widget_apply_css_impl
//              widget_classes_impl widget_count_impl widget_drag_payload_impl
//              widget_draggable_file_impl widget_remove_css_class_impl
//              widget_set_child_impl widget_set_hidden widget_weight_impl
//   window     window_close_impl window_create_impl window_file_drop_deliver
//              window_key_deliver window_on_file_drop_impl window_on_key_impl
//              window_set_body_impl window_set_title_impl window_show_impl
//   wrap       wrap_create
//   zstack     zstack_create
//
// Stage-1 limitations beyond the stubs: a View is retired (its registry slot
// NULLed and its global reference dropped) only when the activity finishes,
// since remove/clear_children are stubs; there is one window; Back finishes
// the activity, which ends the program as closing a desktop window does.
// ===========================================================================

#include <jni.h>
#include <android/log.h>
#include <android/looper.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
           TextView, Button, Activity, String, CharSequence, ClickListener;
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
    jmethodID ClickListener_init;
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
// parallel arrays hold what the driver and the layout need without a JNI
// round trip: the type tag, the registered parent, the child count, a
// stack's spacing.
// ===========================================================================
enum {
    AUI_UNKNOWN = 0,
    AUI_TEXT, AUI_BUTTON, AUI_TOGGLE, AUI_SLIDER, AUI_PICKER,
    AUI_TEXTFIELD, AUI_SECUREFIELD, AUI_TEXTAREA,
    AUI_PROGRESSBAR, AUI_DIVIDER, AUI_SCROLLVIEW,
    AUI_VSTACK, AUI_HSTACK, AUI_ZSTACK, AUI_SPACER,
    AUI_CANVAS, AUI_IMAGE,
    AUI_TABS, AUI_NAVSTACK, AUI_SPLITVIEW, AUI_WRAP, AUI_GRID, AUI_FORM
};

typedef struct {
    jobject view;      // global ref, NULL once retired
    int type;
    int parent;        // registered parent handle, 0 = none
    int child_count;
    int spacing;       // stacks: the DSL's spacing (dp)
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
    widget_count++;
    return widget_count;
}

static AeuiWidget* widget_at(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return &widgets[handle - 1];
}

static jobject view_of(int handle) {
    AeuiWidget* w = widget_at(handle);
    return w ? w->view : NULL;
}

static int get_widget_type(int handle) {
    AeuiWidget* w = widget_at(handle);
    return w ? w->type : AUI_UNKNOWN;
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

// The kind vocabulary every backend reports (GTK4's widget_type_name()).
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
    // The window's content inset, as the desktop backends pad the window body.
    int pad = aeui_dp(16);
    (*env)->CallVoidMethod(env, root, J.View_setPadding, pad, pad, pad, pad);
    jobject lp = (*env)->NewObject(env, J.FrameLayoutParams, J.FrameLayoutParams_init,
                                   (jint)-1 /* MATCH_PARENT */, (jint)-1);
    (*env)->CallVoidMethod(env, g_host, J.ViewGroup_addView, root, lp);
    aeui_check(env, "host.addView");
    (*env)->DeleteLocalRef(env, lp);
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
// Layout -- LinearLayout for vstack/hstack. LinearLayout has no spacing of
// its own, so a stack's spacing becomes the leading margin of every child
// after the first, in dp. Children fill the cross axis of a vstack (as a
// UIStackView/NSStackView with fill alignment does); a spacer takes the
// main-axis slack through layout weight.
// ===========================================================================
enum { LL_HORIZONTAL = 0, LL_VERTICAL = 1 };
enum { LP_MATCH_PARENT = -1, LP_WRAP_CONTENT = -2 };

static int make_stack(int orientation, int spacing, int type) {
    JNIEnv* env = aeui_env();
    if (!env || !g_activity) return 0;
    jobject ll = (*env)->NewObject(env, J.LinearLayout, J.LinearLayout_init, g_activity);
    if (aeui_check(env, "new LinearLayout") || !ll) return 0;
    (*env)->CallVoidMethod(env, ll, J.LinearLayout_setOrientation, (jint)orientation);
    aeui_check(env, "LinearLayout.setOrientation");
    int h = register_widget_typed(env, ll, type);
    (*env)->DeleteLocalRef(env, ll);
    AeuiWidget* w = widget_at(h);
    if (w) w->spacing = spacing;
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

void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    AeuiWidget* p = widget_at(parent_handle);
    AeuiWidget* c = widget_at(child_handle);
    if (!p || !c || !p->view || !c->view) return;
    JNIEnv* env = aeui_env();
    if (!env) return;
    if (p->type != AUI_VSTACK && p->type != AUI_HSTACK) {
        // Every other container kind is a stage-2 widget (a stub returning
        // handle 0), so nothing else can be a parent yet.
        AEUI_LOGW("add_child: parent %d (%s) is not a stack yet", parent_handle,
                  aeui_kind_name(p->type));
        return;
    }
    int vertical = p->type == AUI_VSTACK;
    int w, h;
    float weight = 0.0f;
    if (c->type == AUI_SPACER) {
        // Zero on the main axis, then weight takes the slack. Never
        // WRAP_CONTENT on the cross axis: a bare View asked to wrap measures
        // to all the space it is offered, so a wrap-height spacer in an
        // hstack stretched the row to the bottom of the screen. MATCH_PARENT
        // fills the row as laid out (LinearLayout's uniform-height pass).
        w = vertical ? LP_MATCH_PARENT : 0;
        h = vertical ? 0 : LP_MATCH_PARENT;
        weight = 1.0f;
    } else if (c->type == AUI_DIVIDER) {
        w = vertical ? LP_MATCH_PARENT : aeui_dp(1);
        h = vertical ? aeui_dp(1) : LP_MATCH_PARENT;
    } else {
        w = vertical ? LP_MATCH_PARENT : LP_WRAP_CONTENT;
        h = LP_WRAP_CONTENT;
    }
    jobject lp = (*env)->NewObject(env, J.LinearLayoutParams, J.LinearLayoutParams_init,
                                   (jint)w, (jint)h, (jfloat)weight);
    if (aeui_check(env, "new LinearLayout.LayoutParams") || !lp) return;
    if (p->child_count > 0 && p->spacing > 0) {
        int m = aeui_dp(p->spacing);
        (*env)->CallVoidMethod(env, lp, J.MarginLayoutParams_setMargins,
                               vertical ? 0 : m, vertical ? m : 0, 0, 0);
        aeui_check(env, "setMargins");
    }
    (*env)->CallVoidMethod(env, p->view, J.ViewGroup_addView, c->view, lp);
    aeui_check(env, "ViewGroup.addView");
    (*env)->DeleteLocalRef(env, lp);
    p->child_count++;
    c->parent = parent_handle;
}

// ===========================================================================
// Text (TextView) and button (Button)
// ===========================================================================
static void set_text_on(JNIEnv* env, jobject view, const char* text) {
    jstring s = aeui_jstring(env, text);
    (*env)->CallVoidMethod(env, view, J.TextView_setText, s);
    aeui_check(env, "TextView.setText");
    if (s) (*env)->DeleteLocalRef(env, s);
}

int aether_ui_text_create(const char* text) {
    JNIEnv* env = aeui_env();
    if (!env || !g_activity) return 0;
    jobject tv = (*env)->NewObject(env, J.TextView, J.TextView_init, g_activity);
    if (aeui_check(env, "new TextView") || !tv) return 0;
    set_text_on(env, tv, text);
    int h = register_widget_typed(env, tv, AUI_TEXT);
    (*env)->DeleteLocalRef(env, tv);
    return h;
}

// A label or a button: both are TextViews.
void aether_ui_text_set_string(int handle, const char* text) {
    int t = get_widget_type(handle);
    if (t != AUI_TEXT && t != AUI_BUTTON) return;
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (v && env) set_text_on(env, v, text);
}

// TextView as created: single-line-unbounded (no wrap width set), start
// gravity, no ellipsis. Nothing changes them until text_set_anchor /
// _truncate / text_wrapped land, so these ARE the effective values.
int aether_ui_text_get_wrap(int handle) { (void)handle; return 0; }
int aether_ui_text_get_anchor(int handle) { (void)handle; return 0; }
int aether_ui_text_get_truncate(int handle) { (void)handle; return 0; }

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

static void wire_click(JNIEnv* env, jobject view, void* boxed_closure) {
    if (!boxed_closure) return;
    jobject l = (*env)->NewObject(env, J.ClickListener, J.ClickListener_init,
                                  (jlong)(intptr_t)boxed_closure);
    if (aeui_check(env, "new AetherClickListener") || !l) return;
    (*env)->CallVoidMethod(env, view, J.View_setOnClickListener, l);
    aeui_check(env, "View.setOnClickListener");
    (*env)->DeleteLocalRef(env, l);
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    JNIEnv* env = aeui_env();
    if (!env || !g_activity) return 0;
    jobject b = make_button(env, label);
    if (!b) return 0;
    wire_click(env, b, boxed_closure);
    int h = register_widget_typed(env, b, AUI_BUTTON);
    (*env)->DeleteLocalRef(env, b);
    return h;
}

int aether_ui_button_create_plain(const char* label) {
    return aether_ui_button_create(label, NULL);
}

void aether_ui_button_set_label(int handle, const char* label) {
    if (get_widget_type(handle) != AUI_BUTTON) return;
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (v && env) set_text_on(env, v, label);
}

// ===========================================================================
// Reactive state -- typed cells and text bindings. The same platform-free
// subsystem every backend carries (lifted from the UIKit backend): a set
// re-renders the bound labels through text_set_string, and observers stay in
// the DSL (aether_ui_state_notify).
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

typedef struct {
    int state_handle, widget_handle;
    char* prefix;
    char* suffix;
    int decimals;
} TextBinding;

static StateCell* state_cells = NULL;
static int state_count = 0, state_capacity = 0;
static TextBinding* text_bindings = NULL;
static int text_binding_count = 0, text_binding_capacity = 0;

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

static void apply_text_binding(TextBinding* b) {
    StateCell* c = state_cell(b->state_handle);
    if (!c) return;
    char val[256];
    state_render_value(c, b->decimals, val, sizeof(val));
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s%s", b->prefix, val, b->suffix);
    aether_ui_text_set_string(b->widget_handle, buf);
}

static void update_bindings(int state_handle) {
    for (int i = 0; i < text_binding_count; i++)
        if (text_bindings[i].state_handle == state_handle)
            apply_text_binding(&text_bindings[i]);
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

static TextBinding* text_binding_new(int state_handle, int widget_handle,
                                     const char* prefix, const char* suffix, int decimals) {
    if (text_binding_count >= text_binding_capacity) {
        int cap = text_binding_capacity == 0 ? 32 : text_binding_capacity * 2;
        TextBinding* nb = (TextBinding*)realloc(text_bindings, sizeof(TextBinding) * (size_t)cap);
        if (!nb) return NULL;
        text_bindings = nb;
        text_binding_capacity = cap;
    }
    TextBinding* b = &text_bindings[text_binding_count++];
    b->state_handle = state_handle;
    b->widget_handle = widget_handle;
    b->prefix = strdup(prefix ? prefix : "");
    b->suffix = strdup(suffix ? suffix : "");
    b->decimals = decimals;
    return b;
}

void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix) {
    TextBinding* b = text_binding_new(state_handle, text_handle, prefix, suffix, -1);
    if (b) apply_text_binding(b);
}

void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals) {
    TextBinding* b = text_binding_new(state_handle, widget_handle, "", "", decimals);
    if (b) apply_text_binding(b);
}

// ===========================================================================
// Styling readbacks the driver reports per widget. Nothing applies a style on
// Android yet (the style setters are stubs), so "unset" is the effective
// state: -1 / "" exactly as an unstyled widget reads on every other backend.
// ===========================================================================
int aether_ui_styled_bg_impl(int handle) { (void)handle; return -1; }
int aether_ui_styled_fg_impl(int handle) { (void)handle; return -1; }
int aether_ui_styled_opacity_impl(int handle) { (void)handle; return -1; }
int aether_ui_styled_border_impl(int handle) { (void)handle; return -1; }
const char* aether_ui_styled_weight_impl(int handle) { (void)handle; return ""; }
const char* aether_ui_styled_font_family_impl(int handle) { (void)handle; return ""; }
int aether_ui_state_style_impl(int handle, int state) { (void)handle; (void)state; return -1; }
// No widget that takes a hint (textfield/securefield/textarea) exists yet.
const char* aether_ui_placeholder_impl(int handle) { (void)handle; return ""; }

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

static void hook_widget_text_into(int handle, char* buf, int bufsize) {
    buf[0] = '\0';
    int t = get_widget_type(handle);
    if (t != AUI_TEXT && t != AUI_BUTTON) return;
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (!v || !env) return;
    // Read back from the View, not from what was last set: the driver
    // reports what is on screen.
    jobject cs = (*env)->CallObjectMethod(env, v, J.TextView_getText);
    if (aeui_check(env, "TextView.getText") || !cs) return;
    aeui_charseq_into(env, cs, buf, bufsize);
    (*env)->DeleteLocalRef(env, cs);
}

static int hook_widget_visible(int handle) {
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (!v || !env) return 0;
    jint vis = (*env)->CallIntMethod(env, v, J.View_getVisibility);
    aeui_check(env, "View.getVisibility");
    return vis == 0;   // View.VISIBLE
}

static int hook_widget_enabled(int handle) {
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (!v || !env) return 0;
    jboolean on = (*env)->CallBooleanMethod(env, v, J.View_isEnabled);
    aeui_check(env, "View.isEnabled");
    return on ? 1 : 0;
}

static int hook_widget_parent(int handle) { return aether_ui_widget_parent_impl(handle); }

static int hook_widget_children(int handle, int* out_handles, int max) {
    int n = 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i].parent != handle || !widgets[i].view) continue;
        if (out_handles && n < max) out_handles[n] = i + 1;
        n++;
    }
    return out_handles && n > max ? max : n;
}

// Window-local pixels, as the other backends report them.
static int hook_widget_rect(int handle, int* x, int* y, int* w, int* hgt) {
    jobject v = view_of(handle);
    JNIEnv* env = aeui_env();
    if (!v || !env) return 1;
    jintArray loc = (*env)->NewIntArray(env, 2);
    if (!loc) { aeui_check(env, "NewIntArray"); return 1; }
    (*env)->CallVoidMethod(env, v, J.View_getLocationInWindow, loc);
    if (aeui_check(env, "View.getLocationInWindow")) { (*env)->DeleteLocalRef(env, loc); return 1; }
    jint xy[2] = { 0, 0 };
    (*env)->GetIntArrayRegion(env, loc, 0, 2, xy);
    (*env)->DeleteLocalRef(env, loc);
    *x = xy[0];
    *y = xy[1];
    *w = (*env)->CallIntMethod(env, v, J.View_getWidth);
    *hgt = (*env)->CallIntMethod(env, v, J.View_getHeight);
    aeui_check(env, "View.getWidth/getHeight");
    return 0;
}

// No toggle, slider or progress bar exists in stage 1; the server asks only
// for widgets of those types.
static int hook_toggle_active(int handle) { (void)handle; return 0; }
static double hook_slider_value(int handle) { (void)handle; return 0.0; }
static double hook_progressbar_fraction(int handle) { (void)handle; return 0.0; }

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
            break;   // handled below, against the widget
        default:
            // Not wired in stage 1: 404 honestly rather than pretend.
            ctx->result = 3;
            return;
    }

    jobject v = view_of(ctx->handle);
    JNIEnv* env = aeui_env();
    if (!v || !env) { ctx->result = 3; return; }
    if (ctx->handle == aether_ui_test_server_banner_handle()) { ctx->result = 2; return; }
    if (aether_ui_test_server_is_sealed(ctx->handle)) { ctx->result = 1; return; }

    if (ctx->action == AETHER_DRV_CLICK) {
        // performClick runs the View's OnClickListener exactly as a tap
        // does (and plays the click sound / accessibility event with it).
        (*env)->CallBooleanMethod(env, v, J.View_performClick);
        aeui_check(env, "View.performClick");
    } else if (ctx->action == AETHER_DRV_SET_TEXT) {
        int t = get_widget_type(ctx->handle);
        if (t == AUI_TEXT || t == AUI_BUTTON) aether_ui_text_set_string(ctx->handle, ctx->sval);
    }
    ctx->result = 0;
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
    .widget_hovered       = NULL,
    .widget_pressed       = NULL,
    .widget_parent        = hook_widget_parent,
    .toggle_active        = hook_toggle_active,
    .slider_value         = hook_slider_value,
    .progressbar_fraction = hook_progressbar_fraction,
    .dispatch_action      = hook_dispatch_action,
    .widget_children      = hook_widget_children,
    .widget_enabled       = hook_widget_enabled,
    .widget_rect          = hook_widget_rect,
    .widget_classes_into  = NULL,
    .focused_widget       = NULL,
    .widget_a11y          = NULL,
    .screenshot_png       = hook_screenshot_png,
    .canvas_debug         = NULL,
    .canvas_paint_counters = NULL,
    .run_on_ui_thread     = hook_run_on_ui_thread,
};

static int android_test_server_started = 0;

void aether_ui_enable_test_server_impl(int port, int root_handle) {
    (void)root_handle;   // banner injection is a later pass, as on UIKit
    if (android_test_server_started) return;   // idempotent (env + explicit call)
    android_test_server_started = 1;
    AEUI_LOGI("AetherUIDriver: starting on 127.0.0.1:%d (adb forward tcp:%d tcp:%d)",
              port, port, port);
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

static void JNICALL native_click(JNIEnv* env, jclass cls, jlong closure) {
    (void)env; (void)cls;
    aeui_call0((AeClosure*)(intptr_t)closure);
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
    J.ClickListener      = aeui_find_class(env, "dev/aether/ui/AetherClickListener");
    jclass shim          = aeui_find_class(env, "dev/aether/ui/AetherActivity");
    jclass margin        = aeui_find_class(env, "android/view/ViewGroup$MarginLayoutParams");
    jclass object        = aeui_find_class(env, "java/lang/Object");
    if (!J.View || !J.ViewGroup || !J.LinearLayout || !J.LinearLayoutParams ||
        !J.FrameLayoutParams || !J.TextView || !J.Button || !J.Activity ||
        !J.String || !J.CharSequence || !J.ClickListener || !shim || !margin || !object)
        return JNI_ERR;

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
    J.ClickListener_init       = M(J.ClickListener, "<init>", "(J)V");
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
        { "nativeClick", "(J)V", (void*)native_click },
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
void aether_ui_a11y_get_impl(int handle, char* role, int rolesz, char* name, int namesz, char* desc, int descsz) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_a11y_set_description_impl(int handle, const char* desc) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_a11y_set_label_impl(int handle, const char* name) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_a11y_set_role_impl(int handle, const char* role) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_alert_impl(const char* title, const char* message) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_app_quit_impl(void) { aeui_android_unimplemented(__func__); }
void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_bind_value(int state_handle, int widget_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_button_set_disclosure(int handle, int expanded) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_button_set_disclosure_ctx(void* ctx, int expanded) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_button_set_flat(int handle, int on) { aeui_android_unimplemented(__func__); }
void aether_ui_button_set_flat_ctx(void* ctx, int on) { aeui_android_unimplemented(__func__); }
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
void aether_ui_clear_children_impl(int handle) { aeui_android_unimplemented(__func__); }
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
void aether_ui_focus_impl(int handle) { aeui_android_unimplemented(__func__); }
int aether_ui_focused_widget(void) { aeui_android_unimplemented(__func__); return 0; }
double aether_ui_font_ascent(double size) { aeui_android_unimplemented(__func__); return 0.0; }
double aether_ui_font_descent(double size) { aeui_android_unimplemented(__func__); return 0.0; }
double aether_ui_font_height(double size) { aeui_android_unimplemented(__func__); return 0.0; }
int aether_ui_form_create(void) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_form_section_create(const char* title) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_get_height_impl(int handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_get_min_height_impl(int handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_get_min_width_impl(int handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_get_width_impl(int handle) { aeui_android_unimplemented(__func__); return 0; }
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
int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_grid_place(int grid_handle, int child_handle, int row, int col, int row_span, int col_span) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_grid_set_uniform(int grid_handle, int on) { aeui_android_unimplemented(__func__); }
int aether_ui_image_create(const char* filepath) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_image_from_bytes(const char* data, int length) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_image_get_fill(int handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_image_get_tint(int handle) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_image_has_content(int handle) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_image_set_fill(int handle, int mode) { aeui_android_unimplemented(__func__); }
void aether_ui_image_set_size(int handle, int width, int height) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_image_set_tint(int handle, int on, double r, double g, double b) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_match_parent_height(int handle) { aeui_android_unimplemented(__func__); }
void aether_ui_match_parent_width(int handle) { aeui_android_unimplemented(__func__); }
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
int aether_ui_modifiers_impl(void) { aeui_android_unimplemented(__func__); return 0; }
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
void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_on_layout_impl(int handle, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
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
void aether_ui_picker_add_item(int handle, const char* item) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_picker_create(void* boxed_closure) { aeui_android_unimplemented(__func__); return 0; }
int aether_ui_picker_get_selected(int handle) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_picker_set_selected(int handle, int index) { aeui_android_unimplemented(__func__); }
int aether_ui_progressbar_create(double fraction) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_row_drag_reorder_impl(int row_handle, int index, void* on_drop_closure) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_scrollview_create(void) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_seal_subtree_impl(int handle) { aeui_android_unimplemented(__func__); }
void aether_ui_seal_widget_impl(int handle) { aeui_android_unimplemented(__func__); }
int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    aeui_android_unimplemented(__func__); return 0;
}
void aether_ui_set_alignment(int handle, int alignment) { aeui_android_unimplemented(__func__); }
void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_bg_gradient(int handle, double r1, double g1, double b1, double r2, double g2, double b2, int vertical) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_border(int handle, double width, double r, double g, double b) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_corner_radius(int handle, double radius) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_corner_radius_ctx(void* ctx, double radius) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_distribution(int handle, int distribution) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_edge_insets(int handle, double top, double right, double bottom, double left) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_enabled(int handle, int enabled) { aeui_android_unimplemented(__func__); }
void aether_ui_set_enabled_ctx(void* ctx, int enabled) { aeui_android_unimplemented(__func__); }
void aether_ui_set_focusable_impl(int handle, int on) { aeui_android_unimplemented(__func__); }
void aether_ui_set_font_bold(int handle, int bold) { aeui_android_unimplemented(__func__); }
void aether_ui_set_font_bold_ctx(void* ctx, int bold) { aeui_android_unimplemented(__func__); }
void aether_ui_set_font_family(int handle, const char* family) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_font_size(int handle, double size) { aeui_android_unimplemented(__func__); }
void aether_ui_set_font_size_ctx(void* ctx, double size) { aeui_android_unimplemented(__func__); }
void aether_ui_set_height(int handle, int height) { aeui_android_unimplemented(__func__); }
void aether_ui_set_height_impl(int handle, int px) { aeui_android_unimplemented(__func__); }
void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_min_height_impl(int handle, int px) { aeui_android_unimplemented(__func__); }
void aether_ui_set_min_width_impl(int handle, int px) { aeui_android_unimplemented(__func__); }
void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_opacity(int handle, double opacity) { aeui_android_unimplemented(__func__); }
void aether_ui_set_opacity_ctx(void* ctx, double opacity) { aeui_android_unimplemented(__func__); }
void aether_ui_set_rtl(int handle, int on) { aeui_android_unimplemented(__func__); }
void aether_ui_set_state_style(int handle, int state, double br, double bg_, double bb, double fr, double fg_, double fb) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_text_color(int handle, double r, double g, double b) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_text_color_ctx(void* ctx, double r, double g, double b) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_tooltip(int handle, const char* text) { aeui_android_unimplemented(__func__); }
void aether_ui_set_tooltip_ctx(void* ctx, const char* text) {
    aeui_android_unimplemented(__func__);
}
void aether_ui_set_width(int handle, int width) { aeui_android_unimplemented(__func__); }
void aether_ui_set_width_impl(int handle, int px) { aeui_android_unimplemented(__func__); }
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
int aether_ui_slider_create(double min_val, double max_val, double initial, void* boxed_closure) {
    aeui_android_unimplemented(__func__); return 0;
}
double aether_ui_slider_get_value(int handle) { aeui_android_unimplemented(__func__); return 0.0; }
void aether_ui_slider_set_value(int handle, double value) { aeui_android_unimplemented(__func__); }
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
double aether_ui_text_measure(double size, const char* text) {
    aeui_android_unimplemented(__func__); return 0.0;
}
void aether_ui_text_set_anchor(int handle, int anchor) { aeui_android_unimplemented(__func__); }
void aether_ui_text_set_truncate(int handle, int mode) { aeui_android_unimplemented(__func__); }
int aether_ui_text_wrapped_create(const char* text, int wrap_width_px) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    aeui_android_unimplemented(__func__); return 0;
}
char* aether_ui_textarea_get_text(int handle) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
void aether_ui_textarea_set_text(int handle, const char* text) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    aeui_android_unimplemented(__func__); return 0;
}
const char* aether_ui_textfield_get_text(int handle) {
    aeui_android_unimplemented(__func__); return aeui_empty_string();
}
void aether_ui_textfield_set_text(int handle, const char* text) {
    aeui_android_unimplemented(__func__);
}
int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    aeui_android_unimplemented(__func__); return 0;
}
int aether_ui_toggle_get_active(int handle) { aeui_android_unimplemented(__func__); return 0; }
void aether_ui_toggle_set_active(int handle, int active) { aeui_android_unimplemented(__func__); }
void aether_ui_toggle_set_group(int handle, int group_with) {
    aeui_android_unimplemented(__func__);
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
void aether_ui_widget_set_hidden(int handle, int hidden) { aeui_android_unimplemented(__func__); }
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
