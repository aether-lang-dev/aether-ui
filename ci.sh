#!/bin/bash
# ci.sh — full aether_ui test pipeline as a CI job would run it.
#
# Phases:
#   1. Build every example (catches C/Aether compile regressions).
#   2. Smoke-launch the non-driver examples to catch runtime crashes the
#      HTTP-driven tests can't reach (widget wiring, reactive state init,
#      platform-backend regressions). Each binary is launched, given 1.5s
#      to render, then killed; still-alive = pass.
#   3. Launch example_calculator with the AetherUIDriver test server and
#      run test_calculator.sh (11 assertions).
#   4. Launch example_testable and run test_automation.sh (17 assertions).
#
# Platform handling:
#   macOS    — runs directly (AppKit).
#   Linux    — runs directly if $DISPLAY or $WAYLAND_DISPLAY is set; otherwise
#              auto-wraps with xvfb-run when available. Falls back to build-only.
#   Windows  — native Win32 backend. Aether-level examples are skipped (the
#              DSL has a separate module-resolution issue on MINGW that
#              blocks `ae build`). The C-level backend test suite
#              (tests/test_widgets.c) and the HTTP driver test
#              (tests/test_driver.sh) run instead — invoked via
#              `make contrib-aether-ui-check`.
#
# Exits non-zero only when an implemented platform fails. Leaves no
# background processes.
#
# Usage: ./ci.sh [port]

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR"
PORT="${1:-9222}"

cd "$ROOT"
mkdir -p build

# --- Aether toolchain resolution guard -----------------------------------
# aeb resolves the SDK include AND lib dirs relative to `dirname(command -v ae)`
# WITHOUT dereferencing symlinks. If `ae` is a symlinked shim beside a stale/
# partial ~/.local tree, aeb links the wrong libaether.a (an old one with no
# std.audio/std.worker → `undefined reference to aether_audio_*`) and/or misses
# the nested headers (`fatal error: aether_panic.h`). Point $AETHER at the
# *dereferenced* real binary so aeb's dir resolution lands on the actual
# versioned install for both halves; also pin $AETHER_INCLUDE (which aeb honors
# first) as a backstop. No-op on a canonical /usr/local install where `ae` is
# not a symlink; only applied when the resolved tree is real (has runtime/).
_ae_bin="$(command -v "${AETHER:-ae}" 2>/dev/null || true)"
if [ -n "$_ae_bin" ]; then
    _ae_real="$(readlink -f "$_ae_bin" 2>/dev/null || true)"
    [ -n "$_ae_real" ] || _ae_real="$_ae_bin"   # BSD readlink lacks -f
    _ae_root="$(dirname "$(dirname "$_ae_real")")"
    if [ -x "$_ae_real" ] && [ -d "$_ae_root/include/aether/runtime" ]; then
        [ -z "${AETHER:-}" ]         && export AETHER="$_ae_real"
        [ -z "${AETHER_INCLUDE:-}" ] && export AETHER_INCLUDE="$_ae_root/include/aether"
        echo "  aeb toolchain pinned: AETHER=$AETHER"
    fi
fi
# -------------------------------------------------------------------------

# All examples that must compile in Phase 1.
EXAMPLES=(disclosure_demo icons_demo pills_demo textpath_demo counter form picker styled system canvas testable calculator context_menu overlay_demo vg_tooltip each_demo listbox_demo table_demo transitions_demo split_demo bindings_demo tabs_demo menu rbind_demo typo_demo multiselect_demo dblclick_demo tree_demo tabledeleg_demo weightclamp_demo shortcut_demo polish_demo vlist_demo wshortcut_demo multiwindow_demo winmenu_demo reorder_demo overlaytr_demo a11y_demo material_demo themes_demo csssem_demo zen_demo states_demo undo_demo roles_demo command_demo clipboard window_title)
# Examples without a test server — Phase 2 smoke-launches each.
# calculator and testable are exercised through their HTTP drivers in
# Phases 3-4, so they are not smoke-tested here.
SMOKE_EXAMPLES=(disclosure_demo icons_demo pills_demo textpath_demo counter form picker styled system canvas)
FAIL=0

OS="$(uname -s)"
case "$OS" in
    Darwin)  PLATFORM=macos ;;
    Linux)   PLATFORM=linux ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
    *)       PLATFORM=unknown ;;
esac
echo "=== aether_ui CI on $OS ($PLATFORM) ==="

# A crashing driver app leaves no explanation in its own output. Allow cores so
# a failing spec can print the faulting frame instead of a list of POSTs that
# stopped being answered. Each workflow step is a fresh shell, so this has to
# be set here rather than beside the core_pattern in the workflow.
if [ "$PLATFORM" = "linux" ]; then ulimit -c unlimited 2>/dev/null || true; fi

if [ "$PLATFORM" = "windows" ]; then
    echo "NOTICE: Windows backend uses a separate test flow."
    echo "        Run: make contrib-aether-ui-check"
    echo "        (headless widget suite + HTTP driver integration test)"
    exit 0
fi
if [ "$PLATFORM" = "unknown" ]; then
    echo "ERROR: unrecognized platform '$OS'."
    exit 1
fi

