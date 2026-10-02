# Tsyne browser mode, migrated to Aether

Status: **proposal** (2026-10-02). Nothing here is built yet. No numbers are
measured; the first spike (below) exists to produce them.

A "fat UI browser": a native app that fetches **pages** over HTTP and renders
them as real aether-ui widgets, where a page is a small program (layout plus
the logic behind it), not markup. This is Tsyne's browser mode
(`../tsyne/tsyne/docs/BROWSER_MODE.md`), rebuilt on the Aether stack. Tsyne is
Paul's copyright, so its design, docs and page format are free to mine; nothing
here needs to be Tsyne-*compatible*.

## What Tsyne does today

`tsyne-browser.sh` → `npx tsx` → Node → `createBrowser()`. Per page:

1. HTTP GET returns TypeScript source.
2. `core/src/transpile-cache.ts` transpiles it with esbuild, cached under
   `~/.cache/tsyne/compiled` keyed by `sha256(source) + core version`.
3. `new Function('browserContext', 'tsyne', code)` runs it
   (`core/src/browser.ts:1241`).
4. Every widget call crosses IPC to the Go bridge, which drives Fyne.

The per-page transpile is cheap (esbuild is fast, and cached). The weight
is elsewhere, though this is not measured: the cold start (npx, tsx, Node,
esbuild, the bridge process) and one IPC round trip per widget and per event.
Tsyne pages also `await` widget getters (`await nameEntry.getText()`). That is
only because those getters cross a process boundary.

## What the Aether version removes

One native binary links `ui` and the mquickjs engine:

- **No runtime to start.** No Node, no tsx, no bridge process.
- **No IPC.** A page's `label("x")` is an in-process call into the
  `ui/module.ae` builder.
- **No async in pages.** Getters return synchronously, so the page dialect can
  leave out `async`/`await`, the one feature that would need VM work.

## Three parts, three boundaries

All three live **outside** aether-ui except the host, which is an ordinary app
that imports `ui`.

```
  page.ts ──► [ lowerer ] ──ES5 subset──► [ mquickjs-port ] ──calls──► [ browser host ]
              own repo                     unchanged port              apps/<name>, import ui
```

### 1. mquickjs-port stays a port

`../mquickjs-port` is a faithful Aether port of Bellard's MicroQuickJS. Its
value is that faithfulness: the `.tests.ae` conformance gate, with output
byte-identical to the C build. It accepts ES5 plus `for of`. As of today its
parser has no arrow functions, `let`/`const`, template literals, `class` or
`async`.

We do **not** extend its parser. Changing it in place would make it a
divergent fork that is no longer a port, and the conformance gate would stop
meaning what it says.

### 2. A lowerer: page dialect → ES5 subset (its own repo)

A source-to-source tool. It reads the page dialect and writes ES5-subset text
that the unchanged port runs.

**Why not port an existing transpiler.** There are two jobs: strip the
TypeScript, then lower ES2015+ to ES5. The candidates mostly do only the
first:

| Transpiler | Strips TS | Lowers to ES5 | Port size |
|---|---|---|---|
| tsc | yes | yes, async via helpers | huge; the type checker comes with it |
| esbuild (Go, MIT) | yes | no, as far as we know: the es5 target refuses `let`/`const`, destructuring etc. | medium, but doesn't solve the second job |
| sucrase | yes | no, outputs modern JS | small, same problem |
| Babel / swc | yes | yes | huge plugin systems |

**The dialect.**

- *TypeScript part:* exactly what TS 5.8's `--erasableSyntaxOnly` allows.
  Types, interfaces, `as`, `!` and generics are erased. `enum`, `namespace`
  and constructor parameter properties are rejected. Authors check pages with
  real `tsc --noEmit --erasableSyntaxOnly`, so the type-checking rule is a
  published one, not ours.
- *JS part:* ES5 plus a written list of ES2015 features, each with tests.
  Start with arrow functions, `let`/`const` and template literals, which cover
  most of how Tsyne pages read. Add `class`, destructuring and spread if real
  pages need them.
- *Left out:* `async`/`await` and generators (not needed in-process).

**Keeping error positions right.** Types are erased by replacing them with
spaces (the ts-blank-space approach), so columns don't move. Rewrites keep
line breaks where they were. An engine error's line and column then point at
the `.ts` the author wrote.

**Reusable beyond the browser.** It works with any ES5 engine, and it can run
on a server to precompile pages ahead of time.

The cost is two parses per page. That is small for page-sized sources, and the
bytecode cache below removes it on repeat visits.

### 3. The browser host (an aether-ui app)

**Each page gets its own `JSContext` with a memory limit.** mquickjs runs
inside one fixed memory block, so a runaway page hits its limit instead of
taking the browser with it.

**The binding is the security model.** A page can reach only what the host
registers: the `ui` builders, state, and `browserContext`. There is no file
system or process access unless we add it on purpose. Tsyne's `new Function`
inside Node gives pages all of Node. mquickjs's stdlib table is generated at
build time, so the binding goes in as a custom class, the way
`example_stdlib.c` adds one.

