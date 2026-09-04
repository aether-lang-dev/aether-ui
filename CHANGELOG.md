# Changelog

All notable changes to Aether UI are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [current]

### Added

- **A multi-select listbox can be driven to a known state.** `listbox_select`
  on a multi listbox toggles, which can only ever say "flip this row": calling
  it twice silently undoes itself, so an app that selects a row after every add
  ends up with the wrong row selected and there was no way to keep an
  application's own selection model and the widget's in agreement.
  `listbox_set_selected(lb, i, on)` writes one row's state and
  `listbox_clear_selection(lb)` clears them all, neither firing `on_select`,
  because a state write is not a user action and an app syncing its model would
  otherwise re-enter its own click handler on every sync.
  `listbox_selected_count(lb)` saves every caller writing the same loop.

- **`listbox_selection_mode(lb, 1)` gives a multi listbox the selection
  behaviour every editor, file manager and mail client has**: a plain click
  replaces the selection, cmd/ctrl-click toggles one row, shift-click extends
  from the anchor. This could not be built on top of `on_select`, which reports
  a row index and no modifier state, so the widget does it: it already owns
  `sel_flags` and the hit testing. Mode 0, today's toggle-every-click, stays
  the default, because that is what a checklist wants and what `listbox_multi`
  has always done.

- **`modifiers()`** reports the modifier keys held right now as the same
  bitmask `window_on_key` uses (1 shift, 2 ctrl, 4 alt, 8 super/command). Real
  on all three backends. Reading the live state inside a click callback is what
  lets a widget tell a plain click from a cmd-click without every click
  callback growing an argument, which would break every existing caller.


- A `table` announces itself as a table, and its headers as column headers.
  Before this it announced nothing structural: assistive tech saw an unlabelled
  stack of buttons above a list. The ROWS were already right, because `table`
  composes a `listbox` and those rows carry the `listitem` role — which AppKit
  already maps to `NSAccessibilityRowRole` — so the structure was described
  from the row down but never from the table in.
  `table`, `columnheader` and `row` join the role vocabulary on all three
  backends: `GTK_ACCESSIBLE_ROLE_TABLE` / `_COLUMN_HEADER` / `_ROW`,
  `NSAccessibilityTableRole` / `ColumnRole` / `RowRole`, and
  `ROLE_SYSTEM_TABLE` / `_COLUMNHEADER` / `_ROW`. macOS also reads the two new
  ones back, so the driver reports the effective role rather than only the
  requested one.
  `spec_table_demo` gains a case; three of its assertions fail without the
  change.

### Added

- CI fails if any spec drops its `run_summary` verdict. Since aether v0.613.0
  `run_summary` returns the verdict rather than `exit()`-ing, and
  `run_server_test` judges a spec purely by process exit status, so a bare
  `spec.run_summary(fw)` as the last statement makes a failing suite report
  green — silence that looks like success, which is the worst failure mode a
  harness has.
  This was already documented in `ci.yml`, in capitals, directly above the
  pin. The comment did not prevent 83 specs being converted to the bare form
  in a single commit, and nothing in the pipeline could have said so. Hence a
  check rather than a note. Verified by reintroducing the bare form in one
  spec and confirming the guard names that file and line.

### Fixed

- Every spec returns its `run_summary` verdict again. A previous change in this
  series stripped the `return` from 83 of them on the strength of a local
  `~/.aether` stdlib where `run_summary` was void and called `exit(1)` — a
  stale install, not what CI resolves. The repo had already moved to aether
  v0.613.0, where `run_summary` RETURNS its verdict so that cleanup after a run
  is not skipped on exactly the failing runs that need it, and `ci.sh` judges a
  spec purely by the process exit status (`local rc=$?` / `return $rc`). A bare
  call therefore discards the verdict, and the workflow comment says plainly
  what that costs: a failing suite reports green. All 83 now return it,
  including four that were bare before and would have had the same problem.

### Added

- `table` and `tree` assert that their rows are keyboard-navigable. Both
  compose a `listbox` for their body, so both inherited the focusable rows and
  arrow-key navigation listbox gained — a wider effect than that change
  claimed. Nothing pinned it, though: a future refactor moving either off
  listbox would have taken arrow-key navigation with it, and with that the only
  way to reach those rows without a mouse, while every other assertion in both
  suites kept passing. `spec_table_demo` goes 13 to 14 cases and
  `spec_tree_demo` 3 to 4.

### Fixed

- `window_on_key` supports more than one handler. It kept a single closure, so
  every registration replaced the last and a second `window_on_key` silently
  disabled the first. `on_key` is built on it, which means two `on_key` calls
  on different widgets left only the later one working, with no error anywhere
  and nothing in the driver to show it.
  Measured on a demo holding two independent counters, each with its own
  handler: after three keys the previous code reads `a: 0  b: 3` — the first
  handler never ran at all. It now reads `a: 3  b: 3`.
  Handlers fire in registration order, and "handled" still means "at least one
  ran", so the return value means exactly what it did before.
  Found while giving the listbox keyboard navigation: a handler registered per
  row left only the final row's arrows alive, which is why that feature has to
  register once for the whole list.

### Notes

- `examples/multikey_demo` and `tests/multikey_demo/` (3 assertions, CI Phase
  5e18, spec-matrix suite `multikey`). Verified against every key-driven suite
  — listbox, keyhandler, shortcut, wshortcut, canvasscroll, navstack, overlay,
  40 assertions — since this changes the dispatch path all of them share.
- Three more stale platform claims, found by re-running the sweep from the
  earlier round with looser phrasing — the first regex missed all three because
  none of them says "only" or "no-op" in the shape it matched.
  - `canvas_render_range_rgba` said it is unsupported on "win32/macOS today".
    macOS has had a real `CGBitmapContext` path (which un-premultiplies on the
    way out, since this contract is non-premultiplied) for some time; Win32 is
    the only backend that returns 0.
  - `tray_set_icon_template` said "No-op on Linux/Win". Every backend routes to
    the same shared tray registry, so the flag is recorded — and driver-visible
    — everywhere. What is macOS-only is applying it, since an `NSStatusItem`
    renders a template image mono and adaptive while SNI and `Shell_NotifyIcon`
    have no equivalent.
  - `notify_request_permission` was described as a "macOS-only permission
    prompt". Nothing prompts on any backend: macOS delivers through
    `NSUserNotification`, which needs no runtime grant, and all three return
    granted. The comment read as though a dialog appears on macOS, which it
    does not and never did on this path.
  - `listbox_reorderable` said "a native drag gesture (GTK4 drag source/target)
    calls the same move on drop", which reads as how the gesture is implemented
    rather than where it exists. The gesture is GTK4-only: AppKit and Win32
    record the drop closure but start no drag, and the only thing that fires it
    there is the driver's route, which supplies the source index itself. So a
    user can drag to reorder on Linux and nowhere else; `listbox_move` is the
    portable path.
### Removed

- `aether_ui_animate_opacity_impl`, a second animation API that nothing used
  and that snapped instead of animating on GTK4. It was declared in the backend
  header and as a DSL extern, implemented three times, and called from
  precisely nowhere — no DSL verb wrapped it, and no example, app or test
  referenced it. GTK4's implementation set the opacity immediately, under a
  comment saying so ("For simplicity, set immediately"), so anyone who did find
  it in the header and use it would have got an instant jump on Linux and a
  real fade on the other two.
  Animated opacity already has one working path — `transition()` plus
  `style_opacity()`, which is CSS on GTK4 and a real tween on macOS and Win32,
  and is what `transitions_demo`, `overlaytr_demo` and `states_demo` exercise.
  Keeping a second, half-implemented one alongside it is the "two of a thing"
  case, and the half that was broken was the one on the primary platform.
  Win32's `Animation` struct, its array and its `anim_tick` timer proc went
  with it: they existed solely to serve this entry point.

### Added