# Decide how to launch GUI binaries. On Linux CI runners without a display,
# wrap with xvfb-run so GTK4 has a framebuffer. The screen must be big enough
# that Xvfb's pointer (parked at screen centre) starts OUTSIDE the app
# window: GTK synthesizes crossing/motion events at the pointer position on
# every relayout, and a pointer sitting over the canvas fires the app's hover
# handler between test steps — rewriting the status line the assertions read.
launch_xvfb() { xvfb-run -a -s "-screen 0 3200x2000x24" "$@"; }
LAUNCH_PREFIX=""
if [ "$PLATFORM" = "linux" ]; then
    if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
        if command -v xvfb-run > /dev/null 2>&1; then
            LAUNCH_PREFIX="launch_xvfb"
            echo "no display detected — wrapping GUI launches with xvfb-run"
        else
            echo "NOTICE: no display and xvfb-run missing — will build-only."
            LAUNCH_PREFIX="SKIP_RUNTIME"
        fi
    fi
fi

run_server_test() {
    # Launch a binary with AETHER_UI_TEST_PORT set, wait for the test server,
    # run the given test script against it, kill the binary, propagate status.
    local bin="$1" script="$2" name="$3"
    echo "--- launching $bin ---"
    # Refuse to launch into a squatted port: a stray app from an earlier
    # run answers the spec's requests and every assertion interrogates the
    # WRONG process (stale binary, mutated state) — seen as inexplicable
    # geometry failures. Fail loudly instead.
    if curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets" 2>/dev/null; then
        echo "  FAIL: $name — port $PORT already answering (stray app?)"
        return 1
    fi
    AETHER_UI_TEST_PORT="$PORT" $LAUNCH_PREFIX "$bin" > "/tmp/ci_${name}.app.log" 2>&1 &
    local pid=$!

    # Wait up to 6s for the server to come up.
    local up=0
    for _ in $(seq 1 30); do
        if curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets"; then up=1; break; fi
        sleep 0.2
    done
    if [ "$up" -ne 1 ]; then
        echo "  FAIL: $name test server never responded"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        tail -20 "/tmp/ci_${name}.app.log" | sed 's/^/       /'
        return 1
    fi

    "$script" "$PORT"
    local rc=$?
    # Close the app PROPERLY: ask the driver to shut down (the app exits by
    # the same path as the user closing the window). Signal-killing the
    # xvfb-run wrapper is only a fallback — it can leave the app child alive
    # and HOLDING THE PORT, and the next phase then interrogates the wrong
    # app (a whole family of "impossible" test failures traced back to this).
    curl -sf -m 2 -X POST "http://127.0.0.1:$PORT/shutdown" > /dev/null 2>&1
    local freed=0
    for _ in $(seq 1 25); do
        if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets"; then freed=1; break; fi
        sleep 0.2
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    if [ "$freed" -ne 1 ]; then
        pkill -f "$bin" 2>/dev/null
        for _ in $(seq 1 25); do
            curl -sf -o /dev/null "http://127.0.0.1:$PORT/widgets" || break
            sleep 0.2
        done
    fi
    # A spec that fails AFTER the server answered leaves its diagnosis in the
    # app's own output, which was printed only on the never-responded path.
    # Without this, a mid-run crash reads as a list of "was not 2xx" lines
    # with nothing saying the process had died.
    if [ "$rc" -ne 0 ] && [ -s "/tmp/ci_${name}.app.log" ]; then
        echo "       --- $name app output (last 30 lines) ---"
        tail -30 "/tmp/ci_${name}.app.log" | sed 's/^/       /'
    fi
    # A crash (SIGBUS/SIGSEGV) leaves nothing useful in the app's own output:
    # the process is gone before it can say anything, and the spec only sees
    # POSTs that stopped being answered. macOS files a report naming the
    # faulting frame, so surface it here rather than leaving the next reader
    # to guess from "was not 2xx".
    if [ "$rc" -ne 0 ] && [ "$PLATFORM" = "linux" ] && command -v gdb > /dev/null 2>&1; then
        local core
        core="$(ls -t /tmp/core."$(basename "$bin")".* 2>/dev/null | head -1)"
        if [ -n "$core" ]; then
            echo "       --- $name backtrace from $core ---"
            gdb -batch -ex 'thread apply all bt' "$bin" "$core" 2>/dev/null \
                | head -60 | sed 's/^/       /'
            rm -f "$core"
        fi
    fi
    if [ "$rc" -ne 0 ] && [ "$PLATFORM" = "macos" ]; then
        local base report
        base="$(basename "$bin")"
        report="$(ls -t "$HOME/Library/Logs/DiagnosticReports/${base}"*.ips 2>/dev/null | head -1)"
        if [ -n "$report" ]; then
            echo "       --- $name crash report: $(basename "$report") ---"
            sed -n '1,80p' "$report" | sed 's/^/       /'
        fi
    fi
    return $rc
}

run_smoke_test() {
    # Launch a GUI binary, give it 1.5s to open its window, then kill it.
    # Pass iff the process is still alive at the deadline. A process that
    # exited early is a crash (missing widget impl, null deref on init,
    # backend API mismatch) — propagate non-zero and dump the tail.
    local bin="$1" name="$2"
    $LAUNCH_PREFIX "$bin" > "/tmp/ci_smoke_${name}.log" 2>&1 &
    local pid=$!
    sleep 1.5
    if kill -0 "$pid" 2>/dev/null; then
        echo "  OK   $name (alive 1.5s)"
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        return 0
    fi
    wait "$pid" 2>/dev/null; local rc=$?
    echo "  FAIL $name (exited early, rc=$rc)"
    tail -15 "/tmp/ci_smoke_${name}.log" | sed 's/^/       /'
    return 1
}

