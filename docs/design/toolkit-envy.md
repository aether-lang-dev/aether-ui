# Toolkit envy: what other UI toolkits got right

Ideas aether-ui could take from other UI toolkits, each read from the
toolkit's own source rather than from memory of using it, and each judged
against aether-ui's shape: a declarative, compositional DSL over native
widgets (GTK4, AppKit, Win32, and UIKit in progress), the vg layer for
drawn content, and AetherUIDriver for tests.

The premise, which held for every toolkit studied: their *good* ideas are
mostly protocols, indirection tables and model/view splits, not class
hierarchies, and they port to a compositional DSL essentially intact. Take
the ideas, leave the hierarchy.

## The rounds

| Round | Toolkit | Read from | Items | What it is |
|---|---|---|---|---|
| 1 | Swing | the JDK's `javax/swing/**` | #1–#10 | the original envy list |
| 2 | (Swing, continued) | Round 1 | C1–C10 | Round 1 turned into an ordered change series, each change with how it is falsified |
| 3 | Flutter and Fyne | their trees, 2026-10-01 | F0–F5 | the native-widgets-vs-drawing fork (F0), widget catalogue, cross-cutting gaps |
| 4 | Slint | its tree, 2026-10-01 | S1–S6 | an accessibility-addressed driver, state by condition, model adapters, gettext |
| 5 | Qt Quick / QML | `qtdeclarative`, 2026-10-01 | Q1–Q6 | a UI-aware linter, platform dialog-button order, pooled delegates |
| 6 | Zed's GPUI | `not-ours/zed/crates/gpui`, 2026-10-05 | G1–G10 | a semantics tree for drawn content, key contexts, a recording test platform |

Each round cross-references earlier items where they overlap rather than
repeating them, and ends with what that toolkit should envy back and its
own short list of picks.

## Where things stand

Landed from the change series (see CHANGELOG.md): **C1** `command`, **C5**
`keymap` (bindings as data), **C10** `undo_group`, the sort half of **C2**
(the table sorts its view, never the app's model), and clipboard read (half
of **C6**). Code comments cite items by these IDs, e.g. `(toolkit-envy C5)`.

## If I had to pick, across the rounds

The rounds' own lists, merged and de-duplicated, with what has landed
taken out:

1. **One renderer contract** (C3), with pooled delegates as its third
   answer (Q4): the pieces exist (`table_col_delegate`, `vlist`), the
   contract they all speak does not.
2. **Key contexts and a platform-relative modifier** (G3, G2): keymap
   bindings scoped to part of the window, and a `Mod` that is ⌘ on a Mac
   and Ctrl elsewhere.
3. **A semantics tree for drawn content** (G1 shaping F2.7): built once in
   Aether, published by a thin adapter per backend, as GPUI does through
   AccessKit.
4. **Testable dialogs and a test platform that records** (G6, building on F4): specs
   that can script a file chooser's answer and see what the app asked for.
5. **Model/view index separation, the rest of C2**: filtering and sorting
   as a class of feature, before more apps bind tables directly.

---

# Round 1 — Swing (`javax.swing`)

What Swing got right that aether-ui has not.

Swing is 1997 technology in a language with a different family tree, and it
carries obvious mistakes — the AWT peer inheritance, `JComponent` as a
god-class, `Serializable` on everything, a `JTable` that knows about
printing. The interesting thing is that almost none of its *good* ideas
depend on inheritance. They are protocols, indirection tables and
model/view splits, and they port to a compositional DSL essentially intact.

Paul's observation about the Netscape IFC team is the right frame. If they
had their time again — free of AWT, free of the requirement that everything
descend from `Component` — most of what follows would look *more* natural,
not less. `JFrame` and `JInternalFrame` sharing behaviour through a
`RootPaneContainer` interface rather than a distant common ancestor is
exactly the shape a compositional toolkit reaches for anyway.

## 1. Cell renderers — the rubber-stamp

**The idea.** `ListCellRenderer.getListCellRendererComponent(list, value,
index, isSelected, cellHasFocus)` returns a component that is *not* added to
the tree. It is configured, its `paint()` is called to stamp the cell, and
it is reused for the next row. One component object renders ten thousand
rows.

```java
Component getListCellRendererComponent(
    JList<? extends E> list, E value, int index,
    boolean isSelected, boolean cellHasFocus);
```

**Why it matters.** It decouples "how many rows exist" from "how many
widgets exist" without the app having to think about virtualisation. The
same protocol serves `JList`, `JTable`, `JTree` and combo boxes, so a
renderer written once works in all four.

**Where we are.** `table_col_delegate(cols, title, w, render)` is the same
*shape* — `|item, i, cell|` builds widgets into a per-cell container — and
`vlist` does windowed virtualisation for lists. Two gaps:

- the delegate builds **real widgets per cell**, not a stamped one. `vlist`
  bounds this for lists only: tables and trees both ride a plain `listbox`
  underneath (`TableDef.lb`, `TreeDef.lb`), so every row of either is a live
  widget however large the model.
- there is **no shared renderer protocol**. A delegate written for a table
  column cannot be handed to a list or a tree.

**Worth stealing:** one renderer contract used by every collection widget,
and a stamped (non-retained) path for large models. Our vg layer arguably
makes the stamp *easier* than Swing's — a renderer that emits vg nodes
rather than widgets has no tree to attach to at all.

---

## 2. Editors, and the editor/renderer pair

`TableCellEditor` / `AbstractCellEditor` / `DefaultCellEditor` split *view*
from *edit*: a cell is rendered by one object and edited by another, with
`CellEditor` carrying the lifecycle — `getCellEditorValue`,
`stopCellEditing`, `cancelCellEditing`, `shouldSelectCell`, and a listener
so the table learns when editing finished.

`DefaultCellEditor` then wraps an ordinary `JTextField`, `JCheckBox` or
`JComboBox`, which is the neat part: **any existing control becomes a cell
editor** without a bespoke class.

**Where we are.** Nothing equivalent. Editing a table cell means building it
yourself. `maerkdown` has a whole word-as-widget editing model, so the
capability exists in the codebase — it just is not a reusable cell-edit
protocol.

**Worth stealing:** the `stop`/`cancel`/`getValue` lifecycle, and
specifically the "wrap an ordinary widget as an editor" adapter.

---

## 3. Model / view index separation (`RowSorter`, `RowFilter`)

The single most under-appreciated thing in the list:

```java
public abstract int convertRowIndexToModel(int index);
public abstract int convertRowIndexToView(int index);
```

Sorting and filtering are **not** operations on the data. The model keeps
its order; the sorter maintains a view↔model index mapping; the table asks
the sorter which model row a view row corresponds to. So sorting a 100k-row
table moves no data, filtering destroys nothing, and a selection can be
preserved across a re-sort because selection lives in model coordinates.

`RowFilter.include(entry)` is likewise a *predicate over an entry*, not a
mutation.

**Where we are.** `table_bind`/`listbox` bind to list-state directly, and
`on_sort(t)` gives a header-click hook — but its contract is literally "the
app sorts + updates": the app reorders its own data and calls
`table_update`. That is the mutation Swing avoids. There is no view/model
index distinction, so sorting loses the data's own order and filtering
means rebuilding the list.

**Worth stealing:** this one wholesale, and early. It is a small amount of
machinery that makes sort, filter, selection-stability and
"show N of M rows" all fall out. Retrofitting it after apps depend on
index==position is painful.

---

## 4. `Action` — one command, many surfaces

> in cases where the same functionality may be accessed by several controls

An `Action` bundles the callback with its *presentation*: name, icon,
tooltip, mnemonic, accelerator, and crucially `setEnabled`. Hand the same
`Action` to a toolbar button, a menu item and a keystroke binding, and
disabling it greys out all three simultaneously.

**Where we are.** We have `shortcut`, `shortcut_when`, `shortcut_chord`,
`widget_shortcut`, menus with accelerators, and buttons with callbacks —
but they are **separate registrations**. An app wiring "Save" to a toolbar
button, a File-menu item and Ctrl-S writes the callback three times, and
disabling it means remembering all three sites.

**Worth stealing:** a first-class command object. This is pure win with no
inheritance required — a struct with a callback, a label, an enabled flag
and observers. Probably the highest value-to-effort item in this document.

---

## 5. `InputMap` / `ActionMap` — keystrokes as data, with scope

Swing splits key handling in two: `InputMap` maps a `KeyStroke` to a *name*;
`ActionMap` maps that name to an `Action`. Both are chained (each has a
parent that is searched on miss), and an `InputMap` is registered at one of
three scopes: `WHEN_FOCUSED`, `WHEN_ANCESTOR_OF_FOCUSED_COMPONENT`,
`WHEN_IN_FOCUSED_WINDOW`.

The consequences are worth spelling out:

- **rebindable keys are free** — change the `InputMap` entry, not the code;
- **a look-and-feel can ship default bindings** and an app override them,
  via the parent chain;
- **scope is declarative**, so a dialog's Escape and a text field's Escape
  do not fight.

**Where we are.** `shortcut_when` gives scoping and `shortcut_chord` gives
two-key sequences, which is genuinely good. But bindings are code, not data:
there is no table to enumerate, rebind, or ship as a keymap. No user-facing
"customise shortcuts" is possible without one.

**Worth stealing:** the keystroke→name→command indirection, and the parent
chain.

---

## 6. `LookAndFeel` / `UIDefaults` / `UIManager`

Swing's LnF is not theming — it is **component implementation swapping**.
Each widget delegates painting *and behaviour* to a `ComponentUI` looked up
by key. `UIDefaults` is a lazy table of everything: colours, fonts, borders,
icons, and the UI class names themselves.

Two details our AeCS layer does not have:

- **Lazy values.** `UIDefaults` stores `LazyValue`/`ActiveValue` entries, so
  an expensive resource is built on first ask, and `ActiveValue` is
  recomputed per lookup.
- **`installColors`/`installBorder` semantics** — a LnF only overwrites
  properties the *app* has not explicitly set (`UIResource` marks a value as
  "LnF-owned"). That is how re-theming a live app does not clobber
  deliberate app styling.

**Where we are.** AeCS (`create_styles`/`st_*`/`apply_styles`) has a real
cascade — class.kind → class → kind → container → root — live re-theming and
`styles_for_mode`. That is the theming half, and it is good.

