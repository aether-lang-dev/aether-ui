# Font Picker — an aether-ui port of jsfontpicker

Ported from **[Javascript Font Picker](https://www.jsfontpicker.com/)**,
MIT License, **Copyright (c) 2024-2025 Zygomatic**. The upstream licence is
kept verbatim beside this file as `LICENSE-jsfontpicker`.

`picker_engine.ae` reproduces the upstream logic rule-for-rule — the
compressed family record format, `getDefaultVariant`'s nearest-400 rule,
`Font.toString` naming, Levenshtein fuzzy search against the
`name.length - query.length` threshold, `familyFilter`, `familySort`
(metrics descending), and `sortFamilies`' favourites-then-selection
hoisting. Each ported rule cites its upstream file in a comment.

`font_picker.ae` is the dialog: search, category and sort controls, the
font list, favourites, keyboard navigation, and a live preview.

## Deliberate divergences

| Upstream | This port |
| --- | --- |
| Google Fonts catalogue over the network | Fonts really present on this machine, described in the same compressed record format |
| Preview via CSS `font-family` | Preview drawn from the **actual font file** through `vg.font` + `vg.text_path` — real outlines |
| Favourites in `localStorage` | Favourites in memory |
| Translations, accordion CSS, variant grid | Not ported |

## Tests

* `test_picker_engine.ae` — 45 assertions over the ported rules, with the
  expected values derived from upstream behaviour (ci.sh Phase 0).
* `tests/font_picker/spec_font_picker.ae` — 7 driver specs proving the UI
  drives the engine (spec_matrix / ci.sh Phase 5l).
