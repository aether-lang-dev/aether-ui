#!/usr/bin/env bash
# test_aecs_dark_drawn.sh: an AeCS dark sheet is DRAWN, not just reported
# (asks/aecs-button-fg-and-field-bg-not-drawn.md).
#
# The driver's /widgets reported a button's fg and a text field's bg exactly
# as the sheet asked while the window drew the button's label in the default
# dark colour and the text field and the text area white. A spec reading the
# reported styles is green against that bug, so this one reads PIXELS: it
# decodes the driver's /screenshot and, inside each widget's rectangle, asks
#   - is the ground the sheet's background? (the most common colour)
#   - is there light ink on it, and no dark ink? (the sheet's #e4e4e4 text)
# Then again with each field focused: an edited field is drawn by its field
# editor / text view in edit mode, a second path that must keep the ground.
# Python's zlib is the whole PNG decoder; no imaging library is needed.
#
# Run via ci.sh, or by hand:
#   AETHER_UI_TEST_PORT=9222 ./target/build/examples/aecs_dark_demo/bin/aecs_dark_demo &
#   tests/aecs_dark_demo/test_aecs_dark_drawn.sh 9222
set -uo pipefail

PORT="${1:-9222}"
BASE="http://127.0.0.1:${PORT}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/aecs_dark.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/check.py" <<'PYEOF'
import json, struct, sys, zlib
from collections import Counter

widgets = json.load(open(sys.argv[1]))
raw = open(sys.argv[2], "rb").read()
kinds = sys.argv[3].split(",")
print(sys.argv[4])
focused = "focused" in sys.argv[4]

# --- a minimal PNG decoder: 8-bit RGB / RGBA, non-interlaced -------------
assert raw[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
pos, idat, W, H, ctype = 8, b"", 0, 0, 0
while pos < len(raw):
    n, = struct.unpack(">I", raw[pos:pos + 4]); kind = raw[pos + 4:pos + 8]
    data = raw[pos + 8:pos + 8 + n]; pos += 12 + n
    if kind == b"IHDR":
        W, H, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", data)
        assert depth == 8 and ctype in (2, 6) and interlace == 0, \
            "unsupported PNG (depth %d type %d interlace %d)" % (depth, ctype, interlace)
    elif kind == b"IDAT":
        idat += data
bpp = 4 if ctype == 6 else 3
buf = zlib.decompress(idat)
stride = W * bpp
rows, prev, i = [], bytearray(stride), 0
for _y in range(H):
    f = buf[i]; line = bytearray(buf[i + 1:i + 1 + stride]); i += 1 + stride
    for x in range(stride):
        a = line[x - bpp] if x >= bpp else 0
        b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            line[x] = (line[x] + pr) & 255
    rows.append(line); prev = line

def px(x, y):
    o = x * bpp; r = rows[y]
    return (r[o], r[o + 1], r[o + 2])

def hexrgb(h):
    h = h.lstrip("#"); return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))

# Widget rects are in the window's units (points, dp); the screenshot is in
# device pixels. On the desktops the root fills the capture, so the scale is
# the image's width over the root's (the root at 0,0). On Android the
# capture is the whole
# decor view (status and action bars, margins) and the rects are placed in
# it, so the scale is the device density: the largest standard one that
# keeps the root inside the image AND finds the root's own reported ground
# there (containers were drawn right all along; the root is the ruler, not
# the subject).
root = next(w for w in widgets if w.get("parent") == 0)
cands = sorted({W / float(root["w"]), 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 2.625,
                2.75, 3.0, 3.5, 4.0}, reverse=True)
def root_ground(sc):
    x = int((root["x"] + root["w"] / 2.0) * sc); y = int((root["y"] + root["h"] - 6) * sc)
    return px(x, y) if 0 <= x < W and 0 <= y < H else None
sx = sy = None
if root["x"] == 0 and root["y"] == 0:
    cands = []
    sx = W / float(root["w"]); sy = H / float(root["h"])