# AeVG port unit tests — pure Aether (no GTK/display), so they run even
# under SKIP_RUNTIME. Each is a self-contained `main()` that exits non-zero
# on the first failed assertion. Append new modules' tests here as they land.
AEVG_TESTS=(test_font test_text_path test_transform test_normalizer test_easing test_parser test_bbox test_region test_blur test_rasterize test_grammar_utils test_grammar_context test_grammar_element test_grammar_rendering test_grammar_style test_grammar_shapes test_grammar_factories test_grammar_animations test_loader test_grammar_defs test_grammar_text test_grammar_css test_grammar_events test_path_builder test_polypath test_vg3d test_render_as_raster test_grammar_bind test_grammar_reactive test_gradient_transform test_font_family test_refresh test_reactive_bindpos test_backend_dispatch test_raster_roundtrip test_filter_routing test_gradient_fill test_vg test_transpiler test_grammar_interaction test_vg_interactive test_vg_when test_vg_bindto test_vg_bindpos test_vg_clock test_vg_hidpi test_vg_anim test_live_region test_effects test_behavior test_base64)
# Tests that exercise the REAL cairo text metrics — linked against the GTK4
# backend (the pure-Aether AEVG_TESTS link with $(ae cflags) only).
AEVG_GTK_TESTS=(test_text_metrics test_group_pixels)
# test_group_pixels rasterizes through the GTK backend, so it needs a display;
# test_text_metrics only measures text and does not. Running the display-bound
# one bare is what failed a headless runner with "Failed to open display" even
# though the workflow installs xvfb.
AEVG_GTK_NEEDS_DISPLAY=" test_group_pixels "

# `ae cflags --libs` emits the transitive deps that libaether.a was
# built with (PCRE2 / OpenSSL / zlib / nghttp2 — see Aether CHANGELOG
# [current], the cmd_cflags fix). So `$(ae cflags)` alone is enough
# for any module that imports std.regex / std.cryptography / std.http
# / std.zlib. The earlier per-dep workaround is no longer needed.

echo "=== Phase 0: AeVG unit tests (pure Aether) ==="
for t in "${AEVG_TESTS[@]}"; do
    src="vg/test/${t}.ae"
    cfile="build/aevg_${t}.c"
    bin="build/aevg_${t}"
    if ! aetherc --lib "$ROOT" "$src" "$cfile" > "/tmp/ci_aevg_${t}.log" 2>&1; then
        echo "  FAIL $t (compile)"
        tail -15 "/tmp/ci_aevg_${t}.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
        continue
    fi
    # vg/module.ae declares the cairo text-metric externs; pure-Aether tests
    # link no GTK backend, so a zero-returning stub resolves those symbols
    # (tests importing vg never call them — test_text_metrics uses the real
    # backend via AEVG_GTK_TESTS below).
    if ! gcc "$cfile" vg/test/text_metrics_stub.c $(ae cflags) -o "$bin" >> "/tmp/ci_aevg_${t}.log" 2>&1; then
        echo "  FAIL $t (link)"
        tail -15 "/tmp/ci_aevg_${t}.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
        continue
    fi
    if "$bin" > "/tmp/ci_aevg_${t}_run.log" 2>&1; then
        echo "  OK   $t"
    else
        echo "  FAIL $t (run)"
        tail -15 "/tmp/ci_aevg_${t}_run.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
    fi
done

# GTK-backend-linked unit tests: those exercising the real cairo text
# metrics (aether_ui_text_measure etc.) need the GTK4 backend + gtk4 libs
# linked, unlike the pure-Aether tests above. Skipped when GTK is absent
# (e.g. build-only runners); pure metrics have no display dependency, so
# they run even under SKIP_RUNTIME as long as gtk4 dev libs are present.
if pkg-config --exists gtk4 2>/dev/null; then
    for t in "${AEVG_GTK_TESTS[@]}"; do
        src="vg/test/${t}.ae"; cfile="build/aevg_${t}.c"; bin="build/aevg_${t}"
        if ! aetherc --lib "$ROOT" "$src" "$cfile" > "/tmp/ci_aevg_${t}.log" 2>&1; then
            echo "  FAIL $t (compile)"; tail -15 "/tmp/ci_aevg_${t}.log" | sed 's/^/       /'; FAIL=$((FAIL + 1)); continue
        fi
        if ! gcc $(pkg-config --cflags gtk4) "$cfile" \
                backend/aether_ui_gtk4.c backend/aether_ui_system_extras.c backend/aether_ui_sni.c \
                backend/aether_ui_test_server.c \
                $(ae cflags) -pthread -lm $(pkg-config --libs gtk4) -o "$bin" >> "/tmp/ci_aevg_${t}.log" 2>&1; then
            echo "  FAIL $t (link)"; tail -15 "/tmp/ci_aevg_${t}.log" | sed 's/^/       /'; FAIL=$((FAIL + 1)); continue
        fi
        runner=""
        case "$AEVG_GTK_NEEDS_DISPLAY" in
            *" $t "*)
                if [ "$LAUNCH_PREFIX" = "SKIP_RUNTIME" ]; then
                    echo "  SKIP $t (needs a display; none available and no xvfb-run)"
                    continue
                fi
                runner="$LAUNCH_PREFIX"
                # Xvfb runs need the cairo renderer (GTK's NGL on llvmpipe
                # churns memory), the same reason the GUI spec phases set it.
                case "$runner" in *xvfb*) export GSK_RENDERER=cairo ;; esac
                ;;
        esac
        if $runner "$bin" > "/tmp/ci_aevg_${t}_run.log" 2>&1; then
            echo "  OK   $t (gtk-linked)"
        else
            echo "  FAIL $t (run)"; tail -15 "/tmp/ci_aevg_${t}_run.log" | sed 's/^/       /'; FAIL=$((FAIL + 1))
        fi
        unset GSK_RENDERER
    done
