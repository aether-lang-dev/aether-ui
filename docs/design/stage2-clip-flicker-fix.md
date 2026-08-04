# The Stage 2 clip saga — nine rounds, and what actually broke

**What this is.** The full diagnostic record of two overlapping bugs in the
retained-compositor track: a clip/paint race in `frames_demo` (Rounds 1–5b),
and win32 canvases rendering nothing (Rounds 6–9). It is kept in full,
including the rounds that were WRONG, because how three confident
misdiagnoses were reached is the most reusable thing in it.

Rounds are chronological. Rounds 7 and 8 are superseded — read them for the
reasoning, not the conclusions.

## Current state (read this first)

**The win32 blank-canvas bug is fixed, and it was never in the win32
backend.** The toolchain on that box had been built without pcre2.
`vg/svg/normalizer.ae:172` parses every SVG path `d` string through
`std.regex`, so `regex.compile` failed at runtime, `parse_path` returned zero
commands, and **every `path()` in vg silently became a no-op** while rects
and lines still drew. The signature is distinctive: windows open, gestures
work, counters increment, and the canvas is empty. Rebuilding the toolchain
with pcre2 fixed it, taking that box from 243/23 (8 red) to 255/13 (2 red).
See Round 9.

**Stage 2 clipping is GTK4-only, by design.** Only GTK4 retains a paint
surface between paints (Stage 2.5), so `frames_demo` gates dirty clips on
`backend_name() == "gtk4"`; win32 and macOS rebuild their surface per paint
and would leave stale pixels outside the clip. Porting the retained surface
to those two is **Stage 2.5b** in `retained-compositor.md`, not yet done.

**Four-platform matrix, 2026-08-04:**

| Platform | Result |
| --- | --- |
| Linux / GTK4 | 260 / 0 — all green |
| macOS / AppKit | 255 / 0 (LisMusic binary absent — sqlite provisioning) |
| FreeBSD / GTK4 | 255 / 0 (LisMusic binary absent — same) |
| Windows / win32 | 259 / 2 — `gp_fileops` trash, diagnosis incomplete |

**Still outstanding, roughly in priority order:**

1. **Stage 2.5b** — retained paint surface on win32 and macOS, so clipping
   can be enabled beyond GTK4. The GTK4 mechanism is ~24 lines (a surface
   allocator keyed on size, a clip-clear helper, one branch in the paint
   function); both targets already have the primitive they need
   (`CreateCompatibleBitmap` on win32, `CGBitmapContextCreate` on macOS).
2. **`gp_fileops` on Windows** — Delete-to-trash silently fails. PowerShell
   rejects MSYS paths (`Could not find file '/c/Users/...'`) and accepts
   native ones; a converter landed in `26280d3` but did **not** fix the
   test, so the path reaching that code is not what was assumed.
3. **aeb fan-out on Windows** — every target fails with `aetherc` invoked
   with no arguments, while individual builds via the per-target
   orchestrator work. Filed upstream.
4. **aeb on FreeBSD** — `aeb-link` receives the install prefix where it
   documents the bin directory, and resolves helper tools relative to
   `$PWD`. Filed as
   `aeb/asks/aeb-link-wrong-aether-dir-and-relative-tools.md`.

## Round 1 — the paint clip races the animation loop

*The original report. `067ed50` ("Add frame dirty-region paint clips") landed the region
maths and the backend clip ABI correctly; the INTEGRATION was unsound on an animated
canvas and visibly flickered. Stage 1 (`da0f626`, the region type) is independent and
fine — keep it.*

### The symptom


Run `apps/frames_demo` on a real desktop (not Xvfb, not `AETHER_UI_NO_ANIMATION`)
and move the mouse over the canvas, or drag a frame by its title bar. Large
parts of the canvas flash white — both during pointer motion and during drags.

### The mechanism


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

### The evidence


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

### Why the spec did not catch it


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

### What the fix has to satisfy


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

### Acceptance — what must be true before this is called done


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

## Round 5b — Stage 2.5 is GTK-only, and the other two backends are unsafe


`d4ca538` gives GTK a retained `cairo_surface_t` and makes a clipped paint
clear *within the clip only*, so pixels outside survive. Verified here on a
real display: dragging Alpha over Beta keeps Beta's interior intact, with
clips genuinely active (36800/49600/43200 among full paints). Good fix.

But `frames_demo` now enables clips **by default**, and the other two
backends have the clip ABI with **no retained surface** (`grep -c
paint_surface`: gtk4 39, macos 0, win32 0). Both are structurally the Round 3
bug:

- **win32** (`canvas_paint`, ~5194): `CreateCompatibleDC` +
  `CreateCompatibleBitmap` build a **fresh** bitmap every paint, the clip is
  applied to it, the buffer is replayed, and then the WHOLE bitmap is
  `BitBlt`-ed to the window. A fresh compatible bitmap is uninitialised — so
  everything outside the clip is blitted as garbage over good pixels. There
  is no previous content to preserve because the bitmap is new each time.
