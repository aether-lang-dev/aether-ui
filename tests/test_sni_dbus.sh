#!/usr/bin/env bash
# tests/test_sni_dbus.sh — verify the SNI + DBusMenu surface on the
# session bus. Linux-only. Requires:
#   - a session D-Bus
#   - org.kde.StatusNotifierWatcher running (any modern DE: GNOME
#     with the appindicator extension, KDE Plasma, XFCE+sntray,
#     Cinnamon, Budgie).
#
# Skips with exit 0 when the watcher is absent (e.g. minimal CI
# images, headless server with no DE). The companion
# test_tray_notif_driver.sh covers the headless-CI path.
#
# What we assert by direct D-Bus calls (no UI inspection):
#   - the example_tray process owns org.kde.StatusNotifierItem-<pid>-1
#   - that name appears in the watcher's RegisteredStatusNotifierItems
#   - SNI properties (Title, Status, Menu) read with sensible values
#   - DBusMenu GetLayout returns the three items declared in the .ae
#   - Activate fires the left-click closure (observed via stdout)
#   - Event/clicked on id 1 fires the matching menu_item closure

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OS="$(uname -s)"

[ "$OS" = "Linux" ] || { echo "Linux-only; exit 0 on $OS"; exit 0; }
[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || \
    { echo "no DBUS_SESSION_BUS_ADDRESS — skip"; exit 0; }

# Watcher presence check.
if ! gdbus call --session \
        --dest org.kde.StatusNotifierWatcher \
        --object-path /StatusNotifierWatcher \
        --method org.freedesktop.DBus.Properties.Get \
        org.kde.StatusNotifierWatcher IsStatusNotifierHostRegistered \
        >/dev/null 2>&1; then
    echo "no StatusNotifierWatcher on session bus — skip (need GNOME"
    echo "with appindicator extension, KDE, XFCE+sntray, etc.)"
    exit 0
fi

mkdir -p "$ROOT/build"
cd "$ROOT"
./build.sh examples/tray/tray.ae build/example_tray >/tmp/sni_build.log 2>&1 || {
    echo "FAIL: build failed"; tail -30 /tmp/sni_build.log; exit 1
}

# Reap any orphaned example_tray instances from previous runs so the
# pgrep below resolves uniquely to ours.
pkill -f "build/example_tray" 2>/dev/null
sleep 0.3

LOG=/tmp/sni_app.log
# Line-buffer stdout so the println()s the callback closures emit are
# visible to the post-trigger log greps. Aether's println bottoms out
# in libc puts(), which is block-buffered when stdout is a file.
stdbuf -oL ./build/example_tray >"$LOG" 2>&1 &

cleanup() {
    pkill -f "build/example_tray" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# stdbuf forks before exec; pgrep -f also matches stdbuf's own argv.
# `pgrep -x example_tray` matches by exact comm (the binary's
# basename), which excludes the stdbuf wrapper.
sleep 2
PID=$(pgrep -x example_tray | head -1)
[ -n "$PID" ] || { echo "FAIL: example_tray didn't start"; cat "$LOG"; exit 1; }
NAME="org.kde.StatusNotifierItem-${PID}-1"

FAIL=0
pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1 — $2"; FAIL=$((FAIL+1)); }

# 1. Watcher knows about us.
REG=$(gdbus call --session --dest org.kde.StatusNotifierWatcher \
    --object-path /StatusNotifierWatcher \
    --method org.freedesktop.DBus.Properties.Get \
    org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems 2>&1)
if [[ "$REG" == *"$NAME"* ]]; then
    pass "RegisterStatusNotifierItem succeeded ($NAME)"
else
    fail "RegisterStatusNotifierItem" "$REG"
fi

# 2. SNI props read.
TITLE=$(gdbus call --session --dest "$NAME" --object-path /StatusNotifierItem \
    --method org.freedesktop.DBus.Properties.Get \
    org.kde.StatusNotifierItem Title 2>&1)
[[ "$TITLE" == *"avnsync"* ]] && pass "Title = avnsync" \
                              || fail "Title" "$TITLE"

STATUS=$(gdbus call --session --dest "$NAME" --object-path /StatusNotifierItem \
    --method org.freedesktop.DBus.Properties.Get \
    org.kde.StatusNotifierItem Status 2>&1)
[[ "$STATUS" == *"Active"* ]] && pass "Status = Active" \
                              || fail "Status" "$STATUS"

MENU=$(gdbus call --session --dest "$NAME" --object-path /StatusNotifierItem \
    --method org.freedesktop.DBus.Properties.Get \
    org.kde.StatusNotifierItem Menu 2>&1)
[[ "$MENU" == *"/MenuBar"* ]] && pass "Menu path = /MenuBar" \
                              || fail "Menu" "$MENU"

# 3. DBusMenu layout has all three declared items.
LAYOUT=$(gdbus call --session --dest "$NAME" --object-path /MenuBar \
    --method com.canonical.dbusmenu.GetLayout -- 0 -1 '@as []' 2>&1)
for item in "Sync now" "Open folder" "Quit"; do
    [[ "$LAYOUT" == *"$item"* ]] && pass "GetLayout has '$item'" \
                                 || fail "GetLayout missing '$item'" "$LAYOUT"
done

# 4. Activate → left-click closure fires (prints "Tray left-clicked").
gdbus call --session --dest "$NAME" --object-path /StatusNotifierItem \
    --method org.kde.StatusNotifierItem.Activate -- 100 100 >/dev/null 2>&1
sleep 0.5
grep -q "Tray left-clicked" "$LOG" \
    && pass "Activate fired tray left-click closure" \
    || fail "Activate" "$(tail -20 "$LOG")"

# 5. Event clicked on item id 1 → "Sync now" closure fires.
gdbus call --session --dest "$NAME" --object-path /MenuBar \
    --method com.canonical.dbusmenu.Event -- 1 clicked "<''>" 0 >/dev/null 2>&1
sleep 0.5
grep -q "Sync now clicked" "$LOG" \
    && pass "DBusMenu Event/clicked fired menu_item closure" \
    || fail "Event clicked" "$(tail -20 "$LOG")"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "SNI + DBusMenu tests: all passed"
    exit 0
fi
echo "SNI + DBusMenu tests: $FAIL failure(s)"
echo "--- app stdout/stderr ---"; tail -30 "$LOG"
exit 1
