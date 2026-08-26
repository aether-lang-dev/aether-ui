# Aether UI for Aether

Port of the [Perry](https://github.com/PerryTS/perry) UI framework to Aether.
Declarative widget DSL backed by GTK4 (Linux and FreeBSD), AppKit (macOS),
and a native Win32 backend (Windows) — the three backend implementations
share the same ABI declared in `backend/aether_ui_backend.h`. Uses Aether's
trailing-block builder pattern.

## Credits

This module is a from-scratch Aether + C rewrite of the aether-ui Rust crates
from the [Perry project](https://github.com/PerryTS/perry) by the Perry
contributors. The Rust implementations (`aether-ui-gtk4`, `aether-ui-macos`, and
the core `aether-ui` crate) were used as reference for architecture, widget
API design, reactive state bindings, and platform-specific GTK4/AppKit
patterns. Based on commit
[`7f1e3f9`](https://github.com/PerryTS/perry/commit/7f1e3f979832c33d2da79970ea62bc1b74c2e31a)
of the `main` branch.

Portions Copyright (c) 2026 Perry Contributors collectively, and portions Copyright (c) 2026 Aether Contributors collectively. MIT License.

### Apps built on other people's ideas

Some apps under `apps/` port or borrow from existing projects. Each
carries its own `NOTICE` (and the upstream licence verbatim) beside its
source; the summary:

- **Maerkdown** (`apps/maerkdown`) — the word-as-widget markdown editor.
  Its extended inline syntax (`++insertion++`, `||spoiler||`,
  `==highlight==`, `^superscript^`, `~subscript~`) is taken from the
  [Extended Markdown Syntax](https://github.com/kotaindah55/extended-markdown-syntax)
  plugin for Obsidian by Kotaindah55 (Sheva Ihza),
  [MIT](https://github.com/kotaindah55/extended-markdown-syntax/blob/main/LICENSE).
  The delimiters and their meanings come from that project's documented
  rules; no code was copied, and the parser is an independent
  implementation over this editor's own document model.
- **Font Picker** (`apps/font_picker`) — a rule-for-rule port of
  [Javascript Font Picker](https://www.jsfontpicker.com/), MIT, portions
  Copyright (c) 2024-2025 Zygomatic.
- **Falling Blocks** (`apps/falling_blocks`) — derived from
  [fallingblocks](https://github.com/SanderKlootwijk/fallingblocks) and
  therefore **GPL-3.0**, unlike the rest of this repository. See the
  header in that app's source before distributing binaries built from it.

## Quick start

### Linux (GTK4)

```bash
sudo apt install libgtk-4-dev   # Debian/Ubuntu
./build.sh examples/counter/counter.ae
./build/counter
```

### FreeBSD / GhostBSD (GTK4)

Same GTK4 backend as Linux; `build.sh` detects FreeBSD and uses clang.

```bash
sudo pkg install gtk4 pkgconf   # ensure a zlib.pc exists for freetype2 -> zlib
./build.sh examples/counter/counter.ae
./build/counter
```

`tests/spec_matrix.sh` needs no display of its own — on FreeBSD it starts a
private Xvfb (`pkg install xorg-vfbserver`), unprivileged, and any pre-set
`$DISPLAY` is respected instead.

### macOS (AppKit)

```bash
./build.sh examples/counter/counter.ae
./build/counter
```

### Windows (native Win32)

Build from an MSYS2 MinGW64 shell (no extra dev libraries — USER32, GDI+
and Common Controls ship with Windows itself):

```bash
./build.sh examples/counter/counter.ae
./build/counter.exe
```

The C-level suites build through the same script (a `.c` source links against
the platform backend directly), and run headless:

```bash
# widget + driver smoke suite for the current backend
./build.sh tests/test_widgets.c test_widgets && AETHER_UI_HEADLESS=1 ./build/test_widgets

# microbenchmarks, CSV to stdout
./build.sh benchmarks/bench_widgets.c bench_widgets && AETHER_UI_HEADLESS=1 ./build/bench_widgets
```

The whole pipeline, the way CI runs it:

```bash
./ci.sh                  # build everything, smoke-launch, run every driver spec
./tests/spec_matrix.sh   # just the AetherUIDriver specs
```

`ci.sh` also cross-compiles the Win32 backend when a mingw-w64 compiler is
present (`brew install mingw-w64`, or `apt install gcc-mingw-w64-x86-64`), and
says SKIP when there is none. That is not a substitute for running on Windows,
which nothing here does; it is what stops the one backend nobody can execute
from being edited blind.

See [docs/design/win32-gdiplus-renderer.md](docs/design/win32-gdiplus-renderer.md)
for the Win32 rendering model, and [docs/README.md](docs/README.md) for the
rest of the design notes.

## DSL with Scope

Aether UI is a **"DSL with Scope"** — Matz's own name (he coined it when
asked to name the pattern) for the builder-block style: nested blocks that
describe structure declaratively while keeping full imperative power, with an
*implicit receiver* so children wire to their parent without explicit
plumbing. It runs in the Smalltalk-blocks / Ruby-Shoes / Groovy-SwingBuilder /
Kotlin-Compose / SwiftUI lineage — and, unlike a markup format, the blocks are
**executed code**, not parsed into a DOM for some later actioning. See
[Paul Hammant's "That Ruby and Groovy Language Feature"](https://paulhammant.com/2024/02/14/that-ruby-and-groovy-language-feature.html)
for the full tour, and Aether's own
[`docs/closures-and-builder-dsl.md`](https://github.com/aether-lang-org/aether/blob/main/docs/closures-and-builder-dsl.md)
for the mechanism (trailing blocks, the `_ctx` implicit-receiver convention,
and `builder … with` "configure then execute").

A UI is opened inside a **surface** scope. The surface's *kind* decides its
lifecycle (see [Surfaces](#surfaces-window--render_to--record) below):

```aether
import ui

main() {
    counter = ui.ui_state(0)

    ui.window("My App", 400, 200) {
        ui.vstack(10) {
            ui.text("Hello World")
            ui.text_bound(counter, "Count: ", "")
            ui.hstack(5) {
                ui.btn("+1") callback {
                    ui.ui_set(counter, ui.ui_get(counter) + 1)
                }
                ui.btn("-1") callback {
                    ui.ui_set(counter, ui.ui_get(counter) - 1)
                }
            }
        }
    }
}
```

The `window(…) { … }` block builds the tree, then — because it's a `builder`
function whose body runs *after* the block — opens the window and runs the
event loop. No trailing `app_run(root)`: the surface *is* the entry point.

## Surfaces (window / render_to / record)

A **surface** is the ambient destination a widget/drawing block populates.
The kind decides lifecycle:

| Surface | Lifecycle | What it is |
|---------|-----------|------------|
| `window(title, w, h) { … }` | **lived** — runs the event loop, ends on window close | An on-screen interactive window. Absorbs the old `app_run`. |
| `render_to(target, w, h) { … }` | **bounded** — one render pass, returns | Draw into a target: pixel buffer, PNG, PDF, **paper**. No event loop. |
| `record(w, h) { … }` | **bounded** — captures, returns | A test/recording surface — inspect what was built. No event loop. |
| `window_run(title, w, h, root)` | lived | Explicit-root variant of `window` for trees built imperatively (e.g. a `root_grid` whose cells are `grid_place`'d in). |

Interactive verbs (`onclick`, `onhover`) used inside a **bounded** surface are
*diagnostic-inert*: they render but the handler never fires (there's no event
loop to deliver to). The diagnostic is **collected on the surface** by default
(read it with `surface_diagnostics(handle)`); routing it to stderr or a hard
fail is an explicit opt-in, never the default — the framework never writes to
a stream you didn't ask it to.

**Inside a surface block, use the context-attaching layout verbs** (`vstack`,
`hstack`, `zstack`, …) — **not** the `root_*` variants (`root_vstack`,
`root_hstack`). The `root_*` verbs are *detached*: they take no builder context
and so don't attach to the enclosing surface, leaving you with a window that
maps but renders blank. The `root_*` forms exist only for the explicit-root
`window_run(title, w, h, root)` path, where you build the tree imperatively and
hand the root in. Inside `window {…}` / `render_to {…}` / `record {…}`, always
`vstack` (which the compiler auto-parents to the surface via the `_ctx`
convention).

Why three verbs instead of one `app_run`? Because `app_run` welded together
three jobs — create the window, mount the tree, run the loop — and forced that
*lived* shape onto every program. Most surfaces aren't lived: a render-to-PNG,
a print-to-paper, a headless test needs no loop and ends by reaching `}`. Only
a live window has "a life of its own" that ends on an external event, so only
`window` carries the loop.

## Widgets available

| Widget      | Aether function                                       | GTK4               | AppKit                  | Win32                      |
|-------------|-------------------------------------------------------|--------------------|-------------------------|----------------------------|
| Text        | `ui.text("label")`                             | GtkLabel           | NSTextField (label)     | STATIC                     |
| Button      | `ui.button("label") callback { }`              | GtkButton          | NSButton                | BUTTON (BS_PUSHBUTTON)     |
| VStack      | `ui.vstack(spacing) { children }`              | GtkBox vertical    | NSStackView vertical    | AetherUIStack (custom)     |
| HStack      | `ui.hstack(spacing) { children }`              | GtkBox horizontal  | NSStackView horizontal  | AetherUIStack (custom)     |
| Spacer      | `ui.spacer()`                                  | Expanding GtkBox   | NSView flex filler      | flex placeholder           |
| Divider     | `ui.divider()`                                 | GtkSeparator       | NSBox separator         | GDI line (custom class)    |
| TextField   | `ui.textfield("hint") callback \|val\| { }`    | GtkEntry           | NSTextField             | EDIT                       |
| SecureField | `ui.securefield("hint") callback \|val\| { }`  | GtkPasswordEntry   | NSSecureTextField       | EDIT (ES_PASSWORD)         |
| Toggle      | `ui.toggle("label") callback \|active\| { }`   | GtkCheckButton     | NSButton (switch)       | BUTTON (BS_AUTOCHECKBOX)   |
| Slider      | `ui.slider(min, max, init) callback \|val\|`   | GtkScale           | NSSlider                | TRACKBAR (comctl32)        |
| Picker      | `ui.picker() callback \|idx\| { }`             | GtkDropDown        | NSPopUpButton           | COMBOBOX (CBS_DROPDOWNLIST)|
| TextArea    | `ui.textarea("hint") callback \|val\| { }`     | GtkTextView        | NSTextView              | EDIT (ES_MULTILINE)        |
| ProgressBar | `ui.progressbar(0.75)`                         | GtkProgressBar     | NSProgressIndicator     | PROGRESS (comctl32)        |
| ScrollView  | `ui.scrollview() { children }`                 | GtkScrolledWindow  | NSScrollView            | AetherUIStack + WS_VSCROLL |
| Grid        | `ui.root_grid(cols, rspace, cspace)` + `grid_place(...)` | GtkGrid   | NSGridView              | AetherUIGrid (custom)      |
| Menu bar    | `ui.menu_bar()` + `menu()` + `menu_item()`     | GMenu / GActionMap | NSMenu                  | HMENU (CreateMenu/SetMenu) |

## Presentation, keys and drops

Setters and handlers that control how existing widgets behave. Each names the
platform mechanism, and where a platform cannot do something it is said here
rather than left to be discovered: the driver reports the mode that was
actually applied, never the one that was requested.

```aether
ui.text_truncate(label, "middle")     // none | head | middle | tail
ui.image_fill(pic, "cover")           // original | contain | cover | stretch
ui.image_tint(icon, 0.2, 0.5, 1.0)    // recolour a template/symbolic image
icon = ui.file_icon("/some/path")     // the OS icon for that KIND of file
ui.set_file_icon(icon, "other.md")    // rebind a live icon widget
dir  = ui.pick_folder("New file in", "")   // native folder chooser
```

| Verb | GTK4 | AppKit | Win32 |
|------|------|--------|-------|
| `text_truncate(h, mode)` | `PangoEllipsizeMode` | `NSLineBreakByTruncating*` | `SS_ENDELLIPSIS` / `SS_PATHELLIPSIS`; **no head ellipsis, so head applies as tail** |
| `image_fill(h, mode)` | `GtkPicture` content-fit | `NSImageScaling`, cover drawn directly | one style bit only; **contain and cover apply as original, never stretch** |
| `image_tint(h, r, g, b)` / `image_untint(h)` | CSS colour of a symbolic `GIcon` | template image + `contentTintColor` | bitmap recoloured once, original kept for untint |
| `file_icon(path)` / `set_file_icon(h, path)` | `GIcon` from the content type | `NSWorkspace iconForFile` | `SHGetFileInfoW` |
| `pick_folder(title, start_dir)` | `SELECT_FOLDER` chooser | `NSOpenPanel` (directories) | `SHBrowseForFolderW` |
| `window_on_key(cb)` / `on_key(widget, cb)` | BUBBLE-phase key controller | `NSEvent` local monitor | `WM_KEYDOWN` translated ahead of `IsDialogMessageW` |
| `on_file_drop(cb)` | `GtkDropTarget` over `GDK_TYPE_FILE_LIST` | `NSPasteboardTypeFileURL` | `DragAcceptFiles` + `WM_DROPFILES` |
| `draggable(h, path)` | `GtkDragSource` over a `GFile` | `NSDraggingSession` over an `NSURL` | OLE `DoDragDrop` offering `CF_HDROP` |

`open_file(title, start_dir)`, `save_file(title, name)` and `pick_folder` are
native modals, so all three return `""` under `AETHER_UI_HEADLESS` rather than
block a machine with no seat to dismiss them.

### Keys, and why there are two kinds

`shortcut("Ctrl+R")` and friends answer *"was THIS combo pressed"*.
`window_on_key` answers *"what was pressed"*, which is what type-ahead needs
and what no number of registered shortcuts can express. The any-key handler
fires only when no shortcut consumed the key, so accelerators keep priority,
and it never swallows the key, so whatever has focus still receives it.

```aether
ui.shortcut("Ctrl+R") callback { reload() }          // a bound combo
ui.window_on_key(|k: string, m: int| {               // anything at all
    if k == "BackSpace" { go_up() }
})
ui.on_file_drop(|paths: ptr, n: int| {               // files from another app
    first = string.string_array_get(paths, 0)
})
ui.draggable(row, "/home/me/notes.md")               // drag a file OUT
```

### SVG assets

`image(path)` hands the file to the platform's image decoder, and none of the
three decodes SVG dependably. aether-ui already ships a complete SVG stack in
`vg` (the one `apps/svg_render_png` matches `rsvg-convert` with), so `ui.svg`
routes an SVG through that instead: identical pixels on every backend, no
platform decoder, no conversion step.

```aether
import ui.svg (svg_image, svg_image_sized)

svg_image("assets/logo.svg", 64)          // longer side 64, aspect kept
svg_image_sized("assets/logo.svg", 80, 24) // exact box, aspect ignored
```

Opt-in like `ui.icons`, and for the same reason: an app showing SVG assets
should not have to link the icon vocabulary, and an app that wants a close
glyph should not link the SVG parser. The result is a canvas (a drawing), not
a bitmap, so `image_fill` and `image_tint` have nothing to act on; size it at
the call. A file that cannot be read returns handle 0 rather than taking the
window down.

## Reactive state

```aether
counter = ui.ui_state(0)              // create state cell
ui.text_bound(counter, "Val: ", "")   // auto-updating text
ui.ui_set(counter, 42)                // triggers re-render
val = ui.ui_get(counter)              // read current value
```

## Widget accessors

```aether
ui.set_text(handle, "new text")       // set textfield value
text = ui.get_text(handle)            // get textfield value
ui.set_toggle(handle, 1)              // set toggle on/off
ui.set_slider(handle, 75.0)           // set slider position
ui.set_progress(handle, 0.5)          // set progress bar
```

## Examples

| Example | Widgets demonstrated |
|---------|---------------------|
| [`examples/counter`](examples/counter) | text, button, hstack, vstack, spacer, divider, reactive state |
| [`examples/form`](examples/form) | textfield, securefield, toggle, slider, textarea, progressbar |
| [`examples/picker`](examples/picker) | picker (dropdown), picker_add |
| [`examples/styled`](examples/styled) | form, section, zstack, bg_color, bg_gradient, font_size, corner_radius |
| [`examples/system`](examples/system) | alert, clipboard, dark mode detection, sheet |
| [`examples/canvas`](examples/canvas) | canvas drawing, fill_rect, stroke, on_hover, on_double_click |
| [`examples/testable`](examples/testable) | AetherUIDriver test server, sealed widgets, remote control banner |
| [`examples/rebuild_demo`](examples/rebuild_demo) | clear_children / remove_child on a grid and a stack |
| [`examples/fileicon_demo`](examples/fileicon_demo) | file_icon, set_file_icon, the OS icon for a kind of file |
| [`examples/imagefill_demo`](examples/imagefill_demo) | image_fill: original / contain / cover / stretch |
| [`examples/keyhandler_demo`](examples/keyhandler_demo) | window_on_key type-ahead, and accelerator priority |
| [`examples/filedrop_demo`](examples/filedrop_demo) | on_file_drop, files dropped from another app |
| [`examples/svgimage_demo`](examples/svgimage_demo) | ui.svg: an SVG file as a widget, drawn through vg |
| [`examples/scrollbg_demo`](examples/scrollbg_demo) | small content inside a scroll area, and theming it |
| [`examples/barfill_demo`](examples/barfill_demo) | a pinned toolbar with a body that takes the slack |

## AetherUIDriver — automated UI testing, baked in

Aether UI ships with a built-in HTTP test server that lets any language
with an HTTP client drive the app:

```aether
ui.enable_test_server(9222)
```

Or set `AETHER_UI_TEST_PORT=9222` in the environment before launching —
no code changes needed. A red "Under Remote Control" banner is injected
so a user can't mistake a test-driven session for a real one.

The HTTP API exposes `/widgets` (list + filter), `/widget/{id}` (state),
`/widget/{id}/click | set_text | toggle | set_value` (mutations), and
`/state/{id}` + `/state/{id}/set` (reactive-state cells). See the full
reference and end-to-end examples in
[`tests/test_driver.sh`](tests/test_driver.sh) (curl against every route)
and the Aether specs under [`tests/`](tests) driven by
[`tests/lib/uidriver.ae`](tests/lib/uidriver.ae). Set `AETHER_UI_HEADLESS=1`
to run any of them with no window on screen.

For most native UI frameworks you have to bolt on Selenium/Appium. With
Aether UI it's part of the framework and works identically on macOS,
Linux, FreeBSD, and Windows via the shared
[`backend/aether_ui_test_server.c`](backend/aether_ui_test_server.c).

### Widget sealing

Mark widgets as non-automatable — the test server returns 403 for sealed widgets:

```aether
danger = ui.btn("Delete Everything") callback { ... }
ui.seal_widget(danger)
```

This maps to Aether's `hide`/`seal` philosophy: the app author declares which
capabilities the test harness is denied, not the other way around.

## Architecture

| Layer | File | Role |
|-------|------|------|
| Aether DSL | `ui/module.ae` | Builder-pattern wrappers with `_ctx` auto-injection; surface verbs (`window`/`render_to`/`record`) |
| GTK4 backend | `backend/aether_ui_gtk4.c` | Linux + FreeBSD: GTK4 C API calls, Cairo canvas, test server |
| macOS backend | `backend/aether_ui_macos.m` | macOS: AppKit Objective-C |
| Win32 backend | `backend/aether_ui_win32.c` | Windows: USER32 + GDI+ + Common Controls |
| C header | `backend/aether_ui_backend.h` | Shared backend ABI — implemented by all three backends (four platforms; FreeBSD shares GTK4) |
| Build script | `build.sh` | Auto-detects platform (Darwin/Linux/FreeBSD/MinGW) |
| Spec matrix | `tests/spec_matrix.sh` | Runs every AetherUIDriver spec, one app at a time |
| Widget tests | `tests/test_widgets.c` | Cross-platform C-level smoke suite (40 assertions) |
| Driver tests | `tests/test_driver.sh` | HTTP integration against the embedded test server |
| Benchmarks | `benchmarks/bench_widgets.c` | CSV microbenchmarks — widget create, layout, state, canvas |

## Platform support

| Platform | Backend                         | Status                                                                             |
|----------|---------------------------------|------------------------------------------------------------------------------------|
| Linux    | GTK4  (`backend/aether_ui_gtk4.c`)      | Full — all widgets, canvas, events, styling, AetherUIDriver test server            |
| macOS    | AppKit (`backend/aether_ui_macos.m`)    | Full — all widgets, canvas, events, styling, AetherUIDriver test server            |
| Windows  | Native Win32 (`backend/aether_ui_win32.c`) | Full — USER32 + GDI+ + Common Controls; per-monitor DPI v2; immersive dark mode; AetherUIDriver via winsock2 |
| FreeBSD  | GTK4  (`backend/aether_ui_gtk4.c`)      | Full — shares the Linux backend; clang build, private-Xvfb spec runs           |

"Full" above means the backend implements the whole widget/canvas/event/
styling surface plus AetherUIDriver — not that every suite is green on every
box. `tests/spec_matrix.sh` is the authority; run it on the platform you care
about. Most recent full runs: Linux 228/0, FreeBSD 223/0 (only `lismusic`,
which needs the sqlite contrib archive installed on that host).

## Status

All groups (1-7) plus AetherUIDriver are implemented on every backend.
`./build.sh tests/test_widgets.c test_widgets` builds the cross-platform smoke
suite (47 assertions, headless) and `./build.sh benchmarks/bench_widgets.c
bench_widgets` builds the microbenchmarks, which print a CSV of per-operation
latencies. `./ci.sh` runs everything.
