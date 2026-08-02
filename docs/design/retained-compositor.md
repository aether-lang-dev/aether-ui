# The Retained Compositor — aether-ui's rendering north star

> **Status: Stage 0 built; Stages 1–5 specified, not built.** This records the
> high bar for aether-ui rendering, what we already have that seeds it, the
> gap, and a staged path to it. The rendering path is still immediate-mode
> (see "Where we are today") — no dirty regions exist yet. The staged path
> below is written to be built from: each stage names its deliverable, its API
> surface, and the test that proves it. **Next up: Stage 1, the region type.**

## Licensing boundary (read first)

The technique described here — **dirty-region tracking with opaque-region
subtraction and multi-pass composition** — is a decades-old, widely-documented
approach to incremental redraw. It long predates any one implementation:
Amiga's layers/damage lists, the X11 DAMAGE and region (`XRegion`) machinery,
classic Mac/Windows `InvalidateRect`/update-region models, and the academic
literature on rectangle CSG and occlusion culling all describe it. It is prior
art, freely usable.

**This document is a clean-room specification written from that general prior
art and from our own existing code. It is NOT derived from, and must NOT be
implemented by porting, any GPL-licensed source** — including ChrysaLisp,
whose GUI is a fine *example* of the technique but whose code, data
structures, naming, and algorithms we will not read, transcribe, or adapt.
aether-ui is MIT-licensed; the compositor we build must be independently
authored from first principles and the public prior art. If you implement
this, do not open a GPL codebase for reference — work from this doc, the X11
region API shape, and cairo's clipping primitives.

## The bar

The target is the classic CPU compositor miracle: **overlapping, independently
animating regions** — think two windows each rendering a tumbling cube, one
dragged over the other — repainting with **no flicker, no glitch, and cost
proportional only to the pixels that actually changed and are actually
visible.** Drag the front window and only the newly-*exposed* slivers of the
back one repaint; the area still covered by the front window is never touched.
A live video or game rectangle in the middle of a vector scene repaints only
its own bounds at its own frame rate, while the static chrome around it costs
nothing per frame.

This was achieved on CPUs before GPUs existed. It is not about raw blit speed;
it is about **never drawing a pixel you don't have to.**

## The technique (from general prior art)

Three ideas, composed:

1. **A retained view/element tree.** UI/scene elements form a tree; each has a
   bounding rectangle in its parent's coordinates. (Contrast immediate mode,
   where you replay a flat command list every frame.)

2. **A region as a set of non-overlapping rectangles.** Not a single bounding
   box — a true 2D area built by rectangle CSG: union (add a rect, splitting/
   merging to keep them non-overlapping), subtraction (remove a rect, splitting
   survivors), intersection/clip, translate between coordinate spaces. This is
   the same shape as X11's `XRegion`/`pixman_region`. Each element carries:
   - a **dirty region** — the part of it that changed and needs redraw, and
   - an **opaque region** — the part of it that is fully opaque (nothing behind
     it shows through there).

3. **Multi-pass composition that subtracts occlusion.** The essential move:
   - Propagate dirty areas **up** the tree (a changed child dirties its
     ancestors over its bounds).
   - **Subtract** each opaque element's region from the dirty regions of
     everything behind it (ancestors and earlier-drawn siblings) — so covered
     pixels are removed from the work list entirely.
   - Distribute the surviving dirty region **down**, clipped to each element's
     bounds, producing per-element "what I must actually redraw" rectangles.
   - Draw each element **clipped to those exact rectangles** into an off-screen
     backbuffer, then flush the backbuffer's changed bounds to the screen once.

The occlusion subtraction is the whole game: it is why a dragged front window
costs only the exposed back-window slivers, and why a static scene around a
live rect costs zero per frame. Overdraw is eliminated, not merely buffered.

## Where we are today (immediate mode)

