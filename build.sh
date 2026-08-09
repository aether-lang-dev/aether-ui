#!/usr/bin/env bash
# Build an Aether UI application.
# Usage: ./build.sh <source.ae> [output_binary]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AETHERC="aetherc"

# Aether headers/libraries. Prefer what `ae cflags` reports (works for both an
# installed prefix AND a dev build tree — e.g. the MSYS2/MinGW winbaz box, where
# aether lives at C:\Users\paul\aether\build with no /usr/local prefix). Fall
# back to the canonical Linux install layout when `ae cflags` is unavailable.
AETHER_CFLAGS_INC="$(ae cflags 2>/dev/null | tr ' ' '\n' | grep -E '^-I' | tr '\n' ' ' || true)"
if [ -n "$AETHER_CFLAGS_INC" ]; then
    AETHER_INCLUDES="$AETHER_CFLAGS_INC"
else
    AETHER_INCLUDES="-I/usr/local/include/aether/runtime -I/usr/local/include/aether/runtime/actors -I/usr/local/include/aether/std -I/usr/local/include/aether/std/collections"
fi
# -L dir for -laether: take it from `ae cflags --libs`, else the Linux default.
AETHER_LIB_PATH="$(ae cflags --libs 2>/dev/null | tr ' ' '\n' | grep -E '^-L' | head -1 | sed 's/^-L//' || true)"
[ -n "$AETHER_LIB_PATH" ] || AETHER_LIB_PATH="/usr/local/lib/aether"

SOURCE="${1:?Usage: $0 <source.ae> [output_binary]}"
# Binaries always live under build/ (gitignored), never the repo root. A passed
# output name is reduced to its basename and re-anchored there, so
# `./build.sh x.ae foo` → build/foo — there's no way to drop an artifact in the
# tree root. Default is build/<source-basename>.
OUTPUT_NAME="$(basename "${2:-$(basename "$SOURCE" .ae)}")"
OUTPUT="build/${OUTPUT_NAME}"
C_FILE="${OUTPUT}.c"

# Contrib archives: `ae cflags --libs` can't emit these (ae doesn't see the
# program's import graph — upstream ask aether#1201), and aeb knows via the
# app's .build.ae — but build.sh must detect them itself. Grep the app's dir
# for contrib imports and link the matching installed archives.
CONTRIB_LIBS=""
if grep -rqs "import contrib.sqlite" "$(dirname "$SOURCE")"; then
    # The aether archive wraps the real sqlite3 — link both.
    CONTRIB_LIBS="$CONTRIB_LIBS -laether_sqlite -lsqlite3"
fi
if grep -rqs "import contrib.avcodec" "$(dirname "$SOURCE")"; then
    # Same shape as sqlite: our shim archive plus FFmpeg's own libraries.
    # swscale does the RGBA conversion and avutil carries the frame helpers,
    # so all four are needed together.
    CONTRIB_LIBS="$CONTRIB_LIBS -laether_avcodec -lavcodec -lavformat -lavutil -lswscale"
fi

