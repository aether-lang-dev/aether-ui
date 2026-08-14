# win32: a second renderer (GDI+) behind a switch, judged by MAE

**Status: design only, not built.** Written before implementation because the
last three-stage effort (retained compositor) was wrong twice in ways a
design note caught cheaply, and because this one has a measurement trap in
it that would otherwise have shipped (see *Thresholds*, below).

## Why

GDI has no alpha. Three places in `backend/aether_ui_win32.c` prove it:
`canvas_replay_to_dc` fills the whole surface white before replaying; the
`CV_IMAGE` path composites incoming RGBA against white and stores `a=255`;
`canvas_read_pixel_impl` returns `(255 << 24)` unconditionally.

The consequences are already paid for elsewhere in the tree:

- **Stage 3 frame caching is gated off on win32** — a cached translucent
  frame would blit as an opaque white rectangle over what is beneath it.
  win32 gets occlusion (pure geometry) but never caching.
- **No antialiasing**, so win32's rendering is visibly coarser than the
  other three backends.
- **Textured cube faces** (the `PlgBlt` route to video-on-a-face) would
  inherit the same limitation at the polygon edges.

GDI+ fixes all three and, unlike Direct2D, needs no COM: it has a flat C
API and ships with Windows. It is also in maintenance and has known bugs,
which is exactly why this is a *comparison*, not a migration.

## Shape: branch-by-abstraction, not branch-by-if

The seam is unusually favourable. `canvas_replay_to_dc(cv, mem, w, h)` has
**two callers** — the on-screen paint (`:5269`) and the headless readback
(`:5318`) — and handles **11 command kinds**. The file is 6,474 lines but
the drawing surface is concentrated in one function.

```c
static void canvas_replay_to_dc_gdi    (Canvas*, HDC, int, int);  /* today's, renamed */
static void canvas_replay_to_dc_gdiplus(Canvas*, HDC, int, int);  /* new */

static void canvas_replay_to_dc(Canvas* cv, HDC mem, int w, int h) {
    if (win32_use_gdiplus()) canvas_replay_to_dc_gdiplus(cv, mem, w, h);
    else                     canvas_replay_to_dc_gdi(cv, mem, w, h);
}
```

Both callers stay untouched. The GDI+ implementation is a fresh 11-case
switch rather than conditionals threaded through the existing one — which
is the whole point: **two implementations you can read and run
independently**, rather than two interleaved paths you can never cleanly
diff. Branch-by-if would make the comparison this document exists to
support impossible to interpret.

### Selection: env var, not compile flag

```
AETHER_UI_WIN32_RENDERER=gdiplus      # the default, and what ships
AETHER_UI_WIN32_RENDERER=legacy       # opt OUT, back to plain GDI
```

One binary, two renderers, chosen at run time. A compile flag would mean
two builds and a far weaker comparison — you could never be sure a
difference came from the renderer rather than the build.

**GDI+ is now the default.** The original wording here was "GDI stays the
default until the numbers argue otherwise" — they now do, decisively:

|                | good (<5) | ok (5–15) | diff (≥15) | mean |
|----------------|-----------|-----------|------------|------|
| legacy GDI     | 131       | 40        | 37         | 20.87 |
| GDI+           | 172       | 18        | 17         | **4.71** |

Scored over the 208-file SVG corpus against librsvg. On the 167 files that
carry a real `viewBox`, GDI+ means **3.00** against the GTK4 reference
backend's 1.77, with 104 of 167 within 1.0 of it. (The other 40 files have
no `viewBox`, so `rsvg-convert` and our renderer infer different canvas
extents and the diff scores a scale mismatch rather than rendering. See
"Files the corpus cannot fairly score" below.)

Legacy is **kept, not deleted**. It is the second arm of
`tests/win32/capture-pixelgrid.sh`, it is the fallback when
`GdipCreateFromHDC` fails, and it makes this flip revertible with one
environment variable if a real application regresses in a way 208 static
SVGs did not catch. Deleting it is a later commit, once GDI+ has run as the
default for a while.

### The trap to avoid

