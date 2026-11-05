# png_contrast.py: luminance statistics of a PNG, in plain zlib, so a spec needs
# neither Pillow nor numpy. 8-bit RGB/RGBA, not interlaced (what every driver's
# /screenshot writes), the same subset as png_pixel.py.
#   png_contrast.py ok FILE.png          exit 0 if FILE decodes, else 1
#   png_contrast.py spread FILE.png      max - min luminance, one decimal
#   png_contrast.py contrast FILE.png... one line per file: mean distance of
#                                        the luminance from its median (the
#                                        "ground"), four decimals
# As a module: contrast(png_bytes), spread(png_bytes). Luminance is the
# ITU-R 601 weighting Pillow's convert("L") uses; a pixel with alpha < 255 is
# composited over white first.
import struct, sys, zlib
from collections import Counter


def histogram(data):
    """Counter {luminance: pixel count} of the PNG bytes in `data`."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat = 8, []
    while pos < len(data):
        n, t = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype, _c, _f, inter = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat.append(body)
        pos += 12 + n
    if depth != 8 or inter != 0 or ctype not in (2, 6):
        raise ValueError('unsupported PNG (need 8-bit RGB/RGBA, not interlaced)')
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    prev = bytearray(stride)
    colours = Counter()
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        elif f != 0:
            raise ValueError('bad PNG filter %d' % f)
        # count distinct colours per row; a UI frame has few
        if bpp == 4:
            colours.update(zip(cur[0::4], cur[1::4], cur[2::4], cur[3::4]))
        else:
            colours.update(zip(cur[0::3], cur[1::3], cur[2::3]))
        prev = cur
    lum = Counter()
    for px, n in colours.items():
        v = (299 * px[0] + 587 * px[1] + 114 * px[2]) // 1000
        if len(px) == 4 and px[3] != 255:
            # AppKit's capture is RGBA with a transparent ground and the
            # label's coverage in alpha alone (RGB stays 0): Pillow's
            # convert("L") would drop alpha and read a faded label as
            # unchanged. Composite over white, the ground a viewer sees.
            v = (v * px[3] + 255 * (255 - px[3])) // 255
        lum[v] += n
    return lum


def spread(data):
    lum = histogram(data)
    return max(lum) - min(lum)


def contrast(data):
    lum = histogram(data)
    total = sum(lum.values())
    seen, median = 0, 0
    for v in sorted(lum):
        seen += lum[v]
        if seen * 2 >= total:
            median = v
            break
    return sum(abs(v - median) * n for v, n in lum.items()) / total


if __name__ == '__main__':
    mode, files = sys.argv[1], sys.argv[2:]
    try:
        if mode == 'ok':
            histogram(open(files[0], 'rb').read())
        elif mode == 'spread':
            print('%.1f' % spread(open(files[0], 'rb').read()))
        elif mode == 'contrast':
            for f in files:
                print('%.4f' % contrast(open(f, 'rb').read()))
        else:
            sys.exit('usage: png_contrast.py ok|spread|contrast FILE.png...')
    except (OSError, ValueError, zlib.error, struct.error) as e:
        if mode == 'ok':
            sys.exit(1)
        sys.exit('png_contrast.py: %s' % e)
