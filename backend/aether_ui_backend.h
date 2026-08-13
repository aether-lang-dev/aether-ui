// Aether UI Backend ABI
//
// This header declares the cross-platform widget API that every Aether UI
// backend must implement. The three backends are:
//
//   aether_ui_gtk4.c   — Linux, backed by GTK4
//   aether_ui_macos.m  — macOS, backed by AppKit
//   aether_ui_win32.c  — Windows, backed by USER32+GDI+
//
// The Aether DSL layer (module.ae) declares matching
// `extern` functions and is platform-neutral. Build-time backend selection
// happens in build.sh based on `uname -s`.
//
// Cross-platform test server (AetherUIDriver) lives in aether_ui_test_server.c
// and is linked into every backend.

#ifndef AETHER_UI_BACKEND_H
#define AETHER_UI_BACKEND_H

#include <stdint.h>

// Widget registry
int aether_ui_register_widget(void* widget);
void* aether_ui_get_widget(int handle);
// Reverse lookup: native-pointer → 1-based handle, 0 if not registered.
// O(1) amortized on Win32 (hash-backed); may be linear on other backends.
int aether_ui_handle_for_widget(void* widget);

// App lifecycle
int aether_ui_app_create(const char* title, int width, int height);
void aether_ui_app_set_body(int app_handle, int root_handle);
void aether_ui_app_run_raw(int app_handle);

// Surfaces — "DSL with Scope" destinations (window / render_to / record).
// container_new(kind) makes a detached content container (pushed as _ctx so
// children attach to it) — used as the zero-arg `with` factory for every
// surface kind. surface_run is the `window` builder body: creates the app,
// mounts the populated container, runs the loop (window only). The
// interactive/diag helpers track interactive verbs used on bounded surfaces.
int aether_ui_surface_container_new_impl(int kind);
void aether_ui_surface_run_impl(int container_handle,
                                const char* title, int width, int height);
int aether_ui_surface_note_interactive_impl(int container_handle);
int aether_ui_surface_diag_count_impl(int container_handle);

// Deferred-flush registry — AeVG `vg { … }` records shapes during its block
// (so a shape's trailing fill()/stroke() is known before the backend call)
// and registers a boxed flush closure here. The surface body drains the
// registry after its block closes (window: before the loop), so each scene's
// command buffer is built with final colours. register adds one closure;
// flush_deferred invokes all and clears.
void aether_ui_register_deferred_flush_impl(void* boxed_closure);
void aether_ui_surface_flush_deferred_impl(void);

// Widget creation
int aether_ui_text_create(const char* text);
// Multi-line wrapping label (wraps at wrap_width_px, word boundaries) + text
// anchor (0=start 1=middle 2=end). Driver reads back wrap/anchor for specs.
int aether_ui_text_wrapped_create(const char* text, int wrap_width_px);
void aether_ui_text_set_anchor(int handle, int anchor);
int aether_ui_text_get_wrap(int handle);
int aether_ui_text_get_anchor(int handle);
// Driver-invoke a widget's double-click closure (headless-safe). 1 if fired.
int aether_ui_fire_double_click(int handle);
/* Row drag-reorder: make `row` a drag source (index payload) + drop target;
 * on drop of another row, fire on_drop(source_index). fire_row_drop drives it
 * headlessly (no seat). */
void aether_ui_row_drag_reorder_impl(int row_handle, int index, void* on_drop);
int  aether_ui_fire_row_drop(int row_handle, int src_index);

// vlist native scroll: attach an on_scroll(dy) closure to a container's native
// scroll (wheel/drag) — dy>0 scrolls toward the end. fire_scroll drives it
// headlessly (a real wheel can't be synthesised off-thread), mirroring the
// row-drop fire path. DSL wrapper: vlist_scrollable() in ui/module.ae.
void aether_ui_vlist_attach_scroll_impl(int container_handle, void* on_scroll);
int  aether_ui_fire_scroll(int container_handle, int dy);
int aether_ui_button_create(const char* label, void* boxed_closure);
int aether_ui_button_create_plain(const char* label);
void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure);
int aether_ui_vstack_create(int spacing);
int aether_ui_hstack_create(int spacing);
int aether_ui_spacer_create(void);
int aether_ui_divider_create(void);