aether-ui's canvas backend is **immediate-mode**: a flat command buffer
(`begin_path`/`move_to`/`line_to`/`stroke`/`fill_rect`/`arc`/`fill`/
`fill_text`/`draw_image`/gradients) replayed **in full** on every paint, via
`canvas_replay()` over cairo (GTK4), CoreGraphics (macOS), GDI (Win32). AeVG
shape factories emit into this buffer through the `backend_dispatch` table.

What this gives us:
- **No flicker** — GTK/cairo (and the AppKit/Win32 equivalents) double-buffer,
  and the toolkit does widget-level damage tracking *between* widgets.
- **Correct static rendering** — fine for SVG, the transpiler's output, and the
  librsvg parity harness (all of which render once, to a window or a PNG).

What it does **not** give us:
- **No intra-canvas dirty regions.** A single canvas widget is all-or-nothing:
  to change one shape we replay every command. There is no opaque tracking, no
  occlusion subtraction, no exact-rect clipped redraw *within* the canvas.
- So an animated/live element (video, game, a moving shape) forces a
  whole-canvas redraw per frame: O(entire scene) work for an O(one rect)
  change. This is the architectural ceiling the bar above breaks through.

## What we already have that seeds it

The retained compositor is **not a rewrite from zero.** The reactive and
element layers already built are precisely its foundation:

- **Per-element bounds.** `grammar_element` stores an axis-aligned bounds rect
  per `AevgElement` (`bounds_x/y/w/h` + `has_bounds`) and a working
  `element_hit_test`. That bounds rect is exactly *one rectangle* of a future
  dirty/opaque region. We already recompute it when geometry changes
  (`grammar_reactive.recompute_bounds` on a `bindPos`).
- **A retained-ish element list.** The context tracks elements
  (`ctx_tracked_elements`, `ctx_track/untrack_element`); `grammar_bind` already
  adds/removes/destroys elements (data-driven regions). This is the seed of the
  view tree — it needs to become an ordered tree with parent/child + z-order,
  but the membership and lifecycle machinery exists.
- **A reactive refresh loop.** `refresh(ctx)` → `grammar_reactive` re-evaluates
  per-element bindings and `grammar_bind` diffs regions. Today a refresh
  re-dispatches the whole scene; the compositor change is to make refresh mark
  **only changed elements' bounds dirty** and composite just those.
- **A backbuffer + flush already exist** at the toolkit layer (cairo image
  surface / GTK double buffer; `canvas_write_png` already renders the buffer
  off-screen — proof we can composite to an off-screen target and flush).
- **cairo gives us clipping for free.** `cairo_rectangle` + `cairo_clip` (and
  `CGContextClipToRect` / GDI clip regions) implement the per-element "draw
  clipped to these exact rects" step natively. We do not need to write a
  software clipper; we need to *compute the rects* and set the clip.

So the missing pieces are specific and bounded:
1. A **region type** — a set of non-overlapping rectangles with union/subtract/
   intersect/translate. Independently authored (or wrap an MIT-licensed region
   lib like pixman, which cairo already links). **This is the one genuinely new
   data structure.**
2. An **ordered element tree** with z-order and opaque flags (extend the
   existing tracked-element list).
3. The **multi-pass composite** (dirty up, subtract opaque, distribute down,
   clipped redraw) replacing "replay the whole command buffer." The passes are
   plain tree walks over (1) and (2).

## Staged path

Each stage is independently shippable and leaves the static path working.
Stages 0–5 below are written to be built in order; each names its
deliverable, its acceptance test, and how it is proven.

### Stage 0 — `ui.frames`: the internal-frame widget *(the harness)*

**Status: built as the benchmark harness.** `ui/frames.ae` now provides the
retained frame host/model, z-order, hit testing, drag, resize, close, and VG
chrome. `apps/frames_demo` exercises two overlapping, continuously tumbling
cube payloads inside one canvas, and `tests/frames_demo/spec_frames_demo.ae`
gates the retained behaviours plus the last-paint area/command-count readback
through AetherUIDriver. GTK reports paint metrics via
`GET /canvas/{id}/debug`; the shared driver server exposes the same route and
returns 501 on backends that have not wired the metric yet.

