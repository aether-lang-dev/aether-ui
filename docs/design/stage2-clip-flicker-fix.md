# Stage 2 rework: the paint clip must be owned by the painter

**For:** whoever picks up Stage 2 of `retained-compositor.md` next.
**Status of `067ed50` ("Add frame dirty-region paint clips"):** the region
maths and the backend clip plumbing are good; the *integration* is unsound on
an animated canvas and reproducibly flickers. This note says exactly why, with
evidence, and what the fix has to look like. Stage 1 (`da0f626`, the region
type) is independent and fine — keep it.

**Superseded resolution:** `1a86edf` took the interim-safe live-scene guard,
but Round 2 and Round 3 below showed that was still not enough. Round 4
disabled gesture-side clips by default. Round 5 is the current resolution:
GTK's canvas painter owns a retained backing surface, clips the clear as well
as replay, and `frames_demo` submits dirty-region clips by default. Set
`AETHER_UI_FRAMES_DIRTY_CLIP=0` only for local full-paint comparison.

## The symptom

Run `apps/frames_demo` on a real desktop (not Xvfb, not `AETHER_UI_NO_ANIMATION`)
and move the mouse over the canvas, or drag a frame by its title bar. Large
parts of the canvas flash white — both during pointer motion and during drags.

## The mechanism

`canvas_clear` empties the **command buffer**; it does not erase pixels.
Pixels are only replaced when the buffer is replayed into the widget. In
immediate mode that is safe, because every paint replays a complete scene over
the whole allocation.

A *clipped* paint replays the whole command buffer through a small clip, so
only pixels inside the clip get written. Everything outside keeps whatever was
in the widget's backing store from the previous paint — and nothing guarantees
that content survives. That is the white.

This only bites because **two independent painters share one surface**:

1. `vg/live.ae`'s 60fps loop — `canvas_clear` → `_flush_live` → `canvas_redraw`,
   which expects to repaint the entire canvas every frame (the cubes animate);
2. the Stage 2 gesture path — `frames_*` submits a dirty clip, which is applied
   to *whichever paint happens next* and then consumed
   (`cs->paint_clip_count = 0` in `canvas_draw_func`).

Neither knows about the other. `vg/live.ae` contains no reference to the new
paint-clip mechanism at all (`grep -c paint_clip vg/live.ae` → 0).

## The evidence

Instrumented drag (`AEUI_CANVAS_DEBUG=1`, synthesized press/move×4/release on
the Alpha title bar), counting `area=` per paint:

```
    168 area=294000     <- full-canvas paints (the animation loop)
      1 area=36800      <- clipped paint (a drag step)
      1 area=44200      <- clipped
      3 area=40000      <- clipped
     64 area=294000     <- full-canvas again
```

Clipped paints are *interleaved* with full-canvas ones at 60Hz. A clipped
paint that lands after a clear renders one strip and leaves the rest of the
canvas stale.

## Why the spec did not catch it

`tests/frames_demo/spec_frames_demo.ae` asserts only the **area number**:

```
aeocha.assert_gt(area, 50000, "dirty clip covers old and new frame bounds")
aeocha.assert_true(area < 53000, "dirty clip is much smaller than the canvas")
```

Both are true while the rendered image is wrong. Worse, the suite runs with
`AETHER_UI_NO_ANIMATION=1` "to keep tests deterministic" — which switches off
the very painter the clip races against. The bug is invisible by construction.

This is the same trap the doc already warns about from the other direction:
an assertion that cannot fail proves nothing. `area` dropping is necessary but
nowhere near sufficient.

## What the fix has to satisfy

**Invariant: a clip is only valid if the painter redraws everything visible
inside it.** With a live region present, that means the frame content *and*
the background beneath it, in one pass, in one paint.

Concretely, one of these — in order of preference:

1. **Let the painter own the clip.** The dirty region accumulates in the frame
   host; `vg/live.ae`'s present/loop path consumes it and applies it around its
   own `canvas_clear` + `_flush_live` + `canvas_redraw` cycle. Exactly one
   painter decides the clip for a given frame, so there is no interleaving.
   This is also the shape Stage 3 needs, since occlusion subtraction has to
   happen where the composite happens.

2. **Clip only static scenes.** If a scene has any live region or is
   `scene_is_refreshing`, skip clipping entirely and take the full-canvas cost.
   Correct, trivially safe, and still delivers Stage 2's benefit for the static
   case. A reasonable interim if (1) is too large a change to land at once.

