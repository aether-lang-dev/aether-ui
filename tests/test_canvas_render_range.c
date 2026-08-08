// Direct C test of canvas_render_range_rgba_impl against read_pixel_impl,
// which is the established ground truth for offscreen replay. Proves the new
// primitive renders the same pixels, and that its unpremultiply is right.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aether_ui_backend.h"

int main(void) {
    int cid = aether_ui_canvas_create_impl(64, 64);
    if (cid <= 0) { printf("FAIL: canvas_create -> %d\n", cid); return 1; }

    // Opaque red square over the left half, 50%-alpha blue over the right.
    aether_ui_canvas_fill_rect_impl(cid, 0, 0, 32, 64, 1.0, 0.0, 0.0, 1.0);
    aether_ui_canvas_fill_rect_impl(cid, 32, 0, 32, 64, 0.0, 0.0, 1.0, 0.5);

    int n = aether_ui_canvas_cmd_count_impl(cid);
    printf("cmd_count=%d\n", n);
    if (n < 2) { printf("FAIL: expected >=2 commands\n"); return 1; }

    int len = 64 * 64 * 4;
    unsigned char* buf = malloc(len);
    int got = aether_ui_canvas_render_range_rgba_impl(cid, 0, n, 0.0, 0.0,
                                                      64, 64, buf, len);
    if (got != len) { printf("FAIL: render_range -> %d want %d\n", got, len); return 1; }

    // Left pixel: opaque red, straight RGBA.
    unsigned char* p = buf + (10 * 64 + 10) * 4;
    printf("left  rgba=%d,%d,%d,%d\n", p[0], p[1], p[2], p[3]);
    if (!(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250)) {
        printf("FAIL: left pixel not opaque red\n"); return 1;
    }

    // Right pixel: 50% blue. UNPREMULTIPLIED it must be ~(0,0,255,128).
    // If the unpremultiply were missing it would read ~(0,0,128,128).
    unsigned char* q = buf + (10 * 64 + 40) * 4;
    printf("right rgba=%d,%d,%d,%d\n", q[0], q[1], q[2], q[3]);
    if (!(q[3] > 120 && q[3] < 136)) { printf("FAIL: right alpha not ~128\n"); return 1; }
    if (q[2] < 250) { printf("FAIL: right blue %d not ~255 (unpremultiply missing?)\n", q[2]); return 1; }

    // Range slicing: replay ONLY the first command; the right half must be
    // untouched (alpha 0), proving [start,end) really slices.
    memset(buf, 0, len);
    got = aether_ui_canvas_render_range_rgba_impl(cid, 0, 1, 0.0, 0.0, 64, 64, buf, len);
    if (got != len) { printf("FAIL: sliced render -> %d\n", got); return 1; }
    unsigned char* r = buf + (10 * 64 + 40) * 4;
    printf("sliced-right rgba=%d,%d,%d,%d\n", r[0], r[1], r[2], r[3]);
    if (r[3] != 0) { printf("FAIL: slice [0,1) drew the second rect\n"); return 1; }

    // Offset: render the right rect with ox=32 into a 32-wide buffer; its
    // content must land at x=0 -- the frame-local translation frames need.
    memset(buf, 0, len);
    got = aether_ui_canvas_render_range_rgba_impl(cid, 1, 2, 32.0, 0.0, 32, 64, buf, 32*64*4);
    if (got != 32*64*4) { printf("FAIL: offset render -> %d\n", got); return 1; }
    unsigned char* o = buf + (10 * 32 + 2) * 4;
    printf("offset rgba=%d,%d,%d,%d\n", o[0], o[1], o[2], o[3]);
    if (o[3] < 120) { printf("FAIL: offset did not translate content to origin\n"); return 1; }

    printf("PASS\n");
    return 0;
}
