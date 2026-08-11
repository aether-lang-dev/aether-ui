# Notes to self (LLM assisting on Aether UI)

Not a CLAUDE.md — short, opinionated, written for a future LLM picking up mid-task. Re-read at start of every session.

## Context
This file covers UI/UX development for the Aether UI component. For language-level questions, the compiler, standard library, or general Aether idioms, consult the primary [Aether LLM.md](https://github.com/aether-lang-org/aether/blob/main/LLM.md).

## What Aether UI is
A declarative widget DSL for Aether, porting the Perry UI framework. It supports Linux (GTK4), macOS (AppKit), and Windows (native Win32), sharing a common backend ABI in `aether_ui_backend.h`.

## Development Principles
- **Config IS Code**: Aether UI follows the trailing-block builder pattern. Don't add config loaders; use the DSL.
- **Cross-Platform ABI**: Any new widget functionality must be implemented in all three backends (GTK4, AppKit, Win32). Use the `aether_ui_backend.h` ABI as the single source of truth.
- **Headless Testing**: All UI operations that would normally show a modal (alerts, file pickers) MUST honor the `AETHER_UI_HEADLESS` environment variable by returning early on CI to prevent hanging the test runner.
- **AetherUIDriver**: The built-in HTTP test server is the primary way to test UI interactions. It must be maintained to expose widget state and mutations consistently across platforms.

## Branching: there isn't any
Nic and Paul commit and push **direct to `main`**. No feature branches, no PRs.
If you are an assistant here: put the commit on `main` and push it — do NOT
helpfully create a topic branch "to be safe", that just strands work somewhere
nobody looks. What replaces branch review is `./ci.sh` green BEFORE the push.
(Same rule in the aeb repo.)

## Building / Testing
- **Compiler**: Uses system-wide `aetherc` (v0.140.0+).
- **Environment**: Headers are in `/usr/local/include/aether/`, libraries in `/usr/local/lib/aether/`.
- **Build**: `./build.sh <source.ae> [output]`
- **CI**: `./ci.sh` runs the full lifecycle: build, smoke test, and HTTP driver integration tests.
- **Bootstrap**: If Aether is missing from `/usr/local/`, use `./bootstrap_aether.sh` to fetch and build it locally.

## Files/dirs worth knowing
- `ui/module.ae`: The Aether-facing surface (imports, DSL wrappers).
- `backend/aether_ui_backend.h`: The backend ABI.
- `backend/aether_ui_<platform>.c/m`: The native platform implementations.
- `backend/aether_ui_test_server.c/h`: The shared HTTP test server — used by
  **win32 AND macOS**. GTK4 has its own embedded one inside `aether_ui_gtk4.c`.
  A route added to only one of the two is a spec that goes red on the other
  platforms for no reason (this is the "both-servers rule").
- `ci.sh`: The canonical source of truth for the CI test pipeline.
- `tests/spec_matrix.sh`: Runs every Aeocha suite and tabulates. The platform
  parity baseline in one command — `./tests/spec_matrix.sh [suite...]`.
  Needs `aeb .all.ae` first. macOS is 98/98 green as of 2026-07-17.
- `examples/`: The source of all example apps.
- **Building**: the real build path is `aeb` (`aeb .all.ae`, per-app
  `.build.ae` nodes, backend link block in `build_support/aetherui/module.ae`).
  `./build.sh <src.ae>` is the quick single-app shell twin — if you change how
  a backend links, you must change BOTH.

## Idioms that keep biting
- **Widget Registry**: Native widgets (GtkWidget*, NSView*) are registered in a flat global array and referenced via 1-based integers (`handle`). Never leak these handles.
- **Sealing**: Use `ui.seal_widget()` and `ui.seal_subtree()` to protect widgets from automation in test environments.
- **Imports**: All examples must use `import ui`.
- **CSS**: In the GTK4 backend, avoid global CSS. Apply styling using `aether_ui_apply_css` which scopes rules to a specific widget via a handle-derived class name (`.aui-{handle}`).
- **CSS is a back-channel, not just styling.** `ui.transition()` and
  `ui.style_opacity()` both travel to the backend as CSS declarations
  (`transition: opacity 1200ms ease-out;` / `opacity: 0.15;`), because GTK's
  stylesheet *is* its animation engine. A non-GTK backend that treats
  `apply_css` as a pure no-op silently drops every declared transition. macOS
  parses those two declarations; win32 still drops them.
- **The DSL assembles TOP-DOWN.** A container is added to its parent *before*
  its own children arrive. Any backend logic of the form "decide something
  about this container based on what's inside it" must therefore be
  retroactive — at attach time the container is still empty. (This is what
  made canvas expansion, and Grand Perspective's treemap height, so subtle.)

## Platform traps that cost real time
- **macOS `char` returns.** libaether declares `string_char_at` as `char` but
  codegen prototypes it `int`. Only `AL` is defined for a char return on
  x86-64 — gcc zero-extends, Apple clang sign-extends. Any binary byte ≥128
  read through it comes back negative on macOS. Mask it (`if b < 0 { b += 256 }`).
- **Apple clang errors on `-Wint-conversion`** where gcc warns. Reusing one
  variable for two types compiles on Linux and fails the build on macOS.
- **macOS's allocator is a heap-corruption detector.** glibc rounds small
  mallocs up into bigger bins and quietly absorbs small overflows; macOS
  aborts. An abort on the Mac that "can't happen" is usually a real overflow
  Linux was hiding — reach for `-fsanitize=address` first, it names it instantly.
- **`gcc` on macOS is a clang shim, and `/bin/sh` under `make` does NOT see
  your PATH's GNU coreutils.** A `date +%s%N` that works in your shell can
  still emit a literal `N` inside a Makefile recipe.
