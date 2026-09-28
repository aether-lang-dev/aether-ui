# aether v0.730.0 regression — bare whole-module import resolves to the wrong same-basename module (blocks moving off v0.684.0)

**From:** the aether-ui line, 2026-09-28. **Status:** our CI pin stays at
**v0.684.0** (`.github/workflows/ci.yml`'s `AETHER_REF`); we cannot move to
0.735.0 (current `main`) until this is understood — it breaks `apps/frames_demo`,
which is under active `spec_frames_demo` / `test_stage25_clip_surface.sh`
coverage in `ci.sh`.

## The A/B

Built `aeb .all.ae` fresh (`AETHER_UI_WITH_DRIVER=1`) against a from-source
install of aether `main` (VERSION 0.735.0, commit `2b7f063c`). 139 of 140
targets build clean. The one failure:

```
error[E0200]: Function 'region_set_animated' expects 2 argument(s), got 3
  --> vg/live.ae:266:11
error[E0301]: Undefined function 'region.region_draw_fn'
  --> vg/module.ae:1262:16
error[E0303]: 'region_set_rect' is not exported from module 'vg_geom_region'
  --> ui/frames.ae:453:19
... (24 errors total, all the same two shapes, all in vg/live.ae, vg/module.ae, ui/frames.ae)
```

Every one of these call sites uses the bare `region.` prefix that
`import vg.region` (whole-module, no parens) is supposed to bind. Instead the
compiler is resolving `region.` to `vg.geom.region` — a *different* local
module (rectangle set algebra) that `ui/frames.ae` also imports, selectively:

```aether
import vg.geom.region (
    rgn_new, rgn_add_rect, rgn_sub_rect, rgn_nrects, rgn_rect, rgn_area
)
...
// The live-region module. Note vg.geom.region above is a DIFFERENT module
// (rectangle set algebra) imported selectively, so the bare `region.` name
// is free for this one -- the same form vg/live.ae and vg/module.ae use.
import vg.region
```

That comment's assumption — a *selective* import (`import m (names)`) never
binds the bare module-name prefix, so it can't collide with a later
*whole-module* import of a same-basename sibling — held all the way through
v0.684.0. It stopped holding somewhere in the v0.730.0–v0.734.0 basename-
collision rework (CHANGELOG: "Two modules whose paths end in the same segment
no longer collide" / "A shipped module now keeps its last segment and only
the colliding local module moves").

## Minimal reproducer (no aether-ui needed)

```aether
// vg/geom/region.ae
exports (rgn_new)
rgn_new() -> int { return 1 }
```
```aether
// vg/region.ae
exports (region_kind)
region_kind(x: int) -> int { return x + 1 }
```
```aether
// main.ae
import vg.geom.region (rgn_new)
import vg.region

main() {
    a = rgn_new()
    b = region.region_kind(a)
    return b
}
```

```
$ ae build main.ae -o main --lib .
error[E0303]: 'region_kind' is not exported from module 'vg_geom_region'
  --> main.ae:6:15
6 |     b = region.region_kind(a)
  |               ^ help: add 'export' to the declaration in the module, or use an exported alternative
```

`vg.geom.region` was imported **selectively** and never asked for a bare-name
binding at all; `vg.region` was imported as a **whole module** specifically to
get one. The collision logic appears to register both modules under the
shared last-segment key ("region") the moment they're both loaded into one
compilation, then hands the bare `region.` prefix to the wrong one — the
selectively-imported module, not the whole-module-imported one whose only
reason for existing unqualified is that import.

Confirmed this isn't about local-vs-local tie-breaking order: renaming
`vg/region.ae`'s file (not its module path) doesn't change the outcome, and
neither module is a std/shipped module, so the v0.734.0 "shipped module keeps
its short name" rule shouldn't be in play for either side.

## What would unblock us

Either:
1. A selective import (`import m (names)`) genuinely never contests the bare
   basename namespace, regardless of what else in the compilation imports a
   same-basename module as a whole module — restoring the invariant our
   comment already documented, or
2. If the new collision machinery intentionally now treats every same-basename
   module as contesting the bare name irrespective of import form, tell us the
   supported way to keep two same-basename local modules unambiguous (e.g. does
   `import vg.region as live_region` sidestep it today?) — we're happy to add
   an explicit alias on our end if that's the sanctioned fix, we just don't
   want to guess at a workaround that happens to compile today and silently
   breaks again on the next collision-detection refinement.

Happy to test a candidate fix against our tree — `aeb .all.ae` from a clean
checkout reproduces the full failure set in under 30s.
