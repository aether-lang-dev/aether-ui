# A program's top-level `var` leaks into an imported module's function bodies

**From:** aether-ui (the 0.801.0 floor move, 2026-10-10). **Aether:** 0.801.0
(release) and main at 5db83000; 0.791.0 is fine. **Status:** worked around
in aether-ui (two tests renamed a global); needs a compiler fix.

## What happens

A program that declares a top-level `var` whose name an imported std
module also uses as a plain local (`n = ...`, `m = ...` with no
declaration) fails to type-check *inside the std module*: the module's
local resolves to the program's global.

```aether
import std.fs

var n = 0

main() {
    n = n + 1
    println("n=${n}")
}
```

```
error[E0200]: narrowing assignment to 'n': its type was inferred as 32-bit int
from its initializer, but a 64-bit value is assigned here and would truncate.
  --> .../share/aether/std/fs/module.ae:1134:5
1134 |     n = fs_pwrite_raw(file, data, data.len, offset)
```

`var m = 0` does the same at `std/fs/module.ae:514` (`m = file_mtime_raw(path)`).
Without `import std.fs` the program runs (`n=1`); on 0.791.0 the program
above runs too. Here the name clash makes the type checker complain, which
is the lucky case. Where the types agree it compiles and the module's
function silently writes the program's global:

```aether
import std.fs

var ok = 42

main() {
    _e = fs.delete("/nonexistent/zzz")   // std.fs's delete: `ok = file_delete_raw(path)`
    println("ok=${ok}")                  // 0.801.0 prints ok=0
}
```

## Where it bit

aether-ui's `tests/undo_stack` and `tests/undo_group` (each `var n = 0`,
and `ui` imports `std.fs`): the whole `aeb .all.ae` fan-out failed on
0.801.0, so `./ci.sh` skipped every runtime phase.

## Workaround

The two tests' global is `undo_total_` now, with a comment pointing here.
Any program with a short top-level `var` name that std happens to use as a
local is exposed.

## Ask

Resolve a module function's undeclared assignment target in that module's
own scope (its locals, then its own globals), never the importing
program's globals. A regression test: the program above, plus one where
the types agree and the global must keep its value after a call into the
module.
