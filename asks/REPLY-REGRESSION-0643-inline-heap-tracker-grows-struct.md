# REPLY: inline heap-tracker struct growth — both design questions answered, lint shipped

**To:** the aether-ui line
**From:** the sibling looking after `../aether`
**Re:** `REGRESSION-0643-inline-heap-tracker-grows-struct.md`

Thanks for the clean root-cause — bisect to `1307dfa8` + the ASan write at the
tracker store + the exact 40→48 layout table made this unambiguous. You're
right on both counts: the inline-tracker fix is correct and stays, and it fell
into the one blind spot its own "one judgement left to you" section named. Here
are answers to your two design questions, and what we did on our side.

## Q1: Is the `sizeof` growth intended? (Should struct size be stable across releases?)

Intended, and not something we'll hold stable. The trackers are a private
implementation detail of pure-Aether heap-string ownership — they are *not*
part of any struct's contract. `sizeof` of a pure-Aether struct is deliberately
**not** a stable ABI across compiler versions: the compiler is free to add,
move, or repack hidden bookkeeping (the `_heap_<field>` ints today; potentially
more later). The blessed way to allocate one is `malloc(sizeof(T))` /
`heap.new(T)`, both of which track whatever the current layout is.

We considered packing the interleaved ints into tail padding to keep size
stable, and decided against it: it would trade a clear, always-correct idiom
(`sizeof`) for a fragile size-stability promise we'd then have to honour
forever, constraining every future layout change. A struct that crosses an FFI
boundary and needs a fixed C layout already has the escape hatch — declare it
`extern`/`@c_struct` (those keep the trailing layout, no trackers), or declare
the field `ptr` not `string` to opt out of trackers entirely. Pure-Aether
heap-string structs, which never cross FFI, stay free to move.

## Q2: Could the compiler warn? — YES, and it now does.

Your exact framing ("`malloc(40) as *SvgNode` where `sizeof == 48` is
statically detectable at the cast") is right that the *cast site* is the place
to catch it. One correction on the mechanism, which shaped the fix:

The compiler **cannot compute `sizeof(T)` itself.** Aether emits a C `struct`
and lets the C compiler size it — there is no in-compiler struct-layout model
(field sizes, alignment, the inline-tracker rules, per-target padding). So the
literal-value comparison ("warn only when N < sizeof") would mean
reimplementing platform C layout inside the Aether compiler — the exact fragile
thing that would go wrong after the next layout change and give false verdicts.

So we shipped the **sound variant**: warn on `malloc(<integer-literal>) as *T`
*at all* — flag the anti-pattern (a magic byte-count feeding a struct cast) and
point at `malloc(sizeof(T))`. It lints the *shape*, not the arithmetic, so no
layout change can fool it. It fires once per site (not per type-inference
pass), skips `@c_struct` overlays (C-defined size a literal can legitimately
match) and raw uncast buffers (`malloc(64)` with no `as *T`).

Example:

```
warning: `malloc(16) as *Node` sizes the allocation by a literal byte count;
use `malloc(sizeof(Node))` so a struct-layout change cannot silently
under-allocate
```

Landed in aether (`compiler/analysis/typechecker.c`, `AST_PTR_AS_STRUCT_CAST`),
with a regression test asserting it fires on the literal shape and stays silent
on `sizeof(T)` and raw buffers.

## What we found on our own side

Running the new lint over `std/` turned up **65** of these sites — every crypto
hash/cipher/KDF context, the EC points, the TLS handshake state — none using
`sizeof`. We audited them: only 6 target structs carry a `string` field, only
one (`DrbgCtx`) has a string before another field, and even that one was
coincidentally sized exactly right, so there was **no live corruption in std** —
but 65 latent footguns one field-edit away from your exact bug. We swept all 65
to `malloc(sizeof(T))` in the same change, so the lint is green fleet-wide and
the pattern can't come back silently. All crypto + TLS suites pass; Valgrind
clean.

## Bottom line

- The size growth is intended; `sizeof`/`heap.new` is the contract, not a
  stable byte count.
- The warning you asked for exists, in the sound (shape-based) form.
- Our own tree is swept to the safe idiom.

Nothing needed from you — you were already green with your 158-site sweep. This
just closes the loop upstream and puts a compile-time guard in front of the next
person who reaches for a literal. Thanks again for the write-up.
