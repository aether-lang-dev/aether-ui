// Aether UI — system tray + desktop-notification registry (cross-platform).
//
// Shipped registry-only: the C side owns the records (id, name, tooltip,
// callbacks, menu wiring, icon paths) and the platform-specific native call
// (libayatana-appindicator on Linux, NSStatusItem on macOS, Shell_NotifyIcon
// on Win32; libnotify / UNUserNotificationCenter / Toast XML for
// notifications) is left as a per-backend TODO matching the existing
// aether_ui_menu_* stub precedent. The registry + callback dispatch is
// enough to wire `.build.ae` callbacks against AvnSync v2's tray and
// notification surface AND exercise them via AetherUIDriver
// (`/tray/{id}/click`, `/tray/{id}/menu/activate`, `/notifications/{id}/click`,
// `/notifications/{id}/dismiss`), which is what the asks asked for in
// their "headless mode" + "AetherUIDriver surface" sections.
//
// When a backend eventually wires the real native widget, its only job is
// to call `aether_ui_tray_emit_click()` / `_menu_activate()` /
// `_notif_click()` / `_notif_dismiss()` when the OS delivers the event,
// and to read the registry on creation/update for the state to render.
// No DSL surface or driver-endpoint change is needed at that point.
//
// AETHER_UI_HEADLESS: this layer is the same regardless. It records all
// state and exposes everything via the driver. The only knob the flag
// affects is whether the eventual native call gets made — and since that
// call is currently a no-op stub anyway, the headless flag is currently
// honoured trivially.

#ifndef AETHER_UI_SYSTEM_EXTRAS_H
#define AETHER_UI_SYSTEM_EXTRAS_H

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Tray registry
// ---------------------------------------------------------------------------

// Register a tray icon and store its (optional) left-click closure.
// `boxed_left_click` may be NULL; on Linux/Win the platform conventionally
// uses right-click for the menu, so most apps will pass NULL here and
// rely on tray_set_menu().
int  aether_ui_tray_register(const char* name, void* boxed_left_click);

// Update a tray record. set_menu attaches a menu created with
// aether_ui_menu_create; the driver's /tray/{id}/menu/activate endpoint
// looks up items in that menu by label.
void aether_ui_tray_set_tooltip_reg(int tray_id, const char* text);
void aether_ui_tray_set_menu_reg(int tray_id, int menu_handle);
void aether_ui_tray_set_icon_for_state_reg(int tray_id, int state_handle,
                                            const char* icon_clean,
                                            const char* icon_busy,
                                            const char* icon_alert);
void aether_ui_tray_set_icon_template_reg(int tray_id, int is_template);
void aether_ui_tray_seal_reg(int tray_id);

// Introspection — read-only, called from the driver's HTTP thread.
int  aether_ui_tray_count(void);
const char* aether_ui_tray_name(int tray_id);
const char* aether_ui_tray_tooltip(int tray_id);
int  aether_ui_tray_menu_handle(int tray_id);
const char* aether_ui_tray_current_icon(int tray_id); // resolves via state
int  aether_ui_tray_is_template(int tray_id);
int  aether_ui_tray_is_sealed(int tray_id);

// Dispatch — fires the registered closure on the calling thread.
// Returns: 0 ok, 1 sealed, 3 not-found, 4 no callback installed.
int aether_ui_tray_emit_click(int tray_id);
int aether_ui_tray_menu_activate(int tray_id, const char* item_label);

// ---------------------------------------------------------------------------
// Notification registry
// ---------------------------------------------------------------------------

// notify(title, body) — fire-and-forget, no callback. Returns the
// assigned notification id (1-based) for driver-side referencing.
int aether_ui_notify_register(const char* title, const char* body);

// notify_full(...) — full record with optional click closure.
// `icon_path` and `tag` may be empty strings ("") for none.
int aether_ui_notify_register_full(const char* title, const char* body,
                                    const char* icon_path, const char* tag,
                                    void* boxed_click);

// Permission stub — Linux/Win return 1 (always granted); macOS would
// hit UNUserNotificationCenter requestAuthorization (TODO).
int aether_ui_notify_request_permission(void);

int  aether_ui_notif_count(void);
const char* aether_ui_notif_title(int notif_id);
const char* aether_ui_notif_body(int notif_id);
const char* aether_ui_notif_icon(int notif_id);
const char* aether_ui_notif_tag(int notif_id);
int  aether_ui_notif_dismissed(int notif_id);

// Dispatch — driver-facing.
// Returns: 0 ok, 3 not-found, 4 no callback installed (click only).
int aether_ui_notif_emit_click(int notif_id);
int aether_ui_notif_mark_dismissed(int notif_id);

// ---------------------------------------------------------------------------
// Menu-item side-store — let the driver look up an item's closure by
// (menu_handle, label). Backends call aether_ui_menu_item_record() from
// inside aether_ui_menu_add_item; the driver's tray/menu/activate route
// uses _menu_item_invoke().
// ---------------------------------------------------------------------------

void aether_ui_menu_item_record(int menu_handle, const char* label,
                                 void* boxed_closure);
// Returns 0 ok, 3 not-found, 4 no closure.
int  aether_ui_menu_item_invoke(int menu_handle, const char* label);
// Rename the first recorded item on menu_handle labelled old_label. Returns
// 1 if one was renamed. Backends call it from aether_ui_menu_item_set_label.
int  aether_ui_menu_item_relabel(int menu_handle, const char* old_label,
                                 const char* new_label);
// Enumerate items belonging to a given menu_handle in declaration
// order. Used by the SNI/DBusMenu backend to project the menu over
// D-Bus; the registry only stores labels (no separators) — passing a
// separator-aware view would require extending menu_add_separator to
// also call _record, which Phase 1 doesn't do.
int          aether_ui_menu_item_count_for(int menu_handle);
const char*  aether_ui_menu_item_label_at(int menu_handle, int index);
// Distinct menu handles that have at least one recorded item, in first-seen
// order, written into out[] (up to max). Returns the count. Lets the driver's
// GET /menus enumerate menus without a separate menu registry.
int          aether_ui_menu_handles(int* out, int max);

// ---------------------------------------------------------------------------
// open_url under AETHER_UI_HEADLESS (toolkit-envy G6): a headless run must
// not hand a URL to the OS -- a spec that clicks a link would launch a
// browser on the CI box -- so every backend's open_url_impl records it here
// instead, and the driver's GET /opened_urls lists what the app asked to
// open, in order. Not recorded outside headless: there the URL really opens.
// ---------------------------------------------------------------------------
void        aether_ui_opened_url_record(const char* url);
int         aether_ui_opened_url_count(void);
const char* aether_ui_opened_url_at(int index);   // "" when out of range

// ---------------------------------------------------------------------------
// Headless
// ---------------------------------------------------------------------------

// Shared park-forever helper used by backends as the no-op fallback in
// `aether_ui_app_run_headless_impl`. Each backend defines that entry
// point itself (GTK4 → g_main_loop_run for SNI/DBusMenu signal
// delivery; macOS/Win32 → still park, pending native tray work).
void aether_ui_park_until_killed(void);

#ifdef __cplusplus
}
#endif

#endif