Do **not** keep the current shape and try to paper over it by shrinking the
window of the race — the two painters are genuinely independent and the
interleaving is not a timing accident.

## Acceptance — what must be true before this is called done

- **A pixel assertion, not an area assertion.** With animation ON, drag a
  frame and then verify canvas content is correct: probe a point on the
  background well outside the dirty rect and assert it still holds the
  background colour (`#dfe5ec`, drawn at `frames_demo.ae:250`) rather than
  white. Today that probe fails.

  Note there is **no HTTP pixel route** — `canvas_read_pixel` is an app-side
  call. Follow the established pattern: add a "Probe" button and a bound
  label that reports the sampled colour (maerkdown's `probe()` at
  `apps/maerkdown/maerkdown.ae:469` and sketchpad's are the models), then
  assert on the label text from the spec. Classify by colour, and remember
  the standing rule from those apps: a probe needs a floor AND an anti-flood
  bound, and must classify against something separable from every backdrop
  (win32's replay has a white ground and no alpha).
- **Keep the area assertion too** — it is the right cost signal, it just is not
  a correctness signal.
- **A spec that runs with animation ON.** If determinism is the reason for
  `AETHER_UI_NO_ANIMATION`, add a separate case that enables animation and
  asserts only things that are stable under it (background colour outside the
  dirty region is stable; exact cube pixels are not).
- **Verify by hand on a real desktop**, not just Xvfb. This class of bug has
  form here: the vg.live half-painting click hook (`ba136ed`) was invisible to
  every synthesized spec because driver clicks invoke one closure while a real
  pointer fires every attached controller. Same lesson, same place.
- **The golden gallery must stay byte-identical**, per the doc's standing rule.

## Round 2 (`1a86edf`) — the guard is right, the test is not

`1a86edf` took option 2: skip gesture clips while `scene_is_refreshing`.
**That part is correct and verified here:**

- animating scene, synthesized drag → 238 paints, ALL `area=294000`. No
  interleaving; the race is gone.
- static scene (`AETHER_UI_NO_ANIMATION=1`) → `36800 / 44200 / 40000`.
  Stage 2's benefit is preserved where it is safe.

**But the new pixel assertion does not test anything.** Falsification: revert
the guard to a condition that never fires (`== 2`), rebuild, run the suite —
**7 passing, still green.** The spec cannot detect the bug it was written for.
Two independent reasons, both must be fixed:

1. **`probe_background` repairs the damage before sampling it.** Its first
   line is `scene_refresh(app.scene)` — a full, unclipped repaint of the whole
   canvas. Whatever was stale is corrected, *then* the pixel is read. It can
   only ever report `bg=1 white=0`. Drop that refresh; the probe must sample
   whatever is on screen at the moment it is asked.

2. **`canvas_read_pixel` cannot see the flicker at all.** It creates a fresh
   cairo surface and calls `canvas_replay` into it — it reads the *command
   buffer model*, never the widget's backing store. The stale pixels live in
   the backing store. A model-replay probe is structurally incapable of
   observing a backing-store artifact, on any backend.

   So the pixel probe needs a different instrument: sample the rendered
   widget (GTK: `gtk_widget_snapshot`/`gdk_texture_download` of the drawing
   area, or the existing `/screenshot` path where it works), or verify the
   damage invariant a level up — e.g. assert that a clipped paint is never
   issued while the scene is refreshing, by reading back the count of
   clipped-vs-full paints from `/canvas/{id}/debug`. The latter is cheaper
   and tests the actual invariant the guard enforces.

3. **`AETHER_UI_NO_ANIMATION=1` is exported globally** at
   `tests/spec_matrix.sh:35`, for every suite. Clicking an "Animate" button
   sets `scene_set_refreshing(scene, 1)` but the 60fps timer never installs,
   so the "animation ON" case does not animate under the matrix. Any spec
   meant to exercise the live painter has to opt out of that export for its
   own run.

Note that running the spec by hand with animation genuinely on also made
`title-bar drag moves the raised frame` fail — real animation introduces
nondeterminism the existing cases were not written for. That is worth knowing
before wiring an animated suite into the matrix.

## Round 3 — the remaining bug is not a race, it is the damage model

Reported after `1a86edf`: pointer-move flicker is gone, but **dragging a frame
blanks the other frame** for the duration of the drag — everything except a
~10px border around the dragged frame goes white. Reproduced and photographed
here on a DEFAULT launch (no Animate clicked).

`AEUI_CANVAS_DEBUG` during that drag, non-animating scene:

```
      1 area=36800
      4 area=294000
      1 area=47600
      1 area=294000
      2 area=43200
     57 area=294000
```

Clips are active, because `scene_is_refreshing` is FALSE by default — the
Animate button is opt-in. So the round-2 guard does not cover the path a user
actually exercises. But the deeper problem is not the interleaving:

**`frame_move` marks only the moved frame's old and new bounds dirty**
(`ui/frames.ae`, `_mark_frame_dirty` before and after the position change).
That is correct damage bookkeeping *for a compositor that redraws per frame
object*. It is fatal for the paint model we actually have, where a clipped
paint does `canvas_clear` and then replays THE WHOLE SCENE through the clip:

- clear discards the command buffer,
- the replay is clipped to Alpha's old ∪ new bounds,
- Beta and the page background lie outside that clip and are never drawn,
- the ~10px surviving border is where Alpha's old/new bounds overlap Beta.

This is the same invariant from the top of this note, stated the other way
round: *a clip is only valid if the painter redraws everything visible inside
it* — and equally, **everything visible outside the clip must already be
correct on the surface.** With a full-scene clear before every replay, nothing
outside the clip is ever correct. Damage-based clipping cannot work at all
until the painter stops clearing what it is not going to repaint.

Hence the ordering matters more than round 2 suggested: **Stage 2 cannot be
completed as a gesture-side concern.** Either

- the painter clears only the dirty region (clip the CLEAR as well as the
  replay, so untouched pixels genuinely survive), or
- the scene is composited from retained per-frame content that can be
  re-drawn selectively — which is Stage 3/4 territory.

Until one of those exists, the honest position is **clipping off by default**:
correct beats fast, per the doc's standing rule. `frames_demo` should ship
with clips disabled unless an explicit opt-in flag is set for measurement.

Recommend reverting the clip application in `frames_demo` (keep `ui/frames.ae`
damage tracking and the Stage 1 region type — both are wanted) and folding the
clip into the painter as part of Stage 3.

## Round 4 — current repo state

`frames_demo` implemented the Round 3 recommendation:

- `FrameHost` still tracks dirty regions and exposes
  `frames_dirty_nrects/rect/area/clear`.
- `frames_demo` consumes and clears those dirty regions on gestures, but only
  calls `canvas_set_clip_rects` when `AETHER_UI_FRAMES_DIRTY_CLIP=1`.
- Even with that opt-in, clips are skipped while `scene_is_refreshing(scene)`
  is true.
- The driver spec no longer uses the invalid app-side pixel probe. It asserts
  that default drags repaint the full canvas, preserving correctness until
  Stage 3 owns the clip in the painter/compositor.

## Round 5 — Stage 2.5 resolution

The GTK painter now owns damage for the canvas path:

- `canvas_clear` remains a model operation that frees the command buffer, but
  it also marks the retained paint surface dirty.
- Command appends mark the retained paint surface dirty.
- The next real paint updates the retained cairo image surface. If a dirty
  clip is pending, the painter applies that clip to both the clear and the
  replay, so pixels outside the clip survive from the previous correct
  surface.
- Paints with no dirty model state, including driver snapshots, present the
  retained surface without replaying the model and therefore do not repair the
  probe.
- `/canvas/{id}/debug` reports cumulative full/clip paint counts and the last
  clipped paint area, so tests can assert that a live drag actually hit the
  clipped painter.

`tests/frames_demo/test_stage25_clip_surface.sh` is the Stage 2.5 regression.
It runs with animation enabled, drives a title-bar drag, requires a clipped
paint below the full canvas area, then samples `/screenshot` to confirm the
far frame interior is still rendered. The first version was run red against
the unfixed dirty-clip path with:

```
FAIL: expected clipped drag paint below full canvas, got area=294000 full=294000
```

After the painter-owned surface change, the same scenario reports a clipped
area and a retained far-frame pixel.

## Repro recipe

```sh
aeb apps/frames_demo
AEUI_CANVAS_DEBUG=1 ./target/build/apps/frames_demo/bin/frames_demo 2> /tmp/fd.log
# drag a frame by its title bar on a real display, then:
grep -oE 'area=[0-9]+' /tmp/fd.log | uniq -c
# interleaved 294000 / ~40000 lines == the race is present
```
