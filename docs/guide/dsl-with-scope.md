# DSL with Scope

> "DSL with scope" — Yukihiro "Matz" Matsumoto, when asked what to call the
> builder-block style of programming.

Aether UI (and AeVG, its vector-graphics layer) is built as a **DSL with
Scope**: nested blocks that read declaratively but are *executed code*, with
an **implicit receiver** so children attach to their enclosing scope without
explicit parent-child plumbing.

## The lineage

This pattern runs through:

- **Smalltalk** — block-based APIs (`do:[…]`), the original implicit-receiver
  cascades.
- **Ruby** — Shoes (`Shoes.app do … end`), Sinatra, RSpec; blocks with
  `instance_eval`.
- **Groovy** — SwingBuilder, MarkupBuilder; closures with a delegate.
- **Kotlin** — Jetpack Compose, TornadoFX.
- **Swift** — SwiftUI.

Paul Hammant's
[*That Ruby and Groovy Language Feature*](https://paulhammant.com/2024/02/14/that-ruby-and-groovy-language-feature.html)
surveys the same calculator written across all of these and plots them on a
terse-vs-elegant axis. The defining trait, as that piece draws out (quoting
Gemini on the distinction): a markup language like HTML is **parsed into a
structure** (the DOM) for *secondary* actioning; a DSL-with-Scope is
**interpreted or compiled like any 3GL** — the block *runs*, creating elements
imperatively as it goes. It is pseudo-declarative: it reads like a description
of structure, but every line is real code that can loop, branch, and capture
state.

## How Aether does it

Aether's mechanism is documented in the language repo's
[`docs/closures-and-builder-dsl.md`](https://github.com/aether-lang-org/aether/blob/main/docs/closures-and-builder-dsl.md).
Three pieces matter here:

1. **Trailing blocks** — any call may be followed by `{ … }`, run inline as
   DSL structure (`window("App", 400, 200) { … }`).
2. **The `_ctx` implicit receiver** — a function whose first parameter is
   `_ctx: ptr` has that parameter *hidden from callers* and *auto-injected*
   from the builder-context stack inside a trailing block. This is how
   `text("hi")` inside a `vstack { … }` attaches to the vstack without anyone
   passing the parent. (Matz's "implicit receiver"; Groovy's delegate;
   Ruby's `self` in an `instance_eval` block.)
3. **`builder … with <factory>` — "configure then execute"** — a `builder`
   function flips the order: the trailing block runs *first* (filling a
   config object the `with` factory created and pushed as `_ctx`), then the
   function *body* runs. This is the after-block epilogue: the block builds
   eagerly and in place, and the function gets to act *after* it closes.
4. **UFCS — `x.f(args)` → `f(x, args)`** — the orthogonal *value-chain* axis
   (Aether #928, cross-module #934). When a free function's first parameter
   matches `typeof(x)`, it's callable in method position. The explicit-handle
   modifiers (`style_font_size`, `width`, `height`, …) each return their
   handle, so they compose left-to-right — see
   [Chaining explicit-handle modifiers](#chaining-explicit-handle-modifiers-ufcs)
   below.

## Surfaces are DSL-with-Scope all the way down

The Aether UI entry point is a **surface** scope — `window`, `render_to`, or
`record` (see the [README](../README.md#surfaces-window--render_to--record)).
Each is a `builder … with` function:

- The `with` factory creates the surface's content container and pushes it as
  `_ctx`, so the block's widgets/shapes attach to the surface.
- The block runs first — eagerly, in order — populating the surface.
- The body runs *after*: `window` runs the event loop; the bounded surfaces
  (`render_to`, `record`) finalize and return.

```aether
window("Calculator", 280, 260) {     // surface scope (lived: owns the loop)
    root_vstack(4) {                  // layout scope
        text_bound(display, " ", "")
        hstack(4) {                   // nested scope
            btn("7") callback { call(digit, 7) }
            button("+") {             // leaf + its config block
                bg_color(0.95, 0.85, 0.5, 1.0)
                onclick() callback { call(apply_op, plus) }
            }
        }
    }
}
```

Every `{ }` is a scope; every verb inside acts on the ambient receiver of its
enclosing scope. The `window` body — the loop — runs once the whole tree is
built, because `window` is a `builder`.

### Why a surface, not `app_run`?

The earlier `app_run(title, w, h, root)` bundled three jobs — create the
window, mount the tree, run the loop — and forced that *lived* shape onto every
program. But a drawing rendered to a PNG, printed to paper, or captured for a
test needs no event loop and no window: it establishes a surface, the block
runs into it, and control returns at `}`. Only a live window has a life of its
own that ends on an external event (the close button), so only `window` runs a
loop. Splitting the one verb into three surface kinds makes the lifecycle
honest — and means "applications need not have a fat UI": a program can open a
`record` or `render_to` surface, draw, and exit, never touching a window loop.

## Composite widgets take a block too

A composite like the data grid is configured the same way a button is: its
block runs with the grid as the receiver, and the grid's own verbs act on
it. The SAME verbs exist by handle (`datagrid_cells`, `datagrid_header`,
`datagrid_on_select`, …), each returning the grid so they chain.

```aether
g = datagrid(100000, 50, 20, 6, 1) {      // rows, cols, shown rows, shown cols, frozen
    column_width(80)
    cells() callback |r: int, c: int| { return "R${r}C${c}" }
    headers() callback |c: int| { return letters(c) }
    on_cell_activate() callback |r: int, c: int| { open_row(r) }
    on_cell_edit() callback |r: int, c: int, t: string| { store(r, c, t) }
}
// A handler that needs `g` itself: a closure made inside the block is made
// before `g` is assigned (closures capture by value), so it takes the
// handle form after the block.
g.datagrid_on_select() callback |r: int, c: int| { show(datagrid_anchor_row(g), r) }
```

The grid's verbs (`cells`, `headers`, `column_width`, `column_limits`,
`on_cell_click`, `on_cell_select`, `on_cell_activate`, `on_cell_edit`,
`on_col_resize`, `on_col_move`) do nothing outside a grid's block.

Two event handlers have ambient forms for any widget's block, as `onclick`
does: `ondrag()` (press, drag, release) and `onsubmit()` (Return in a field).
A `textfield`'s trailing slot is its `on_change` closure, so the field that
takes a block is `textfield_bound`:

```aether
text("⋮") { ondrag() callback |phase: int, x: float, y: float| { resize(phase, x) } }
textfield_bound("Formula…", formula) { onsubmit() callback |t: string| { run(t) } }
```

sae pages write the same shapes in TypeScript, the block as a last-argument
arrow function: `ui.datagrid(1000, 40, 15, 5, 1, () => { cells((r, c) => …);
on_cell_edit((r, c, t) => …) })`, `text("⋮", () => { ondrag(…) })`,
`textfield("Formula…", onChange, () => { onsubmit(…) })`.

## Chaining explicit-handle modifiers (UFCS)

The `_ctx` axis above styles the *ambient* widget inside a block
(`button("OK") { corner_radius(8); tooltip("…") }`). The complementary case
is styling a widget you captured *by name* — and that's where UFCS reads best.
Every explicit-handle modifier (`style_*`, `width`, `height`, `fill_*`,
`alignment`, `distribution`, `edge_insets`, `margin_of`, the `set_*` state
mutators, the `on_*` handlers, and the `tray_set_*` setters) returns its
handle, so `x.f(a)` (which desugars to `f(x, a)`) chains:

```aether
title = text("Aether UI Styled Demo")
title.style_font_size(20).style_font_bold().style_text_color(1.0, 1.0, 1.0)

submit.style_bg_color(0.2, 0.6, 0.3, 1.0)
      .style_corner_radius(8)
      .style_tooltip("Click to submit the form")

// A grid configured by handle, chained (a chain carries on past a trailing
// callback block):
g.datagrid_set_col_widths(80).datagrid_col_limits(40, 400)
 .datagrid_cells() callback |r: int, c: int| { return cell(r, c) }
 .datagrid_header() callback |c: int| { return title(c) }

// A flat button: no frame and no face of its own until the pointer is over
// it -- toolbar icons, a tree's disclosure chevron, an inline action.
more = btn("…") callback { open_menu() }
more.style_flat()                 // or, in a block: button("…") { flat() }
```

(A method call on a name you ALSO import selectively —
`import ui (datagrid_cells)` then `g.datagrid_cells()` — needs an aether with
the fix in its `tests/integration/ufcs_selective_import`; before it, leave such
names out of the selective import list and the call resolves through
`import ui`.)

Both forms still work — UFCS is a *last-resort* fallback (it only fires when a
dotted call would otherwise be undefined), so the flat
`style_font_size(title, 20)` is unchanged. The chain is purely sugar: each
`x.f(a)` lowers to the same `f(x, a)` call, and the handle threaded through is
a register-width int the C backend elides.

See also: AeVG's `vg { … }` vector-graphics scope (a surface-nestable drawing
block) in the AeVG README.
