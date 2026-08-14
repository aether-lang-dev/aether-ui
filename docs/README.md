# aether-ui docs

Two audiences, two directories.

## `guide/` — using aether-ui

End-user DX: how to write apps against the widget DSL (`ui`) and the
vector layer (`vg`).

- [**dsl-with-scope.md**](guide/dsl-with-scope.md) — the builder-block
  style the whole toolkit is written in: nested blocks that read
  declaratively but are executed code, with an implicit receiver.
- [**typography.md**](guide/typography.md) — drawing, measuring, and
  fitting text in `vg`: baselines, `text_extent`, centring, ellipsize.

## `design/` — how it's built (and where it's going)

Internal architecture notes, plans, and cross-platform handoffs. Not
needed to *use* aether-ui; read these to change it.

### The architecture

- [**semantics-belong-above-the-abi.md**](design/semantics-belong-above-the-abi.md)
  — a backend should translate, never interpret. The rule, the eleven
  leaks an audit of all 241 externs found, the ratchet for widening an
  ABI across three backends safely, and why MAE is a regression tripwire
  rather than an oracle.
- [**retained-compositor.md**](design/retained-compositor.md) — the
  rendering north star (aspirational; the current backend is
  immediate-mode).
- [**reactivity-unification.md**](design/reactivity-unification.md) —
  typed `ui` state and the binding surface over it.
- [**styling.md**](design/styling.md) — AeCS, the Aether Cascading Styles
  layer: the neutral style surface above the backends' native ones.
- [**accessibility.md**](design/accessibility.md) — the a11y semantics
  layer (roles, labels, descriptions) across GtkAccessible / MSAA /
  NSAccessibility.
- [**multi-window.md**](design/multi-window.md) — co-equal top-level
  windows over one event loop.

### Rendering

- [**win32-gdiplus-renderer.md**](design/win32-gdiplus-renderer.md) — the
  branch-by-abstraction that put a GDI+ renderer beside the legacy GDI
  one, the comparison that decided between them, and which files the
  corpus cannot fairly score.
- [**vg-drawn-controls.md**](design/vg-drawn-controls.md) — the strategic
  fork: four postures for drawn vs native chrome, scored against this
  codebase. **Not scheduled**; the verdict and its conditions are
  recorded so the option stays open.
- [**backdrop-material.md**](design/backdrop-material.md) — frosted
  scrims and what each platform can actually honour.
- [**aevg-live-regions-plan.md**](design/aevg-live-regions-plan.md) —
  glitch-free live content (video/games) composited inside one AeVG scene.
- [**aevg-resize-native-followup.md**](design/aevg-resize-native-followup.md)
  — the `vg{}`-scene resize contract for the AppKit / Win32 backends
  (GTK reference to mirror).
- [**stage2-clip-flicker-fix.md**](design/stage2-clip-flicker-fix.md) —
  nine rounds on one clip bug, and what each wrong theory cost.
- [**stage3-per-frame-surfaces.md**](design/stage3-per-frame-surfaces.md)
  — per-frame surfaces and the caching they enable.
- [**path-cubic-steps-desync.md**](design/path-cubic-steps-desync.md) —
  a "load-bearing constant" that turned out to be a desync between two
  copies of the same number.

### Testing and platforms

- [**one-driver-not-two.md**](design/one-driver-not-two.md) — the
  AetherUIDriver: one implementation behind a hooks seam, not one per
  backend. The driver is the *only* instrument that can see widget-level
  behaviour — overlays, navigation, focus, selection are invisible to the
  SVG corpus and to golden images.
- [**getting-windows-and-macos-green-via-remote-agents.md**](design/getting-windows-and-macos-green-via-remote-agents.md)
  — proving the Win32 + AppKit backends compile and run on real hardware
  via remote build agents.
