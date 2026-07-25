// AetherUIDriver — shared HTTP test-server interface.
//
// Each backend (GTK4, AppKit, Win32) fills in a small set of hooks that
// describe how to introspect and mutate widgets, and then calls
// aether_ui_test_server_start() to spawn the accept loop. The HTTP
// parsing, URL routing, JSON formatting, sealed-widget bookkeeping, and
// cross-platform socket setup live once in aether_ui_test_server.c.
//
// The hooks are intentionally tiny and synchronous: each action struct
// is filled by the HTTP thread, handed to dispatch_action() which must
// marshal it onto the UI thread, and the HTTP thread reads the result
// back. GTK uses g_idle_add, AppKit uses dispatch_async to the main
// queue, Win32 uses SendMessage to a hidden AE_WM_DRIVER window.

#ifndef AETHER_UI_TEST_SERVER_H
#define AETHER_UI_TEST_SERVER_H

#include <stddef.h>  /* for size_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AETHER_DRV_CLICK     = 0,
    AETHER_DRV_SET_TEXT  = 1,
    AETHER_DRV_TOGGLE    = 2,
    AETHER_DRV_SET_VALUE = 3,
    AETHER_DRV_SET_STATE = 4,
    AETHER_DRV_FOCUS     = 5,
    AETHER_DRV_WIN_RESIZE = 6,
    AETHER_DRV_WIN_KEY   = 7,
    // Added when macOS adopted this server. These were previously reachable
    // only on GTK4's embedded server, which is why split/overlay/canvas specs
    // were red on BOTH win32 and macOS. A backend that cannot honour one sets
    // ctx->result = 3 and the route answers 404 honestly.
    AETHER_DRV_SPLIT_POS    = 8,   // handle=splitview, ival=px (<0 reads), retval=position
    AETHER_DRV_CANVAS_CLICK = 9,   // handle=canvas, dval=x, dval2=y
    AETHER_DRV_CANVAS_MOVE  = 10,  // handle=canvas, dval=x, dval2=y
    AETHER_DRV_CANVAS_KEY   = 11,  // handle=canvas, sval=key name
    AETHER_DRV_PICK         = 12,  // ival=x, ival2=y → retval=handle, ival2=on_scrim
    AETHER_DRV_SHUTDOWN     = 13,  // close the top-level; app exits via the user-close path
    AETHER_DRV_CTX_MENU     = 14,  // handle=widget → retval=1 if it has a menu
    AETHER_DRV_CTX_ACTIVATE = 15,  // handle=widget, ival=item index → retval=1 if fired
    AETHER_DRV_TAB_SELECT   = 16,  // handle=tabs, ival=index → retval=resulting index
    AETHER_DRV_CANVAS_RELEASE = 17,// handle=canvas, dval=x, dval2=y — completes a drag
    // ── QE additions ────────────────────────────────────────────────
    // Pointer position over a WIDGET (not a canvas): the piece missing for
    // testing :hover styling, tooltips and hover-reveal affordances. A
    // backend enters/leaves the widget as a real pointer would.
    AETHER_DRV_HOVER        = 18,  // handle=widget (0 = leave everything)
    // Press-and-hold / release on a widget, so a spec can assert the
    // ACTIVE (pressed) visual state, and can drive press-drag-release
    // gestures on ordinary widgets rather than only canvases.
    AETHER_DRV_PRESS        = 19,  // handle=widget
    AETHER_DRV_RELEASE      = 20,  // handle=widget
} AetherDriverActionKind;

/* Interaction-state readback (QE): report whether the pointer is over a
   widget / it is held pressed, so specs assert hover styling without pixels. */
typedef struct {
    AetherDriverActionKind action;
    int    handle;
    double dval;
    double dval2;   // CANVAS_CLICK/MOVE: y (dval carries x)
    char   sval[512];
    int    ival;    // WIN_RESIZE: width. SPLIT_POS: px. PICK: x
    int    ival2;   // WIN_RESIZE: height. PICK: y in, on_scrim out
    int    retval;  // WIN_KEY: 1 = fired. SPLIT_POS: position. PICK: handle
    // Output: 0=ok, 1=sealed, 2=banner, 3=not_found
    int    result;
    // Set by dispatch_action → 1 when the UI thread has finished. Used by
    // the HTTP thread to poll the result; the server wrapper does this
    // for you, backends don't need to touch it.
    volatile int done;
} AetherDriverActionCtx;

typedef struct {
    // Introspection — called on the HTTP server thread.
    // These must be thread-safe against the UI thread. In practice, GTK,
    // AppKit, and Win32 all tolerate read-only queries about widget state
    // from off-thread; the only true races would be widget registry
    // growth under concurrent creation, which test harnesses don't do.
    int         (*widget_count)(void);
    const char* (*widget_type)(int handle);
    void        (*widget_text_into)(int handle, char* buf, int bufsize);
    int         (*widget_visible)(int handle);
    /* QE: interaction state, so hover/pressed styling is assertable without
       pixels (and on boxes with no pointer). NULL = backend does not report. */
    int         (*widget_hovered)(int handle);
    int         (*widget_pressed)(int handle);
    int         (*widget_parent)(int handle);
    int         (*toggle_active)(int handle);
    double      (*slider_value)(int handle);
    double      (*progressbar_fraction)(int handle);

    // Marshal the action onto the UI thread and block until it completes.
    // The server reads ctx->result after return.
    void (*dispatch_action)(AetherDriverActionCtx* ctx);

    // Optional: list the direct children of a widget (1-based handles).
    // Return value is the number of children written into `out_handles`;
    // pass NULL for out_handles to query the count only. Hooks that leave
    // this NULL cause GET /widget/{id}/children to return 501.
    int (*widget_children)(int handle, int* out_handles, int max);

    // Optional introspection (item-9/8 parity; NULL = field omitted /
    // route 501). widget_enabled: 1/0. widget_rect: window-local px, 0 on
    // success. widget_classes_into: space-separated class list ("" none).
    // focused_widget: handle with keyboard focus (0 none).
    int  (*widget_enabled)(int handle);
    int  (*widget_rect)(int handle, int* x, int* y, int* w, int* hgt);
    void (*widget_classes_into)(int handle, char* buf, int bufsize);
    int  (*focused_widget)(void);

    // Optional: fill the widget's EFFECTIVE accessible role/name/description
    // (auto when unset). NULL hook → /widget/{id}/a11y returns 501 and the
    // /widgets JSON omits the role/a11y_name fields.
    void (*widget_a11y)(int handle, char* role, int rolesz,
                        char* name, int namesz, char* desc, int descsz);

    // Optional: capture the application's root window to a PNG byte buffer.
    // On success, set *out_data (caller-freed with free()) and *out_len,
    // return 0. On failure, return non-zero. NULL hook → 501.
    int (*screenshot_png)(unsigned char** out_data, size_t* out_len);
} AetherDriverHooks;

// Spawn the accept-loop thread. Returns immediately. The server stays up
// for the life of the process; there is currently no shutdown hook (the
// inherited test_server_thread in each backend never exposed one either).
void aether_ui_test_server_start(int port, const AetherDriverHooks* hooks);

// Banner + seal management. Implemented in aether_ui_test_server.c and
// callable from backends that need to query sealed state (e.g., when
// walking a subtree to seal it recursively).
void aether_ui_test_server_set_banner(int handle);
int  aether_ui_test_server_banner_handle(void);
void aether_ui_test_server_seal_widget(int handle);
int  aether_ui_test_server_is_sealed(int handle);

#ifdef __cplusplus
}
#endif

#endif
