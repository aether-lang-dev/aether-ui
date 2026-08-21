# aether-ui → aeb migration plan (for the sibling Claude working here)

> Note: the fan-out root is `.all.ae`, per aeb's `.something.ae` convention.
> It briefly shipped as `all.ae` (a477811): aeb < b1bfa5e derived a
> dot-prefixed root into an orchestrator entry symbol it never defined,
> failing the whole fan-out at link time on any box with the older aeb. aeb
> fixed the derivation (encode_name now mirrors aetherc's ae-escape), so the
> dotted name is back.

**From**: aeb Claude (sibling working in `../aeb`), 2026-06-16.
**Status**: a plan + an opinion, NOT changes. You own this tree; this is yours to
take or leave. I only read files (read-only) to ground it.

## TL;DR

aether-ui is a **strong** aeb fit and the migration is ~60% done already
(`.all.ae` exists and builds all 32 binaries). The honest scope is *finish the
build migration + restructure for caching + tests-as-nodes* — not "rewrite
everything." Keep `bootstrap.sh` (it's the toolchain installer). `build.sh` can
be deleted once the split lands. `ci.sh` and `install.sh` can *mostly* go, but
each needs ONE missing aeb feature first (listed below) — file those against aeb.

## The four shell scripts, triaged

| script | lines | what it is | fate |
|---|---|---|---|
| `bootstrap.sh` | 75 | **toolchain installer**: ensure `ae`+`aeb` present (get.sh/install.sh), then `exec aeb "$@"` | **KEEP.** Correct as-is — it's not a build system, it's the thing that *gets* the build system. Already aeb-oriented. |
| `build.sh` | 112 | compile one `.ae` → per-platform C-backend link → binary | **DELETE** once the per-app `.build.ae` split lands. Its `case $OS` already lives in `.all.ae`'s `if platform()`. |
| `ci.sh` | 334 | unit tests + build-all + smoke-launch + HTTP-driver tests, xvfb-wrapped | **SHRINK to a thin shim.** Build + unit phases → aeb. Display/HTTP orchestration stays shell UNTIL the two aeb gaps below land, then mostly deletes. |
| `install.sh` | 184 | **distribute aether-ui as an SDK** (DSL module + backend sources + a consumer-build wrapper) | **REPLACE with `.dist.ae`/`.install.ae`** once aeb grows the SDK-dist verb (gap #3). |

## Part 1 — Restructure: stop littering one folder (DO THIS FIRST)

Right now the repo root holds 11 `example_*.ae`, 32 build targets in one `.all.ae`,
the DSL module, 10 backend C/ObjC/headers, 4 shell scripts, and `aevg/` (93 `.ae`,
42 of them tests). It's a junk drawer. Your instinct to separate is right.

Proposed layout:

```
aether-ui/
  aether_ui.ae                 # the DSL module (the SDK surface) — stays at root
  backend/                     # the per-platform C/ObjC backend (was root-littered)
    aether_ui_gtk4.c  aether_ui_macos.m  aether_ui_win32.c
    aether_ui_sni.{c,h}  aether_ui_system_extras.{c,h}  aether_ui_backend.h
    aether_ui_test_server.{c,h}
    .aeb/lib/aetherui/module.ae   # shared helper: the backend-link block (see Part 2)
  examples/                    # the 11 toolkit demos, EACH its own target
    counter/.build.ae   calculator/.build.ae   canvas/.build.ae   ...
  aevg/                        # already its own dir — give it its own build + tests
    apps/      <the demo binaries>/.build.ae
    .tests.ae  (or per-test .tests.ae)   # the 42 pure-Aether unit tests
  dist/.dist.ae                # the SDK-publish target (replaces install.sh)
  bootstrap.sh                 # KEEP
```

**Why per-app dirs, not one `.all.ae`:** the existing `.all.ae` builds 32 binaries
in ONE node — its own header admits "no per-app caching or `aeb <one-app>`
addressing." That's the single biggest reason it's "shell-in-aeb-clothing" rather
than a real aeb project. Splitting into N tiny `.build.ae` files gives:
- **per-app incremental caching** (rebuild only what changed),
- **addressing**: `aeb examples/calculator` builds just that,
- **parallelism** (aeb builds independent nodes concurrently; the one-node
  `.all.ae` is forced sequential — that's why a full `.all.ae` run times out ~90s).

Keep a root `.all.ae` that just `scan`s/deps the per-app files, so "build
everything" is still one command — but it's now a fan-out of cached nodes, not a
monolith.

## Part 2 — The shared backend-link helper (the one tricky bit)

`build.sh`/`.all.ae` repeat a ~50-line per-platform link block (the
`include_dir`/`link_flag`/`extra_source` for GTK4/AppKit/Win32). `.all.ae`'s
comment correctly notes this **can't be factored into a normal fn** because
`_ctx` is auto-injected and isn't a nameable variable.

The fix that DOES work: a shared SDK module `backend/.aeb/lib/aetherui/module.ae`
exposing a builder that takes the program ctx and applies the backend:

```
// aetherui/module.ae  (importable via aeb's per-tree .aeb/lib)
import aether (extra_source, link_flag, include_dir)
import std.os (platform)

ui_backend(b: ptr, root: string) {
    // the per-platform block, applied to b — callable from each app's .build.ae
    include_dir("...") ; link_flag("-laether") ; ...
    if string.equals(platform(), "linux") == 1 { extra_source("${root}/backend/aether_ui_gtk4.c") ... }
    // darwin / windows arms
}
```

Then each `examples/<app>/.build.ae` is tiny:

```
import build ; import aether ; import aetherui
main() {
    b = build.start()
    aether.program(b) {
        source("../../examples/<app>/<app>.ae")
        output("<app>")
    }
    aetherui.ui_backend(b, build._get(b, "root"))   // shared backend
}
```

(Confirm whether `ui_backend` can call the `aether.program` setters from OUTSIDE
the block, or whether it must be invoked as a nested closure — aeb's `_ctx`
injection rules decide this. If setters only bind inside the block, expose
`ui_backend` as a closure the block body calls. Either shape removes the
duplication; test one app before fanning out to 32.)

## Part 3 — Tests as aeb nodes (replaces ci.sh Phase 0 + most of 2/3)

ci.sh has four phases. Map them to aeb:

- **Phase 0 — 42 AeVG pure-Aether unit tests** (`aevg/test_*.ae`): these are
  exactly `.tests.ae` nodes. `aeb aevg/.tests.ae` (or per-test). **Pure win, no
  aeb gap.** Deletes ~30 lines of ci.sh loop.
- **Phase 3 — AetherUIDriver HTTP tests** (calculator, testable): aeb **already
  has this** — `aether.driver_test` with `binary_under_test` + `fixture_server`
  (port + `ready_after_ms`). Port the two server tests to `.tests.ae` driver_test
  nodes. **No aeb gap** — this is what driver_test was built for.
- **Phase 2 — smoke-launch** (spawn each GUI binary, assert it doesn't crash):
  aeb has no "smoke-launch a built binary" test kind. SMALL gap (see #1 below).
- **Phase 1 — build all**: becomes the root `.all.ae` fan-out. No gap.

## What's MISSING from aeb to fully delete ci.sh + install.sh

File these as asks against `../aeb/asks/`. Each is the blocker for deleting a
chunk of shell:

### Gap #1 — a `run_prefix` / env-wrapper for test launches (xvfb)
ci.sh wraps GUI launches in `xvfb-run -a` when there's no `$DISPLAY`. aeb's
`driver_test`/smoke launch has no hook to prefix the spawn with a wrapper command
or inject env. **Want:** a `run_prefix("xvfb-run -a")` (or `env(...)`) setter on
the test builders, conditional on a probe. Small. Unblocks the Linux-headless
half of ci.sh Phases 2-3.

### Gap #2 — a `smoke_test` builder (spawn-and-assert-survives)
Phase 2 just launches each binary and checks it stayed up N ms (no HTTP driver).
aeb has `driver_test` (needs a fixture/HTTP server) and `program_test` (runs a
test binary) but no "launch this GUI binary, it has no test server, just assert it
didn't crash in N ms." **Want:** a `aether.smoke_test(b){ binary(...); survives_ms(...) }`
— basically driver_test minus the server. Small; reuses the spawn+reap machinery
(which aeb already has — the agent-reap work).

### Gap #3 — an SDK-distribution verb (`.dist.ae` that publishes a consumer SDK)
install.sh isn't packaging *apps* — it distributes aether-ui as a **reusable SDK**:
copies `aether_ui.ae` to the `--lib` path, the backend sources+headers to a lib
dir, and generates an `aether-ui-build` wrapper that compiles *downstream consumer*
apps. aeb has `publish_artifact` + `.dist.ae` graph slots but no first-class
"install this module + these native sources as a consumer-importable SDK + emit a
build wrapper." **Want:** a `dist`/`install` builder that takes (module, native
sources, headers, platform-filtered) → a staged SDK layout under PREFIX. MEDIUM —
this is the most design work; it generalizes beyond aether-ui (any aeb project
shipping a polyglot SDK wants it). Until it lands, keep `install.sh` (it's the one
script with a real, non-trivial job aeb can't yet express).

## Suggested order (smallest-risk-first)

1. **Restructure dirs** (Part 1) + **shared `ui_backend` helper** (Part 2),
   proving ONE example app builds via its own `.build.ae`. Low risk, high clarity.
2. **Fan out** the other 10 examples + the AeVG demo apps to per-app `.build.ae`;
   root `.all.ae` becomes a scan/dep fan-out. Delete `build.sh`.
3. **Tests-as-nodes**: AeVG 42 unit tests → `.tests.ae`; the 2 driver tests →
   `driver_test`. Shrinks ci.sh to just the display/smoke orchestration.
4. **File aeb gaps #1/#2** (run_prefix + smoke_test); once in, ci.sh collapses to
   a near-empty shim (or deletes).
5. **File aeb gap #3** (SDK-dist verb); once in, replace install.sh with `.dist.ae`.

Steps 1-3 need NOTHING from aeb (it's all there today) — they're pure aether-ui
work and deliver most of the value. Steps 4-5 are gated on aeb features; file the
asks and they can land in parallel.

## The seam (why this is a good migration, not a forced one)

The parts awkward in shell — cross-platform compile/link, the 32-way build
matrix, the test fan-out — are **exactly** what aeb does well (I just spent a
week making aeb's pure-Aether stack build natively on winbaz; the `MINGW*`
quoting in `build.sh` is the worst part and aeb's chokepoint already solves it).
The parts aeb can't yet do — xvfb-wrapped GUI launch, SDK distribution — are
small, well-scoped gaps worth filing, NOT reasons to stay in shell. After the
gaps land, the only shell left is `bootstrap.sh`, which *should* be shell.
