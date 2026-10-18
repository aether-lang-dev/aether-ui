# png_pixel.py FILE.png X Y [X Y ...]: print "r,g,b" for each pixel, in the
# PNG's own pixels. Plain zlib, so a spec needs no imaging library. 8-bit
# RGB/RGBA, not interlaced (what every driver's /screenshot writes).
# Also: png_pixel.py FILE.png width  -> the width in pixels
import struct, sys, zlib

def load(path):
    d = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype, _c, _f, inter = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    assert depth == 8 and inter == 0 and ctype in (2, 6), 'unsupported PNG'
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride, rows, prev = w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: cur[i] = (cur[i] + a) & 255
            elif f == 2: cur[i] = (cur[i] + b) & 255
            elif f == 3: cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        rows.append(cur)
        prev = cur
    return w, h, bpp, rows

w, h, bpp, rows = load(sys.argv[1])
if sys.argv[2] == 'width':
    print(w)
else:
    a = sys.argv[2:]
    for i in range(0, len(a), 2):
        x, y = int(a[i]), int(a[i + 1])
        p = rows[y][x * bpp:x * bpp + 3]
        print('%d,%d,%d' % (p[0], p[1], p[2]))
