#!/usr/bin/env python3
"""redbox.py PNG BX:BY:BW:RW X:Y:W:H ... -- how many red pixels (the demo's floating box, #d82626)
lie in each box, one count a line. With BOX_COLOR=rrggbb in the environment, the pixels
within 24 of that colour in each channel instead. With BOX_RUN=1, how many pixel rows
of the box hold an unbroken run of dark ink (darker than mid-grey, as ink.py reads it)
across 60% of its width: a line drawn under some text (an underline), which no row
through letters is.

Reads the 8- or 16-bit RGB(A) PNG a /screenshot gives (stdlib only). Boxes are
in points, as /widgets gives them; they become pixels through the driver's red
banner, BW points wide at (BX, BY) as /widgets gives it: where it lies in the
screenshot gives the scale and the offset. Where the banner is not a solid bar
in the picture (Win32's print of it is its red text alone), the screenshot is the
window, RW points wide, from its corner. The banner itself is red: a box meant
to count red must lie below it.
"""
import os
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth in (8, 16) and ctype in (2, 6), "8- or 16-bit RGB or RGBA only"
            # Bytes a pixel takes; a 16-bit sample's high byte is read.
            bpp = (3 if ctype == 2 else 4) * (depth // 8)
            step = depth // 8
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, step, rows


def main():
    w, h, bpp, step, rows = read_png(sys.argv[1])
    bx_, by_, banner_w, root_w = (float(v) for v in sys.argv[2].split(":"))
    red = [(x, y) for y in range(0, min(h, h // 4)) for x in range(0, w, 1)
           if rows[y][x * bpp] > 150 and rows[y][x * bpp + step] < 110 and rows[y][x * bpp + 2 * step] < 110]
    bar = False
    if red:
        bl = min(p[0] for p in red); br_ = max(p[0] for p in red)
        bt = min(p[1] for p in red); bb = max(p[1] for p in red)
        bar = len(red) > 0.5 * (br_ - bl + 1) * (bb - bt + 1)
    if bar:
        scale = (br_ - bl + 1) / banner_w
        ox, oy = bl - bx_ * scale, bt - by_ * scale
    else:
        scale = w / root_w
        ox, oy = 0.0, 0.0
    want = os.environ.get("BOX_COLOR")
    wr, wg, wb = (int(want[i:i + 2], 16) for i in (0, 2, 4)) if want else (0, 0, 0)
    for spec in sys.argv[3:]:
        bx, by, bw, bh = (float(v) for v in spec.split(":"))
        x0, y0 = int(ox + bx * scale), int(oy + by * scale)
        x1, y1 = min(w, x0 + int(bw * scale)), min(h, y0 + int(bh * scale))
        n = 0
        run_rows = os.environ.get("BOX_RUN")
        for y in range(max(0, y0), y1):
            r = rows[y]
            if run_rows:
                run = best = 0
                for x in range(max(0, x0), x1):
                    p = x * bpp
                    run = run + 1 if (r[p] + r[p + step] + r[p + 2 * step]) < 3 * 110 else 0
                    best = max(best, run)
                if best >= 0.6 * (x1 - x0):
                    n += 1
                continue
            for x in range(max(0, x0), x1):
                p = x * bpp
                if want:
                    if abs(r[p] - wr) <= 24 and abs(r[p + step] - wg) <= 24 and abs(r[p + 2 * step] - wb) <= 24:
                        n += 1
                elif r[p] > 200 and r[p + step] < 80 and r[p + 2 * step] < 80:
                    n += 1
        print(n)


main()
