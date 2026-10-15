# REPLY: drag within the app, to reorder (a spreadsheet's sheet tabs)

**RESOLVED**, 2026-10-15. Answers the spreadsheet's
`asks/aether-ui-drag-to-reorder.md` (shape A). In aether-ui `main` from the
commit that adds this file's sibling code.

## What landed

Shape A, as asked:

```aether
reorderable(stack, cb)        // cb(from: int, to: int)
```

The children of an `hstack` or `vstack` can be dragged among themselves.
Press on a child, move with the button held and it lifts (a press and
release still clicks it, so a tab still goes to its sheet); while it is
dragged a bar shows the gap it would land in; let go and `cb(from, to)`
runs with the child's index and the index it would have after the move,
both in the order before it. The toolkit does not move the children: the
page runs its command and rebuilds the row from its state, as `each`
already does, so undo and scripts stay the one path. A drop that moves
nothing (the gap on either side of the child) does not call back.

For a row built with `each`, the stack is `each_parent(e)`:

```aether
reorderable(each_parent(tabs)) callback |from: int, to: int| {
    run("sheet.move", from, to)     // then each_update(tabs, sheets)
}
```

`to` is the index the sheet has after the move, so it is `sheet.move to=`
directly.

### Each backend, for real

| Backend | Built on |
|---|---|
| GTK4 | one `GtkDragSource` (capture phase, so it sees the press before a child button's click gesture; it claims only past the drag threshold) and one `GtkDropTargetAsync` on the STACK, over the private mime type `application/x-aether-ui-reorder`; the dragged child is the drag icon; the gap is a `box-shadow` class on the child it lies before |
| AppKit | an `NSPanGestureRecognizer` on the stack with `delaysPrimaryMouseButtonEvents` (a click is delivered to the child when the recogniser fails), starting an `NSDraggingSession` with a snapshot of the child under the private pasteboard type `org.aether-lang.aether-ui.reorder`; the stack is its own `NSDraggingDestination` (`registerForDraggedTypes:`); the gap is a 3pt bar in `controlAccentColor` |
| Win32 | the ask's "mouse capture within the window": a `WH_MOUSE` hook on the UI thread sees the press on any child (a `BUTTON` captures its own mouse, so the stack never would), and past `SM_CXDRAG` takes the capture from the child (which drops its press) and gives it to the stack; the release lands the drop and is swallowed, so nothing clicks; the gap is a 3px bar in `COLOR_HIGHLIGHT`, a window of the stack's own for the drag's duration |
| UIKit | `UIDragInteraction` (enabled on a phone too) + `UIDropInteraction` on the stack; the item's index rides as a `localObject`, the drop is refused unless the drag began in this very stack; the child is the lift preview |
| Android | `View.startDragAndDrop` from a long press on a child (armed as children attach), the stack as the `OnDragListener`; label `aeui-stack`, not global; the gap as the stack's foreground |

Why the stack and not each child: `each_update` rebuilds the children after
every move, and the DSL assembles top-down (the children are not there when
the stack is armed), so a controller per child would have to be re-armed on
every rebuild. One gesture on the stack sees them all, rebuilt or not. This
is also why the existing `listbox_reorderable` gesture was GTK4-only: its
per-row arming never got an AppKit/Win32 gesture. (That one is unchanged;
a later pass could re-base it on this.)

The gap rule is the same everywhere: the gap is the number of children
whose midpoint along the stack's axis lies before the pointer (0 = before
the first, n = after the last). Every drop, native or driven, goes through
`aether_ui_stack_reorder_deliver(stack, from, gap)`; the DSL turns the gap
into `to` (`_reorder_target`) and drops the no-ops.

### Testing

- The driver's `POST /widget/{child}/drag?x=&y=&dx=&dy=` on a child of a
  reorderable stack (one with no `on_drag` of its own) plays a press at
  (x, y) in the child and a release at the offset through the stack's own
  gap rule, with the real geometry. `GET /widget/{stack}` reports
  `"reorderable": true`. A widget in no reorderable stack still answers 404.
- `ui.reorder_deliver(stack, from, gap)` is the headless entry (what a drop
  calls), for a test with no driver.
- `tests/reorderable` (headless, in ci.sh's UI_TESTS): the gap-to-index
  rule, no-ops, clamping, re-arming replaces the callback.
- `examples/reorderable_demo` + `tests/reorderable_demo/spec_reorderable_demo.ae`
  (over the driver): the row reports reorderable, A dragged past C by its
  measured x/w lands at 2 and the app's `each_update` rebuilt the row, a
  nudge beside the child moves nothing, the rebuilt children are still
  reorderable (D to the front), a tab still clicks.

The spreadsheet's `it` in `spec_sheet` (drag a tab two places right, the
sheet moved, a formula naming it still reading it, Undo taking it back) can
be written against `/widget/{tab}/drag` with the tabs' `x`/`w` from
`/widget/{id}`, as the demo spec does.

## Not done (and why)

Shape B (`drag_text` / `on_drop_text`) did not fall out of A: A is one
gesture on the container, B is a source/target pair on arbitrary widgets
with a payload crossing them. It is a separate ask if a chart dragged to
another sheet, or a range onto a panel, is wanted.

sae has no binding for `reorderable` yet (nor for `draggable`/`on_file_drop`);
that is sae's side, `each_parent` and `reorderable` are what it binds.
