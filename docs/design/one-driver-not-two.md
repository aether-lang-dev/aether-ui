# AetherUIDriver: one implementation, and a seam to leave it out

**Status: design only, not built.** Written first because the last comparable
effort (the GDI+ renderer seam) was cheaper for having a note, and because the
measurements below changed my mind about the shape twice.

## The problem, measured

AetherUIDriver — the HTTP control surface the test harness drives apps
through — is implemented **twice**:

| | file | lines | platforms |
| --- | --- | --- | --- |
| standalone | `backend/aether_ui_test_server.c` | 1,326 | win32, macOS |
| woven in | `backend/aether_ui_gtk4.c` (≈6049–7211) | ~1,160 | Linux, FreeBSD |

`build_support/aetherui/module.ae` decides which: linux/freebsd compile
`aether_ui_gtk4.c` and **never** `aether_ui_test_server.c`; darwin/windows
compile the standalone one.

Two consequences, both live today.

### 1. The two have already drifted

Each exposes 46 routes. They are not the same 46:

```
in GTK4 only        /canvas/{id}/pixel
in test_server only /overlays
```

and until earlier today `/canvas/{id}/pixelgrid` was a third: it was added to
`aether_ui_test_server.c` for the GDI+ comparison, which silently gave it to
win32 and macOS only. Every measurement in that line could only have been
taken on Windows, and nobody would have noticed until someone tried it on
Linux — where the request fell through to the `/pixel` arm (whose
`strstr(path, "/pixel")` also matches `/pixelgrid`) and answered a plausible
`{"pixel":-1}` rather than a 404.

**That is the real cost of the duplication: divergence presents as a wrong
answer, not as a missing one.**

### 2. It ships in production binaries

There is no compile-time exclusion anywhere. The `#ifdef`s in
`aether_ui_test_server.c` are all `_WIN32` platform splits; there is no
`AETHER_UI_DRIVER` or equivalent. So:

- every app on all four platforms compiles and links the server;
- `nm` on a demo app that never calls `enable_test_server` still finds its
  symbols;
- the only gate is a runtime `if` **in each application's own source**:

  ```aether
  if os_getenv("AETHER_UI_TEST_PORT") != null { enable_test_server(9222) }
  ```

  An app that omits that line listens unconditionally. `enable_test_server_impl`
  itself has no guard — it sets the port, injects the banner, and spawns the
  listener thread.

## Why not just `#ifdef` it

Because we cannot set the symbol. Checked at all three layers:

- **aetherc** takes no `-D`.
- **aeb** per-target setters are `extra_source`, `extra_source_glob`,
  `link_flag`, `include_dir`. No `define()`/`cflag()`, so `-DFOO` cannot
  reach the compiler line.
- **Aether's `select()`** does emit a real `#ifdef` chain — verified, not from
  docs — but its keys are matched by `strcmp` against four literals
  (`codegen_expr.c:3964-3975`) into a fixed `_WIN32`/`__APPLE__`/`#else`
  shape, and it wraps an *expression*, so it could not guard declarations even
  with user-defined keys.

Filed upstream as aether-lang-dev/aether#1527. **This design does not wait on
it.**

## Shape: one file, and exclusion by source omission

The mechanism we already have is aeb's `extra_source`. A file that is not
listed is not compiled — that is a perfectly good "ifdef-equivalent", and it
is stronger than a preprocessor symbol in one respect: a production app that
calls `enable_test_server` gets a **link error**, not a silent listener.

```
backend/aether_ui_driver.c      ← the ONE implementation: HTTP, routing,
                                  request parsing, JSON assembly
backend/aether_ui_driver.h      ← the backend-facing interface it calls
```

and in `build_support/aetherui/module.ae`:

```
ui_backend(root)            → production. No driver source. No listener.
ui_backend_testable(root)   → adds aether_ui_driver.c
```

Default is the non-testable one, so the ~50 apps that do not opt in stop
shipping it the moment this lands.

### What the seam looks like

The encouraging measurement: the GTK4 server region has only **44** GTK
references in ~1,160 lines, and 21 of those are `g_idle_add` — thread
marshalling, not widget logic. The HTTP and routing code is essentially
generic already. What differs per backend is a small set of operations:

```c
/* aether_ui_driver.h — implemented once per backend */
int  aeui_drv_widgets_json(char* buf, size_t cap);
int  aeui_drv_widget_json(int id, char* buf, size_t cap);
int  aeui_drv_click(int widget_id);
int  aeui_drv_canvas_read_pixel(int cid, int x, int y, int w, int h);
int  aeui_drv_canvas_debug(int cid, int* area, int* commands, int* w, int* h);
int  aeui_drv_screenshot_png(const char* path);
/* ...one per route family... */

/* And the marshalling difference, which is the real per-backend split: */
void aeui_drv_run_on_ui_thread(void (*fn)(void*), void* arg);
```

GTK4 implements the last one with `g_idle_add` + a wait; win32 posts to the
message loop; macOS uses `dispatch_async` to the main queue. That single
function absorbs most of the 21 `g_idle_add` sites and is the honest reason
the two implementations diverged in the first place — they were written
around different threading models rather than around different routes.

### Ordering, and how to not break the matrix doing it

The routes are the contract; 273/0 across four platforms is the gate. So:

1. **Write a route-parity spec first.** Assert that both servers answer the
   same route list, and watch it FAIL on `/canvas/{id}/pixel` vs `/overlays`
   — the drift we already know about. A refactor that starts with a failing
   test that names the defect is worth more than one that starts green.
2. **Resolve the two divergent routes deliberately.** `/pixel` and `/overlays`
   should each either exist everywhere or nowhere. My guess is both should be
   universal, but that is a decision, not a merge artefact.
3. Extract `aether_ui_driver.c` from the standalone one (it is already
   separate, so this is mostly a rename plus the interface calls).
4. Point GTK4 at it: delete the woven region, implement the interface.
5. Split `ui_backend` / `ui_backend_testable`; flip test targets over.
6. Full matrix, all four platforms. The route-parity spec now passes on all
   of them, which is the thing that keeps this from happening again.

## Risks

- **The GTK4 region is not cleanly delimited.** It runs ≈6049–7211 but
  `inject_remote_control_banner` and the widget-tree walkers are called from
  it and may be used elsewhere. Step 4 needs a careful read, not a block
  delete.
- **Threading is the actual hard part.** Not the routes. Getting
  `run_on_ui_thread` right on three backends is where a subtle hang would
  come from, and it is worth a dedicated spec that hammers concurrent
  requests.
- **`/screenshot` differs in kind, not just presence.** On win32 it is
  `PrintWindow`/`BitBlt` (which is why it cannot see the renderer seam at
  all); GTK4 uses a paintable snapshot. The interface should not pretend
  these are the same thing.
- **Route-parity is necessary but not sufficient** — two servers can expose
  the same route and answer differently. The spec should assert shapes where
  it cheaply can, not just names.

## What "done" looks like

1. One `aether_ui_driver.c`; no HTTP server in any backend file.
2. A production build with **no driver symbols** — `nm` proves it, the way
   `nm` currently proves the opposite.
3. An app that calls `enable_test_server` without opting in **fails to link**.
4. A route-parity spec that would have caught the `/pixelgrid` divergence,
   green on four platforms.
5. Matrix still 273/0.
