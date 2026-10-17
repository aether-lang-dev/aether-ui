# vg/chart — Chartist, ported to Aether

`vg/chart` is a port of [Chartist](https://github.com/chartist-js/chartist)
v2.0.0 (commit `cf181279cf60a9c89cc0b19bb05e82b48f9ebcdf`, MIT; see
[LICENSE-chartist](LICENSE-chartist)). It was ported once and is maintained
in Aether from here on: Chartist is the reference it came from, not a
dependency.

It keeps Chartist's algorithms and the edge cases its issue tracker taught
it (holes, multi-value points, empty and flat ranges, the step search, the
full-circle pie slice, stacked negatives, label skipping), but not its
imperative DOM style. A chart is a function of data and options that returns
a declarative node tree, with Chartist's class names on it; `render` draws
that tree with vg.

```aether
import vg.live (vg)
import vg (view_box)
import vg.chart
import vg.chart.render

main() {
    c = chart.line_chart("{labels: ['Mon', 'Tue', 'Wed'], series: [[5, 2, 4]]}",
                         "{low: 0, showArea: true}")
    tree = chart.built_root(chart.build(c, 600.0, 300.0))
    render.theme(tree)                         // Chartist's default stylesheet
    window("chart", 620, 320) {
        vg(view_box(0, 0, 600, 300), 600, 300) { render.draw(tree) }
    }
}
```

`examples/vg_chart_demo` draws a smoothed line, a stacked bar and a donut;
`tests/vg_chart_demo` reads them back over the driver.

## API

### Data and options: `vg.chart.value`

Chartist's input is loose JavaScript, and its normalisation of that input is
much of what it gets right, so the port keeps the JavaScript value model.
Data and options are `*JsValue` trees (undefined, null, booleans, numbers,
strings, arrays, objects in insertion order). `value.parse(text)` reads a
JavaScript literal: JSON plus `undefined`, `NaN`, `Infinity`, single quotes,
unquoted keys, trailing commas and comments, so Chartist's own examples and
test data are written as they are upstream, and a page can pass what
`JSON.stringify` gives it. The module also has `Number()` / `String()`
(`to_number`, `js_string`), `JSON.stringify` (`stringify`), the JS
shortest round-trip number formatting (`js_number`), deep `equals` and
Chartist's deep `extend`.

### Charts: `vg.chart`

| Call | |
|---|---|
| `line_chart(data, options)`, `bar_chart(…)`, `pie_chart(…)` | A chart from two JS literals (`""` for no options). The `_v` forms take `*JsValue`s. |
| `build(chart, width, height) -> *CtBuilt` | Runs the pipeline at that size. A numeric `width`/`height` option wins. |
| `built_root(b)` | The tree: `svg.ct-chart-line > g.ct-grids, g > g.ct-series.ct-series-a > path.ct-line…, g.ct-labels`. |
| `built_area(b)`, `built_axis_x(b)`, `built_axis_y(b)`, `built_options(b)`, `built_error(b)` | The chart area, the axes (for projecting values), the options it was built with, and what Chartist would have thrown. |
| `responsive(chart, query, options)` | Overrides that apply while `query` matches the chart's width: `(min-width: Npx)`, `(max-width: Npx)`, `(width < N)` and friends, `screen`, joined by `and`. Matching entries extend the options in order. |
| `set_options(chart, options, replace)` | Extend the current options, or (`replace = 1`) start again from the defaults. |
| `label_x(chart) callback \|v: ptr, i: int\| { … }`, `label_y`, `label_pie` | `labelInterpolationFnc` / `labelTransform`: return a `*JsValue` label; null, undefined or false (but not 0 or "") skips the tick's grid line and label. |
| `line_smooth(chart, interp)`, `series_line_smooth(chart, name, interp)` | An interpolation value, custom ones included. The `lineSmooth` option also takes `true`, `false` or `{type: 'cardinal', tension: 0.2, fillHoles: true}` (also `simple` with `divisor`, `step` with `postpone`, `none`, `monotoneCubic`). |
| `on_compose(chart) callback \|n: *CtNode\| { … }` | Chartist's compose phase: every node with a role, bottom-up; return it (changed in place or not), a replacement, or null to drop it. |
| `on_scene(chart) callback \|root: *CtNode\| { … }` | The whole tree, last. |
| `measure_style(chart) callback \|prop: string, classes: string\| { … }` | What the stylesheet says (a donut slice's `stroke-width`), for layout. |

All of Chartist's 1.x options apply (`axisX`/`axisY` with `offset`,
`position`, `labelOffset`, `showLabel`, `showGrid`, `scaleMinSpace`,
`onlyInteger`, `type: 'AutoScaleAxis' | 'FixedScaleAxis' | 'StepAxis'`,
`ticks`, `divisor`, `low`/`high`, `referenceValue`; `showLine`, `showPoint`,
`showArea`, `areaBase`, `fullWidth`, `reverseData`, `showGridBackground`,
`chartPadding`, per-series `series: {name: {...}}`; bars' `stackBars`,
`stackMode`, `horizontalBars`, `distributeSeries`, `seriesBarDistance`;
pies' `startAngle`, `total`, `donut`, `donutWidth`, `showLabel`,
`labelOffset`, `labelPosition`, `labelDirection`, `ignoreEmptyValues`,
`preventOverlappingLabelOffset`; `classNames`).

### The tree: `vg.chart.node`

A `*CtNode` has an element `name`, ordered `attrs`, `classes`, `children`,
optional text, and what it means: `role` (series, line, point, area, bar,
slice, label, grid, gridBackground), `key` (`layer/series/value`) and `ctx`
(seriesIndex, valueIndex, value, meta, x, y, …). Line, area and slice nodes
carry their `CtSvgPath`, grid and axis-label nodes their axis. Nodes are
mutable: `attr`, `add_class`, `remove_class`, `append`, `prepend`,
`remove_child`, `replace_child`, `set_text`; and `find_all(root, role,
seriesIndex, valueIndex)`, `map_nodes`, `walk`, `assign_keys`, and
`query_all(root, ".ct-series-a .ct-point")` (tags and classes, descendant
combinators).

### Drawing: `vg.chart.render`

`theme(tree)` writes Chartist's default theme (the 15 series colours, line,
point, bar and donut widths, area opacity, grid and label colours) as
presentation attributes, leaving any a hook set. `draw(tree)` inside a vg
block emits `g`, `line`, `path`, `rect`, `circle` and `text`, with the node's
classes on every element (`css_class`), so `GET /canvas/{id}/nodes` reads
them back. `draw_with(tree) callback |el: ptr, n: *CtNode| { … }` hands each
drawn vg node and its chart node over: that is where `vg.animate`,
`tween_fill` / `tween_opacity`, `on_hover`, `on_click`, `tooltip` and
`bind_*` attach.

