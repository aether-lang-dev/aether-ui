#!/bin/bash
# test_spring_curve.sh — proves the SPRING curve overshoots.
#
# tests/transitions_demo/test_easing_curve.sh separates ease-out from linear by
# how fast the curve decelerates. That cannot see a spring: a spring also
# decelerates, so it would pass the ease-out test and tell you nothing new.
#
# THE DISCRIMINATOR IS OVERSHOOT. A spring goes PAST its target and settles
# back; linear and ease-out approach monotonically and never exceed it. So this
# measures PEAK progress: > 1.0 means the label faded further than its own end
# state at some point mid-flight, which no CSS timing function and no ease-out
# can produce. Measured on GTK4: peak 1.10 at ~720ms of a 1200ms tween, against
# a hard ceiling of exactly 1.000 for every monotone curve.
#
# Before 2026-08-14, easing="spring" was ACCEPTED AND IGNORED on all three
# backends -- each did `strstr(t, "linear") ? 0 : 1`, so anything that was not
# "linear" became ease-out. Measured then, "spring" produced an ease-out curve
# with no overshoot at all. This test is what makes that difference visible.
#
# Same harness as the ease-out test, for the same reasons: deadlines computed
# from ONE t0 (a capture costs ~50ms, and sleeping BETWEEN captures drifts the
# samples enough to change the verdict), a blindness guard for uniform
# screenshots, and a dense sweep so no single frame is load-bearing.
#
#   usage: test_spring_curve.sh [port]
set -u
PORT="${1:-9222}"
SCRATCH="${TMPDIR:-/tmp}"
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

BTN=$(curl -s -m 5 "$BASE/widgets" | tr '}' '\n' | grep '"text":"Toggle spring"' \
      | grep -oE '"id":[0-9]+' | head -1 | cut -d: -f2)
[ -n "$BTN" ] && ok "Toggle spring button found (id $BTN)" || { bad "Toggle spring button found"; exit 1; }

curl -s -m 5 "$BASE/screenshot" -o $SCRATCH/sp_start.png
if ! python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1])" "$SCRATCH/sp_start.png" 2>/dev/null; then
    echo "  SKIP: /screenshot returned no image — the app is probably on a"
    echo "        different display than the one being captured."
    exit 0
fi

SPREAD=$(python3 - "$SCRATCH/sp_start.png" <<'PY'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert("L"), dtype=float)
print(f"{a.max() - a.min():.1f}")
PY
)
if python3 -c "import sys; sys.exit(0 if $SPREAD < 1.0 else 1)"; then
    echo "  SKIP: /screenshot is uniform (spread $SPREAD) — it captured the"
    echo "        window background but no child controls, so the label is not"
    echo "        in the picture. A blind instrument, NOT a failed tween."
    exit 0
fi

# 16 samples at 90ms across a 1200ms tween, deadlines from one t0.
python3 - "$BASE" "$BTN" > $SCRATCH/sp_curve.txt <<'PY'
import sys, time, urllib.request
from PIL import Image
import numpy as np
base, btn = sys.argv[1], sys.argv[2]
def ink():
    urllib.request.urlretrieve(base + "/screenshot", "/tmp/_sp.png")
    a = np.asarray(Image.open("/tmp/_sp.png").convert("L"), dtype=float)
    return (255.0 - a).mean()
i0 = ink()
t0 = time.time()
urllib.request.urlopen(urllib.request.Request(base + "/widget/" + btn + "/click",
                                              method="POST")).read()
pts = []
for k in range(16):
    d = t0 + 0.09 * (k + 1) - time.time()
    if d > 0: time.sleep(d)
    pts.append((time.time() - t0, ink()))
print(i0)
for el, v in pts: print(f"{el:.4f} {v:.6f}")
PY

read -r I_START < $SCRATCH/sp_curve.txt
I_END=$(tail -1 $SCRATCH/sp_curve.txt | cut -d' ' -f2)
echo "  ink: start=$I_START settled=$I_END  (16 samples over 1440ms)"

python3 -c "import sys; sys.exit(0 if $I_END < $I_START - 0.01 else 1)" \
  && ok "the label faded (settled is lighter than start)" \
  || bad "the label faded"

# THE SPRING ASSERTION. Peak progress above 1.0 means the tween went PAST its
# own settled value mid-flight. Threshold 1.03 clears sampling noise while
# sitting far below the measured 1.10, and is unreachable for any monotone
# curve -- ease-out and linear top out at exactly 1.000.
python3 - $SCRATCH/sp_curve.txt <<'PY' && ok "spring OVERSHOOTS its target (peak > 1.03; monotone curves cap at 1.0)" || bad "spring curve — no overshoot, so this is an ease-out or linear tween"
import sys
ls = open(sys.argv[1]).read().split("\n")
i0 = float(ls[0])
pts = [tuple(map(float, l.split())) for l in ls[1:] if l.strip()]
end = pts[-1][1]
d = i0 - end
if d <= 0.001:
    print("  no fade to measure")
    sys.exit(1)
peak, at = 0.0, 0.0
for el, v in pts:
    p = (i0 - v) / d
    if p > peak: peak, at = p, el
print(f"  peak progress {peak:.3f} at {at*1000:.0f}ms  (>1.0 == past the target)")
print(f"  (linear and ease_out cannot exceed 1.000)")
sys.exit(0 if peak > 1.03 else 1)
PY

echo
if [ "$FAIL" -eq 0 ]; then echo "spring curve: all $PASS passed"; exit 0
else echo "spring curve: $FAIL failed"; exit 1; fi
