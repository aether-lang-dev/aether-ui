# Turtle — a small turtle-graphics editor

A code editor + canvas + a picker of bundled examples, wired to a tiny
interpreted turtle-graphics command language. Type (or pick) a script,
press **Run**, watch it draw.

## Credit

Ported from **Swiby**'s `demo/turtle/` (Jean Lazarou's JRuby-over-Swing GUI
DSL, <https://github.com/j-lazarou/swiby>, 2007) — the idea of a live turtle
editor and the bundled example scripts (`flower`, `star`, `geometric_form`,
`house`) are theirs. This is a fresh implementation over aether-ui's `canvas`
and `textarea` widgets (`turtle_interp.ae`), not a line-by-line port: the
original's language leaned on JRuby's dynamic `method_missing` dispatch
(`to name(:params) { ... }` procedures, `@param` references, `unless`
conditionals), which has no direct Aether equivalent.

## What this port is — and is not

**v1 grammar** (see `turtle_interp.ae` for the full list): movement
(`forward`/`fd`, `back`/`bk`), turning (`left`/`lt`, `right`/`rt`), pen
state (`penup`/`pu`, `pendown`/`pd`), `home`, `color r g b`, and `repeat N
[ ... ]` with arbitrary nesting.

**Not ported (v1)**:
- **User-defined procedures** (`to name(:args) { ... }`) and recursion.
  Swiby's `square_spiral_90.turtle` — a recursive `spiral` procedure that
  grows its step size each call — genuinely needs this and isn't one of the
  bundled examples here for that reason.
- **Localized command names** (Swiby's `demo/turtle/lang/fr.rb` — French
  turtle commands). A nice stretch goal, not built here.

The four bundled `examples/*.turtle` scripts are hand-adapted from Swiby's
originals to the v1 grammar (`repeat(N){...}` → `repeat N [ ... ]`,
`up`/`down` → `penup`/`pendown`, and `house.turtle`'s `rectangle(w, h)`
procedure calls inlined to their body) — see the comment at the top of each
file for exactly what changed.

## Build and run

```
./build.sh apps/turtle/turtle.ae build/turtle
./build/turtle
```
