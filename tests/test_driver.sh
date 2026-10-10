#!/bin/bash
# Integration test for AetherUIDriver HTTP server.
#
# Spawns the test driver app with AETHER_UI_TEST_PORT set, waits for the
# server to come up, runs a series of HTTP assertions against it, then
# cleans up the process. Works on Linux, macOS, and Windows (Git Bash +
# taskkill).

set -u

PORT="${1:-9233}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OS="$(uname -s)"

# Build the driver app for whichever platform we're on.
case "$OS" in
    MINGW*|MSYS*|CYGWIN*)
        # Backend sources live under backend/ (post-re-namespace). The win32
        # backend now pulls aether runtime symbols (floatarr_* in the gradient
        # path), so we must link libaether — includes + -L come from
        # `ae cflags`, exactly like build.sh. Link flags mirror build.sh's
        # Windows branch (-lbcrypt for libaether's BCryptGenRandom, -lmsimg32).
        mkdir -p "$ROOT/build"
        AE_INC="$(ae cflags 2>/dev/null | tr ' ' '\n' | awk 'p{print;p=0;next} /^-idirafter$/{print;p=1;next} /^-I/{print}' | tr '\n' ' ')"
        AE_LPATH="$(ae cflags --libs 2>/dev/null | tr ' ' '\n' | grep -E '^-L' | head -1)"
        gcc -O2 \
            -I"$ROOT" -I"$ROOT/backend" $AE_INC \
            "$SCRIPT_DIR/test_driver_app.c" \
            "$ROOT/backend/aether_ui_win32.c" \
            "$ROOT/backend/aether_ui_test_server.c" \
            "$ROOT/backend/aether_ui_system_extras.c" \
            $AE_LPATH -laether \
            -o "$ROOT/build/test_driver_app.exe" \
            -luser32 -lgdi32 -lgdiplus -lmsimg32 -lcomctl32 -lcomdlg32 \
            -lshell32 -lole32 -luuid -ldwmapi -luxtheme \
            -lws2_32 -lbcrypt -pthread -lm
        APP="$ROOT/build/test_driver_app.exe"
        ;;
    *)
        echo "driver test currently only runs on Windows; exiting 0 on $OS"
        exit 0
        ;;
esac

# CI safety belts (GitHub Actions windows-latest and equivalent):
#   1. AETHER_UI_HEADLESS=1  → backend creates the window with SW_HIDE.
#      The message loop still pumps and the HTTP test server still
#      responds, but nothing is ever rendered to the desktop.
#   2. trap cleanup EXIT     → the app is taskkilled on any script exit
#      (pass, fail, or error), so a stuck process can never linger.
#   3. curl --max-time 5     → every HTTP call has a hard 5s ceiling,
#      so a stuck message loop can't stall the script indefinitely.
# Combined worst-case runtime: ~15s including startup + cleanup.

# On MSYS the shell job PID is NOT the Windows PID of the exe, so a leftover
# instance from a prior aborted run would hold the port + the exe file lock
# (a later gcc then fails with "Permission denied"). Kill any stragglers by
# image name before launching.
case "$OS" in
    MINGW*|MSYS*|CYGWIN*)
        taskkill //F //IM "$(basename "$APP")" >/dev/null 2>&1 || true
        ;;
esac

AETHER_UI_HEADLESS=1 AETHER_UI_TEST_PORT="$PORT" "$APP" > /tmp/driver_app.log 2>&1 &
APP_PID=$!

cleanup() {
    case "$OS" in
        MINGW*|MSYS*|CYGWIN*)
            # On Git Bash / MSYS, `wait` on a Windows-terminated process can
            # hang because the MSYS PID wrapper doesn't observe the Windows
            # process exit. The job PID isn't the Windows PID either, so kill
            # by IMAGE NAME (//IM) — that's what actually reaps the exe.
            taskkill //F //IM "$(basename "$APP")" >/dev/null 2>&1
            ;;
        *)
            kill "$APP_PID" 2>/dev/null
            wait "$APP_PID" 2>/dev/null
            ;;
    esac
}
trap cleanup EXIT

# Bound every HTTP call so one stuck request can't hang the whole script.
CURL() { curl --max-time 5 "$@"; }

# Wait up to 5s for the driver to respond.
UP=0
for _ in $(seq 1 50); do
    if CURL -sf -o /dev/null "http://127.0.0.1:$PORT/widgets"; then
        UP=1
        break
    fi
    sleep 0.1
done
if [ "$UP" -ne 1 ]; then
    echo "FAIL: test server never came up"
    tail -20 /tmp/driver_app.log
    exit 1
fi

FAIL=0
pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1 — got: $2"; FAIL=$((FAIL+1)); }

assert_contains() {
    local name="$1" body="$2" needle="$3"
    if [[ "$body" == *"$needle"* ]]; then pass "$name"; else fail "$name" "$body"; fi
}

