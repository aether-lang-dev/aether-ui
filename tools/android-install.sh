#!/usr/bin/env bash
# tools/android-install.sh — install an APK on the attached device, robustly.
#
#   tools/android-install.sh target/android/counter/counter.apk
#
# `adb install` streams the whole APK in one transfer. On some host/adb
# combinations that transfer kills the adb server once it passes a few
# hundred KB (seen on a macOS beta with a Pixel 6a: ADB_LIBUSB=1 is needed at
# all, and large pushes still die). So this sends the APK in 256 KB pieces
# through `adb exec-in`, checks each piece's size on the device, reassembles
# it there, compares md5s, and installs with `pm install` on the device. Each
# adb call is retried, restarting the adb server when it has died.
#
# Environment: ADB (default adb), ADB_SERIAL (-s), ADB_LIBUSB (exported as 1
# unless set), PIECE_KB (256).
set -euo pipefail

APK="${1:?usage: tools/android-install.sh <app.apk>}"
[ -f "$APK" ] || { echo "error: no such APK: $APK" >&2; exit 1; }
ADB="${ADB:-adb}"
export ADB_LIBUSB="${ADB_LIBUSB:-1}"
PIECE_KB="${PIECE_KB:-256}"
SERIAL_ARGS=()
[ -n "${ADB_SERIAL:-}" ] && SERIAL_ARGS=(-s "$ADB_SERIAL")
REMOTE_DIR=/data/local/tmp/aeui-install
REMOTE_APK=/data/local/tmp/aeui-install.apk

# adb with retries; a dead server is restarted between attempts.
adbr() {
    local attempt
    for attempt in 1 2 3 4 5; do
        if "$ADB" ${SERIAL_ARGS[@]+"${SERIAL_ARGS[@]}"} "$@"; then return 0; fi
        echo "  adb $1 failed (attempt $attempt); restarting the adb server" >&2
        pkill -x adb 2>/dev/null || true
        sleep 1
        "$ADB" start-server > /dev/null 2>&1 || true
        "$ADB" ${SERIAL_ARGS[@]+"${SERIAL_ARGS[@]}"} wait-for-device
    done
    return 1
}

md5_local() { md5 -q "$1" 2>/dev/null || md5sum "$1" | cut -d' ' -f1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
split -b "${PIECE_KB}k" -a 3 "$APK" "$WORK/p"
PIECES=("$WORK"/p*)
echo "Sending $(basename "$APK") ($(wc -c < "$APK" | tr -d ' ') bytes) in ${#PIECES[@]} pieces"

adbr shell "rm -rf $REMOTE_DIR $REMOTE_APK; mkdir -p $REMOTE_DIR"
for piece in "${PIECES[@]}"; do
    name="$(basename "$piece")"
    want="$(wc -c < "$piece" | tr -d ' ')"
    ok=0
    for attempt in 1 2 3 4 5; do
        adbr exec-in "cat > $REMOTE_DIR/$name" < "$piece" || true
        got="$(adbr shell "wc -c < $REMOTE_DIR/$name" 2>/dev/null | tr -d ' \r')"
        if [ "$got" = "$want" ]; then ok=1; break; fi
        echo "  piece $name: $got of $want bytes (attempt $attempt), resending" >&2
    done
    [ "$ok" -eq 1 ] || { echo "error: could not send piece $name" >&2; exit 1; }
done

adbr shell "cat $REMOTE_DIR/p* > $REMOTE_APK && rm -rf $REMOTE_DIR"
want_md5="$(md5_local "$APK")"
got_md5="$(adbr shell "md5sum $REMOTE_APK" | cut -d' ' -f1 | tr -d '\r')"
if [ "$want_md5" != "$got_md5" ]; then
    echo "error: md5 mismatch after reassembly ($got_md5 on device, $want_md5 here)" >&2
    exit 1
fi
echo "Reassembled on the device (md5 $got_md5); installing"
adbr shell "pm install -r $REMOTE_APK; rc=\$?; rm -f $REMOTE_APK; exit \$rc"
