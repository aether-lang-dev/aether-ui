#!/usr/bin/env python3
"""Axis-ratio oracle for the radial-gradient ellipse repros.

The check the SVG corpus cannot provide. Each repro's expected axis ratio is
known BY CONSTRUCTION -- from the SVG's own matrix or bbox -- so correctness
needs no reference renderer, no MAE and no librsvg. A renderer that collapses
gradientTransform (or the bbox) to a scalar radius paints a CIRCLE and reads
1.00 where the answer should be 2.00 or 0.50.

Why this exists: the corpus has 20 files with a non-uniform gradientTransform
on a radial, but only ONE (gallardo) is both affected and fairly measurable --
the rest either score well already with the wrong geometry, or have no viewBox
so their MAE is dominated by canvas-size inference. Worse, MAE can FALL for a
more-correct renderer (measured: three corrected radial geometries all scored
worse on php.svg). MAE is a regression tripwire; this is the oracle.

  usage: radial_ellipse_check.py <dir-of-rendered-pngs>
         radial_ellipse_check.py --file <name> <rendered.png>

Expects PNGs named <repro-stem>.png. Exit 0 if all within tolerance.
"""
import os
import sys
from PIL import Image
import numpy as np

TOL = 0.15

# stem -> (expected ratio, what it pins)
EXPECT = {
    "radial_ellipse_repro":   (2.00, "matrix(2,0,0,1) - wide ellipse"),
    "radial_ellipse_tall":    (0.50, "matrix(1,0,0,2) - tall ellipse (axis swap guard)"),
    "radial_uniform_control": (1.00, "matrix(2,0,0,2) - CONTROL, correct today, must stay"),
    "radial_ellipse_bbox":    (2.00, "objectBoundingBox on a 2:1 rect - the third loss site"),
}

# Files whose defect is the TILT, not the axis lengths. An axis-aligned probe
# cannot see a rotation, so these are scored on the ratio of the two DIAGONAL
# half-maximum extents instead: a backend that drops the tilt paints the
# ellipse axis-aligned and both diagonals come out equal (~1.0).
EXPECT_DIAG = {
    "radial_ellipse_rotated": (2.02, "rotate(45) x scale(2,1) - the tilt itself"),
}


def axis_ratio(path):
    """Half-maximum width/height through the gradient's own brightest pixel.

    Robust to where the peak lands and to brightness differences between
    backends. The shape must fill the canvas -- a visible background would
    be brighter than the gradient and become the 'peak'.

    Measures the CONTIGUOUS run through the peak, not every pixel above the
    threshold anywhere on the line. A backend can leave bright artefacts at
    the canvas edge -- GDI+'s pad-past-rim fill leaves a single stray value
    at x=0 -- and a global scan turns one such pixel into a span reaching
    the frame, which then reads as "clipped" and refuses to score a
    perfectly good gradient. Walking outward from the peak ignores anything
    not connected to it.

    Still returns None when the contiguous run genuinely reaches the edge:
    the falloff is then bounded by the frame rather than by the gradient,
    and the ratio would measure the canvas.
    """
    a = np.asarray(Image.open(path).convert("RGB"), dtype=int)[:, :, 0]
    py, px = np.unravel_index(a.argmax(), a.shape)
    half = a[py, px] // 2
    h, w = a.shape

    x0 = px
    while x0 > 0 and a[py, x0 - 1] >= half:
        x0 -= 1
    x1 = px
    while x1 < w - 1 and a[py, x1 + 1] >= half:
        x1 += 1
    y0 = py
    while y0 > 0 and a[y0 - 1, px] >= half:
        y0 -= 1
    y1 = py
    while y1 < h - 1 and a[y1 + 1, px] >= half:
        y1 += 1

    if x0 == 0 or x1 == w - 1 or y0 == 0 or y1 == h - 1:
        return None      # clipped by the frame: unmeasurable, not wrong
    return (x1 - x0 + 1) / (y1 - y0 + 1)


def diag_ratio(path):
    """Ratio of the two 45-degree half-maximum extents through the peak."""
    a = np.asarray(Image.open(path).convert("RGB"), dtype=int)[:, :, 0]
    py, px = np.unravel_index(a.argmax(), a.shape)
    half = a[py, px] // 2
    h, w = a.shape

    def ray(dx, dy):
        n = 0
        while True:
            xx, yy = int(px + dx * (n + 1)), int(py + dy * (n + 1))
            if xx < 0 or yy < 0 or xx >= w or yy >= h:
                return None            # ran off the canvas: unmeasurable
            if a[yy, xx] < half:
                return n
            n += 1

    k = 0.70710678
    q = [ray(k, k), ray(-k, -k), ray(k, -k), ray(-k, k)]
    if any(v is None for v in q):
        return None
    d1 = q[0] + q[1]
    d2 = q[2] + q[3]
    if d2 <= 0:
        return None
    return d1 / d2


def check_diag(stem, path):
    want, why = EXPECT_DIAG[stem]
    got = diag_ratio(path)
    if got is None:
        print(f"  {stem:24} UNMEASURABLE (ran off the canvas)")
        return False
    ok = abs(got - want) <= 0.25
    note = "" if ok else "  <-- tilt dropped: an axis-aligned ellipse reads ~1.00"
    print(f"  {stem:24} want {want:4.2f}  got {got:4.2f}  "
          f"{'OK' if ok else 'FAIL'}{note}  [diagonal probe]")
    if not ok:
        print(f"  {'':24} pins: {why}")
    return ok


def check(stem, path):
    want, why = EXPECT[stem]
    got = axis_ratio(path)
    if got is None:
        print(f"  {stem:24} UNMEASURABLE (gradient clipped by the canvas "
              f"edge, or none found) -- {os.path.basename(path)}")
        return False
    ok = abs(got - want) <= TOL
    note = "" if ok else ("  <-- a circle reads 1.00" if abs(got - 1.0) < TOL else "  <-- WRONG")
    print(f"  {stem:24} want {want:4.2f}  got {got:4.2f}  {'OK' if ok else 'FAIL'}{note}")
    if not ok:
        print(f"  {'':24} pins: {why}")
    return ok


def main(argv):
    if len(argv) == 4 and argv[1] == "--file":
        return 0 if check(argv[2], argv[3]) else 1
    if len(argv) != 2:
        print(__doc__)
        return 2
    d, bad, seen = argv[1], 0, 0
    for stem in EXPECT:
        p = os.path.join(d, stem + ".png")
        if not os.path.exists(p):
            print(f"  {stem:24} (not rendered)")
            continue
        seen += 1
        if not check(stem, p):
            bad += 1
    for stem in EXPECT_DIAG:
        p = os.path.join(d, stem + ".png")
        if not os.path.exists(p):
            print(f"  {stem:24} (not rendered)")
            continue
        seen += 1
        if not check_diag(stem, p):
            bad += 1
    if seen == 0:
        print(f"no repro PNGs found in {d}")
        return 2
    print(f"\n  {seen - bad}/{seen} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