else
    echo "  SKIP AEVG_GTK_TESTS (gtk4 dev libs absent)"
fi

# App-level pure engine tests — game logic factored out of an app with NO UI
# or vg dependency, so it's unit-testable in isolation (the svg_tetris engine:
# collision, rotation, line-clear, ghost, scoring, game-over). Compiled with
# the app's own dir on the lib path so `import <engine>` resolves; links
# libaether only (no backend, no text-metrics stub — nothing imports vg).
ENGINE_TESTS=("svg_tetris:test_tetris_engine" "falling_blocks:test_fb_engine" "rubiks_cube:test_cube_engine" "font_picker:test_picker_engine" "maerkdown:test_mdown")
for spec in "${ENGINE_TESTS[@]}"; do
    app="${spec%%:*}"; t="${spec##*:}"
    src="apps/${app}/${t}.ae"; cfile="build/eng_${t}.c"; bin="build/eng_${t}"
    if ! aetherc --lib "apps/${app}" "$src" "$cfile" > "/tmp/ci_eng_${t}.log" 2>&1; then
        echo "  FAIL $t (compile)"; tail -15 "/tmp/ci_eng_${t}.log" | sed 's/^/       /'; FAIL=$((FAIL + 1)); continue
    fi
    if ! gcc "$cfile" $(ae cflags) -o "$bin" >> "/tmp/ci_eng_${t}.log" 2>&1; then
        echo "  FAIL $t (link)"; tail -15 "/tmp/ci_eng_${t}.log" | sed 's/^/       /'; FAIL=$((FAIL + 1)); continue
    fi
    if "$bin" > "/tmp/ci_eng_${t}_run.log" 2>&1; then
        echo "  OK   $t"
    else
        echo "  FAIL $t (run)"; tail -15 "/tmp/ci_eng_${t}_run.log" | sed 's/^/       /'; FAIL=$((FAIL + 1))
    fi
done
echo

echo "=== Phase 1: build all aether_ui examples (aeb fan-out) ==="
# Each example is a per-app .build.ae node under examples/<app>/; the root
# .all.ae scans + deps them, so this one command builds all 11 (cached,
# parallel). Binaries land at target/build/examples/<app>/bin/<app>; EX_BIN
# resolves that for the smoke/driver phases below.
EX_BIN() { echo "$ROOT/target/build/examples/$1/bin/$1"; }
if ( cd "$ROOT" && aeb .all.ae ) > /tmp/ci_build_all.log 2>&1; then
    for ex in "${EXAMPLES[@]}"; do
        if [ -x "$(EX_BIN "$ex")" ]; then echo "  OK   $ex"; else
            echo "  FAIL $ex (no binary)"; FAIL=$((FAIL + 1)); fi
    done
else
    echo "  FAIL: aeb .all.ae fan-out build failed"
    # The fan-out prints one progress line per app, so a plain tail is all
    # progress and no diagnostic. A bare grep is not enough either: a linker
    # error names the missing symbol on the lines AFTER the match, so print
    # trailing context or the actual cause is lost (that is how an undefined
    # symbol reached CI as four unattributed line numbers).
    grep -nEi -A6 "error|failed|undefined|cannot |no such file" /tmp/ci_build_all.log \
        | grep -viE "^[0-9]+[:-] *build: " | head -60 | sed 's/^/       /'
    echo "       --- last 60 lines ---"
    tail -60 /tmp/ci_build_all.log | sed 's/^/       /'
    FAIL=$((FAIL + 1))
fi

# Contrib-dependent apps are deliberately outside the .all.ae fan-out: they link
# archives (libaether_sqlite, libaether_avcodec) that a clean checkout does not
# have, so including them would make every fresh clone and every CI run red for
# a missing optional dependency. Build them only when the archive is actually
# present, and say plainly when they are skipped rather than passing silently.
CONTRIB_LIB_DIR="$(ae cflags --libs 2>/dev/null | tr ' ' '\n' | grep -E '^-L' | head -1 | sed 's/^-L//')"
LISMUSIC_BUILT=0
echo "=== Phase 1b: contrib apps (built only when their archives exist) ==="
contrib_app() {
    app="$1"; archive="$2"
    if [ -n "$CONTRIB_LIB_DIR" ] && [ -f "$CONTRIB_LIB_DIR/lib$archive.a" ]; then
        if ( cd "$ROOT" && aeb "apps/$app/.build.contrib.ae" ) > "/tmp/ci_contrib_$app.log" 2>&1; then
            echo "  OK   $app"
            return 0
        fi
        echo "  FAIL $app (lib$archive.a present but the build failed)"
        tail -30 "/tmp/ci_contrib_$app.log" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
        return 1
    fi
    echo "  SKIP $app (lib$archive.a not installed; see roadmap.md for its manual build)"
    return 1
}
contrib_app LisMusic    aether_sqlite  && LISMUSIC_BUILT=1
contrib_app video_frame aether_avcodec || true