// Input widgets (Group 2)
int aether_ui_textfield_create(const char* placeholder, void* boxed_closure);
void aether_ui_textfield_set_text(int handle, const char* text);
const char* aether_ui_textfield_get_text(int handle);

int aether_ui_securefield_create(const char* placeholder, void* boxed_closure);

int aether_ui_toggle_create(const char* label, void* boxed_closure);
void aether_ui_toggle_set_active(int handle, int active);
int aether_ui_toggle_get_active(int handle);

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure);
void aether_ui_slider_set_value(int handle, double value);
double aether_ui_slider_get_value(int handle);

int aether_ui_picker_create(void* boxed_closure);
void aether_ui_picker_add_item(int handle, const char* item);
void aether_ui_picker_set_selected(int handle, int index);
int aether_ui_picker_get_selected(int handle);

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure);
void aether_ui_textarea_set_text(int handle, const char* text);
char* aether_ui_textarea_get_text(int handle);

int aether_ui_scrollview_create(void);
int aether_ui_progressbar_create(double fraction);
void aether_ui_progressbar_set_fraction(int handle, double fraction);

// Layout containers (Group 3)
// splitview: native two-pane draggable splitter (GtkPaned / NSSplitView /
// win32 stub). Position get/set in px from the start edge; -1 = not a split.
int aether_ui_splitview_create(int vertical);
int aether_ui_split_position_impl(int handle);
void aether_ui_split_set_position_impl(int handle, int px);
// tabs: native tab strip over a page stack (GtkStackSwitcher+GtkStack /
// NSTabView / win32 stub). tabs_create takes the boxed on_change closure
// (fired with the new index); tab_add returns the page's inner container
// handle so its DSL block's children attach inside. selected/count are
// -1/0 when the handle isn't a tabs composite.
int aether_ui_tabs_create(void* boxed_closure);
int aether_ui_tab_add(int tabs_handle, const char* title);
int aether_ui_tabs_selected(int tabs_handle);
int aether_ui_tabs_count(int tabs_handle);
void aether_ui_tabs_select(int tabs_handle, int index);
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure);
// weight: proportional main-axis sharing among stack children (Flutter
// Expanded semantics — unweighted children keep natural size, weighted
// ones split the leftover). on_layout: cb(w, h) fires after the stack's
// allocation changes (the GeometryReader). GTK4 only; no-op stubs
// elsewhere.
void aether_ui_widget_weight_impl(int handle, int n);
// Lay an hstack's children right-to-left (on=1) or back to LTR (on=0).
void aether_ui_set_rtl(int handle, int on);
void aether_ui_on_layout_impl(int handle, void* boxed_closure);
// wrap: flow container, children wrap to the next line when width runs
// out (GtkFlowBox; plain stack stub elsewhere).
int aether_ui_wrap_create(void);
int aether_ui_zstack_create(void);
int aether_ui_form_create(void);
int aether_ui_form_section_create(const char* title);
int aether_ui_navstack_create(void);
void aether_ui_navstack_push(int handle, const char* title, int body_handle);
void aether_ui_navstack_pop(int handle);

