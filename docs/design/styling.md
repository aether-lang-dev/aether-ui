# AeCS — Aether Cascading Styles

**AeCS** is aether-ui's stylesheet layer: a declarative, cascading styles
model over the imperative `style_*` setters, so a theme is a *document* and
re-theming a running app is one call. The name follows the family convention
(AeVG, aeb, Aeocha) and says what the layer actually does — the cascade
(`class.kind → class → kind → container → root`) is the mechanism.

## Lineage (credit)

The design is modelled on **Swiby** (Jean Lazarou & the Swiby committers,
BSD-licensed) — specifically `create_styles` / `apply_styles` / `load_styles`
and the banking demo's Settings dialog, which live-previews a theme on one
panel and then applies it to the whole running app. Swiby's resolution order
(id → class → element → root) is the model here, minus ids (v1). The
`themes_demo` themes are lifted from swiby's `demo/banking/theme/*.rb` values.

## API (pure module.ae)

```
s = create_styles()
st_font_size(s, "root", 11.0)         // the cascade's fallback
st_color(s, "text", 0x5C458A)         // element rule (driver kind names)
st_color(s, "button", 0x5C458A)
st_bg(s, "container", 0xD6CFE6)       // alias for vstack/hstack/zstack
st_bold(s, "header.text")             // class-scoped rule; widgets opt in
st_color(s, "header.text", 0x6030BF)  //   via add_css_class(w, "header")
apply_styles(0, s)                    // restyle the LIVE tree (0 = all;
                                      //   else a subtree root handle)
```