for sc in cands:
    if (root["x"] + root["w"]) * sc > W + 1 or (root["y"] + root["h"]) * sc > H + 1:
        continue
    g = root_ground(sc)
    if root.get("bg") is None or (g and all(abs(p - q) <= 10 for p, q in zip(g, hexrgb(root["bg"])))):
        sx = sy = sc; break
assert sx, "could not place the root (%s) in the %dx%d screenshot" % (root, W, H)

def sample(w, inset=4):
    x0 = int((w["x"] + inset) * sx); x1 = min(int((w["x"] + w["w"] - inset) * sx), W)
    y0 = int((w["y"] + inset) * sy); y1 = min(int((w["y"] + w["h"] - inset) * sy), H)
    return [px(x, y) for y in range(y0, y1) for x in range(x0, x1)]

def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000

fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok: fails += 1

SHEET_BG = {"button": "#33373c", "textfield": "#2a2d31", "textarea": "#2a2d31"}
TOL = 10
for kind in kinds:
    w = next((w for w in widgets if w.get("type") == kind), None)
    check(w is not None, "%s: in /widgets" % kind)
    if w is None: continue
    check(w.get("bg") == SHEET_BG[kind] and w.get("fg") == "#e4e4e4",
          "%s: reports the sheet's bg/fg (%s/%s)" % (kind, w.get("bg"), w.get("fg")))
    pix = sample(w)
    check(len(pix) > 0, "%s: has pixels to read (%dx%d at %d,%d)" %
          (kind, w["w"], w["h"], w["x"], w["y"]))
    if not pix: continue
    ground = Counter(pix).most_common(1)[0][0]
    want = hexrgb(SHEET_BG[kind])
    check(all(abs(p - q) <= TOL for p, q in zip(ground, want)),
          "%s: its ground is DRAWN in %s (drew #%02x%02x%02x)" %
          ((kind, SHEET_BG[kind]) + tuple(ground)))
    # Ink: pixels much lighter than the ground (the light label / text), or
    # darker than it (the default dark text the bug drew). A dark default
    # label on #33373c has no light ink at all.
    light = sum(1 for p in pix if lum(p) >= 160 and lum(p) > lum(ground) + 80)
    dark = sum(1 for p in pix if lum(p) < lum(ground) - 20)
    # Focused, a field's text is all selected (a field taking focus selects
    # it), and selected text is drawn in the platform's selection colours,
    # which the sheet does not set: there only the ground, and no default
    # dark text, are the sheet's to prove.
    if not focused:
        check(light >= 12, "%s: its text is DRAWN light (%d light ink pixels)" % (kind, light))
    check(dark <= max(8, light // 4), "%s: not in the default dark (%d dark ink pixels)" % (kind, dark))

sys.exit(1 if fails else 0)
PYEOF

# AECS_SHOTS=<dir> keeps each screenshot read (shot-1.png, ...) to look at.
n_shot=0
shot() {
    curl -sf -m 10 "$BASE/widgets" -o "$TMP/widgets.json" &&
    curl -sf -m 20 "$BASE/screenshot" -o "$TMP/shot.png" || return 1
    n_shot=$((n_shot + 1))
    if [ -n "${AECS_SHOTS:-}" ]; then cp "$TMP/shot.png" "$AECS_SHOTS/shot-$n_shot.png"; fi
}
id_of() {
    python3 -c 'import json, sys
print(next(w["id"] for w in json.load(open(sys.argv[1])) if w.get("type") == sys.argv[2]))' \
        "$TMP/widgets.json" "$1"
}

rc=0
shot || { echo "FAIL: no /widgets or /screenshot"; exit 1; }
python3 "$TMP/check.py" "$TMP/widgets.json" "$TMP/shot.png" \
    button,textfield,textarea "as built:" || rc=1

for kind in textfield textarea; do
    curl -sf -m 5 -X POST "$BASE/widget/$(id_of "$kind")/focus" >/dev/null ||
        { echo "FAIL: could not focus the $kind"; rc=1; continue; }
    sleep 0.3
    shot || { echo "FAIL: no /screenshot"; rc=1; continue; }
    python3 "$TMP/check.py" "$TMP/widgets.json" "$TMP/shot.png" \
        "$kind" "with the $kind focused:" || rc=1
done
exit $rc