// Styling (Group 4)
// Stylesheet support (ui.create_styles / apply_styles — AeCS, the swiby-shaped
// CSS-alike layer; see docs/design/styling.md). The DSL walks the live widget
// tree in-process and needs kind/parent/classes per widget; styled_bg/fg read
// back the last EXPLICITLY-set colors (packed 0xRRGGBB, -1 = unset) so the
// driver JSON can prove a theme swap actually restyled the backend.
int aether_ui_widget_count_impl(void);
const char* aether_ui_widget_kind_impl(int handle);   // "" for dead/unknown
int aether_ui_widget_parent_impl(int handle);         // 0 = none/top
const char* aether_ui_widget_classes_impl(int handle); // space-separated, "" none
int aether_ui_styled_bg_impl(int handle);   // packed 0xRRGGBB or -1
int aether_ui_styled_fg_impl(int handle);   // packed 0xRRGGBB or -1
// Widget-level font family (closes the AeCS v1 gap): the family string is
// handed to the platform verbatim — generic CSS families ("monospace",
// "serif") resolve on GTK4/macOS; on win32 prefer real face names
// ("Consolas"). styled_* read back what was explicitly set ("" = never).
void aether_ui_set_font_family(int handle, const char* family);
const char* aether_ui_styled_font_family_impl(int handle);   // "" unset
const char* aether_ui_styled_weight_impl(int handle);        // ""/"bold"/"normal"
int aether_ui_styled_opacity_impl(int handle);               // 0..100, -1 unset
int aether_ui_styled_border_impl(int handle);                // (w<<24)|flag|rgb, -1 unset
int aether_ui_state_style_impl(int handle, int state);       // 0=hover 1=active, rgb or -1

// AeCS v1.2 — current sheet (use_styles: constructors consult it) +
// appearance-change registry (styles_for_mode auto re-theme). The holders
// live in aether_ui_system_extras.c (linked into every build); watch/fire
// are per-backend (OS event hookup / driver fire with thread marshalling).
void aether_ui_current_sheet_set_impl(void* sheet);
void* aether_ui_current_sheet_get_impl(void);
void aether_ui_appearance_register_impl(void* boxed_closure);
int aether_ui_appearance_override_get(void);
void aether_ui_appearance_override_set(int dark);
int aether_ui_appearance_invoke(int dark);
void aether_ui_watch_appearance_impl(void);   // idempotent OS-event hookup
int aether_ui_fire_appearance(int dark);      // driver: override + invoke

// Undo/redo (Swing UndoManager shape; stack lives in system_extras.c).
// _step is the DIRECT call (UI thread — app-side undo()/redo());
// fire_undo/redo are the driver's marshalled twins (HTTP thread safe).
void aether_ui_undo_push_impl(const char* label, void* undo_boxed, void* redo_boxed);
int aether_ui_undo_step_impl(void);
int aether_ui_redo_step_impl(void);
int aether_ui_undo_depth_impl(void);
int aether_ui_redo_depth_impl(void);
const char* aether_ui_undo_label_impl(void);
int aether_ui_fire_undo(void);
int aether_ui_fire_redo(void);

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a);
void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical);
void aether_ui_set_text_color(int handle, double r, double g, double b);
void aether_ui_set_font_size(int handle, double size);
void aether_ui_set_font_bold(int handle, int bold);
void aether_ui_set_corner_radius(int handle, double radius);
void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left);
void aether_ui_set_width(int handle, int width);
void aether_ui_set_height(int handle, int height);
void aether_ui_set_opacity(int handle, double opacity);
void aether_ui_set_enabled(int handle, int enabled);
void aether_ui_set_tooltip(int handle, const char* text);

// Accessibility (semantics layer). Set the platform accessible role/name/
// description on a widget; backends map to GtkAccessible / MSAA /
// NSAccessibility. See docs/design/accessibility.md.
void aether_ui_a11y_set_role_impl(int handle, const char* role);
void aether_ui_a11y_set_label_impl(int handle, const char* name);
void aether_ui_a11y_set_description_impl(int handle, const char* desc);
// Driver readback: fill role/name/description with the widget's EFFECTIVE
// accessible values (auto when unset). Any buffer may be left empty.
void aether_ui_a11y_get_impl(int handle,
                             char* role, int rolesz,
                             char* name, int namesz,
                             char* desc, int descsz);
