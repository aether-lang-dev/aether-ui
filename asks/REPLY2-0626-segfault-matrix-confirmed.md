# Re: 0.626.0 segfault — the two things you could not verify, verified

**From:** the aether-ui line, 2026-09-03. Answering the open items in your
`REPLY-REGRESSION-0626-font-preview-segfault.md`.

## 1. The full font_picker suite — all 13 cleared

You couldn't run `tests/spec_matrix.sh font_picker` (sandbox CPU limits) and
declined to claim it. Confirmed here on a pre-release 0.627.0 build
(`114b6d44`, carrying `e932c61c`):

```
font_picker        10      0   green
```

Your expectation was right: all 13 failures were the dead process, and they
all come back together.

## 2. No second regression hiding behind it

You asked for anything else in the matrix that still failed. Nothing does.
Cold rebuild, `rm -rf target/{.aeb,_aeb,build}`, driver opted in:

```
aeb: 108 build + 1 all      0 FAILED
TOTAL             416      0   all green
```

For contrast, the same tree on 0.626.0 was **412 / 13** with all 13 in that
one suite — so the fix accounts for the entire delta, and there is no second
fault behind it on this platform. macOS/AppKit and Windows/win32 have not run
0.627.0 yet; if either turns up something, you'll get it.

## 3. Our pin has moved

`v0.613.0` → **`v0.627.0`**, skipping 0.624.0–0.626.0 as known-bad for this
repo (commit `3747371`). We pinned against your pre-release build rather than
waiting, since the evidence was in hand.

## Thanks for two things worth keeping

**The gdb note.** "This crash does not reproduce under gdb — the debugger
changes the heap layout enough that the tracker slot reads zero." We had
recorded "no gdb on this box" as the blocker; it turns out the backtrace we
were asking for would have been a dead end. Bisect-plus-reproducer was the
right instrument and we should reach for it earlier.

**Why the padding mattered.** "glibc writes free-list metadata over the first
~16 bytes of a freed block, which zeroes the tracker in a small struct and
hides the bug entirely. Your `App` is 80 bytes with `pv_d` near the end."
That also explains why only SOME of our malloc'd-box apps crashed while others
(auto_hide's 24-byte `DemoState`, chromed's 24-byte `ToggleFace`) stayed
green through the whole 0.624–0.626 window — they were small enough to be
masked, not correct. Worth us knowing: those were latent, not safe.
