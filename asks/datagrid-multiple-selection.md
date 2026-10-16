<!-- STATUS 2026-10-16: RESOLVED (REPLY-datagrid-multiple-selection.md). Raised by the spreadsheet (aether-lang-dev/spraedsheet,
     TODO.md 1.2). -->

# A datagrid selection of several areas

**From:** the spreadsheet app, step 1.2: Ctrl/Cmd+click and Ctrl+drag add
an area to the selection, as Excel's do, and every command then works over
all of them (the app already runs its commands over "A1:B2,D4:D9").

## What there is

One selection: an anchor and a cell (`datagrid_selection(g)` gives
`[anchorRow, anchorCol, row, col]`, `datagrid_select_range` sets it).

## The ask

1. Ctrl (Cmd on macOS) + click or drag adds an area, keeping the others
   drawn; a plain click goes back to one.
2. `datagrid_selection_areas(g)` -> `"r1,c1,r2,c2;..."` (model rows,
   on-screen columns, as `datagrid_selection`), and
   `datagrid_select_areas(g, areas)` to set them; `on_cell_select` as now
   for the active cell.
3. Over the driver, so a spec can add an area and read them back.

On every backend (the grid is Aether-level widgets, so likely
backend-free).

## Meanwhile

The spreadsheet takes several areas in its commands and in
`SELECTRANGE("A1:B2,D4:D9")`; the grid shows the first.