`canvas_read_pixel_impl` and `canvas_painted_pixels_impl` must go through
the **same switch**. If the metrics read one renderer while the screen
shows the other, every pixel assertion measures the wrong thing — which is
precisely the class of bug that bit twice this session (`canvas_read_pixel`
replaying its own command buffer while the screen stayed black; a circular
`cache=` gate that could never contradict itself). Same seam, or the
instruments lie.

## Judging it: per-pixel MAE between the two renderers

Both renderers consume the **same command buffer**, so unlike the SVG
conformance harness there is no reference-renderer disagreement muddying
the signal. Every non-zero pixel is attributable to the drawing layer.

The loop:

1. run a suite on winbaz with `=legacy`, capture `GET /screenshot` per suite
2. run it again with `=gdiplus`, capture again
3. score the pairs with `pixel_mae()` — already written, at
   `vg/test/svg-compare-aevg.py:306`, numpy over RGB
4. rank by MAE; eyeball the top of the list

### Verified prerequisites (measured 2026-08-10, this box)

| Thing | Result |
| --- | --- |
| `/screenshot` on real hardware, no Xvfb | works — 31,278 B PNG |
| Two captures, same renderer, settled scene | **byte-identical** |
| MAE of that pair | **0.0** exactly |
| numpy + PIL | present |

MAE 0.0 between repeat captures is the load-bearing fact: **there is no
capture noise**, so any non-zero score is genuine renderer difference.

### Thresholds — do NOT reuse the SVG buckets

`svg-compare-aevg.py`'s `bucket()` uses good <20, ok 20–40, diff ≥40. Those
are calibrated for whole-image SVG conformance and are **far too loose
here**. Measured on a real 560×480 capture:

| Simulated change | MAE | SVG bucket says |
| --- | --- | --- |
| 1-pixel shift of the whole image | 4.02 | good |
| ±3 levels of noise everywhere (AA-scale) | 1.71 | good |
| **an element missing over 11% of the frame** | **13.4** | **"good"** |

A missing element scoring "good" would make this comparison worthless. The
signal we care about lives in **single digits**, so:

```
< 1.0    identical in practice — GDI+ faithful, bought nothing here
1 – 8    antialiasing / alpha scale — the EXPECTED improvement
8 – 15   substantial: look at it, could be better or broken
> 15     something is missing or misplaced — regression until proven otherwise
```

These are starting points to be re-tuned once real pairs exist; the point
is that they are chosen for *this* comparison rather than borrowed.

### MAE ranks; a human judges

The instrument cannot tell improvement from regression, and pretending
otherwise would be the same mistake as an unfalsifiable assertion:

- high MAE on **antialiased edges** → GDI+ better (GDI has no AA)
- high MAE in **translucent regions** → GDI+ better (the whole point)
- high MAE from a **missing element** → GDI+ broken
- near-zero everywhere → faithful, but bought nothing

Only the third is a regression. This is a **triage instrument**, not a
pass/fail gate, and the note says so deliberately so nobody later wires it
into CI as one.

## What "done" looks like

Not "GDI+ replaces GDI". The deliverable is a *decision*, supported by:

1. every suite scored both ways, ranked by MAE;
2. the top divergences eyeballed and classified improvement / regression;
3. a specific answer on whether **Stage 3 caching can be un-gated on
   win32** — the concrete prize, and testable: `frames` currently reports
   `rerender=4 blit=0 cache=0` there, and would report `blit>0` if GDI+
   carries alpha correctly;
4. goldens: `tests/goldens/win32/*.sig` **will change** under GDI+.
   Antialiasing guarantees it. They are signature files, not PNGs, so they
   cannot serve as the "before" side of the MAE comparison — capture both
   sides fresh from one binary instead. Re-blessing them is a deliberate
   step taken *after* the eyeball pass, never before.

## Risks

- **GDI+ initialisation** is process-global (`GdiplusStartup`/`Shutdown`)
  and must not run per paint.
- **Text metrics may shift.** GDI+ measures text differently; specs that
  assert widget geometry could move. That is a genuine behaviour change,
  not just pixels, and would show up as spec failures rather than MAE.
- **`PlgBlt` for cube faces is GDI**, so a textured-face implementation
  would need the GDI+ equivalent (`Graphics::DrawImage` with a
  destination-parallelogram overload) to benefit — worth designing the two
  together if both proceed.
