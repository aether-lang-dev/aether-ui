# Stage 3 design note — per-frame surfaces, then occlusion

**Status: design only, not built.** Written after Stages 0–2.5 landed and
clipping turned out to deliver nothing (see
`retained-compositor.md` and `stage2-clip-flicker-fix.md`). This note fixes
the API shape and the acceptance test *before* implementation, because the
previous approach was capped in a way nobody noticed until four rounds in.

## Why the previous approach capped out

Stage 2.5 gave the GTK canvas one retained surface and clipped both the clear
and the replay, so pixels outside the damage region survived between paints.
That is correct — for content that does not change.

It cannot work for live content. **A retained surface preserves only pixels
that do not change.** `frames_demo` animates two cubes at 60fps, so anything
outside the clip is stale the instant it is skipped: the un-dragged frame's
cube stops advancing and then blanks. The Stage 2.5 invariant —

> a clip is valid only if the painter redraws everything visible **inside** it
> AND everything **outside** it is already correct on the surface

— has an unsatisfiable second half here. `frames_demo` consequently skips
clipping whenever `scene_is_refreshing`, and today **zero clipped paints occur**
(measured: ~237 paints per drag, all 614400px, full canvas).

The fix is not a better clip. It is to stop sharing one surface.

## The shape

**Each frame owns a surface.** Then:

- a frame whose content has not changed is **blitted**, not re-rendered;
- a frame that is animating re-renders **only itself**, into its own surface,
  at its own rate — its neighbours are untouched, so they cannot go stale;
- the composite walks frames **back-to-front**, blitting each; once frames can
  be redrawn independently, **occlusion subtraction becomes meaningful** and
  is layered on top.

That ordering is the point. Occlusion is worthless while everything must be
redrawn together — which is why the original "Stage 3 = opaque subtraction"
framing put the cart first.

## What already exists (more than expected)

The primitive needed is an offscreen render target plus a blit, and most of
that is present:

- **`canvas_draw_image(cid, x, y, iw, ih, rgba, len)`** is implemented on all
  three backends (`aether_ui_canvas_draw_image_impl` in each). So *blitting an
  RGBA buffer into a canvas is already a solved, cross-platform operation.*
- **`canvas_write_png`** proves each backend can render a command buffer into
  an offscreen surface headlessly (cairo image surface / `CGBitmapContext` /
  `CreateCompatibleBitmap` + DIB).
- **The live-region raster path** (`vg/region.ae`, `LIVE_RASTER`,
  `region_push`/`region_frame`) already models "a rectangle of the scene whose
  pixels are produced independently and blitted in Z order". `_blit_one_raster`
  in `vg/live.ae` does the blit. **This is the closest existing analogue to
  what Stage 3 needs, and the new work should extend it rather than invent a
  parallel mechanism.**
- **GTK4's `canvas_ensure_paint_surface`** (~24 lines, allocate-or-reuse keyed
  on size) is the per-canvas version of the allocator each frame now needs.

So Stage 3 is less "new backend capability" than "generalise the raster region
from one-per-scene to one-per-frame, and drive it from damage rather than from
a frame clock".

## What the code actually looks like (surveyed 2026-08-08, before implementing)

Reading the real call path changed the plan in one important way, recorded
here so it is not rediscovered.

**Frames do not own command buffers.** `_draw_frame` emits *vg scene nodes*
(`rect`, `text_sized`, `path`) into a shared node tree `rn`, which is
rasterised once, downstream, by whoever owns the canvas. There is no
per-frame command buffer sitting there waiting to be replayed offscreen, so
"render this frame to its own surface" has no seam to cut at as written.

**But the seam does exist, one layer down.** `render_one_draw`
(`vg/module.ae`) already renders a DRAW region's callback as an independent
**sub-scene**: `scene_new` → invoke the callback → `vg_flush` →
`scene_free_recording`. That is exactly "produce this rectangle's contents
independently", and it is the mechanism Stage 3 should generalise — the
design note's instinct to extend the region path rather than invent a
parallel one was right.

**What is missing is the offscreen target.** `render_one_draw` passes the
parent's `s.backend` into the sub-scene, so its commands land in the same
canvas buffer as everything else. Nothing in the stack can currently
redirect a render into memory:

- `canvas_write_png` renders a command buffer offscreen — but only to a
  **file**;
- `canvas_read_pixel` renders offscreen — and returns **one pixel**;
- every RGBA extern in `aether_ui_backend.h` passes buffers **into** the
  backend (`canvas_draw_image`, gradient stops), never out.

So Stage 3 needs one new backend primitive: **render the current command
buffer into an in-memory RGBA buffer**. Confirmed absent on all three.

