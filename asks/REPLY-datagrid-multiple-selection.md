# REPLY: a datagrid selection of several areas

**RESOLVED** by the commit that adds this file. A Ctrl or Cmd click keeps
the selection so far as an area and starts another at the cell; a plain
click, a key or a select from code makes it one area again. Every area is
painted as the selection is; `datagrid_is_selected` counts them.
`datagrid_add_area(g, ar, ac, r, c)` adds one from code,
`datagrid_clear_areas(g)` drops them, `datagrid_areas(g)` gives the ones
before the current, `"ar,ac,r,c;..."` (model rows, on-screen columns, as
`datagrid_selection`). At most 64 besides the current. There is no
Ctrl+drag: the grid has no drag-select, Shift+click and Shift+keys extend.

To drive it, `POST /widget/{id}/click?mods=N` holds modifier keys for that
one click, as `modifiers()` reads them on every backend
(`aether_ui_modifiers_override`, consulted first by each backend's
`aether_ui_modifiers_impl`); uidriver's `click_widget_mods(id, mods)`.

Proved by `tests/datagrid_demo/spec_datagrid_demo.ae` ("a Cmd/Ctrl click
adds an area"): 69 passing on AppKit, GTK4 (aether-linux-2604, ae-x64),
Win32 (win11) and Android (emulator). UIKit compiles with -Wall -Werror and
links (ci.sh Phase 1e's check); the iOS simulator's simctl hung at install
here, so it was not driven.