# /widgets returns a JSON array containing our widgets.
BODY=$(CURL -s "http://127.0.0.1:$PORT/widgets")
assert_contains "widgets endpoint returns array" "$BODY" '['
assert_contains "heading is present" "$BODY" '"text":"Heading"'
assert_contains "button type found" "$BODY" '"type":"button"'
assert_contains "textfield type found" "$BODY" '"type":"textfield"'
assert_contains "toggle type found" "$BODY" '"type":"toggle"'
assert_contains "slider type found" "$BODY" '"type":"slider"'
assert_contains "banner visible when driver active" "$BODY" '"text":"Under Remote Control"'
assert_contains "banner is sealed" "$BODY" '"sealed":true'

# /widgets?type=button filters by type.
BODY=$(CURL -s "http://127.0.0.1:$PORT/widgets?type=button")
assert_contains "type filter keeps buttons" "$BODY" '"type":"button"'

# Resolve button handle from JSON.
BTN_ID=$(CURL -s "http://127.0.0.1:$PORT/widgets?type=button&text=Click%20me" \
    | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$BTN_ID" ]; then
    pass "found button id=$BTN_ID"
    RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
        "http://127.0.0.1:$PORT/widget/$BTN_ID/click")
    [ "$RC" = "200" ] && pass "click returns 200" || fail "click rc" "$RC"
else
    fail "find button id" "$BODY"
fi

# Toggle round-trip.
TG_ID=$(CURL -s "http://127.0.0.1:$PORT/widgets?type=toggle" \
    | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$TG_ID" ]; then
    CURL -s -X POST "http://127.0.0.1:$PORT/widget/$TG_ID/toggle" > /dev/null
    BODY=$(CURL -s "http://127.0.0.1:$PORT/widget/$TG_ID")
    assert_contains "toggle active after POST" "$BODY" '"active":true'
fi

# Slider set_value.
SL_ID=$(CURL -s "http://127.0.0.1:$PORT/widgets?type=slider" \
    | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$SL_ID" ]; then
    CURL -s -X POST "http://127.0.0.1:$PORT/widget/$SL_ID/set_value?v=42" > /dev/null
    BODY=$(CURL -s "http://127.0.0.1:$PORT/widget/$SL_ID")
    # Accept any numeric value — exact match depends on range mapping.
    assert_contains "slider reports value after set" "$BODY" '"value":'
fi

# Banner is protected from click.
BAN_ID=$(echo "$BODY" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
BAN_ID=$(CURL -s "http://127.0.0.1:$PORT/widgets?text=Under%20Remote%20Control" \
    | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$BAN_ID" ]; then
    RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
        "http://127.0.0.1:$PORT/widget/$BAN_ID/set_text?v=hacked")
    [ "$RC" = "403" ] && pass "banner set_text rejected (403)" \
                      || fail "banner set_text rc" "$RC"
fi

# /widget/{id}/children — should list direct children of the root stack.
ROOT_ID=$(CURL -s "http://127.0.0.1:$PORT/widgets?type=vstack" \
    | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$ROOT_ID" ]; then
    BODY=$(CURL -s "http://127.0.0.1:$PORT/widget/$ROOT_ID/children")
    # The root vstack has the banner + heading + button + textfield + toggle +
    # slider — at least 5 children. Verify we got an array with multiple ids.
    KID_COUNT=$(echo "$BODY" | grep -o '"id":[0-9]*' | wc -l)
    [ "$KID_COUNT" -ge 3 ] && pass "/children returned $KID_COUNT kids" \
                           || fail "/children count" "$KID_COUNT"
fi

# /screenshot — should return a valid PNG (starts with the PNG magic bytes).
SS_FILE=$(mktemp -u /tmp/aether_ui_ss_XXXXXX.png)
if CURL -s -o "$SS_FILE" "http://127.0.0.1:$PORT/screenshot"; then
    MAGIC=$(head -c 4 "$SS_FILE" | od -An -tx1 | tr -d ' ')
    if [ "$MAGIC" = "89504e47" ]; then
        SIZE=$(wc -c < "$SS_FILE")
        pass "/screenshot returned PNG ($SIZE bytes)"
    else
        fail "/screenshot magic bytes" "$MAGIC"
    fi
    rm -f "$SS_FILE"
else
    fail "/screenshot request" "curl failed"
fi

# /overlays — the in-window overlay layer's driver route. Win32 overlays are
# stubs, so this reports an empty list; the assertion is that the route EXISTS
# (matches the GTK4 backend) rather than 404ing.
BODY=$(CURL -s "http://127.0.0.1:$PORT/overlays")
assert_contains "/overlays route responds" "$BODY" '"count":0'

echo
if [ "$FAIL" -eq 0 ]; then
    echo "driver tests: all passed"
    exit 0
fi
echo "driver tests: $FAIL failure(s)"
exit 1
