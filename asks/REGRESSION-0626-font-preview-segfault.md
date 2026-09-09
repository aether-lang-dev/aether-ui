# aether v0.626.0 regression — segfault in the font-preview path (blocks our pin)

**From:** the aether-ui line, 2026-09-03. **Status:** our CI pin stays at
**v0.613.0**; we cannot move to 0.626.0 until this is understood.

## The A/B

Same source (aether-ui `2e4d7da`), same machine, same one click:

| toolchain | result |
| --- | --- |
| v0.613.0 | survives, selection label updates |
| v0.626.0 | **Segmentation fault (core dumped)** |

Rebuilt `apps/font_picker` under each toolchain with a fresh
`AEB_CACHE_DIR` (aeb's cache key does not include the toolchain — see
`aeb/asks/cache-key-omits-toolchain-identity.md` — so a shared cache
would have served the old binary and hidden this).

## Reproducer

```sh
AETHER_UI_WITH_DRIVER=1 aeb apps/font_picker/.build.ae
AETHER_UI_TEST_PORT=9222 target/build/apps/font_picker/bin/font_picker &
# click the row whose label starts "DejaVu Sans": POST the label's PARENT id
curl -sX POST "http://127.0.0.1:9222/widget/<parent-of-DejaVu-Sans-label>/click"
```

On 0.626.0 the process is gone within a second. Suite view:
`tests/spec_matrix.sh font_picker` → **6 pass, 13 fail**, first failure
`POST /widget/N/click was not 2xx (transport ok?)`, then an EMPTY
`/widgets` body — the app is dead, not misbehaving. Everything else is
green: full matrix on 0.626.0 is **412 / 13**, and all 13 are this one
suite. The build itself is clean — 108 nodes, 0 failures, no source
changes needed to compile on 0.626.0.

## What the click runs

`on_select` → `refresh_selection` → `draw_preview` (apps/font_picker/
font_picker.ae:455), which re-parses the TTF and regenerates glyph
outlines through `vg/font.ae` — heavy float→int work over
`floatarr.get_unchecked` indices, plus `vg/geom/path_builder.ae`.

## What we RULED OUT

`math.lrint` is **not** the cause, despite being 0.626.0's headline
change and despite `vg/font.ae` being one of the 30 modules declaring
`extern lrint`. We migrated `vg/font.ae` to `math.lrint` alone and
rebuilt: **it still segfaults identically**. So the regression is
something else in 0.613.0→0.626.0. Reverted that experiment; our tree is
unchanged.

Candidate areas from your own changelog, offered as starting points
rather than conclusions — 0.624.0's struct-`string`-field ownership
change (the destructor/setter double-free rework) looks closest to a
crash of this shape, and the preview path allocates and frees per-glyph
strings heavily.

## What we need

A backtrace, which we cannot produce here: this box has no gdb and
`kernel.core_pattern` is `|/sbin/crash_reporter`, so cores are captured
by the OS. If you can reproduce on a box with a debugger, that is the
fastest path. If it does not reproduce for you, say so and we will find
a way to get you one.

---

## RESOLVED — aether `e932c61c`, shipping in v0.627.0 (verified 2026-09-03)

Fixed upstream as #1873, and the diagnosis is more precise than our
"candidate areas" guess. It WAS the 0.624.0 struct-`string`-ownership
rework (#1866), but not in the way we supposed:

#1866 correctly made the ownership wrapper fire for a pointer-to-struct
PARAMETER, so a setter's store takes ownership like a local assignment
does. But the emitted store also READS the box's `_heap_<field>` tracker
to decide whether to free the previous value — and a box made with
`malloc(n) as *T` has never had that tracker initialised. Non-zero
garbage there means freeing a garbage pointer.

That is exactly our shape: `malloc(n) as *T` is the documented pattern
this repo uses for every escaping app state (font_picker's `App`,
auto_hide's `DemoState`, chromed's `ToggleFace`, turtle's
`TurtleUiState`) — chosen deliberately, because a closure capturing a
plain local captures it BY VALUE at creation time, while field reads
through a pointer stay live. So a previously-safe store became a
load-bearing read of uninitialised memory, and the font preview — which
sets string fields through setters on a malloc'd box, per glyph — hit it
first and hardest.

The fix keeps #1866's leak repair (the parameter case still SETS the
tracker, so destructors reclaim) while no longer READING a tracker it
cannot trust.

**Verified here on a pre-release 0.627.0 build (`114b6d44`):** the
one-click reproducer above now survives and the selection label reads
`selected: DejaVu Sans` — the assertion that was failing. Full matrix
result follows in the pin-bump commit.

**Correction to our own analysis, for the record:** this report
originally suspected `math.lrint`, since it was 0.626.0's headline change
and `vg/font.ae` is one of 30 modules declaring `extern lrint`. We
migrated that module to `math.lrint` and it segfaulted identically, which
ruled it out before we filed — worth keeping visible, because the
plausible-and-wrong theory cost more time than the measurement that
killed it.
