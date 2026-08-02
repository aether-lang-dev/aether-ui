# Stage 2 rework: the paint clip must be owned by the painter

**For:** whoever picks up Stage 2 of `retained-compositor.md` next.
**Status of `067ed50` ("Add frame dirty-region paint clips"):** the region
maths and the backend clip plumbing are good; the *integration* is unsound on
an animated canvas and reproducibly flickers. This note says exactly why, with
evidence, and what the fix has to look like. Stage 1 (`da0f626`, the region
type) is independent and fine — keep it.

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

## Repro recipe

```sh
aeb apps/frames_demo
AEUI_CANVAS_DEBUG=1 ./target/build/apps/frames_demo/bin/frames_demo 2> /tmp/fd.log
# drag a frame by its title bar on a real display, then:
grep -oE 'area=[0-9]+' /tmp/fd.log | uniq -c
# interleaved 294000 / ~40000 lines == the race is present
```
