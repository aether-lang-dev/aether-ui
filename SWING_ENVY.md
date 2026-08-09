# Swing envy

What Swing got right that aether-ui has not, read from the JDK source
(`javax/swing/**`) rather than from memory of using it.

Swing is 1997 technology in a language with a different family tree, and it
carries obvious mistakes — the AWT peer inheritance, `JComponent` as a
god-class, `Serializable` on everything, a `JTable` that knows about
printing. The interesting thing is that almost none of its *good* ideas
depend on inheritance. They are protocols, indirection tables and
model/view splits, and they port to a compositional DSL essentially intact.

That is the premise here: take the ideas, leave the class hierarchy.

Paul's observation about the Netscape IFC team is the right frame. If they
had their time again — free of AWT, free of the requirement that everything
descend from `Component` — most of what follows would look *more* natural,
not less. `JFrame` and `JInternalFrame` sharing behaviour through a
`RootPaneContainer` interface rather than a distant common ancestor is
exactly the shape a compositional toolkit reaches for anyway.

---

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

## If I had to pick three

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
