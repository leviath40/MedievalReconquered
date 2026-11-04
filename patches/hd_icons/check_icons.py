"""Find broken HD sprites: shrink each hd_icons/<hash>_<frame>.png back to the sprite's size and compare
it with the original frame. Upscayl sometimes returns garbage (noise, stripes) for very small images.

    python patches\\hd_icons\\check_icons.py [--delete]

Prints the bad files (mean colour error over opaque pixels above a threshold); --delete removes them so
build_icons.py can redo them.
"""
import os, struct, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(GAME, 'tools'))
sys.path.insert(0, HERE)
import bif
from build_icons import sprite_hash, sources, OUT

def read_png(path):
    d = open(path, 'rb').read(); p = 8; idat = b''
    while p < len(d):
        n, t = struct.unpack_from('>I4s', d, p); c = d[p + 8:p + 8 + n]
        if t == b'IHDR': w, h, depth, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT': idat += c
        p += 12 + n
    bpp = {6: 4, 2: 3}[ct]; st = w * bpp; raw = zlib.decompress(idat); rows = []; prev = bytearray(st)
    for y in range(h):
        f = raw[y * (st + 1)]; r = bytearray(raw[y * (st + 1) + 1:(y + 1) * (st + 1)])
        for i in range(st):
            a = r[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 1: r[i] = (r[i] + a) & 255
            elif f == 2: r[i] = (r[i] + b) & 255
            elif f == 3: r[i] = (r[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                r[i] = (r[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(r); prev = r
    return w, h, bpp, rows

def error(path, b, fr):
    w, h, bpp, rows = read_png(path)
    sx, sy = w / b['w'], h / b['h']; tot = n = 0
    for y, r in enumerate(fr):
        for x, p in enumerate(r):
            if p is None or b['pal'][p] == 0xF81F: continue
            e = bif.rgb565(b['pal'][p])
            X, Y = int((x + 0.5) * sx), int((y + 0.5) * sy)
            g = rows[Y][X * bpp:X * bpp + 3]
            tot += sum(abs(e[i] - g[i]) for i in range(3)) / 3; n += 1
    return tot / n if n else 0

if __name__ == '__main__':
    bad = []; seen = set()
    for path, _ in sources(['campmap', 'frontend_files']):
        with open(path, 'rb') as f: hsh = '%08x' % sprite_hash(f.read())
        if hsh in seen: continue
        seen.add(hsh)
        try: b = bif.read(path)
        except Exception: continue
        if max(b['w'], b['h']) >= 200: continue          # 2x outputs: slow to check in pure Python, and not small
        for i, fr in enumerate(b['frames']):
            png = os.path.join(OUT, '%s_%d.png' % (hsh, i))
            if not os.path.exists(png): continue
            e = error(png, b, fr)
            if e > 40: bad.append((png, os.path.relpath(path, GAME), b['w'], b['h'], e))
    for png, src, w, h, e in bad: print('%-50s %3dx%-3d err %5.1f  %s' % (src[:50], w, h, e, os.path.basename(png)))
    print(len(bad), 'bad files')
    if '--delete' in sys.argv:
        for x in bad: os.remove(x[0])
        print('deleted')