mkdir -p "$(dirname "$C_FILE")"
echo "Compiling $SOURCE -> $C_FILE"
# An app moved under aevg/apps/<name>/ imports sibling modules (vg, vg_live,
# loader, …) that live in aevg/ — give aetherc that dir on its --lib search
# path (the shell twin of the aeb per-node lib() setter). Harmless for sources
# that don't import them.
LIB_FLAGS=()
case "$SOURCE" in
    */aevg/apps/*|aevg/apps/*) LIB_FLAGS=(--lib "$SCRIPT_DIR/aevg") ;;
esac
"$AETHERC" "${LIB_FLAGS[@]}" "$SOURCE" "$C_FILE"

OS="$(uname -s)"
case "$OS" in
    Darwin)
        echo "Platform: macOS (AppKit)"
        # AETHER_LIBS pulls in libaether's transitive deps (PCRE2 / SSL / zlib /
        # …) via `ae cflags --libs`. Apps importing std.regex-using modules need
        # -lpcre2-8 at link time; plain -laether doesn't pull it in (same as the
        # Linux branch — falling_blocks / rubiks_cube hit this on the Mac).
        AETHER_LIBS="$(ae cflags --libs 2>/dev/null || true)"
        clang -O0 -g -fobjc-arc \
            $AETHER_INCLUDES \
            "$C_FILE" "$SCRIPT_DIR/backend/aether_ui_macos.m" \
            "$SCRIPT_DIR/backend/aether_ui_test_server.c" \
            "$SCRIPT_DIR/backend/aether_ui_system_extras.c" \
            -L"$AETHER_LIB_PATH" -laether \
            -o "$OUTPUT" \
            -framework AppKit -framework Foundation -framework QuartzCore \
            -framework CoreText -framework ImageIO -pthread -lm \
            $AETHER_LIBS $CONTRIB_LIBS
        ;;
    Linux|FreeBSD)
        if ! pkg-config --exists gtk4 2>/dev/null; then
            echo "Error: GTK4 dev libraries not found."
            [ "$OS" = "FreeBSD" ] && echo "  FreeBSD: pkg install gtk4 pkgconf; ensure a zlib.pc exists for freetype2->zlib."
            exit 1
        fi
        echo "Platform: $OS (GTK4)"
        # C compiler: honor $CC, else gcc on Linux, clang on FreeBSD (whose
        # base system ships clang, not gcc). Mirrors ae build's $AE_CC/$CC.
        CC_BIN="${CC:-}"
        if [ -z "$CC_BIN" ]; then
            if [ "$OS" = "FreeBSD" ]; then CC_BIN=clang; else CC_BIN=gcc; fi
        fi
        # libnotify is optional — if present, the GTK4 backend wires real
        # OS notifications via NotifyNotification; otherwise notify*()
        # land in the registry only (still drivable via AetherUIDriver).
        LIBNOTIFY_CFLAGS=""
        LIBNOTIFY_LIBS=""
        if pkg-config --exists libnotify 2>/dev/null; then
            LIBNOTIFY_CFLAGS="-DAEUI_HAVE_LIBNOTIFY=1 $(pkg-config --cflags libnotify)"
            LIBNOTIFY_LIBS="$(pkg-config --libs libnotify)"
        fi
        # GDBus for StatusNotifierItem + DBusMenu is part of GIO, which
        # is a transitive dep of GTK4 — no extra pkg-config probe needed.
        #
        # AETHER_LIBS pulls in libaether's transitive deps (PCRE2 / SSL /
        # zlib / nghttp2) via `ae cflags --libs`. Examples that import
        # std.regex-using modules (e.g. the AeVG port's rasterize/parser)
        # need -lpcre2-8 at link time; plain -laether doesn't pull it in.
        AETHER_LIBS="$(ae cflags --libs 2>/dev/null || true)"
        "$CC_BIN" -O0 -g -pipe \
            $(pkg-config --cflags gtk4) \
            $AETHER_INCLUDES \
            $LIBNOTIFY_CFLAGS \
            "$C_FILE" "$SCRIPT_DIR/backend/aether_ui_gtk4.c" \
            "$SCRIPT_DIR/backend/aether_ui_system_extras.c" \
            "$SCRIPT_DIR/backend/aether_ui_sni.c" \
            -L"$AETHER_LIB_PATH" -laether \
            -o "$OUTPUT" \
            -pthread -lm $(pkg-config --libs gtk4) $LIBNOTIFY_LIBS $AETHER_LIBS $CONTRIB_LIBS
        ;;
    MINGW*|MSYS*|CYGWIN*)
        echo "Platform: Windows (native Win32)"
        OUT_EXE="${OUTPUT}.exe"
        [[ "$OUTPUT" != *.exe ]] && ACTUAL_OUT="$OUT_EXE" || ACTUAL_OUT="$OUTPUT"
        # AETHER_LIBS mirrors the Linux/mac branches: `ae cflags --libs`
        # emits libaether's transitive deps AND any installed contrib
        # archives (-laether_sqlite for LisMusic, ssl/pcre2/...). The
        # explicit -lssl/-lcrypto/-lpcre2-8 stay as a fallback for boxes
        # where `ae cflags` predates --libs.
        AETHER_LIBS="$(ae cflags --libs 2>/dev/null || true)"
        gcc -O2 -g -pipe \
            $AETHER_INCLUDES \
            "$C_FILE" "$SCRIPT_DIR/backend/aether_ui_win32.c" \
            "$SCRIPT_DIR/backend/aether_ui_test_server.c" \
            "$SCRIPT_DIR/backend/aether_ui_system_extras.c" \
            -L"$AETHER_LIB_PATH" -laether \
            -o "$ACTUAL_OUT" \
            -luser32 -lgdi32 -lgdiplus -lmsimg32 -lcomctl32 -lcomdlg32 \
            -lshell32 -lole32 -loleaut32 -luuid -loleacc -ldwmapi -luxtheme \
            -lws2_32 -lbcrypt -lpcre2-8 -lssl -lcrypto -pthread -lm \
            $AETHER_LIBS $CONTRIB_LIBS
        OUTPUT="$ACTUAL_OUT"
        ;;
    *)
        echo "Error: Unsupported platform '$OS'."
        echo "Aether UI supports macOS (AppKit), Linux and FreeBSD (GTK4), and Windows (Win32)."
        exit 1
        ;;
esac

echo "Built: $OUTPUT"
