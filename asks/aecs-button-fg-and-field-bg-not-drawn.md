# AeCS: a button's colour and a text field's background reported, not drawn

**From:** the chess app (RexxJS/example-app-chess, `app/chess/view/looks.ts`,
TODO.md 3.6), through sae's `ui.styles`, 2026-10-16. macOS, sae-driver with
`AETHER_UI_HEADLESS=1`, aether-ui at its checkout of that day.

## What happens

A page switches to a dark theme with one AeCS sheet:

```ts
ui.styles({
  root: { background_color: 0x1b1d20, color: 0xe4e4e4 },
  container: { background_color: 0x1b1d20 },
  label: { color: 0xe4e4e4 },
  button: { color: 0xe4e4e4, background_color: 0x33373c },
  textfield: { color: 0xe4e4e4, background_color: 0x2a2d31 },
});
```

The driver's `/widgets` then reports what was asked, for example for a
button: `"bg":"#33373c","fg":"#e4e4e4"`, and `"bg":"#2a2d31"` for the
text field. But the driver's screenshot draws:

- the containers' and labels' colours as asked (dark ground, light text);
- each **button's background** as asked, but its **label in the default
  dark colour**, so it can hardly be read on #33373c;
- the **text field and the textarea white**, their text dark.

So `fg` on a button and `bg` on a text field or textarea reach the
widget record but not what is drawn. (Whether a windowed AppKit run draws
them is not checked yet; the screenshot is what a spec can see.)

## Why it matters

A dark mode is one of the chess app's looks (TODO.md 3.6), and any app
with a theme sheet meets the same. The chess spec
(`tests/specs/spec_app_chess_looks.ae`) holds the reported styles to the
sheet; it cannot yet hold the drawn ones.

## What is asked

Draw an AeCS `color` on a button's label, and `background_color` (with
`color`) on a text field and a textarea, on every backend (GTK4, AppKit,
Win32, UIKit) and in the driver's screenshot; a spec that samples the
screenshot's pixels to prove it.
