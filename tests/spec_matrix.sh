#!/usr/bin/env bash
# spec_matrix.sh — run every Aeocha suite against its app and tabulate.
#
# The platform-parity baseline in one command. The Windows equivalent of this
# lived in a session scratchpad and was never committed (see roadmap.md), which
# meant the next person had to reconstruct it from ci.sh. This is that script,
# committed, and portable across all three backends.
#
#   ./tests/spec_matrix.sh              # every suite
#   ./tests/spec_matrix.sh split table  # just these
#
# Binaries come from `aeb .all.ae` (target/build/...). Run that first.
#
# Counting: the number printed is aeocha's own "N passing" — one per it()
# block. Failures are counted from its "N failing" tail. Those two numbers
# come straight from the tool, so the matrix is reproducible rather than
# hand-tallied (the roadmap's Windows row mixes it-counts and assertion-counts
# and is confusing as a result).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT=9222
# HEADLESS only where a hidden window still LAYS OUT: win32 (SW_HIDE children
# get real rects) and macOS (Auto Layout runs unmapped). On GTK4, headless
# realizes-but-never-presents, so NO allocation pass runs — every geometry
# read is 0, picks miss, canvas coords are dead. Discovered on this script's
# first-ever Linux run: 8 suites red purely from this flag; ci.sh (the Linux
# harness) never sets it and the same suites are green there. Linux runs
# mapped-under-Xvfb instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Darwin) export AETHER_UI_HEADLESS=1 ;;
esac
export AETHER_UI_NO_ANIMATION=1

# Per-OS launch wrapper + the exe suffix.
#
# FreeBSD (GhostBSD) has no display in a plain SSH session, so — like Linux CI
# — we spin up our OWN private Xvfb (pkg: xorg-vfbserver) at 3200x2000 with the
# pointer parked off-window, and point launches at it via DISPLAY. This runs
# UNPRIVILEGED (no sudo, no lightdm :0), and gives the same mapped-but-hidden
# framebuffer Linux uses, so pointer-parking and HEADLESS behave identically.
# Override the display via $DISPLAY (any pre-set DISPLAY is respected and Xvfb
# is skipped — e.g. to use a real :0). See ~/aether-ui-freebsd-notes.md on .204.
EXE=""; FREEBSD=0; XVFB_PID=""
case "$(uname -s)" in
    FreeBSD) FREEBSD=1 ;;
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

