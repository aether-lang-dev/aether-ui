<!-- STATUS 2026-10-16: NEW. Raised by the spreadsheet (aether-lang-dev/spraedsheet,
     TODO.md 3.9). -->

# A datagrid selection that glides to where it goes

**From:** the spreadsheet app, step 3.9: as in recent Excel, the selection
outline animates from the old cell to the new one (a short ease, about
100 ms), and a setting turns it off.

## The ask

`datagrid_selection_glide(g, ms)`: the selection's outline moves from its
old rectangle to its new one over `ms` milliseconds on the frame clock
(`0` off, the default), on every backend and drawn by the grid itself (the
page cannot draw over the grid's cells). The driver should be able to tell
a glide is running and where the outline is, so a spec can prove it ends
on the new cell and that `0` makes it jump.

## Meanwhile

The spreadsheet has no glide; its `SETANIMATIONS(0)` waits for this.