- **macOS** (`drawRect:`, ~3826): clips and replays straight into the
  `NSGraphicsContext` CG context. AppKit does not guarantee backing-store
  content outside the dirty rect, so this is unsafe by contract even where it
  happens to look right.

Empirically macOS *did* render correctly in a driver-driven drag (screenshot
clean, Beta intact) — but that is AppKit choosing to preserve the backing
store, not a guarantee we hold. Windows could not be tested: aeb's own
tooling is broken there independently of this work (`transform-ae` invocation
fails with `C:/Users/paul/aether-ui: Is a directory`), so `frames_demo` does
not currently build on winbaz at all.

**Ask:** either give macOS and win32 the same retained-surface treatment, or
gate the clipped path on a backend capability so it stays off where the
painter cannot own damage. Shipping `AETHER_UI_FRAMES_DIRTY_CLIP` on by
default while two of three backends cannot honour the invariant is exactly
the "fast and wrong" the design doc rules out. The win32 case needs no
testing to condemn — a fresh `CreateCompatibleBitmap` per paint cannot
preserve anything by construction.

## Round 6 — the session-0 theory *(CORRECT — wrongly discarded at the time; see Round 9)*


Wiring `canvas_debug` on win32 (`1c7a792`) was supposed to give Windows a
pixel-free way to check the Stage 2.5 invariant. It does not work, and the
reason is worth writing down because it also explains the blank screenshot.

`GET /canvas/{id}/debug` answers — so the hook is wired, not 501 — but with
`{"area":0,"commands":0,"w":0,"h":0}`, before AND after a drag. The metrics
are recorded inside `canvas_paint`, and `canvas_paint` is reachable from
exactly one place: `case WM_PAINT` in the canvas window proc (~5351).

Under the driver, these apps run **headless in a Windows service session**
with no mapped, visible window — so `WM_PAINT` never fires. Nothing paints,
so there are no paint metrics to report, and equally nothing for
`PrintWindow`/`WM_PRINTCLIENT` to capture. **The blank `/screenshot` and the
zeroed `/canvas/{id}/debug` are the same bug**, not two.

Consequences:

- Windows cannot verify Stage 2.5 (or Stage 3) by either instrument until
  something drives painting headlessly. The `frames` suite's four failures
  on Windows are this, not a compositor regression: every metric assertion
  reads 0.
- The fix is to make canvas painting reachable without `WM_PAINT` — e.g. an
  explicit "render to DC" entry point the driver can call (win32 already has
  `canvas_replay_to_dc`, used by `canvas_read_pixel`), with the metrics
  recorded there too. That single change would light up both the screenshot
  and the debug counters.
- Until then, treat win32's Stage 2.5 status as **unverified by execution**;
  the source-level argument in Round 5 (fresh `CreateCompatibleBitmap` per
  paint cannot preserve pixels outside a clip) still stands and is why
  `frames_demo` gates clips off on non-GTK backends.

## Round 7 — SUPERSEDED by Round 9 *(claimed `canvas_paint` never runs)*


Round 6 guessed that a locked session was the cause. **Wrong.** Paul
unlocked the desktop and the metrics still read
`{"area":0,"commands":0,"w":0,"h":0}`. Two further guesses also died:
`AETHER_UI_HEADLESS=1` (exported for Windows/macOS at
`tests/spec_matrix.sh:33`, which hides the window via `SW_HIDE`) is not it
either — running explicitly non-headless, with a visible window on an
unlocked desktop, still reports zeros.

Settled by measurement instead: a `fprintf` tap at the top of
`canvas_paint` (compiled in — verified `grep -c` on the source and a binary
newer than it) fired **zero times** across app start, a click and a drag.
The hook resolves the right canvas (`/canvas/1/debug` answers,
`/canvas/2/debug` correctly 404s) and the metrics assignment is present at
`aether_ui_win32.c:5232`. `canvas_paint` is simply never called.

So the win32 canvas window is not receiving `WM_PAINT` — not because the
session is locked, not because the window is hidden, but for some reason
still to be found in the canvas HWND's creation/parenting or message
routing. That single fact explains ALL of it: the blank `/screenshot`, the
zeroed debug counters, and the four `frames` failures on Windows, which are
therefore NOT a compositor regression.

Next diagnostic step (not yet done): confirm the canvas HWND exists and is
visible (`IsWindow`/`IsWindowVisible`/`GetClientRect` from the driver
thread), and check whether it is parented under the off-screen
`widget_holder` that `hook_screenshot_png`'s comments already describe as
"never composited". If the canvas lives there, nothing will ever paint it
and the fix is a driver-callable render-to-DC entry point
(`canvas_replay_to_dc` already exists for `canvas_read_pixel`) that records
metrics too.

