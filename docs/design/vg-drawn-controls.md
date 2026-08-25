# vg-drawn controls — the hybrid fork (comparison + verdict)

> **Status: SCHEDULED (verdict C, hybrid) — Paul, 2026-08-25. Phase 1
> LANDED the same day**: `ui/chrome.ae` (tokens, state-aware control box
> with focus ring, measured centred label, one-call `chrome_control`) +
> `vg.rrect` + `vg/test/test_chrome` (21 asserts, headless recording
> backend, in ci.sh Phase 0). The semantics bridge shipped as the
> documented host-widget pattern — zero new code, the drawn control's real
> host carries the existing `a11y_role/label` — exactly as sketched below.
> The concrete drivers that tipped the decision: win32's frames-sandwich
> skip, its GDI-alpha caching gate, and the scaled-blit 1:1 drift found in
> FOUR independent backend sites in one day — all the same root (backends
> without a shared drawing path). Native stays the default; every drawn
> control ships behind a flag with a parity spec. Next: phase 2, drawn
> button + toggle on this substrate.

## The question

aether-ui today is a **native-widget toolkit**: a `btn` is a real `GtkButton` /
Win32 `BUTTON` / `NSButton`, a `textfield` a real `GtkEntry` / `EDIT` /
`NSTextField`, and so on. The vg layer (AeVG — a mature, headless, platform-
independent vector engine: rasterizer, path builder, shape factories, text with
anchoring and font-family, Gaussian blur, clip masks, gradients) draws *scenes*
— SVG, the transpiler's output, showcase apps — but not the toolkit's own
controls.

The **Flutter turn** would be to draw the controls too: a `btn` becomes vg
geometry (a rounded rect + centered text + states), not a native button. The
appeal is real and specific:

- **Pixel-identical across backends.** One drawing path, not three native
  toolkits that disagree on metrics, focus rings, and default padding.
- **Fully themeable / effect-capable.** Controls can carry the same shadows,
  gradients, and materials the vg layer already does for scenes; a themed
  control set stops being three per-OS CSS/appearance problems.
- **Compositor-immune.** Drawn-in-window content never touches xdg_popup, DWM
  acrylic-on-child, or NSMenu quirks — the class of bug the overlay layer and
  `AETHER_UI_PICKER=drawn` were built to route around.

The costs are also specific, and this codebase makes them concrete rather than
hypothetical — see the comparison.

## What we already have (this is not zero-to-one)

The roadmap's claim that "overlay, typography, and effects were built as
foundations of the hybrid" is literally true, and there are **working drawn
controls in the tree today**:

- **`AETHER_UI_PICKER=drawn`** — a fully drawn dropdown that replaces the native
  picker (built because sommelier/Crostini never maps xdg_popups). It coexists
  with the native picker behind an env toggle; the driver and specs treat both
  identically. This is the exact "drawn variant selected by a flag, same driver
  contract" mechanism the fork would generalize. *(gtk4 `aeui_picker_use_drawn`
  / `drawn_picker_of` / `.aui-drawn-dropdown`.)*
- **`AETHER_UI_TOOLTIP=drawn`** / `vg_tooltip_drawn` — a drawn tooltip surface.
- **The overlay layer** — an in-window z-stack (scrim, card, toast, dropdown,
  tooltip) that draws *over* the app with real hit-testing, no compositor
  popups. Every drawn control needs exactly this substrate, and it exists and is
  driver-tested.
- **Typography** — `vg.text_extent` / CoreText / GDI text metrics on the same
  font the canvas draws with, `text_anchored`, font-family, wrap/anchor. A drawn
  control's label needs measured text; it's here.
- **Effects** — `vg.shadow` on paths/text, gradients, and now scrim materials.
- **AeVG** — the vector engine itself: shape factories, a software rasterizer
  with scanline fill and clip masks, a `PathBuilder`, blur. Drawing a rounded-
  rect button with a shadow and centered measured text uses only primitives
  that already exist and are unit-tested headless (vg Phase 0).