# Phase 1.5: RUN the headless AeVG renderers (build was done by the .all.ae
# fan-out in Phase 1 — every vg app is a apps/<name>/ node now). The
# value here is RUNTIME coverage the build-only fan-out can't give: each writes
# a PNG, exercising the raster-blit + draw-region compose path. Linux/GTK only.
AEVG_BIN() { echo "$ROOT/target/build/apps/$1/bin/$1"; }
if [ "$PLATFORM" = "linux" ]; then
    echo "  --- AeVG headless renderers (run → PNG) ---"
    run_png() {  # $1=app $2=desc
        local bin; bin="$(AEVG_BIN "$1")"
        # HONOUR $LAUNCH_PREFIX like every other runtime phase. "Headless
        # renderer" means no WINDOW -- GTK still needs a display connection to
        # initialise, so on a DISPLAY-less box these died with
        # "Gtk-WARNING: cannot open display" while every other phase was
        # correctly wrapped in xvfb-run. Found by the first fresh-box run
        # (asks/ci-sh-on-a-fresh-headless-box.md).
        if [ "$LAUNCH_PREFIX" = "SKIP_RUNTIME" ]; then
            echo "  SKIP $1 ($2 — no display, no xvfb-run)"
            return
        fi
        if [ -x "$bin" ] \
                && AEVG_OUT="/tmp/ci_$1.png" $LAUNCH_PREFIX "$bin" > "/tmp/ci_run_$1.log" 2>&1 \
                && [ -f "/tmp/ci_$1.png" ]; then
            echo "  OK   $1 ($2)"
        else
            echo "  FAIL $1"
            tail -15 "/tmp/ci_run_$1.log" 2>/dev/null | sed 's/^/       /'
            FAIL=$((FAIL + 1))
        fi
    }
    run_png aevg_live_png   "live raster + draw region composite"
    run_png aevg_video_png  "raw-RGBA clip in a live region"
    run_png analog_clock_png "one-frame clock render"
fi

if [ "$FAIL" -gt 0 ]; then
    echo
    echo "=== CI result: $FAIL build failure(s) — skipping runtime phases ==="
    exit 1
fi

if [ "$LAUNCH_PREFIX" = "SKIP_RUNTIME" ]; then
    echo
    echo "=== CI result: builds passed; runtime phases skipped (no display) ==="
    exit 0
fi

echo
echo "=== Phase 2: smoke-launch non-driver examples ==="
for ex in "${SMOKE_EXAMPLES[@]}"; do
    run_smoke_test "$(EX_BIN "$ex")" "$ex" || FAIL=$((FAIL + 1))
done

# Phases 3-6 are driver specs (tests/<app>/spec_*.ae — Aether programs on
# the shared tests/lib/uidriver.ae client; tests/run_spec.sh is launcher
# glue).
#
# They used to need an aeocha CHECKOUT, resolved from ~/scm/aeocha or a
# sibling directory, with a hard FAIL when absent. That requirement is gone:
# aeocha's core was absorbed into the stdlib as `std.spec` (ae 0.539) and its
# HTTP matchers as `std.http.client.httptest`, so the framework now ships with
# the toolchain. Nothing to clone, nothing to locate, and no way for a fresh
# box to fail these phases for want of a repo.
#
# SPEC_OK is kept (rather than deleting ~10 guards) so the phase structure is
# unchanged; it is simply always satisfied now.
SPEC_OK=1

# Implicit transitions (item 6) exist now: every DRIVER phase runs with
# animations OFF so specs assert end states deterministically (house rule).
# Phase 5h is the exception — it PROVES the tween, so it unsets this.
export AETHER_UI_NO_ANIMATION=1

echo
echo "=== Phase 3: AetherUIDriver calculator spec ==="
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=calculator/spec_calculator \
    run_server_test "$(EX_BIN calculator)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" calculator || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 3b: AetherUIDriver text-metrics spec ==="
# App-agnostic: exercises the GET /text_extent route against the calculator
# binary (any driver-armed app exposes it). Verifies the cairo text metrics
# behave over the real HTTP surface (roadmap item 2).
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=text_metrics/spec_text_metrics \
    run_server_test "$(EX_BIN calculator)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" text_metrics || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 4: AetherUIDriver testable spec ==="
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=testable/spec_testable \
    run_server_test "$(EX_BIN testable)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" testable || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5: AetherUIDriver context-menu spec ==="
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=context_menu/spec_context_menu \
    run_server_test "$(EX_BIN context_menu)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" context_menu || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5b: AetherUIDriver overlay-layer spec ==="
# In-window overlay layer (roadmap item 1): toast open + auto-dismiss, modal
# scrim proven by a real /window/pick hit-test (the glass pane resolves ahead
# of the button beneath it), dismiss restores access.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=overlay_demo/spec_overlay_demo \
    run_server_test "$(EX_BIN overlay_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" overlay_demo || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5c: AetherUIDriver vg drawn-tooltip spec ==="
# The drawn-tooltip half of the overlay layer: a vg shape's tooltip() opens a
# label overlay near the pointer (forced on via AETHER_UI_TOOLTIP=drawn) —
# hover a shape → tooltip appears; off it → gone. Driven via /canvas/1/move.
if [ "$SPEC_OK" -eq 1 ]; then
    AETHER_UI_TOOLTIP=drawn \
    UI_SPEC=vg_tooltip/spec_vg_tooltip \
    run_server_test "$(EX_BIN vg_tooltip)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" vg_tooltip || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5d: AetherUIDriver picker ABI spec (drawn surface) ==="