## Round 8 — SUPERSEDED by Round 9 *(claimed win32 on-screen paint was never wired)*


Paul launched `frames_demo` and `tumbling_cube` from Windows Search (no
scripts, no driver harness, unlocked desktop). Result: **windows open and
move, drag counters increment, and the canvas is empty** — no cubes, no
frame content. Every non-painting path works; every painting path does not.

That prompted a bisect. It is not needed, and the reason is more useful
than a commit hash.

**The apparent contradiction.** `1a1b1b2` (Aug 1) fixed a win32 canvas bug
and its message quotes exact read-back pixels — "dark 15,15,22 / red
229,76,51 / blue 51,102,229". So canvases demonstrably rendered on Windows
two days ago, which seems to refute "win32 never paints".

**The resolution.** There are TWO independent paths:

- `canvas_read_pixel` → builds its own memory DC → `canvas_replay_to_dc`
  (5288). **Never calls `canvas_paint`.**
- on-screen painting → `WM_PAINT` → `canvas_paint` → `canvas_replay_to_dc`
  (5249).

Every win32 pixel proof this repo has ever recorded — the `begin_path`
frame-wipe fix, the `CV_STROKE` pen fix, the goldens, every spec probe —
went through the FIRST path. They tested the command buffer and the replay,
both of which are fine. **No test has ever asserted that a win32 canvas
reaches the screen**, and the human check that would have caught it (look
at the window) had never been done until now.

So: nothing regressed, there is no bad commit, and the four `frames`
failures plus the `sketchpad`/`maerkdown`/`golden` reds are not
compositor damage. On-screen canvas painting on win32 is an
**unimplemented-and-untested capability**, not a broken one — hidden for
months because the replay path it shares is genuinely correct, and because
the apps that pass on Windows are built from native widgets.

Corollary for the compositor track: Stage 2.5b (retained surface on win32)
is pointless until `canvas_paint` runs at all. And the missing test is not
a pixel probe — those pass — but an assertion that `canvas_paint` executes,
which the `canvas_debug` counters (`1c7a792`) can now express.

## Round 9 — it was never win32. It was pcre2. *(the winbaz agent, 2026-08-04)*


Diagnosed on the machine, by an agent that could see the screen. Three
"established facts" I handed it in the brief were wrong, and it produced
measurements for each rather than arguing.

**Round 7's "canvas_paint runs zero times" was a measurement artifact.**
Launches from the SSH/MSYS context land in Windows **session 0**, window
station `Service-0x0-...`. A 60-line vanilla Win32 program — one window,
one child, none of our code — also gets zero `WM_PAINT` there, with
`IsWindowVisible` returning 0 despite `WS_VISIBLE`. Reached properly on the
real desktop (session 1, WinSta0) via an interactive scheduled task,
`canvas_paint` runs **565–777 times in a few seconds**. Round 6 had already
identified session-0; I discarded it as "the earlier (wrong) locked-session
theory" and then took Round 7's number *inside the very session Round 6
named*. That is the methodological failure worth remembering: I replaced a
correct diagnosis with a measurement taken under the condition it warned
about.

**The parenting theory was also wrong.** The first diagnostic came back
clean: `AetherUICanvas → AetherUIStack → AetherUIStack → AetherUIAppWindow`,
all visible, client 470x446. Never under `widget_holder`.

**The actual cause.** The command buffer held exactly **3** commands —
CLEAR, the zero-size anchor rect, and the backdrop. `pp_to_path` emitted
perfect `d` strings, but `normalizer.parse_path` returned zero commands,
because `regex.compile` fails at runtime with *"regex: built without
libpcre2-8"*. The Aether toolchain on that box was built without pcre2, so
**every `path()` in vg silently rendered nothing** while `rect`/line
drawing still worked. Hence "black canvas with no cube" and "frames with no
content": frame chrome is rects, the cubes inside are paths.
`vg/svg/normalizer.ae:172` is the call.

The fix was to the machine, not the repo: rebuild 0.478.0 with pcre2. Three
traps, each costing real time — `make` alone reuses `.o` files and ignores
changed CFLAGS (needs `make clean`); `pkg-config --libs libpcre2-8` yields
the import lib, so apps die silently when double-clicked (link
`-l:libpcre2-8.a`); and stale extensionless `ae`/`aetherc` in
`/usr/local/bin` shadow the new `.exe`s.

**Result:** 243 pass / 23 fail / 8 red → **255 pass / 13 fail / 2 red**.
`vg_tooltip`, `stroker`, `vg3d`, `golden`, `sketchpad`, `tumbling_cube` all
went green. The cube visibly tumbles; `frames_demo` shows both cubes with
Beta occluding Alpha.