### The pieces underneath

`vg.chart.core` (math, normalisation, high/low, bounds, segments,
serialize), `vg.chart.svgpath` (SvgPath), `vg.chart.interpolation` (the
five interpolations and reversePath) and `vg.chart.axis` (the three axes,
the chart rect, grid lines, label boxes and SVG text labels) are public
too; the tests drive them directly.

## What was ported, from which Chartist files

| vg/chart | Chartist (packages/chartist/src) |
|---|---|
| `value.ae` | `utils/utils.ts`, `utils/extend.ts`, and the JS semantics the data code relies on |
| `core.ae` | `core/math.ts`, `core/lang.ts`, `core/constants.ts`, `core/data/{data,normalize,highLow,range,bounds,segments,serialize}.ts` |
| `svgpath.ae` | `svg/SvgPath.ts`, `svg/types.ts` |
| `interpolation.ae` | `interpolation/{none,simple,step,cardinal,monotoneCubic}.ts`, `layers/path.ts` (interpolationOf, reversePath) |
| `axis.ae` | `axes/{Axis,StepAxis,AutoScaleAxis,FixedScaleAxis}.ts`, `scales/{factories,axis,grid,svgLabels}.ts`, `render/measure.ts` (estimateText, textAscent) |
| `node.ae` | `scene/node.ts`, `scene/tree.ts` |
| `module.ae` | `presets/{line,bar,pie}{Chart,Defaults}.ts`, `presets/preset.ts`, `layers/{line,bar,pie,label,path}.ts`, `pipeline/phases.ts`, `responsive/responsive.ts` (applyResponsive) |
| `render.ae` | `styles/_settings.scss`, `styles/index.scss` |

## What was replaced, and why

- **The DOM `Svg` class** became the declarative node tree. Nothing is created
  until `render` draws, so a chart can be built, inspected, changed and
  tested headless.
- **Responsive options** are matched against the chart's own width when it is
  built, not a window's media queries: a chart in a vg scene has a size, and
  no window to ask.
- **The event / `draw` pipeline and plugins** became hooks: `on_compose` (per
  data node, bottom-up, return to keep, replace or drop) and `on_scene`, as
  Chartist v2's compose and scene phases; and `render.draw_with` for the drawn
  elements. That is how a page or a plugin customises a chart.