- **`vg/region.ae`** — a non-overlapping-rectangle region type already exists
  (also the seed of the retained compositor, see below).

So the fork is not "build a widget renderer from scratch." The renderer, the
overlay substrate, the text metrics, and two real drawn controls already ship.
The open work is (a) generalizing the drawn set, and (b) the two genuinely hard
things natives give us for free: **IME text input** and **accessibility**.

## The comparison

Four postures, scored against this codebase's constraints (three backends, a
driver-first house rule, sommelier as a release gate, MIT license).

### A. Stay native (status quo)

- **Pixel parity:** ✗ three toolkits, three sets of metrics/focus/padding.
- **Themeability:** partial — per-OS (CSS on GTK, appearance APIs elsewhere).
- **a11y:** ✓✓ free and real (GtkAccessible / MSAA / NSAccessibility — we just
  wired `a11y_role/label/description` onto exactly these).
- **IME / text input:** ✓✓ free and real (native entries own the IME).
- **Compositor risk:** ✗ the recurring tax — xdg_popups, per-backend menu/
  dropdown/overlay quirks (the reason the overlay layer and drawn picker exist).
- **Effort:** zero (it's what we have).

### B. Full vg-drawn (the pure Flutter turn)

- **Pixel parity:** ✓✓ one drawing path.
- **Themeability:** ✓✓ total.
- **a11y:** ✗✗ **lost** — a drawn button is an opaque rectangle to the platform
  AT. Requires a hand-built **semantics tree** bridged to GtkAccessible / UIA /
  NSAccessibility per backend. This is a large, permanent maintenance surface
  (it is why Flutter maintains a semantics tree).
- **IME / text input:** ✗✗ **brutal** — a drawn text field must implement
  caret, selection, clipboard, bidi, and **IME preedit/composition** by hand,
  per platform. This is the single hardest thing in GUI toolkits; natives spent
  decades on it.
- **Compositor risk:** ✓✓ eliminated (everything is in-window).
- **Effort:** very high, and the a11y + IME costs are *ongoing*, not one-time.

### C. Hybrid — native text inputs, drawn chrome (SwiftUI's answer)

- **Pixel parity:** ✓ for chrome (buttons, menus, dropdowns, tabs, toolbars,
  overlays, maybe tables) — the controls that *don't* need IME.
- **Themeability:** ✓ for chrome; text fields inherit native look (acceptable —
  users expect native entries).
- **a11y:** ◑ native entries keep free a11y; drawn chrome needs a **bounded**
  semantics bridge — but chrome semantics (button/menuitem/tab/listitem with a
  name) is *far* smaller than text a11y, and we already have the role/name/desc
  API to feed it.
- **IME / text input:** ✓✓ **kept native** — the brutal part is never taken on.
- **Compositor risk:** ✓ eliminated for the drawn chrome (which is exactly the
  set that hits compositor bugs today).
- **Effort:** moderate and *incremental* — one control at a time, each behind a
  flag, each with the drawn picker as the working template.

### D. Selective drawn (where natives fail) — today's actual posture

- This is C, done opportunistically and only when a native control genuinely
  fails (sommelier picker, tooltip). It's not a "fork" — it's the pragmatic
  status quo, and it's why B/C aren't zero-to-one.

## Verdict

**If this fork is ever taken, it is C (hybrid), reached by extending D — never
B.** The reasoning is grounded, not stylistic:

1. **IME is the disqualifier for B.** Native text entries own composition/preedit
   correctly on all three OSes; reimplementing that by hand is a multi-quarter,
   never-quite-done effort. C keeps `textfield`/`textarea`/`securefield` native
   and loses nothing.
2. **a11y just became cheap to feed but expensive to fake.** We wired
   `a11y_role/label/description` to native accessibles this cycle. Under B that
   API would need a full semantics-tree backend behind it; under C the native
   inputs keep real a11y and the drawn chrome needs only a small role+name
   bridge — and the API to drive it already exists.
3. **The wins we actually want are chrome wins.** Pixel parity and compositor-
   immunity matter most for menus, dropdowns, tabs, overlays — precisely the
   controls C draws. Text fields looking native is a *feature*, not a loss.
4. **The template already works.** `AETHER_UI_PICKER=drawn` proves the pattern:
   a drawn control, behind a flag, honoring the same `/widgets` + driver
   contract as its native twin. C is "do that, deliberately, for more chrome."

**Recommendation: do not schedule B. Keep D as the default. Only promote to C
when a concrete driver — a theming requirement, or compositor bugs that the
overlay layer can't absorb — makes uniform drawn chrome worth the a11y-bridge
cost.** Until then this stays a named, specified track, like the retained
compositor.

## Phased, ci-gated path (only if C is acted on)

Each phase is independently shippable, leaves native the default, and ships its
drawn variant behind a flag with a driver spec proving parity — the house rules
apply unchanged (driver-first, both-servers, sommelier gate).

1. **Drawn-chrome substrate + theme tokens.** Formalize the overlay-layer +
   AeVG-shape path into a small "draw a control" kit: a themed rounded-rect,
   measured-text label, state visuals (hover/active/disabled/focus-ring), and a
   token set (colors, radii, spacing). No new controls yet — just the shared
   drawing + the semantics-bridge shim (`a11y_role`/name on a drawn node,
   forwarded to the platform AT via a synthetic accessible). Unit-testable
   headless via `canvas_write_png` (house rule 4).
2. **Button + toggle, drawn variant behind a flag.** The simplest chrome. Same
   `/widgets` JSON (`type:button`, text, enabled, `role`/`a11y_name`), same
   click route, a spec asserting drawn and native are driver-indistinguishable.
   Sommelier-gated. Generalizes the drawn-picker precedent.
3. **Menus, dropdowns, tabs, toolbars.** The compositor-pain controls — the
   highest-value drawn set. The drawn picker already exists; bring the rest onto
   the same substrate. Each ships native-default, drawn-behind-flag, parity-
   spec'd.
4. **Tables / lists as drawn (optional).** Only if theming/perf demands it; the
   virtualized `vlist` + delegate columns are already drawn-friendly (real row
   widgets today, but the render path is ours).
5. **Never: drawn text inputs.** `textfield`/`textarea`/`securefield` stay
   native permanently. If a theming need forces a drawn *appearance*, the honest
   move is a native entry with restyled chrome, not a hand-rolled IME.

The **semantics bridge** (phase 1) is the one genuinely new, permanent piece —
a drawn node must expose role+name+state to GtkAccessible / UIA / NSAccessibility
so a screen reader sees a button, not a rectangle. It is bounded (chrome
semantics, not text) and it reuses the `a11y_role/label/description` API already
shipped. That boundedness is the whole reason C is viable and B is not.

## Relationship to the retained compositor

The retained compositor (`retained-compositor.md`) and this fork are
**complementary, independent tracks**. The compositor makes *drawing* cheap
(dirty-region + occlusion, so a drawn scene repaints only what changed); drawn
controls make *what's drawn* include the widgets. Drawn chrome would be a
natural consumer of the compositor (a hovered button dirties only its bounds),
and `vg/region.ae` already seeds both. But neither requires the other: drawn
chrome works on today's immediate-mode canvas (controls are small; whole-canvas
replay is fine at control scale), and the compositor is worth building for
live/animated scenes regardless of whether controls are ever drawn.

## Licensing boundary

The vg engine (AeVG) is a clean-room Aether port of Tsyne's CVG, MIT-consistent.
Any drawn-control work builds on AeVG and the public toolkit-drawing prior art
(the shape of every immediate-mode widget renderer). As with the compositor: do
**not** implement drawn controls by porting GPL widget-toolkit source. Work from
this doc, AeVG's primitives, and the platform a11y APIs' published contracts.
