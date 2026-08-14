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
# ENVIRONMENT: the app must be on the display /screenshot captures. Launching
# it under its OWN `xvfb-run` while a real session exists makes the capture
# read a different, blank display and return "screenshot capture failed" --
# a launch mistake, NOT a missing framebuffer (this box has a real display and
# it works there). Run the app on the session display, or let ci.sh's
# run_server_test own the xvfb wrapper.
#
# MEASURED on GTK4: progress 0.440 at 25% and 0.758 at 50%, against the
# ease-out curve's predicted 0.438 and 0.750. Linear would read 0.25/0.50, so
# the two are cleanly separated.
#
# ON WIN32 THIS IS NOT YET MEASURABLE, and the guard below does not catch it.
# The screenshot decodes fine, so the no-image SKIP does not fire, but every
# pixel is the SAME background grey (min == max == 240, zero dark pixels): the
# capture contains no child controls at all, faded or opaque. Constant ink
# across the samples is therefore a BLANK CAPTURE, not a failed tween -- this
# path cannot see the label either way, so it can neither confirm nor refute
# the curve on win32. Two other instruments failed the same way while checking
# this: GetPixel on a Session-0 desktop reads nothing, and PrintWindow returns
# a uniformly black bitmap. Whether win32 child alpha actually animates is
# still OPEN; the overlay exit fade uses the same mechanism on child windows
# and does work, so do not assume it is broken. Needs a capture path that sees
# child controls before this test means anything here.
#
# NB the demo fades over 1200ms, which is luxuriously slow for real UI (150-250ms
# is typical) -- it is long on purpose so a mid-flight frame is easy to sample.
#
#   usage: test_easing_curve.sh [port]
set -u
PORT="${1:-9222}"
# Scratch dir both halves of the toolchain agree on. On MSYS the shell's /tmp
# and a native-Windows python's /tmp are DIFFERENT directories, so curl would
# write a PNG that python then could not find -- which reads as "no image" and
# skips the whole test. TMPDIR (set by MSYS to a real path) avoids the split.
SCRATCH="${TMPDIR:-/tmp}"
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

curl -s -m 5 "$BASE/screenshot" -o $SCRATCH/ec_start.png
if ! python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1])" "$SCRATCH/ec_start.png" 2>/dev/null; then
    echo "  SKIP: /screenshot returned no image — the app is probably on a"
    echo "        different display than the one being captured. The curve can"
    echo "        only be read from pixels, so there is nothing to assert."
    exit 0
fi
# A DECODABLE screenshot is not necessarily a USEFUL one. On win32 the capture
# comes back valid but perfectly uniform -- window background only, no child
# controls -- and a uniform frame can never show a fade. Without this check the
# constant ink reads as "the label did not fade", which is a false accusation
# against the tween: the label was never in the picture. Any real UI frame has
# both light and dark pixels, so a zero spread means the instrument is blind.
SPREAD=$(python3 - "$SCRATCH/ec_start.png" <<'PY'
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
    echo "        in the picture and the curve cannot be read from it. This is"
    echo "        a blind instrument, NOT a failed tween."
    exit 0
fi

# SAMPLE THE WHOLE CURVE, NOT TWO POINTS ON IT.
#
# The first version of this test read progress at 25% and 50% and compared the
# midpoint against a threshold. That is too fragile to be evidence:
#   * each capture costs ~50ms, so sleeping BETWEEN captures pushed the second
#     sample from 0.60s to ~0.65s and dragged a correct ease-out reading from
#     0.71 down to 0.50 -- indistinguishable from linear, and a FAIL;
#   * consecutive captures sometimes return the same frame, which produced two
#     identical readings at different times (0.226 and 0.226) -- a physically
#     impossible curve, and another FAIL.
# Both were the instrument, not the code. A dense sweep plus a shape test over
# all of it is stable because no single frame is load-bearing.
#
# 14 samples at 100ms across a 1200ms tween. Deadlines are computed from ONE t0
# so capture cost cannot accumulate into drift.
python3 - "$BASE" "$BTN" > $SCRATCH/ec_curve.txt <<'PY'
import sys, time, urllib.request
from PIL import Image
import numpy as np
base, btn = sys.argv[1], sys.argv[2]
def ink():
    urllib.request.urlretrieve(base + "/screenshot", "/tmp/_ec.png")
    a = np.asarray(Image.open("/tmp/_ec.png").convert("L"), dtype=float)
    return (255.0 - a).mean()
i0 = ink()
t0 = time.time()
urllib.request.urlopen(urllib.request.Request(base + "/widget/" + btn + "/click",
                                              method="POST")).read()
pts = []
for k in range(14):
    d = t0 + 0.10 * (k + 1) - time.time()
    if d > 0: time.sleep(d)
    pts.append((time.time() - t0, ink()))
print(i0)
for el, v in pts: print(f"{el:.4f} {v:.6f}")
PY

read -r I_START < $SCRATCH/ec_curve.txt
I_END=$(tail -1 $SCRATCH/ec_curve.txt | cut -d' ' -f2)
echo "  ink: start=$I_START settled=$I_END  (14 samples over 1400ms)"

# A tween happened at all: the settled frame is lighter than the start.
python3 -c "import sys; sys.exit(0 if $I_END < $I_START - 0.01 else 1)" \
  && ok "the label faded (settled is lighter than start)" \
  || bad "the label faded"

# NOT A SNAP: an instant change is fully done by the first 100ms sample.
P_FIRST=$(python3 -c "
d=$I_START-$I_END
v=$(sed -n '2p' $SCRATCH/ec_curve.txt | cut -d' ' -f2)
print(f'{($I_START-v)/d if d>0.001 else 0:.3f}')")
python3 -c "import sys; sys.exit(0 if $P_FIRST < 0.97 else 1)" \
  && ok "not a snap (still mid-flight at the first sample, $P_FIRST)" \
  || bad "not a snap — already finished at the first sample"

# THE SHAPE TEST. Ease-out decelerates: every successive 100ms step covers LESS
# ground than the one before. Linear covers the same each step; ease-IN covers
# more. Comparing the first third of the travel against the last third
# discriminates all three without depending on any individual frame landing on
# time, which is what made the old midpoint assertion flaky.
python3 - $SCRATCH/ec_curve.txt <<'PY' && ok "ease_out curve (decelerating: early steps move further than late ones)" || bad "ease_out curve — the curve does not decelerate"
import sys
ls = open(sys.argv[1]).read().split("\n")
i0 = float(ls[0])
pts = [tuple(map(float, l.split())) for l in ls[1:] if l.strip()]
vals = [i0] + [v for _, v in pts]
steps = [vals[i] - vals[i + 1] for i in range(len(vals) - 1)]   # ink lost per step
live = [s for s in steps if s > 1e-6]                            # ignore settled tail
if len(live) < 4:
    print(f"  only {len(live)} moving samples — too few to judge the shape")
    sys.exit(1)
n = max(1, len(live) // 3)
early, late = sum(live[:n]) / n, sum(live[-n:]) / n
print(f"  mean travel per 100ms: early {early:.4f}  late {late:.4f}  ratio {early/late:.2f}x")
print(f"  (linear would be ~1.0x; ease-out > 1; ease-in < 1)")
sys.exit(0 if early > late * 1.5 else 1)
PY

echo
if [ "$FAIL" -eq 0 ]; then echo "easing curve: all $PASS passed"; exit 0
else echo "easing curve: $FAIL failed"; exit 1; fi
