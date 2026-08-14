# aether-ui — open follow-ons

Tracked items not yet built. Backends are at full spec-matrix parity
(GTK4 188/0, win32 188/0, macOS per its sibling cadence); these are the
next layers. Fuller context: the toolkit-inspired backlog in roadmap.md.

## Golden-image tests (Flutter-style visual regression) — DONE 2026-08-01

Shipped as examples/golden_gallery + tests/goldens/<backend>/*.sig (67d3cb6):
per-cell signature files rather than PNG diffing (text, git-diffable, no
image decoder needed on any platform), captured through each backend's
canvas replay via ui.canvas_read_pixel, driven by the AetherUIDriver spec
`golden` in every matrix run. Blessed and green on all four platforms —
FreeBSD passes against Linux's shared gtk4 references. PNGs still land in
target/golden_out/ for human eyes. Original sketch kept below.

### The original sketch

Screenshot-based regression over the existing driver `/screenshot` route:
per-suite golden PNGs, checked in per platform (`tests/goldens/gtk4/…`,
`tests/goldens/win32/…`), compared with an MAE tolerance gate — the
librsvg-parity philosophy (vg/test/svg-compare-aevg.py) applied to
widgets. Motivation: the win32 h:0 era — every widget rendered 0-tall for
weeks while click-driven specs stayed green; only a pixel gate catches
silent visual breakage. Sketch:
- `tests/golden_check.sh <suite>`: launch app (fixed size via
  /window/resize), GET /screenshot, compare vs golden (MAE < ~3 good,
  regenerate with `--bless`).
- Start with a handful of stable suites (calculator, themes_demo skins,
  splitview); grow as fonts/AA differences per box are understood (may
  need per-host goldens or a tolerance bump — be honest about flake).

## Widget Inspector — DONE 2026-08-14 (apps/inspector)

Shipped as `apps/inspector`, an aether-ui app that browses any other running
app's widget tree over the AetherUIDriver. No new backend surface, exactly as
the brief predicted: the protocol was already sufficient.

    inspector [port]        # default 9222

Verified end-to-end against a live `listbox_demo`: Refresh loads its 13
widgets, indented by parent depth with type/id/text/size per row, and
selecting a row fills the detail pane (id 9, type button, text "Add",
parent 8). Driven through the inspector's OWN driver on 9444 -- it serves
the same protocol it consumes, so its spec can click its own buttons.

Read-only by choice: nothing mutates the target, so it is safe to point at
something mid-debug. The brief's optional "flash the selected widget" was
dropped for that reason.

Two things worth keeping:
* `enable_test_server` is a BUILDER call and needs the live `_ctx` -- called
  from `main()` it silently no-ops, which cost a debugging cycle.
* `visible`/`enabled`/`sealed` are JSON booleans and need
  **`json.json_get_bool`**. `json_get_int` returns 0 for BOTH `true` and
  `false` without reporting an error, so reading them as ints shows a
  confident, uniform "0" for every widget -- in an inspector that is worse
  than an obvious gap. (I first concluded std.json had no boolean support at
  all and nearly filed an upstream issue; I had probed for `json_is_bool` /
  `json_is_true` / `json_type_of`, which do not exist, and stopped there.
  `json_get_bool`, `json_is_null`, `json_type` and `json_get_number` all do.
  Probe for the feature, not for a guessed name.)

### The original sketch

A live widget-tree browser over the AetherUIDriver — the protocol already
exists; the inspector is just a client (and can itself be an aether-ui
app: dogfood). Connect to any running app's port and:
- browse the tree (GET /widgets: type/text/geometry/classes/fg/bg/
  fontFamily/role/a11y_name, windows, overlays);
- click a node → flash the live widget (style_bg_color pulse or a
  temporary class via the existing routes);
- inspect panel: raw widget JSON + a11y (GET /widget/{id}/a11y) +
  live re-poll;
- bonus: /window/pick under a crosshair mode ("what widget is here?").
No new backend surface required for v1; anything missing (e.g. a
subscribe/poll diff) becomes a driver follow-up.

## Video / live external content in a frame

`apps/video_frame` (b5d765d) proves the mechanism end-to-end: an MP4 plays
inside a `ui.frames` internal frame with other widgets around it, at 15fps
320x240 (clock-accurate against the source) and 28fps / 33 MB/s at 640x480.
The toolkit needed no changes — `LIVE_RASTER` regions were designed for
exactly this, and everything below `region_push` is the existing path.
Stage 5 shows plainly in the counters: `rerender=1 blit=1`, the video frame
re-rendering while the static frame beside it is served from cache.

What ships today is a **proof, not a video player**. Everything in that app
is up for rewrite. The long-term aims, roughly in dependency order:

### 1. Real decode, in-process
Today ffmpeg is spawned to transcode the whole clip to a raw-RGBA file,
which the app `fs.pread`s frames out of. That costs a full intermediate:
27.6 MB for 6s of 320x240, ~1.5 GB per minute of 1080p. Worse, a **live**
source (camera, network stream, screen capture) has no file to pread and
no workaround at all.

Wants a libavcodec contrib shim in the aether tree — the same shape as
`aether_sqlite` (`make contrib` → `libaether_avcodec.a` → `link_flags`).
Then decode happens in-process and the intermediate disappears.

### 2. Streaming input (blocked upstream)
Even with ffmpeg as an external process, the natural shape is a pipe:
`ffmpeg -i clip.mp4 -f rawvideo -pix_fmt rgba -` read frame by frame. Not
possible today: `std.os.run_pipe` returns a `parent_read_fd` that Aether
cannot read (`std.net` has `fd_write`/`fd_close` and no `fd_read`;
`std.ipc` is write-only from the child; `run_pipe_drain_and_wait` drains
in C and yields bytes only at exit). Filed upstream — see the
`net.fd_read` / `fd_read_into` ask. Unblocks live sources without a
decoder binding.

### 3. Zero-copy to the region
`bytes.to_string` + `fs.pread_into` already reuse one frame buffer, so
there is no per-frame allocation. The remaining copy is `region_push`
retaining the string. At 1080p a frame is 8 MB; at 30fps that is 250 MB/s
of copying. Wants a decoder writing **directly into the region's buffer** —
`region_frame_buffer(rgn) -> ptr` plus a "contents changed" nudge, so the
pixels land where the blit reads them.

### 4. Audio, and A/V sync — DONE 2026-08-10 (86a95cf + edcd6a2 + the load_pcm round)
Shipped in three steps: PTS presentation (edcd6a2 — a 15fps clip stopped
playing at 41fps), the audio device's clock as master (86a95cf — video
chases audio.position_ms, ~25ms on synthetic, 20-25ms on a real 720p/5.1
clip), and in-clip audio via avcodec.audio_pcm -> std.audio load_pcm
(0.512.0), which removed the sidecar-WAV extraction. One file in, both
streams out, no intermediates anywhere. Residue: audio is whole-buffer
(~20 MB in memory for 2 min), so a feature film wants load_pcm's
streaming-push sibling when it exists.

### 5. Faster than the compositor
28fps at 640x480 was the compositor's paint rate, not the region path.
A 60fps video in a frame needs the paint loop to keep up — worth measuring
where that ceiling actually is before optimising, and worth checking
whether a video region can present on its own cadence independently of the
scene's repaint (Stage 5 for regions, in effect).

### 6. Windows
The region blit path works on win32, but Stage 3 caching is gated off
there (GDI has no alpha — see `backend/aether_ui_win32.c`). A video frame
will still play; its *neighbours* just will not be cached. Worth a
measurement rather than an assumption once there is something to measure.

### Not yet decided
Whether `video_frame` belongs in `apps/` long-term or becomes a
`ui.video_frame` widget that any app can drop in. The latter is the point
of the exercise, but the API should follow a second real consumer rather
than be guessed at from one demo.

## win32 child-widget opacity (and the capture path that would prove it)

`ui.style_opacity` on a child widget does nothing visible on win32, and
`ui.transition`'s easing therefore has nothing to animate there. Two separate
pieces, only the first of which is understood.

**Wiring it up.** `aether_ui_set_opacity` is top-level-only by design. The
guard that enforced this was broken until 2026-08-14 — it tested `WS_CHILD`
against `GWL_EXSTYLE`, where that bit is `WS_EX_NOINHERITLAYOUT` and normally
clear, so children *were* being made `WS_EX_LAYERED` while the comment claimed
they were skipped. Now fixed, which makes the no-op honest but still a no-op.

Child alpha is **not** known to be impossible here: uniform alpha has worked on
child windows since Win8, and the overlay exit fade (`w32_make_layered` +
`SetLayeredWindowAttributes` on `e->content`, a child) depends on it and is
green at 3/3. So the likely job is wiring `set_opacity` and the tween through
the same mechanism the overlays already use, not inventing a new one. The open
design question is whether a STATIC label honours it the way the overlay's
container does, and if not, whether to owner-draw the label or fade a layered
parent.

**Proving it is the harder half, and is a prerequisite.** Three instruments
were tried on 2026-08-14 and all three are blind to child controls on that box:

| instrument | result |
|---|---|
| driver `/screenshot` | decodes fine, but **uniform** — `min == max == 240`, zero dark pixels, no controls in frame |
| `GetPixel` on the live desktop | reads nothing (Session 0, no interactive desktop over ssh) |
| `PrintWindow` into a DIB | uniformly black bitmap |

This matters more than it looks. A capture that is merely *blank* still decodes
and still yields a number, so a pixel test reads constant ink and reports "the
label did not fade" — a false accusation against code that may be correct.
`tests/transitions_demo/test_easing_curve.sh` now guards on pixel SPREAD and
skips as a blind instrument rather than failing; until that skip stops firing,
**no pixel evidence about win32 easing is admissible either way**.

Worth fixing the capture first: it is the same instrument golden-image tests
would need on win32, so it is not effort spent only on this item.

## Push rendering semantics out of the backends and into the vg layer

**The balance of C to Aether is roughly right; the boundary is wrong in one
place.** aether-ui is 64% Aether / 36% C (44k / 24.5k lines), and 86% of the
C is three implementations of one surface — gtk4 7.0k, win32 7.9k, macOS
6.0k — behind a 241-function ABI in `ui/module.ae`. That much C is not the
problem: GTK4 and AppKit bindings cannot be written in anything else, and
the vg layer already goes through `ui.*` rather than touching externs.

The problem is **what** the C decides. A backend should *translate* ("here
is a path, here is a paint, draw it"), not *interpret*. Where SVG/canvas
semantics leak below the ABI, the same decision gets re-made three times —
which costs 3x to fix and can be silently wrong in two of the three.

The 2026-08-13 GDI+ session is the evidence. Of ten bugs fixed:

  * Five were genuinely win32-local — a stale pen, a 256-point clip cap, a
    hardcoded wrap mode. Fixed once each. This is what a platform backend
    is *for*.
  * Two were semantics that had leaked: `fill-rule` and
    stroke-linecap/linejoin on gradients. Each had to be threaded through
    the C ABI and implemented in **all three backends**.

`fill-rule` is the cautionary one. No backend was told the rule; each
hardcoded one. GTK4 looked correct purely because cairo's default (winding)
happens to match SVG's default (nonzero) — so it had been **silently wrong
on every `fill-rule="evenodd"` file** for as long as the feature existed,
and nothing caught it because the reference backend agreed with librsvg on
the common case. Fixing it moved GTK4 too: accessible.svg 17.87 -> 0.39,
ruby 4.95 -> 0.05, paths-data-09-t 13.54 -> 6.51.

The same fault runs the other way as well: `vg/grammar/defs.ae` collapses
`gradientTransform` to `affine_average_scale`, so a non-uniform matrix is
lossy *before any backend sees it*. All three are equally crippled by one
Aether-side decision. That is the top remaining GDI+ gap (`python.svg`,
`car.svg`'s grille, `compuserver_msn_Ford_Focus`) and it is not a win32 bug.

### What to do

Make the backend switch policy-free — the vg layer emits fully resolved
geometry and paint state, the backend only draws it. Concretely:

1. **Audit the 241 externs for policy.** Anything a backend has to
   *interpret* (fill rules, spread methods, cap/join defaults, units,
   transform resolution) either travels with the command or is resolved
   upstream. Start from the ones already known: `gradientTransform` (lossy
   upstream — carry the full matrix to the brush), radial gradient geometry
   (`CanvasCmd` carries a scalar `gr` that cannot describe an ellipse).

   **Started.** `vg/test/test_gradient_transform.ae` (in `ci.sh`) pins the
   `gradientTransform` half. It is a vg-layer test linking no backend,
   because the loss happens upstream of all three. Nine assertions pass —
   they guard the uniform case, which must not move — and **three are
   EXPECTED-FAIL**, spelling out what a correct resolution has to preserve:
   the two semi-axes of the ellipse, and its orientation under a rotated
   non-uniform matrix. They are reported, not fatal, so the suite stays
   green while the item is open; the file has a `STRICT` const to flip to
   `1` once the fix lands, which turns it into a real gate. The rotation
   assertion is the argument against the cheap fix: carrying a second
   radius would still lose the tilt, so the matrix itself has to travel.

   **Next step — scoped 2026-08-13, not yet started.** Tracing the radial
   path end to end found the loss happens in THREE independent places, not
   one:

   | # | site | what it does |
   |---|------|--------------|
   | 1 | `defs.ae:303` | `r *= average_scale(gradientTransform)` |
   | 2 | `shapes.ae:222` | `r *= average_scale(current_transform)` — userSpaceOnUse |
   | 3 | `shapes.ae:236` | `r = gr * max(bw, bh)` — objectBoundingBox |

   (3) is not even a matrix collapse: it picks the LARGER bbox extent, an
   explicit circle-in-bbox approximation that its own comment admits to. So
   a fix at `defs.ae` alone would be defeated twice over downstream.

   The **linear** path is the proof that this is a design choice rather
   than a constraint. It has none of these: it maps both endpoints through
   the transform (`shapes.ae:249-252`, `defs.ae:311+`) and loses nothing.
   That is exactly why linear gradients could be fixed per-backend during
   the GDI+ session and radial could not.

   The shape of the fix follows: give a radial the same treatment as a
   linear — carry enough resolved geometry to describe an ellipse (two
   conjugate axes, or the matrix itself), through all three sites, and
   widen `canvas_fill_radial_gradient` once. Then each backend maps it to
   its native call (cairo takes a matrix on the pattern; GDI+ needs a
   transformed path gradient; CoreGraphics has one too) and no backend
   decides anything. Sequence: fix the vg layer first with the test's
   EXPECTED-FAILs as the gate, then the three backends, then flip STRICT.

   **Is the SVG corpus enough to verify this? NO — and it can argue for
   the wrong code.** Measured 2026-08-13, before starting:

   * 20 corpus files carry a non-uniform `gradientTransform` on a radial
     (268 such gradients). But only **4 have meaningful headroom**, and
     **3 of those 4 have no `viewBox`** (`AJ_Digital_Camera`, `car`,
     `juanmontoya_lingerie`), so their MAE is dominated by the scale
     artefact rather than by gradient geometry. Exactly ONE file --
     `gallardo` (gtk4 6.18 / gdi+ 8.06) -- is both affected and fairly
     measurable. One usable signal is not a safety net for a change that
     touches three loss sites, the ABI and three backends.
   * The rest (`AC`, `JC`, `KC`, `QC`, `KS`, `AD`, `AH`, Trajan) score
     **0.8–3.2 today WITH the wrong geometry**. A correct ellipse moves
     their pixels; MAE may rise on files that have become more correct, or
     fall on files that got luckier. Neither outcome is evidence.
   * Precedent from the same session: `php.svg` scores 4.15 with a RADIAL
     stroke painted by a LINEAR brush. Three more-correct radial geometries
     were measured and **all scored worse**. The corpus actively argued for
     the wrong code, and was right to be overruled.

   So the corpus is a **regression tripwire, not an oracle**: it answers
   "did I break something that used to work", never "is this correct".
   Correctness has to come from `test_gradient_transform.ae`'s assertions
   (derived through the transform, not from any renderer's output) plus a
   purpose-built SVG whose expected geometry is known by construction --
   a circle under `matrix(2,0,0,1,...)` must render as an ellipse with a
   2:1 axis ratio, measurable directly from the pixels without reference
   to librsvg at all.

   **Ratcheting sequence — each step ships green, no step is a cliff:**

   1. ~~Add the purpose-built repro SVGs and pixel-measure the axis ratio.~~
      **DONE 2026-08-13.** Four repros in `vg/test/svg/` plus the oracle
      `tests/radial_ellipse_check.py`, which measures the half-maximum
      extent through the gradient's own peak — expected ratio known BY
      CONSTRUCTION, so no reference renderer is involved:

      | repro | want | pins |
      |---|---|---|
      | `radial_ellipse_repro` | 2.00 | `matrix(2,0,0,1)` — wide ellipse |
      | `radial_ellipse_tall` | 0.50 | `matrix(1,0,0,2)` — axis-swap guard |
      | `radial_uniform_control` | 1.00 | CONTROL: correct today, must stay |
      | `radial_ellipse_bbox` | 2.00 | objectBoundingBox — the third loss site |

      GTK4 today scores **1/4**: the control passes, the three real cases
      all read 1.00 (a circle). librsvg reads 1.98 / 0.51 / 1.00 / 2.02,
      confirming the expectations independently. No production code
      changed. Complements `gallardo.svg`, the corpus's only affected AND
      fairly-measurable file.

      **Also found, and separate:** GDI+ fails even the UNIFORM control --
      not by getting the axes wrong (it draws a correct circle) but by
      drawing it far too LARGE and off-centre. Half-maximum span 202px
      where librsvg and GTK4 both measure 122px, peak at (215,200) against
      their (199,199). That is the bounding-box approximation in the radial
      fill: it sizes the gradient from the SHAPE's bbox, which for a
      canvas-filling rect is the whole canvas. Distinct from the ellipse
      work and worth its own fix. The checker now returns UNMEASURABLE when
      the half-maximum extent touches the canvas edge, rather than
      reporting a ratio that is really measuring the frame -- GDI+ read
      0.67 on a perfectly round circle before that guard, which would have
      sent the ellipse work chasing a size bug.


   2. ~~Carry the ellipse geometry through the vg layer alongside the
      existing scalar radius.~~ **DONE 2026-08-13.** `GradientDef` gains
      `rx`, `ry`, `rot_deg` (semi-axes and tilt), populated in `defs.ae`
      from the gradientTransform's transformed basis vectors; the scalar
      `r` is untouched and still authoritative, and nothing consumes the
      new fields yet. Verified INERT: all **212** rendered files (corpus +
      repros) byte-identical on GTK4. Five new assertions in
      `test_gradient_transform.ae` check the carried values are right
      before any backend is pointed at them -- including that a UNIFORM
      matrix collapses back onto the old scalar exactly (120 == 120), which
      is what keeps `radial_uniform_control.svg` passing. NB `mk_gradient_def`
      hand-computes its malloc size; it grew 128 -> 192 for the three new
      floats.

   3. Per backend, one at a time: consume the new key, keep the old as the
      fallback when it is absent. **GTK4 DONE 2026-08-13** — cairo patterns
      carry their own matrix, so a unit-circle radial mapped by
      translate/rotate/scale expresses the ellipse exactly. Scores **4/4**
      on the axis-ratio oracle (1.98 / 0.51 / 1.00 / 2.02, matching
      librsvg), and the corpus improved with **nothing regressing**:
      mean 3.44 -> 3.27, radialgradient1 15.54 -> 0.89,
      rg1024_Ufo_in_metalic_style 15.99 -> 3.15, intertwingly 2.43 -> 0.98.
      win32 and macOS accept the widened ABI and explicitly ignore the new
      arguments, still reading the scalar `radius` — so they are unchanged
      and the step was not a cliff.

      Gotcha worth keeping: the focal point lives in PATTERN space, so its
      offset must be scaled PER AXIS (and de-rotated first). Collapsing it
      to `|f-c| / rx` is only correct for a circle and cost intertwingly
      +2.90 before it was fixed.

      **win32 DONE 2026-08-13** (semi-axes only). That branch already drew
      with separate rw/rh -- it just set both to the scalar radius, so the
      eccentricity had been discarded upstream and could not have been
      honoured anyway. Oracle **1/4 -> 3/4** (1.99 / 0.50 / 1.00);
      corpus mean 4.51 -> 4.44, rg1024_Ufo_in_metalic_style 16.17 -> 5.68,
      intertwingly 3.12 -> 1.77, one +0.07 regression.

      Two things deliberately NOT done on win32 yet:
      * **Rotation.** `grot` is carried but not applied -- GDI+ needs a world
        transform around the fill (GdipSetWorldTransform et al are not even
        declared in this file). No corpus file has a rotated non-uniform
        radial, so this is unmeasurable today; the test's third
        EXPECTED-FAIL is the thing that will catch it.
      * **The bbox repro reads UNMEASURABLE on GDI+**, but that is a HARNESS
        difference, not a rendering one: svg_render_png renders
        `viewBox="0 0 200 100"` onto a 400x400 canvas on BOTH backends
        (librsvg gives 400x200). GTK4 scores it only because its shape still
        fills the canvas. Worth fixing in the harness, separately.

      **macOS DONE 2026-08-13**, built and verified on the real Mac KVM
      (`ssh macvm`). CoreGraphics draws only circular radial gradients, but
      the CTM is ours to bend inside the existing SaveGState/RestoreGState
      pair: translate, rotate, scale, then draw a UNIT circle. That makes
      macOS the ONLY backend doing the full ellipse INCLUDING rotation, and
      it scores **4/4** on the oracle (2.01 / 0.50 / 0.99 / 2.02) -- the
      only backend to score the objectBoundingBox case, which GDI+ cannot
      measure and which needed the CTM to get right.

      First macOS corpus baseline against librsvg, while we were there:
      **1.82** on the 167 viewBox files (155 good / 8 ok / 4 diff), 3.55
      all-files -- level with GTK4's 1.77 and ahead of GDI+'s 3.00.

      **win32 rotation DONE 2026-08-13.** GDI+ fills axis-aligned ellipses
      only, so the tilt comes from the world transform: rotate about the
      centre, fill, restore. Oracle 3/5 -> 4/5 (the rotated repro's diagonal
      probe 1.02 -> 1.96 against librsvg's 2.02); corpus mean 4.44 -> 4.43,
      nothing meaningfully changed -- which is the point, since no corpus
      file has a rotated non-uniform radial.

      **VERIFIED ON ALL THREE, 2026-08-13**, after the canvas-aspect fix in
      `svg_render_png` (which was what blocked GDI+'s fifth repro):

      | backend | oracle | corpus, viewBox-only |
      |---|---|---|
      | GTK4 | **5/5** | 1.75 -> **1.47** |
      | macOS | **5/5** | 1.82 -> **1.55** |
      | GDI+ | **5/5** | 2.73 -> **2.49** |

      The corpus improvement is the canvas-aspect fix, not the ellipse work;
      both are folded in here because the arms had to be re-rendered to be
      comparable. NB the baselines quoted earlier in this file (GTK4 1.77,
      GDI+ 3.00) were measured on square canvases and are superseded.

      **ALL THREE BACKENDS NOW CONSUME THE ELLIPSE.** The three original
      EXPECTED-FAILs are gone: they asserted a scalar might one day express
      an ellipse, which it never can, so they were restated as direct
      assertions of the loss and the test is fully green.
      `tests/radial_ellipse_check.py`: GTK4 5/5, macOS 4/4, GDI+ 4/5 (its
      one gap is the harness canvas-size issue below, not rendering).

   4. ~~Remove the scalar radius, flip STRICT, narrow the ABI.~~
      **ASSESSED 2026-08-13, and deliberately NOT done wholesale** — each
      part judged on merit rather than executed because the plan said so:

      * **Remove the scalar from the three loss sites — NO.** The vg layer
        now always emits RX/RY, so the scalar is dead *there*; but
        `canvas_fill_radial_gradient` is a PUBLIC export with no in-tree
        callers, and dropping `radius` would break any external one for no
        gain. The fallback costs one float compare per gradient. Removing
        it is API churn, not simplification.
      * **Flip STRICT — already resolved, differently.** The flag and its
        machinery are deleted: the assertions it guarded claimed a scalar
        might one day express an ellipse, which is permanently false. They
        are now direct assertions OF the loss, and the test is green.
      * **Converge the two gradient calls — NO.** A merged call needs the
        union of both geometries (8 floats + a kind flag, against 4 and 8
        today), so every linear caller would pass four dead arguments.
        Net 241 -> 240 externs: one line saved, clarity lost. Linear and
        radial are genuinely different GEOMETRY, not policy leaking into
        the backend — which is what this item is about.

      **On the ABI-width target:** this work ADDED three parameters, against
      a stated goal of trending down. That is the right trade and worth
      being explicit about: the three carry *data the renderer needs*
      (semi-axes and tilt) so the backend can stop *deciding* what a
      gradient's shape is. The number to watch is decisions-per-backend,
      not parameters; a wider surface with dumber backends is the stated
      success condition.


   Step 2 is the important one: it makes the change **additive**, so there
   is never a window where the vg layer emits geometry no backend
   understands. Doing it the other way -- fix vg, then chase three backends
   -- leaves every affected file broken in between, which is the cliff this
   sequence exists to avoid.
   **AUDIT, 2026-08-13.** Read the 31 canvas externs (the largest group, and
   where all four known leaks lived) looking for anything a backend has to
   *interpret* rather than translate. Findings, worst first:

   * **FONT FAMILY — the big one, and the same shape as gradientTransform.**
     `canvas_fill_text` / `canvas_stroke_text` carry `font_flags: int`, which
     is THREE BITS: mono, bold, italic (`vg/backend/gtk.ae:499-502`). The
     actual `font-family` IS read upstream — `shapes.ae:999` does
     `ff = style_get(style, "font-family")` — and then immediately reduced to
     `is_monospace(ff)`, discarding the name. So every backend substitutes
     its own family from the same three bits:

     | backend | what it draws with |
     |---|---|
     | GTK4 | `cairo_select_font_face` from the flags |
     | win32 | hardcoded **"Segoe UI"** |
     | macOS | `systemFontOfSize` (the system font) |

     Three backends, three different fonts, one input. 21 of 208 corpus
     files contain `<text>`. Exactly the "data read upstream then thrown
     away" pattern, and the fix is the same shape: carry the family string
     and let each backend map it to a face.

   * **Reviewed and CLEAN** (they translate, they do not interpret):
     `arc` carries centre/radius/start/end with no sweep ambiguity;
     `clip_rect` / `set_clip_rects` / `reset_clip` are pure geometry;
     `fill` now carries `even_odd`; `stroke` carries cap/join; both gradient
     calls now carry spread plus the full ellipse. The path primitives
     (`begin_path`/`move_to`/`line_to`/`close_path`) are as dumb as they
     should be.

   So the canvas surface is in good shape apart from text. The remaining
   **AUDIT COMPLETED over the remaining 210 externs, 2026-08-14.** The
   non-canvas groups are widget-level, so a leak there looks different --
   platform CONVENTION rather than discarded geometry. Findings:

   * **OVERLAY TRANSITION KIND — a real leak, three interpretations.**
     `overlay_set_transition(handle, kind, ms)` passes a string each backend
     reads differently: GTK4 knows `fade`/`scale`/`none`; win32 animates ANY
     non-empty kind as a fade (it stores the string but branches only on
     non-emptiness, so `scale` silently fades); macOS discards it outright
     (`(void)kind`) and honours only the duration. Same shape as fill-rule
     before it was fixed: one string, three answers, and the reference
     backend looks right so nothing flags it.

   * **`widget_apply_css` — inherent, not a leak.** GTK4 hands the string
     straight to `gtk_css_provider_load_from_data`; win32 and macOS parse the
     subset they can honour. CSS is a GTK-native format, so this is a
     deliberate asymmetry rather than semantics leaking below the ABI.
     Narrowing it would mean designing a neutral style surface -- the AeCS
     layer already is that, one level up.

   * **Reviewed and CLEAN:** a11y (all three recognise the identical role
     set -- button/checkbox/dialog/heading/image/link/list/listitem),
     overlay material (`dim`/`blur`/`none`, consistent), tray, window,
     widget, sheet, surface, undo, shortcut, notify, bind. These pass
     handles, geometry and opaque payloads; none require a backend to decide
     what something MEANS.

   So across all 241 externs the audit found **ten** leaks, and **all ten are
   now closed.** The overlay transition kind was fixed the same day it was
   recorded: win32 now distinguishes slide-up/slide-down/scale (moving and
   resizing the layered window on the same tween clock as the alpha) and
   macOS honours them via Core Animation frame tweens, having previously
   discarded the string outright. Unrecognised kinds still fade, so nothing
   that worked before changed. `overlaytr` 3/3 green on GTK4 and on win32
   with a forced rebuild.



   **FONT FAMILY: DONE on all three backends, 2026-08-13.** The raw CSS
   stack now travels as a `fontFamily` opt and through
   `canvas_fill_text`/`canvas_stroke_text`; each backend resolves it with its
   own matcher (cairo/fontconfig, GDI+ walking the stack, AppKit
   `fontWithName:`), per the design call that resolving a prioritised list
   against installed fonts is a platform question, not policy. Step 2 was
   verified inert (208 files byte-identical) before any backend consumed it.

   * GTK4: viewBox-only 1.47 -> **1.33**. `decimal` 15.45 -> 0.44 — the
     largest single-file win of the session — plus three W3C conformance
     files.
   * win32: 2.49 -> 2.61, i.e. the score got WORSE while the code got more
     correct. `decimal` asks for `serif`; librsvg on the Linux reference box
     resolves that to Noto Serif (confirmed with `fc-match`), Windows has no
     Noto Serif and maps it to Times New Roman. Both right on their own
     platform. Kept on the evidence of what the code does, not the number.
   * macOS: **no change, and honestly so** — see the bug below.

   ~~**FOUND, NOT FIXED: macOS renders TEXT-ONLY SVGs completely blank.**~~
   **FIXED 2026-08-13** — `CANVAS_FILL_TEXT` draws with AppKit's
   `-[NSString drawAtPoint:]`, which renders into
   `+[NSGraphicsContext currentContext]` and NOT into the `CGContextRef` it
   is handed; headless there was no focused view, so every glyph went
   nowhere. Pushing a context backed by the PNG bitmap around the replay
   (`flipped:YES`, matching the already-flipped CTM) fixed it. Original
   finding below.

   **THE FINDING:**
   `decimal`, `Steps` and `blocks_game` produce ZERO ink on macOS while GTK4
   and librsvg draw them (decimal: librsvg 25115, GTK4 25125, macOS 0);
   `helloworld`, which has other content alongside its text, renders fine.
   Ink was 0 both before and after the font-family change, so it is
   pre-existing and unrelated. That is why macOS's corpus mean did not move:
   the resolver is correct but the path it feeds never runs for exactly the
   files that would show it off. 21 of 208 corpus files contain `<text>`.
   Worth its own investigation — start by checking whether the AppKit
   drawing context is even flushed for a canvas with no path commands.


2. **Target: the ABI width trends DOWN, not up.** 241 functions is the
   number to watch. A wider surface with dumber backends is fine; a wider
   surface with *smarter* backends is the failure mode.
3. **Make the third backend cheap.** The test of success is that adding a
   platform costs work proportional to its *surface* (how do I draw a path
   here?) and not to its *semantics* (what does even-odd mean?).

Worth doing before any fourth backend, and before the remaining
gradientTransform work — that fix belongs in the vg layer and would improve
GTK4, win32 and macOS at once.
