# Reply — 0.626.0 font-preview segfault: found, fixed, merged

**From:** the aether/ line, 2026-09-03. **Status: fixed on `main`** (PR #1874,
merge `0fb720c4`, issue #1873). It will be in the next release after 0.626.0.

**Your pin can lift** once that release is cut — see "Verified against your app"
below, where your own click reproducer passes.

## It was our bug

Your instinct was right. You wrote that 0.624.0's struct-`string`-field
ownership change "looks closest to a crash of this shape". That is exactly what
it was — #1866, shipped in 0.624.0.

You were also right to rule out `math.lrint`. Migrating `vg/font.ae` alone and
still crashing was the correct experiment, and it saved us from chasing the
headline change.

## Root cause

`refresh_selection(app: *App)` assigns a `string` field through a
**pointer-to-struct parameter**:

```aether
refresh_selection(app: *App) {
    app.pv_d = build_preview_path(app)
}
```

#1866 made that store take ownership and release the previous value, which is
right in principle. But the emitted C reads the box's ownership tracker to
decide whether to free:

```c
{ const char* _tmp_old = app->pv_d; app->pv_d = ...;
  if (app->_heap_pv_d) aether_heap_str_free(_tmp_old); app->_heap_pv_d = 0; }
```

And `mk_app()` does:

```aether
a = malloc(80) as *App     // hand-malloc'd — NOT zeroed
```

so `_heap_pv_d` is **uninitialised garbage**. When it is non-zero, that store
calls `free()` on a garbage pointer. First click, instant segfault.

Nothing in your code was wrong. `malloc(n) as *T` is a documented pattern, and
before #1866 that store never read the tracker, so it was sound.

## The fix

The parameter case now stores and *sets* the tracker — so the destructor still
reclaims the value and #1866's leak fix is preserved — but no longer *reads* a
tracker it cannot know was initialised. A parameter is no promise that the
caller zeroed the box. `heap.new` boxes are unaffected and keep the releasing
behaviour.

## Verified against your app

Not just the reproducer — your actual `font_picker`, rebuilt against the fixed
compiler, driver enabled, fresh `AEB_CACHE_DIR`:

```
POST /widget/47/click   ->  http=200
process:                    alive
selection label:            "selected: DejaVu Sans"
```

That is the v0.613.0 behaviour your A/B table describes, now on `main`.

One thing I could **not** run here: your full `tests/spec_matrix.sh font_picker`
— my sandbox kept killing the shell on CPU grounds. So the "6 pass / 13 fail"
figure is un-retested by me. Please re-run it on the next release; I expect all
13 to clear since they all died from the process being gone, but I am not
claiming it as verified.

## On the backtrace you asked for

You asked for one because your box has no gdb. Worth knowing for next time:
**this crash does not reproduce under gdb** — the debugger changes the heap
layout enough that the tracker slot reads zero. A backtrace would have been a
dead end.

What actually located it was a 30-line reproducer plus a bisect:

| compiler | result |
| --- | --- |
| `21004a26^` (pre-#1866) | `survived: hello` ×3 |
| `main` (0.626.0) | segfault ×5 |

The padding in that reproducer is load-bearing: glibc writes free-list metadata
over the first ~16 bytes of a freed block, which zeroes the tracker in a small
struct and hides the bug entirely. Your `App` is 80 bytes with `pv_d` near the
end, which is why you hit it and our test suite did not.

## Why our CI missed it

The existing regression test for #1866 used a `heap.new` box — zero-initialised,
so the tracker was always valid and the test passed throughout. The new test
(`tests/integration/heap_field_setter_malloc_box`) uses a hand-malloc'd box with
padded fields, and I confirmed it fails with signal 11 on the unfixed compiler
before landing the fix.

## Ask

When you do move off the pin, if anything else in that matrix still fails,
send it over — a second regression hiding behind this one would not surprise me,
since every one of the 13 failures had the same cause (dead process) and none
of them could report anything else.
