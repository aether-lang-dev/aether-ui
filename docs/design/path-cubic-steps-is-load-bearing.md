# `PATH_CUBIC_STEPS` is load-bearing, not a quality dial

**Status: investigation, unresolved.** Recorded so the next person does not
"improve" this constant and quietly break every gradient-filled path.

## The constant

`vg/grammar/shapes.ae`:

```aether
const PATH_CUBIC_STEPS: int = 16
```

Every SVG cubic bezier is flattened to 16 line segments before the backend
sees anything. There is no curve command in the canvas API at all — the kinds
are `BEGIN/MOVE/LINE/STROKE/FILL_RECT/CLEAR/ARC/CLOSE/FILL/FILL_TEXT/
DRAW_IMAGE/FILL_LINEAR/FILL_RADIAL` — so this is the fidelity ceiling for
*all four* backends. GTK4 reaches MAE 3.1 on `python.svg` purely by
antialiasing that 16-segment polyline.

Raising it looks like free accuracy. It is not.

## What actually happens at 64

Measured on Linux/GTK4, `python.svg`, against librsvg:

| steps | MAE | white px | dominant fill |
| --- | --- | --- | --- |
| 16 | **3.1** | 64,768 | `#ffee55` (gradient stop 1) |
| 64 | **56.3** | 25,670 | `#eecc33` (near stop 2) |

The render does not degrade gracefully — **the gradient floods the whole
canvas** instead of staying inside the two path outlines. 126,714 of 160,000
pixels differ, spanning the full image (x 0–399, y 0–399), so it is not a
geometry artefact at the curve edges.

Nothing is truncated: the PNG grows 18,336 → 38,083 bytes, i.e. the extra
points ARE reaching the renderer. The polyline is richer and the fill is
wrong.

## What it is not

Ruled out by measurement, so nobody repeats the work:

- **Not a point-count cap in the vg layer.** `emit_pt` appends to a
  strbuilder; no limit.
- **Not `replay_path` truncating.** It splits the whole string; no cap.
- **Not the win32 256-point cap.** That is real (`pts[256]` in both the
  `CV_FILL` and gradient cases) and would bite at 64 steps — but this
  regression is on **GTK4**, which has no such cap.
- **Not the filter/clipPath raster branch.** `needs_raster()` keys off
  `filter=` / `clip-path=` only, and `python.svg` has neither.

## The shape of the suspicion

The dominant fill colour shifting from stop 1 to stop 2 says the gradient is
being *sampled* differently, and the flood says it is being *applied*
differently. Both point at the gradient fill path rather than at path
flattening as such — the same "fill escaping its shape" signature as the win32
bug fixed in c03af5c, which was a missing path clip.

So the working hypothesis is: something in the gradient fill depends on the
point count, and 16 happens to keep it inside a working range.

## Why this matters beyond one constant

If the hypothesis holds, the bug is present at 16 too — merely not visible.
Any SVG whose paths already produce many points would hit it today, on every
backend. That makes this worth chasing rather than pinning the constant and
moving on.

## Reproduction

```bash
sed -i 's/PATH_CUBIC_STEPS: int = 16/PATH_CUBIC_STEPS: int = 64/' vg/grammar/shapes.ae
aeb apps/svg_render_png/.build.ae
python3 vg/test/svg-compare-aevg.py --svg-dir ~/scm/tsyne/tsyne/cosyne/test/svg python.svg
# 3.1 -> 56.3
```
