<!-- STATUS 2026-10-16: RESOLVED (REPLY-datagrid-header-click.md). Raised by the spreadsheet (aether-lang-dev/spraedsheet,
     TODO.md 1.1). -->

# A datagrid's column header clicked: told to the page

**From:** the spreadsheet app, step 1.1: a click on a column's header
selects the whole column, as Excel's does (Shift+click extending it, a
click on the corner selecting all).

## What there is

A header label's press goes to `_datagrid_header_drag` (`ui/module.ae`):
a drag on the grip resizes, a drag on the label moves the column. A press
and release with no move is dropped; nothing tells the page.

## The ask

`datagrid_on_header_click(g, fn(c, mods))`: a header pressed and let go
without moving (under the drag threshold), `c` the model column, `mods`
the modifiers, as `on_key` gives them; the top-left corner header as
`c = -1`. A drag still resizes or moves as now. On every backend (the
header is Aether-level widgets, so likely backend-free) and over the
driver (a `/widget/{header}/click` firing it), so a spec can prove a
column selected.

## Meanwhile

The spreadsheet selects a row by a click on its number (a cell of the
grid's frozen column), a column by Ctrl+Space, all by Ctrl+A, and any of
them by `SELECTRANGE("B:B")`.
