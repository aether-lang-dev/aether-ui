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

## Widget Inspector (Flutter DevTools-style)

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

      **ALL THREE BACKENDS NOW CONSUME THE ELLIPSE.** The three original
      EXPECTED-FAILs are gone: they asserted a scalar might one day express
      an ellipse, which it never can, so they were restated as direct
      assertions of the loss and the test is fully green.
      `tests/radial_ellipse_check.py`: GTK4 5/5, macOS 4/4, GDI+ 4/5 (its
      one gap is the harness canvas-size issue below, not rendering).

   4. Only once all three consume it: remove the scalar radius from the
      three loss sites, flip `STRICT` to 1, and narrow the ABI if the two
      gradient calls can now converge.

   Step 2 is the important one: it makes the change **additive**, so there
   is never a window where the vg layer emits geometry no backend
   understands. Doing it the other way -- fix vg, then chase three backends
   -- leaves every affected file broken in between, which is the cliff this
   sequence exists to avoid.
2. **Target: the ABI width trends DOWN, not up.** 241 functions is the
   number to watch. A wider surface with dumber backends is fine; a wider
   surface with *smarter* backends is the failure mode.
3. **Make the third backend cheap.** The test of success is that adding a
   platform costs work proportional to its *surface* (how do I draw a path
   here?) and not to its *semantics* (what does even-odd mean?).

Worth doing before any fourth backend, and before the remaining
gradientTransform work — that fix belongs in the vg layer and would improve
GTK4, win32 and macOS at once.
