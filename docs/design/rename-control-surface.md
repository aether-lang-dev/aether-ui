# TODO: rename the "test_server" control surface to a neutral "control" name

**Status:** OPEN. Raised 2026-09-02 after the driver-out-by-default work
(commit dd3c0cc1), which introduced the *concept* of a "control" surface with
two implementations (`aether_ui_no_control.c` + `aether_ui_test_server.c`) but
left the active file and its symbols under the old `test_server` name.

**Why:** "test_server" reads as test-only scaffolding, but this is the app
*control / automation* surface (the AetherUIDriver seam). The names should be
neutral so the pair reads as two implementations of one thing:
`aether_ui_control.c` (the real control server) and `aether_ui_no_control.c`
(the no-op default). `no_control.c` is already named right; the active side is
not.

There are **two tiers**. Tier 1 is the rename the pair needs and is
backend-scoped. Tier 2 is a separate, much larger decision — do it only if we
want to rename the *app-facing DSL name* too.

---

## Tier 1 — the internal control-server file + symbols (do this one)

Backend-only, mechanical, ~7 files. No `.ae` app churn.

**Files to rename (`git mv`):**
- `backend/aether_ui_test_server.c` → `backend/aether_ui_control.c`
- `backend/aether_ui_test_server.h` → `backend/aether_ui_control.h`

**Symbols to rename** (the internal server API — NOT the app-facing
`enable_test_server`, see Tier 2):
- `aether_ui_test_server_start`        → `aether_ui_control_start`
- `aether_ui_test_server_set_banner`   → `aether_ui_control_set_banner`
- `aether_ui_test_server_banner_handle`→ `aether_ui_control_banner_handle`
- `aether_ui_test_server_seal_widget`  → `aether_ui_control_seal_widget`
- `aether_ui_test_server_is_sealed`    → `aether_ui_control_is_sealed`

**Reference sites to update** (the symbols are called from all four backends;
the `.c`/`.h` filenames appear in the build + gates):
- Backends: `aether_ui_macos.m`, `aether_ui_gtk4.c`, `aether_ui_win32.c`,
  `aether_ui_uikit.m`, and the two impls `aether_ui_no_control.c` +
  `aether_ui_control.c` (self) + `aether_ui_backend.h` if it re-declares any.
- `#include "aether_ui_test_server.h"` → the new header, everywhere it appears
  (the backends + `aether_ui_no_control.c`).
- Build/source selection: `build_support/aetherui/module.ae` — `control_source()`
  already returns the filenames; update the real one to `aether_ui_control.c`.
- Gates / scripts that name the file directly:
  `ci.sh` (Phase 1d win32 cross-compile list, Phase 1e iOS compile+link+render
  list, and the render-probe link line), `build.sh`, `tests/win32/*` (the
  win32 runtime/link harness compiles the server), and `tests/ios/link_stub.c`
  context if referenced.

**Verify after:**
- `AETHER_UI_WITH_DRIVER=1 ./ci.sh` → all phases pass (Phase 7 LisMusic drives
  via the real control server).
- Default build clean: `strings <bin> | grep 'AetherUIDriver: listening'` → 0.
- iOS gate green: `ci.sh` Phase 1e (compile+link+render).
- `grep -rE 'aether_ui_test_server_' --include='*.c' --include='*.m' \
  --include='*.h'` (outside target/) → empty.

Keep the runtime env var `AETHER_UI_WITH_DRIVER` and the driver port env
`AETHER_UI_TEST_PORT` as-is unless Tier 2 is also done (see below) — they are
independent of the file/symbol names and renaming them is extra churn.

---

## Tier 2 — the app-facing `enable_test_server` DSL name (SEPARATE decision)

Much bigger. The app-facing ABI is `aether_ui_enable_test_server_impl` /
`aether_ui_enable_test_server_ctx`, surfaced in the DSL as
`enable_test_server(port)` / `enable_test_server_root(...)`. That DSL name is
called from **~120 app + example `.ae` files** (every driver-tested app writes
`enable_test_server(9222)` in its `window { … }` block) plus `ui/module.ae` and
all four backends.

If we want full neutrality, rename to e.g. `enable_control(port)` /
`aether_ui_enable_control_impl` — but that is a public-ish DSL rename touching
~120 files and every backend, and needs a deprecation shim
(`enable_test_server` → `enable_control`) so out-of-tree apps don't break. Treat
as its own change; do NOT bundle it with Tier 1.

An alternative that avoids the churn: leave the *verb* `enable_test_server`
(it reads fine — "arm the test/automation server") and only do Tier 1. That is
the recommended default.

---

## Coordination note

The four backends are shared code the aether-lang sibling co-maintains. Tier 1
touches all four (symbol renames) — mechanical, but land it as one commit with
the full-repo `grep` verification above so no backend is left half-renamed
(the class of bug the Win32 backend has hit before: a shared symbol renamed in
three backends but not the fourth compiles until that one platform links).
