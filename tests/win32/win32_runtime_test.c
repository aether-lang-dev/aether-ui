// tests/win32/win32_runtime_test.c — the Win32 backend, LINKED AND RUN.
//
// This backend has no runtime CI: nobody on the team can run it, and every
// fix to it has shipped on a reading of the code alone. The cross-compile
// step proves syntax and the link proves the Windows APIs resolve, but a
// primitive that compiles, links, and then paints nothing looks identical to
// one that works. canvas_clip_rect was exactly that for the entire life of
// the backend -- it discarded its arguments and the replay had no case for it
// -- and canvas_reset_clip cleared unrelated state on all three backends.
//
// So this binary drives the backend for real and asserts on rendered pixels.
// It needs no window and no message loop: canvas_read_pixel_impl replays the
// command buffer into a memory bitmap, which is the same seam a live paint
// goes through. CI runs it under Wine, once per renderer, because
// canvas_replay_to_dc dispatches to a GDI+ path and a legacy GDI path that
// implement clipping with entirely different calls.
//
// IF A NEW UNDEFINED SYMBOL APPEARS, that is this file working: the backend
// started calling into the aether runtime somewhere new. Add it below with a
// comment naming the call site. Do NOT stub a WINDOWS symbol here -- an
// undefined Windows API is the bug this exists to catch, and its fix is a
// missing -l flag, not a stub.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "aether_ui_backend.h"

// aether_ui_win32.c reads canvas gradient stops and paint clip rects out of a
// std.floatarr. The DSL side is not linked here, so these read the plain
// double buffer this harness passes, which is the layout the backend expects.
double floatarr_get_raw(void* arr, int index);
double floatarr_get_unchecked(void* arr, int index);

double floatarr_get_raw(void* arr, int index) {
    return arr ? ((const double*)arr)[index] : 0.0;
}
double floatarr_get_unchecked(void* arr, int index) {
    return floatarr_get_raw(arr, index);
}

static int failures = 0;
static const char* renderer = "gdiplus";

#define RED   0xFFFF0000u
#define BLUE  0xFF0000FFu

static unsigned px(int cid, int x, int y) {
    return (unsigned)aether_ui_canvas_read_pixel_impl(cid, x, y, 100, 100);
}

static void expect_eq(unsigned got, unsigned want, const char* what) {
    if (got == want) {
        printf("  ok   [%s] %s\n", renderer, what);
    } else {
        printf("  FAIL [%s] %s: got 0x%08X want 0x%08X\n", renderer, what, got, want);
        failures++;
    }
}

static void expect_ne(unsigned got, unsigned bad, const char* what) {
    if (got != bad) {
        printf("  ok   [%s] %s\n", renderer, what);
    } else {
        printf("  FAIL [%s] %s: got 0x%08X, the value it must not be\n",
               renderer, what, got);
        failures++;
    }
}

// Clip to the top-left quadrant and fill the WHOLE canvas red, so red may
// land only in that quadrant. Then reset the clip and fill the opposite
// quadrant blue: it lies wholly outside the clip that was in force, so it
// paints only if the reset actually widened the clip back.
static void canvas_clip_and_reset(void) {
    int cid = aether_ui_canvas_create_impl(100, 100);
    if (cid < 1) {
        printf("  FAIL [%s] canvas_create returned %d\n", renderer, cid);
        failures++;
        return;
    }

    aether_ui_canvas_clip_rect_impl(cid, 0.0, 0.0, 50.0, 50.0);
    aether_ui_canvas_fill_rect_impl(cid, 0.0, 0.0, 100.0, 100.0, 1.0, 0.0, 0.0, 1.0);

    expect_eq(px(cid, 25, 25), RED, "clip: inside the clip took the fill");
    expect_ne(px(cid, 75, 75), RED, "clip: past both edges took none");
    expect_ne(px(cid, 75, 25), RED, "clip: past the right edge took none");

    aether_ui_canvas_reset_clip_impl(cid);
    aether_ui_canvas_fill_rect_impl(cid, 50.0, 50.0, 50.0, 50.0, 0.0, 0.0, 1.0, 1.0);

    expect_eq(px(cid, 75, 75), BLUE, "reset: the fill outside the old clip landed");
    expect_eq(px(cid, 60, 90), BLUE, "reset: and across that quadrant");
    expect_eq(px(cid, 25, 25), RED,  "reset: the earlier clipped fill survived");
    expect_ne(px(cid, 25, 75), RED,  "reset: widened to the canvas, not past it");
    expect_ne(px(cid, 25, 75), BLUE, "reset: the untouched quadrant stayed clear");
}

