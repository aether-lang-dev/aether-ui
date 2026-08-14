#!/bin/bash
# test_easing_curve.sh — proves the easing CURVE is honoured, not just that
# *a* tween happens.
#
# tests/test_tween.sh already shows a mid-flight frame differs from the settled
# one. That passes for ANY curve, including none-with-a-delay, so it cannot
# tell "ease_out" from "linear" — and until 2026-08-14 the easing argument
# reached only macOS: GTK4 hardcoded `ease-out` in its CSS and win32 had no
# easing code at all. This test is what makes that difference observable.
#
# WHY PIXELS: the driver reports the MODEL opacity, which jumps to its target
# the instant style_opacity is called (measured: 0.15 at every sample through
# a 1200ms tween). Only the presentation layer animates — on GTK4 via CSS, on
# macOS via CoreAnimation, both invisible to any property readback. So the
# curve has to be read out of the framebuffer.
#
# THE DISCRIMINATOR: sample screen brightness at 25% and 50% through a fade.
# For a fade from 1.0 to 0.15:
#   linear    is ~halfway faded at the halfway point
#   ease_out  is MOSTLY faded by then — it moves fastest at the start
# so the ratio (progress at 25%) / (progress at 50%) separates them: near 0.5
# for linear, well above it for ease-out. A snap gives 1.0 at both samples.
#
# ENVIRONMENT: needs a REAL framebuffer. /screenshot returns "screenshot
# capture failed" under a bare `xvfb-run -a`, and the pre-existing
# test_tween.sh degrades the same way on the same box -- so a failure here
# with zero-byte PNGs means no display, not a broken curve. ci.sh launches
# with `xvfb-run -a -s "-screen 0 3200x2000x24"`, which is where both of
# these are meant to run.
#
#   usage: test_easing_curve.sh [port]
set -u
PORT="${1:-9222}"
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

# Mean ink of a screenshot: how much dark text is on screen. As the label
# fades toward the background this falls, so it is a direct proxy for the
# animated opacity — no per-widget probe needed.
ink() {
  python3 - "$1" <<'PY'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert("L"), dtype=float)
print(f"{(255.0 - a).mean():.4f}")
PY
}

BTN=$(curl -s -m 5 "$BASE/widgets" | tr '}' '\n' | grep '"text":"Toggle fade"' \
      | grep -oE '"id":[0-9]+' | head -1 | cut -d: -f2)
[ -n "$BTN" ] && ok "Toggle button found (id $BTN)" || { bad "Toggle button found"; exit 1; }

curl -s -m 5 "$BASE/screenshot" -o /tmp/ec_start.png
if ! python3 -c "from PIL import Image; Image.open('/tmp/ec_start.png')" 2>/dev/null; then
    echo "  SKIP: /screenshot is unavailable here (no framebuffer) — the curve"
    echo "        can only be read out of pixels, so there is nothing to assert."
    echo "        Run under ci.sh, which starts a sized Xvfb screen."
    exit 0
fi
I_START=$(ink /tmp/ec_start.png)

curl -sf -m 5 -X POST "$BASE/widget/$BTN/click" > /dev/null
sleep 0.3                                   # 25% through the 1200ms tween
curl -s -m 5 "$BASE/screenshot" -o /tmp/ec_q1.png
sleep 0.3                                   # 50%
curl -s -m 5 "$BASE/screenshot" -o /tmp/ec_q2.png
sleep 1.2                                   # settled
curl -s -m 5 "$BASE/screenshot" -o /tmp/ec_end.png

I_Q1=$(ink /tmp/ec_q1.png); I_Q2=$(ink /tmp/ec_q2.png); I_END=$(ink /tmp/ec_end.png)
echo "  ink: start=$I_START  25%=$I_Q1  50%=$I_Q2  settled=$I_END"

# A tween happened at all: the settled frame is lighter than the start.
python3 -c "import sys; sys.exit(0 if $I_END < $I_START - 0.01 else 1)" \
  && ok "the label faded (settled is lighter than start)" \
  || bad "the label faded"

# Progress at each sample, as a fraction of the total change.
P1=$(python3 -c "d=$I_START-$I_END; print(f'{(($I_START-$I_Q1)/d) if d>0.001 else 0:.3f}')")
P2=$(python3 -c "d=$I_START-$I_END; print(f'{(($I_START-$I_Q2)/d) if d>0.001 else 0:.3f}')")
echo "  progress: 25%->$P1  50%->$P2"

# NOT A SNAP: if the change were instant both samples would read ~1.0.
python3 -c "import sys; sys.exit(0 if $P1 < 0.97 else 1)" \
  && ok "not a snap (25% sample is mid-flight, not complete)" \
  || bad "not a snap — the 25% sample is already finished"

# EASE-OUT: fastest at the start, so it is well past halfway by the midpoint.
# Linear would sit at ~0.5 here. Threshold 0.55 leaves room for sampling
# jitter while still excluding a linear curve.
python3 -c "import sys; sys.exit(0 if $P2 > 0.55 else 1)" \
  && ok "ease_out curve (past halfway at the midpoint, not linear)" \
  || bad "ease_out curve — progress at the midpoint reads linear or slower"

echo
if [ "$FAIL" -eq 0 ]; then echo "easing curve: all $PASS passed"; exit 0
else echo "easing curve: $FAIL failed"; exit 1; fi