Properties: `st_color` (text, packed 0xRRGGBB like swiby's themes), `st_bg`,
`st_font_size`, `st_weight` (two-valued — `0` genuinely un-bolds on a
re-theme; `st_bold` is sugar for `st_weight(…, 1)`), `st_radius`,
`st_font_family` (verbatim to the platform — generic CSS families resolve on
GTK4/macOS; on win32 prefer real face names like `"Consolas"`).

Resolution per property, first match wins (swiby's full order):
`#id` → `class.kind` (**every** class, in add order) → `class` (every) →
`kind` → `container` (stacks only) → `root`.

Ids: `style_id(w, "submit")` names a widget for `"#submit"` rules. Stored as
a `#`-prefixed token in the class list — one shared store, tracked on all
three backends, visible in the driver's `classes` field.

Dark/light pairs: `styles_for_mode(light, dark)` returns the sheet matching
`is_dark_mode()` right now. Re-call + re-apply on a mode flip (no
appearance-change event yet).

Semantic roles (v1.3): `color_scheme()` + `role(sc, "primary", 0x…)` name
colours by meaning; `scheme_sheet(sc)` generates an ordinary sheet from them.
See **v1.3** below — roles sit *above* everything in this section rather than
beside it.

## How it works

`apply_styles` walks the live widget registry in-process — four tiny ABI
getters per backend (`widget_count/kind/parent/classes_impl`, wrapping state
the driver hooks already kept) — resolves each property down the cascade, and
calls the existing cross-backend setters (`set_text_color`, `set_bg_color`,
`set_font_size`, `set_font_bold`, `set_corner_radius`). No new rendering
machinery; the sheet is an interpreter over what already worked.

Re-theming = `apply_styles` with a different sheet (the banking-demo move).
New subtrees built after an apply need a re-apply — same as swiby's
`use_styles` per new screen.

## Driver contract (both servers)

The backends' color setters stash the last explicitly-set values;
`aether_ui_styled_bg/fg_impl` read them back and the widget JSON emits
`"bg"/"fg":"#rrggbb"` when present. So a spec proves a theme swap changed
what the platform was actually given, not just the DSL model.
`spec_themes_demo`: element rule, class-beats-element, container bg, and a
live re-theme flipping the same widgets to a second theme's colors.

## The reset-sheet convention (fake un-styling, honestly)

AeCS **overwrites but never unsets**: applying a sheet without a color leaves
the previous color in place (there is no "revert to platform default" ABI).
The convention: **every theme is a full restatement** — each sheet sets every
property any sibling theme sets, so switching themes always overwrites
everything. With `st_weight` two-valued (v1.1), this now genuinely covers
weight too; `themes_demo`'s green theme is the worked example.

## v1 boundaries closed in v1.1 (2026-07-19)

- ~~One class per widget~~ — **all** classes resolve, in add order.
- ~~No id selectors~~ — `style_id` + `"#name"` rules, highest precedence.
- ~~`st_bold` set-only~~ — `st_weight(…, 0/1)`; re-themes un-bold.
- ~~No `font_family`~~ — `aether_ui_set_font_family` landed on all three
  backends (GTK4 Pango attr / CSS; win32 LOGFONT face name; macOS NSFont) +
  `st_font_family` in the sheet + `fontFamily`/`fontWeight` driver readback.
- Dark/light: `styles_for_mode(light, dark)`.

## v1.2 (2026-07-19, same day): liveness, files, properties, mode events

- **`use_styles(s)`** — the current sheet: widget constructors consult it at
  creation (class/id rules land when `add_css_class`/`style_id` run), so
  subtrees built later arrive already themed. `current_styles()` reads it.
- **`load_styles(path)`** — themes as files: `sel.prop = value` lines
  (`//` comments; the LAST dot splits selector from property; props
  color/bg/size/weight/radius/family/opacity). See
  `examples/themes_demo/theme/purple.aecs`.
- **`st_opacity` / `st_gradient` / `st_insets`** — and win32 opacity went
  REAL (layered-window alpha; was a no-op). `"opacity"` joins the driver
  readback.
- **`use_styles_pair(light, dark)` / `on_appearance_change(cb)`** — auto
  re-theme on OS light/dark flips (GTK settings notify / WM_SETTINGCHANGE /
  macOS theme notification), driver-steerable via `POST /appearance?dark=N`
  (single callback slot; latest registration wins).

Spec: 13/13 on GTK4 AND win32 (winbaz), including a clean-close check after
the suite (no lingering process, port freed).

## v1.3 (2026-08-14): semantic colour roles

Name colours by **meaning**, not by widget kind — Flutter's `ColorScheme`, also
Material 3 tokens and iOS semantic colours. A theme becomes a table of eight
decisions rather than a long list of per-kind rules, and a dark theme is the
same eight decisions with different values instead of a second stylesheet to
keep in sync.

```
sc = color_scheme()
role(sc, "primary", 0x5C458A)
role(sc, "on_primary", 0xFFFFFF)
use_styles(scheme_sheet(sc))          // an ORDINARY AeCS sheet
```

**Roles are a generator, not a parallel colour system.** `scheme_sheet()` emits
plain `st_bg`/`st_color` rules into a normal sheet, so everything already built
keeps working untouched: the `class.kind → class → kind → container → root`
cascade, `styles_for_mode`, `ui_states`, `load_styles`, and the driver's
`fg`/`bg` readback. You can `st_*` over the top of a generated sheet to
override one widget without leaving the roles system, because there is no
separate system to leave. It is also why this needed **no backend change** — by
the time a colour reaches a platform it is just a colour.

The eight roles are Flutter's core set minus those needing surfaces we do not
model (`inverse*`, `*Container` beyond one level):

| pair | meaning |
|---|---|
| `primary` / `on_primary` | the main action, and what sits legibly on it |
| `surface` / `on_surface` | ordinary background and its text |
| `error` / `on_error` | destructive or invalid |
| `accent` / `outline` | secondary emphasis; borders and dividers |

`on_X` is the contrast partner of X and is always used as the **foreground**
wherever X is the background. That pairing is the point: setting a background
without its partner is how themes end up with dark-on-dark text after a mode
flip, so each half only lands when actually specified, and unset roles emit
nothing (a partial scheme layers rather than blanks).

The mapping is deliberately small: `surface` → root + container, `primary` →
button, `error` → the opt-in `.danger` class, `accent` → the `.accent` class,
`outline` → divider + a 1px textfield border. `error` reaching **only** the
widget that opted in is the cascade doing the work, not the roles code — and
the spec pins it with an unclassed button beside a classed one.

Also: `role.<name> = #RRGGBB` lines in `load_styles` files, `role_of` (returns
−1 rather than 0 when unset, since black is a legitimate colour),
`scheme_into` for layering under hand-written rules, and `use_scheme_pair`,
which routes both schemes through the existing `use_styles_pair` machinery so
an OS mode flip re-themes by exactly the path a hand-written pair does.

Unknown role names are accepted and stored — an app may carry its own
(`brand`, `warning`) and read them back with `role_of` to paint bespoke
chrome — but only the eight known names generate rules.

Spec: `examples/roles_demo` + `tests/roles_demo`, 6 assertions.

## Still-open boundaries (deliberate)

- No live re-matching, inheritance, pseudo-states, or descendant
  combinators — those would make AeCS a style *engine*; see the honest
  AeCS-vs-CSS comparison in the session notes.
