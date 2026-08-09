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

### 4. Audio, and A/V sync
There is no audio path for video at all. LisMusic has audio machinery
worth reading first. Sync is the real work: today the frame index is
`t * fps` off the scene clock, which drifts against an audio device's own
clock. Wants a presentation-timestamp model (decode-time PTS per frame,
present against the audio clock, drop/repeat to hold sync) rather than
frame-index arithmetic.

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