- **Maintenance status.** GDI+ is frozen. If its bugs bite our 11 command
  kinds, the answer is Direct2D and a much larger effort — which this
  comparison would at least have established with evidence.

## Suggested first commit

Not the renderer. The **harness**: capture both ways, score, and report —
with the GDI+ path stubbed to call the GDI one. That produces MAE 0.0
everywhere, which proves the measurement works end to end before any
drawing code exists, and gives an honest baseline to move away from. Same
discipline as Stage 3's counters landing before the caching did.

## Files the corpus cannot fairly score

**40 of the 207 scored files have no `viewBox`.** With no viewBox, the
canvas extent is *inferred*, and `rsvg-convert -w 400 -h 400
--keep-aspect-ratio` infers it differently from our renderer. The diff then
measures a scale-and-position mismatch, not rendering.

`tiger.svg` is the clearest demonstration: it renders beautifully on both
backends, yet scores 59.4 (GTK4) / 59.2 (GDI+). Normalise for scale by
cropping both to their ink bounding box and resampling, and it drops to
**7.7 / 10.6** — roughly 85% of that score was the artefact. `bloglines.svg`
is worse still: its attribute is spelled `viewbox`, lowercase, which is not
valid SVG at all, and the file is one every browser paints differently.

So there are two means worth quoting, and the narrower one is the honest
measure of the renderer:

| set | GTK4 | GDI+ | macOS |
|-----|------|------|-------|
| 167 files with a real `viewBox` | **1.32** | **2.61** | **1.61** |
| all 207 | 2.79 | 4.28 | 3.37 |

*(Superseded numbers: this table read 1.77 / 3.00 / — when first written.
Everything moved on 2026-08-13/14 from work that was NOT about GDI+ at all:
`svg_render_png` sizing its canvas from the SVG's own aspect instead of
always square, `font-family` reaching the backends, `fill-rule` and
`gradientTransform` being carried rather than guessed, and stroked text
finally drawing on macOS. See
[semantics-belong-above-the-abi.md](semantics-belong-above-the-abi.md).
The GDI+/GTK4 gap narrowed as a side effect of fixing the vg layer, which is
the point that document makes.)*

## What is still wrong on GDI+

Nine of the 167 fairly-scored files are more than 3 MAE worse than GTK4.
They are four causes, not nine problems:

1. ~~**Radial gradient geometry**~~ **FIXED 2026-08-13.** The fill sized
   itself from the shape's bounding box rather than the gradient's own
   centre and radius, which `CanvasCmd` had carried all along. `json`
   27.25 → 3.21, `jsonatom` 26.50 → 4.29.
2. ~~**Non-uniform `gradientTransform`**~~ **FIXED 2026-08-13**, and it was
   indeed a vg-layer change that benefited all three backends, exactly as
   this entry predicted. The matrix was collapsed to a scalar at **three**
   sites, not one; `rx`/`ry`/`rot_deg` now travel and every backend renders
   the real ellipse. Verified by an axis-ratio oracle rather than by MAE:
   `tests/radial_ellipse_check.py`, GTK4 5/5, macOS 5/5, GDI+ 5/5.
3. **Radial gradient STROKES still use a linear brush** — `php` 4.2, and it
   still scores *well by accident*. Three corrected geometries were measured
   and all scored worse, because they trade php against the dedicated radial
   tests. Genuinely open.
4. ~~**Stroked text is not painted**~~ **FIXED 2026-08-13/14** on macOS
   (via `NSStrokeWidthAttributeName`, which is a stroke-only mode when
   positive) and GTK4 (whose stroke pass was selecting a *different face*
   than its fill pass). `bloglines` 39.10 → 26.59 on GTK4, 71.93 → 49.51 on
   macOS. **Still open on win32**: `GdipDrawString` can only fill, a filled
   second pass overpaints rather than outlines (measured: moved `Steps.svg`
   *away* from GTK4), and it needs glyph outlines via `GdipAddPathString`.
   Minimal repro: `vg/test/svg/text_stroke_repro.svg`.

So two of the four are closed, one is half-closed, and one (php's radial
stroke) is genuinely open. The two that closed did so in the **vg layer** —
see [semantics-belong-above-the-abi.md](semantics-belong-above-the-abi.md).