- **Animation** (Chartist's SMIL `animate`) is vg's: `draw_with` hands over
  the elements for `vg.animate`, `tween_fill`, `tween_opacity`.
- **The stylesheet** became `render.theme`: vg's CSS has no descendant
  selectors, and Chartist's colours hang on `.ct-series-a .ct-line`. The
  classes are still on every element, so single-class vg `css()` rules apply
  to what the theme leaves unpainted.
- **HTML labels** in `foreignObject` became SVG text, laid out the way
  Chartist's own SVG label mode lays them out (wrapped, aligned in the label
  box as the 1.x CSS aligns HTML ones, text measured by Chartist's estimate:
  0.6 em a glyph at 12 px), the same on every backend.
- **Label functions and custom interpolations** are Aether closures on the
  chart, since a JS literal cannot hold a function.

## Deferred

- Plugins: tooltips (a vg `tooltip` on drawn elements stands in), axis titles,
  legends, target lines, bands and trend lines (Chartist's v2 extra layers).
- SVG export (`renderToString`).
- Chartist v2's composition API: `chart()` with `cartesianAxes()` lists of
  axes, several axes per side, layers bound to axes and series, a bar
  layer's `offset`.
- The live renderer (`chartist/live`: keyed diffing and keyframe animation
  between updates).
- Container queries in responsive options (they would match the chart's
  width just as media queries do).
- Dashed grid lines: vg has no `stroke-dasharray`, so grids are drawn solid.
- Text with `<` or `&`: vg's live text path rebuilds an SVG string without
  escaping, so such a label does not draw correctly yet (a vg fix, not a
  chart one).

## Tests

`vg/chart/test/` holds Chartist's own unit tests, ported with their expected
values, each area ending with a negative control (an assertion that must
come out false, so a comparison that cannot tell is noticed). They run in
`ci.sh` Phase 0, headless.

| Test | From Chartist | Upstream cases ported |
|---|---|---|
| `test_chart_core` (119 assertions) | `core/math`, `core/lang`, `core/data/{bound,highLow,range,normalize,segments,serialize}`, `utils/extend` | 52 of 56 |
| `test_chart_path` (135) | `svg/SvgPath`, `interpolation/{monotoneCubic,simple,step}`, `layers/path` | 46 of 46 |
| `test_chart_charts` (497) | `charts/{LineChart,BarChart,PieChart}` (68 of 68), `layers/{line,bar,pie,label}` (43 of 55), `axes/*` (7 of 7), `scales/{grid,scales,svgLabels}` (14 of 23), `presets/{lineChart,barChart,pieChart}` (22 of 30), `core/creation` (8 of 11), `responsive/responsive` (1 of 7) | 163 of 201 |
| `test_chart_render` (73) | `scene/{node,tree}`, plus the hooks, the theme and the vg drawing | 10 of 13 |

Not ported, and why:

- `normalize` and `serialize` of `Date` values: a JS literal has no Date
  (a page passes the epoch milliseconds, which is what Chartist turns a Date
  into).
- `extend`'s cross-realm array and prototype-pollution cases: JS-only hazards
  with no counterpart in a value tree.
- `scene/node` immutability (builder methods returning copies, shared
  fields): these nodes are mutable by design.
- `layers/*` "needs both axes", "draws in polar coordinates only", the bar
  layer's `offset`, its reported extents and the label precedence between
  `labelTransform` and `labelInterpolationFnc`: the v2 composition API and
  two label-function names, neither of which the port has.
- `scales/grid`'s comparisons with the 1.x `createGridAndLabels`, and
  `scales/scales`' with the 1.x axis classes and `legacyAxis`: differential
  tests between two Chartist implementations; the port has one.
- `presets/lineChart`'s extra layers (bands, target lines), its handle's
  `on`/`off`/plugin state and destroy-time listener counts, and
  `responsive/responsive`'s DOM probes and media listeners: the DOM, events
  and plugins, replaced (above). The option behaviour they cover
  (`setOptions`, responsive overrides staying on top, #1472) is ported.
- `scales/composition`, `chart/*`, `pipeline/*`, `render/*`, `live/*`,
  `compat/*`, `svg/Svg`, `svg/animation`, `core/creation`'s `createSvg`:
  the DOM, the 1.x compatibility layer, plugins, events, the live renderer
  and SMIL animation, all replaced (above).

Three upstream specs are stronger here than upstream: LineChart's "holes
everywhere" cases register their `draw` listener after the chart has drawn,
so upstream their expectations never run (here they do, against the line's
path), and BarChart's empty-data cases assert `querySelector(...)`
`.toBeDefined()`, which null passes (here they count).