// True group opacity: two overlapping opaque rects inside one group, painted
// once at the group alpha. The property that matters is that the overlap is
// the SAME colour as the parts either rect covers alone. Painting each child
// at the alpha instead makes the overlap darker, which is the defect the
// feature exists to prevent, and it is asserted as an equality so it does not
// depend on the exact blend arithmetic.
//
// GDI+ ONLY, and deliberately so. The legacy replay has no group case at all:
// compositing a layer needs per-pixel alpha and GDI has none, which is why
// GDI+ is the default renderer. Asserting it on legacy would be asserting a
// limitation of the API rather than anything about this code.
static void canvas_group_opacity(void) {
    if (strcmp(renderer, "gdiplus") != 0) {
        printf("  skip [%s] group opacity: needs per-pixel alpha, GDI has none\n",
               renderer);
        return;
    }

    int cid = aether_ui_canvas_create_impl(100, 100);
    if (cid < 1) {
        printf("  FAIL [%s] canvas_create returned %d\n", renderer, cid);
        failures++;
        return;
    }

    aether_ui_canvas_group_begin_impl(cid);
    aether_ui_canvas_fill_rect_impl(cid,  0.0, 0.0, 60.0, 60.0, 1.0, 0.0, 0.0, 1.0);
    aether_ui_canvas_fill_rect_impl(cid, 40.0, 0.0, 60.0, 60.0, 1.0, 0.0, 0.0, 1.0);
    aether_ui_canvas_group_end_impl(cid, 0.5);

    unsigned only_a  = px(cid, 20, 30);
    unsigned overlap = px(cid, 50, 30);
    unsigned only_b  = px(cid, 80, 30);

    expect_eq(overlap, only_a, "group: the overlap did not double-darken");
    expect_eq(only_b,  only_a, "group: both children blended alike");
    expect_ne(only_a, RED,        "group: the alpha was actually applied");
    expect_ne(only_a, 0xFFFFFFFFu, "group: and something was painted");
}

// A gradient stop component outside [0,1] must SATURATE, not wrap. Both
// replay paths scale the stop by hand into a fixed-width channel, so an
// over-range component that is not clamped first spills: the GDI+ path shifts
// it into the neighbouring byte, and the legacy path truncates it into a
// COLOR16. Either way full red comes out as roughly 40% red.
//
// Runs on both renderers, because both do that conversion and each has its
// own copy of it.
static void canvas_gradient_stop_clamp(void) {
    int cid = aether_ui_canvas_create_impl(100, 100);
    if (cid < 1) {
        printf("  FAIL [%s] canvas_create returned %d\n", renderer, cid);
        failures++;
        return;
    }

    aether_ui_canvas_begin_path_impl(cid);
    aether_ui_canvas_move_to_impl(cid, 0.0, 0.0);
    aether_ui_canvas_line_to_impl(cid, 100.0, 0.0);
    aether_ui_canvas_line_to_impl(cid, 100.0, 100.0);
    aether_ui_canvas_line_to_impl(cid, 0.0, 100.0);
    aether_ui_canvas_close_path_impl(cid);

    double offsets[2] = { 0.0, 1.0 };
    /* Stop 0 asks for 1.4 red. Saturating gives full red; wrapping gives
       about 0x66, which is what this exists to catch. */
    double rgba[8] = { 1.4, 0.0, 0.0, 1.0,
                       0.0, 0.0, 1.0, 1.0 };
    aether_ui_canvas_fill_linear_gradient_impl(cid, 0.0, 0.0, 100.0, 0.0,
                                               2, offsets, rgba,
                                               0.0, 0, 0, 0);

    unsigned left  = px(cid, 4, 50);
    unsigned right = px(cid, 95, 50);
    int lr = (int)((left  >> 16) & 0xFF), lb = (int)(left  & 0xFF);
    int rr = (int)((right >> 16) & 0xFF), rb = (int)(right & 0xFF);

    if (lr >= 200) {
        printf("  ok   [%s] gradient: the over-range stop saturated (R=%d)\n",
               renderer, lr);
    } else {
        printf("  FAIL [%s] gradient: over-range stop wrapped instead of "
               "saturating (R=%d, wanted >= 200)\n", renderer, lr);
        failures++;
    }
    if (lb <= 80 && rb >= 180 && rr <= 80) {
        printf("  ok   [%s] gradient: ran red to blue across the box\n", renderer);
    } else {
        printf("  FAIL [%s] gradient: ramp wrong (left B=%d, right R=%d B=%d)\n",
               renderer, lb, rr, rb);
        failures++;
    }
}

int main(void) {
    // Unbuffered: under Wine a fault would otherwise discard everything this
    // has printed, which is exactly the run you most need the output from.
    setvbuf(stdout, NULL, _IONBF, 0);

    const char* r = getenv("AETHER_UI_WIN32_RENDERER");
    if (r && strcmp(r, "legacy") == 0) renderer = "legacy";
    printf("win32 runtime test (renderer: %s)\n", renderer);

    canvas_clip_and_reset();
    canvas_group_opacity();
    canvas_gradient_stop_clamp();

    if (failures) {
        printf("%d assertion(s) failed\n", failures);
        return 1;
    }
    printf("all assertions passed\n");
    return 0;
}
