# Declarative consistency audit

*2026-08-15. Prompted by `command` shipping imperative, and the observation
that "pseudo-declarative excellence" should hold for ALL of Aether UI, not
just the widget tree.*

The rule is [dsl-with-scope.md](../guide/dsl-with-scope.md): nested blocks that
read declaratively but are executed code, with an implicit receiver so children
attach to their enclosing scope. `text("hi")` inside `vstack { … }` needs no
parent passed. Every *structure-building* API should read that way.

## What the surface actually looks like

332 exports in `ui/module.ae`, classified by shape:

| shape | count | verdict |
|---|---|---|
| takes `_ctx` — a scope verb | 55 | correct |
| takes a handle and **returns it** | 48 | correct — the documented UFCS value-chain axis (`title.style_font_size(20).style_bold()`) |
| takes a handle, does not chain | 177 | mostly fine; **six families are not** |

The 177 is not a problem list. Most are queries (`listbox_count`,
`canvas_cmd_count`), one-shot setters on an already-built widget
(`window_close`), or the canvas immediate-mode drawing verbs, which are
imperative *by nature* — a path is a sequence of moves, not a tree.

## The six families that build structure by threading a handle

These are the ones where a user assembles a STRUCTURE and must carry a handle
to every line to do it. This is the shape `command` had before 2026-08-15.

| family | today | wants |
|---|---|---|
| ~~**styles**~~ **DONE** | `s = create_styles(); st_bg(s,"container",0x…); st_color(s,"text",0x…)` | `styles() { bg("container", 0x…); color("text", 0x…) }` |
| ~~**roles**~~ **DONE** | `sc = color_scheme(); role(sc,"primary",0x…)` | `scheme() { primary(0x…); on_primary(0x…) }` |
| ~~**table columns**~~ **DONE** | `cols = table_cols(); table_col(cols,"Name",220)` | `columns() { col("Name", 220) }` |
| ~~**states**~~ **DONE** | `ws = ui_states(card); add_state(ws,"open",sheet)` | `states(card) { on_state("open") { bg(…); color(…) } }` |
| ~~**menus**~~ **DONE** | `m = menu("File"); menu_item(m,"Save") callback {…}` | `menu("File") { item("Save") callback {…}; separator() }` |
| ~~**navstack**~~ **DONE** | `nav_push(nav, "Detail", body)` | `nav_page(nav, "Detail") { … }` |

The pattern is identical in each: a factory returns a handle, and every
subsequent call repeats it as argument 1.

## Why this is worth fixing, and why it is not urgent

**Worth fixing:** it is the difference between reading a theme as a
description and reading it as a script. Compare, from
`examples/themes_demo`:

```aether
s = create_styles()                      // today
st_font_family(s, "root", "monospace")
st_color(s, "text", 0x5C458A)
st_color(s, "button", 0x5C458A)
st_bg(s, "container", 0xD6CFE6)
```

Five lines, `s` on four of them, and nothing structurally marks where the
sheet begins and ends.

**Not urgent:** every one of these works, is specced, and is green. This is
ergonomics, not correctness — so it should land family by family, each with
its demo and spec, rather than as one sweeping change to a 332-function
surface.

## What `command` established (do this again)

The declarative `command` landed 2026-08-15 and is the template:

1. **A scope function returning a config ptr.** Aether pushes any return value
   as `_ctx` for the trailing block, so a plain function is enough — no
   `builder … with` needed unless the body must run *after* the block.
2. **Verbs inside read the ambient `_ctx`.** They take `_ctx: ptr` as
   parameter one and never require the handle.
3. **Keep the imperative form.** Both should work; the scope form is sugar
   over the same calls. `command` kept `btn_command(save)` alongside
   `on_button()`.

### FIVE traps, all paid for once already

* **`f(args) callback {…} {…}` is not a call shape** — one trailing block per
  call. Passing a callback *and* a block compiles and produces a broken call.
  Put the body inside the block as a verb (`does() callback { … }`).
