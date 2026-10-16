# REPLY: a datagrid's column header clicked

**RESOLVED** by the commit that adds this file:
`datagrid_on_header_click(g) callback |c| { ... }` (and the block form
`on_header_click()`): a header label pressed and let go with under 4 px of
travel is a click, told with the model column (a frozen column's too, so
the corner over the row numbers is column 0); a drag still moves the
column and a drag on the grip still resizes it. Aether-level in
`ui/module.ae` (`_datagrid_header_drag`), so the same on every backend.

Proved by `tests/datagrid_demo/spec_datagrid_demo.ae` ("a header pressed
and let go where it was is a click"): 68 passing on AppKit (this Mac),
GTK4 (aether-linux-2604, ae-x64 under xvfb), Win32 (win11) and Android
(the emulator, aedroid emu). The iOS simulator's simctl hung at install
on this Mac, so UIKit is compiled and linked (ci.sh Phase 1e) but not
driven for this.

The spreadsheet (`../spraedsheet`, TODO.md 1.1) selects a column by its
header with it, through sae's `ui.datagrid_on_header_click`.