What we do not have is the `UIResource` distinction: **nothing marks a value
as "set by the theme" versus "set by the app"**, so a live re-theme cannot
know what it is allowed to overwrite. Nor is there a swappable
*implementation* tier — our backends differ per platform, but a single
platform cannot swap component behaviour.

**Worth stealing:** `UIResource` marking, definitely. Lazy defaults,
probably. Full pluggable ComponentUI, probably not — it is the most
inheritance-bound idea here, and vg gives us drawn components more directly.

---

## 7. `Scrollable` — the component negotiates its own scrolling

```java
int getScrollableUnitIncrement(Rectangle visibleRect, int orientation, int direction);
int getScrollableBlockIncrement(Rectangle visibleRect, int orientation, int direction);
boolean getScrollableTracksViewportWidth();
```

A scrolled component tells the viewport how far one "unit" is (a line of
text, a row, a grid square), what a page means, and whether it should be
stretched to the viewport rather than scrolled. That is why a mouse wheel
scrolls a `JTable` by rows and a `JTextArea` by lines without either knowing
about the other.

**Where we are.** We have scrollviews and `vlist_scroll_to`, but scroll
amounts are not component-defined. Wheel scrolling is generic.

**Worth stealing:** the unit/block/tracks-viewport trio. It is three
functions and it is why Swing scrolling feels right.

---

## 8. `TransferHandler` — copy/paste and drag/drop as one protocol

One object per component handles cut, copy, paste, drag *and* drop, and
Swing wires the standard keyboard bindings to it automatically. Data moves
as a `Transferable` with declared flavours, so a drag between two
unrelated components negotiates a common representation.

**Where we are.** More than "mentions", and less than a protocol.
`listbox_reorderable` does real row drag-reorder within one list — a drag
source carrying its index, a drop target, `on_drop(source_index)` — and
`clipboard_write` puts text out. That is the whole surface: there is **no
clipboard read**, so nothing can be pasted *into* an aether-ui app at all;
no cross-widget drag; no flavours. The missing paste is the sharpest gap —
Swing gets Ctrl-V for free the moment a `TransferHandler` exists.

**Worth stealing:** the single-object framing, and flavours. Less urgent
than the rest unless a real app needs inter-widget drag.

---

## 9. The text tier (`javax.swing.text`)

`Document`, `AbstractDocument`, `StyledDocument`, `AttributeSet`, `View`,
`Caret`, `Highlighter`, `EditorKit`, `DocumentFilter`. A whole
model/view/controller stack for text where the *document* is a first-class
model with attributed runs, the *views* are a composable hierarchy
(`BoxView`, `ComponentView`), and an `EditorKit` bundles the read/write and
default bindings for a content type.

`DocumentFilter` deserves special mention: it intercepts *before* a mutation
lands, so input restriction is a model-level policy rather than key
filtering.

**Where we are.** `maerkdown` implements word-as-widget editing with a
`mdown` model and a `wordflow` layout engine, which is genuinely close in
spirit — and its "expand a modifier into its ASCII source when the caret
enters" behaviour is something Swing's text tier cannot do at all. But it is
one app's engine, not a reusable text tier.

**Worth stealing:** `DocumentFilter`'s before-mutation hook, and the idea
that attributed text is a *model* other widgets can share. The full `View`
hierarchy is the most inheritance-heavy thing in Swing and I would not port
it.

---

## 10. Smaller things, briefly

- **`SwingWorker`** — background work with typed intermediate publishing and
  a done-on-the-UI-thread hook. We have actors and timers; we do not have
  this shape.
- **`Timer`** (Swing's, not util's) — fires on the UI thread. Ours already
  behaves this way; worth noting Swing had to say it explicitly.
- **`ToolTipManager`** — centralised dismiss/reshow/initial delays, so
  tooltips across an app feel consistent. Ours carry per-widget text with no
  delay policy anywhere, which is the sharper statement of the gap.
- **`InputVerifier`** — focus-transfer validation: a field can refuse to
  yield focus. No equivalent.
- **`JLayer`** / `LayerUI` — a decorator that can intercept paint and events
  for an arbitrary subtree. Our overlay layer covers some of this.
- **`GroupLayout`** with `LayoutStyle` — layout that asks the *platform* for
  the correct gap between a label and its field. Our layouts are explicit.
- **`ProgressMonitor`** — the "only show a dialog if it turns out to be
  slow" pattern, which is a UX idea more than a widget.
- **`ButtonGroup`** — mutual exclusion as a separate object rather than a
  container, so radio semantics do not depend on layout.
- **`javax.swing.undo`** — examined and mostly NOT envied: we already have
  `undoable(label, do, undo)` with `undo`/`redo`/depths/labels, which covers
  `UndoManager`'s core. What Swing adds that we lack is `CompoundEdit`
  (batch many edits into one undo step — a drag is one gesture, not thirty
  moves) and the significant/insignificant distinction. Worth taking if an
  editor needs gesture-level undo; maerkdown eventually will.

---

## What Swing should envy back

Worth stating, so this is not one-directional:

- **Frames are cheap.** `ui.frames` internal frames render through the
  retained compositor: a static frame is produced once and blitted, an
  animating one re-renders only itself (Stage 5), and a frame fully covered
  by an opaque one is skipped entirely (Stage 4). Swing's `JDesktopPane`
  repaints far more eagerly, and `JInternalFrame` drags in the whole
  `JComponent` machinery.
- **Live content is a first-class citizen.** An MP4 decodes in-process and
  blits into a vg raster region inside a draggable frame, synced to the
  audio clock within 3 ms, while the frame beside it never redraws. Swing
  has no answer short of `JavaFX`/`JMF`.
- **Type as geometry.** `vg.text_path` makes glyphs first-class path data —
  transformable, strokeable, kerned. Swing's text is drawn, not modelled.
- **The driver.** Every widget's geometry, style, a11y name and paint
  counters are readable over HTTP, which makes four-platform parity testable
  in a way Swing never made easy.
- **No god-class.** `JComponent` is 5,692 lines that every widget inherits.
  Ours compose.

---

## If I had to pick three (Round 1)

