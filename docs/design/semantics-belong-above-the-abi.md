# Semantics belong above the ABI

*Written 2026-08-14, from an audit of all 241 externs in `ui/module.ae` and
the eleven leaks it found. A twelfth — the easing curve — turned up the same
day while adding spring animation, which is the argument for the rule rather
than against it: the pattern is easy to reintroduce and hard to notice.*

## The rule

A backend should **translate**, never **interpret**.

"Here is a path, here is a paint, draw it" is translation. "Decide what
`fill-rule` means" is interpretation — and interpretation in three backends is
three chances to be wrong, three places to fix, and no single place to look.

The C is not the problem. aether-ui is 64% Aether / 36% C, and 86% of that C
is three implementations of one surface (gtk4 7.0k, win32 7.9k, macOS 6.0k
lines) behind a 241-function ABI. You cannot write GTK4 or AppKit bindings in
anything else. What matters is **what** the C decides.

## How a leak looks

Every one had the same shape:

1. The vg layer reads a real SVG/CSS property.
2. It reduces that property to something smaller — a bit, a scalar, a
   default — before dispatch.
3. Each backend, handed the reduced form, invents the rest.
4. The reference backend's invention looks right, so nothing flags it.

The **easing curve** (found 2026-08-14) is the pattern arriving late and
unmistakably. `ui.transition(h, prop, ms, easing)` takes a curve NAME, and all
three backends reduced it to a boolean with the same line:

    strstr(t, "linear") ? 0 : 1

So every value that was not "linear" became ease-out. `"spring"` was accepted
and silently ignored on all three — measured, it produced a curve identical to
ease_out with no overshoot at all. Nothing flagged it because ease-out is a
perfectly plausible fade, which is step 4 exactly: the invention looks right.
Fixing it meant carrying the curve rather than a bit, and because a spring
overshoots — something no CSS timing function expresses — GTK4 takes an
overshooting bezier while win32 and macOS drive the closed form
`p(t) = 1 - e^(-6t)·cos(9t)` on their own timers.

Step 4 is why these survive. `fill-rule` is the cleanest example: no backend
was ever told the rule, so each hardcoded one. GTK4 looked correct purely
because cairo's default (winding) happens to match SVG's default (nonzero) —
so it had been **silently wrong on every `fill-rule="evenodd"` file** for as
long as the feature existed. Fixing it moved GTK4 too: `accessible.svg`
17.87 → 0.39, `ruby` 4.95 → 0.05.

## The twelve

| leak | what was lost | fixed in |
|---|---|---|
| `fill-rule` | the rule itself; each backend hardcoded one | all 3 + ABI |
| gradient cap/join | stroke caps on gradient strokes | all 3 + ABI |
| `spreadMethod` | pad/reflect/repeat; win32 hardcoded repeat | win32 |
| `gradientTransform` | the ellipse — collapsed to a scalar radius at **three** sites | all 3 + ABI |
| gradient stop offsets | stops spaced evenly instead of at their offsets | win32 |
| radial geometry | gradient sized from the shape's bbox, not its own definition | win32 |
| `font-family` | the family name, reduced to a mono/not-mono bit | all 3 + ABI |
| UA default family | each backend's own fallback face (sans / Segoe UI / system) | vg layer |
| stroked text | not drawn at all on macOS; wrong face on GTK4 | macOS + gtk4 |
| overlay transition kind | `slide`/`scale` silently fading; macOS discarding it | win32 + macOS |
| navstack `title` | discarded by all three — hence no back chrome | **open** |
| easing curve | reduced to a BIT; every value but "linear" became ease-out | all 3 |

## Two instruments, and neither is sufficient alone

**The SVG corpus (MAE vs librsvg) is a regression tripwire, not an oracle.**
It answers "did I break something that used to work". It cannot answer "is
this correct", and on this work it argued for demonstrably worse code **three
times**:

- `php.svg` scores 4.15 with a radial stroke painted by a *linear* brush.
  Three more-correct radial geometries all scored worse.
- `decimal.svg` asks for `serif`. librsvg on Linux resolves that to Noto
  Serif; Windows has no Noto Serif and picks Times New Roman. Both are right
  on their own platform, so win32's score *rose* (20.22 → 46.36) as its font
  selection became more correct.
- macOS drew **no text at all** headless. Fixing it made the score worse,
  because blank text scored better than text in a different face.

**Purpose-built oracles answer correctness.** `tests/radial_ellipse_check.py`
measures the axis ratio of a gradient whose expected value is known *by
construction* — no reference renderer involved. A renderer that collapses an
ellipse to a circle reads 1.00 where 2.00 is required, and no amount of
font-substitution noise can hide that.

**The driver is the only instrument for widget-level behaviour.** Overlay
transitions, navigation stacks, focus, selection — none are visible to the
corpus or to golden images. `spec_navstack_demo` caught a real win32 pop bug
on its first run.

Corpus coverage is also thinner than it looks: of 20 files with a non-uniform
`gradientTransform`, only **one** (`gallardo`) is both affected and fairly
measurable — the rest either score well already with the wrong geometry, or
have no `viewBox` so their MAE is dominated by canvas-size inference.

## The ratchet

Widening an ABI across three backends invites a window where the vg layer
emits data no backend understands. The sequence that avoids it:

1. **Pin the loss with a test that fails.** Purpose-built, not corpus-based.
2. **Carry the new data alongside the old**, with the old still
   authoritative. Verify **inert** — byte-identical output.
3. **Convert one backend at a time.** The others still read the old field and
   are provably unchanged.
4. **Only then** consider removing the old field.

Step 2 is what makes it safe. The `gradientTransform` work verified 208 files
byte-identical before any backend consumed the new geometry, then converted
GTK4 → win32 → macOS independently, each verified against the oracle.

## On ABI width

This work **added** parameters (three for the ellipse, one for `fill-rule`,
one for `font-family`) against a stated goal of the surface trending *down*.
That is the right trade, and worth being explicit about: those parameters
carry **data the renderer needs** so the backend can stop **deciding**.

The metric that matters is *decisions per backend*, not parameters. A wider
surface with dumber backends is the success condition. The test: adding a
platform should cost work proportional to its **surface** ("how do I draw a
path here?") and not to its **semantics** ("what does even-odd mean?").

## What is still open

- **navstack `title`** — discarded by all three backends; no back chrome.
- **win32 child opacity, and the instrument to see it** — `style_opacity` is
  top-level-only there, so `ui.transition`'s easing has nothing to animate. The
  sharper problem is that three capture paths (driver `/screenshot`,
  `GetPixel`, `PrintWindow`) are all blind to child controls on that box, and a
  *blank* capture still produces a plausible number. That is a new failure mode
  for the "two instruments" argument above: not a tripwire that argues for
  worse code, but an instrument that reports a confident measurement of
  nothing. A pixel test must prove it can SEE the thing before its readings
  count — `test_easing_curve.sh` now checks pixel spread and skips if not.
  See TODO.md.
- **win32 navstack pop** — pages remain in the widget census after popping.
  `spec_navstack_demo` is red on win32 on purpose, describing a real defect.
- **`widget_apply_css`** — asymmetric by design rather than leaking: GTK4
  hands the string to `gtk_css_provider_load_from_data`, the others parse a
  subset. CSS is a GTK-native format; the neutral surface already exists one
  level up as AeCS.
- **Per-platform references** — win32 and macOS are permanently penalised by
  a Linux-fontconfig reference. The honest fix is rendering references on each
  platform, which is a harness change rather than a renderer one.
