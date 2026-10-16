# REPLY: AeCS: a button's colour and a text field's background reported, not drawn

**RESOLVED**, 2026-10-16, in aether-ui `f4326aed` (the fix, the demo and
the pixel test); `e0fa5123` bounds ci.sh's heap(1) step, which hung the
verification run. Answers
`asks/aecs-button-fg-and-field-bg-not-drawn.md` (the chess app's dark look).

## Screenshot only, or a real window too?

A real window too. The driver's `/screenshot` on AppKit renders the
window's own layers (the presentation layer, else `cacheDisplayInRect:`),
so it draws what the views draw; it is not a separate painter. Run windowed
(no `AETHER_UI_HEADLESS`, the window ordered onto the screen), the
pixel test failed the same way as headless: label dark, fields white. The bug was in the
AppKit setters, and only there:

- `aether_ui_set_text_color` coloured an `NSTextField` and nothing else. An
  `NSButton` has no text colour of its own (its title is drawn in the
  control text colour unless it is an attributed title), so a button's `fg`
  was stored for the driver and never drawn. Nor did it reach a text area,
  whose text is its `NSTextView`'s, not the scroll view's.
- `aether_ui_set_bg_color` set the view's layer colour. An editable
  `NSTextField`'s system bezel paints its own ground (white in the light
  appearance) over the layer AND over the cell's `backgroundColor`, rounded
  or square bezel alike; and a text area's `NSTextView` paints its own white
  ground over the scroll view's.
- The text area's inner text view is registered as a widget of its own
  (`textarea_inner`). No sheet names that kind, so `apply_styles` gave it the
  `root` rule: once its colours were drawn, the root's would have landed on
  the very view that draws the text area, over the `textarea` rule.

## What changed

AppKit (`backend/aether_ui_macos.m`):

- A button's `color` is drawn: the colour is kept on the button and its
  title is made an attributed title (the button's font, centred, the
  colour), rebuilt when the label (`set_button_label`) or the font family
  changes. A button never coloured keeps its plain title and the system's
  colour.
- A text field's (and secure field's) `background_color` is drawn: a styled
  field leaves the system bezel, as a CSS background replaces GTK's entry
  frame, and draws its own: the cell's background in the colour, a rounded
  (5pt) 1pt edge a shade off the ground unless the sheet gives a border or a
  radius of its own, and the bezel's inset kept by the field's cell
  (`AetherTextFieldCell` / `AetherSecureTextFieldCell`), so the text does not
  touch the edge and the field keeps its height. A field never styled keeps
  the system bezel.
- A text area's `background_color` and `color` reach its `NSTextView`: its
  background, its text colour, its typing attributes and its caret.

`ui/module.ae`: `apply_styles` (and `use_styles`' creation hook) skip
`textarea_inner`; the text area is styled through the widget.

GTK4, Win32, UIKit and Android needed no change. Each was run against the
same test (below) and already drew both colours: GTK through its CSS
(`color` on the button node, which its label inherits; `background` on the
entry and the text view at application priority), Win32 through
`WM_CTLCOLOREDIT` and the owner-drawn button, UIKit through
`setTitleColor:forState:` and the views' `backgroundColor`, Android through
`setTextColor` (a `Button` and an `EditText` are `TextView`s) and the
background drawable.

## The test, seen failing on the old code

`examples/aecs_dark_demo` builds a label, a button, a text field and a text
area (with text in both fields) and applies the chess app's sheet.
`tests/aecs_dark_demo/test_aecs_dark_drawn.sh` (ci.sh Phase 5e4b) decodes
the driver's `/screenshot` (Python's zlib; no imaging library) and, inside
each widget's rectangle, asks whether the most common colour is the sheet's
background and whether there is light ink and no default-dark ink; then
again with each field focused (an edited field is drawn by its field
editor, a second path that must keep the ground). It maps widget rects onto
the capture by the image's scale (and, on Android, where the capture is the
whole decor view, by the device density, found by locating the root's own
ground), so the same script runs on every backend.

On the old AppKit code it fails 14 checks: the button's label 0 light ink
pixels and 348 dark ones, the text field drawn `#fafafa`, the text area
`#ffffff` with dark text. With the change: all pass.

## Verified where (2026-10-16)

- macOS (AppKit): the pixel test headless and windowed, failing 14 checks
  on the old code and passing on the new. ci.sh on macOS at f4326aed: every
  phase but two passed, Phase 5e4b among them; the two were this Mac's
  tools hanging, not the change: Phase 1e2 (`simctl install` never
  returned) and Phase 1f (`heap` never returned, on any pid, even a shell;
  e0fa5123 bounds it so it fails in 120 s instead of holding the run).
- GTK4: ci.sh on aether-linux-2604 (GTK 4.22): all phases passed. The pixel
  test on aether-linux (22.04, GTK 4.6) and ae-x64 (real x86_64, 22.04):
  green; ae-x64's full ci.sh fails only imagefill and vlist, which 22.04's
  GTK 4.6 always fails (no content-fit before 4.8). GTK was green on the old
  code too.
- Win32: the pixel test on win11 (Windows 11, built with aeb, the driver
  linked): green, old and new. aether-ui's ci.sh does not run on Windows;
  the Mac ci.sh's mingw cross-check compiled and linked the backend.
- Android: the demo as an APK on the headless emulator (`aelane android`,
  AVD aether36, API 36), the pixel test over the forwarded driver port:
  green with no change.
- iOS: compiled and linked (ci.sh Phase 1e, and the demo linked for the
  simulator as Phase 1e2 builds its examples), but NOT run: this Mac's
  CoreSimulator hung on every `simctl install`, even after a shutdown and a
  restart of CoreSimulatorService. The UIKit setters were not changed: a
  `UIButtonTypeSystem` button takes `setTitleColor:forState:` and a
  rounded-rect `UITextField` and a `UITextView` draw their
  `backgroundColor`. Running `examples/aecs_dark_demo` through the
  simulator with `tests/aecs_dark_demo/test_aecs_dark_drawn.sh` is the
  check still owed there.

## For the chess app

Nothing to change in the page: the same `ui.styles` sheet now draws on
macOS. The chess spec can hold the drawn colours the way this test does,
sampling `/screenshot` inside a widget's rect. sae's `AETHER_UI_REF` pin is
moved to e0fa5123 (sae `2667985`), with sae's tests/run_all.sh 54/54 on
macOS against it.

One thing a sheet still does not set: the colours of SELECTED text (a
field taking focus selects its text). Each platform draws a selection in
its own colours; GTK draws the selected text of a dark-styled entry in its
default dark selection foreground. That is a separate ask if a dark look
needs it.
