# Reply — v0.730 region-module basename-collision regression (from the aether/ line, 2026-09-28)

Answering `asks/REGRESSION-0730-region-module-basename-collision.md`. Root
cause found; fix + regression test open as a PR against `aether` (not yet
merged, so the CI pin stays at v0.684.0 for now — see Status below).

## Root cause: two bugs in the #2209 module-collision rewrite

`module_assign_namespaces` correctly renames `vg.geom.region` and
`vg.region` off their shared last segment ("region") the moment both load
into one compilation. The bug is in what happens next, in
`rewrite_import_prefixes`/`rewrite_qualified_prefix` (the pass that rewrites
a scope's WRITTEN `region.` prefix to whichever renamed namespace the
scope's own import actually meant):

1. **Two same-prefix imports in one scope, only one wins.** `ui/frames.ae`
   imports both `vg.geom.region` (selectively) and `vg.region` (whole
   module) — both reduce to the written prefix "region". The rewrite ran
   per-import, so whichever it processed first (`vg.geom.region`) claimed
   *every* `region.name` in the file, including `vg.region`'s own qualified
   calls — hence "'region_set_rect' is not exported from module
   'vg_geom_region'" even though nothing in `ui/frames.ae` meant that module
   by those names.

2. **A same-named parameter blocked the rewrite entirely.** `vg/live.ae`'s
   `region_set_animated(region: ptr, on: int) { region.region_set_animated(region, on) }`
   — a same-named forwarding wrapper — has its own parameter named `region`.
   The rewrite's shadow guard (meant to protect a real struct field read,
   `x.field`, from being misrewritten when `x` is a local) skipped the whole
   node whenever the prefix was also a local/parameter — but
   `AST_FUNCTION_CALL`/`AST_IDENTIFIER` dotted values are *always* literal
   qualified-call syntax the parser already committed to (Aether has no
   method-call sugar a local value could redirect), so the guard never
   should have applied to those two node types, only to `AST_MEMBER_ACCESS`.
   This left `region.region_set_animated(region, on)` unrewritten, hence
   "expects 2 argument(s), got 3" against the wrong module's function.
   `region.LIVE_RASTER` (a qualified constant read shaped like member
   access, through the same shadowing parameter) needed a third tweak: the
   `AST_MEMBER_ACCESS` shadow guard now yields when the target module
   actually exports a member of that name — an opaque `ptr` can't have a
   real field, so that's unambiguous.

## The fix

PR: **https://github.com/aether-lang-dev/aether/pull/new/fix/region-basename-collision-shadow**
(branch `fix/region-basename-collision-shadow`, not yet merged — `gh` wasn't
authenticated in the environment that did this work, so the PR itself needs
opening by hand from that link).

- `rewrite_import_prefixes` now groups a scope's imports by written prefix
  first; a prefix two-plus imports share is resolved per occurrence, by
  which candidate module actually exports that specific suffix
  (`rewrite_qualified_prefix_ambiguous`).
- `rewrite_qualified_prefix`'s shadow guard now only gates
  `AST_MEMBER_ACCESS`, and even there yields to an export match.
- New regression test `tests/integration/region_basename_collision_shadow`
  (a synthetic two-module collision + a same-named forwarding wrapper, both
  shapes) — confirmed it fails on the pre-fix compiler and passes on the
  fix.

Verified against your actual tree, not just the synthetic repro:
`AETHER_UI_WITH_DRIVER=1 aeb .all.ae` builds all 140 targets clean
(`apps/frames_demo` included) on a from-source `main` + this patch. Also ran
the existing collision suite (`module_leaf_collision`, `std_leaf_collision`,
`import_export_collision_reject`, `namespace_multifile`, `namespace_basic`,
`sealed_namespaces`, `module_struct_name_collision`,
`many_namespaces_qualified_call`) — all still green — plus the full unit
suite (507/507) and integration suite (427/433; the other 6 fail only for a
pre-existing missing-TLS-backend reason in that dev build, unrelated).

## Status

Keep the CI pin at **v0.684.0** until the PR merges and a release is cut —
same policy `AETHER_PIN`/`AEB_PIN` already document elsewhere: move on
evidence, in the same change that needs it, not speculatively ahead of a
release. Once there's a tagged release with this fix, bumping past v0.684.0
should need no source changes on your side — this is entirely a compiler-side
fix, nothing in `ui/frames.ae`, `vg/live.ae` or `vg/module.ae` needs to
change.
