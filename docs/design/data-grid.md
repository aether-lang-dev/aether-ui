# A data grid: design note

Status: phase 1 built (2026-10-10): `datagrid` in `ui/module.ae`, with
`vslider` added for its scrollbars; the scrolling was decided as its own
viewport with native sliders (option B below, Paul's call), not a native
scroll view. Phases 2-6 open. Asked for by sae's spreadsheet (demo 18), which
built about 120 lines of grid by hand in `vg`; the same widget would make data
grids, log viewers and property sheets cheap. The ask, from a cloud session
building that demo: Swing's `JTable`, QML's `TableView` or Flutter's
`TwoDimensionalScrollView` — virtualised cells, resizable and reorderable
columns, a selection model, cell editors, a sticky header, scrolling both
ways, and something a screen reader can read.

## What exists (survey, 2026-10-10)

- `table(cols, cell)` (`ui/module.ae` ~5415) is composed in Aether on every
  backend: a header `hstack` of flat buttons with fixed widths over a
  `listbox` body. It has sorting (`table_sorter`, a stable insertion sort
  with a view/model split), filtering (`table_filter`), delegate columns
  (`table_col_delegate`), single-row selection and Up/Down keys inherited
  from `listbox`, and roles `table` / `columnheader`.
- `listbox` has single, multi-toggle and standard (click, ctrl/cmd, shift
  range) selection, one roving tab stop, Up/Down/Home/End/Enter, and row
  drag-reorder.
- `vlist` is the only virtualised list, and only one-dimensional: native on
  GTK4 (GtkListView), AppKit (a one-column NSTableView) and Android
  (ListView), composed on Win32 and UIKit.
- Missing for a grid: column or row virtualisation in `table`, column resize
  and reorder, cell (not row) selection and ranges, Left/Right cell
  movement, in-place editing (`docs/design/toolkit-envy.md` §2: "nothing
  equivalent"), a sticky header (today it scrolls away unless the app
  scrolls only the body), and `grid` / `row` / `gridcell` roles with row and
  column indices (rows still say `listitem`).

## The three ways to build it

| | A. Native per backend | B. Composed, virtualised | C. Drawn in vg |
|---|---|---|---|
| What | GtkColumnView, a multi-column NSTableView, Win32 ListView (LVS_REPORT, owner data), UICollectionView, Android RecyclerView | Aether code over real widgets: a 2D window of live cell widgets, recycled as it scrolls | one canvas; cells are vg text and rects |
| Accessibility | the platform's own grid | real widgets with `grid` / `row` / `gridcell` roles and indices | needs the vg semantics bridge (wishlist item 9) first |
| Cost | five controls with five feature sets; editing and resize differ on each; months | one implementation, the existing listbox and selection code extended | cheapest to draw, dearest to make accessible |
| Parity | hard (each control's quirks become ours) | by construction | by construction |
| Scale | best | thousands of rows fine with a window of ~hundreds of live cells | best |

## Recommendation: B, with a contract that would let A or C replace it

Build the grid composed and virtualised (B), because parity and
accessibility come by construction and most of the parts exist. Define it
behind a small model/renderer/editor contract (the "one renderer contract"
that `toolkit-envy.md` puts first), so a backend can later swap in a native
control (A) for the parts where native wins, without changing apps.

The contract:

- **Model:** `row_count`, `col_count`, `cell_text(r, c)`, and optionally
  `cell_kind(r, c)` for a renderer to choose by. A function-backed model, so
  a 1,000,000-row source is never materialised.
- **Columns:** width (resizable, with min and max), a header label, an
  order (reorderable, kept as a view-to-model index map, like the sorter's
  map), frozen leading columns.
- **Renderer:** a closure building or updating a cell widget from
  `(r, c, value, selected, focused)`, reused for the visible window only
  (QML's pooled delegates, Q-round of toolkit-envy).
- **Editor:** begins on Return, F2, double click or typing; a `textfield`
  overlaid on the cell, committed by `on_submit` (now on every backend) or
  Tab, cancelled by Escape; `on_edit(r, c, text)` decides what is stored.
- **Selection:** cell, row or column, single or range, with an anchor and a
  focused cell; extends `listbox_selection_mode`'s vocabulary.
- **Keyboard:** arrows move the focused cell, Shift extends, Home/End,
  PageUp/PageDown, Tab/Shift+Tab along a row, Return begins or commits an
  edit.
- **Scrolling:** a body scrolled in both directions, the header row (and any
  frozen columns) pinned and kept in step with the body's offsets.

## Phases, each with its proof

1. **Static grid with a sticky header.** DONE: `datagrid` (148 widgets for
   a 100,000 x 50 model; `spec_datagrid_demo`). A function-backed model, fixed
   columns, a 2D window of live cells, both-ways scrolling, the header and
   frozen columns pinned. Spec: 100,000 x 50 model, at most a few hundred
   live widgets (counted over the driver), and the header still at the top
   after scrolling to row 90,000 and column 40.
2. **Selection and keys.** Cell focus, ranges, the key table above. Spec:
   arrows, Shift-ranges and Home/End read back as selection state.
3. **Column resize and reorder.** Header drag handles (a driver verb to
   drag one), the view-to-model column map. Spec: widths and order read
   back; cells follow.
4. **Editing.** The overlaid editor with `on_submit`, Tab and Escape. Spec:
   edit, commit, cancel, and the model's `on_edit` call.
5. **Accessibility.** `grid` / `row` / `gridcell` / `columnheader` roles
   with row and column index and count on every backend (GTK4
   GtkAccessible, AppKit NSAccessibility table/cell protocols, Win32 UIA or
   MSAA, UIKit, Android `CollectionInfo`). Spec: the driver's a11y read-out
   of a cell names its row, column and header.
6. **sae.** `ui.grid` (or `ui.table`) for pages, and sae's spreadsheet moves
   onto it, dropping its hand-built grid.

Each phase runs on every lane (macOS, the iOS simulator, win11, Android,
GTK 4.6 and 4.22, ae-x64) before it lands, as the rest of aether-ui does.

## Open questions

- Whether the first phase should reuse `vlist`'s native backings for the
  rows (GTK4, AppKit, Android) with composed columns inside each row, or
  stay fully composed on every backend for one code path.
- How much of the current `table` API to keep: the grid could become what
  `table` is built on, or `table` stays as the simple form.
