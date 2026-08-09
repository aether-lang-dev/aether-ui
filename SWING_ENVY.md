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
  bounds this for lists; `table_col_delegate` does not obviously do so for
  tables.
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

**Where we are.** There are `drop`/`clipboard` mentions in the surface but
no unified transfer protocol, and no flavour negotiation.

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
