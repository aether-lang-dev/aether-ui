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
2. **Target: the ABI width trends DOWN, not up.** 241 functions is the
   number to watch. A wider surface with dumber backends is fine; a wider
   surface with *smarter* backends is the failure mode.
3. **Make the third backend cheap.** The test of success is that adding a
   platform costs work proportional to its *surface* (how do I draw a path
   here?) and not to its *semantics* (what does even-odd mean?).

Worth doing before any fourth backend, and before the remaining
gradientTransform work — that fix belongs in the vg layer and would improve
GTK4, win32 and macOS at once.