**What the `area` metric is NOT, yet.** `last_paint_area` currently reports
`width * height` — the canvas allocation, which is a CONSTANT while the
window is unresized (700x420 = 294000 in the demo today). It proves the
readback plumbing works end to end; it cannot yet express the Stage 3
claim, because immediate mode genuinely does repaint the whole canvas
every frame, so "area repainted" and "canvas area" are the same number
until dirty regions exist. Stage 2 is where the metric acquires meaning:
once the composite sets a clip from a dirty region, `last_paint_area`
must become the summed area of the clip rects actually painted. The
Stage 3 assertion (B repaints ~(1-f) of its area when A covers fraction
f) is only falsifiable after that change — asserting on it before then
would pass vacuously against a constant.

**Why it came first.** It is useful on its own, it is pure vg over today's
immediate-mode canvas, and it is the consumer that makes every later stage
*measurable* rather than speculative. A JInternalFrame-alike: draggable,
resizable, z-ordered sub-windows living inside ONE canvas we own end to
end. It must be internal frames, not OS windows — two real windows would
prove nothing, because the host compositor (Mutter, DWM, Quartz) would be
doing the work rather than us.

Module `ui/frames.ae`, app-agnostic and reusable:

```
frames_new(scene, x, y, w, h)      -> ptr   // a frame host over one scene
frame_add(host, title, x, y, w, h) -> ptr   // returns a frame handle
frame_set_content(frame, cb)                // cb(rn, t, x, y, w, h)
frame_move / frame_resize / frame_raise / frame_close
frame_bounds(frame) -> (x, y, w, h)         // in host coords
frame_z(frame) -> int                       // 0 = back
frames_hit(host, x, y) -> ptr               // topmost frame at a point, or null
frame_count(host) -> int
frames_draw(host, rn, t)                    // paint all frames, back → front
frames_on_click/move/release(host, ...)     // gesture routing
```

(As built. The content callback receives the frame's current content rect
rather than a translated origin — the payload scales itself into `(x, y, w,
h)`, which is what lets a resizable frame hold a cube without the cube
knowing anything about frames.)

Behaviour: title bar drag moves; bottom-right gripper resizes (min 80×60);
clicking anywhere in a frame raises it to front; a close box removes it.
Chrome is drawn with vg primitives (`rect`/`path`/`text_sized`/`fill`/
`stroke`) — no native widgets and, as built, no `ui.icons` dependency, so it
renders identically on all four backends.

*Acceptance:* a driver spec that adds two frames, asserts z-order after a
raise, drags one and asserts its new bounds, resizes and asserts the new
size, and confirms `frames_hit` returns the topmost frame in an overlap.

*Demo app* `apps/frames_demo`: **two tumbling cubes in one window**, each in
its own internal frame — continuously animating, six painter-sorted
translucent faces apiece, scaled into whatever content rect its frame
currently has. Overlap the two frames and today, on immediate mode, both
repaint in full forever. That is the baseline the compositor has to beat,
and this app is how we see it.

The cube geometry is a local copy, deliberately: `apps/tumbling_cube` folds
its screen framing into one projective mat4 to stay pixel-identical to its
pre-vg3d original (its goldens depend on that), whereas a framed cube needs
a per-call frame-local projection. Extracting a shared `cube.ae` — the
vertex/face/normal tables, `cube_model`, `face_depth`, the spin integrator,
~80 lines — is worthwhile once there is a third consumer, but it must leave
each caller's projection alone. Note `const` lowers to `#define`, so a
shared module needs deliberate prefixing to avoid colliding with MinGW
headers on Windows.

### Stage 1 — the region type *(the one new data structure)*

**Status: built as a rect-list v1.** `vg/geom/region.ae` implements the
region API below using a disjoint rectangle list: adding a rect subtracts
existing coverage from the incoming rect, then appends only uncovered
survivors; subtract/intersect/translate operate in place. The implementation
is intentionally simple rather than banded because Stage 1/2 rect counts are
tiny. `vg/test/test_region.ae` covers overlapping union, middle and covering
subtractions, region subtraction, translate/intersect, clone independence,
area conservation, and degenerate inputs; it is wired into `ci.sh` Phase 0.

