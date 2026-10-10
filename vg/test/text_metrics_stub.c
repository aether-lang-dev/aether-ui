// text_metrics_stub.c — zero-returning stubs for the cairo text-metric
// symbols that vg/module.ae declares extern (aether_ui_text_measure et al.).
//
// The pure-Aether Phase-0 unit tests link with `$(ae cflags)` only — no GTK
// backend — so any test importing `vg` needs these symbols resolved. The
// real cairo-backed implementations live in backend/aether_ui_gtk4.c; tests
// that actually exercise metrics (test_text_metrics) link that backend
// instead of this stub (ci.sh AEVG_GTK_TESTS). Everyone else just needs the
// symbols to exist — a pure-vg test never calls them, and if it did, 0 is
// the same safe degrade the win32/macOS backends give.
double aether_ui_text_measure(double size, const char* text) { (void)size; (void)text; return 0.0; }
double aether_ui_font_ascent(double size)  { (void)size; return 0.0; }
double aether_ui_font_descent(double size) { (void)size; return 0.0; }
double aether_ui_font_height(double size)  { (void)size; return 0.0; }

/* vg outline-font holder — REAL (4-line) implementation, not a zero stub:
 * Phase-0 tests exercising text_path need use_font() to actually stick.
 * Mirrors backend/aether_ui_system_extras.c. */
static void* g_aeui_vgfont = 0;
void* aether_ui_vgfont_get(void) { return g_aeui_vgfont; }
void aether_ui_vgfont_set(void* f) { g_aeui_vgfont = f; }

/* The vg image element's decoder and the string->ptr cast (vg.image_bytes).
 * A pure-vg test feeds pixels directly (image_pixels), never bytes, so the
 * decoder answers "not an image"; the cast is the same identity the shared
 * system_extras.c gives. */
#include <stddef.h>
unsigned char* aether_ui_image_decode_rgba_impl(const unsigned char* data, int length,
                                                int* out_w, int* out_h) {
    (void)data; (void)length;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return 0;
}
void* aether_ui_string_ptr(const char* s) { return (void*)s; }

/* GET /canvas/{id}/nodes: no driver in a unit test, so vg never builds it. */
int  aether_ui_canvas_nodes_wanted(void) { return 0; }
void aether_ui_canvas_nodes_publish(int canvas_id, const char* json) { (void)canvas_id; (void)json; }
