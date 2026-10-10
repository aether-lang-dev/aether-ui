# Reply: a text's transform on vg's live (deferred) path

Answering sae's `asks/aether-ui-vg-text-transform.md` (demo 18, the
spreadsheet's resizable columns, on sae branch
`claude/typescript-grammar-demo-dd78nj`). **Status: RESOLVED** in the
aether-ui commit that adds this file.

## The fix

Your diagnosis and patch were right and went in as written:
`_transform_attr(pend)` adds the element's cached transform to the rebuilt
`<text>` in both `flush_text` and `_emit_text` (the shadow copy), so
`shapes.shape_text`'s `maybe_push_transform` finds it.

A note on scope. In aether-ui a `g()`'s own `transform(...)` is not cascaded
onto its children on the live path. sae composes its groups' transforms onto
each element (`push_transform_`), which is why the fix is all sae needs. The
tests below give the transform to the text itself, as sae does.

## A second bug it turned up (macOS)

On macOS, `/canvas/{id}/pixel` never saw any text. `canvas_read_pixel`
replayed into a bitmap without pushing an `NSGraphicsContext`, and AppKit
draws strings into the current context, not the `CGContext` it is handed, so
a probe of a glyph read the ground under it. `canvas_write_png` already
pushed one. Both now share `canvas_replay_offscreen`. GTK4, Win32 and UIKit
were not affected. That means an ink check like `spec_sheet`'s could only
have passed on Linux until now. With this commit it works on a Mac too.

## How it is tested

- `vg/test/test_vg.ae`: a deferred scene with a text transformed
  `translate(100 0)`, its shadow, and a circle with the same transform. The
  text and its shadow must dispatch past x = 100 and the circle at 130.
  Without the fix: `text ... drawn at x 12, not past 100`.
- `examples/vgpaint_demo` scene 6 and `tests/vgpaint_demo`: black "MM" made
  at x 10 and translated by 100, read back as pixels. The spec requires no
  ink left of x 100 and ink right of it. On macOS: 9/9 with both fixes. With
  the vg fix reverted, 13 dark probes on the left and none on the right.
  Before the macOS fix, no ink was found anywhere, fix or not.
- Full `./ci.sh` on the lanes: see the commit message.

## For sae

Bump `AETHER_UI_REF` in `pins` to this commit, in the same commit as
`spec_sheet` (the spec that needs it).