1. **`Action` as a command object** (#4) — smallest change, immediate payoff,
   removes real duplication today.
2. **Model/view index separation** (#3) — cheap now, expensive later, and it
   unlocks sorting and filtering as a class of feature rather than one-offs.
3. **A shared cell-renderer protocol** (#1) — we have the pieces
   (`table_col_delegate`, `vlist`); what is missing is one contract they all
   speak.

`InputMap`-as-data (#5) is the one I would most like but is the biggest
change, since it means rebuilding shortcut registration around a table
rather than calls.

---

# Round 2 — the change series to functional equivalence

The envy list above, converted into an ordered series of changes. Each is
stated in aether-ui's own idiom — closures and builders, no inheritance —
names what it builds on (every cited primitive exists today), and says how
it gets falsified, because three assertions this year shipped green against
the bug they guarded and the discipline is now house rule.

Order is by dependency, not importance: #1 and #2 unblock most of the rest.

## C1. `command` — one callback, many surfaces  (S)

```
save = command("Save", "Ctrl+S") callback { do_save() }
command_set_enabled(save, 0)          // greys button + menu + kills the key
btn_command(save)                     // a button wired to it
menu_item_command(fmenu, save)        // a menu item wired to it
```

Builds on: `shortcut` (the accelerator registers through it), the
state-observer primitive behind `computed_s` (enabled-state fans out to
every attached surface), `menu_item`.

Acceptance: one command attached to a button, a menu item and its key;
`command_set_enabled(0)` must disable all three — asserted via the driver's
widget `enabled` field and a shortcut fire that must NOT run the callback.
Falsify by detaching the enabled fan-out: the button greys, the key still
fires, spec goes red.

## C2. `rowview` — the view↔model index split  (M)

```
rv = rowview(items)                   // wraps a ui_state_list
rowview_sort(rv, cmp)                 // reorders the MAPPING, not the data
rowview_filter(rv, pred)              // hides rows; data untouched
rowview_to_model(rv, i) / rowview_to_view(rv, i) / rowview_count(rv)
```

`listbox`, `table`, `vlist` and `tree` accept a `rowview` wherever they
take list-state today; selection is stored in model coordinates and
survives a re-sort. `on_sort`'s header-click hook stops meaning "the app
mutates its data" and starts meaning `rowview_sort`.

Builds on: `ui_state_list`, `each_bind`, `on_sort`.

Acceptance: sort a bound table, assert the underlying list-state is
byte-identical (driver reads it back) while the first visible row changed;
select a row, re-sort, assert the SAME model item is still selected.
Falsify by making rowview_sort mutate the list: the byte-identical
assertion goes red. This is the "cheap now, expensive later" item — it
should land before more apps bind tables directly.

## C3. One renderer contract, and the vg stamp  (M)

```
r = cell_renderer() callback |item, i, sel, focus, cell| { ... }   // widgets
rs = cell_renderer_vg() callback |item, i, sel, focus, rn, x, y, w, h| { ... }
```

Both forms accepted by `listbox`, `table_col_delegate`, `tree` and the
dropdown — one contract, four consumers, which is the whole point. The
`_vg` form is the rubber-stamp: it emits vg nodes into the row's rect and
retains nothing, so a 100k-row table costs a window of stamps, not 100k
widget rows. That is also the fix for tables/trees riding a plain
`listbox`: rebase them on `vlist` + the stamp.

Builds on: `table_col_delegate` (the shape already exists for one widget),
`vlist` (the windowing), vg deferred scenes (a stamp with no tree to attach
to — easier here than in Swing).

Acceptance: same renderer closure passed to a listbox and a table column
renders identically (golden-cell signature); a 100k-row stamped table's
widget count stays bounded (driver counts widgets). Falsify by pointing the
table back at the widget path: the count assertion goes red.

## C4. Cell editing lifecycle  (M, needs C3)

```
e = cell_editor(
    callback |item| { textfield_bound(...) },   // begin: build the editor
    callback |w| { ... },                        // value out
)
table_col_editable(cols, "Name", 160, r, e)     // renderer + editor pair
```

Enter commits, Escape cancels, focus-loss commits (Swing's default);
`on_cell_edited(t) callback |row, col, value|` tells the app. The adapter
insight is the part to keep: any existing widget becomes an editor by
wrapping, no bespoke kind.

Builds on: C3 (the renderer half of the pair), `textfield_bound`,
`focused_widget`.

Acceptance: double-click edits, Enter fires on_cell_edited with the new
value, Escape restores the rendered cell unchanged — all driveable today
(`/widget/{id}/double_click` exists). Falsify by breaking cancel: Escape
leaves the new value, red.

## C5. `keymap` — bindings as data  (M, needs C1)

```
km = keymap(parent_km)                          // chained lookup
keymap_bind(km, "Ctrl+S", "file.save")          // key -> NAME
command_register("file.save", save)             // name -> command (C1)
keymap_attach(km, scope)                        // widget | window | global
keymap_bindings(km)                             // enumerable -> rebind UI
```

`shortcut`/`shortcut_when`/`shortcut_chord` become sugar that writes into
the default keymap — no caller changes. The parent chain is what lets a
platform keymap ship defaults an app overrides, and `keymap_bindings` is
what makes a user-facing "customise shortcuts" panel possible at all.

Builds on: C1, the existing `shortcut_when` scoping and chord machinery.

Acceptance: rebind Ctrl-S to Ctrl-Shift-S through the API at runtime; old
key inert, new key fires, `keymap_bindings` reflects it. Falsify by
skipping the unbind: both keys fire, red.

## C6. Clipboard read + `transfer`  (M–L, wants C5; backend work)

```
transfer(widget) {
    t_export("text/plain") callback { selected_text() }
    t_import("text/plain") callback |s| { insert(s) }
}
```

Two halves. First `clipboard_read_impl` on all three backends — today
`clipboard_write` exists and there is NO read, so paste into an aether-ui
app is impossible; that is the sharpest single gap in this document. Then
the transfer builder: Ctrl-C/X/V arrive via the keymap (C5) at the focused
widget's transfer, and cross-widget drag negotiates the first common
flavour, generalising what `row_drag_reorder` already does for one list.

Per the four-OS rule: a backend without clipboard read returns -1 and the
spec asserts that, not a vacuous pass.

Acceptance: driver route `POST /clipboard?text=...` then Ctrl-V into a
textfield lands the text; copy from widget A, paste into widget B via
their declared flavours. Falsifiable at every step.

## C7. Scroll negotiation  (S)

```
scroll_units(widget, unit_px, block_px, tracks_width)
```

Scrollview asks its child; wheel scrolls a table by rows and a text area by
lines. Three numbers and a lookup — smallest real item here.

## C8. Theme-owned vs app-owned styling  (S–M)

AeCS values applied by `apply_styles` get tagged theme-owned; `style_*`
setters tag app-owned; a re-theme only overwrites theme-owned values.
`GET /widget/{id}/style_origin` exposes the tag so the spec can assert that
an app-set colour SURVIVES a live re-theme and a theme-set one changes —
which is precisely Swing's `UIResource` test, minus the marker interface.

## C9. Tooltip policy  (S)

```
tooltip_delays(initial_ms, dismiss_ms, reshow_ms)   // app-wide, once
```

Applies to both the native and drawn (`vg_tooltip_show`) paths. Today there
is per-widget text and no delay policy anywhere.

## C10. `undo_group` — the CompoundEdit  (S)

```
undo_group("Move 3 frames") {
    undoable(...) ; undoable(...) ; undoable(...)
}                                    // one undo step, one label
```

Builds on the existing `undoable`/`undo`/`redo` stack, which already covers
UndoManager's core. A drag becomes one gesture instead of thirty steps.
maerkdown is the first real consumer.

## Deliberately NOT in the series

- **The text tier.** maerkdown's `mdown`/`wordflow` is the proto-tier, and
  the house rule applies: the API follows a second real consumer, not a
  port of `javax.swing.text`. When a second app needs attributed text,
  extract; until then, leave. `DocumentFilter`'s before-mutation hook is
  the one piece worth adding to `mdown` on its own merits.
- **Pluggable ComponentUI.** The most inheritance-bound idea in Swing, and
  vg gives drawn components more directly. C8 takes the useful residue.
- **`InputVerifier`.** Focus-veto needs per-backend investigation (whether
  win32/AppKit can even refuse a focus transfer cleanly) before it is worth
  an API. Parked, not rejected.
- **`SwingWorker`.** Actors plus `ui.timer` cover the need; sugar can wait
  for evidence it is missed.

## Sequencing

```
C1 command ──► C5 keymap ──► C6 transfer (+ clipboard_read backends)
C2 rowview ──► (tables/trees gain sort+filter for free)
C3 renderer ─► C4 editors, and tables/trees move onto vlist
C7, C8, C9, C10 — independent, any time, S-sized
```

Three phases if phased: **A** = C1+C2 (the foundations, both small enough
to land falsified in a session each), **B** = C3+C4+C5 (the collection
widgets become Swing-class), **C** = C6 (the only one needing new backend
surface on all three platforms at once).

---

# Round 3 — Flutter and Fyne

The same exercise against two modern toolkits, read from source:

| Tree | Snapshot | Measured scale |
|---|---|---|
| `not-ours/flutter` | `4e5a0929`, 2026-10-01 | framework only (`packages/flutter/lib/src`): ~568k LOC Dart — `material` 211k, `widgets` 160k, `rendering` 53k, `cupertino` 48k, `semantics` 8k. Engine (Impeller/Skia) not counted. |
| `not-ours/fyne` | `3122191`, 2026-10-01 | ~93k LOC Go, non-test. |
| `aether-ui` | `96f41d65`, 2026-09-30 | `ui/` 7.9k LOC `.ae`, `vg/` 17.9k, `backend/` 39.9k C/ObjC. 515 exported names in `ui/module.ae`. |

Status of Round 2 at the time of this round, from `ui/module.ae`'s exports:
C1 (`command`, `btn_command`, `menu_item_command`), C5 (`keymap*`), C10
(`undo_group`) and C6's read half (`clipboard_read`) have shipped, and C2's
intent is served by `table_sorter`/`table_filter`. Items below say "Swing §n"
or "Cn" when they meet something above.

## F0. The fork in the road: native widgets vs drawing everything

Everything in this round sits on one architectural difference, so it comes
first.

**Flutter and Fyne draw every pixel themselves.** Flutter through its own
engine (Impeller/Skia) on a surface the OS hands it; Fyne through OpenGL, or a
software painter. A Flutter button is a rectangle and some text it rasterised.

**aether-ui uses the platform's widgets** — `GtkButton`, `NSButton`, a Win32
`BUTTON` — behind one backend ABI, and draws only where it chooses to: the vg
layer on canvases, SVG, drawn icons, and (since August) opt-in drawn *chrome*
faces via `ui.chrome`/`ui.chromed`, with text inputs staying native "forever,
because IME is the disqualifier" (`docs/design/vg-drawn-controls.md`).

That split decides which features are cheap for whom:

| Cheap for Flutter/Fyne, expensive for us | Cheap for us, expensive for them |
|---|---|
| Pixel-identical rendering on every platform | Native look and feel per platform, for free |
| Animating *any* property of *any* widget | Platform accessibility (a GtkButton is already a `button` to the screen reader) |
| Mobile and web (same renderer, new host) | IME, spell-check, text editing behaviour users already know |
| A new widget is pure code — no per-backend work | Small binary; no bundled renderer; C ABI embeddable |
| Snapshot-testing the render tree in-process | Real-OS behaviour under test (AetherUIDriver drives the real controls) |

So "Flutter has it" is not, on its own, a reason. The useful question for each
item is whether it is a *widget-catalogue* gap (cheap to close: one more
native control or a composite), a *cross-cutting* gap (one mechanism, many
widgets), or an *architectural* gap (only cheap if we draw — usually decline).

## F1. Widget catalogue — what they ship that we don't

aether-ui's catalogue is already broad: text, button, textfield/securefield,
textarea, toggle (and `toggle_group` radios), slider, picker, progressbar,
listbox (multi, reorderable), `table` (sort/filter/delegates), `tree`,
`vlist` (virtualised), image/svg/file_icon, canvas/gpuview/native_view, tabs,
navstack, splitview, scrollview, zstack, grid, form/section, disclosure
(accordion), menus/context menus, tray, notifications, overlay/toast/sheet,
alert, open/save/folder dialogs. Against that:

| Missing here | Flutter | Fyne | Note |
|---|---|---|---|
| **Date / time picker** | `date_picker`, `time_picker`, `calendar_date_picker`, `input_date_picker_form_field` | `Calendar`, `DateEntry` | Every platform has a native one (`GtkCalendar`, `NSDatePicker`, Win32 `DATETIMEPICK_CLASS`). Catalogue gap, cheap. |
| **Editable combo / autocomplete** | `Autocomplete`, `DropdownMenu`, `SearchAnchor` | `SelectEntry` | `picker` is select-only. Native: `GtkDropDown`+entry / `NSComboBox` / Win32 `CBS_DROPDOWN`. |
| **Indeterminate progress / spinner** | `CircularProgressIndicator` with `value: null` | `ProgressBarInfinite`, `Activity` | `progressbar(fraction)` only. Native on all three (`GtkSpinner`, `NSProgressIndicator`, `PBS_MARQUEE`). |
| **Rich text display** | `RichText`, `Text.rich`, `SelectableText` | `RichText` (+ `ParseMarkdown`), `Selectable` | We have `text`/`text_wrapped`. See F2.5. |
| **Rich text *editing*** | `EditableText` with spans | `RichTextEntry` — styled segments, its own undo (`richtextentry_undo.go`) | New in Fyne this cycle. See F2.5. |
| **Colour picker dialog** | not in the framework | `dialog/color_picker` (wheel, channels, preview) | `NSColorPanel` / `GtkColorChooserDialog` / Win32 `ChooseColor` all exist. A `pick_color()` beside `pick_folder()`. |
| **Toolbar** | `AppBar`, `OverflowBar` | `Toolbar` | `ui.chrome` draws toolbar *faces*; there is no toolbar widget. |
| **Hyperlink** | not in the framework | `Hyperlink` | `open_url` exists; the clickable label does not. Trivial composite. |
| **Card** | `Card` | `Card` | Composite of existing pieces; mainly a theme decision. |
| **Closable document tabs** | — | `DocTabs` | `tabs` has no close affordance. |
| **Text grid / terminal view** | — | `TextGrid` (per-cell rune + style) | A vg-drawn widget would do it; no consumer yet. |
| **Range slider** | `RangeSlider` | — | Flutter only. Low priority. |
| **Inner windows / MDI** | — | `InnerWindow`, `MultipleWindows` | Partly: `ui.frames` (see "What Swing should envy back" above). |

Deliberately not on that list: chips, badges, carousel, `PageView`,
`Dismissible`, pull-to-refresh, navigation drawer/rail, FAB, bottom sheets,
snackbars. They are mobile idioms; we have no mobile backend (F3.1).

## F2. Cross-cutting gaps — one mechanism, many widgets

These matter more than the catalogue, because each one is felt in every app.

### F2.1 Input validation — worth taking

**Theirs.** Fyne: a `Validatable` interface, `Entry.Validator`, required-field
state (`entry_validation.go`, `SetOnRequiredChanged` "for parent widgets or
containers"), stock validators in `data/validation` (`regexp`, `time`), and a
`Form` whose `Validator` must pass "before it will enable Submit". Flutter:
`FormField` validators and `Form.validate()`.

**Ours.** Nothing. `form`/`section` are layout; a field's validity is whatever
the app computes in `on_change`.

**Relation to Swing §10.** `InputVerifier` was parked above because a
focus *veto* needs per-backend investigation. Fyne and Flutter show the
shape that avoids it: never refuse focus, mark the field invalid and gate the
*submit*. That is all-backend-safe today.

**Shape.** A validator closure on `textfield`/`textarea` returning `""` or a
message; the backend shows the native error affordance (GTK `.error` CSS class,
AppKit/Win32 an inline label); a form-level `ui_state_b` that is true when
every field in the form is valid, so `bind_enabled(submit, form_valid)` is one
line. Small, and it composes with the existing state system rather than
inventing a second one.

### F2.2 Preferences, and bindings backed by them — worth taking

**Theirs.** Fyne: `app.Preferences()` (a `preferences.json` in the app's
storage root on desktop and mobile, `localStorage` on web) and
`binding.BindPreferenceBool/Int/String/…` — a bound value that *is* a
persisted setting. A settings screen is a set of widgets bound to preferences,
with no save button and no load code.

**Ours.** Nothing in `ui/`, `vg/`, `apps/` or `docs/` — every app that
remembers anything (OpenDisk-ae's last folder, font_picker's filters) rolls its
own file.

**Shape.** `ui_state_*_pref("key", default)`: the existing typed state, loaded
from and written through to a per-app store (`$XDG_CONFIG_HOME`,
`~/Library/Preferences`, `%APPDATA%`). It is a state *source*, not a new
binding mechanism, so every `bind_*` already works with it.

### F2.3 Window close intercept and app lifecycle — worth taking

**Theirs.** Fyne: `Window.SetCloseIntercept`, `Lifecycle` hooks
(`SetOnStarted`, `SetOnStopped`, `SetOnEnteredForeground`,
`SetOnExitedForeground`). Flutter: `AppLifecycleListener`, `PopScope`.

**Ours.** No quit hook, no close intercept (searched `ui/`, `vg/`, `apps/`,
`docs/` for every spelling). "You have unsaved changes — quit anyway?" is not
expressible.

**Shape.** `window_on_close_request(w) { -> bool }` and `app_on_quit {}`. All
three backends have the hook already (`GtkWindow::close-request`,
`windowShouldClose:`, `WM_CLOSE`), so this is a thin ABI addition, and the
headless rule applies: under `AETHER_UI_HEADLESS` the intercept is consulted
but never shows UI.

### F2.4 In-app drag and drop — C6's other half

**Theirs.** Flutter `Draggable` + `DragTarget<T>` with typed payloads and
accept/reject; Fyne's `Draggable` interface on any widget.

**Ours.** `draggable` is a *source* (out to the OS), `on_file_drop` a *sink*
for OS files, and `listbox_reorderable` moves rows within one list. Nothing
accepts a drag from another widget in the same app. This is the cross-widget,
flavour-negotiated half of C6 — `clipboard_read` shipped, `transfer` did not.
Do it there, once.

### F2.5 Rich text, and markdown into it — worth taking, read-only first

**Theirs.** Fyne's `RichText` is a list of segments (text with style, link,
image, list, separator), and `ParseMarkdown` fills one from a string; this cycle
added `RichTextEntry`, the same segments made editable with their own undo.
Flutter's `TextSpan` tree is the same idea.

**Ours.** `text` is one style per widget. `apps/maerkdown` already parses
markdown — as an *app*, building widgets per word — so the parser exists and
the widget does not.

**Shape.** Read-only first: `richtext(segments)` with bold/italic/mono/link/
size segments, natively (`GtkLabel` Pango markup, `NSAttributedString`,
Win32 RichEdit `EM_SETTEXTEX` with RTF), and `markdown_text(src)` built on
maerkdown's parser. A *display* widget is not the text tier, so it does not
trip Round 2's "Deliberately NOT" rule; editable rich text is the text tier
(Swing §9) and stays parked until a second consumer, as decided above.

### F2.6 Animation breadth — consider, not yet

**Theirs.** Flutter animates anything: implicit `AnimatedX` widgets, tweens,
curves, `Hero`, physics simulations, `AnimatedSwitcher`. Fyne: `fyne.Animation`
with curves, plus `canvas.NewColorRGBAAnimation`, `NewPositionAnimation`,
`NewSizeAnimation`, `NewShaderAnimation`.

**Ours.** The mechanism exists at both layers. Native widgets have
`transition(handle, prop, ms, easing)` — declare once, every later change of
`prop` tweens — with `linear`/`ease_out`/`spring` (a real damped overshoot,
driven by per-backend timers where CSS can't express it) and
`AETHER_UI_NO_ANIMATION` for specs. vg has `animate`/`add_animation` and the
`ease_in`/`ease_out`/`ease_in_out` (+ cubic) curves; overlays have enter/exit
transitions.

**The gap is breadth, not mechanism.** `transition`'s v1 properties are
`opacity` and `all` (CSS-driven changes); Flutter and Fyne animate position,
size and colour. Widening the prop list is the cheap move where a backend has a
native path; a Flutter-style "animate anything" is the F0 trap and is declined.

### F2.7 Accessibility of *drawn* content — the one that grows with the roadmap

**Theirs.** Flutter's whole UI is drawn, so it carries a full semantics tree
(`semantics/`, 8k LOC, plus `accessibility_inspector`, `semantics_debugger`):
every widget declares role, label, value, actions, and the engine maps that
onto the platform's accessibility API. Fyne only started this cycle —
`AccessibleRole` "Since: 2.8" has four values (`button`, `container`, `link`,
`text`).

**Ours.** Native controls are accessible for free, and `a11y_role`/
`a11y_label`/`a11y_description` override or supply what the platform guessed
(`docs/design/accessibility.md`). Here we are *ahead* of Fyne and structurally
advantaged over Flutter.

**The gap is the drawn half.** A canvas, a vg scene, an SVG widget and — the
important one — a `chromed` button face are invisible to a screen reader,
because there is no platform control under them. Every step of the
vg-drawn-controls plan moves a control from the accessible side to the
invisible side. Flutter's semantics tree is the reference for what that layer
needs; this should be a precondition of drawn chrome going default-on, not a
follow-up.

### F2.8 Internationalisation wiring — small, worth taking

**Theirs.** Fyne: `lang.L("key")` lookups, translation files bundled into the
binary, and `fyne translate` ("Scans for new translation strings"). Flutter:
`Localizations` + generated `AppLocalizations`.

**Ours.** The hard parts exist one layer down: Aether's `std.message` (ICU
MessageFormat), `std.plural`, `std.language` (BCP 47), `std.number`, and
`contrib.i18n` (collation). aether-ui has `rtl()` and nothing that connects a
widget's text to a message catalogue. The delta is the wiring and an
extraction tool, not the i18n.

## F3. Architectural gaps — mostly decline

### F3.1 Mobile and web

Fyne ships to iOS, Android, wasm/web and bare-metal (`driver/mobile`,
`driver/embedded`, `driver/software`; `fyne package` has `package-mobile.go`
and `package-web.go`). Flutter the same. This is the largest single delta and
the one most directly caused by F0: a self-drawn toolkit gets a platform by
porting a surface; we get one by writing a fourth and fifth native backend plus
the mobile idioms F1 declined. **Decline without a consumer.** If one appears,
the cheaper route is probably the drawn path (vg + chrome) on a GL surface
rather than UIKit/Android-View backends — which is Fyne's architecture, arrived
at from the other side.

*Correction (2026-10-05, from Round 6):* a fourth native backend already
exists: `backend/aether_ui_uikit.m`, iOS/iPadOS on UIKit, pass 1 behind a
compile+link gate since 2026-08-31. So mobile is not declined; the native
route is under way, and the drawn-path alternative above is the fallback,
not the plan.

### F3.2 Hot reload and widget previews

Flutter's hot reload swaps code into a running Dart VM; `widget_previews`
(`@Preview` annotations rendered in the IDE) is new this cycle. Neither needs a
VM in our hands — see Q6 for the Aether-only route (rebuild-and-relaunch is
already ~0.7 s warm; a state-preserving swap goes through `--emit=lib` +
`std.dl`). Previews are the easy half: `render_to(png, w, h) {…}` already
renders a tree without an event loop, so an annotated-preview gallery is a
tooling exercise over a primitive we have.

## F4. Tooling and testing

| | Flutter | Fyne | aether-ui |
|---|---|---|---|
| Black-box UI driving | `integration_test` (in-app) | — | **AetherUIDriver**: HTTP+JSON against the real native controls on all three backends; widget sealing |
| Tree inspection | DevTools inspector | — | **`apps/inspector`** — a DevTools-shaped browser, written as an aether-ui app over the driver protocol |
| Render-tree snapshot | widget tests, golden images | `test/markup_renderer.go` (render tree → markup), image asserts | **`record(w, h) {…}`** captures a built tree with no loop; no stored-snapshot comparison on top of it |
| Packaging | `flutter build` per target | **`fyne package`** (`.app`, `.exe` with icon/manifest, `.desktop`, mobile, web), `bundle`, `release`, `translate` | `build.sh` / aeb produce a bare binary |

Two deltas worth acting on:

- **Packaging — worth taking, in aeb.** A macOS `.app` (Info.plist, `.icns`),
  a Windows `.exe` with icon and manifest, a Linux `.desktop` + icon. This is
  build work, not toolkit work, so it belongs as an aeb SDK verb (next to
  `build_support/aetherui`) rather than in `build.sh`; every app that ships
  (OpenDisk-ae, fight-flash-fraud-port's GUI) currently hand-rolls or skips it.
- **Snapshots over `record` — worth taking.** Fyne's markup renderer is the
  model: serialise the recorded tree to stable text, check it in, diff in
  review. `record` already produces the tree; what's missing is the stable
  serialisation and the regenerate/compare commands. C3's "golden-cell
  signature" acceptance test would be the first consumer. (Same idea as
  aether's #1604 Roc snapshot recommendation, one layer up.)

## F5. Already covered — don't re-propose

Checked against their equivalents and present here, often in a different
shape: reactive typed state and bindings (`ui_state_*`, `bind_*`, `each_bind`,
`table_bind`, `computed_s`, `bind_text_fmt` — vs Fyne `data/binding`);
commands, undo/redo and keymaps (C1/C5/C10); theming with colour-scheme roles,
dark mode and appearance-change events (vs Fyne `theme`, Flutter `ThemeData`);
drawn vector icons (`ui.icons`, vs Fyne's bundled icon theme); tables with
sort/filter and trees; virtualised lists; split views; tabs; accordion
(`ui.disclosure`); multiple top-level windows (`window_create`,
`docs/design/multi-window.md`); system tray and notifications; clipboard
read/write; file/folder dialogs; `on_layout` for responsive layout (vs Flutter
`LayoutBuilder`); RTL; native property transitions (F2.6); trackpad zoom
deltas on canvases (`canvas_on_scroll`, `apps/gesture_probe`).

## What Flutter and Fyne should envy back

In addition to the Swing list above:

- **Native controls.** Look, feel, accessibility, IME and spell-check from the
  OS — the things Flutter spends `semantics/`, `editable_text`, `spell_check`
  and `autofill` on, and Fyne is only starting (four accessible roles).
- **Testing the real thing.** AetherUIDriver asserts against the actual
  `GtkButton`/`NSButton`/Win32 control, not an in-process model of one, and the
  "report the effective state, never the requested one" rule makes a spec fail
  when a backend silently degrades.
- **Bounded surfaces.** `render_to`/`record` as first-class siblings of
  `window` — print, PNG and headless capture with no event loop — is cleaner
  than either of theirs.
- **A complete in-house vector stack.** vg's SVG parser/renderer, raster and
  3D layers are pure Aether, headless and in CI Phase 0; Fyne leans on the GPU
  driver, Flutter on Impeller/Skia.
- **Platforms they don't target.** FreeBSD/GhostBSD on GTK4 is first-class here.

## If I had to pick five (Round 3)

In order, each independently shippable, none blocked on the open Round 2 items:

1. **Close intercept + quit hook** (F2.3) — small ABI addition, unblocks the
   "unsaved changes" case every document app needs.
2. **Validation** (F2.1) — small, composes with existing state and
   `bind_enabled`, and replaces the parked `InputVerifier` with a shape that is
   safe on all three backends.
3. **Preference-backed state** (F2.2) — removes hand-rolled settings files from
   every app.
4. **Catalogue: date/time picker, editable combo, indeterminate progress**
   (F1) — all three are native on every backend, so they are wrappers, not
   inventions.
5. **Semantics for drawn content** (F2.7) — not because it's small, but because
   every vg-drawn-controls phase without it makes the toolkit less accessible
   than it is today. Do it before drawn chrome goes default-on.

Next tier: read-only rich text + markdown (F2.5), packaging as an aeb verb and
`record` snapshots (F4), i18n wiring (F2.8), wider `transition` props (F2.6).
Open from Round 2 and still the right home for their items: C3/C4 (renderer +
editors), C6's `transfer` (F2.4), C7, C8, C9. Declined without a consumer:
mobile/web (F3.1), animate-anything (F2.6), the mobile widget idioms (F1).

---

# Round 4 — Slint

`not-ours/slint` at `e045cc5`, 2026-10-01 (shallow single-commit clone):
~357k LOC Rust, 177k LOC `.slint`, bindings for Rust, C++, JavaScript and
Python; renderers `femtovg`, `skia`, `software` (`no_std`), backends `winit`,
`qt`, `linuxkms`, `android-activity`; widget styles `fluent`, `material`,
`cupertino`, `cosmic` and `qt`, chosen with `SLINT_STYLE` at build time.

Where it sits relative to F0: Slint draws everything, like Flutter and Fyne,
but its `qt` style borrows Qt's native painting to *look* native without being
native controls. And its UI is a separate markup language (`.slint`) compiled
to the host language or interpreted at runtime. That last part is not envy —
aether-ui's "Config IS Code" rule exists precisely so the UI and the logic are
one language with no loader and no binding layer between them (see the envy-
back list below). The interesting things are elsewhere.

## S1. A driver addressed by accessibility, with an MCP face — worth taking

**Theirs.** The testing backend's `search_api.rs` finds elements the way a
screen reader would: `find_by_accessible_label`, `match_accessible_role`,
`find_by_element_id`, `find_by_element_type_name`, composable
`query_descendants`/`match_predicate`. It reads 20-odd accessible properties
(`accessible_value`, `_checked`, `_expanded`, `_item_selected`,
`_value_minimum/maximum/step`, `_live_region`, …) and **invokes accessible
actions** — `invoke_accessible_default_action`, `_increment_`, `_decrement_`,
`_expand_`, `set_accessible_value`. A test written that way passing is
evidence the accessibility tree is usable, not just that a click landed.

On top of it, `mcp_server.rs`: with `SLINT_MCP_PORT` set, the running app
serves MCP over Streamable HTTP "allowing MCP clients (e.g. Claude) to inspect
and interact with the running UI" — tools `get_element_tree`,
`find_elements_by_id`, `click_element`, `drag_element`, `hover_element`,
`scroll_element`, `set_element_value`, `invoke_accessibility_action`,
`dispatch_key_event`, `take_screenshot`, `list_windows`, and
`start_event_recording`/`stop_event_recording`.

**Ours.** AetherUIDriver is already the harder half: HTTP+JSON against the
*real* native controls on three platforms, with geometry, style, paint
counters and `GET /widget/{id}/a11y` (effective role + accessible name). What
it lacks: lookup *by* role or name (every route is addressed by handle),
invoking accessibility actions, event recording, and an MCP face (no `mcp`
anywhere in `backend/`, `ui/`, `apps/`, `docs/`).

**Shape.**
- **MCP adapter (S).** A thin translation layer — each tool maps onto an
  existing route (`/widgets`, `/click`, `/set_value`, `/scroll`, `/key`,
  `/screenshot`, `/windows`). It needs no backend work, and `apps/inspector`
  already proves the protocol is complete enough to browse a live app. Given
  how much of this repo's work is agent-driven, the leverage is immediate.
- **Address by accessibility (S–M).** `GET /find?role=button&name=Save` and
  `POST /widget/{id}/a11y_action?action=default|increment|decrement|expand`.
  This is also what makes F2.7 *testable*: a drawn control without semantics
  cannot be found this way, so its spec goes red instead of passing on a
  handle.
- **Recording (M).** Record a session as driver calls, replay it as a spec.
  Lower priority; the first two make it easy later.

## S2. States selected by a condition, not by a call — worth taking

**Theirs.** `states [ alarm when level > 10: { … } warning when level > 5:
{ … } ]` — each state carries a `when` condition over any property in scope;
when several hold, the first in source order wins (`sls.state.first-wins`);
`in`/`out`/`in-out` blocks inside a state bind animations to entering or
leaving it (the standalone `transitions [ ]` block is legacy and no longer
supported).

**Ours.** QML-alike states already exist (`ui_states`, `add_state`,
`set_state`, `current_state`, `on_state`), with declared `ui.transition`
properties animating between them and the current state driver-visible as an
`st-<name>` class. But selection is **imperative**: the app calls
`set_state` from wherever the condition changes, which is exactly the
scattered-update problem reactive state was meant to remove.

**Shape.** `add_state_when(ws, "alarm", sheet, alarm_b)` where `alarm_b` is a
`ui_state_b` or `computed_*`; the first state whose condition is true is
applied, re-evaluated when any condition changes. Small — it is an observer
over the existing state system feeding the existing `set_state`.

## S3. Model adapters — Round 2's C2, generalised

**Theirs.** `VecModel` plus composable adapters `FilterModel`, `SortModel`,
`MapModel` and `ReverseModel` over any model, consumed by any `for` repeater
or `ListView`.

**Ours.** `table_sorter`/`table_filter` landed C2's *intent* for tables only.
Slint's design is C2 as originally written — the view↔model mapping is a
property of the **model**, so `listbox`, `vlist` and `tree` get sort and filter
for free — plus one piece C2 never had: **`MapModel`**, a projection, so a
list of records can feed a list of strings without copying. Recommendation:
when C2 is revisited, move sort/filter from the table onto the list-state,
and add map.

## S4. Translations through gettext, switchable live — strengthens F2.8

**Theirs.** `@tr("Hello, {}", name)` with context (`@tr("ctx" => …)`) and
plural forms; `slint-tr-extractor` writes standard `.pot`; translators use
their existing tools (Poedit, Lokalize, Transifex); the result is either
gettext `.mo` at runtime or bundled into the binary. Because a translated
string is a reactive binding, switching language at runtime re-renders every
label with no app code.

**Ours.** F2.8 found the i18n primitives in Aether's stdlib and no widget
wiring. Slint answers the format question F2.8 left open: **use gettext
`.pot`/`.po`**, not a bespoke catalogue, so the translator toolchain comes free;
and make the lookup a bound value, so language switching is a state change
rather than a rebuild of the window.

## S5. Coverage of UI source, not generated code — an Aether-level idea

**Theirs.** Slint SC (`api/slint-sc`, the safety-critical subset targeting
ISO 26262 / IEC 61508 / IEC 62304, `no_std`, no allocation) compiles `.slint`
with `--coverage`: the compiler records a coverage point for "every element,
binding, … callback handler, call, state and state's condition, and both
outcomes of every `?:`, `&&` and `||`", and `slint-sc-coverage` turns LLVM's
coverage of the *generated* code back into lcov for the `.slint` file.

**Why it's here.** Aether compiles to C and already emits `#line`; the same
trick — gcov/llvm-cov on the C, mapped back to `.ae` — would give source-level
coverage for aether-ui apps and for Aether generally. That is a compiler
feature, so it belongs on the aether tracker, not in this repo; recorded here
because this is where it was found.

## S6. Smaller things

- **SpinBox.** Slint's std-widgets add `SpinBox` to the catalogue gaps F1
  listed; its `DatePickerPopup`, `TimePickerPopup`, `ComboBox` and `Spinner`
  confirm F1's priority item 4. `GTK SpinButton`, `NSStepper`+field and Win32
  `UPDOWN_CLASS` are all native.
- **Change callbacks and two-way bindings.** `changed value => { … }` runs
  code when a property changes; `a <=> b` keeps two properties identical.
  Worth checking our `ui_state` observers and `textfield_bound`/`bind_value`
  against both before proposing anything — likely already covered.
- **Purity enforced by the compiler.** Slint rejects side effects in binding
  expressions, because bindings are lazy and their evaluation order is
  unspecified. Our `computed_*` closures can do anything. That is a language
  feature (an effect system — aether #1066), not a toolkit one; noted, parked.
- **Style packs for drawn chrome.** When `ui.chromed` faces go beyond opt-in,
  Slint's fluent/material/cupertino/cosmic switch is the model: one face per
  platform design language, not one house face.
- **A spec with clause IDs.** Slint's language docs carry ~260 stable rule IDs
  (`{#sls.state.first-wins}`) that tests cite. A docs-practice idea for
  aether's `language-reference.md`/`edge-cases.md` rather than aether-ui.

## Declined

- **The markup language — but not what it buys.** `.slint` is a second
  language interpreted for live preview; ours stays compiled Aether by design.
  Live preview and `slint-viewer`-style auto-reload are still reachable in
  plain Aether (Q6). The visual editor (`editor-preview`'s component catalogue
  and drag-to-insert editing), SlintPad and Figma import are large and wait
  for a consumer — but they would edit Aether source, not need a markup.
- **MCU / `linuxkms`.** A `no_std` software renderer and a framebuffer backend
  with no display server. vg already renders headless to raster, so a KMS
  present path is less far off than it sounds — but there is no consumer.

## What Slint should envy back

- **One language.** UI and logic are the same Aether, so a model is a value
  and a callback is a closure — no `.slint`→Rust/C++/JS/Python binding layer,
  no generated getters, no marshalling across it.
- **Real native controls**, not a native-*looking* style over a drawn toolkit.
- **A driver against the real OS.** Slint's testing backend is an in-process
  fake window; AetherUIDriver drives `GtkButton`/`NSButton`/Win32 controls.

## If I had to pick three (Round 4)

1. **MCP adapter over AetherUIDriver** (S1) — small, no backend work, and the
   most-used client of this toolkit's driver is already an agent.
2. **Driver lookup and actions by accessibility** (S1) — makes F2.7 testable,
   and makes specs survive handle renumbering.
3. **Condition-selected states** (S2) — small, and it finishes the reactive
   story the states API started.

Fold-ins rather than new work: S3 into C2, S4 into F2.8, SpinBox into F1. S5
goes to the aether tracker.

---

# Round 5 — Qt Quick / QML (`qtdeclarative`)

`not-ours/qtdeclarative` at `0be90e31`, 2026-10-01 (shallow single-commit
clone): ~835k LOC C++ and 89k LOC QML across the QML engine, Qt Quick, Quick
Controls (templates plus the `basic`, `fusion`, `material`, `universal`,
`fluentwinui3`, `imagine`, `ios`, `macos`, `windows` and `native` styles),
Quick Dialogs, Layouts, Shapes, and the tooling (`qmllint`, `qmlls`,
`qmlformat`, `qmlcachegen`, `qmltc`, `qmlprofiler`, `qmlpreview`,
`svgtoqml`).

**The rule this round reads under.** Qt is GPL/LGPL/commercial. aether-ui has
already taken two QML *ideas* — States (`ui_states`/`set_state`) and Behavior
(`ui.transition`) — and the comment on the states API says why they are named
"QML-alike": "no QML source was read or ported here … an independent
implementation of a published idea". Everything below is the same: ideas to
re-implement against our own ABI, not code to port. (Where code *was* ported —
`apps/LisMusic`, from a Qt6/QML original — the directory says so and keeps the
upstream terms.)

Where it sits relative to F0: Qt Quick draws everything with its own scene
graph, and its `macos`/`windows`/`ios`/`native` Controls styles imitate native
look on top of that, like Slint's `qt` style. It is also the toolkit our states
and transitions were modelled on, so this round is partly "what the rest of
that model has".

## Q1. A UI-aware linter, with a plugin API — worth taking

**Theirs.** `qmllint` warns in typed categories — unqualified access,
unresolved types and aliases, missing, read-only and required properties,
deprecated APIs, unused imports, singleton access — and `QQmlSA::LintPlugin`
lets a project add its own domain rules. `qmlls` (the language server) shows
the same warnings in the editor as you type.

**Ours.** The traps are written down as prose, not checked. README
"Surfaces": inside `window`/`render_to`/`record`, a `root_*` verb is detached
and leaves "a window that maps but renders blank"; an interactive verb inside a
bounded surface is "diagnostic-inert" and only reported at runtime through
`surface_diagnostics`. AGENTS.md's "Idioms that keep biting" is the rest of the
list.

**Shape.** An aether-ui lint over `aetherc --emit=ast` (post-typecheck JSON,
already pinned by `tests/integration/aetherc_emit_ast` in aether), seeded with
exactly the two rules above, wired into `ci.sh`. Longer term the right home is
an Aether-level lint-plugin hook — Aether already has E-codes and W1001-style
warnings — so `ae check` runs it and the LSP shows it, which is the `qmlls`
half. The script version is an afternoon; the hook is an aether issue.

## Q2. Dialog buttons in the platform's order — worth taking

**Theirs.** `DialogButtonBox` with standard-button roles and a `buttonLayout`
of `WinLayout`, `MacLayout`, `KdeLayout`, `GnomeLayout` or `AndroidLayout` —
"a policy appropriate for applications on" each. OK/Cancel land where each
platform's users expect them without the app knowing the rules.

**Ours.** Native `alert()`s order themselves. Our own dialogs — the
`ui.dialog` frame, sheets, any app-built confirm row — order buttons however
the app wrote them, so one order is wrong on at least one platform. Swing §10
noted the same family of problem (`GroupLayout` asking the platform for
gaps).

**Shape.** `dialog_buttons(roles…)` with roles `accept`/`reject`/
`destructive`/`help`, ordered per backend: GTK4 → GNOME rules, AppKit → Mac,
Win32 → Windows. Small, and it is the AGENTS.md rule "state cross-platform
defaults explicitly" applied to layout.

## Q3. Persist an existing property, don't declare a new one — reshapes F2.2

**Theirs.** `Qt.labs.settings`: `Settings { property alias x: window.x;
property alias y: window.y }`. Nothing new is declared; an *existing* property
is aliased into the store and restored at startup. The canonical uses are
exactly the ones every desktop app needs: window geometry, splitter positions,
column widths, last tab.

**Ours.** F2.2 proposed `ui_state_*_pref`, which only helps state the app
declares itself. The commonest things to remember are widget properties we
already expose — `split_position`/`split_set_position`, `get_width`/
`get_height`, `tab_selected` — and none of them persist.

**Shape.** `persist(handle_or_state, "key")`: restore on creation, save on
change. Same per-app store as F2.2. It subsumes F2.2's declared-state case
rather than competing with it.

## Q4. Pooled delegates — a third answer to C3

**Theirs.** `TableView` and the item views have `reuseItems`: off-screen row
delegates go into a pool and are re-bound to new rows (with `pooled`/`reused`
signals), rather than destroyed and recreated or replaced by a stamp.

**Ours.** C3 proposed Swing's rubber-stamp via vg for large tables. `vlist`
already recycles a window of rows ("recycled window over a large item list"),
but tables and trees still ride a plain `listbox`.

**Why it changes C3.** A vg stamp is a drawn row, so every row becomes
invisible to a screen reader — the F2.7 problem, created by the fix for
scaling. A **pool of real native rows** scales the same way and stays
accessible. So C3's default should be Qt's pooling (extend `vlist`'s
recycling to tables and trees), with the vg stamp kept for drawn surfaces
where there was no accessibility to lose.

## Q5. Smaller things

- **List sections.** `ListView` `section.property` groups rows under headers,
  with `labelPositioning` for sticky headers. `listbox`/`vlist` have no
  grouping. Small-to-medium, and it rides on C2/S3's model work.
- **Lazy subtrees.** `Loader` with `active` (build when true, destroy when
  false) and `asynchronous` (build off the critical path). We found no lazy-
  build primitive in `ui/module.ae`, so tab pages, disclosure bodies and
  sheets are built up front. A `lazy { … }` that builds on first show is the
  small version.
- **Checkable commands in exclusive groups.** `Action.checkable` and
  `ActionGroup.exclusive` keep a view-mode choice in sync across the menu's
  check mark, a toggled toolbar button and its shortcut. Our `command` (C1)
  carries enabled but not checked. A small extension to C1.
- **Transitions keyed by from/to.** `Transition { from; to; reversible }` and
  sequential/parallel animation groups: a different animation for a→b than
  b→a. Our `transition` is per-property and direction-blind. Low priority;
  extends S2.
- **Explicit focus navigation.** `KeyNavigation.left/right/up/down/tab/
  backtab` and `FocusScope`. We have `focus`, `set_focusable` and
  `focused_widget`, and take the platform's default tab order. Overriding it,
  and arrow-key movement across a grid of buttons, are missing. How evenly the
  three backends let a focus chain be set needs checking before an API.
- **Binding-loop detection.** The QML engine reports "Binding loop detected"
  and names the property. Our state observers fire "on every set"
  (`aether_ui_backend.h`), so two `computed_*` cells that feed each other have
  nothing to stop them recursing, and nothing reports it. Detect re-entry on a
  cell and report the chain.
- **Render visualisation.** `QSG_VISUALIZE=batches|clip|overdraw` colours the
  scene graph by what the renderer is doing. The vg compositor's claims ("a
  static frame is produced once and blitted", "a fully covered frame is
  skipped") are asserted through driver paint counters; an
  `AETHER_UI_VISUALIZE` overlay would make them visible to a human too.
- **Markdown as a text format.** `Text` and `TextEdit` take
  `textFormat: MarkdownText`. That reshapes F2.5: a `text_format(h,
  "markdown")` on the existing text widget, not a separate `markdown_text`
  widget.
- **Catalogue.** Beyond F1 and S6: `SearchField`, `DoubleSpinBox`, `Dial`,
  `Tumbler`, `DelayButton` (press-and-hold to confirm — a good pattern for
  destructive actions like OpenDisk-ae's delete), `FontDialog` (native on all
  three: `GtkFontDialog`, `NSFontPanel`, `ChooseFont`), `MessageDialog`, and
  calendar building blocks (`MonthGrid`, `DayOfWeekRow`, `WeekNumberColumn`).
- **Asset-driven skins.** The `imagine` style draws every control from image
  assets supplied per state, so a designer restyles without code. A candidate
  model for drawn chrome alongside S6's style packs.

## Q6. The engine's jobs, done in straight Aether — worth taking

Declining QML's JavaScript engine is not declining what it is *for*. Each job
it does in Qt Quick has an Aether-only answer, most from pieces that already
exist:

| QML engine job | Aether in its place | State |
|---|---|---|
| Live preview / `qmlpreview` hot reload | `aeb --watch` rebuilds on save; relaunch the window. A full build of `examples/counter` is **~0.7 s warm, 1.0 s cold** on this Mac — already preview speed. | Exists in pieces; needs the relaunch loop wired. |
| Hot reload that keeps app state | Build the UI-building module with `--emit=lib`, load it with `std.dl`, and on change tear the tree down and re-run the new build function against the same `ui_state` handles (state lives in the host, not the library). | Plausible, unproven. The hard part is every closure the old library registered with the backend: they must be detached before `dl.close`, or the next event jumps into unmapped code. Spike before promising. |
| UI loaded at runtime (a QML file from disk or network) | A plugin is a `--emit=lib` library loaded with `std.dl`; an untrusted one runs as a sandboxed child via `contrib.host.aether` (compiled to a native binary, `LD_PRELOAD` sandbox with a grant list). | Mechanisms exist; no UI-plugin convention yet. |
| Interactive scene play (`qmlscene`, `qml` runner) | `ae repl` exists; a REPL session that drives a live window is the same idea without JS. | Speculative. |
| `qmlcachegen` / `qmltc` (compile QML to C++) | Not needed — Aether is already compiled. | n/a |
| Qt Design Studio round-tripping | A visual editor that writes Aether source, not a markup. | Large; waits for a consumer. |

Recommendation: wire the relaunch loop first (small, and it gives
`slint-viewer`/`qmlpreview`-class iteration today), then spike the
`std.dl` swap with one example to find out whether closure teardown is
tractable before designing an API around it.

## Declined

- **The QML/JavaScript engine itself.** A second runtime, a second language
  and a garbage collector inside a toolkit whose point is one compiled
  language over a C ABI. Its jobs are taken in Q6.
- **Pointer handlers as a general system.** `TapHandler` (with
  `longPressThreshold`), `DragHandler`, `PinchHandler`, `WheelHandler`,
  `HoverHandler` attached to any item, with grab arbitration. The desktop-
  relevant pieces — long-press and drag-to-move — are worth noting for vg
  scenes; the general system is a self-drawn toolkit's problem.
- **Anchors, attached properties, particles, `WorkerScript`, `LocalStorage`,
  `XmlListModel`.** Our stacks/grid plus `edge_insets`/`margin` cover layout;
  handle-taking setters (`tooltip`, `a11y_role`) do what attached properties
  do; Aether's stdlib covers workers, SQLite and XML.

## Already ahead or converged

- **States and Behavior** — taken, as QML-alikes, and the spring easing
  drives a real damped curve on win32 and macOS.
- **SVG compiled to source at build time.** `svgtoqml` emits QML shapes from
  an SVG; `apps/svg_transpile_module` / `svg_transpile_render_fn` already emit
  AeVG source (a `vg{}` module or a `render_<name>()` function) from one.
- **Two-way binding** — `bind_value` is the editable-widget⇄state link.

## What Qt Quick should envy back

- **No second runtime.** No JavaScript engine, no meta-object compiler, no
  QML type registration: the UI is compiled Aether calling a C ABI.
- **Real native controls**, again — Quick Controls' `macos`/`windows` styles
  imitate them on a drawn scene graph.
- **Licence.** aether-ui is MIT; Qt's choice of GPL, LGPL or commercial is a
  decision every Qt user has to make.
- **A driver against the real OS**, where Qt Quick Test runs in-process.

## If I had to pick three (Round 5)

1. **Platform-ordered dialog buttons** (Q2) — small, and wrong on at least one
   platform today.
2. **`persist()` for existing properties** (Q3) — window geometry, splitters
   and column widths; becomes F2.2's shape.
3. **A UI lint over `--emit=ast`** (Q1) — turns the two documented
   "renders blank / never fires" traps into failures.

And one design decision rather than new work: **C3 should pool native rows
(Q4) before it stamps drawn ones**, so scaling tables does not cost
accessibility.

Next tier: **the rebuild-and-relaunch preview loop** (Q6) — small, and it
supersedes the "hot reload is out of scope" note this document used to carry
in F3.2 and Round 4; the state-preserving `std.dl` swap after a spike.

---

# Round 6 — Zed's GPUI

`not-ours/zed` at `cac9d17`, 2026-10-05 (shallow clone): `crates/gpui` ~87k
LOC Rust, plus the platform crates it is assembled from — `gpui_macos` with
`gpui_apple` (Metal), `gpui_linux` (Wayland/X11) with `gpui_wgpu` (wgpu and
cosmic-text; there is no `blade` left in the tree), `gpui_windows`
(Direct3D 11, DirectWrite) and `gpui_web` (wasm), another ~57k between them.
Apache-2.0, so no licence fence this time.

Where it sits relative to F0: firmly on Flutter's side. Every pixel is
GPUI's, the element tree is rebuilt from state every frame and laid out by
taffy, and state lives in `Entity<T>`s owned by the `App` and reached through
contexts (`_ownership_and_data_flow.rs`, `docs/contexts.md`). There is no
markup — the UI is Rust, as ours is Aether. What earns it a round is that an
editor team built it for a keyboard-first desktop app, so the problems a
drawn toolkit has on the desktop — key dispatch, accessibility, IME,
deterministic tests — are solved in source here rather than on a roadmap.

## G1. One semantics tree for drawn content — how to do F2.7

**Theirs.** `Element` has `a11y_role`, `write_a11y_info` and
`a11y_synthetic_children` (`element.rs`). Each frame, every element with an
id and a role becomes a node; the nodes go to AccessKit as a `TreeUpdate`,
whose per-platform adapters diff it and drive NSAccessibility, UIA and AT-SPI
(`window/a11y.rs`). Actions come back per node (`on_a11y_action`; `on_click`
registers `Click` itself), and synthetic children let one element present
many nodes — the guide's example is a text field exposing its text runs. The
guide (`_accessibility.rs`) records two lessons: node ids must be stable
across rebuilds, and `text!` takes its id from its call site, so one `text!`
in a loop yields duplicates that "in release builds … get silently dropped".

**Ours.** Native widgets are accessible for free, overridden through three
hand-written per-backend paths (`docs/design/accessibility.md`). The drawn
half — canvases, vg scenes, `chromed` faces, any future C3 stamp — has no
nodes at all.

**Shape.** The vg layer emits flat semantics nodes `{id, parent, role, name,
value, bounds, actions}` in Aether, above the ABI; each backend gains one
call that publishes a canvas's nodes as the platform's *virtual children*.
Ids come from the vg node's own key, never from position, and a duplicate is
a reported error, not a dropped node. Hand-written adapters versus linking
AccessKit (a Rust dependency in a C build) is a decision to take on purpose.
**L**, and still F2.7's precondition for drawn chrome going default-on. It
softens Q4 a little: pooled native rows stay the default, but a stamp with
synthetic children is no longer inaccessible by construction.

## G2. A platform-relative modifier — worth taking

**Theirs.** `secondary-` "means `cmd` on macOS and `ctrl` on other
platforms" (`platform/keystroke.rs`), and every `KeyContext` starts with
`os=macos|linux|windows`, so one keymap says "Mod-S" and can still
predicate on the OS where it must.

**Ours.** The macOS backend maps `Ctrl` to the literal Control key on
purpose — "a spec asserting `Ctrl+B` must mean what it says" — and maps
`Primary` to Control too, the opposite of GTK's `<Primary>`. There is no way
to say "the platform's command key", and it already shows:
`enable_undo_shortcuts()` registers `Ctrl+Z`, so undo on a Mac is ⌃Z, not ⌘Z.

**Shape.** A `Mod` token resolved per backend (⌘ on AppKit, Ctrl elsewhere),
`Ctrl` left literal so the spec contract holds, `Primary` made an alias of
`Mod` (nothing in `ui/`, `examples/`, `apps/` or `tests/` uses it today), and
`enable_undo_shortcuts` moved to `Mod`. Menus display the resolved key.
**S.** Acceptance: on macOS `Cmd+Z` via `/window/key` undoes and `Ctrl+Z`
does not; GTK4 and Win32 the reverse. Falsify by resolving `Mod` to Ctrl
everywhere: the macOS half goes red.

## G3. Key contexts, and asking the keymap — the scope C5 left out

**Theirs.** Any element can declare `key_context("Editor")`, with key=value
entries too. A binding carries a predicate over the focus path (`==`, `!`,
`&&`, `||`, `>` for descendant — `KeyBindingContextPredicate`); matching
walks up from the focused element, deepest context first, then later-added
(user over defaults), and `Unbind` suppresses an inherited binding in one
context (`keymap.rs`, `key_dispatch.rs`). The keymap is also asked in
reverse: `keystroke_text_for(action)` is what a menu displays, and
`available_actions()` lists everything reachable from focus — Zed's command
palette is a list over it.

**Ours.** C5 shipped the chain without its scope: `keymap_attach(km)`
registers every key window-wide. Scope lives in `shortcut_when` and
`widget_shortcut` — and `widget_shortcut` promises "(or a descendant)" while
checking `focused_widget() == widget`. `focused_widget` returns the nearest
*registered* widget at the focus, so a container passed as the scope never
matches while one of its children has focus. In reverse, `menu_item_command`
shows `c.accel`, the string given to `command()`; after `keymap_bind` moves
the key, the menu still shows the old one.

**Shape.** `key_context(widget, "list")` and `keymap_bind_in(km, key, name,
"list")`. The backend reports the key and the focused handle; Aether walks
the ancestors and resolves deepest-first, so `widget_shortcut` becomes a
one-context special case with its descendant case fixed by construction.
Then `keymap_keys_for(km, name)` feeds menu and tooltip text, and
`commands_available()` returns enabled in-scope commands with label and key —
a palette is an app-side textfield and listbox. **M, needs C5.** Acceptance:
one key bound in two contexts fires A's handler with focus in a *child* of A;
after a rebind, `/menus` shows the new key. Falsify by matching the focused
widget alone: the child-of-A case goes red, which is today's bug.

## G4. Notifications through a queue, flushed once — reshapes Q5's loop item

**Theirs.** `notify` and `emit` push an `Effect`; `flush_effects` runs when
the outermost `App::update` finishes, and `pending_notifications`
deduplicates, so ten notifies in one update make one observer pass and no
observer runs mid-mutation (`app.rs`). Observing returns a `Subscription`;
dropping it unsubscribes.

**Ours.** Observers fire synchronously "on every set"
(`aether_ui_backend.h`), are append-only, and are implemented four times
(`fire_state_observers` in gtk4, macOS, win32 and uikit). So
`computed_s(…, first, last)` recomputes twice when both change and sees the
inconsistent state between; an observer of a destroyed widget keeps firing;
and Q5's binding loop is stack recursion.

**Shape.** Move the observer table into Aether — one copy, not four.
`ui_batch { … }` defers observer passes to its close; outside a batch every
set is its own batch, so simple apps see no change. Deduplicate per cell;
`state_observe` returns a token for `state_unobserve`. Q5's loop detection
becomes a bound on flush iterations, and the queue is the chain it reports.
**M.** Acceptance: `computed_s` over two cells both set in one `ui_batch`
recomputes once (a counter the driver reads). Falsify by flushing per set.

## G5. Background work owned by what it updates — worth taking

**Theirs.** A `Task<T>` is a value — hold it or `detach()` it; dropping it
cancels. `cx.spawn` hands the future a `WeakEntity`, so a completion after
its view has gone fails instead of touching dead state; async contexts are
fallible because "the context may outlive the window or even the app".

**Ours.** `background(work, done)` runs `done` whatever happened meanwhile,
and `std.worker` says "no cancellation". Close the window that started a scan
and its `done` runs against handles whose registry slots have self-NULLed.

**Shape.** `background_for(owner, work, done, dropped)`: if `owner` is no
longer live (the registry knows), call `dropped(result)` instead of `done`,
so the result is still freed; plus a cooperative `background_cancel(job)`
that `work` polls. Actor-owned work already has this through the actor's
lifetime; this is the closure path. **S–M.**

## G6. A test platform that records, and a clock the spec advances

**Theirs.** The test platform records what the app asked of the OS and lets
the test answer: `simulate_prompt_answer`, `simulate_new_path_selection`,
`opened_url`, `shown_system_notifications` (`app/test_context.rs`). The
`TestDispatcher` runs tasks in seeded random order on a fake clock —
`advance_clock`, `run_until_parked`, `SEED`/`ITERATIONS` to replay or sweep
(`platform/test/dispatcher.rs`, `seeds.rs`).

**Ours.** Notifications already work this way (`/notifications` lists,
clicks, dismisses). Dialogs do not: under `AETHER_UI_HEADLESS` a file chooser
returns `""` (`aeui_run_file_chooser`), so no spec can reach any app's "user
picked a file" branch; and GTK4's `open_url` calls `gtk_show_uri` with no
headless check, so a spec that clicks a link launches a browser on the CI
box. Time is wall-clock: `AETHER_UI_NO_ANIMATION` removes transitions rather
than stepping them, and the chord timeout can only be waited out.

**Shape.**
- **Scripted answers (S).** `POST /prompts/answer?kind=open&value=/tmp/x`
  queues an answer a headless `open_file`/`save_file`/`pick_folder`/`alert`
  consumes before falling back to cancel; `GET /prompts` lists what was
  asked; headless `open_url` records to `GET /opened_urls`. Both servers.
- **A manual clock (M).** `AETHER_UI_CLOCK=manual` and `POST /clock/advance
  ?ms=`, with timers, transition ticks and the chord timeout reading one
  clock; each backend's timer source needs a seam. Seeded task ordering is
  declined: scheduling is Aether's runtime, not the toolkit's.

## G7. The creation site as identity — Aether already has the primitive

**Theirs.** Elements record their construction site with `#[track_caller]`;
the inspector's id is the nearest global id plus that site plus an instance
ordinal (`inspector.rs`), and picking an element shows where it was built.

**Ours.** Widgets are registry handles that renumber when build order
changes, and every driver route and `apps/inspector` address by handle. But
Aether's `line: int = __LINE__, file: string = __FILE__` *default
parameters* substitute the caller's site (`aether/docs/language-reference.md`,
"Source-location intrinsics").

**Shape.** Widget builders gain those defaults; `/widgets` entries gain
`"src":"main.ae:42"` plus an ordinal for repeats from one site (GPUI's
`text!`-in-a-loop lesson); `GET /find?src=main.ae:42&n=0`; the inspector
shows the site. A third lookup key beside S1's role and name, stable across
edits anywhere but that line. Check first that defaulted parameters compose
with trailing-block builders. **M**, mechanical.

## G8. Stall detection on the UI thread — worth taking

**Theirs.** `profiler/journal.rs` rings foreground work — task polls, action
handlers, input, draws — and `profiler/hang.rs` flags any item over a
threshold *or* an interval over the frame budget, since "many small pieces
of work can drop a frame … as thoroughly as one long stall".

**Ours.** Paint counters prove the compositor skips work; nothing measures
how long the UI thread was blocked — the failure `background()`'s own comment
cites ("scanning a disk in 15 ms timer slices on the UI thread").

**Shape.** Time each closure dispatched on the UI thread into a ring and
serve `GET /debug/stalls?over=50`, so a spec can assert a full scan never
blocks the UI for more than 50 ms. First, and overdue anyway: the GTK4
backend alone has ~50 direct `c->fn(c->env)` call sites to route through one
invoke helper per backend. **S–M.** Pairs with Q5's `AETHER_UI_VISUALIZE`.

## G9. IME for drawn text — consider, not yet

**Theirs.** `EntityInputHandler` (`input.rs`) is the IME's view of an
element: `text_for_range`, `selected_text_range`, `marked_text_range`,
`unmark_text`, `replace_text_in_range`, `replace_and_mark_text_in_range`,
`bounds_for_range`, `character_index_for_point`. On the web, which has no
such protocol, `gpui_web/src/ime_mirror.rs` mirrors the document into a
hidden `<textarea>` instead.

**Ours.** Text fields stay native "forever, because IME is the
disqualifier" (`vg-drawn-controls.md`), and rightly. But maerkdown is already
a drawn editor taking key strings from `canvas_on_key`, with no composition
path for an input method to talk to. GPUI sizes the fix — eight methods per
editable canvas, one adapter per backend (NSTextInputClient, GtkIMContext,
IMM32/TSF) — a bounded **L** for when maerkdown or Round 2's second text-tier
consumer needs it.

## G10. Smaller things

- **Interaction styles as data.** Beside `hover`/`active`, `div` has
  `focus_visible`, `group_hover`/`group_active` (hover a row, restyle a
  child) and `drag_over`. AeCS has `st_hover`/`st_active`; the rest are
  selector extensions, mostly for drawn chrome. `drag_over` goes with F2.4.
- **A retargetable spring.** `SpringConfig::step` "preserves velocity,
  allowing an interrupted spring to be retargeted without restarting it";
  `step_ramp` follows a dragged target (`spring.rs`). Ours is the fixed
  `1 - e^(-6t)·cos(9t)`; for vg, a stateful step is small and widens F2.6.
- **Tab order as groups.** `tab_index`/`tab_stop`/`tab_group` build ordered
  paths (`tab_stop.rs`), not one global number — the shape for Q5's focus
  navigation.
- **A list that follows its tail.** `list` keeps measured heights in a sum
  tree with `FollowMode::Tail` for logs (`elements/list.rs`); `vlist` is a
  fixed window of rows. Fold into C3/Q4 when a log view needs it.
- **Close and quit.** `on_window_should_close` returns a bool and quit
  handlers get a bounded `SHUTDOWN_TIMEOUT` (200 ms) — F2.3's shape, still
  open here, plus a time bound.

## Declined

- **Drawing everything, with its own text system.** F0 stands: GPUI pays with
  a renderer per platform, a text stack per platform, AccessKit and an IME
  protocol. `gpuview`/`native_view` already give an app a GPU surface.
- **Rebuilding the tree every frame under taffy.** Right for a drawn toolkit,
  wrong for widgets the OS retains; `Entity::cached` views are GPUI winning
  back what a retained tree has by default.
- **Entity/Context ownership.** It solves Rust's shared-mutability problem,
  not a UI one. G4 takes the UI part, the effect queue.
- **Tailwind-style styling** (`.px_4().rounded_md()`). Style at the call site
  is what C8 and the AeCS cascade avoid; G10 takes the state selectors only.
- **The touch gesture arena** (`gestures.rs`) — a self-drawn toolkit's
  problem; the UIKit backend uses native recognisers.

## Already ahead or converged

- **Reuse what didn't change.** Cached views reuse prepaint and paint ranges
  until notified (`view.rs`); the retained compositor blits static frames —
  and ours is asserted through driver paint counters.
- **Off the UI thread and back** — `background(work, done)`; G5 adds
  ownership. **Container queries** — `on_layout`. **An a11y dump for tests**
  (`debug_a11y_tree_json`) — `GET /widget/{id}/a11y`.

## What GPUI should envy back

- **Native controls**, which make G1 and G9 the platform's job, not a
  subsystem each.
- **A driver outside the process, against the real OS**, usable by an agent
  (S1); `TestAppContext` is in-process.
- **A C ABI**, where GPUI is a Rust-only API whose README warns of "breaking
  changes between versions".
- **No GPU in the path for ordinary widgets**, and no full Xcode install,
  which GPUI's README requires for Metal.

## If I had to pick three (Round 6)

1. **A `Mod` modifier** (G2) — small, and undo is ⌃Z on a Mac today.
2. **Key contexts** (G3) — finishes C5's scope and fixes `widget_shortcut`'s
   descendant case on the way.
3. **Scripted answers to dialogs** (G6, first half) — small, and every app's
   "user picked a file" branch becomes testable.

And one design decision rather than new work: **F2.7 should be one semantics
tree built in Aether with a thin virtual-children adapter per backend** (G1),
not three hand-grown paths.

Next tier: the notification queue (G4), stall detection (G8), owned
background work (G5), creation-site identity (G7), then the manual clock.
IME for drawn text (G9) waits for maerkdown.
