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


def axis_ratio(path):
    """Half-maximum width/height through the gradient's own brightest pixel.

    Robust to where the peak lands and to brightness differences between
    backends. The shape must fill the canvas -- a visible background would
    be brighter than the gradient and become the 'peak'.

    Returns None when either extent touches the image EDGE. A gradient
    drawn too large runs off the canvas, the half-maximum span is clipped
    by the frame rather than by the falloff, and the ratio becomes a
    measure of the canvas instead of the geometry. Measured: GDI+ paints
    the uniform control far larger than it should and read 0.67 -- which
    looks like an axis error but is a CIRCLE, correctly round, merely
    oversized. Refusing to answer is right; a wrong number would have sent
    the ellipse work chasing a size bug.
    """
    a = np.asarray(Image.open(path).convert("RGB"), dtype=int)[:, :, 0]
    py, px = np.unravel_index(a.argmax(), a.shape)
    half = a[py, px] // 2
    h, w = a.shape
    hx = [x for x in range(w) if a[py, x] >= half]
    vy = [y for y in range(h) if a[y, px] >= half]
    if not hx or not vy:
        return None
    if hx[0] == 0 or hx[-1] == w - 1 or vy[0] == 0 or vy[-1] == h - 1:
        return None      # clipped by the frame: unmeasurable, not wrong
    return (hx[-1] - hx[0] + 1) / (vy[-1] - vy[0] + 1)


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
    if seen == 0:
        print(f"no repro PNGs found in {d}")
        return 2
    print(f"\n  {seen - bad}/{seen} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
