#!/usr/bin/env bash
# tests/test_tray_notif_driver.sh — headless integration test for the
# system-tray + desktop-notification surface.
#
# Builds example_tray, launches it in headless mode with the
# AetherUIDriver enabled, then asserts the new /tray and /notifications
# endpoints over HTTP. Cleans up on exit.
#
# Passes on Linux today; macOS / Windows backends ship registry-only
# stubs (see aether_ui_macos.m + aether_ui_win32.c System tray
# sections) so the same script will work there once their backends
# include their per-backend HTTP server's route additions.

set -u

PORT="${1:-9234}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$ROOT/build"

OS="$(uname -s)"
case "$OS" in
    Linux)
        if ! pkg-config --exists gtk4 2>/dev/null; then
            echo "GTK4 dev libs missing — skipping" ; exit 0
        fi
        ;;
    Darwin|MINGW*|MSYS*|CYGWIN*)
        echo "tray/notif driver test currently Linux-only; exiting 0 on $OS"
        exit 0
        ;;
    *)
        echo "unsupported platform: $OS"; exit 0
        ;;
esac

cd "$ROOT"
./build.sh examples/tray/tray.ae build/example_tray > /tmp/tray_build.log 2>&1 || {
    echo "FAIL: build failed"
    tail -30 /tmp/tray_build.log
    exit 1
}

AETHER_UI_HEADLESS=1 AETHER_UI_TEST_PORT="$PORT" ./build/example_tray \
    > /tmp/tray_app.log 2>&1 &
APP_PID=$!

cleanup() {
    kill "$APP_PID" 2>/dev/null
    wait "$APP_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

CURL() { curl --max-time 5 "$@"; }

# Wait up to 5s for the driver to come up.
UP=0
for _ in $(seq 1 50); do
    if CURL -sf -o /dev/null "http://127.0.0.1:$PORT/widgets"; then
        UP=1; break
    fi
    sleep 0.1
done
if [ "$UP" -ne 1 ]; then
    echo "FAIL: test server never came up"
    tail -30 /tmp/tray_app.log
    exit 1
fi

FAIL=0
pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1 — got: $2"; FAIL=$((FAIL+1)); }

assert_contains() {
    local name="$1" body="$2" needle="$3"
    if [[ "$body" == *"$needle"* ]]; then pass "$name"; else fail "$name" "$body"; fi
}

# --- /tray ---
BODY=$(CURL -s "http://127.0.0.1:$PORT/tray")
assert_contains "/tray returns the avnsync record" "$BODY" '"name":"avnsync"'
assert_contains "/tray includes the tooltip"       "$BODY" '"tooltip":"AvnSync — synced"'
assert_contains "/tray references the menu_handle" "$BODY" '"menu_handle":'

# Resolve tray id from JSON.
TRAY_ID=$(echo "$BODY" | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)
[ -n "$TRAY_ID" ] && pass "tray id resolved ($TRAY_ID)" || fail "tray id" "$BODY"

# --- GET /tray/{id}/icon — current icon resolves the state cell ---
ICON=$(CURL -s "http://127.0.0.1:$PORT/tray/$TRAY_ID/icon")
assert_contains "tray icon path includes clean.svg" "$ICON" "clean.svg"

# --- POST /tray/{id}/click — fires the left-click handler, which
# pushes another notification onto the registry (see examples/tray/tray.ae). ---
PRE=$(CURL -s "http://127.0.0.1:$PORT/notifications" | grep -o '"id":[0-9]*' | wc -l)
RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$PORT/tray/$TRAY_ID/click")
[ "$RC" = "200" ] && pass "tray click returns 200" || fail "tray click rc" "$RC"
POST=$(CURL -s "http://127.0.0.1:$PORT/notifications" | grep -o '"id":[0-9]*' | wc -l)
[ "$POST" -gt "$PRE" ] && pass "tray click fired callback (notification added)" \
                       || fail "click side-effect" "$PRE -> $POST"

# --- POST /tray/{id}/menu/activate — invoke the "Sync now" item, which
# both transitions the state cell and emits another notification. ---
ICON_BEFORE=$(CURL -s "http://127.0.0.1:$PORT/tray/$TRAY_ID/icon")
RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
    "http://127.0.0.1:$PORT/tray/$TRAY_ID/menu/activate?label=Sync+now")