void aether_ui_set_distribution(int handle, int distribution);
void aether_ui_set_alignment(int handle, int alignment);
void aether_ui_match_parent_width(int handle);
void aether_ui_match_parent_height(int handle);
void aether_ui_set_margin(int handle, int top, int right, int bottom, int left);
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left);
void aether_ui_enable_test_server_ctx(int port, void* ctx);

// System integration (Group 5)
void aether_ui_alert_impl(const char* title, const char* message);
char* aether_ui_file_open(const char* title);
char* aether_ui_file_save(const char* title, const char* default_name);
void aether_ui_clipboard_write_impl(const char* text);
int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure);
void aether_ui_timer_cancel_impl(int timer_id);
void aether_ui_open_url_impl(const char* url);
int aether_ui_dark_mode_check(void);

int aether_ui_window_create_impl(const char* title, int width, int height);
void aether_ui_window_set_body_impl(int win_handle, int root_handle);
void aether_ui_window_show_impl(int win_handle);
void aether_ui_window_close_impl(int win_handle);
/* Unified driver window view: 1 = primary, 2.. = extras. */
int  aether_ui_window_count_impl(void);
const char* aether_ui_window_title_impl(int win_handle);
int  aether_ui_window_is_open_impl(int win_handle);
int  aether_ui_widget_window_impl(int widget_handle);   /* widget's window, 0=none */
void aether_ui_close_window_by_handle_impl(int win_handle);

/* In-window overlay layer (roadmap item 1). */
int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal);
void aether_ui_overlay_close_impl(int overlay_handle);
void aether_ui_overlay_set_on_dismiss_impl(int overlay_handle, void* boxed_closure);
int aether_ui_overlay_is_live_impl(int overlay_handle);
int aether_ui_overlay_count_impl(void);
int aether_ui_overlay_is_modal_impl(int overlay_handle);
// Per-entry enter/exit transition (transition_overlay). set stores kind/ms +
// plays enter; is_exiting is 1 while the exit tween plays before real removal.
void aether_ui_overlay_set_transition_impl(int overlay_handle, const char* kind, int ms);
int aether_ui_overlay_is_exiting_impl(int overlay_handle);
// Scrim material: "dim"|"blur"|"tint". Real backdrop blur only on macOS
// (NSVisualEffectView); win32/GTK4 degrade "blur"→"tint". The _effective getter
// reports what was actually applied. See docs/design/backdrop-material.md.
void aether_ui_overlay_set_material_impl(int overlay_handle, const char* kind);
const char* aether_ui_overlay_material_effective_impl(int overlay_handle);
int aether_ui_toast_impl(int win_handle, const char* text, int ms);
void aether_ui_widget_apply_css_impl(int handle, const char* property_css);
void aether_ui_widget_add_css_class_impl(int handle, const char* cls);
void aether_ui_widget_remove_css_class_impl(int handle, const char* cls);
void aether_ui_canvas_group_begin_impl(int canvas_id);
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha);
int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py,
                                     int width, int height);
int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text,
                                   double cx, double cy);
void aether_ui_vg_tooltip_hide_impl(void);
int aether_ui_vg_tooltip_drawn_impl(void);

int aether_ui_sheet_create_impl(const char* title, int width, int height);
void aether_ui_sheet_set_body_impl(int handle, int root_handle);
void aether_ui_sheet_present_impl(int handle);
void aether_ui_sheet_dismiss_impl(int handle);

int aether_ui_image_create(const char* filepath);
// Decode encoded image bytes (PNG / JPEG / GIF / BMP …) into an image
// widget — no temp file. `data` is a byte buffer of `length` bytes (an
// AetherString on the caller side, binary-safe via the explicit length).
// Returns an image widget handle; on decode failure an empty image (the
// widget still exists so the tree is stable). For downloaded/generated/
// embedded-art bytes that never touch disk.
int aether_ui_image_from_bytes(const char* data, int length);
void aether_ui_image_set_size(int handle, int width, int height);

