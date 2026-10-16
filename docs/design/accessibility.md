# Accessibility (semantics layer)

Native widgets carry platform accessibility for free: a GtkButton is a
`button` role with its label as the accessible name; the same holds for
NSButton (macOS) and a Win32 BUTTON control (MSAA). The gap this layer
closes is twofold:

1. **Authoring** — override or supply role/name/description where the auto
   value is wrong or missing: an icon-only button (no text → no name), a
   plain container acting as a group/listitem, a canvas/vg-drawn control
   that the platform sees as an opaque rectangle.
2. **Verification** — a driver route that reports each widget's *actual*
   accessible role/name/description, so specs assert accessibility rather
   than assume it.

## DSL

Three setters on any widget handle (idempotent, last-write-wins):

```
a11y_role(w, "button")          // ARIA-ish role name (see roles below)
a11y_label(w, "Delete file")    // accessible NAME
a11y_description(w, "Removes the selected file permanently")
```

Roles are a small cross-platform vocabulary, mapped per backend:
`button, checkbox, radio, link, heading, image, group, list, listitem,
tab, tablist, menu, menuitem, dialog, alert, textbox, slider, progressbar,
none`. `none` hides a decorative widget from the a11y tree.

The listbox rows (plain hstacks today) are auto-tagged `listitem` with
their first text cell as the name, and the group `list` — so the built-in
widgets are accessible without the app author doing anything.

## Backends

All three set REAL platform accessibility; the driver reads back what the
platform actually exposes where the API allows it.

- **GTK4** — `gtk_accessible_update_property(GTK_ACCESSIBLE_PROPERTY_LABEL/
  DESCRIPTION)` for name/description; role is set at widget-construction
  where GTK allows it and otherwise reported from
  `gtk_accessible_get_accessible_role`. Readback for the driver uses
  `gtk_accessible_get_accessible_role` (role) and
  `gtk_test_accessible_check_property(…, GTK_ACCESSIBLE_PROPERTY_LABEL)`
  (name) — the in-process AT view, so the driver reports the tree a real
  screen reader would see, not just our side-store.

- **Win32** — MSAA via a per-widget side-store surfaced through
  `WM_GETOBJECT`. For our custom-drawn/container widgets we answer
  `WM_GETOBJECT` with an `IAccessible` whose `get_accRole`/`get_accName`/
  `get_accDescription` read the side-store; standard controls keep their
  system `IAccessible` and we override name/description via the store when
  set. (UI Automation bridges over MSAA, so an MSAA provider is visible to
  Narrator.) The driver reports the side-store values (what we hand the AT).

- **macOS** — `NSAccessibility` informal protocol on the widget's NSView:
  `accessibilityRole`, `accessibilityLabel`, `accessibilityHelp` read the
  side-store when set, else fall through to `super`. The driver reports the
  effective values via the same accessors.

## Announcements

`a11y_announce(w, text)` has a screen reader say `text` now, from `w`,
without moving focus: a move made on a drawn board, the square under its
keyboard cursor. Per backend: GTK4 `gtk_accessible_announce` (GTK 4.14;
before it, the widget's accessible description is set to the text, a
property change AT-SPI tells the reader); AppKit
`NSAccessibilityAnnouncementRequestedNotification` from the widget's window;
UIKit `UIAccessibilityAnnouncementNotification`; Android
`View.announceForAccessibility`; Win32 a UI Automation notification event
(`UiaRaiseNotificationEvent`, Windows 10 1709; before it, the MSAA
description and `EVENT_OBJECT_DESCRIPTIONCHANGE`). Each records the text in
the shared side-store `aether_ui_a11y_announce.h`, and
`GET /widget/{id}/a11y` reports `"announced"` (the last) and
`"announcements"` (how many). `examples/a11y_demo` and its spec cover it,
run on all five (macOS, Linux GTK 4.6 and 4.22, Windows 11, the iOS
simulator, the Android emulator).

## Grids (data-grid.md phase 5)

`a11y_grid(w, rows, cols)`, `a11y_grid_row(w, row)`,
`a11y_grid_cell(w, row, col, header)` and `a11y_grid_header(w, col)` give a
grid's parts their roles (grid, row, gridcell, columnheader), places and
column headers; rows and columns count from 0. `a11y_next_role(role)` gives
the next widget made its role at construction, the only way GTK4 takes one.
Per backend: GTK4 accessible relations; AppKit index ranges, counts and
column header elements; UIKit UIAccessibilityContainerDataTable; Android
CollectionInfo/CollectionItemInfo; Win32 a UI Automation provider with the
Grid, Table, GridItem and TableItem patterns. `/widgets` reports `a11y_rows`
/ `a11y_cols` (a grid), `a11y_row` (a row) and `a11y_row` / `a11y_col` /
`a11y_header` (a cell). `datagrid` uses them.

## Driver surface (both servers)

- `GET /widget/{id}/a11y` → `{"role":"button","name":"Delete","description":"","announced":"","announcements":0}`
- `/widgets` entries gain `"role"`/`"a11y_name"` fields (empty when unset,
  so existing specs are unaffected).

A widget with nothing set reports its AUTO role/name (proving the "native
gets it free" claim); a widget with `a11y_*` set reports the override.

## Verification

- GTK4: fully driver-verifiable in-process (the check_property/role readback
  is the same view an AT sees). Spec asserts both auto and overridden values.
- Win32/macOS: the driver confirms the values we hand the platform AT.
  End-to-end confirmation with a real screen reader (Narrator/VoiceOver) is
  a manual step outside the headless harness — noted, not automated.