[ "$RC" = "200" ] && pass "menu activate returns 200" || fail "menu rc" "$RC"
ICON_AFTER=$(CURL -s "http://127.0.0.1:$PORT/tray/$TRAY_ID/icon")
[ "$ICON_BEFORE" != "$ICON_AFTER" ] && pass "menu callback transitioned state (icon swap)" \
                                     || fail "icon swap" "$ICON_BEFORE == $ICON_AFTER"

# --- POST /tray/{id}/menu/activate with unknown label → 404 ---
RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
    "http://127.0.0.1:$PORT/tray/$TRAY_ID/menu/activate?label=Nonexistent")
[ "$RC" = "404" ] && pass "unknown menu item returns 404" || fail "404 rc" "$RC"

# --- POST /tray/{id}/set_tooltip — update tooltip ---
RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
    "http://127.0.0.1:$PORT/tray/$TRAY_ID/set_tooltip?v=AvnSync+busy")
[ "$RC" = "200" ] && pass "set_tooltip returns 200" || fail "set_tooltip rc" "$RC"
BODY=$(CURL -s "http://127.0.0.1:$PORT/tray/$TRAY_ID")
assert_contains "tooltip reflects update" "$BODY" '"tooltip":"AvnSync busy"'

# --- /notifications ---
BODY=$(CURL -s "http://127.0.0.1:$PORT/notifications")
assert_contains "/notifications has the Ready record"  "$BODY" '"body":"Ready"'
assert_contains "/notifications has the conflict tag"  "$BODY" '"tag":"avnsync-conflict-README.md"'

# Resolve the conflict notification's id and click it.
CONF_ID=$(echo "$BODY" | python3 -c "
import sys, json
arr = json.loads(sys.stdin.read())
for n in arr:
    if 'conflict' in n['body']:
        print(n['id']); break
" 2>/dev/null)
if [ -n "$CONF_ID" ]; then
    pass "conflict notification id resolved ($CONF_ID)"
    RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
        "http://127.0.0.1:$PORT/notifications/$CONF_ID/click")
    [ "$RC" = "200" ] && pass "notification click returns 200" || fail "notif click rc" "$RC"

    RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
        "http://127.0.0.1:$PORT/notifications/$CONF_ID/dismiss")
    [ "$RC" = "200" ] && pass "notification dismiss returns 200" || fail "notif dismiss rc" "$RC"

    BODY=$(CURL -s "http://127.0.0.1:$PORT/notifications")
    assert_contains "dismissed flag flipped" "$BODY" '"dismissed":true'
else
    fail "resolve conflict notif id" "$BODY"
fi

# --- Click a notification with NO callback (the "Ready" one) → 204 ---
READY_ID=$(echo "$BODY" | python3 -c "
import sys, json
arr = json.loads(sys.stdin.read())
for n in arr:
    if n['body'] == 'Ready':
        print(n['id']); break
" 2>/dev/null)
if [ -n "$READY_ID" ]; then
    RC=$(CURL -s -o /dev/null -w "%{http_code}" -X POST \
        "http://127.0.0.1:$PORT/notifications/$READY_ID/click")
    [ "$RC" = "204" ] && pass "click on callback-less notif returns 204" \
                       || fail "204 rc" "$RC"
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "tray + notification driver tests: all passed"
    exit 0
fi
echo "tray + notification driver tests: $FAIL failure(s)"
tail -20 /tmp/tray_app.log
exit 1