# Picker ABI parity: the same select/read-back assertions pass on the drawn
# dropdown (a button + in-window overlay list) as on the native GtkDropDown.
# Run here under the DRAWN surface — the native surface is the everyday path
# and is smoke-launched in Phase 2.
if [ "$SPEC_OK" -eq 1 ]; then
    AETHER_UI_PICKER=drawn \
    UI_SPEC=picker/spec_picker \
    run_server_test "$(EX_BIN picker)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" picker || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5e: AetherUIDriver each (dynamic children) spec ==="
# each (roadmap item 3): Add/Remove/Reset drive each_update; the spec asserts
# group children appear/disappear in /widgets and per-item closures fire with
# the RIGHT item (needs aether >= 0.390 closure-capture fixes).
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=each_demo/spec_each_demo \
    run_server_test "$(EX_BIN each_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" each_demo || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5f: AetherUIDriver listbox spec ==="
# listbox (item 4 D1): rows are real widgets; the driver clicks a ROW (click
# falls back to gesture handlers on non-buttons), selection reads back via
# the tracked "classes" JSON field; 200-row updates stay driver-visible.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=listbox_demo/spec_listbox_demo \
    run_server_test "$(EX_BIN listbox_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" listbox_demo || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5g: AetherUIDriver table spec ==="
# table (item 4 D2): header buttons fire on_sort with the right column index,
# app-side re-sort re-renders row order (asserted via widget creation order),
# cells render per column, selection delegates to the listbox layer.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=table_demo/spec_table_demo \
    run_server_test "$(EX_BIN table_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" table_demo || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5i: AetherUIDriver splitview spec ==="
# splitview (item 7 D1): panes are real widgets; POST split_position moves
# the splitter and the pane allocations follow; app-side get/set round-trips.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=split_demo/spec_split_demo \
    run_server_test "$(EX_BIN split_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" split_demo || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5j: AetherUIDriver typed-state + bindings spec ==="
# bindings (item 8): typed /state routes (float byte-compatible), one bool
# state flipping enabled/enabled-inverted/hidden, driver + app-side sets.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=bindings_demo/spec_bindings_demo \
    run_server_test "$(EX_BIN bindings_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" bindings_demo || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5k: AetherUIDriver tabs spec ==="
# tabs: type "tabs" with tabSelected/tabCount, each tab() page built, the
# tab_select route + app-side buttons switch pages and fire on_tab_change.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=tabs_demo/spec_tabs_demo \
    run_server_test "$(EX_BIN tabs_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" tabs_demo || FAIL=$((FAIL + 1))
fi

echo "=== Phase 5k2: AetherUIDriver menu-bar spec ==="
# menu: GET /menus lists the bar's menus + item labels; POST
# /menu/{h}/activate fires the item's real closure (native GTK4 GAction /
# NSMenu / win32), with the effect observable in the bound counter.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=menu/spec_menu \
    run_server_test "$(EX_BIN menu)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" menu || FAIL=$((FAIL + 1))
fi

echo "=== Phase 5k3: AetherUIDriver reactive-bindings spec (each_bind + computed) ==="
# rbind: list-typed state drives each_update via each_bind; computed_s recomputes
# a derived state when an input changes. Backend-agnostic (state layer).
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=rbind_demo/spec_rbind_demo \
    run_server_test "$(EX_BIN rbind_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" rbind_demo || FAIL=$((FAIL + 1))
fi

echo "=== Phase 5k4: AetherUIDriver typography / multi-select / double-click specs ==="
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=typo_demo/spec_typo_demo \
    run_server_test "$(EX_BIN typo_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" typo_demo || FAIL=$((FAIL + 1))
    UI_SPEC=multiselect_demo/spec_multiselect_demo \
    run_server_test "$(EX_BIN multiselect_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" multiselect_demo || FAIL=$((FAIL + 1))
    UI_SPEC=dblclick_demo/spec_dblclick_demo \
    run_server_test "$(EX_BIN dblclick_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" dblclick_demo || FAIL=$((FAIL + 1))
fi

echo "=== Phase 5k5: AetherUIDriver tree + table-delegate/bind specs ==="
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=tree_demo/spec_tree_demo \
    run_server_test "$(EX_BIN tree_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" tree_demo || FAIL=$((FAIL + 1))
    UI_SPEC=tabledeleg_demo/spec_tabledeleg_demo \
    run_server_test "$(EX_BIN tabledeleg_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" tabledeleg_demo || FAIL=$((FAIL + 1))
fi