// Menus (Group 5b) — native menu bars and context menus.
// Backend-implemented on Win32 (HMENU), GTK (GMenu), AppKit (NSMenu).
int  aether_ui_menu_bar_create(void);
int  aether_ui_menu_create(const char* label);
void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure);
void aether_ui_menu_add_separator(int menu_handle);
void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle);
void aether_ui_menu_bar_attach(int app_handle, int bar_handle);
/* Per-window menu bar (win_handle: 1=primary, 2..=extras). */
void aether_ui_menu_bar_attach_window(int win_handle, int bar_handle);
// Context menu: popup a menu at cursor / widget bounds.
void aether_ui_menu_popup(int menu_handle, int anchor_widget);

// Grid layout (Group 3b) — 2D layout container.
// Children are placed with aether_ui_grid_place() at (row, col) with
// optional row/col spans. Unlike stacks, columns align across rows so
// labels-on-left / fields-on-right forms actually line up.
int  aether_ui_grid_create(int cols, int row_spacing, int col_spacing);
void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span);

// Canvas drawing (Group 6)
int aether_ui_canvas_create_impl(int width, int height);
int aether_ui_canvas_get_widget(int canvas_id);
// Register a resize hook: the boxed closure (taking new w, h) fires when the
// canvas allocation changes. AeVG's vg{} scope uses it to re-map its viewBox
// and re-flush its shapes at the new scale, so a resized window rescales the
// vector scene. No-op on backends without live resize delivery.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure);
// Register a single-click hook on a canvas. The boxed closure takes
// (x: double, y: double) in canvas-local pixels — AeVG hit-tests shapes with
// them. No-op on backends without live click delivery.
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure);
// Register a pointer-move hook on a canvas. The boxed closure takes
// (x: double, y: double) in canvas-local pixels — fired on every motion event,
// for hover hit-testing. No-op on backends without live motion delivery.
void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure);
// Register a key-down hook on a canvas. The boxed closure takes (key: string) —
// a key name (e.g. "Left", "a", "space", "Escape"). No-op on backends without
// live key delivery.
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure);
// Pointer-release: fires (x, y) canvas-local px when the button comes up.
// Pairs with on_click + on_move to form a press→drag→release swipe.
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure);
void aether_ui_canvas_begin_path_impl(int canvas_id);
void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y);
void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y);
// cap: 0=butt (SVG default) / 1=round / 2=square. join: 0=miter / 1=round / 2=bevel.
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b,
                             double a, double line_width, int cap, int join);
void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y,
                                double w, double h,
                                double r, double g, double b, double a);
// Intersect the canvas clip region with rect (x,y,w,h) — SVG viewport
// overflow:hidden. Persists for the rest of the frame. No-op where unsupported.
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y,
                                double w, double h);
// One-shot paint clip for retained-compositor dirty regions. `rects` is an
// Aether floatarr-style x,y,w,h buffer with `n` rectangles. The next live paint
// is clipped to their union and the debug paint-area metric reports their
// summed area.
void aether_ui_canvas_set_clip_rects_impl(int canvas_id, void* rects, int n);
void aether_ui_canvas_reset_clip_impl(int canvas_id);
void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                                double start_angle, double end_angle);
void aether_ui_canvas_close_path_impl(int canvas_id);
/* even_odd: 1 = SVG fill-rule="evenodd", 0 = nonzero (the SVG DEFAULT).
   Hardcoding either is wrong: no.svg's crossing X needs nonzero (even-odd
   punches a hole through the centre) while accessible.svg and the
   paths-data-0*-t conformance files need even-odd. */
void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a,
                                int even_odd);
void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text,
                                      double x, double y, double font_size,
                                      int font_flags,
                                      double r, double g, double b, double a);
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text,
                                        double x, double y, double font_size,
                                        double line_width, int font_flags,
                                        double r, double g, double b, double a);
void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len);
// As draw_image but scales the iw×ih source to a dw×dh dest rect at (x,y) —
// for a frame whose pixel resolution differs from the target region extent.
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y,
                                       double dw, double dh, int iw, int ih,
                                       const unsigned char* rgba, int byte_len);
