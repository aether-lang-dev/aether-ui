#!/usr/bin/env bash
# Stage 2.5 retained-compositor regression: dirty clips are only correct if
# the rendered surface outside the clip remains intact.
set -euo pipefail

PORT="${1:-9222}"
BASE="http://127.0.0.1:${PORT}"
SS="/tmp/frames_stage25_surface.png"

get_json() { curl -sf -m 5 "$BASE$1"; }
post() { curl -sf -m 5 -X POST "$BASE$1" >/dev/null; }
trap 'curl -sf -m 2 -X POST "$BASE/canvas/1/release?x=140&y=100" >/dev/null 2>&1 || true' EXIT

field() {
    python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"
}

sleep 0.25
before="$(get_json /canvas/1/debug)"
before_clip="$(printf '%s' "$before" | field clip_paints)"
post "/canvas/1/click?x=80&y=70"
post "/canvas/1/move?x=140&y=100"

clip_area=0
full=0
for _ in $(seq 1 40); do
    dbg="$(get_json /canvas/1/debug)"
    clip_paints="$(printf '%s' "$dbg" | field clip_paints)"
    clip_area="$(printf '%s' "$dbg" | field last_clip_area)"
    w="$(printf '%s' "$dbg" | field w)"
    h="$(printf '%s' "$dbg" | field h)"
    full=$((w * h))
    if [ "$clip_paints" -gt "$before_clip" ] && [ "$clip_area" -gt 0 ] && [ "$clip_area" -lt "$full" ]; then
        break
    fi
    sleep 0.025
done

if [ "${clip_paints:-0}" -le "$before_clip" ] || [ "$clip_area" -le 0 ] || [ "$clip_area" -ge "$full" ]; then
    echo "FAIL: expected clipped drag paint below full canvas, got clip_paints=${clip_paints:-0} before=$before_clip last_clip_area=$clip_area full=$full"
    exit 1
fi

widgets="$(get_json /widgets)"
canvas_xy="$(python3 - "$widgets" <<'PY'
import json
import sys

for w in json.loads(sys.argv[1]):
    if w.get("type") == "canvas":
        print(f'{int(w.get("x", 0))} {int(w.get("y", 0))}')
        break
else:
    raise SystemExit("FAIL: no canvas widget in /widgets")
PY
)"
read -r canvas_x canvas_y <<EOF
$canvas_xy
EOF

curl -sf -m 5 "$BASE/screenshot" -o "$SS"
python3 - "$SS" "$canvas_x" "$canvas_y" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
sx = int(sys.argv[2]) + 400
sy = int(sys.argv[3]) + 180

data = open(path, "rb").read()
if not data.startswith(b"\x89PNG\r\n\x1a\n"):
    raise SystemExit("FAIL: /screenshot did not return a PNG")

pos = 8
width = height = color_type = bit_depth = None
idat = []
while pos + 8 <= len(data):
    n = struct.unpack(">I", data[pos:pos+4])[0]
    typ = data[pos+4:pos+8]
    chunk = data[pos+8:pos+8+n]
    pos += 12 + n
    if typ == b"IHDR":
        width, height, bit_depth, color_type, _comp, _flt, _interlace = struct.unpack(">IIBBBBB", chunk)
    elif typ == b"IDAT":
        idat.append(chunk)
    elif typ == b"IEND":
        break

if bit_depth != 8 or color_type not in (2, 6):
    raise SystemExit(f"FAIL: unsupported PNG format bit_depth={bit_depth} color_type={color_type}")
if sx < 0 or sy < 0 or sx >= width or sy >= height:
    raise SystemExit(f"FAIL: sample {sx},{sy} outside screenshot {width}x{height}")

channels = 4 if color_type == 6 else 3
stride = width * channels
raw = zlib.decompress(b"".join(idat))
rows = []
prev = bytearray(stride)
i = 0
for _y in range(height):
    filt = raw[i]
    i += 1
    cur = bytearray(raw[i:i+stride])
    i += stride
    for x in range(stride):
        left = cur[x - channels] if x >= channels else 0
        up = prev[x]
        up_left = prev[x - channels] if x >= channels else 0
        if filt == 1:
            cur[x] = (cur[x] + left) & 255
        elif filt == 2:
            cur[x] = (cur[x] + up) & 255
        elif filt == 3:
            cur[x] = (cur[x] + ((left + up) >> 1)) & 255
        elif filt == 4:
            p = left + up - up_left
            pa = abs(p - left)
            pb = abs(p - up)
            pc = abs(p - up_left)
            pr = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
            cur[x] = (cur[x] + pr) & 255
        elif filt != 0:
            raise SystemExit(f"FAIL: unsupported PNG filter {filt}")
    rows.append(cur)
    prev = cur

off = sx * channels
r, g, b = rows[sy][off], rows[sy][off + 1], rows[sy][off + 2]
if r > 240 and g > 240 and b > 240:
    raise SystemExit(f"FAIL: far frame interior went white at canvas 400,180: rgb={r},{g},{b}")
if not (r < 90 and g < 110 and b < 130):
    raise SystemExit(f"FAIL: far frame interior did not look rendered at canvas 400,180: rgb={r},{g},{b}")
print(f"PASS: clipped area preserved far frame pixel rgb={r},{g},{b}")
PY

post "/canvas/1/release?x=140&y=100"
trap - EXIT