echo "=== Phase 5k6: AetherUIDriver weight-clamp + shortcut-scope/chord specs ==="
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=weightclamp_demo/spec_weightclamp_demo \
    run_server_test "$(EX_BIN weightclamp_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" weightclamp_demo || FAIL=$((FAIL + 1))
    UI_SPEC=shortcut_demo/spec_shortcut_demo \
    run_server_test "$(EX_BIN shortcut_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" shortcut_demo || FAIL=$((FAIL + 1))
    UI_SPEC=polish_demo/spec_polish_demo \
    run_server_test "$(EX_BIN polish_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" polish_demo || FAIL=$((FAIL + 1))
    UI_SPEC=vlist_demo/spec_vlist_demo \
    run_server_test "$(EX_BIN vlist_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" vlist_demo || FAIL=$((FAIL + 1))
    UI_SPEC=wshortcut_demo/spec_wshortcut_demo \
    run_server_test "$(EX_BIN wshortcut_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" wshortcut_demo || FAIL=$((FAIL + 1))
    UI_SPEC=multiwindow_demo/spec_multiwindow_demo \
    run_server_test "$(EX_BIN multiwindow_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" multiwindow_demo || FAIL=$((FAIL + 1))
    UI_SPEC=winmenu_demo/spec_winmenu_demo \
    run_server_test "$(EX_BIN winmenu_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" winmenu_demo || FAIL=$((FAIL + 1))
    UI_SPEC=reorder_demo/spec_reorder_demo \
    run_server_test "$(EX_BIN reorder_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" reorder_demo || FAIL=$((FAIL + 1))
    # Overlay entry transitions — the deterministic contract (animation OFF):
    # dismiss removes the entry at once. The exit-tween-observable case runs in
    # its own animation-ON phase below.
    UI_SPEC=overlaytr_demo/spec_overlaytr_demo \
    run_server_test "$(EX_BIN overlaytr_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" overlaytr_demo || FAIL=$((FAIL + 1))
    # Accessibility semantics — role/name/description read back from the real
    # backend accessible state (GtkAccessible / MSAA / NSAccessibility).
    UI_SPEC=a11y_demo/spec_a11y_demo \
    run_server_test "$(EX_BIN a11y_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" a11y_demo || FAIL=$((FAIL + 1))
    # Backdrop material — a blur request degrades to tint on GTK4/win32 (no
    # in-window backdrop blur); the effective material is reported, not silent.
    UI_SPEC=material_demo/spec_material_demo \
    run_server_test "$(EX_BIN material_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" material_demo || FAIL=$((FAIL + 1))
    # Stylesheet/theming layer — element/class cascade + live re-theme,
    # asserted from the backend's fg/bg readback.
    UI_SPEC=themes_demo/spec_themes_demo \
    run_server_test "$(EX_BIN themes_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" themes_demo || FAIL=$((FAIL + 1))

    UI_SPEC=roles_demo/spec_roles_demo \
    run_server_test "$(EX_BIN roles_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" roles_demo || FAIL=$((FAIL + 1))

    UI_SPEC=command_demo/spec_command_demo \
    run_server_test "$(EX_BIN command_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" command_demo || FAIL=$((FAIL + 1))
    # AeCS vs CSS semantics — W3C Selectors/cascade intent + pinned divergences.
    UI_SPEC=csssem_demo/spec_csssem_demo \
    run_server_test "$(EX_BIN csssem_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" csssem_demo || FAIL=$((FAIL + 1))
    # AeCS Zen Garden — one tree, many skins (in-code + .aecs file palettes).
    UI_SPEC=zen_demo/spec_zen_demo \
    run_server_test "$(EX_BIN zen_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" zen_demo || FAIL=$((FAIL + 1))
    # QML-alike states on AeCS + Swing-style undo manager.
    UI_SPEC=states_demo/spec_states_demo \
    run_server_test "$(EX_BIN states_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" states_demo || FAIL=$((FAIL + 1))
    UI_SPEC=undo_demo/spec_undo_demo \
    run_server_test "$(EX_BIN undo_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" undo_demo || FAIL=$((FAIL + 1))
    UI_SPEC=textpath_demo/spec_textpath_demo \
    run_server_test "$(EX_BIN textpath_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" textpath_demo || FAIL=$((FAIL + 1))
    UI_SPEC=pills_demo/spec_pills_demo \
    run_server_test "$(EX_BIN pills_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" pills_demo || FAIL=$((FAIL + 1))
    UI_SPEC=icons_demo/spec_icons_demo \
    run_server_test "$(EX_BIN icons_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" icons_demo || FAIL=$((FAIL + 1))
    UI_SPEC=disclosure_demo/spec_disclosure_demo \
    run_server_test "$(EX_BIN disclosure_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" disclosure_demo || FAIL=$((FAIL + 1))
fi

echo "=== Phase 5l: AetherUIDriver app/demo specs (frames / falling_blocks / svg_tetris / rubiks_cube / tumbling_cube) ==="
# The AeVG games + interactive demos (apps/, not examples/): each drives its
# buttons and canvas through the driver end-to-end, complementing the pure
# engine unit tests in Phase 0. AEVG_BIN resolves target/build/apps/<name>/bin/<name>.
if [ "$SPEC_OK" -eq 1 ]; then
    UI_SPEC=frames_demo/spec_frames_demo \
    run_server_test "$(AEVG_BIN frames_demo)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" frames_demo || FAIL=$((FAIL + 1))
    # This one runs with animations OFF, unlike the transition proofs below.
    # It asserts that a drag produces a CLIPPED paint, and frames_demo never
    # clips while the scene animates (7e1ebd1). It was wired here with
    # animations forced on a day before that gate landed (d4ca5382), so its
    # premise has been false ever since: every paint went full-canvas and the
    # assertion could not pass on any backend. Leaving the global
    # AETHER_UI_NO_ANIMATION=1 in place is what lets the clip path engage.
    run_server_test "$(AEVG_BIN frames_demo)" \
                    "$SCRIPT_DIR/tests/frames_demo/test_stage25_clip_surface.sh" \
                    frames_stage25 || FAIL=$((FAIL + 1))
    UI_SPEC=falling_blocks/spec_falling_blocks \
    run_server_test "$(AEVG_BIN falling_blocks)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" falling_blocks || FAIL=$((FAIL + 1))
    UI_SPEC=svg_tetris/spec_svg_tetris \
    run_server_test "$(AEVG_BIN svg_tetris)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" svg_tetris || FAIL=$((FAIL + 1))
    UI_SPEC=rubiks_cube/spec_rubiks_cube \
    run_server_test "$(AEVG_BIN rubiks_cube)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" rubiks_cube || FAIL=$((FAIL + 1))
    UI_SPEC=tumbling_cube/spec_tumbling_cube AEVG_FREEZE=1 \
    run_server_test "$(AEVG_BIN tumbling_cube)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" tumbling_cube || FAIL=$((FAIL + 1))
    UI_SPEC=font_picker/spec_font_picker \
    run_server_test "$(AEVG_BIN font_picker)" \
                    "$SCRIPT_DIR/tests/run_spec.sh" font_picker || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5h2: overlay entry EXIT transition proof (animations ON) ==="
