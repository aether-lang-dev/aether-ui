# REPLY: frozen columns at run time, and split panes

**RESOLVED** by the commit that adds this file.

1. `datagrid_freeze_cols(g, n)`: the n columns after the grid's own frozen
   ones (a spreadsheet's row numbers) stay put as the rest scroll across,
   set and changed at any time; 0 frees them; `datagrid_frozen_cols(g)`
   reads it. The grid's column positions are fixed at creation (`npos`),
   so freezing more makes fewer scrolling positions; at most all but one.
   Newly frozen columns are drawn as data, not greyed as the grid's own.
2. Split panes need nothing new here: a page makes a second grid over the
   same model (the same cell, style and edit functions), each scrolling on
   its own, which is what Excel's Split shows. The spreadsheet does so.

Proved by `tests/datagrid_demo/spec_datagrid_demo.ae` ("columns frozen at
run time"): 71 passing on AppKit, GTK4 (aether-linux-2604, ae-x64), Win32
(win11) and Android (emulator); UIKit compiles -Wall -Werror.
