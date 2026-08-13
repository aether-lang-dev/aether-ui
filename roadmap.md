# Roadmap: what remains

**Status: PARED DOWN.** 2026-07-14. The nine-item ranked roadmap of
2026-07-12 (overlay layer, typography, `each`, table/list, effects,
transitions, layout, bindings, focus) was executed in full on
2026-07-12/13 — see git history for the original document with its
per-item DONE blocks, evidence tables, and commit trails (last full
version at 1ade88b; the wound-driven method writeup lives there too).
This document now lists ONLY what is not done, in three buckets:
platform parity, deferred follow-ups, and the backlog/strategic fork.

## 1. Platform parity

### macOS — DONE 2026-07-14 (tabs added 2026-07-17). Full parity; `./ci.sh` all phases pass.

The AppKit backend is no longer the stubbed one. Every item that was
listed here as a macOS gap is real, and the spec matrix is **85/85
green on the Mac** (`./tests/spec_matrix.sh`), including all five
grand_perspective app suites and `ui.tabs`.

| Feature | macOS today |
|---|---|
| Driver introspection + `/focus` `/window/key` `/window/resize` | **shared server** — the private one is deleted (see below) |
| Text metrics (`vg.text_extent`) | CoreText (`CTLineGetTypographicBounds`) on the SAME font the canvas draws with |
| splitview | real `NSSplitView`, draggable divider (NB its `vertical` is the INVERSE of GTK's) |
| tabs (`ui.tabs`) | real `NSTabView`; on_change fires via the delegate, deduped against last index (swallows AppKit's initial auto-select) |
| weight / on_layout / wrap | Auto Layout multiplier constraints / frame-change observer / `AetherWrapView` flow layout |
| shortcut (accelerators) | one `NSEvent` local monitor + a normalized-combo registry |
| focus (grab / `GET /focus` / Tab) | `makeFirstResponder:`; **Tab follows BUILD order** (matches GTK — see below) |
| Overlay layer (toast/modal/tooltip/dropdown) | `AetherOverlayHost` interposed under the window; real scrim, hit-test-blocking |
| Context menus | `-[NSView setMenu:]` (the native right-click shape) |
| Canvas on_click/on_key/on_release/on_resize, clip_rect, `canvas_write_png` | all real; write_png is headless (CGBitmapContext) |
| Implicit transitions (`ui.transition`) | hand-driven tween (NOT `-[NSView animator]` — see below) |
| toggle_group | real radio exclusivity |

**The big structural change: macOS adopted the shared test server.**
It used to carry its own ~370-line HTTP server, which had drifted badly
(no `enabled`/geometry/`classes` fields, no `/focus`, `/window/key`,
`/window/resize`, no tray/notification routes, no URL-decoding, and it
read NSView state straight off the server thread). It is gone; macOS now
fills in `AetherDriverHooks` exactly as win32 does. **The "three-servers
rule" is back to two** — GTK4's embedded one, and the shared one behind
both other backends.

**Routes added to the SHARED server** (so win32 gained them too — they
had been GTK4-only, which is why split/overlay/canvas specs were red on
*both* non-Linux backends regardless of backend quality):
`/text_extent`, `/overlays`, `/window/pick`, `/canvas/{id}/click|move|key`,
`/widget/{id}/split_position`, `/widget/{id}/context_menu[/{idx}]`,
`/shutdown`, plus `splitPosition` and `selected` in the widget JSON.

Traps found the hard way, all now commented at their site:
- **Tab order.** AppKit's key-view loop only walks text fields unless the
  user has *Full Keyboard Access* on system-wide — Tab order would depend
  on a setting outside the app. macOS therefore walks the widget registry,
  whose order IS build order. **macOS matches GTK here where win32 cannot.**
- **Geometry must report the ALIGNMENT rect, not the frame.** An
  `NSTextField`'s frame is ~2px wider per side than the box Auto Layout
  positions; reporting frames made adjacent siblings look like they
  *overlapped* by 4px and a row's widths sum past its container.
- **Expand must propagate UP, retroactively.** GTK: a box holding an
  expanding child expands. AppKit has no such rule, and the DSL assembles
  top-down (a container joins its parent *before* its children arrive) —
  so the canvas announces "I expand" long after its row was already
  height-chained to its siblings. `aeui_mark_expand` walks up and retracts
  those chains. Without it, Grand Perspective's treemap was pinned to the
  toolbar's height and everything below the fold was unclickable.
- **`-[NSView animator]` is invisible to the driver.** It sets the model
  value immediately and animates the *presentation* layer, so every
  mid-tween screenshot already shows the settled frame. Transitions are
  hand-stepped on a timer so intermediate states are real and assertable.
- **`ui.style_opacity` travels as CSS**, not through the `set_opacity` ABI
  call. Miss that in `apply_css` and every declared transition snaps.

**Buttons stretching vertically (mostly fixed 2026-07-17).** Buttons carry
contentHugging 200, so they absorb a vstack's leftover space. The fix was
NOT to change the button (the calculator *depends* on that stretch for its
grid, and has no canvas to hand the slack to): instead the CANVAS's
size-preference constraint was dropped to priority 150, below the button
hugging. A canvas and a button both grow to take slack; the lower-resistance
one wins, so the canvas now out-competes buttons. rubiks_cube's Reset/
Shuffle/Solve went from 146px tall to a natural 20px and the cube canvas
took the freed space; the calculator (no canvas) is untouched. Remaining:
in a stack with a **non-canvas** slack-taker missing (e.g. a lone button row
with nothing below it, or the **tabs page area** sharing a vstack with a
button row) the buttons still absorb the slack. The real end-state is
natural button hugging + explicit `ui.weight` opt-in, which wants a GTK
side-by-side first; the canvas-priority fix covers the common case.

### win32 — PARITY ACHIEVED 2026-07-20. Full matrix 188/0, equal to Linux.

Real: focus/Tab (dialog order, not build order), driver introspection,
state/bindings, canvas `on_click`/`on_key`/`on_release` (`c21334c`),
picker (combobox + driver set_value + default selection, `962ed0e`),
**overlay layer** (`962ed0e` — raised child HWNDs over the client area:
toast/modal-scrim/tooltip, append-only `/overlays`, Escape-dismiss,
deep `/window/pick`; toast lifecycle proven green), and now everything
the shared server gained above.
*(Historical, ALL CLOSED 2026-07-20 — see FINAL BASELINE below.)*
~~Still stubbed: splitview, shortcuts edge-cases, text metrics (GDI),
weight / on_layout / wrap.~~ All real now.

**~~Two cross-cutting win32 bugs~~ (both fixed 2026-07-20):**
- **widget geometry reports `h:0`.** Every widget's driver rect has
  height 0 (the calculator shows it too — it's green only because its
  specs don't read geometry). This makes `/window/pick` at a widget's
  computed centre miss, so the overlay pick/scrim tests (4) fail even
  though the overlay layer works. Also likely behind gp_hover_and_resize
  and any future geometry assert. The HWNDs render fine — it's the
  `widget_rect` hook / stack-layout-timing that reports wrong.
- **canvas scene doesn't rescale** (`canvas_on_resize` is a stub): the
  vg scene stays at its initial viewBox size, so hover px coords don't
  map to scene hit regions → vg_tooltip hover finds no shape (4 fails).
  The overlay/tooltip plumbing underneath is correct.

~~Known harness gap~~ FIXED 2026-07-16: gp clips crumb-button labels
(>20 chars → 17+"..."), and on Windows the fixture's long native path
truncates while Linux's short `~/.gp-ci-XXXXXX` doesn't —
`gp_driver.root_crumb_label()` now mirrors the clip, so the spec
searches for the AS-RENDERED label. With that plus the canvas-events
commit, **gp_map_nav is 2/2 green on Windows** (drill + crumbs +
keyboard nav proven through the matrix). NB: the ae module cache can
serve a STALE spec helper — `rm -rf ~/.aether/cache` when a spec edit
seems to have no effect.

**Windows matrix baseline (2026-07-16 pm, `962ed0e`): 52 passing / 56
failing.** GREEN suites: calculator 9, each 6, listbox 6, table 4,
bindings 6, picker 3, gp_map_nav 2. Partial: overlay 3/7 (toast
lifecycle green; pick tests blocked by the h:0 geometry bug above),
vg_tooltip 1/5 (hover blocked by canvas-scene scaling above). Remaining
reds map to: **the two cross-cutting bugs** (overlay pick, vg_tooltip
hover), shortcuts (testable ×2, gp_scan_and_list ×4), splitview+layout
(split 13), GDI metrics (text_metrics), ctx-menu driver actions + gio
(context_menu, gp_fileops), toggle_group radio exclusivity
(gp_legend), Tab order platform-native (testable 1). (The tabs strip is
no longer a stub — win32 tabs went real 2026-07-18; LisMusic's spec is
5/5 on winbaz.)

**Windows spec-matrix baseline (2026-07-14 evening, winbaz, post
macOS-parity pull + timer fix):** `AEOCHA_DIR=... ./tests/spec_matrix.sh`
→ 44 passing / 81 failing over 17 suites. GREEN: calculator 9, each 6,
listbox 6, table 4, bindings 6. Every red maps to a NAMED win32 gap in
the table above, none to harness artifacts: text_metrics (stub),
testable 3 (shortcuts unwired ×2, Tab order is Windows-native),
context_menu (driver ctx actions unwired), overlay+vg_tooltip+picker
(overlay host stub), split (splitview stub + on_layout no-op),
gp suites (canvas click/key unwired, shortcuts, gio absent on
Windows). Fixed to get here: build.sh fallback + cygpath fixture paths
in spec_matrix.sh, and a REAL long-standing win32 bug — timers created
before app_run ran as thread timers whose system-assigned id never
matched ours, so ui.timer closures never fired (gp's scan pump was
dead on Windows, likely forever).

**FINAL BASELINE 2026-07-20: GTK4 188/0 AND win32 188/0 — full parity.**
The 2026-07-19 named-gap list below was closed in one arc (44/81 on 07-14 →
141/47 on 07-19 → 188/0 on 07-20). What it took, in order: bottom-up natural
sizing for containers (THE h:0 fix — stacks measured as their current rect,
which is 0 until laid out, so nothing nested ever grew), cross-axis fill +
greedy primary-axis flex with expand-propagating-up (the macOS lesson) +
a pinned-size veto that exempts intrinsically-greedy kinds; real splitview
(divider drag), real wrap (flow layout), real on_layout, GDI text metrics,
context menus (store + TrackPopupMenu + driver actions), toggle radio
groups, driver-Tab in build order, canvas rescale (WM_SIZE + on_resize);
plus five deep bugs: /window/pick skipped WS_EX_LAYERED scrims
(ChildWindowFromPointEx) then IsWindowVisible under a hidden toplevel
(manual z-walk + own-style-bit), overlays mounting in the hidden widget
holder (detached-content GA_ROOT), /window/resize resizing the BANNER
(topmost child ≠ root — masked while specs resized to ≤ startup size),
and WM_GETMINMAXINFO clamping growth to the small VM screen. gp's trash
uses the real Recycle Bin on Windows; vg apps link (-lpcre2-8 in
build.sh); LisMusic needs its documented manual sqlite build (+-lssl
-lcrypto). AEUI_LAYOUT_DEBUG=1 prints per-stack measure tables.

**(Historical) REFRESHED BASELINE 2026-07-19 (both drivable backends, full matrix):**

- **GTK4 (Linux): 188 pass / 0 fail — ALL 44 suites green.** First-ever
  full spec_matrix run on Linux. It initially showed 8 red suites — ALL
  from one harness artifact, not regressions: spec_matrix exported
  `AETHER_UI_HEADLESS=1` unconditionally, and GTK4's headless mode
  realizes-but-never-PRESENTS the window, so no allocation pass runs and
  every geometry read is 0 (win32 SW_HIDE and macOS Auto Layout still lay
  out hidden windows, so the flag is harmless there). Fixed: the export is
  now scoped to MINGW/MSYS/Darwin; Linux runs mapped under Xvfb.
- **win32 (winbaz): 141 pass / 47 fail** (was 44/81 on 07-14 — pass count
  ×3.2). All 23 of this week's feature suites are green on real Windows
  (themes 13, multiwindow 6, zen 6, a11y 5, reorder 5, csssem 5, vlist 4,
  material 3, overlaytr 3, …). The 10 genuinely red suites still map to
  NAMED gaps, now confirmed current: **geometry h:0** (overlay picks —
  the top cross-cutting fix), **canvas rescale stub** (vg_tooltip,
  gp_hover), **splitview stub** (split 0/13), **GDI text metrics**
  (text_metrics 1/5), **ctx-menu driver route** (context_menu 0/7,
  gp_fileops 0/4), **shortcut-with-entry-focused + /window/key edges**
  (testable 5/8, gp_scan 3/7), **toggle radio-group** (gp_legend 2/4),
  plus Tab order (by-design Windows divergence). falling_blocks/
  svg_tetris/rubiks_cube/lismusic are NOT failures — the vg-app exes are
  simply unbuilt on that clone (manual build path; LisMusic needs the
  sqlite 2-step).

**Update 2026-07-18 — win32 tabs went real:** the tabs backend was a
stub (a vstack that appended pages, no strip, no switching; tabs_selected
= -1). It's now a real tab strip — a header hstack of one button per tab
over a zstack of pages; stack_do_layout special-cases WK_TABS; a strip
click or programmatic tab_select shows the page, bolds the active button,
fires on_change deduped. Verified on winbaz: the LisMusic driver spec is
5/5. Two harness bugs fixed in passing (both cross-backend): the
test-server now JSON-escapes ALL control chars in widget text (a stray
U+0001 in a search title was making the /widgets body invalid JSON, so
client json.parse silently returned 0), and a widget_id_last_of driver
helper disambiguates duplicate labels. Known residual (separate, not
tabs): a win32 transient-widget-text glitch garbles the echoed search
term in one result row — cosmetic, doesn't affect the spec.

## 2. Deferred from the ranked items (recorded at each closeout)

- **Typography:** ~~multi-line wrap (`text_wrapped`), text-anchor
  middle/end~~ **DONE 2026-07-18** (`text_wrapped` + `text_anchor`;
  wrap/anchor in the text JSON; 3/3 GTK4+win32). vg/CANVAS text-anchor
  was ALREADY done (`vg.text_anchored`, "middle"/"end" → center/trailing
  with real advance-width shifting; test_grammar_text asserts it). So this
  line is fully closed.
- **Table/list:** ~~virtualization~~ **DONE 2026-07-18** — `vlist` renders
  only a bounded WINDOW of row widgets over an arbitrarily large item list;
  `vlist_scroll_to` slides the window and recycles (1000 items → 10
  widgets; scroll → still 10). Pure module.ae, so all-backend; 3/3 on
  GTK4. ~~native-scroll gesture~~ **DONE 2026-07-19** —
  `vlist_scrollable(v)` wires a real wheel/drag on the container to the
  window (GTK4 GtkEventControllerScroll, win32 WM_MOUSEWHEEL, macOS stores
  the closure — scrollWheel: is the AppKit follow-up); each step slides one
  row and recycles. Driver-fire `POST /widget/{container}/scroll?dy=N` on
  both servers. `vlist_demo`/spec native-scroll test **4/4 on GTK4 AND
  win32 (winbaz)**.
  ~~delegate cells (%-bars, chips)~~ **DONE** (`table_col_delegate`),
  ~~row double-click~~ **DONE**, ~~multi-select~~ **DONE**, ~~**tree
  mode**~~ **DONE**. The whole Table/list line is now closed. All pure
  module.ae (backend-agnostic); verified GTK4+win32.
- **Effects:** ~~shadows on paths/text~~ **DONE 2026-07-18** — vg.shadow
  now covers `<path>` (silhouette follows the real outline via
  fill_path_in_buffer_scaled + path_bounds, not a bbox rect) and `<text>`
  (offset dark copy under the glyphs; crisp — a blurred glyph shadow needs
  a glyph rasterizer vg lacks). test_effects 22/22 on GTK4 AND win32
  (winbaz — required rebuilding winbaz's aether toolchain WITH libpcre2:
  it lacked std.regex, so the SVG path parser returned no edges and path
  shadows silently no-op'd. Fixed the real cause — static libpcre2-8 on
  MinGW needs -DPCRE2_STATIC, aether commit 1896e5f0).
  ~~**backdrop blur** / materials (frosted scrim)~~ **DONE 2026-07-19** —
  `overlay_material(overlay, "dim"|"blur"|"tint")` on a modal scrim. A real
  backdrop blur is only native on macOS (NSVisualEffectView, behindWindow
  frost); win32 (can't blur behind a child HWND — acrylic/DWM are top-level
  only) and GTK4 (no backdrop-filter) degrade "blur"→"tint" (a heavier/
  lighter frosty scrim). `overlay_material_effective` + the `"material"` field
  on `/overlays` report what the backend ACTUALLY applied, so the degrade is
  asserted and nobody over-claims. `material_demo` + spec: a blur request must
  degrade to "tint" on the drivable backends (never a silent "dim") — **3/3 on
  GTK4 AND win32 (winbaz)**; macOS frosts for real (sibling). Design:
  docs/design/backdrop-material.md.
- **Transitions:** ~~enter/exit transitions on overlay ENTRIES~~
  **DONE 2026-07-19** — `transition_overlay(overlay, kind, ms)` declares a
  per-entry enter+exit animation (kind: fade / slide-up / slide-down /
  scale). Enter plays on open; on dismiss the exit tween plays and the entry
  is only removed AFTER it. Driver-visible: `/overlays` entries now carry
  `"exiting"` alongside `"live"` (open → live:1/exiting:0; dismiss →
  exiting:1 during the tween; then live:0). The exit tween is REAL on all
  three backends: GTK4 CSS-keyframe enter/-out classes + a `g_timeout`
  deferred detach; **win32 a layered-window alpha fade** (WS_EX_LAYERED + a
  WM_TIMER stepping the content+scrim alpha 255→0 over trans_ms, holding
  exiting:1, then detach — every kind renders as the fade for now, a real
  slide is a follow-up); macOS NSAnimationContext (sibling). `AETHER_UI_NO_
  ANIMATION` collapses exit to instant so suites stay deterministic.
  `overlaytr_demo` + spec adapts to launch mode — anim-off asserts instant
  removal (the parity contract), anim-on asserts exiting:1-then-gone (ci
  Phase 5h2). **Verified anim-on 3/3 on GTK4 (local), win32 (winbaz), macOS
  (spec_matrix 160/160).** Fixed a latent shared-server bug along the way:
  TWO /overlays handlers in one if-else chain (the reachable one missing the
  new field) — deduped.
- **Layout:** ~~weight-share min-clamping~~ **DONE 2026-07-18** — a
  weighted child is pinned to its min and pulled from the weight pool when
  its share would fall under it, iterated to a fixed point. GTK4
  (AeuiFlexLayout) AND win32 (the stack layout now flex-distributes too,
  same clamp) — 2/2 on both, identical widths. `weight()` is real on win32
  now (was a no-op). ~~RTL~~ **DONE 2026-07-18 (all three backends)** —
  `rtl(hstack, 1)` lays children right-to-left (GTK4
  gtk_widget_set_direction / macOS NSStackView / win32 mirror-x); polish
  spec 3/3 on GTK4 AND win32. Fixing win32 RTL surfaced and fixed a
  PRE-EXISTING win32 stack bug: SetParent inserts children at the TOP of
  the sibling Z-order, so GetWindow(GW_CHILD) enumerated them in REVERSE
  creation order — every stack laid its children out backwards (unnoticed
  because most rows are symmetric). Fixed by pushing each new child to
  HWND_BOTTOM. 7/7 layout-sensitive win32 specs pass, no regressions.
- **Bindings:** ~~list-typed state (`each_bind`)~~ **DONE 2026-07-18**
  (`ui_state_list` + `each_bind`; a `ui_set_list` re-runs `each_update`
  via a generic state-observer primitive), ~~two-way binding~~ **DONE**
  (`bind_value`/`textfield_bound`), ~~computed/derived state~~ **DONE**
  (`computed_s` — a void recompute closure fired on input change, seeds
  once). All three via the new `aether_ui_state_on_change` observer +
  `AEUI_STATE_LIST` cell, in all three backends; 5/5 (rbind) + 7/7
  (bindings) on GTK4. ~~`table_bind`~~ **DONE 2026-07-18** (it was already
  landed in the table-delegate batch — this note was stale) — `table_bind(t,
  list_state)` is the each_bind sibling over `table_update`: a `ui_set_list`
  re-runs the table render. `tabledeleg_demo` + `spec_tabledeleg_demo` assert
  it re-renders on list-state change (new rows + delegate re-run, old rows
  cleared); registered in ci.sh + spec_matrix. **The reactivity line is now
  fully closed.** Design notes in docs/design/reactivity-unification.md §5.
- **Focus/shortcuts:** ~~conditional shortcut scopes~~ **DONE 2026-07-18**
  (`shortcut_when(combo, enabled, cb)` — predicate-gated; inert combo
  propagates), ~~chorded shortcuts~~ **DONE** (`shortcut_chord(a, b, cb)`
  — two-key sequence, 1.5s window). Real on ALL THREE (win32 registry
  behind /window/key; macOS event_to_combo + chord machine, commit
  2bd6b5a). ~~per-widget scopes~~ **DONE 2026-07-18** —
  `widget_shortcut(widget, combo, cb)`: a combo active only while that
  widget (or a descendant) has focus, built on shortcut_when + a new
  `focused_widget()` getter (real on all three backends). 2/2 GTK4.
  ~~auto menu↔shortcut binding~~ **DONE** — `menu_item_accel(menu, label,
  accel, cb)` adds the item AND registers shortcut(accel, cb); 3/3. **The
  whole Focus/shortcuts line is now closed.**
- **Menus:** ~~GTK4 native menu wiring (GMenu/GActionGroup) is still the
  recorded stub~~ **DONE 2026-07-18** — the GTK4 menu bar is real
  (GtkPopoverMenuBar over a GMenu model; each item backed by a
  GSimpleAction on the app, registered in on_activate; separators split
  sections). `examples/menu` + `tests/menu/spec_menu` (GET /menus, POST
  /menu/{h}/activate, effect-observed counter) — **4/4 on GTK4 AND
  win32** (macOS expected-parity: same shared side-store + shared server
  routes, unverified this pass). Driver routes added to BOTH servers
  (house rule 5). `menu_popup` (standalone popup of a GMenu model) is now
  real on GTK4 too — `gtk_popover_menu_new_from_model` anchored under the
  widget, headless-no-op like the win32/macOS TrackPopupMenu/
  popUpMenuPositioningItem paths (so no driver assertion — the items stay
  reachable via /menu/{h}/activate). The drawn context menu remains the
  right-click surface.

## 3. Backlog (real, smaller, or gated)

- ~~**File dialogs** (`ui.open_file`/`save_file`)~~ **DONE 2026-07-18** —
  `open_file(title)` / `save_file(title, default_name)`, real on all three
  (GtkFileChooserNative + nested loop / GetOpenFileName+GetSaveFileName /
  NSOpenPanel+NSSavePanel). Headless-no-op (returns ""), so no spec —
  verified headless-safe (no hang) on GTK4 + win32. (gp's xdg-open is a
  file *launcher*, a separate thing, still there.)
- ~~**Tab view** (`ui.tabs`)~~ **DONE** — real tab strip on all three
  backends (GtkStackSwitcher/GtkStack, NSTabView, win32 strip-over-zstack);
  `tabs_demo` 4/4 everywhere. (This backlog note was stale.)
- ~~**Drag & drop** (list reorder)~~ **DONE 2026-07-19** — `listbox_reorderable`
  rows are a native drag source + drop target; a drop fires
  `on_drop(src)` → `listbox_move(src, target)`, rebuilding the list in the
  new order (`on_reorder |from,to|`, `listbox_items` for the live list).
  GTK4 GtkDragSource/GtkDropTarget is real; win32/macOS store the closure
  (native OLE/AppKit drag is the inter-app follow-up). Driver-fire path
  `POST /widget/{row}/drop?src=N` on both servers; `reorder_demo` 5/5 on
  winbaz. Uncovered + fixed a win32 driver bug: the never-shrinking widget
  registry left rebuilt-listbox rows resolvable by text, so a second drag
  reordered by a stale index — now dead subtrees report type "null" (GTK4
  parity via its weak-ref slot nulling). Inter-app drag & drop still TODO.
- ~~**Multi-window**~~ **DONE 2026-07-18** — co-equal top-level windows.
  One event loop owns N windows (`window_create`/`window_set_body`/
  `window_show`/`window_close`); the loop stays alive while ≥1 window is
  live (GTK4 g_application_hold per window / win32 WM_DESTROY last-close
  count / macOS applicationShouldTerminateAfterLastWindowClosed). Driver
  story: `GET /windows`, `"window":N` per widget, `POST /window/{id}/close`
  — on BOTH servers. Cross-window shared state works (state layer is
  window-agnostic). `multiwindow_demo` + spec 5/5 on GTK4 AND win32 (open,
  list, per-widget tag, cross-window bump, close-leaves-primary-live).
  macOS coded (mirrors the pattern) — sibling verify. Design:
  docs/design/multi-window.md. **§3 (per-window overlays/sheets) mostly
  done:** GTK4 overlays were ALREADY per-window (the "~50 sites" was
  overstated); win32 overlays now honour the target window (were hardcoded
  to apps[0]) — verified, an overlay on window 2 carries window:2 on GTK4
  AND win32 (6/6). macOS sheets follow the KEY window, and macOS overlays
  got per-window hosts + safe main-thread close (sibling, spec_matrix
  145/145). Per-window overlays/sheets done on ALL THREE.
  ~~per-window menu bars~~ **DONE 2026-07-19** —
  `attach_menu_bar_to_window(win_handle, bar)`: GTK4 packs a
  GtkPopoverMenuBar in the window's content; win32 SetMenu's the HWND;
  macOS uses the app-global OS bar (best-effort). `winmenu_demo` + spec
  **2/2 on GTK4 AND win32 (winbaz)**. **Multi-window is now complete.**
- **AeCS — Aether Cascading Styles (the swiby-shaped CSS-alike)** — **DONE 2026-07-19**
  (was never on the ranked list; added after a gap-analysis against Swiby's
  banking demo). `create_styles()` + `st_color/st_bg/st_font_size/st_bold/
  st_radius` build a cascading sheet (selectors: `kind`, `class.kind`,
  bare `class` via add_css_class, `container` alias, `root` fallback —
  swiby's resolution order minus ids); `apply_styles(0, s)` walks the LIVE
  widget tree in-process (four tiny ABI getters per backend) and drives the
  existing cross-backend style_* setters. A theme is just a sheet;
  re-theming a running app is one call (the swiby banking Settings-dialog
  move). Driver contract: setters stash last-set colors, widget JSON emits
  `"bg"/"fg"` — specs prove the swap from the backend. `themes_demo`
  (accounts list + Blue/Green themes, values lifted from swiby's
  theme/*.rb, credited) + spec: element rule, class-beats-element,
  container bg, live re-theme — **5/5 on GTK4 AND win32 (winbaz)**.
  **v1.1 (same day)** closed the cheap gaps: multi-class resolution (every
  token), `style_id` + `"#name"` selectors (ride the class store as
  `#`-tokens — zero new state, highest precedence), **widget-level
  `font_family` on all three backends** (GTK4 Pango/CSS, win32 LOGFONT
  face, macOS NSFont; closes the v1 known gap) + `st_font_family`,
  two-valued `st_weight` (re-themes genuinely un-bold — makes the
  documented reset-sheet convention real), `styles_for_mode(light, dark)`
  dark/light pairs, and `fontFamily`/`fontWeight` driver readback — spec
  now **9/9 on GTK4 AND win32 (winbaz)**. Still-open boundaries in
  docs/design/styling.md (no live re-matching/inheritance/pseudo-states —
  those would make AeCS a style engine). Also the front half of any future
  vg-drawn-controls theming (see that doc's "concrete driver").
- **Accessibility** — **semantics layer DONE 2026-07-19**.
  `a11y_role/a11y_label/a11y_description` set REAL platform a11y on any
  widget; the driver reads the effective values back via
  `GET /widget/{id}/a11y` (+ `role`/`a11y_name` in /widgets). Native widgets
  keep their free a11y; these override/supply it (icon-only buttons,
  containers, headings). listbox rows auto-tag `listitem` / the group `list`.
  Backends all real: GTK4 `gtk_accessible_update_property`; win32 MSAA Dynamic
  Annotation (`IAccPropServices` — `+-loleacc -loleaut32`) so Narrator reads
  it; macOS `NSAccessibility` on the NSView. `a11y_demo` + spec assert
  role/name/desc round-trips (auto role+name for free, a11y_label override,
  heading role, listitem rows, /widgets fields). **win32 5/5 on winbaz;
  macOS verified in the sibling batch (spec_matrix 160/160).** GTK4 backend +
  DSL compile clean via aeb; its runtime spec awaits the local display
  recovering (same exit-144 issue), but the readback is side-store based and
  backend-agnostic and green on the other two. Design:
  docs/design/accessibility.md. Screen-reader end-to-end (Narrator/VoiceOver)
  is a manual step outside the harness.
  Still open: anything vg-DRAWN (the dropdown, a plan-B table) needs a
  semantics bridge — the real cost of the drawn path (why Flutter keeps a
  semantics tree). Track it; don't block on it.

## Toolkit-inspired backlog (2026-07-20 survey: SwiftUI/QML/Flutter/Swing/Swiby)

Surveyed after win32 parity. Tier 1 (fits current architecture):

- ~~**QML States on AeCS**~~ **DONE 2026-07-20** — `ui_states/add_state/
  set_state/current_state`: named states per widget/subtree, each an AeCS
  sheet fragment applied via apply_styles(target); declared ui.transition
  properties animate between states; current state driver-visible as an
  `st-<name>` marker class. `states_demo` 3/3 GTK4 AND win32.
- ~~**Undo/Redo (Swing UndoManager)**~~ **DONE 2026-07-20** —
  `undoable(label, do, undo)` + undo/redo/depths/label +
  `enable_undo_shortcuts()` (Ctrl+Z/Ctrl+Shift+Z); shared edit stack in
  system_extras; redo-tail truncation on new edits; driver POST /undo,
  POST /redo, GET /undo_state on both servers (per-platform marshalled
  fires). `undo_demo` 5/5 GTK4 AND win32. Follow-up: text-field
  auto-enrolment (coalesced edits).
- **Widget Inspector (Flutter DevTools)** — a live tree browser over the
  existing AetherUIDriver; itself an aether-ui app. The protocol already
  exists; the inspector is a client.
- ~~**Golden-image tests (Flutter)**~~ **DONE 2026-08-01** (67d3cb6) —
  shipped as `examples/golden_gallery` + `tests/goldens/<backend>/*.sig`,
  blessed on gtk4/win32/macOS and driven by the `golden` suite in every
  spec_matrix run. Landed as per-cell SIGNATURE files rather than PNG
  diffing: text, git-diffable, and no image decoder needed on any
  platform. PNGs still land in `target/golden_out/` for human eyes.
  See TODO.md for the original sketch.
- **NavigationStack × swiby sweb pages** — real push/pop navigation with
  back chrome (the navstack ABI exists as an unexercised stub) + swiby's
  multi-page app shape ($context.goto + session).

Tier 2: semantic color roles for AeCS (Flutter ColorScheme); QML
property-to-property bindings (width: parent.width/2 via on_layout);
Swing TableRowSorter (built-in table sort/filter); QML SpringAnimation
easing for transitions.

Noted, not scheduled: hero/matchedGeometry animations (wants the
retained compositor); Flutter-style hot reload as "hot restart with
state snapshot" (toolchain-adjacent: aeb watch + ui_state serialization).

## The strategic fork (not scheduled): vg-drawn controls

The Flutter turn — a widget set *drawn by vg* — would make every control
pixel-identical across backends, fully themeable, effect-capable, and
immune to compositor politics. The costs are exactly Flutter's costs:
you take on a11y (semantics tree), IME/text input (brutal), and
"platform-feel" drift. SwiftUI's hybrid is the likelier right answer for
a three-backend toolkit: **native text inputs, drawn chrome** — draw
what natives do worst across our platforms (menus, dropdowns, overlays,
maybe tables), keep native what they do best (entries, IME, a11y).
The overlay, typography, and effects layers were deliberately built as
the foundations of that hybrid, so this fork stays open without being
committed to. **The comparison-doc treatment now exists:
`docs/design/vg-drawn-controls.md`** — the four postures (native / full-
drawn / hybrid / selective) scored against this codebase's constraints,
the verdict (**hybrid C, reached by extending today's selective posture —
never full-drawn B; IME and a11y are the disqualifiers for B**), and a
phased ci-gated path if C is ever acted on. It stays NOT scheduled: keep
native the default, promote to C only on a concrete driver (theming need,
or compositor bugs the overlay layer can't absorb).

## House rules (policy, carried forward)

1. **Driver-first widgets:** every new widget/effect ships with its
   `/widgets` JSON representation, driver routes, uidriver helpers, and
   an Aeocha spec in the same commit (the `toggle_group` precedent).
2. **Sommelier is a release gate** for anything compositor-adjacent —
   test on the Chromebook before calling it done; never let a Crostini
   workaround become the default (gate on `$SOMMELIER_VERSION`).
3. **Idle must cost zero** — the static-until-dirty discipline
   (region_set_animated lesson) applies to transitions and overlays too.
4. **Headless parity:** everything renderable must render via
   canvas_write_png so specs can screenshot it (the /screenshot path).
5. **Both-servers rule** (item 8's lesson): driver route changes land in
   the GTK4 embedded server AND backend/aether_ui_test_server.c. There are
   exactly TWO servers again (macOS gave up its private one, 2026-07-14),
   and the shared one now backs both win32 and macOS — so a route added
   there lands on two backends at once, and a route added ONLY to GTK4 is
   a spec that is red on two platforms for no reason. That is precisely
   how split/overlay/canvas/text_extent stayed red on both.
6. **Geometry specs pin the window** (item 7's lesson): GTK4 on a cold
   Xvfb can map windows at natural size — /window/resize first, then
   assert.
