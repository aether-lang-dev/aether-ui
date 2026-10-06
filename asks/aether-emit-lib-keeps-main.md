# Ask (aether): `--emit=lib` should keep a program's `main()` callable

From: aether-ui, Android backend stage 1 (docs/design/android-backend.md),
2026-10-06, against aether `main` at 0.785.0.

## Motivation

An Android app is a shared library. `AetherActivity.onCreate` calls
`System.loadLibrary("app")` and then has to run the program's entry point,
its `main()`, which builds the widget tree and returns into Android's loop.
`ae build --target=aarch64-linux-android --emit=lib` is the right build
(it works, including the bionic cross-link and `-fPIC` runtime), but it
**drops `main()` entirely**:

```c
// compiler/codegen/codegen.c, generate_main_function
// --emit=lib only: suppress the C `int main(int,char**)` entry point.
// If the .ae file defined main(), its body is currently dropped in
// lib-only mode ... A future Shape B extension could emit an
// `aether_main()` wrapper so hosts can invoke the script's entry point
// explicitly.
if (!gen->emit_exe) return;
```

So an unmodified app (`examples/counter/counter.ae`) builds to a library
with nothing to call.

## Repro

```sh
cat > hello.ae <<'EOF'
extern println(s: string)
main() { println("hi") }
EOF
AETHER_SYSROOT=<bases/aarch64-android29> \
  ae build hello.ae --target=aarch64-linux-android --emit=lib -o libhello.so
nm -D libhello.so      # no main, no aether_main: the body is gone
```

(The same on a host `--emit=lib`; Android is just where it matters first.)

## The local workaround (in aether-ui now)

`tools/android-apk.sh` builds from a generated copy of the source with
`^main()` renamed to `aeui_app_main()`. A renamed top-level function gets the
usual `--emit=lib` alias, `aether_aeui_app_main`, which the backend calls
(weak reference in `backend/aether_ui_android.c`). It works, but it is a sed
over the user's source, it breaks for any app whose `main` is not spelled
`main()` at column 0, and it means the program's entry point runs without
whatever `main()` prologue an executable gets (args init etc.; harmless for
the UI apps tried so far, since the scheduler starts on first spawn).

## What would fix it

The "Shape B" wrapper the comment already names: under `--emit=lib`, emit a
program's `main()` body as an exported function (`aether_main`, or behind a
flag such as `--emit=lib --keep-main[=name]`), with the same runtime prologue
an executable's `main` runs. aether-ui would then drop the rename and call
`aether_main()` directly.

## Also noticed (not blocking)

`--emit=lib` is capability-empty by default, so `import std.fs`/`std.os`
(which `ui/module.ae` imports) are rejected without `--with=fs,net,os`. That
is by design and the flag is the right answer for an app that IS the host;
noted only because an app-as-library is a new kind of `--emit=lib` user.