`vg/geom/region.ae` — a set of **non-overlapping** rectangles. (Note: the
name `vg/region.ae` is taken by the live-region abstraction; this is
`geom/region` to avoid the collision.)

```
rgn_new() / rgn_free / rgn_clone
rgn_add_rect(r, x, y, w, h)        // union, splitting to keep disjoint
rgn_sub_rect(r, x, y, w, h)        // subtract, splitting survivors
rgn_sub_region(r, other)           // the occlusion primitive
rgn_intersect_rect(r, x, y, w, h)  // clip
rgn_translate(r, dx, dy)
rgn_is_empty(r) -> int
rgn_bounds(r, out4) -> int
rgn_nrects(r) -> int ; rgn_rect(r, i, out4)
rgn_area(r) -> float               // for the cost assertions below
```

Implementation: band-based (rows of y-spans, each holding sorted disjoint
x-spans) — the classic approach, independently authored from the public
description of the technique, never transcribed from a GPL implementation
(see the licensing boundary above). A simple rect-list with split-on-insert
is an acceptable v1 given our rect counts are tiny; the band structure can
follow if profiling asks for it.

*Acceptance:* headless unit tests in `vg/test/test_region.ae`, wired into
ci.sh Phase 0. Cover: add overlapping rects → disjoint set with correct
total area; subtract a middle rect → four survivors; subtract a covering
rect → empty; translate then intersect; area conservation under
add/subtract sequences; degenerate and zero-size inputs.

### Stage 2 — dirty invalidation, single buffer

Give each frame (and later each element) a dirty region seeded from its
bounds. A change marks only that object's bounds dirty. The composite
becomes: union the dirty regions, set the backend clip to those rects,
replay the command buffer clipped, flush.

New backend surface, one call per backend:

```
canvas_set_clip_rects(canvas_id, rects, n)   // cairo_rectangle+cairo_clip,
canvas_reset_clip(canvas_id)                 // CGContextClipToRects, GDI
```

This still replays the whole buffer, but the backend rasterizes only inside
the clip — so an animating frame over a static background already costs its
own area rather than the whole canvas.

*Acceptance:* `frames_demo` gains a "dirty area" readback (sum of
`rgn_area` per frame). A spec asserts that moving one frame dirties an area
close to (old bounds ∪ new bounds) and NOT the whole canvas. Plus the
existing golden gallery must stay byte-identical: clipped redraw may not
change static output.

### Stage 3 — opaque subtraction + z-order *(the miracle)*

Each frame declares an opaque region (its chrome and any opaque content).
The composite gains the two passes that matter: propagate dirty up, then
**subtract each opaque frame's region from the dirty regions of everything
behind it**, then distribute what survives down, clipped per frame.

*Acceptance — the falsifiable claim.* In `frames_demo`, put frame A over
frame B covering a measured fraction *f* of B. Drive an animation frame and
read back the area B actually repainted. Assert it is ≈ (1 − *f*) of B's
area, within tolerance. On today's immediate mode that number is 1.0
regardless of *f*; after this stage it must track the exposed fraction.
That single assertion is the whole design's proof, and it is exactly the
kind of pixel-level claim this repo already gates on.

Instrumentation is already wired: `GET /canvas/{id}/debug` returns
`{area, commands, w, h}` (gtk4 real, 501 elsewhere) and `uidriver` exposes
`canvas_debug_area/commands/w/h`. The `AEUI_CANVAS_DEBUG` stderr taps report
the same per paint — they are what exposed vg.live's half-painting click
hook. What is missing is only the *meaning* of `area`: see Stage 0's note.
Stage 2 must make it the summed clip-rect area, at which point the
frames_demo spec's current `assert_eq(area, w * h)` will start failing —
that failure is the signal Stage 2 worked, and the assertion should then
flip to the occlusion form above.

### Stage 4 — per-element regions inside a scene

