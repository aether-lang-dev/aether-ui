<!-- STATUS 2026-10-16: NEW. Raised by the spreadsheet (aether-lang-dev/spraedsheet,
     TODO.md 3.8) through sae's ui.datagrid. -->

# A datagrid's frozen columns set at run time, and split panes

**From:** the spreadsheet app (`../spraedsheet`, a sae app), step 3.8:
Excel's Freeze Panes (rows and columns) and Split.

## What there is

`datagrid(rows, cols, vis_rows, vis_cols, frozen)` takes its frozen
leading columns at creation (the spreadsheet uses 1, for the row numbers),
and `datagrid_freeze_rows(g, n)` sets frozen rows at any time. There is no
way to change the frozen columns afterwards, and no split.

## The ask

1. `datagrid_freeze_cols(g, n)`: the first `n` columns stay put as the
   rest scroll across, set and changed at any time, as
   `datagrid_freeze_rows` is (the grid's own leading columns counted in
   `n`, or added to it: whichever reads better). The driver's widget tree
   should show it (a frozen column's cells keep their x as the grid
   scrolls), so a spec can prove it.
2. A split: two (or four) panes over one grid's model, each scrolling on
   its own, a draggable divider between them (Excel's View > Split);
   `datagrid_split(g, row, col)`, `0, 0` to remove it.

On every backend (GTK4, AppKit, Win32, UIKit, Android) and the headless
one; if the grid is all Aether-level widgets in `ui/module.ae`, (1) may be
backend-free.

## Meanwhile

The spreadsheet freezes rows only; `FREEZEPANES(rows, cols)` refuses
columns and says why.
