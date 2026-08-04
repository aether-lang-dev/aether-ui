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