Push what stage 3 does for frames down to `AevgElement`: element bounds
(already stored) become per-element dirty/opaque regions, so a single
changed shape in a large static scene repaints only itself. This is what
turns `scene_refresh` from "rebuild everything" into true damage-driven
redraw — and would retire the whole-canvas rebuild that Maerkdown performs
per keystroke.

*Acceptance:* a spec that changes one shape's fill in a 500-shape scene and
asserts the repainted area is that shape's bounds, not the canvas.

### Stage 5 — live/foreign content as a composited node

A `video`/`game`/GL element is an opaque node with its own draw clock; the
compositor region-clips around it and never redraws it from the vector
pipeline. Until stage 3 lands, this is the *escape hatch* (a native
`GtkVideo`/`GtkGLArea` positioned by the vg layout, composited by the host)
— and notably the escape hatch is the *same architectural shape* as the
final design, so it is not throwaway.

## Risks and honest costs

- **Backend clip parity.** Three backends must agree on clipped output or
  the goldens diverge. cairo and CoreGraphics clip natively; GDI needs
  `SelectClipRgn` and has historically been the fussy one (see the win32
  `begin_path` and `CV_STROKE` scars). Budget win32 debugging.
- **Correctness beats speed.** Every stage must leave the golden gallery
  byte-identical. A compositor that is fast and wrong is worse than the
  immediate mode we have.
- **Opaque is a promise.** Declaring a region opaque when it is not
  produces stale pixels — the classic compositor bug. Translucent content
  (tumbling_cube's faces are 0.55 alpha!) must NOT be marked opaque; only
  frame chrome and explicitly opaque backgrounds qualify. Note the demo's
  own cubes are glass: the frames' *chrome* is what occludes, which is a
  useful honesty check on the implementation.
- **Scope discipline.** Stages 0–2 are worth doing on their own merits.
  Stage 3 is the expensive one. Do not start it without the stage-0 harness
  in place to prove it works.

## Relationship to the immediate-roadmap work

*Original argument (still on record):* everything on the near-term roadmap —
the `vg {}` drawing DSL, the SVG→AeVG transpiler, `render_to(png)`, the
librsvg parity gate over the 208-sample corpus — is **static**: rendered
once, to a window or a PNG. None of it animates. Deferred-flush
immediate-mode is correct and sufficient for all of it, and building the
compositor first would delay all of it to serve content
(video/game/animation) that is not yet a target. So: ship the static path on
immediate mode; treat the retained compositor as a named future track.

*Update — the demand signal has since arrived.* That argument was written
when the toolkit rendered only static content. It no longer does:

- **Live regions and animated scenes** shipped (`canvas_region`,
  `scene_set_refreshing`, the self-retiring 60fps loop).
- **Three continuously-animating 3D apps** exist (tumbling cube, Rubik's
  cube, Trajan's column), each repainting an entire canvas per frame.
- **Sketchpad** drives a 60fps loop for the duration of every pointer drag.
- **Maerkdown** rebuilds its whole page layout and command buffer on every
  keystroke — an O(document) cost for an O(one word) change, which is
  exactly the ceiling named in "Where we are today."
- The **read_pixel replay cache** (a per-canvas rendered-surface cache keyed
  on clear-generation) was added because a grid probe re-rasterized the whole
  buffer per pixel. That cache is a point fix for the same underlying
  problem this document solves generally.

None of these are broken — they are correct, and fast enough on a desktop.
But they are all paying O(entire scene) for O(one rect) changes, and the
list is growing rather than shrinking. The compositor is no longer
speculative infrastructure looking for a consumer; it has several, and
Stage 0 gives it a measurable one.

## The lineage (acknowledgement, not a dependency)

The bar this doc holds up is the one set by CPU compositors that achieved
glitch-free overlapping animated windows long before GPUs — the Amiga and
TaOS-era systems, and carried forward today by systems like ChrysaLisp. We
admire the result and aim for the same bar. We will reach it by independent
implementation of the public technique, consistent with aether-ui's MIT
license — *not* by porting any GPL implementation of it.
