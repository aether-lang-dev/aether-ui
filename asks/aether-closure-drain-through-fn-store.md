# A capturing closure stored through a `fn` parameter is freed after the call

**From:** aether-ui (wave1/raster, 2026-10-08). **Aether:** 0.790.0 (dev
tree) and 0.791.0 (release) both do it. **Status:** worked around in
aether-ui; needs a compiler fix.

## What happens

```aether
struct Holder { cb: ptr }
set_cb(p: ptr, h: fn) { e = p as *Holder; e.cb = h }      // fn -> ptr boxes h
...
set_cb(holder) callback |x: float, y: float| { use(captured) }
```

The call site is emitted as a *transient* closure argument:

```c
{ _AeClosure _ad_132 = <closure>; vg_on_click(_aether_ctx_get(), _ad_132);
  if (_ad_132.env) _closure_env_13_free((void*)_ad_132.env); }
```

so the env (holding `captured`) is freed the moment the setter returns, and
the handler, invoked later from a click, reads freed memory. glibc reuses
the chunk at once (the captured ptr reads as `0x400000010`, a segfault in
the handler); macOS's allocator left it intact, so every aether-ui spec on
the Mac was green against the bug. Seen in `vg.on_click` (element.ae's
`element_on_click` stores `e.click_handler = h`), `scene_set_arm_hook` and
`scene_set_present_hook` (vg/module.ae), i.e. every live vg scene, not just
the image element that found it.

## Why

`transient_closure_arg` drains when `callee_param_escapes_via_body` proves
the param does not escape. The walk's store sink wants the RHS to *directly
carry* the param (`e.cb = h`), but after the `fn -> ptr` coercion the RHS is
a boxing CALL, and "a nested CALL does NOT directly carry" — the call-rule
then asks whether that callee retains its argument, finds no body for the
coercion, and answers no. A store through the coercion is therefore
invisible, and the proof is wrong in the direction the design says it must
never be (false non-escape = UAF). The verdict also varies with unrelated
details of the closure body (interpolating the closure's own parameters
flipped it on 0.790; 0.791 drains the plain form too).

## Repro

`examples/vg_image_demo` at aether-ui `a44c6667` on Linux (gcc, glibc):
`AETHER_UI_WITH_DRIVER=1 ./build.sh examples/vg_image_demo/vg_image_demo.ae
vg_image_demo`, run under xvfb, `POST /canvas/1/click?x=43&y=42` → segfault
in the handler, backtrace through `events_invoke_click`. Or compile anything
that calls `vg.on_click(...) callback |x, y| { <captures> }` and grep the C
for `_ad_.*closure_env` beside `vg_on_click(`.

## Ask

Treat a `fn` parameter that is stored into a struct field (through the
coercion or not), a list, a map, or any ptr-typed sink as escaping; or have
the boxing coercion count as a retaining call in the walk. Until then every
setter of ours aliases first (`kept = h; e.cb = kept`), which the
declaration sink does see, with a comment pointing here.
