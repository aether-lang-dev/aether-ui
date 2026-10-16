# REPLY: a datagrid that takes the height it is given

**RESOLVED** by the commit that adds this file, as the ask's second way:
`datagrid_set_height(g, px)` shows the rows that fit in `px` (the first
scrolling one however tall), at most the `vis_rows` the grid was made
with, and `0` all of them again; `datagrid_height(g)` reads it. The
vertical scrollbar follows, and a row past the height says nothing, as a
hidden one does. A page that wants a grid to grow makes it with as many
`vis_rows` as it may need and gives it the height it has room for. All in
`ui/module.ae` (the rows' budget, `_datagrid_full_h`), so the same on every
backend.

Proved by `tests/datagrid_demo/spec_datagrid_demo.ae` ("a grid given less
or more height"): 70 passing on AppKit, GTK4 (aether-linux-2604, ae-x64),
Win32 (win11) and Android (emulator); UIKit compiles -Wall -Werror (its
simulator's simctl hangs at install here).