**The new test** (`tests/tumbling_cube/spec_tumbling_cube.ae`, +40) asserts
the *painter*, not pixels: `GET /screenshot` (which drives a real
`canvas_paint` even under `AETHER_UI_HEADLESS=1`), then `w`, `h`,
`area == w*h`, and **`commands > 100`**. Note my brief suggested
`commands > 0` — **that would have passed against the bug**, which emitted
3. The floor is the whole point.

Falsified before landing, same spec against two binaries:

```
FAIL: the replayed scene carries real path geometry, not just a backdrop
      — expected > 100, got 3
tumbling_cube       3      3   RED      <- pre-fix binary
tumbling_cube       6      0   green    <- rebuilt binary
```

### Two loose ends, diagnosed and deliberately untouched

- **maerkdown** regressed 9/1 → 7/3, and honestly so: the H1 word's left hit
  edge moved from ≤24 to ~31 now that glyph outlines actually render, so the
  spec's hard-coded `/canvas/1/click?x=30&y=30` lands ~1px outside. `x=40` is
  solidly inside. The old pass was a false green against a canvas where
  outlines never rendered. Left alone because the spec calls that coordinate
  a cross-backend contract, and GTK4/macOS can't be tested from that box.
- **frames** stays 4/4 red, and it is *not* the compositor: as the spec
  reads it, `{"area":0,...}`; after a `GET /screenshot`, `{"area":299600,
  "commands":305,"w":700,"h":428}`. Two assertions just need a driven paint;
  the other two assert a *clipped* paint, which win32 gates off by design
  (Round 5b), so they need a backend-aware expectation.

### The durable question, raised by that agent and worth answering upstream

A missing optional dependency turned every vector path into a silent no-op
and cost eight rounds pointed at the wrong file. Should `vg` **fail loudly**
when `regex.compile` errors, or drop the dependency for a hand-rolled path
tokenizer? That belongs in aether/vg, not the win32 backend. Note this is
the *second* time pcre2 has done this to us — it killed the SVG d-parser on
macOS in an earlier session too.

## Repro recipe


```sh
aeb apps/frames_demo
AEUI_CANVAS_DEBUG=1 ./target/build/apps/frames_demo/bin/frames_demo 2> /tmp/fd.log
# drag a frame by its title bar on a real display, then:
grep -oE 'area=[0-9]+' /tmp/fd.log | uniq -c
# interleaved 294000 / ~40000 lines == the race is present
```

## The durable lessons

Written down because each cost days, and each will recur.

**A test that cannot fail is worse than no test.** Three separate regression
tests in this saga shipped GREEN against the very bug they were written to
catch: an area assertion that only checked a number had dropped while the
image was wrong; a "clips disabled while refreshing" guard exercised on a
path that was never refreshing; and a pixel probe that called
`scene_refresh()` before sampling, repairing the damage it was meant to
detect. **Falsify every new test against the unfixed code before accepting
the fix.** If it does not go red, it is not a test.

**A measurement taken under a condition an earlier round warned about is not
evidence.** Round 6 identified Windows session 0 as a place where no window
receives `WM_PAINT` at all. Round 7 discarded that as "the earlier (wrong)
locked-session theory", then took its headline measurement — "`canvas_paint`
runs zero times" — *inside session 0*. The number was real and the
conclusion was worthless.

**Silent degradation of an optional dependency is worse than a hard
failure.** `std.regex` without pcre2 returns an error string that every
caller is free to ignore. Three layers up, `vg` drew nothing and reported
success. The failure presented as a *rendering* bug, so eight rounds went to
the graphics backend. A loud failure at `regex.compile` would have cost
minutes. Raised upstream on aether#1389.

**Choose an instrument that can observe the thing.** `canvas_read_pixel`
builds its own memory DC and replays the command buffer into it — it never
touches the painter. Every win32 pixel proof this repo had ever recorded
went through that path and passed legitimately while the screen stayed
black. Nothing asserted that a canvas reaches the screen until the
`GET /canvas/{id}/debug` counters and the `tumbling_cube` painter test.

**A signal must exceed the noise floor.** "Bold eats dark ground" moved ~30
samples out of ~14,200 on GTK4 (0.2%), and the identical edit moved 10 the
*other* way on win32 — antialiasing jitter around glyph edges, not the
stroker's outline. An inequality on that passes or fails by luck. Removed;
the stroker is properly proven in `tests/stroker_demo` by pixel-equivalence
against native stroking.

**Ask the human to look.** The deadlock broke when Paul launched the app
himself and reported "two windows open and are moveable but no cubes …
attempt to drag shows the counter increment". That split — model alive,
paint dead — is what no amount of remote probing had produced, and it
pointed straight at the real layer.
