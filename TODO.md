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

## Rename scribe → maerkdown

The word-as-widget markdown editor (apps/scribe) should be renamed
Maerkdown: apps/scribe/ → apps/maerkdown/, binary + window title +
spec suite name + ci ENGINE_TESTS entry + spec_matrix row, keeping the
Sutherland-free identity (mdown.ae/wordflow.ae module names can stay).