# With animation on, dismissing a transitioned overlay must play its exit tween
# (exiting:1) BEFORE removal — the spec's anim-on branch asserts that. Same
# binary + spec as the deterministic run above; only the flag differs.
if [ "$SPEC_OK" -eq 1 ]; then
    ( unset AETHER_UI_NO_ANIMATION
      UI_SPEC=overlaytr_demo/spec_overlaytr_demo \
      run_server_test "$(EX_BIN overlaytr_demo)" \
                      "$SCRIPT_DIR/tests/run_spec.sh" overlaytr_demo ) || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 5h: implicit transitions tween proof (animations ON) ==="
# ui.transition must actually TWEEN: screenshot mid-flight differs from
# settled. Shell checks (PNG byte compare — see tests/transitions_demo/).
if true; then
    ( unset AETHER_UI_NO_ANIMATION
      run_server_test "$(EX_BIN transitions_demo)"                       "$SCRIPT_DIR/tests/transitions_demo/test_tween.sh" transitions ) || FAIL=$((FAIL + 1))
    ( unset AETHER_UI_NO_ANIMATION
      run_server_test "$(EX_BIN transitions_demo)"                       "$SCRIPT_DIR/tests/transitions_demo/test_easing_curve.sh" easingcurve ) || FAIL=$((FAIL + 1))
    ( unset AETHER_UI_NO_ANIMATION
      run_server_test "$(EX_BIN transitions_demo)"                       "$SCRIPT_DIR/tests/transitions_demo/test_spring_curve.sh" springcurve ) || FAIL=$((FAIL + 1))
fi

echo
echo "=== Phase 6: AetherUIDriver grand_perspective tests (Aeocha specs) ==="
# One Aeocha spec per app component (tests/grand_perspective/spec_*.ae —
# Aether programs driving the HTTP API via std.http.client; run_spec.sh is
# only include-path glue). Each spec runs against a FRESH app instance
# scanning a FRESH fixture — the fileops spec really trashes a fixture
# file, so isolation is what makes the specs order-independent. The app
# scans $AEVG_DIR on launch; specs assert against the same tree via
# $GP_FIXTURE. Fixture under $HOME: gio trash refuses /tmp on some OSes
# (FreeBSD: "Trashing on system internal mounts is not supported").
# Xvfb runs need the cairo renderer (GTK's NGL on llvmpipe churns memory).
if [ "$SPEC_OK" -eq 1 ]; then
    case "$LAUNCH_PREFIX" in *xvfb*) export GSK_RENDERER=cairo ;; esac
    for gp_spec in scan_and_list map_nav legend fileops hover_and_resize; do
        GP_FIX=$(mktemp -d "$HOME/.gp-ci-XXXXXX")
        mkdir -p "$GP_FIX/sub"
        head -c 400000 /dev/urandom > "$GP_FIX/big.bin"
        head -c 250000 /dev/urandom > "$GP_FIX/mid.bin"
        head -c 200000 /dev/urandom > "$GP_FIX/sub/inner.bin"
        export AEVG_DIR="$GP_FIX" GP_FIXTURE="$GP_FIX" UI_SPEC="grand_perspective/spec_${gp_spec}"
        run_server_test "$ROOT/target/build/apps/grand_perspective/bin/grand_perspective" \
                        "$SCRIPT_DIR/tests/run_spec.sh" "gp_${gp_spec}" || FAIL=$((FAIL + 1))
        unset AEVG_DIR GP_FIXTURE UI_SPEC
        rm -rf "$GP_FIX"
    done
    unset GSK_RENDERER
fi

echo
echo "=== Phase 7: AetherUIDriver LisMusic port spec ==="
# LisMusic (apps/LisMusic): the three-region shell renders, sidebar nav drives
# the right-page tab stack, search populates the each-list, transport wired.
# Real backends: contrib.sqlite (search history, history.db — cleaned per run),
# std.audio (playback; NULL backend under headless CI), std.worker+std.http
# (async search). LIS_OFFLINE=1 forces the deterministic canned search so the
# spec's assertions don't depend on a live network. AETHER_UI_HEADLESS keeps
# std.audio on its silent null backend.
if [ "$SPEC_OK" -eq 1 ] && [ "$LISMUSIC_BUILT" -eq 1 ]; then
    rm -f "$ROOT/history.db"
    export LIS_OFFLINE=1
    UI_SPEC=LisMusic/spec_lismusic \
    run_server_test "$ROOT/target/build/apps/LisMusic/bin/LisMusic" \
                    "$SCRIPT_DIR/tests/run_spec.sh" lismusic || FAIL=$((FAIL + 1))
    unset LIS_OFFLINE
    rm -f "$ROOT/history.db"
elif [ "$SPEC_OK" -eq 1 ]; then
    echo "  SKIP lismusic spec (LisMusic was not built; libaether_sqlite absent)"
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "=== CI result: all phases passed ==="
    exit 0
else
    echo "=== CI result: $FAIL phase(s) failed ==="
    exit 1
fi
