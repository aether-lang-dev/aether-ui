// tests/ios/render_probe.m — the iOS canvas RENDER gate (ci.sh Phase 1e).
//
// Compiling and linking the UIKit backend proves it builds; it does not prove a
// single pixel is right. UIKit runs natively on macOS via Mac Catalyst
// (-target <arch>-apple-ios*-macabi), so CI can drive the real canvas ABI —
// aether_ui_uikit.m's Core Graphics executor — and READ BACK the pixels, with
// no simulator, device or iOS build of libaether.
//
// It draws known solid shapes, samples them with aether_ui_canvas_read_pixel,
// and asserts the colours (house rule #4: everything renderable must render via
// the headless path). Exits non-zero on any mismatch. Also writes a PNG for a
// human to eyeball when AETHER_UI_RENDER_PROBE_PNG points somewhere.
//
// It carries its own main() and the two floatarr_* stubs the backend names
// (closures go through the _AeClosure fn pointer, so nothing else is external),
// so it links WITHOUT tests/ios/link_stub.c.

#include "aether_ui_backend.h"
#include <stdio.h>
#include <stdlib.h>

double floatarr_get_raw(void* a, int i) { (void)a; (void)i; return 0.0; }
double floatarr_get_unchecked(void* a, int i) { (void)a; (void)i; return 0.0; }

static int failures = 0;

// read_pixel packs the sample as (A<<24)|(R<<16)|(G<<8)|B. An opaque pixel has
// A=255, so the value is negative as a signed int (0xFFrrggbb) — do NOT treat
// that as the -1 error sentinel; mask the channels out. A genuine error (no
// canvas / out of bounds) reads back 0xFFFFFFFF → 255,255,255, which no sampled
// point here expects, so it fails the colour check naturally.
static void expect_rgb(int cid, int N, int px, int py,
                       int er, int eg, int eb, const char* what) {
    unsigned int v = (unsigned int)aether_ui_canvas_read_pixel_impl(cid, px, py, N, N);
    int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
    int tol = 10;
    if (abs(r - er) <= tol && abs(g - eg) <= tol && abs(b - eb) <= tol) {
        printf("  ok   %-14s (%3d,%3d,%3d)\n", what, r, g, b);
    } else {
        printf("  FAIL %-14s got (%3d,%3d,%3d) want (%3d,%3d,%3d)\n",
               what, r, g, b, er, eg, eb);
        failures++;
    }
}

static void filled_square(int cid, double x, double y, double s,
                          double r, double g, double b) {
    aether_ui_canvas_begin_path_impl(cid);
    aether_ui_canvas_move_to_impl(cid, x, y);
    aether_ui_canvas_line_to_impl(cid, x + s, y);
    aether_ui_canvas_line_to_impl(cid, x + s, y + s);
    aether_ui_canvas_line_to_impl(cid, x, y + s);
    aether_ui_canvas_close_path_impl(cid);
    aether_ui_canvas_fill_impl(cid, r, g, b, 1.0, 0);
}

int main(void) {
    const int N = 300;
    int cid = aether_ui_canvas_create_impl(N, N);

    // Dark background, then three solid shapes at known spots.
    aether_ui_canvas_fill_rect_impl(cid, 0, 0, N, N, 0.10, 0.10, 0.10, 1.0);
    aether_ui_canvas_fill_rect_impl(cid, 40, 40, 80, 80, 0.90, 0.10, 0.10, 1.0);  // red rect
    filled_square(cid, 180, 40, 80, 0.10, 0.60, 0.20);                            // green path
    filled_square(cid, 40, 180, 80, 0.05, 0.25, 0.75);                            // blue path
    // Text: exercises fill_text / UIFont / UIGraphicsPushContext (not asserted —
    // antialiased edges make pixel asserts brittle; the point is it must not
    // crash and must go through the current-context path).
    aether_ui_canvas_fill_text_impl(cid, "iOS render probe", 20, 285, 20, 0,
                                    "sans-serif", 1, 1, 1, 1);

    // 0.90*255=230, 0.10*255=26, 0.60*255=153, 0.20*255=51, etc.
    expect_rgb(cid, N,  80,  80, 230,  26,  26, "red-rect");
    expect_rgb(cid, N, 220,  80,  26, 153,  51, "green-path");
    expect_rgb(cid, N,  80, 220,  13,  64, 191, "blue-path");
    expect_rgb(cid, N,  10,  10,  26,  26,  26, "background");

    const char* png = getenv("AETHER_UI_RENDER_PROBE_PNG");
    if (png && png[0]) {
        int ok = aether_ui_canvas_write_png_impl(cid, png, N, N);
        printf("  png  %s -> %d\n", png, ok);
        if (!ok) failures++;
    }

    if (failures == 0) { printf("render probe: all checks passed\n"); return 0; }
    printf("render probe: %d check(s) FAILED\n", failures);
    return 1;
}
