#!/usr/bin/env bash
# Build + run the C-level test for the Stage 3 canvas primitives
# (canvas_cmd_count / canvas_render_range_rgba) against the GTK4 backend.
#
# Standalone by design: it compiles ONLY the backend translation unit and
# auto-stubs the unrelated aether-ui runtime symbols it references (tray,
# notifications, undo, menus -- none of which the canvas path touches), so
# proving the primitive needs no Aether toolchain or app build.
#
# GTK4 only. win32/macOS return 0 from render_range_rgba by design (Stage 3
# is gated off there), so there is nothing to assert yet.
set -u
cd "$(dirname "$0")/.."

command -v pkg-config >/dev/null || { echo "SKIP: no pkg-config"; exit 0; }
pkg-config --exists gtk4 || { echo "SKIP: gtk4 not available"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CFLAGS="$(pkg-config --cflags gtk4)"
LIBS="$(pkg-config --libs gtk4)"

gcc -c -o "$TMP/backend.o" backend/aether_ui_gtk4.c -Ibackend $CFLAGS || {
    echo "FAIL: backend did not compile"; exit 1; }

# Discover the unresolved runtime symbols and stub them as no-ops.
gcc -o "$TMP/t" tests/test_canvas_render_range.c "$TMP/backend.o" \
    -Ibackend $CFLAGS $LIBS -lm 2>"$TMP/link.err"
if [ ! -x "$TMP/t" ]; then
    grep -oE "undefined reference to \`[A-Za-z0-9_]+'" "$TMP/link.err" \
      | grep -oE "\`[A-Za-z0-9_]+'" | tr -d "\`'" | sort -u > "$TMP/missing.txt"
    {
        echo "/* auto-generated no-op stubs */"
        echo "#include <stddef.h>"
        while read -r n; do
            [ -n "$n" ] && echo "void* $n() { return NULL; }"
        done < "$TMP/missing.txt"
    } > "$TMP/stubs.c"
    gcc -c -o "$TMP/stubs.o" "$TMP/stubs.c" || { echo "FAIL: stub compile"; exit 1; }
    gcc -o "$TMP/t" tests/test_canvas_render_range.c "$TMP/backend.o" "$TMP/stubs.o" \
        -Ibackend $CFLAGS $LIBS -lm || { echo "FAIL: link"; exit 1; }
fi

RUN="$TMP/t"
if command -v xvfb-run >/dev/null 2>&1; then RUN="xvfb-run -a $TMP/t"; fi
out="$($RUN 2>&1)"
echo "$out"
echo "$out" | grep -q "^PASS$" || { echo "FAIL: assertions did not pass"; exit 1; }
exit 0