// extend: SVG spreadMethod → 0=pad, 1=reflect, 2=repeat.
// cap/join apply to the STROKE case (line_width > 0) and are ignored for a
// fill: 0=butt/miter, 1=round, 2=square/bevel. They have to ride along on the
// gradient command because it is the ONLY thing dispatched for a gradient
// stroke — there is no accompanying canvas_stroke_cj to carry them.
void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id,
        double x1, double y1, double x2, double y2,
        int n_stops, void* offsets, void* rgba, double line_width, int extend,
        int cap, int join);
void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width, int extend,
        int cap, int join);
void aether_ui_canvas_clear_impl(int canvas_id);
void aether_ui_canvas_redraw_impl(int canvas_id);
// Off-screen render of the canvas command buffer to a PNG. Headless-capable
// (pure cairo image surface; no window). Returns 1 on success, 0 on failure.
/* Stage 3 (per-frame surfaces). canvas_cmd_count lets a caller bracket one
   frame's drawing as a contiguous command range; render_range_rgba renders
   that range offscreen and copies it out as tightly packed, NON-premultiplied
   RGBA8 -- the form canvas_draw_image takes, so a rendered frame can be
   blitted back.

   Implemented on GTK4 only for now. win32 and macOS return 0/-1, and
   ui.frames treats that as "this backend cannot cache" and draws inline, so
   they are gated OFF rather than left enabled-and-wrong (see the four-OS
   parity rule in docs/design/retained-compositor.md). Both already have an
   offscreen replay for write_png to build on when they are ported. */
void aether_ui_canvas_draw_image_impl_ptr(int canvas_id, double x, double y,
                                          int iw, int ih,
                                          const unsigned char* rgba, int byte_len);
int aether_ui_canvas_cmd_count_impl(int canvas_id);
/* Pixels of the painter's retained surface differing from the background.
   0 means the app painted nothing. GTK4 only; -1 elsewhere (no retained
   paint surface to read -- see Stage 2.5b). */
int aether_ui_canvas_painted_pixels_impl(int canvas_id);
int aether_ui_canvas_render_range_rgba_impl(int canvas_id, int start, int end,
                                            double ox, double oy,
                                            int width, int height,
                                            unsigned char* out, int out_len);

int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height);

// Events
void aether_ui_on_hover_impl(int handle, void* boxed_closure);
void aether_ui_on_double_click_impl(int handle, void* boxed_closure);
void aether_ui_on_click_impl(int handle, void* boxed_closure);

// Animation
void aether_ui_animate_opacity_impl(int handle, double target, int duration_ms);

// Widget manipulation
void aether_ui_remove_child_impl(int parent_handle, int child_handle);
void aether_ui_clear_children_impl(int handle);

// AetherUIDriver
void aether_ui_enable_test_server_impl(int port, int root_handle);
void aether_ui_seal_widget_impl(int handle);
void aether_ui_seal_subtree_impl(int handle);

// System tray (Group 7) — OS-level status-area icon with optional
// tooltip, popup menu, and reactive icon swap. Phase 1 unblocks
// tray-only apps (AvnSync v2 etc). Backend storage + driver
// dispatch live in aether_ui_system_extras.c; per-backend native
// wiring (libayatana-appindicator / NSStatusItem / Shell_NotifyIcon)
// calls into the shared registry on click/menu-activate events.
int  aether_ui_tray_create_impl(const char* name, void* boxed_left_click);
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text);
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle);
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle,
                                             const char* icon_clean,
                                             const char* icon_busy,
                                             const char* icon_alert);
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template);
void aether_ui_tray_seal_impl(int tray_id);

// Desktop notifications (Group 7b) — fire-and-forget OS notifications.
// Click callback (notify_full) marshals back to the main thread before
// firing the Aether closure.
int  aether_ui_notify_impl(const char* title, const char* body);
int  aether_ui_notify_full_impl(const char* title, const char* body,
                                 const char* icon_path, const char* tag,
                                 void* boxed_click);