if [ "$FREEBSD" -eq 1 ] && [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null 2>&1; then
    # A private display; :99 by convention unless taken.
    XV_DISP=":99"
    Xvfb "$XV_DISP" -screen 0 3200x2000x24 -ac >/tmp/aui_xvfb.log 2>&1 &
    XVFB_PID=$!
    export DISPLAY="$XV_DISP"
    for _ in $(seq 1 20); do
        xdpyinfo -display "$XV_DISP" >/dev/null 2>&1 && break
        sleep 0.2
    done
    trap 'kill "$XVFB_PID" 2>/dev/null' EXIT
fi

# Launch `$bin` with the given inline `VAR=val` env pairs — a plain env exec on
# every platform now (the display is set up above). Backgrounded by the caller.
aui_launch() {   # aui_launch VAR=val ... -- <bin>
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    env "${envs[@]}" "$@"
}

# suite | binary | spec | extra env
SUITES=(
  "calculator|examples/calculator|calculator/spec_calculator|"
  "text_metrics|examples/calculator|text_metrics/spec_text_metrics|"
  "testable|examples/testable|testable/spec_testable|"
  "context_menu|examples/context_menu|context_menu/spec_context_menu|"
  "overlay|examples/overlay_demo|overlay_demo/spec_overlay_demo|"
  "vg_tooltip|examples/vg_tooltip|vg_tooltip/spec_vg_tooltip|AETHER_UI_TOOLTIP=drawn"
  "picker|examples/picker|picker/spec_picker|AETHER_UI_PICKER=drawn"
  "each|examples/each_demo|each_demo/spec_each_demo|"
  "listbox|examples/listbox_demo|listbox_demo/spec_listbox_demo|"
  "table|examples/table_demo|table_demo/spec_table_demo|"
  "split|examples/split_demo|split_demo/spec_split_demo|"
  "bindings|examples/bindings_demo|bindings_demo/spec_bindings_demo|"
  "tabs|examples/tabs_demo|tabs_demo/spec_tabs_demo|"
  "menu|examples/menu|menu/spec_menu|"
  "rbind|examples/rbind_demo|rbind_demo/spec_rbind_demo|"
  "typo|examples/typo_demo|typo_demo/spec_typo_demo|"
  "multiselect|examples/multiselect_demo|multiselect_demo/spec_multiselect_demo|"
  "dblclick|examples/dblclick_demo|dblclick_demo/spec_dblclick_demo|"
  "tree|examples/tree_demo|tree_demo/spec_tree_demo|"
  "tabledeleg|examples/tabledeleg_demo|tabledeleg_demo/spec_tabledeleg_demo|"
  "weightclamp|examples/weightclamp_demo|weightclamp_demo/spec_weightclamp_demo|"
  "shortcut|examples/shortcut_demo|shortcut_demo/spec_shortcut_demo|"
  "polish|examples/polish_demo|polish_demo/spec_polish_demo|"
  "vlist|examples/vlist_demo|vlist_demo/spec_vlist_demo|"
  "wshortcut|examples/wshortcut_demo|wshortcut_demo/spec_wshortcut_demo|"
  "multiwindow|examples/multiwindow_demo|multiwindow_demo/spec_multiwindow_demo|"
  "winmenu|examples/winmenu_demo|winmenu_demo/spec_winmenu_demo|"
  "reorder|examples/reorder_demo|reorder_demo/spec_reorder_demo|"
  "overlaytr|examples/overlaytr_demo|overlaytr_demo/spec_overlaytr_demo|"
  "a11y|examples/a11y_demo|a11y_demo/spec_a11y_demo|"
  "material|examples/material_demo|material_demo/spec_material_demo|"
  "themes|examples/themes_demo|themes_demo/spec_themes_demo|"
  "csssem|examples/csssem_demo|csssem_demo/spec_csssem_demo|"
  "zen|examples/zen_demo|zen_demo/spec_zen_demo|"
  "states|examples/states_demo|states_demo/spec_states_demo|"
  "undo|examples/undo_demo|undo_demo/spec_undo_demo|"
  "textpath|examples/textpath_demo|textpath_demo/spec_textpath_demo|"
  "pills|examples/pills_demo|pills_demo/spec_pills_demo|"
  "icons|examples/icons_demo|icons_demo/spec_icons_demo|"
  "disclosure|examples/disclosure_demo|disclosure_demo/spec_disclosure_demo|"
  "stroker|examples/stroker_demo|stroker_demo/spec_stroker_demo|"
  "vg3d|examples/vg3d_demo|vg3d_demo/spec_vg3d_demo|"
  "golden|examples/golden_gallery|golden_gallery/spec_golden_gallery|"
  "falling_blocks|apps/falling_blocks|falling_blocks/spec_falling_blocks|"
  "svg_tetris|apps/svg_tetris|svg_tetris/spec_svg_tetris|"
  "rubiks_cube|apps/rubiks_cube|rubiks_cube/spec_rubiks_cube|"
  "tumbling_cube|apps/tumbling_cube|tumbling_cube/spec_tumbling_cube|AEVG_FREEZE=1"
  "font_picker|apps/font_picker|font_picker/spec_font_picker|"
  "lismusic|apps/LisMusic|LisMusic/spec_lismusic|LIS_OFFLINE=1"
)

# grand_perspective: one spec per component, each against a FRESH app scanning
# a FRESH fixture — the fileops spec really trashes a file, so isolation is what
# makes these order-independent. Fixture lives under $HOME: `gio trash` refuses
# /tmp on some systems.
GP_SPECS=(scan_and_list map_nav legend fileops hover_and_resize)

WANT=("$@")
want_suite() {
    [ ${#WANT[@]} -eq 0 ] && return 0
    for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done
    return 1
}

# Wait for the port to stop answering — a lingering app would let the NEXT
# suite interrogate the PREVIOUS app's widget tree, which produces a whole
# family of impossible failures.
# App startup budget: $STARTUP_TRIES * 0.25s. 10s is plenty on Linux, but a
# macOS app on a VM guest can spend ~5s in CoreText font enumeration BEFORE
# the driver binds, then needs the first window render — apps were reported
# "DID NOT START" when they were merely slow. 30s everywhere; the loop still
# exits early the moment /widgets answers, so this costs nothing when fast.
STARTUP_TRIES=${STARTUP_TRIES:-120}

port_free() {
    for _ in $(seq 1 $STARTUP_TRIES); do
        curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets" || return 0
        sleep 0.25
    done
    return 1
}

teardown() {
    local pid="$1" bin="$2"
    curl -s -o /dev/null --max-time 2 -X POST "http://127.0.0.1:$PORT/shutdown" || true
    port_free || { kill "$pid" 2>/dev/null; sleep 0.5; kill -9 "$pid" 2>/dev/null; }
    # Belt-and-suspenders EVERYWHERE, not just FreeBSD: killing $pid only
    # reaps the direct child. A demo that holds a native modal (winmenu) or
    # re-execs outlives it, keeps $PORT bound, and every LATER suite then
    # aborts with "port still busy" — silently truncating the run. Match the
    # basename exactly (-x, not -f) so this never matches our own shell.
    local base; base="$(basename "$bin")"
    port_free || pkill -x "$base" 2>/dev/null
    wait "$pid" 2>/dev/null
    # pkill only SENDS the signal. Returning here while the process is still
    # dying lets the NEXT suite launch a binary with the same name into the
    # window where a late SIGTERM is still in flight — the new app binds the
    # port, prints "listening", then dies, and the run reports "APP DID NOT
    # START". Observed ~4-in-5 on the macOS VM. Wait for the name to actually
    # clear before handing the port on.
    for _ in $(seq 1 40); do
        pgrep -x "$base" >/dev/null 2>&1 || break
        sleep 0.25
    done
}

printf "%-14s %6s %6s   %s\n" SUITE PASS FAIL RESULT
printf -- "------------------------------------------------------------\n"

TOTAL_PASS=0 TOTAL_FAIL=0 SUITES_RED=0

for row in "${SUITES[@]}"; do
    IFS='|' read -r name appdir spec extra <<< "$row"
    want_suite "$name" || continue

    base="$(basename "$appdir")"
    bin="target/build/$appdir/bin/$base"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            # aeb's fan-out can't build UI apps on MSYS yet (the
            # _orchestrator.c generation bug) — build.sh into build/ is
            # the Windows path, building on demand.
            bin="build/$base.exe"
            if [ ! -x "$bin" ]; then
                ./build.sh "$appdir/$base.ae" "$base" \
                    > "/tmp/smx_$base.log" 2>&1 || true
            fi
            ;;
        FreeBSD)
            # No aeb on the box → prefer the fan-out artifact if present,
            # else build.sh into build/ on demand. Absolutized for safety.
            if [ ! -x "$bin" ]; then
                bin="build/$base"
                [ -x "$bin" ] || ./build.sh "$appdir/$base.ae" "$base" \
                    > "/tmp/smx_$base.log" 2>&1 || true
            fi
            [ -x "$bin" ] && bin="$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"
            ;;
    esac
    if [ ! -x "$bin" ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "NO BINARY ($bin) — run: aeb .all.ae (or see /tmp/smx_*.log on Windows)"
        SUITES_RED=$((SUITES_RED + 1))
        continue
    fi

    port_free || { echo "port $PORT still busy; aborting"; exit 1; }

    # Launch, waiting for the driver to answer. RETRIED, because a launch can
    # fail for reasons that have nothing to do with the app: macOS rate-limits
    # rapid relaunches of the same binary (every failing log carries
    # "NSXPCSharedListener ... Connection interrupted", and the app binds the
    # port, prints "listening", then dies). Measured on the .160 VM: 2-in-4
    # BARE launches fail with no test harness involved, and a 60s pause always
    # clears it. Backing off and retrying turns a spurious red into a real
    # result; a suite that is genuinely broken still fails all attempts.
    ready=0; log=""; pid=""
    for attempt in 1 2 3; do
        log="$(mktemp)"
        # shellcheck disable=SC2086
        aui_launch AETHER_UI_TEST_PORT=$PORT $extra -- "$bin" >"$log" 2>&1 &
        pid=$!
        for _ in $(seq 1 $STARTUP_TRIES); do
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        [ "$ready" -eq 1 ] && break
        kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        pkill -x "$(basename "$bin")" 2>/dev/null
        [ "$attempt" -lt 3 ] && sleep $((attempt * 5))
    done
    if [ "$ready" -ne 1 ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "APP DID NOT START after 3 tries (see $log)"
        SUITES_RED=$((SUITES_RED + 1))
        kill "$pid" 2>/dev/null
        pkill -x "$(basename "$bin")" 2>/dev/null
        continue
    fi

    out="$(UI_SPEC="$spec" tests/run_spec.sh 2>&1)"
    rc=$?
    teardown "$pid" "$bin"

    pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
    fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
    pass=${pass:-0}; fail=${fail:-0}

    retried=""
    if [ "$rc" -ne 0 ] && [ "$pass" = "0" ] && [ "$fail" = "0" ]; then
        # The SPEC PROGRAM died before reporting anything — a launch-layer
        # failure (`ae run` fresh-exe exec killed under MSYS subprocess
        # pressure; seen as "Program crashed (signal 1)"), not an assertion
        # outcome. Retry ONCE with a fresh app. Assertion failures (fail>0)
        # are never retried — only the nothing-ran class is.
        sleep 2
        port_free || { echo "port $PORT still busy; aborting"; exit 1; }
        # shellcheck disable=SC2086
        aui_launch AETHER_UI_TEST_PORT=$PORT $extra -- "$bin" >"$log" 2>&1 &
        pid=$!
        ready=0
        for _ in $(seq 1 $STARTUP_TRIES); do
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        if [ "$ready" -eq 1 ]; then
            # Virgin cache for the retry: the first crash may have PINNED a
            # bad exe under ~/.aether/cache (the poisoned-publish shape) —
            # rerunning it would fail deterministically. A throwaway
            # AETHER_CACHE_DIR forces a fresh compile+exec.
            rcache="$(mktemp -d)"
            out="$(AETHER_CACHE_DIR="$rcache" UI_SPEC="$spec" tests/run_spec.sh 2>&1)"
            rc=$?
            rm -rf "$rcache"
            retried=" (retried)"
        fi
        teardown "$pid" "$bin"
        pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
        fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
        pass=${pass:-0}; fail=${fail:-0}
    fi

    if [ "$rc" -ne 0 ] && [ "$pass" = "0" ] && [ "$fail" = "0" ]; then
        printf "%-14s %6s %6s   %s\n" "$name" - - "SPEC ERROR"
        printf '%s\n' "$out" | tail -4 | sed 's/^/                              | /'
        SUITES_RED=$((SUITES_RED + 1))
        rm -f "$log"
        continue
    fi

    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    if [ "$fail" -gt 0 ]; then
        SUITES_RED=$((SUITES_RED + 1))
        printf "%-14s %6s %6s   %s\n" "$name" "$pass" "$fail" "RED"
        printf '%s\n' "$out" | grep -E '✗|FAIL|not ok' | head -6 \
            | sed 's/^/                              | /'
    else
        printf "%-14s %6s %6s   %s\n" "$name" "$pass" "$fail" "green$retried"
    fi
    rm -f "$log"
done

# --- grand_perspective (the real vg app: canvas hit-testing, resize, fileops) --
GP_BIN="target/build/apps/grand_perspective/bin/grand_perspective"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        GP_BIN="build/grand_perspective.exe"
        if [ ! -x "$GP_BIN" ]; then
            ./build.sh apps/grand_perspective/grand_perspective.ae \
                grand_perspective > /tmp/smx_gp.log 2>&1 || true
        fi
        ;;
    FreeBSD)
        if [ ! -x "$GP_BIN" ]; then
            GP_BIN="build/grand_perspective"
            [ -x "$GP_BIN" ] || ./build.sh apps/grand_perspective/grand_perspective.ae \
                grand_perspective > /tmp/smx_gp.log 2>&1 || true
        fi
        [ -x "$GP_BIN" ] && GP_BIN="$(cd "$(dirname "$GP_BIN")" && pwd)/$(basename "$GP_BIN")"
        ;;
esac
for gp in "${GP_SPECS[@]}"; do
    want_suite "gp_$gp" || continue
    if [ ! -x "$GP_BIN" ]; then
        printf "%-14s %6s %6s   %s\n" "gp_$gp" - - "NO BINARY — run: aeb .all.ae"
        SUITES_RED=$((SUITES_RED + 1))
        continue
    fi
    port_free || { echo "port $PORT still busy; aborting"; exit 1; }

    fix="$(mktemp -d "$HOME/.gp-ci-XXXXXX")"
    mkdir -p "$fix/sub"
    head -c 400000 /dev/urandom > "$fix/big.bin"
    head -c 250000 /dev/urandom > "$fix/mid.bin"
    head -c 200000 /dev/urandom > "$fix/sub/inner.bin"
    # The app and the spec are NATIVE binaries — hand them a native path,
    # not an MSYS one (/c/Users/... opens as no-such-dir in the Windows CRT
    # and gp scans nothing).
    fix_app="$fix"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) fix_app="$(cygpath -m "$fix")" ;;
    esac

    log="$(mktemp)"
    aui_launch AETHER_UI_TEST_PORT=$PORT AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
        -- "$GP_BIN" >"$log" 2>&1 &
    pid=$!
    ready=0
    for _ in $(seq 1 $STARTUP_TRIES); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
    if [ "$ready" -ne 1 ]; then
        printf "%-14s %6s %6s   %s\n" "gp_$gp" - - "APP DID NOT START (see $log)"
        SUITES_RED=$((SUITES_RED + 1))
        kill "$pid" 2>/dev/null
        [ "$FREEBSD" -eq 1 ] && pkill -f "$GP_BIN" 2>/dev/null
        rm -rf "$fix"
        continue
    fi

    out="$(AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" UI_SPEC="grand_perspective/spec_${gp}" \
           tests/run_spec.sh 2>&1)"
    teardown "$pid" "$GP_BIN"

    pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
    fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
    pass=${pass:-0}; fail=${fail:-0}
    retried=""
    if [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; then
        # Nothing ran — the spec-launch flake (same class as the widget
        # loop's retry). The spec never touched the fixture, so it's still
        # pristine: relaunch the app against it and retry once.
        sleep 2
        port_free || { echo "port $PORT still busy; aborting"; exit 1; }
        aui_launch AETHER_UI_TEST_PORT=$PORT AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
            -- "$GP_BIN" >"$log" 2>&1 &
        pid=$!
        ready=0
        for _ in $(seq 1 $STARTUP_TRIES); do
            if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/widgets"; then ready=1; break; fi
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        if [ "$ready" -eq 1 ]; then
            # Virgin cache — see the widget loop's retry for why.
            rcache="$(mktemp -d)"
            out="$(AETHER_CACHE_DIR="$rcache" AEVG_DIR="$fix_app" GP_FIXTURE="$fix_app" \
                   UI_SPEC="grand_perspective/spec_${gp}" tests/run_spec.sh 2>&1)"
            rm -rf "$rcache"
            retried=" (retried)"
        fi
        teardown "$pid" "$GP_BIN"
        pass=$(printf '%s' "$out" | grep -oE '[0-9]+ passing' | grep -oE '[0-9]+' | head -1)
        fail=$(printf '%s' "$out" | grep -oE '[0-9]+ failing' | grep -oE '[0-9]+' | head -1)
        pass=${pass:-0}; fail=${fail:-0}
    fi
    rm -rf "$fix" "$log"
    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail))
    if [ "$fail" -gt 0 ] || { [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; }; then
        SUITES_RED=$((SUITES_RED + 1))
        printf "%-14s %6s %6s   %s\n" "gp_$gp" "$pass" "$fail" "RED"
        printf '%s\n' "$out" | grep -E '✗|FAIL|not ok' | head -6 \
            | sed 's/^/                              | /'
    else
        printf "%-14s %6s %6s   %s\n" "gp_$gp" "$pass" "$fail" "green$retried"
    fi
done

printf -- "------------------------------------------------------------\n"
printf "%-14s %6s %6s   %s\n" TOTAL "$TOTAL_PASS" "$TOTAL_FAIL" \
    "$([ "$SUITES_RED" -eq 0 ] && echo "all green" || echo "$SUITES_RED suite(s) red")"
[ "$SUITES_RED" -eq 0 ]
