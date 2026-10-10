#!/usr/bin/env bash
# test_tween.sh — drives transitions_demo (launched by ci with animations ON).
# Proves ui.transition tweens: a screenshot taken right after the toggle
# differs from the settled one (mid-flight frame), and end states land.
# Shell (not Aeocha): the proof is PNG byte comparison, which the Aether
# spec's string plumbing can't carry. Precedent: tests/test_driver.sh.
set -u
PORT="${1:-9222}"
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

BTN=$(curl -s -m 5 "$BASE/widgets?type=button" | tr '}' '\n' | grep '"text":"Toggle fade"' | grep -oE '"id":[0-9]+' | head -1 | cut -d: -f2)
[ -n "$BTN" ] && ok "Toggle button found (id $BTN)" || bad "Toggle button found"

curl -s -m 5 "$BASE/screenshot" -o /tmp/tw_before.png
curl -sf -m 5 -X POST "$BASE/widget/$BTN/click" > /dev/null
curl -s -m 5 "$BASE/screenshot" -o /tmp/tw_mid.png       # inside the 1200ms tween
sleep 1.8
curl -s -m 5 "$BASE/screenshot" -o /tmp/tw_after.png     # settled

M1=$(md5sum /tmp/tw_before.png | cut -d' ' -f1)
M2=$(md5sum /tmp/tw_mid.png | cut -d' ' -f1)
M3=$(md5sum /tmp/tw_after.png | cut -d' ' -f1)
[ "$M2" != "$M3" ] && ok "mid-flight frame differs from settled (a real tween)" \
                   || bad "mid-flight frame differs from settled (snapped?)"
[ "$M1" != "$M3" ] && ok "settled faded state differs from opaque" \
                   || bad "settled faded state differs from opaque"
curl -s -m 5 "$BASE/widgets" | grep -q "state: faded" && ok "end state text: faded" \
                                                      || bad "end state text: faded"

curl -sf -m 5 -X POST "$BASE/widget/$BTN/click" > /dev/null
sleep 1.8
curl -s -m 5 "$BASE/widgets" | grep -q "state: opaque" && ok "toggle back settles opaque" \
                                                       || bad "toggle back settles opaque"

echo
if [ "$FAIL" -eq 0 ]; then echo "transitions: all $PASS passed"; exit 0
else echo "transitions: $FAIL failed"; exit 1; fi
