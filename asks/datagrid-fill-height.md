<!-- STATUS 2026-10-16: NEW. Raised by the spreadsheet (aether-lang-dev/spraedsheet,
     TODO.md 3.0.4) through sae's ui.datagrid. -->

# A datagrid that takes the height it is given

**From:** the spreadsheet app (`../spraedsheet`, a sae app), step 3.0.4 of
its TODO.md: its fifteen rows of buttons become panels on demand, one open
at a time under the grid, and the grid should take the height back when
none is open, as Excel's grid fills the window.

## What blocks it

`datagrid(rows, cols, vis_rows, vis_cols, frozen)` makes `vis_rows` x
`(frozen + vis_cols)` cell widgets once (`ui/module.ae`, the comment above
`datagrid`), and its height is `vis_rows * row_h` (`_datagrid_*`, e.g. the
`nh > g.vis_rows * g.row_h` clamp). Nothing grows the grid when the
widgets below it are hidden (`set_visible(h, 0)`) or the window is made
taller: the space goes to nothing.

## The ask

A grid that fills the height its container gives it, on every backend
(GTK4, AppKit, Win32, UIKit, Android, and the headless one for the
driver), by any of:

1. an option at creation (`datagrid_fill_height(g, on)`), the grid
   expanding vertically and making or dropping row widgets as its height
   changes (a resize, a sibling shown or hidden), `vis_rows` its minimum; or
2. `datagrid_set_visible_rows(g, n)`, so a page can size it itself once it
   knows the height (`ui.height` of a container).

Either way the driver's `/widget/{grid}` `h` should say the new height, so
a spec can prove "the grid is taller when no panel is open".

## Meanwhile

The spreadsheet hides and shows its panels under a grid of fixed height
(25 rows); its spec checks the panels, not the grid's height.