**That primitive is a variant of code that already works.** GTK4's
`canvas_write_png_impl` is 16 lines: create a cairo image surface, call
`canvas_replay(cr, cs)`, write it out. Returning
`cairo_image_surface_get_data` instead of writing a PNG is a small change,
and `canvas_read_pixel_impl` already keeps a cached replay surface keyed on
`(gen, count, w, h)` — the caching Stage 3 wants, already written. **win32
and macOS both implement `write_png_impl` too**, so all three have a working
offscreen replay to build on. This is not new rasterisation on any backend.

`canvas_draw_image_scaled` already blits source→dest with scaling, which
covers the hidpi risk listed below.

## The blocker: deferred scenes have no command range to bracket

**Attempted 2026-08-08, measured, reverted.** The obvious wiring does not
work, and the reason is worth stating precisely because it looks like it
should.

The plan was: bracket `_draw_frame` with `canvas_cmd_count` before and after,
take the difference as that frame's contiguous command slice, render the
slice offscreen, blit it next paint. Implemented end to end. Result:

```
composite: rerender=3 blit=0 total=249/0 gdraw=83 paints=83
```

Zero blits in 83 paints. Instrumenting the loop showed why — `cmd_count`
read **2 on every call and never grew**:

```
DBG cid=1 cmdcount=2   (identical for every frame, every paint)
```

**The live `vg` path runs the scene DEFERRED** (`scene_set_deferred(sub, 1)`
in `render_one_draw`; `deferred = 1` in `vg/module.ae`). Shapes are recorded
into a `pending` list during the block and dispatched to the canvas by
`vg_flush` only *after* the whole callback returns. So while `frames_draw` is
running, **no frame's commands exist in the canvas buffer yet** — there is
nothing to bracket, and nothing to slice.

This is not a bug to fix in the frames layer. It is the deferred design doing
what it is for: shapes are dispatched with their final cached style, which is
why the live path can re-style without redrawing.

**Consequences for the next attempt.** Bracketing at the frames layer is
dead. The seam must be somewhere the commands actually exist:

1. **Slice at flush time.** `vg_flush` walks `pending` in draw order. If a
   frame tags its pending entries, the flush knows each frame's range as it
   dispatches. Most faithful to the existing design; needs a tag on
   `VgPending`.
2. **Give each frame its own sub-scene**, as `render_one_draw` already does
   per draw-region: `scene_new` → invoke content → `vg_flush` → capture. The
   frame's commands then land in a buffer of its own, and no range arithmetic
   is needed. Closest to the note's original "generalise the raster region"
   instinct, and probably the right one.
3. **Capture after the fact from the shared buffer** using the frame's *rect*
   rather than a command range — render the whole buffer once and cut out
   each frame's pixels. Wrong: it cannot skip the drawing work, which is the
   entire point.

Option 2 is the recommended next step. Note it needs the frame's content to
be renderable into a sub-scene whose backend targets an offscreen surface,
which the `(ox, oy)` argument on `canvas_render_range_rgba` already
anticipates.

## Option 2 attempted: the sub-scene works, the CACHE does not

**Attempted 2026-08-08, measured, reverted.** Option 2 above — give each
frame its own sub-scene — was implemented. The sub-scene mechanism works
exactly as predicted, and the cache built on it is still wrong. Both halves
matter.

**What worked.** `scene_new(scene_ctx, scene_backend)` + `scene_set_deferred`
+ `_draw_frame(bf, vg_root(sub), t)` + `vg_flush(sub)` puts a frame's
commands in the canvas buffer at a point where they can be bracketed. The
ranges came out contiguous and correct:

```
DBG enter cid=1 start=2   iw=460 ih=320   -> fin=153   (151 commands)
DBG enter cid=1 start=153 iw=500 ih=360   -> fin=304   (151 commands)
DBG enter cid=1 start=304 iw=300 ih=200   -> fin=371   ( 67 commands)
DBG cap got=588800 need=588800            (capture full, every frame)
```

The acceptance test went **green** — `gdraw=1` across `paints=102`, exactly
the claim. `frames` 10/0, `golden` 2/0, goldens unchanged.

**And the canvas was blank.** A screenshot showed nothing but the `#dfe5ec`
background: no frames, no cubes. Before the change, 27557 bytes of PNG with
three frames and two cubes; after, 3231 bytes of flat colour.

**Why.** `vg/live.ae` calls `ui.canvas_clear(canvas_id)` at the start of
every paint, and `aether_ui_canvas_clear_impl` **frees the pixel buffers of
`CANVAS_DRAW_IMAGE` commands** as it empties the buffer. So the blit command
issued for a cached frame is queued into a command buffer that is cleared
before it is ever painted. Nothing survives to the screen. The `start=2` at
the top of each paint is the clear's fingerprint.

The deeper point: **a command-buffer blit is not a substitute for drawing,
because the command buffer is rebuilt from scratch every paint.** Caching
pixels only helps if the cache lives somewhere the per-paint clear does not
reach. The retained *paint surface* (Stage 2.5, GTK4's
`canvas_ensure_paint_surface`) is such a place; the command buffer is not.