**Callbacks.** JS functions kept for later (click handlers, state observers)
must be held as GC roots (the port's `gcref` / `add_gc_ref`) and released when
the page is torn down. One context is single-threaded, so `ui.background`
results are posted back to the UI thread before they reach JS.

**Navigation.** `browserContext.changePage(url)`, `back()`, `forward()`,
`reload()`, `currentUrl`, taken from Tsyne's API. Pages live on the existing
navstack push/pop. Leaving a page tears down its widget subtree and frees its
context. The navstack pop/unregister bug in `TODO.md` matters here: any leak
there repeats on every navigation. This also closes the roadmap's open item
"swiby's `$context.goto` + session shape is untouched".

**Bytecode cache.** Tsyne's content-hash idea, applied to mquickjs bytecode
(`-o` / `-b`). The key is `sha256(page source) + lowerer version + engine
version + word size + byte order`, because bytecode depends on the last two.

## Pages stay declarative

The Aether DSL looks declarative but is imperative underneath. `vstack`
(`ui/module.ae:993`) creates a handle, attaches it to the ambient `_ctx` and
returns it. The compiler then runs the trailing block with `_ctx` set to that
handle. Ambient modifiers (`margin`, `bg_color`) act on whatever `_ctx` is.

The host does the same thing at run time with a stack of `_ctx` handles:

- `vstack(4, fn)` calls `ui.vstack(top, 4)`, pushes the handle, calls `fn`,
  pops the handle and returns it.
- `margin(...)` / `bg_color(...)` apply to the top of the stack.

So we bind at the `ui/module.ae` builder level, **not** the raw
`aether_ui_*_create` + `add_child` ABI, and pages keep the shape of
`examples/calculator/calculator.ae`:

```js
const { window, vstack, hstack, btn, button, bg_color, onclick,
        divider, text_bound, ui_state, ui_set, margin } = ui;

let num = 0, prev = 0, op = (a, b) => a;
const display = ui_state(0);
const digit = d => { num = num * 10 + d; ui_set(display, num); };
const apply_op = f => { prev = num; num = 0; op = f; ui_set(display, 0); };

window("Calculator", 280, 260, () => {
  vstack(4, () => {
    margin(12, 12, 12, 12);
    text_bound(display, " ", "");
    divider();
    hstack(4, () => {
      btn("7", () => digit(7));
      btn("8", () => digit(8));
      button("+", () => {
        bg_color(0.95, 0.85, 0.5, 1.0);
        onclick(() => apply_op((a, b) => a + b));
      });
    });
  });
});
```

How Aether forms map:

- Aether's trailing block becomes the last function argument.
- The `callback` postfix keyword becomes a function argument.
- `btn` (whose function is a click handler) and `button` (whose function is a
  build block) stay separate names, for the same reason they are separate in
  Aether.

The rules the binding must enforce, which Aether's compiler enforces for free:

- **A modifier with an empty stack throws.** `margin()` called outside any
  block is a compile error in Aether. Here it must be a clear JS error, not a
  modifier silently applied to nothing.
- **A block that throws still pops.** Otherwise every later widget attaches
  to the wrong parent.
- **Top-down assembly holds unchanged** (see AGENTS.md). JS runs a block after
  its container is attached, the same order the compiler produces.

Builders still return handles, so a page that needs imperative control has it,
as Aether code does today.

## Testing comes free

Pages build real `ui` widgets, so the AetherUIDriver, the per-suite specs and
`tests/spec_matrix.sh` apply unchanged on GTK4, macOS and win32. That is the
TsyneTest equivalent without writing one. Driver specs for the host
(navigate, back, forward, a page that throws, a page over its memory limit)
come on top.

The lowerer and the port test separately, below the UI: dialect-in → ES5-out
goldens for the lowerer, and the existing conformance gate for the port.

## To mine from Tsyne

From `../tsyne/tsyne/docs/`:

- `BROWSER_MODE.md`: the `browserContext` API; how HTTP 200, 302 and 404 are
  handled; pages served by any backend language (filesystem-mapped
  `/about → pages/about.ts`).
- `BROWSER_TESTING.md`: how browser pages were tested.
- `Browser_TODO.md`: what was still open, so the same gaps aren't rediscovered.
- `WEB_FEATURE_MAPPING.md`: web concepts and their page-side equivalents.
- `SECURITY_HARDENING.md`: what a sandboxed page should and should not reach.

Credit lines follow the house rule: "-alike", and name the source.

## First spike: measure before building

The point of all this is speed, so measure first.

1. Link mquickjs-port into one aether-ui app. Bind about five builders
   (`window`, `vstack`, `hstack`, `text`, `btn`) through the context stack.
2. Time a page from bytes in memory to widgets on screen, in two forms:
   ES5 source, and precompiled bytecode.
3. Compare the same page in `tsyne-browser.sh`, from cold start and warm.

If the Aether port of the engine is much slower than Bellard's C, this is
where it shows. If the numbers are good, the rest is wiring: the lowerer, HTTP
fetch through std's client, browser chrome, and the cache.

## Open questions

- **Where the host lives.** `apps/<name>` in aether-ui, with mquickjs-port as
  an aeb dependency, or its own repo. The lowerer and the port are outside
  either way.
- **Lowerer language.** Write it in Aether (one toolchain, runs in the
  browser in-process), or another language (servers only)? Aether is the
  assumption so far, because the browser needs it in-process for pages served
  as raw `.ts`.
- **Name.** The browser, the dialect and the lowerer each need one.
- **Server-driven pages as a second mode.** `../aether/docs/liveview-lite-roadmap.md`
  describes server-held state with diffs pushed to a thin client. That
  complements client-side page logic; it does not replace it. Out of scope for
  v1.