int  aether_ui_notify_request_permission_impl(void);

// Widget tree
void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle);
void aether_ui_widget_set_hidden(int handle, int hidden);

// Text mutation
void aether_ui_text_set_string(int handle, const char* text);
void aether_ui_button_set_label(int handle, const char* label);

// Context menu: append a right-click menu item to a widget. The menu (and
// its right-click gesture) is created lazily on the first item; the boxed
// closure fires when the item is chosen. GTK4 has the full implementation;
// AppKit/Win32 are currently no-op stubs (same precedent as canvas_on_move).
void aether_ui_context_menu_item_impl(int handle, const char* label,
                                      void* boxed_closure);

// Reactive state — typed cells in one handle space (item 8; see
// docs/design/reactivity-unification.md). The float trio below is the
// original API, unchanged. aether_ui_state_type: 0=float 1=int 2=bool
// 3=string, -1=bad handle. Cross-type get/set is a no-op/zero read.
int aether_ui_state_create(double initial);
double aether_ui_state_get(int handle);
void aether_ui_state_set(int handle, double value);
void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix);
int aether_ui_state_create_s(const char* initial);
int aether_ui_state_create_i(int initial);
int aether_ui_state_create_b(int initial);
const char* aether_ui_state_get_s(int handle);  // malloc'd
int aether_ui_state_get_i(int handle);
int aether_ui_state_get_b(int handle);
void aether_ui_state_set_s(int handle, const char* value);
void aether_ui_state_set_i(int handle, int value);
void aether_ui_state_set_b(int handle, int value);
int aether_ui_state_type(int handle);   // …4=list
// LIST cells: an opaque std.list ptr + a revision that bumps on each set (so
// a driver can observe that the list changed). Powers each_bind.
int   aether_ui_state_create_list(void* list_ptr);
void* aether_ui_state_get_list(int handle);
void  aether_ui_state_set_list(int handle, void* list_ptr);
int   aether_ui_state_list_rev(int handle);
// Generic state observer: a boxed closure fired (no args) on every set of
// state_handle. The unifying primitive behind each_bind (re-run each_update)
// and computed state (recompute + re-set the derived cell).
void  aether_ui_state_on_change(int state_handle, void* boxed_closure);
// Two-way value binding: editable widget ⇄ string state (bind_value).
void  aether_ui_bind_value(int state_handle, int widget_handle);
// Property bindings: state → widget property links, applied on set.
// decimals: float text formatting, -1 = smart. invert: negate the
// truthiness before applying (enabled/hidden).
void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals);
void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert);
void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert);

// Shortcuts + focus (item 9). shortcut: window-scoped accelerator,
// combo "Ctrl+R" (or GTK "<Control>r"); fires regardless of focus.
// focus: explicit keyboard-focus grab (Tab order is the build order —
// GTK's default focus chain; that default IS the feature).
// GTK4 real; win32/macOS no-op stubs.
void aether_ui_shortcut_impl(const char* combo, void* boxed_closure);
// Conditional shortcut: enabled_closure (|-> int) gates the combo; 0 = inert
// (key propagates). NULL predicate = always active.
void aether_ui_shortcut_when_impl(const char* combo, void* boxed_closure,
                                  void* enabled_closure);
// Chorded shortcut: a two-key sequence (first_combo then second_combo within
// ~1.5s) fires boxed_closure.
void aether_ui_shortcut_chord_impl(const char* first_combo,
                                   const char* second_combo,
                                   void* boxed_closure);
void aether_ui_focus_impl(int handle);
// Registry handle of the currently keyboard-focused widget (0 = none). Safe on
// the UI thread; used by per-widget shortcut scopes.
int aether_ui_focused_widget(void);
void aether_ui_context_menu_item_accel_impl(int handle, const char* label,
                                            const char* accel,
                                            void* boxed_closure);

#endif