- Listbox rows are keyboard-navigable. Up/Down move the selection, Home/End
  jump to the ends, and all four clamp rather than wrap. Selection moves
  through the same path a click takes, so `on_select` fires and
  `.aui-row-selected` moves exactly as it would for a mouse; there is one
  selection path, not two that can disagree.
  This closes a real accessibility gap rather than adding a convenience: a
  listbox already gave its rows the `listitem` role, so assistive tech was
  told "this is a list" about something that could not be reached without a
  mouse. It is also the concrete thing native `NSTableView`/`GtkListView`
  backing would have provided for free (see #8) — the a11y roles already map
  natively on both, and `vlist` already recycles, so keyboard navigation was
  the substantive remainder.
- `set_focusable(handle, on)` on all three backends, the prerequisite. Native
  controls decide focus by their own rules, but a container accepts none by
  default anywhere: `gtk_widget_set_focusable` on GTK4, `WS_TABSTOP` on Win32,
  and on macOS an `NSStackView` subclass whose `acceptsFirstResponder` answers
  from a flag — `NSView` returns NO and offers no setter, so a plain container
  could never be focused however hard `focus()` tried. The flag defaults to
  off, so an unmarked stack behaves exactly as before.

### Notes

- The listbox registers ONE key handler for the whole list rather than one per
  row. `window_on_key` keeps a single closure and each registration replaces
  the last, so a per-row `on_key` leaves only the final row's handler alive and
  arrows do nothing anywhere else. Worth knowing more generally: two `on_key`
  calls on different widgets silently disable the first.

### Fixed

- CI builds again. `9c84d786` converted all 109 build nodes to aeb Shape A
  (`bldr.build() {}`, b-free), but the pipeline pins `AEB_REF` to v0.282, which
  has no `bldr` module at all: every node failed type-checking with
  `Undefined function 'bldr.build'`, the fan-out scheduled nothing, and main
  went red for four consecutive commits. The pin moves to v0.283, the first
  release carrying `bldr`.
  It is a strict move forward: v0.282 was pinned for b1bfa5e (encode_name
  ae-escapes a dot-prefixed fan-out root, so `.all.ae` links), which v0.283
  also carries. The `AETHER_REF` pin is untouched, because `bldr` is aeb's
  module and the ae compiler needed no change to type-check against it -
  verified rather than assumed: with this repo's existing v0.553.0 and aeb
  v0.283 the whole fan-out builds, exit 0 with 0 failed nodes, while the same
  ae against v0.282 fails every node.
- The macOS leg makes Homebrew's libraries linkable. Getting past the type
  error above exposed the next one: `ld: library 'ssl' not found`. macOS
  searches `/usr/lib` and not `/opt/homebrew/lib`, and `openssl@3` is keg-only
  besides, while Linux finds these in system paths. It became load-bearing with
  the same conversion, because `ae cflags --libs` emits
  `-lssl -lcrypto -lz -lnghttp2 -lpcre2-8` for libaether.a's transitive deps
  and the `bldr` module links the runtime into the fan-out ORCHESTRATOR, where
  the old `build` module did not.
- A `textarea`'s placeholder is honoured on macOS and GTK4, not Win32 alone.
  macOS discarded the argument with a bare `(void)placeholder` and GTK4 never
  referenced it, so hint text in a multi-line box worked only on the one
  platform with no CI. Neither toolkit offers the property the field versions
  use -- `placeholderString` belongs to `NSTextField` and `placeholder-text` to
  `GtkEntry`, while a textarea is a text VIEW -- so macOS draws the hint from a
  `NSTextView` subclass when the buffer is empty, and GTK4 overlays a dimmed
  label on the view, kept in sync from the buffer's own `changed` signal so it
  is right however the text arrives.

### Added

- `/widgets` reports a `placeholder` field. Nothing exposed one before, which
  is why the gap above could not be caught: the argument was accepted by every
  backend and honoured by one, and no test could tell. It covers `textfield`
  and `securefield` too, which already worked and were equally unobserved.

### Notes

- `examples/placeholder_demo` and `tests/placeholder_demo/` (3 assertions, CI
  Phase 5e17, spec-matrix suite `placeholder`). The textarea case is the
  regression test: on the previous macOS code it reports absent while the other
  two still report their text, which isolates the gap to the textarea rather
  than to the new readback.

### Fixed

- Win32 stroked canvas text honours its font family. The `CV_STROKE_TEXT` draw
  already called `gdip_resolve_family(cmd->font_family)`; it was the command
  builder that never filled the field in, discarding the argument with a bare
  `(void)font_family`. Filled text, whose builder sits ten lines above and does
  store it, honoured the requested face, so the same string rendered in two
  different typefaces depending on whether it was filled or stroked. The
  existing cleanup already frees the field for both command kinds, so nothing
  else changes.

### Fixed

- macOS honours SVG `spreadMethod` on gradients. `extend` (0=pad, 1=reflect,
  2=repeat) was dropped with a "not yet honored" note, so every gradient
  rendered as pad while GTK4 (`CAIRO_EXTEND_*`) and Win32 both honoured it.
  Measured through the driver on a red-to-blue band spanning x=0..50 of a
  200-wide canvas with repeat: x=55, 75, 105 and 150 all read `0x0000FF`, flat
  blue to the edge. They now cycle back through red every 50px.
  CoreGraphics genuinely has no reflect or repeat -- `CGGradient` offers pad
  and nothing else -- so rather than ask the gradient to tile, the stops are
  pre-tiled: a stop list covering `reps` copies either side is built in
  gradient-parameter space and the axis (or radius) is stretched by the same
  factor, so tile k lands where the k-th repeat belongs. Reflect mirrors odd
  tiles. `reps` comes from the clip bounding box projected onto the gradient
  axis, so it covers exactly what is visible, and is clamped because a
  degenerate axis would otherwise ask for an enormous stop array.
  Offsets are clamped and kept non-decreasing: `CGGradient` requires a
  monotonic location array and misdraws silently otherwise.

### Notes

- `examples/gradspread_demo` and `tests/gradspread_demo/` (5 assertions, CI
  Phase 5e16, spec-matrix suite `gradspread`) cover it. Asserted by channel
  comparison rather than exact colours, because the value at a sample depends
  on antialiasing and per-backend rounding and pinning one would test the
  rasteriser instead of the spread. Two of the three cases fail on the previous
  code.

### Fixed

- macOS paints `style_hover` and `style_active` instead of only recording them.
  The colour went into an associated object that nothing but the driver's own
  readback ever consulted; no draw path used it, so a control styled with
  `style_hover` still "read as dead" on this backend, which is the exact thing
  that verb's own comment says it exists to prevent. GTK4 does it with a
  `:hover` CSS rule and Win32 in its owner-draw path; macOS now applies the
  colour to the view's layer, with active beating hover and the base
  `style_bg_color` restored when neither applies.
  Both routes go through one helper so they cannot drift: the driver's
  simulated hover/press, and a real `NSTrackingArea` armed the first time a
  state style is set. The clear paths repaint too, which the first cut missed,
  leaving the last hovered widget wearing its hover colour permanently.
- The driver's `bg` readback answers for the state the widget is actually in,
  on both backends. macOS reads the layer that paints (extending the rule the
  scroll-view arm already followed); GTK4 reconstructs it from the widget's
  real state flags, because the `:hover` / `:active` CSS genuinely paints there
  but GTK exposes no getter for the background a widget currently renders. The
  colours are recorded from the same values the CSS is generated from, so the
  two agree by construction. Without this the readback reported the resting
  colour for a widget that was visibly in another state. Deliberately narrow: reading the layer for every view
  changes what the field MEANS, from "the background that was set" to "whatever
  the layer happens to paint", and made the injected banner start reporting a
  colour it never asked for.

### Notes

- `examples/hoverpaint_demo` and `tests/hoverpaint_demo/` (5 assertions, CI
  Phase 5e15, spec-matrix suite `hoverpaint`) cover it, with three distinct
  colours so each state is identified by WHICH colour rather than by "it
  changed". The round trip is asserted, not just the states: releasing must
  fall back to hover rather than base while the pointer is still over the
  widget, and leaving must restore the base. Both were wrong in the first cut
  and only the round trip caught them.

### Fixed

- macOS honours group opacity. `canvas_group_begin` / `canvas_group_end` were
  two empty stubs there, so an SVG `<g opacity="0.5">` painted **fully opaque**:
  measured through the driver, a 0.5 group over white read `0xFFFF0000` instead
  of `0xFFFF7F7F`. This is the same defect the Win32 backend's own comment
  records for `mememe.svg`, fixed there and left standing here. macOS now
  composites the group into one `CGContextBeginTransparencyLayer` and paints it
  once at the group alpha, matching GTK4's `cairo_push_group` +
  `cairo_paint_with_alpha`.
  CoreGraphics takes a transparency layer's alpha from the gstate **at begin**,
  while the alpha arrives with the end command, so the replay looks ahead for
  the matching end and sets it first. The look-ahead is depth-counted: a nested
  group's end must not be mistaken for the outer one's.

### Notes

- `examples/groupalpha_demo` and `tests/groupalpha_demo/` (4 assertions, CI
  Phase 5e14, spec-matrix suite `groupalpha`) cover it, with two squares
  overlapping by half inside a 0.5 group. Two assertions because there are two
  distinct wrong answers: dropping the alpha (the stub, which fails the first)
  and applying it per child, which leaves the overlap darker than either square
  and fails the second. The second passes trivially on the old code, which is
  exactly why both are there.

### Fixed

- `canvas_clip_rect` clips on Win32. It was a silent no-op there: the impl
  discarded all five arguments and the paint replay had no case for the
  command at all, while macOS and GTK4 both push it and honour it. Since
  `vg/live.ae` emits one at scene-flush start to enforce the SVG viewport's
  `overflow:hidden`, **every AeVG live scene drew past its viewport on Windows
  and nowhere else**. Both Win32 replay paths now honour it: `IntersectClipRect`
  on the GDI path, `GdipSetClipRectI` with CombineMode Intersect on the GDI+
  one, each matching the documented semantic of narrowing the current clip for
  the rest of the frame. Both also reset the clip when a replay starts, because
  the `read_pixel` path reuses one cached DC across calls and a clip left by
  the previous replay would shrink the visible area a little more every frame.

### Notes

- The clip primitive now has a test that asserts clipping. It had none:
  `vg/test/test_canvas_clip.ae` wrote a PNG, checked the write returned 1 and
  told a human to "inspect" it, despite a header comment claiming it asserted a
  pixel inside the clip and one outside; and it was in no CI run list, so it
  never ran. It is replaced by `examples/canvasclip_demo` plus
  `tests/canvasclip_demo/` (5 assertions, CI Phase 5e13, spec-matrix suite
  `canvasclip`), which reads real pixels through `GET /canvas/{id}/pixel` and
  runs on both backends CI has.
  Scope stated plainly: there is no Windows CI, so that spec does not prove the
  Win32 fix. It pins the contract the fix implements and stops macOS and GTK4
  regressing. It asserts "inside is red, outside is NOT red" rather than an
  exact background value, because the offscreen replay's background differs per
  backend and pinning one would test the wash instead of the clip.

### Fixed

- Six stale platform-support claims corrected. Each said a verb was GTK4-only
  or a no-op on macOS/Win32 when all three backends had implemented it, in
  some cases years of commits ago. These are not cosmetic: a comment saying
  `weight()` does nothing off GTK4 sends an app to build a workaround for a
  feature CI already proves works.
  - `weight()` said "GTK4 only; no-op on win32/macOS". Real on all three, and
    `spec_weightclamp_demo` asserts the min-clamp on macOS and Linux in CI.
  - `canvas_on_key` said "macos/win32 stubs". Real via `-keyDown:` and
    `WM_KEYDOWN`.
  - `canvas_on_release` said "no-op on backends without a release bridge yet".
    Real via `-mouseUp:` and `WM_LBUTTONUP`. `spec_rubiks_cube` and
    `spec_frames_demo` drive both paths in CI.
  - `context_menu_item` said "AppKit/Win32 are no-op stubs". macOS hands the
    items to `-[NSView setMenu:]`, Win32 pops a `TrackPopupMenu` from
    `WM_CONTEXTMENU`; `spec_context_menu` covers it.
  - `add_css_class` said "GTK-only, no-op stubs elsewhere", which reads as
    "does nothing". The class list is tracked on all three (it is how the
    driver reports selection state and how `.aui-row-selected` works
    everywhere); what is GTK4-only is applying STYLE from it, since the other
    two have no stylesheet engine.
  - `aether_ui_win32.c` carried "not yet implemented on Win32... No-op stub"
    directly above a later note saying the opposite. The stale sentence had
    been left in place rather than replaced.
  `on_layout`'s comment already records this exact rot happening once before,
  which is why it now states which backend does what rather than naming one.

### Added

- `canvas_on_scroll` is wired on macOS and Win32, not GTK4 only. It was an
  honest no-op elsewhere, documented as such at the DSL, but the consequence
  was that a shipped app's scroll-to-zoom (`apps/turtle`) did nothing on two of
  the three platforms, and the boxed closure handed in was discarded and
  leaked. AppKit gets a real `-scrollWheel:` (precise deltas when the device
  reports them, coarse `deltaY` otherwise), Win32 a `WM_MOUSEWHEEL` in the
  canvas proc.
  **Each backend converts to the DSL's convention rather than passing its
  platform's own sign through**: `dy < 0` is "away from the user", the
  conventional zoom-IN direction. AppKit's `scrollingDeltaY` is the opposite
  sign and flips again under natural scrolling, which
  `-isDirectionInvertedFromDevice` reports; Win32's wheel delta is the opposite
  sign and arrives in multiples of `WHEEL_DELTA`, so it is divided into notches
  rather than handing an app a number 120x larger on Windows alone. Getting
  either wrong never fails loudly, it silently inverts or over-scales zoom, so
  both are written out longhand.
- Driver route `POST /canvas/{id}/scroll?dx=&dy=`, on all three backends, so
  the above is testable at all. A canvas with no handler answers 404
  ("unwired") rather than 200, matching the other canvas verbs.

### Notes

- `examples/canvasscroll_demo` and `tests/canvasscroll_demo/` (5 cases, CI
  Phase 5e12, spec-matrix suite `canvasscroll`) cover it. The spec asserts the
  SIGN, not merely that something fired. It states plainly what it cannot
  prove: the route fires the closure the backend stored, so it shows the
  closure is kept and the app sees the documented convention, but nothing
  generates a real `-scrollWheel:` or `WM_MOUSEWHEEL`. That is the driver's
  standing blindness for input paths, which is why the conversion arithmetic
  is spelled out in each backend instead of folded into the call.

### Fixed

- GTK4's `timer_cancel` is idempotent, like the other two backends. It handed
  the raw `GSource` id back as the timer handle and passed it straight to
  `g_source_remove`, so cancelling twice, or cancelling a handle whose timer is
  already gone, raised `GLib-CRITICAL: Source ID N was not found when
  attempting to remove it`, which is fatal under `G_DEBUG=fatal-criticals`. The
  same call is a harmless no-op on macOS, which invalidates an already-nil
  `NSTimer`, and on Win32, which finds no live entry in the table it keeps.
  GTK4 now keeps that table too, so a handle names an entry of ours rather than
  a GLib source directly.
  Two things this is NOT, both measured rather than assumed, so the next reader
  need not re-derive them: GLib does not recycle source ids, so a stale handle
  could not have named someone else's source; and removing a source from inside
  its own dispatch, which `canvas_on_hover`'s watchdog does, raises nothing.

### Notes

- The timer verbs had no spec coverage. `examples/timer_demo` and
  `tests/timer_demo/spec_timer_demo.ae` (4 cases, CI Phase 5e10, spec-matrix
  suite `timer`) now cover them, and that phase runs with
  `G_DEBUG=fatal-criticals` so GLib misuse is a failure a spec can see rather
  than a line in a log: nothing the driver reports changes when a CRITICAL is
  raised, so without it the bug above is invisible from out here.
- A survey of the DSL surface found 96 of 382 public verbs are never called
  from any example, app or spec. Some are thin wrappers over ABI the C suite
  does cover, so it is an upper bound, but it is the same gap the sheet and
  timer defects were sitting in.

### Fixed

- Win32 gradient stops clamp all four colour components, not just alpha. Red,
  green and blue were built from the same float input and shifted into their
  own bytes unclamped, so a stop component outside `[0,1]` spilled into the
  neighbouring channel instead of saturating. The sibling helper that packs a
  solid colour had always clamped all four.

### Notes

- The Win32 cross-compile gate now runs at `-Wall -Werror`. It deliberately ran
  at the default level while there was a backlog of pre-existing warnings,
  because failing someone's change for six warnings it did not introduce
  teaches people to ignore the gate. The backlog is zero, so the ratchet
  closes: this is the one backend nobody here can run, and the compiler is the
  only reader it gets. It paid for itself immediately, since
  `-Wmisleading-indentation` is what surfaced the unclamped gradient stops
  above.
- GTK4 honours `AETHER_UI_HEADLESS` when presenting a sheet. It called
  `gtk_window_present` unconditionally, so a headless run put a real sheet on
  the desktop, on the backend CI uses for Linux, against a contract
  `tests/test_widgets.c` states in its own header and names `sheet_present` in.
  It now realizes without mapping, exactly as `window_show_impl` forty lines up
  already did. macOS already guarded it and Win32 got it for free by
  delegating to `window_show_impl`.
- Dismissing a sheet unregisters the widgets inside it. No backend did, so a
  dismissed body stayed listed in `/widgets` forever; on GTK4 the registry
  holds raw, unreffed pointers and destroying the window finalizes its
  children, so those entries dangled rather than merely leaked. On macOS the
  unregister happens in `sheet_dismiss` itself rather than only from
  `windowWillClose:`, because `-endSheet:` ends the modal session without
  closing the window and sends no willClose. That is the path taken whenever
  the app is not headless, so a headless-only test never reaches it.
- macOS stops registering a dead widget for every sheet created. The sheet's
  own `contentView` was registered and the handle thrown away
  (`register_widget_typed(...) * 0 + idx`) under a comment saying callers could
  reference it by handle. They never could: the caller gets the sheet index,
  `sheet_set_body` replaces that `contentView` moments later, and the type it
  was tagged with has no entry in the name table, so the orphan reported as a
  plain `"widget"`. The two enum values left unused by its removal are gone
  with it.

### Notes

- The sheet verbs had no coverage at all, in C or over the driver. They now
  have both: `examples/sheet_demo` with `tests/sheet_demo/spec_sheet_demo.ae`
  (5 cases, CI Phase 5e10 and a `sheet` suite in the spec matrix), and a
  `sheets` case in the C suite, which takes it from 55 to 61 assertions. Four
  of the five spec assertions fail on the previous code.

### Fixed

- Widget removal now releases the per-handle state that lives outside the core
  registry arrays. On macOS a retired `draggable()` widget kept its drag payload
  string; on Windows a retired tinted `image()` or `file_icon()` kept its base
  bitmap and icon. Handles are never recycled, so nothing inherited the stale
  value, but nothing freed it either: every list rebuild (`each_update`,
  `vlist`, `listbox_update`) retires its rows, so a list that repopulates leaked
  once per row per rebuild. On Windows those were GDI objects, which a process
  has a hard quota of, so a long-lived tinted list could eventually stop drawing.
  GTK4 was already correct here, it attaches the same state with
  `g_object_set_data_full` and the widget's own finalize releases it.
- An opacity tween in flight now ends when its widget is removed. On macOS the
  tween object stayed in the handle-keyed table forever, so an animated row
  leaked one per rebuild; the weak view reference meant it stopped animating,
  but nothing dropped the entry. On Windows the tween runs on a thread timer
  rather than a window timer, so destroying the window did not stop it and it
  kept waking at 60Hz against a dead window until its duration elapsed.
- Closing an overlay now unregisters its widget subtree instead of only
  detaching it, on all three backends. Every open/close cycle previously
  stranded the whole card in the widget registry: the census grew by one entry
  per widget per cycle and never fell, and because the click table still held
  the dead card's callbacks the driver could click a button inside a dialog
  that no longer existed. Those callbacks capture the app's overlay-handle
  variable, which by then names a different, live overlay, so clicking a
  destroyed card's Dismiss dismissed the open one.
  On GTK4 this was worse than a leak. The registry holds raw, unreffed
  `GtkWidget*`, and `gtk_overlay_remove_overlay` drops the last reference, so
  the stale entries were dangling pointers rather than retained widgets.
  On Windows the scrim was destroyed without being marked dead first, the order
  the same file's other removal paths document as mandatory, and the content
  window was never destroyed at all, only hidden and reparented onto the
  holder, so every dialog ever opened stayed alive as a real window.
  `spec_overlay_demo` gained two cases covering it; they fail on the previous
  code and pass now.
- Closing a window now unregisters the widgets inside it, on all three
  backends. Each backend flipped the window's own `live` flag and stopped
  there, so every open/close cycle stranded the window's whole widget subtree:
  the census never fell, and a label inside a closed window still answered
  `/widget/{id}` with `"visible":true`. Measured on `multiwindow_demo`, the
  registry went 11, 15, 19 over three cycles and accumulated a stale label
  each time; it now returns to its baseline after every close.
  On GTK4 this was worse than a leak, for the same reason as the overlay case:
  the registry holds raw, unreffed `GtkWidget*` and destroying a window
  finalizes its children, so the stale entries were dangling pointers. The
  unregister runs from the `destroy` handler, which covers a close from the
  title bar as well as `window_close`.
- macOS reports an open secondary window as open. It had no window delegate, so
  it inferred the answer from `-isVisible`, and the headless contract is that a
  window is never ordered onto the desktop: a window `window_show` had opened
  read as closed, and `/windows` said `live:false` for it while GTK4 and Win32,
  which both keep an explicit flag, said true for the same program. The
  existing spec asserting `live:false` after a close was therefore passing on
  an answer that had been false all along. macOS now keeps the same explicit
  flag, cleared from `windowWillClose:` so a title-bar close is observed too.
  `spec_multiwindow_demo` gained two cases; both fail on the previous code.

### Added

- **macOS desktop notifications now post for real — `NSUserNotification`**
  (`aether_ui_macos.m`). The mac `notify` / `notify_full` were registry-only
  stubs; they now deliver an `NSUserNotification` via
  `NSUserNotificationCenter`, matching the gtk4/libnotify path (register
  first, then show). `NSUserNotification` is used rather than the modern
  `UNUserNotificationCenter` because the latter requires a signed `.app`
  bundle, which the aether-ui CLI apps are not; it is the peer of
  win32/toast + gtk4/libnotify. A shared `NSUserNotificationCenterDelegate`
  — held in a static strong ref because the center's `delegate` is weak —
  forces presentation while the app is frontmost and routes activation to
  `aether_ui_notif_emit_click` (resolving the registry id carried in
  `userInfo`), the same dispatch the AetherUIDriver `/notifications/{id}/click`
  route uses. `tag` maps to the notification `identifier` so a same-tag
  notification replaces the previous one; `icon_path` becomes the
  `contentImage`. Wrapped in a `-Wdeprecated-declarations` pragma so the
  `-Werror` build stays green (NSUserNotification is deprecated since 10.14
  but still functions un-bundled). Native macOS tray (`NSStatusItem`) remains
  the next TODO; verified on macOS CI (the `.m` cannot build on Linux).

- **Linux system tray now renders for real — StatusNotifierItem + DBusMenu over GDBus** (`aether_ui_sni.{h,c}` (new), `aether_ui_system_extras.{h,c}` (menu-item enumeration helpers), `aether_ui_gtk4.c` (tray impl forwards to SNI, `app_run_headless` runs `g_main_loop_run`), `aether_ui_macos.m` / `aether_ui_win32.c` (own `app_run_headless` that parks until killed; native tray remains TODO on those backends), `build.sh`, `tests/test_sni_dbus.sh` (new — 9 assertions)). The GTK4 tray is no longer "registry-only with a TODO comment" — it claims a per-process bus name `org.kde.StatusNotifierItem-<pid>-<n>`, exports the `org.kde.StatusNotifierItem` and `com.canonical.dbusmenu` interfaces on `/StatusNotifierItem` and `/MenuBar` respectively, and `RegisterStatusNotifierItem`s against `org.kde.StatusNotifierWatcher`. The icon appears in the desktop's status area (GNOME with `gnome-shell-extension-appindicator`, KDE Plasma, XFCE+`xfce4-sntray-plugin`, Cinnamon, Budgie — every modern Linux DE that speaks SNI). The path forward called out in `system-tray-status-icon-needed.md` is now the implementation; the GTK3 libayatana-appindicator dead-end is not a dependency. **Distro-agnostic**: the only library dep is GIO (part of glib2 — `libglib2.0-dev` on Debian/Ubuntu, `glib2-devel` on Fedora/RHEL, `glib2` on Arch, etc.), which is already a transitive dep of GTK4 wherever aether-ui builds. Wayland-clean (the GTK3 appindicator path has Wayland edge cases; SNI is pure D-Bus and display-server agnostic). When the StatusNotifierWatcher is absent (minimal CI images, headless server with no DE), `RegisterStatusNotifierItem` fails gracefully and the existing registry-only path keeps the AetherUIDriver routes working — `tests/test_tray_notif_driver.sh` (19 assertions, headless mode) still passes either way. Wiring: `aether_ui_tray_create_impl` calls into SNI when not headless; `tray_set_tooltip` / `_set_menu` / `_set_icon_for_state` emit `NewToolTip` / `LayoutUpdated` / `NewIcon` so the host re-reads. SNI `Activate`/`SecondaryActivate` and DBusMenu `Event/clicked` bottom out in the same `aether_ui_tray_emit_click` / `_menu_activate` dispatch helpers as the AetherUIDriver routes — single source of truth, two delivery channels.

- **System-tray surface — `tray_icon` / `tray_set_tooltip` / `tray_set_menu` / `tray_set_icon_for_state` / `tray_set_icon_template` / `tray_seal`, plus `app_run_headless`** (`aether_ui.ae`, `aether_ui_backend.h`, `aether_ui_system_extras.{h,c}`, `aether_ui_gtk4.c`, `aether_ui_macos.m`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `example_tray.ae`, `tests/test_tray_notif_driver.sh`). Phase 1 of `system-tray-status-icon-needed.md` lands the cross-backend DSL surface, a shared registry that owns each tray record (name, tooltip, menu_handle, reactive icon state, left-click closure, sealed flag), and AetherUIDriver routes `GET /tray`, `GET /tray/{id}`, `GET /tray/{id}/icon`, `POST /tray/{id}/click`, `POST /tray/{id}/menu/activate?label=…`, `POST /tray/{id}/set_tooltip?v=…`. The driver routes go through both the per-backend GTK4 server and the shared `aether_ui_test_server.c` (used by Win32) so the same HTTP surface works on Linux and Windows today. Real native tray wiring is left as a per-backend TODO with rationale: libayatana-appindicator's only available variant on Debian/Ubuntu is GTK3-linked, which conflicts with GTK4 at the `#include` level (same trap as the existing menu stubs); a StatusNotifierItem-over-GDBus implementation is the GTK4-friendly path forward. macOS and Win32 backends share the same registry-only shape; their real implementations need NSStatusItem and Shell_NotifyIcon respectively. Phase 1 unblocks AvnSync v2's DSL-side wiring + CI tests against the AetherUIDriver. `tests/test_tray_notif_driver.sh` is 19 assertions covering record introspection, click → callback → side-effect, menu-item activate → state transition → icon swap, unknown-item 404, tooltip update.

- **Desktop notifications — `notify(title, body)` and `notify_full(title, body, icon, tag) callback { … }`** (same files as the tray entry, plus `AEUI_HAVE_LIBNOTIFY` in `build.sh`). Phase 1 of `desktop-notifications-needed.md`. On Linux the GTK4 backend wires real `libnotify` (NotifyNotification with `"default"` action callback for click, `"closed"` signal for dismiss) when `pkg-config --exists libnotify`; without the lib, notifications still land in the registry so AetherUIDriver tests pass. macOS and Win32 ship registry-only stubs (TODOs reference UNUserNotificationCenter and `Windows.UI.Notifications.ToastNotificationManager`). Driver routes `GET /notifications`, `POST /notifications/{id}/click`, `POST /notifications/{id}/dismiss`. Tag-replace semantics matching libnotify `replaces_id` / UN identifier / Toast `tag+group` — passing the same `tag` to a follow-up `notify_full` reuses the slot. `notify_request_permission()` returns 1 on Linux/Win (no-op) and is wired through the same path on macOS for a future UNUserNotificationCenter implementation.

### Fixed

- **`clear_children` / `remove_child` now really remove, on every backend, and the removed widgets leave the AetherUIDriver registry** (`aether_ui_gtk4.c`, `aether_ui_win32.c`, `examples/rebuild_demo/` (new), `tests/rebuild_demo/spec_rebuild_demo.ae` (new, 4 cases), `ci.sh` Phase 5e2, `tests/spec_matrix.sh` suite `clearchildren`). Closes #1. Two separate defects, both invisible to a status-code-only test because every call answered 200. GTK4 also lost the cell entirely when `grid_place` reparented it: `gtk_widget_unparent` drops the parent's reference, which is normally the last one, so the child was finalized before `gtk_grid_attach` ran (`Gtk-CRITICAL: assertion GTK_IS_WIDGET (child) failed`, three silently missing cells). It now holds a reference across the reparent, the same idiom the menubar reparent already used. Removal on GTK4 also unregisters the widget subtree explicitly rather than waiting for the `g_object_weak_ref` planted at registration: a removed widget is not necessarily finalized (GTK can still hold a reference), and until it is, the driver kept listing it and kept firing its callback from a stale id. GTK4 handled a `GtkBox` parent and nothing else: a grid, flow box, scrolled window, overlay or paned child was never unparented, so the cell stayed on screen AND stayed in the registry (the slot is cleared by the widget's weak ref on finalize, which cannot run while the parent still holds the last reference). It now dispatches on the container type, mirroring every arm of `aether_ui_widget_add_child_ctx` plus `grid_place`, and falls back to `gtk_widget_unparent`. Win32 `remove_child` called `DestroyWindow` without `mark_subtree_dead`, the flag the driver actually reads: since the registry never shrinks and Windows recycles `HWND` values, the removed widget kept reporting a live type and `POST /widget/<id>/click` kept firing its callback. `clear_children` and `navstack_pop` had already been fixed this way; `remove_child` was the one left. macOS was already correct via `unregister_view_tree` and is now gated by the same spec. The new example rebuilds a grid (not a box) and a stack (a box) so all three backends exercise both shapes, and the spec asserts all three halves of removal: gone from `/widgets`, stale id refused with 404, and no callback fired.

- **The documented build path works again: `build.sh` links the shared driver on Linux, says which file it could not find, and builds the C suites** (`build.sh`, `README.md`, `ci.sh` Phase 1c, `tests/test_widgets.c`, `benchmarks/bench_widgets.c`, every `examples/*/*.ae` and `apps/*/*.ae` header comment). Closes #24. Four things were broken for anyone following the README. (1) `build.sh`'s Linux/FreeBSD branch never linked `backend/aether_ui_test_server.c`, which the GTK4 backend now calls into since the driver was unified; the aeb path links it, `build.sh` did not, so `./build.sh <example>` on Linux produced a `.c` and then died at link with undefined `aether_ui_test_server_*`, which is exactly the "there is a counter.c, no exe" in the report. (2) The quick start told you to build `example_counter.ae`, a flat filename that has not existed since every example moved into its own directory; a missing source now names the path it tried and, when a same-named example or app exists, prints the real command. (3) The README's first code sample called `aether_ui.` (the module is `ui`) and used `root_vstack` inside a `window` block, which the README's own Surfaces section says renders blank; the sample is now the one from `examples/counter` and compiles as written. Every `example_*.ae` path in the README, the example table, the architecture table, and each example's own header comment now points at a file that exists, and every relative link in the README resolves. (4) `tests/test_widgets.c` and `benchmarks/bench_widgets.c` had not compiled in a long time (a stale `../aether_ui_backend.h` from before the `backend/` move, plus a six-argument call to the eight-argument `canvas_stroke` ABI), because nothing in the repo built them. `build.sh` now accepts a `.c`/`.m` source and links it against the platform backend like any app, and `ci.sh` Phase 1c builds and runs the widget suite (47 assertions, headless) so it cannot rot unnoticed again. Also drops a dead `--lib aevg` case from `build.sh`, left behind when the AeVG tree became `vg/`.

- **`/overlays` reports `exit_played`, a sticky record that an exit tween ran, so the transition spec stops being a race** (`backend/aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `tests/overlaytr_demo/spec_overlaytr_demo.ae`). `exiting` is true only WHILE the tween plays, 200ms in the demo, and the spec sampled it by polling. When the first poll lands after the tween has finished the fact is gone for good, and no amount of further polling recovers it, so the phase failed on a loaded macOS runner while passing everywhere else. Measured directly: immediately after Close the entry reads `live:1, exiting:1`; 300ms later it reads `live:0, exiting:0` and there is nothing left to observe. `exit_played` is set beside `exiting = 1` and never cleared, which works because the overlay table is never compacted (closed entries stay listed with `live:0`), so the spec can now ask the question it actually means, did the exit animate before removal, at any time after the click. The animations-off contract gained an assertion rather than losing one: `exit_played` must be 0 there, since no tween ever started. GTK4 initialises the new field explicitly at entry creation, where the entry comes from `realloc` with no `memset` and every field is set by hand; macOS and win32 `memset` the entry, so they need nothing.

- **Native folder picker, and a start directory for the file chooser: `pick_folder(title, start_dir)`, `open_file(title, start_dir)`** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `ui/module.ae`, `apps/maerkdown/maerkdown.ae`, `tests/test_widgets.c`). Closes #9. Open and save were already there; the folder picker, the third verb the issue asked for, was missing entirely, which is what blocked a file manager's "New in..." flow. GTK4 gets `GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER` through the existing chooser helper (with a `_Select` accept label rather than `_Open`); macOS folds both pickers onto one `NSOpenPanel` differing only in `canChooseFiles` / `canChooseDirectories`; win32 uses `SHBrowseForFolderW` with `BIF_NEWDIALOGSTYLE`, matching the generation of API the rest of that file already speaks (`GetOpenFileNameW`), with the start folder delivered via the documented `BFFM_SETSELECTION`-on-init callback. **API change**: `open_file` now takes a second argument, the folder to open in ("" for the platform default); the one in-tree caller is updated. All three verbs keep the `AETHER_UI_HEADLESS` contract from `LLM.md`, and `tests/test_widgets.c` now asserts it: the suite calls each chooser headless and requires an empty string back. That test is deliberately shaped so a backend that forgets the guard HANGS CI rather than failing quietly, which is the loudest available signal for a modal with nobody to dismiss it. Verified 50 passing on both the AppKit and the GTK4 build of the suite.

- **`text_truncate(handle, mode)`: ellipsise a label that will not fit, "none" | "head" | "middle" | "tail"** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `ui/module.ae`, `examples/typo_demo`, `tests/typo_demo/spec_typo_demo.ae`). Closes #14. Alignment (`text_anchor`) and wrapping (`text_wrapped`) were already there on all three backends; truncation was the missing third, which is why an app had to pre-trim paths in model code to keep a label inside its box. `PangoEllipsizeMode` on GTK4, the cell's `NSLineBreakByTruncating*` on AppKit, `SS_ENDELLIPSIS` / `SS_PATHELLIPSIS` on win32. The driver reports it on every text widget as `"truncate"`, and reports the mode that was actually APPLIED rather than the one requested: a Win32 STATIC has no head ellipsis, so head degrades to tail there and the getter says tail, following the same honesty rule as `overlay_material_effective` for a blur that degraded to a tint. `typo_demo` gained the three ellipsised labels, an untouched control, and a label naming the backend, so the spec can assert the exact effective mode on the platform answering rather than settling for a weaker cross-platform claim. Verified 5 passing on AppKit and on a GTK4 build.

- **`file_icon(path)` and `set_file_icon(handle, path)`: the icon the OS uses for a KIND of file** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `ui/module.ae`, `examples/fileicon_demo/` (new), `tests/fileicon_demo/spec_fileicon_demo.ae` (new, 3 cases), `tests/test_widgets.c`, `ci.sh` Phase 5e3, `tests/spec_matrix.sh` suite `fileicon`). Closes #4. `image(path)` loads a file as a picture, which only helps when the file IS a picture; a file browser needs the other question answered, and had been falling back to emoji glyphs. `NSWorkspace iconForFile` on macOS, the `GIcon` from `g_file_query_info` (falling back to `g_content_type_guess`) on GTK4, `SHGetFileInfoW` with `SHGFI_USEFILEATTRIBUTES` on win32. **A path that does not exist still answers**: every backend falls back to the extension, which is what a listing needs to draw a row before it has stat'ed it. `set_file_icon` rebinds a live widget, the shape a recycled list row needs, and on win32 it destroys the icon it replaces so a list that re-icons on every repaint does not leak a handle a row. The driver now reports `"has_image"` on image widgets, which is what lets a spec tell a real platform icon from an empty box: the demo's last icon starts empty on purpose, so the rebind has something observable to change. Verified 3 passing on AppKit and on a GTK4 build, and the C suite (55 passing on both) covers the ABI directly, including the empty-path and missing-path cases.

- **macOS: a scroll area shows content smaller than its viewport, and can be themed** (`aether_ui_macos.m`, `aether_ui_test_server.c` readback, `examples/scrollbg_demo/` (new), `tests/scrollbg_demo/spec_scrollbg_demo.ae` (new, 3 cases), `ci.sh` Phase 5e4, `tests/spec_matrix.sh` suite `scrollbg`). Closes #6. A grid inside `scrollview() { }` rendered nothing on AppKit. Measured through the driver before the fix: the document view sat at `y=360` in a 360-tall window with `w=0,h=0`, and its four cells piled up at a single point with zero width. Two causes. `setDocumentView:` left the document view with `translatesAutoresizingMaskIntoConstraints` off and no constraints of its own, so it had no size at all, and AppKit's clip view is bottom-left origin, so content smaller than the viewport sits below the visible area rather than at its top. The scroll view now uses a flipped clip view (matching every other container in this backend) and pins the document view's leading, top and width to it, leaving height to the content, which is what makes it scroll. After the fix the same grid reads `y=24, w=414, h=54` with its cells in a real 2x2. Separately, `set_bg_color` set only the layer colour, which an `NSScrollView` paints its own background straight over, so `style_bg_color` on a scroll area did nothing visible; it now sets the scroll view's own `backgroundColor` and turns `drawsBackground` on (creation leaves it off). The driver's `bg` readback for a scroll view answers from that property rather than from the stashed request, because the old readback reported a theme that was not on screen. The demo carries an unstyled scroll area beside the styled one so the spec's colour assertion has a control.

- **`window_on_key(cb)` and `on_key(widget, cb)`: a handler for ANY key, with modifiers** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `ui/module.ae`, `examples/keyhandler_demo/` (new), `tests/keyhandler_demo/spec_keyhandler_demo.ae` (new, 4 cases), `ci.sh` Phase 5e5, `tests/spec_matrix.sh` suite `keyhandler`). Addresses the one part of #18 that was genuinely missing: every shortcut verb answers "was THIS combo pressed", while type-ahead asks "what was pressed", and that cannot be built by registering one shortcut per letter. `cb(key_name, mods)` with names matching `canvas_on_key` ("Left", "BackSpace", "a") and mods as a bitmask (1 shift, 2 ctrl, 4 alt, 8 super/command). It fires only when no shortcut consumed the key, so accelerators keep priority, and never swallows it, so the focused widget still receives what was typed: on GTK4 that is a BUBBLE-phase key controller returning FALSE, on macOS the existing local key monitor falling through after `shortcut_dispatch` declines. `on_key(widget, cb)` is built on `window_on_key` plus `focused_widget`, exactly as `widget_shortcut` is built on `shortcut_when`, so there is one key path rather than two that can disagree. **The driver path had to be made to agree with the real one**: `combo_normalize` lower-cases, so `POST /window/key?combo=BackSpace` was delivering `backspace` while an actual keypress delivered `BackSpace`, which would have had every spec asserting a string the app never receives. The canonical spellings are now mapped back on the driver path. win32 registers the handler and answers the driver route; its real `WM_KEYDOWN` translation is the same missing piece already noted for its shortcut registry. Verified 4 passing on AppKit and on a GTK4 build, including that a bound `Ctrl+R` fires its shortcut and does NOT also reach the key handler.

- **`image_fill(handle, mode)`: original / contain / cover / stretch** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `ui/module.ae`, `examples/imagefill_demo/` (new), `tests/imagefill_demo/spec_imagefill_demo.ae` (new, 3 cases), `ci.sh` Phase 5e6, `tests/spec_matrix.sh` suite `imagefill`). Addresses the fill-mode half of #16. `image_size` set a bounding box but said nothing about how the picture fills it, so a thumbnail distorted or overflowed. GTK4 moves its picture widgets from `GtkImage` to `GtkPicture`: `GtkImage` is an icon widget that always scales down keeping aspect, so cover and stretch are not expressible on it, while `GtkPicture` has exactly these four as `content-fit` (`file_icon` stays on `GtkImage`, since only it can show a themed `GIcon`). AppKit gets three from `NSImageScaling` and **cover from a real `drawRect:`**, scaling to the larger axis, centring and clipping, because `NSImageScaling` has no cover mode and a thumbnail grid is precisely what wants one. win32 can only do original and stretch (a `STATIC` scales through one style bit), so contain and cover degrade to **original, never stretch**: distortion is the exact thing a fill mode exists to prevent, and the driver's new `"fill"` field reports what was applied. **The default is now stated rather than inherited**: `GtkPicture` defaults to `CONTAIN` and `NSImageView` to `ProportionallyDown`, so an untouched image behaved differently per backend and the driver reported `contain` on Linux against `original` on macOS for the same program. Both now set contain explicitly, and the macOS getter stops reporting `ProportionallyDown` as "original" when it is really contain. Verified 3 passing on AppKit and on a GTK4 build, with all four modes reported identically. Tint and SVG, the other two parts of #16, are not in this change.

- **Regression coverage for the fixed-bar + filling-body layout** (`examples/barfill_demo/` (new), `tests/barfill_demo/spec_barfill_demo.ae` (new, 3 cases), `tests/lib/uidriver.ae` (`wait_root_height_change`), `ci.sh` Phase 5e7, `tests/spec_matrix.sh` suite `barfill`). Closes #5. The reported behaviour no longer reproduces: the surface root is pinned to all four edges of its host in the delegate, so it follows the window, and a row pinned with `height()` keeps that height while a `fill_height()` body absorbs the slack. Measured on AppKit at 460x400 then resized to 460x700: the bar stays 56 both times, a second row with no explicit height also stays 56 rather than ballooning, and the body goes 264 to 564, taking all 300 new pixels. What was missing was any test holding that, which matters here more than usual because the failure mode is not a crash: every widget exists and answers, and only the numbers are wrong, so a widget census sees a healthy tree. The spec asserts the INVARIANT (whatever the root gained, the body gained) rather than the requested pixel count, since a window manager may grant a different size than asked and asserting 700 would test the WM instead of the layout. For the same reason the natural-height row is asserted UNCHANGED across the resize rather than equal to a number: its height is whatever the platform's metrics make it, 56 on AppKit and 34 under GTK4, and pinning either would have been a test of the theme.

- **`on_file_drop(cb)`: files dropped onto the window** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `ui/module.ae`, `examples/filedrop_demo/` (new), `tests/filedrop_demo/spec_filedrop_demo.ae` (new, 3 cases), `ci.sh` Phase 5e8, `tests/spec_matrix.sh` suite `filedrop`). Addresses the part of #17 the issue itself called the minimum, and the case an editor or file manager cannot do without. Internal row-reorder drag already existed (`listbox_reorderable`, real `GtkDragSource`/`GtkDropTarget` on GTK4); accepting a drop FROM ANOTHER APP did not. `NSPasteboardTypeFileURL` on the window's content view (which already spans the whole window and outlives every widget in it, so a drop lands wherever the pointer is rather than only over a chosen control), a `GtkDropTarget` over `GDK_TYPE_FILE_LIST` on the window, and `DragAcceptFiles` + `WM_DROPFILES` on win32, matching the generation of API that file speaks throughout. The paths cross the C ABI newline-separated in one string and the DSL splits them, which keeps the boundary a plain `char*` instead of building an Aether list across it; `cb(paths, count)` hands back the same string array `string_split` produces everywhere else in the tree. The driver route needed **percent-decoding**: the separator is a newline, so without it two paths arrived as one literal string containing `%0A`, which looks exactly like a single file with a strange name and errors nowhere. Verified 3 passing on AppKit and on a GTK4 build. `draggable(handle, payload)`, the outbound half of #17, is not in this change.

- **`draggable(handle, path)`: drag a file OUT of the app** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `ui/module.ae`, `examples/filedrop_demo`, `tests/filedrop_demo/spec_filedrop_demo.ae`). Closes #17, whose other half (`on_file_drop`) landed alongside it. A `GtkDragSource` over a `GFile` on GTK4; on AppKit an `NSDraggingSession` over an `NSURL`, started from a pan recogniser because `beginDraggingSessionWithItems:` is an `NSView` method and so needs no subclass of whatever widget is being dragged. win32 records the payload and the driver can read it, but does not start a real drag: that means OLE `DoDragDrop` with an `IDataObject` and an `IDropSource`, and unlike the folder picker there is no non-COM equivalent to reach for, so it is written down rather than half-done. The driver reports `"drag"` on any widget that has a payload, and omits it otherwise, so an app that never calls `draggable()` sees no change in the JSON. The spec asserts BOTH the source and a control widget beside it: if every widget reported a payload the field would prove nothing about `draggable()` having been called.

- **`image_tint(handle, r, g, b)` / `image_untint(handle)`: recolour a template or symbolic image** (`aether_ui_backend.h`, `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`, `aether_ui_test_server.c`, `ui/module.ae`, `examples/fileicon_demo`, `tests/fileicon_demo/spec_fileicon_demo.ae`). The tint half of #16, leaving SVG. Only meaningful where the shape carries the meaning and the colour does not, which is what the issue asked for ("for template/symbol images"): a photograph has nothing to tint. macOS marks the image as a template and sets `contentTintColor`, since AppKit only tints template images and setting the colour without that flag is silently ignored; GTK4 sets the CSS colour that a symbolic `GIcon` paints itself with. **win32 has no native equivalent** (recolouring a `STATIC`'s bitmap means compositing a new one through GDI+ on every repaint, a different feature from a style bit), so it applies nothing and `get_tint` reports nothing even after being asked. The driver's `"tint"` field is therefore the EFFECTIVE tint: absent means not tinted, whether because nobody asked or because the backend could not, and the spec branches on the reported backend so win32 is held to "does not claim a tint it cannot do" rather than to a colour. Verified 4 passing on AppKit and on a GTK4 build, both reporting `#337fff` on the tinted icon and nothing on the untinted control beside it.

- **`ui.svg`: an SVG FILE as a widget, drawn rather than decoded** (`ui/svg.ae` (new), `examples/svgimage_demo/` (new), `tests/svgimage_demo/spec_svgimage_demo.ae` (new, 3 cases), `ci.sh` Phase 5e9, `tests/spec_matrix.sh` suite `svgimage`, README). Closes #16 with the tint change before it. `image(path)` hands the file to the platform's image decoder and none of the three decodes SVG dependably: GTK4 might, through whatever pixbuf loaders happen to be installed, and AppKit and Win32 will not, so an SVG asset was unusable or, worse, usable on one machine only. aether-ui already ships a complete SVG stack in `vg`, the one `apps/svg_render_png` matches `rsvg-convert` with, so this routes an SVG through THAT: identical pixels on every backend, no platform decoder, no conversion step, and no new dependency. Kept an **opt-in module** in the shape `ui/icons.ae` established, for the reason that file gives: an app wanting a close glyph should not link the SVG parser, and an app showing SVG assets should not carry the icon vocabulary. `svg_image(path, size)` fits the longer side and lets the other follow the SVG's aspect (square-fitting a 2:1 drawing letterboxes it, which reads as a rendering bug rather than a framing choice); `svg_image_sized(path, w, h)` takes the caller's box instead. The result is a canvas, a drawing rather than a bitmap, so `image_fill` and `image_tint` have nothing to act on. **The canvas has to be pinned**: canvases are created hexpand/vexpand TRUE so a full-window canvas fills, which stretched the 120x60 badge to 340x202 and threw away the aspect computed for it a line earlier, exactly the trap `ui/icons.ae` documents. A file that cannot be read returns handle 0 and makes no widget, because a missing asset is a normal thing to hit.

- **The Win32 backend is now compiled on every change** (`ci.sh` Phase 1d, `.github/workflows/ci.yml` job `win32-crosscheck`, README, LLM.md). Refs #47. Nothing in CI runs Windows, and the workflow says so deliberately rather than soft-failing a job, which is right. The consequence went unnoticed: `aether_ui_win32.c` could be edited and shipped WITHOUT EVER BEING COMPILED, and was, by eight commits in two days. A mingw-w64 cross-compiler closes that for the price of one apt package and about a second per source. It is syntax-only and at the project's own warning level, not `-Wall`: the gate exists to catch code that cannot build, and widening it to style would fail on pre-existing warnings unrelated to whatever is under test. Its own CI job on purpose, since the GTK4 leg's install line carries a warning about being extended casually (adding `gdb` hung it for 45 minutes, twice) and an isolated job cannot take the real gates down with it; it needs no aether toolchain either, because those sources compile against `backend/` and the Windows SDK headers alone. `ci.sh` prints SKIP where no cross-compiler exists rather than passing quietly, since a skipped gate that reads as a green one is how the sources drifted in the first place. Verified by injecting an undeclared symbol into `aether_ui_win32.c`: the check reports the error and fails.

- **Win32: a real keypress now reaches the shortcut registry and the any-key handler** (`aether_ui_win32.c`, `ui/module.ae` comments, README). Refs #47, the first of that issue's three gaps. Every shortcut verb and `window_on_key` already REGISTERED correctly there and fired through the driver route, so specs passed; what was missing was the `WM_KEYDOWN` to combo translation, meaning nothing an actual user typed ever reached them. `w32_key_from_msg` produces both forms the two consumers need: the combo string the registry stores and the driver sends ("Ctrl+R", matched verbatim, so a keystroke and `POST /window/key` end in the same closure), and the bare key name plus modifier bitmask the any-key handler takes. Key names match `aeui_key_name_for_event` on AppKit and `gdk_keyval_name` on GTK4, because an app comparing `k == "BackSpace"` has to see the same string whichever backend delivered it. It runs BEFORE `IsDialogMessageW`, for the reason the canvas path already did: dialog navigation eats Return, Escape and Tab, and a bound `Ctrl+R` must not also be typed into whatever has focus. A bound combo is consumed; an unbound key is offered to the any-key handler and then carries on, so the focused control still receives it. Two things the sentinel had to get right: a modifier key pressed alone is not a keypress (it would fire an any-key handler on every Ctrl on the way to Ctrl+R), and "not a key" is -1 rather than 0, since 0 is the modifier mask an unmodified letter legitimately carries. **Written under the cross-compile gate added alongside**, which caught the declaration-order errors before the push rather than after.

- **Win32: `image_tint` actually tints** (`aether_ui_win32.c`, `ui/module.ae` comment, README, `tests/fileicon_demo/spec_fileicon_demo.ae`). Refs #47, the second of that issue's three gaps. The note left in the code claimed this "means compositing a new bitmap through GDI+ on every repaint, which is a different feature from a style bit", and that framing is what made it look out of reach. It is wrong: **a tint changes only when someone sets one**, so the recolour happens ONCE and the STATIC is handed the result. The image's alpha shape is kept and its colour replaced, which is what a template tint means and what the other two backends do. The untinted original is stashed per handle, so `image_untint` restores the real thing rather than approximating it back, and tinting twice starts from the source instead of stacking. An icon-backed widget (what `file_icon` makes) has no bitmap to recolour, so its colour plane is taken via `GetIconInfo` and the control becomes bitmap-backed, style flipped to match. One defensive detail that would otherwise be invisible: an image whose alpha is entirely zero, which covers every 24bpp source and plenty of opaque 32bpp ones, is treated as fully opaque, because taking that alpha at face value would tint the shape to nothing. The spec no longer branches on the backend: all three are held to reporting the colour, so it fails rather than passing quietly if any of them goes back to merely recording the request.

- **Win32: `draggable` starts a real drag** (`aether_ui_win32.c`, `ui/module.ae` comment, README). Closes #47's third and last gap. An `IDropSource` and a CF_HDROP `IDataObject`, implemented by hand: there is no non-COM route here the way `SHBrowseForFolderW` was for the folder picker. Both are **static singletons with a fixed refcount** rather than heap objects, which is safe precisely because `DoDragDrop` is synchronous: it does not return until the drop or the cancel, so exactly one drag exists at a time and there is nothing to race. That removes the whole class of COM lifetime bug from a path nobody here can run and debug. The drag starts on a mouse MOVE with the button held rather than on the press, because starting on the press would swallow ordinary clicks and a draggable row could never simply be clicked. CF_HDROP is a `DROPFILES` header followed by a DOUBLE-null-terminated path list, and the second terminator is what tells the receiver the list ended; omitting it is the classic way a drop arrives as one path plus garbage. `EnumFormatEtc` is deliberately `E_NOTIMPL`: shell drop targets query CF_HDROP directly rather than enumerating, so an enumerator would exist only to be correct on paper. Verified beyond compiling: a link probe against the real mingw import libraries resolves every COM and shell symbol (`DoDragDrop`, `OleInitialize`, `IID_IDropSource`, `IID_IDataObject`, `SetWindowSubclass`), leaving only the two libaether runtime symbols that have no Windows build on this machine.

- **The Win32 gate links, not just compiles** (`ci.sh` Phase 1d, `.github/workflows/ci.yml`, `tests/win32/link_stub.c` (new), README, LLM.md). Syntax was only half the story: a Windows API declared in a header whose IMPORT LIBRARY is missing compiles perfectly and fails at link, which is invisible to everyone who cannot build on Windows. Proven rather than argued: dropping `-lole32` leaves the syntax check reporting **0 errors** while the link reports `__imp_CoCreateInstance`, `__imp_CoTaskMemFree`, `__imp_CreateStreamOnHGlobal` and `__imp_GetHGlobalFromStream`. The backend reaches into ole32, shell32, comctl32, gdiplus and uxtheme, so that class of mistake is live. The only thing between the check and a real Windows binary is the libaether runtime, which has no Windows build on a Linux or macOS box; `tests/win32/link_stub.c` supplies exactly those two symbols and says what to do when it stops being enough: add the new symbol with the call site that wants it, never disable the check, and never stub a WINDOWS symbol, since an undefined Windows API is the bug this exists to catch and the fix for one of those is a missing `-l` flag.

- **Win32: three defects found by reading back the drag and tint code merged an hour earlier** (`aether_ui_win32.c`). All three are in the paths nothing here can run, which is exactly why they were worth re-reading rather than trusting. (1) The drag gesture kept a `static int pressed` inside a proc SHARED by every draggable widget, so it was global state: press on row A, hold, move over row B, and B started a drag carrying B's file. The user would have grabbed one row and dropped another. It now records WHICH handle was pressed, which is the whole state needed since there is one mouse. (2) Tinting an icon-backed widget leaked the original `HICON`: `DeleteObject` on an icon handle silently does nothing, and the icon was never stashed. (3) Untinting such a widget restored the icon's COLOUR PLANE, not the icon, so it would have come back as an opaque rectangle: an icon carries a mask, which is what gives it its transparent shape. The tint state now keeps both an original bitmap and an original icon, restores the icon with `STM_SETICON` and flips the style back to `SS_ICON`, and never passes an icon handle to `DeleteObject`. Cross-compiles and links clean (0 errors, 0 warnings, 508KB binary).

- **Win32: two more from the second reading, in the key and drag paths** (`aether_ui_win32.c`). (1) Combo matching was exact-string, which was fine while the only producer was the driver sending back what the app had registered. A real keypress is a SECOND producer and has to invent a spelling: an app writes `shortcut("Ctrl+Space")` while the translation emits `"Ctrl+space"` from the key-name table, and `strcmp` would never fire. Comparison is now case-insensitive, which is what GTK4 and macOS already achieve by normalising at registration; doing it at the comparison instead means nothing already stored changes form. (2) The drag held a POINTER into the payload registry for the duration of the drag, and `DoDragDrop` runs its own message loop, so app code executes while a drag is in flight: a timer callback calling `draggable(handle, other)` frees that string and `GetData` reads freed memory. The path is copied for the drag's duration instead, which removes the question rather than reasoning about how likely it is.

- **macOS: a path that is not valid UTF-8 no longer raises** (`aether_ui_macos.m`). `stringWithUTF8String:` returns nil for such bytes and `fileURLWithPath:nil` RAISES, so `file_icon`, `draggable` and `open_file`'s start directory could each take the process down rather than decline. Measured rather than assumed while fixing it: APFS **rejects** a non-UTF-8 filename outright (creating one fails with `EILSEQ`), so this Mac's own filesystem will never produce such a path, and the first draft of this comment claiming "a file manager is exactly where such names turn up" was wrong for the platform. The exposure is the API surface, not the disk: a path from a network share, a config file, or a listing produced on Linux can all be handed to these verbs by an app. All three now convert through `stringWithFileSystemRepresentation:` and return nil harmlessly when even that cannot decode, because an icon that does not appear beats a process that raises.

- **A dropped file whose name contains a NEWLINE is omitted rather than split** (`aether_ui_macos.m`, `aether_ui_gtk4.c`, `ui/module.ae`). A flaw in the ABI chosen for `on_file_drop`: the paths cross the C boundary newline-separated, and a newline is a legal character in a filename. Measured rather than assumed: APFS accepts `two\nlines.txt` without complaint, so this is not a Linux-only curiosity. Such a path was being delivered as TWO strings, neither of which names a file that exists, and an app would act on both: for a file manager that means operating on the wrong path, which is worse than not seeing the file at all. Both backends now drop such a path from the batch, giving fewer paths but every one of them real. **The limitation is stated in the DSL comment rather than papered over**, because it cannot be fixed within this ABI: the boundary carries NUL-terminated strings, so there is no separator that a path can never contain. Fixing it properly would mean changing the callback shape, which is a bigger decision than this change.

- **GTK4: an outbound drag reads the CURRENT file, and stops dangling** (`aether_ui_gtk4.c`). The drag source captured the payload string when it was armed and kept it as the handler's user data. That was wrong twice over. Calling `draggable()` again with a new path runs the `g_object_set_data_full` destructor, which FREES the captured string, so the next drag read freed memory. And even before any free, the handler would have offered the FIRST path forever, so a recycled list row that rebinds its file, which is exactly the shape `set_file_icon` exists for, would have handed over the wrong one. The handler now reads the live payload off the widget through `gtk_event_controller_get_widget`. NB the driver's `drag` field reads the same widget data directly, so a driver-level spec could not have caught either half: this is only observable in a real drag, which is why it needed reading rather than running.
- **macOS: tinting stops mutating a SHARED icon, and survives a rebind** (`aether_ui_macos.m`, `examples/fileicon_demo`, `tests/fileicon_demo/spec_fileicon_demo.ae`). Two defects at the seam between `image_tint` and `set_file_icon`, both in the recycled-row case the pair exists for. (1) `setTemplate:YES` was applied to the image `NSWorkspace` returned, and NSWorkspace hands out CACHED, SHARED `NSImage`s per file type: tinting one row turned that icon into a template everywhere else in the app, so unrelated rows showing the same file type changed with it. A private copy is marked instead. (2) A tint lives on the VIEW while the template flag lives on the IMAGE, so `set_file_icon` left the view still tinted and the new image not a template, and `get_tint` reported a colour the widget had stopped showing. The rebind now carries the flag onto the new image. **Neither half is observable through the driver**, which reads `contentTintColor` off the view and so answers the same either way; the new spec case asserts what it can (the tint is not cleared outright) and says in its own comment where its sight ends.

- **macOS: a `cover` image honours its tint instead of silently dropping it** (`aether_ui_macos.m`, `ui/module.ae`). An interaction between two features added separately in the same session. AppKit applies `contentTintColor` when IT draws a template image; the cover path draws the image itself with `drawInRect:`, so the tint was skipped while `get_tint` went on reporting it: the widget claimed a tint it was not showing. The cover path now fills `SourceIn` over what it just drew, which keeps that alpha and replaces its colour, reaching the same result by hand. Also documents what the combination does on each backend, because it is not uniform and cannot be: on GTK4 a tint is the CSS colour of a symbolic `GIcon`, which only a `GtkImage` shows, while content-fit belongs to `GtkPicture`, and one widget is not both, so `get_tint` reports none there and the difference is visible rather than silent.

### Notes

- The shared registry layer (`aether_ui_system_extras.{h,c}`) is cross-platform C with no platform deps. When real native tray/notification backends land per-platform, the only hook they need is to call `aether_ui_tray_emit_click(id)` / `_menu_activate(id, label)` / `_notif_emit_click(id)` from the OS-delivered event handler — the DSL surface and driver routes don't change.
- A pre-existing menu-item side-store (`aether_ui_menu_item_record`) was added to the menu paths in all three backends so the AetherUIDriver's `/tray/{id}/menu/activate` route can resolve a label → closure across backends, including the still-stubbed GTK4 menu path.
- POST tray/notification dispatch fires the Aether closure synchronously on the HTTP server thread (vs. the existing widget-click pattern that marshals to the GTK main thread via `g_idle_add`). Intentional: `app_run_headless` apps have no main loop draining `g_idle_add`, so a marshalling dispatch would silently never fire under headless mode. Tray callbacks must therefore not touch GtkWidgets directly — they mutate reactive state cells or schedule work, matching the AvnSync v2 shape from the asks. When a real native backend lands, the OS callback itself already runs on the GTK thread, so the dispatch model still holds.