**Every automated signal said success.** The counters, the acceptance test,
the goldens and all 11 other specs were green while the app rendered
nothing. The specs read the status *label*; the goldens have no frames. The
only thing that caught it was looking at a screenshot — which is exactly the
check the Risks section below says to do by eye, written before any of this
was built.

**Consequences for attempt 3.** Options 1 and 2 are both dead for the same
underlying reason, so the surviving direction is narrower:

- The cached surface must be composited **at paint time**, not queued as a
  command — i.e. into the retained paint surface, below/around the clip, in
  the painter itself. That means the frames layer cannot own the blit; the
  backend paint path has to.
- Or: frames stop drawing into the shared scene at all and become genuine
  live regions (`LIVE_RASTER`), which already have a paint-time blit that
  survives the clear (`_blit_one_raster`). This is the closest thing to a
  working precedent in the tree and should be evaluated first.
- **Any attempt 3 must be checked with a screenshot before it is believed.**
  Add a pixel assertion to the frames spec so the harness can see it too.

## Proposed API

In `ui/frames.ae`, additive — the existing `frames_draw` path stays working:

```
frame_set_cache_mode(frame, mode)   // 0 = draw inline (today), 1 = own surface
frame_invalidate(frame)             // mark content dirty; next paint re-renders
frame_surface_valid(frame) -> int   // 0 if the next paint must re-render it
```

and on the host:

```
frames_composite(host, rn, t)       // blit back-to-front; re-render only
                                    // frames whose surface is invalid
frames_rerender_count(host) -> int  // how many frames re-rendered last paint
frames_blit_count(host) -> int      // how many were blitted instead
```

Those last two are **the instrumentation the acceptance test needs** — they
must exist from the first commit, not be retrofitted. Stage 2's assertions
were unfalsifiable precisely because the only observable was an area number
that could not distinguish "clipped correctly" from "clipped wrongly".

A frame is invalidated by: content callback requesting it (an animating
payload calls `frame_invalidate` each tick), a resize, or a theme/chrome
change. Moving a frame does **not** invalidate it — that is the whole saving:
a dragged frame's pixels are unchanged, only its position is.

## Acceptance — falsifiable, and it cannot pass vacuously

**The claim:** with two frames where only one animates, the still frame
re-renders **zero** times while the animating one re-renders every frame.

```
Alpha animating, Beta static, 60 paints:
    frames_rerender_count summed  == 60   (Alpha only)
    frames_blit_count summed      == 60   (Beta only)
```

Today both re-render on every paint, so the assertion reads 120/0 and fails.
**Demonstrate that failure before landing the fix** — three regression tests
in this repo shipped green against the very bugs they guarded, which is why
this rule is written into every stage now.

Second claim, once occlusion lands: a frame fully covered by an **opaque**
frame is neither re-rendered nor blitted.

> **Honesty check.** `frames_demo`'s frames are 0.42-alpha translucent
> (`c284f99`). They are **not opaque** and cannot be occluded. Any
> implementation reporting an occlusion saving on the current demo is
> reporting a bug. Test occlusion with an explicitly opaque frame, or make
> opacity a per-frame property and use an opaque one.

Third: the golden gallery stays byte-identical. A compositor that is fast and
wrong is worse than the immediate mode we have.

## Risks

- **Memory.** One surface per frame at canvas resolution. Ten frames on a
  1920×1080 canvas is ~80MB of ARGB. Needs a cap, or surfaces sized to the
  frame rather than the canvas (the latter is correct and no harder).
- **Scaling and hidpi.** A frame surface rendered at 1× and blitted onto a 2×
  canvas will be soft. The existing raster path already has scaled-blit
  handling (`_blit_one_raster`) — reuse its approach rather than re-deriving.
- **Translucency ordering.** Blitting back-to-front is correct for alpha, but
  only if each frame's surface is composited with its own alpha rather than
  baked in. Get this wrong and the current translucent look breaks — which the
  golden gallery will not catch, because the gallery has no frames. Check it
  by eye.
- **Backend parity.** `canvas_draw_image` exists everywhere, so the blit is
  safe; the offscreen *render* differs per backend and is where the work is.
  Per the standing rule, a backend that cannot yet do it must be **gated off**,
  not left enabled-and-wrong.
- **Do not port Stage 2.5b first.** Porting the shared retained surface to
  win32/macOS buys nothing for the animated case and delays this.

## Suggested first commit

Not the whole stage. Just:

1. `frames_rerender_count` / `frames_blit_count`, reporting today's behaviour
   (everything re-renders, nothing blits).
2. A spec asserting the *current* 2-renders-per-paint truth, so the numbers
   are pinned before anything moves.

That is a small, safe, falsifiable starting point — and it makes the
subsequent change measurable rather than assertable.
