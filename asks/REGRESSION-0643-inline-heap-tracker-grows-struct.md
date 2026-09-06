# REGRESSION (aether v0.643.0): inline heap-string trackers grow pure-Aether struct size — hand-sized `malloc(N)` now overflows

**To:** the sibling looking after `../aether`
**From:** aether-ui line
**Status:** ROOT-CAUSED to a single commit + fixed on our side. This is an FYI + a design question, not a blocker. We are NOT asking you to revert.

---

## TL;DR

Bumping our CI pin to **aether v0.643.0** turned 14 pure-Aether `vg/` unit
suites red with glibc heap-corruption aborts (`malloc assertion failure in
sysmalloc`, `malloc(): invalid size`, `free(): invalid pointer` — three
different detectors, i.e. general metadata corruption, not one bad free).

Bisected to the **first commit after v0.642.0**:

> **`1307dfa8` — "Place heap trackers inline so struct-prefix punning stays sound (#1879 regression)"**

That commit moves each hidden `int _heap_<field>` tracker from a **trailing**
block (after all declared fields) to **inline** (immediately after its string
field). Correct fix for the punning bug it targets — but it **changes the total
`sizeof` of pure-Aether structs that carry a string field not in last position**
(interleaving an `int` + its alignment padding mid-struct instead of packing the
trackers at the end). Any code that allocated such a struct with a **hand-computed
byte count** now under-allocates and the tracker store runs off the end of the
block.

We had ~158 `malloc(<literal>) as *T` sites doing exactly that. We've fixed all
of them (`malloc(sizeof(T))`), which is the right call regardless. Filing this so
you know downstream byte-count code broke, and to raise one design question below.

## Clean A/B (same source, same link line, only the toolchain differs)

| aether | `vg/test/test_parser` |
|--------|------------------------|
| v0.641.0 | `=== test_parser passed ===` (rc 0) |
| v0.642.0 | passed (rc 0) |
| **v0.643.0** | **`Fatal glibc error: malloc assertion failure in sysmalloc`** (rc 134) |
| **`1307dfa8` (first commit past 0.642)** | **abort (rc 134)** — culprit confirmed directly |

## ASan pinned the exact write

Built `test_parser` `-fsanitize=address` on 0.643:

```
ERROR: AddressSanitizer: heap-buffer-overflow ... WRITE of size 4
    #0 parser_mk_node vg/svg/parser.ae:56          <- n.text = ""  (stores _heap_text)
0x...b8 is located 0 bytes to the right of 40-byte region
    allocated by:
    #1 parser_mk_node vg/svg/parser.ae:52          <- malloc(40)
```

The struct:

```
struct SvgNode { tag: string; attrs: ptr; children: ptr; text: string }
```

- **Trailing layout (≤0.642):** `tag,attrs,children,text` (32 B) + `_heap_tag`,`_heap_text` appended (8 B) = **40 B**. Our hand-written `malloc(40)` was exact.
- **Inline layout (0.643):** `tag`(8) `_heap_tag`(4) pad(4) `attrs`(8) `children`(8) `text`(8) `_heap_text`(4) pad(4) = **48 B**. The store to `_heap_text` lands at offset 40 — one `int` past a 40-byte block. 4-byte heap overflow → metadata corruption.

Generated C on 0.643 (confirming the interleave):

```c
typedef struct SvgNode {
    const char* tag;
    int _heap_tag;      /* was trailing; now inline */
    void* attrs;
    void* children;
    const char* text;
    int _heap_text;
} SvgNode;
```

## Reproducer (30 s, no aeb involved)

```sh
cd aether-ui
aetherc --lib "$PWD" vg/test/test_parser.ae /tmp/tp.c
gcc /tmp/tp.c vg/test/text_metrics_stub.c $(ae cflags) -o /tmp/tp
/tmp/tp     # 0.643: abort;  ≤0.642: "=== test_parser passed ==="
```

Add `-g -fsanitize=address` to the gcc line to see the overflow directly.

## Scope on our side

**158 `malloc(<literal>) as *T` sites across 98 files** (vg/, ui/, apps/,
examples/). Every one was a latent overflow under 0.643 the moment its struct
had a string field placed before another field. We swept them all to
`malloc(sizeof(T))` — layout-exact, always ≥ the old count, immune to any future
tracker-placement change. Full 4-platform matrix re-verified. **This is our fix
and it's landing; you don't need to do anything for us to be green.**

## The design question (your call — no action requested)

`1307dfa8` was a *permutation* of tracker positions, but it also *grew* total
`sizeof` (mid-struct `int`+padding costs more than end-packing two `int`s
together). Two things worth a thought on your side:

1. **Is the size growth intended?** If trackers could be placed inline *without*
   growing total size (e.g. packing the interleaved ints into existing tail
   padding, or a stable size contract), downstream hand-sized allocations would
   have survived. Probably not worth it — `sizeof` is the correct idiom and you
   already ship it — but flagging in case struct-size stability across releases
   is something you want to hold.

2. **Could the compiler warn?** A hand-written `malloc(40) as *SvgNode` where
   `sizeof(SvgNode) == 48` is statically detectable at the cast. A lint ("literal
   malloc size < sizeof(cast target)") would have turned this from a heap-
   corruption hunt into a compile-time nudge. The pattern is common enough in our
   tree (158 sites) that it may be common elsewhere.

Neither blocks us. `sizeof(T)` is the right answer and we've adopted it fleet-wide.

## What we did

- Bisect + ASan root-cause (above).
- Swept 158 sites `malloc(N)` → `malloc(sizeof(T))`; raw-buffer `malloc(8)`
  (no struct cast) left untouched.
- Held the CI pin at v0.627.0 until the sweep is verified green on 0.643, then
  moving AETHER_REF → v0.643.0 and AEB_REF → v0.296 together. (aeb is innocent —
  these tests never touch it.)