* **Two contexts, one slot.** If a block's `_ctx` is the config object, a verb
  inside it cannot also reach the enclosing *widget* scope: Aether exposes
  `builder_context()` (top of stack) only. Capture the outer scope when the
  scope function runs (`command` stashes it in C beside the AeCS
  current-sheet cell, since Aether has no top-level mutable module state).
* **Plain function, not `builder`,** when the scope must be recorded BEFORE
  the block runs — a `builder` runs its block first and its body after.
* **~~A short verb name is only free if nothing uses it as a local~~ — that
  was an AETHER BUG, fixed in 0.544.0 (#1606).** Recorded because the wrong
  lesson is worse than none.

  The menu scope added a global `item()`, and `item` is the conventional
  parameter name for a row here (16 call sites:
  `table(cols) callback |item: ptr, c: int|`, plus listbox, each, tree).
  `table_demo` then segfaulted at startup. I called it "a global shadows the
  parameter" and renamed to `menu_entry` — which fixed the symptom while
  getting the cause wrong.

  What was actually happening: aetherc's `rename_intra_module_refs` rewrote
  `item` inside the closure to the namespaced `<ns>_item`, because
  `collect_local_names` did not recognise `AST_CLOSURE_PARAM` as a local
  binding and closures established no scope of their own. So the closure passed
  **the address of the function** where the parameter's value belonged. It
  compiled and type-checked cleanly, and the callee read a function pointer as
  data. Closures now establish a scope that extends the enclosing one.

  So a same-named module function and closure parameter is FINE from 0.544.0
  on. The renames stay (`menu_entry` reads better than `item` anyway, and
  `weight`/`font_size` were genuine duplicate definitions — a different and
  still-real problem), but nobody needs to grep for `|name:` before adding a
  verb.

  The transferable lesson is about method, not naming: **the generated C being
  byte-identical apart from one identifier is what proved this was not mine.**
  Reaching for that diff early would have saved a long bisect.

* **THE PARENTHESES ARE LOAD-BEARING.** `scope { … }` without them compiles to
  a function-POINTER read and pushes no context, so everything inside silently
  sees the enclosing scope. That segfaulted for a long bisect. Filed as an ae
  bug with a 30-line reproducer
  (`~/scm/aether/asks/zero-arg-fn-with-trailing-block-reads-fn-pointer.md`);
  until it is diagnosed, write `scope() { … }`.

## Sequencing

**Done 2026-08-15:** `styles`, `scheme`, `menus`. The first two needed no C at
all. `menus` needed one small extern — a builder `_ctx` is an opaque `void*`
but a menu handle is an `int`, and Aether will not cast between them, so
`aether_ui_ctx_to_handle_impl` does in C what the widget path already did
inline (`aether_ui_widget_add_child_ctx` casts `(int)(intptr_t)ctx`). Any
future scope whose ambient context is a HANDLE rather than a struct needs the
same helper.

**ALL SIX DONE.** `styles`, `scheme`, `menus`, `columns` (2026-08-15);
`states`, `navstack` (2026-08-17).

Two things the last pair taught:

* **`state` is a RESERVED KEYWORD** and cannot be an identifier at all, so the
  verb is `on_state` — which also matches the `on_button`/`on_menu_item`
  convention the command scope set. Worth noting the compiler says this
  plainly, unlike the silent shadowing failures elsewhere in this audit; a
  reserved word is the *good* kind of collision.
* **`navstack` is the one family that genuinely wants `builder`.** Every other
  scope needed a plain function because its config object had to exist BEFORE
  the block ran. Here the opposite holds: a page body must be fully built
  before it can be pushed, which is exactly what a builder's after-block
  epilogue provides. `nav_push takes a handle, not a builder block` was a
  comment in navstackdemo at the call site — that comment is now gone.

Neither needed the `aether_ui_ctx_to_handle_impl` extern after all: `ui_states`
already returned a ptr, and `nav_page`'s builder threads the nav handle as a
normal argument.

**Run the FULL matrix before pushing a scope, not just its own suite.** The
menu scope passed `menu` 5/0 and broke `table_demo` at startup; only the full
run would have shown it.
