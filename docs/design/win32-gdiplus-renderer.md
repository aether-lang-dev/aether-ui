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
AETHER_UI_WIN32_RENDERER=gdiplus      # opt in
AETHER_UI_WIN32_RENDERER=legacy       # default, and what ships
```

One binary, two renderers, chosen at run time. A compile flag would mean
two builds and a far weaker comparison — you could never be sure a
difference came from the renderer rather than the build.

**GDI stays the default** until the numbers argue otherwise.

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
