"""Put the MedievalReconquered logo on the loading screen and the main menu.

Reads the untouched originals from MedievalOriginal (read-only) and writes into the game install:
  frontend_files/Load Screen.tga, frontend_files/Load Screen.BIF   (loading screen, 1280x1024)
  frontend_files/Main_Menu/MTW_LOGO.BIF                            (main menu sprite, 321x131)
Run from MedievalReconquered/:  python patches\\logo\\build_logo.py  [--preview DIR]
"""
import argparse, struct, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import bif  # noqa: E402

ORIG = ROOT.parent / 'MedievalOriginal' / 'Medieval Original' / 'frontend_files'
OUT = ROOT / 'frontend_files'
SRC = Path(__file__).with_name('logo_src.png')

# loading screen: old logo box (erased), new logo width and centre
OLD_BOX = (250, 418, 1050, 614)          # x0, y0, x1, y1
LOAD_W, LOAD_CY = 900, 516
MENU_W, MENU_H = 321, 131
SOLID = 0.3                               # bg distance at which a pixel counts as opaque
KEY = 0xF81F                              # magenta: transparent in the game's BIF loader


def read_png(path):
    d = Path(path).read_bytes(); p = 8; idat = b''
    while p < len(d):
        n, t = struct.unpack_from('>I4s', d, p); c = d[p + 8:p + 8 + n]; p += 12 + n
        if t == b'IHDR': w, h, depth, ctype = struct.unpack_from('>IIBB', c)
        elif t == b'IDAT': idat += c
    assert depth == 8 and ctype in (2, 6), 'need 8-bit RGB/RGBA PNG'
    bpp = 3 if ctype == 2 else 4; raw = zlib.decompress(idat); stride = w * bpp
    rows, prev, q = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[q]; line = bytearray(raw[q + 1:q + 1 + stride]); q += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)]); prev = line
    return w, h, rows


def unblend(rows):
    """Logo on an off-white background -> premultiplied RGBA floats (alpha from distance to bg)."""
    bg = [sum(rows[y][x][i] for y, x in ((0, 0), (0, -1), (-1, 0), (-1, -1))) / 4 for i in range(3)]
    out = []
    for r in rows:
        o = []
        for px in r:
            # distance from the background, remapped so solid letter metal is fully opaque
            a = max(0.0, min(1.0, max((bg[i] - px[i]) / bg[i] for i in range(3)) / SOLID))
            # premultiplied colour: c = a*C + (1-a)*bg  ->  a*C = c - (1-a)*bg
            o.append((max(0.0, px[0] - (1 - a) * bg[0]), max(0.0, px[1] - (1 - a) * bg[1]),
                      max(0.0, px[2] - (1 - a) * bg[2]), a))
        out.append(o)
    # crop to content
    ys = [y for y, r in enumerate(out) if any(p[3] > 0.03 for p in r)]
    xs = [x for x in range(len(out[0])) if any(out[y][x][3] > 0.03 for y in ys)]
    return [r[xs[0]:xs[-1] + 1] for r in out[ys[0]:ys[-1] + 1]]


def resize(img, nw):
    """Area-average downscale of premultiplied RGBA to width nw (aspect kept)."""
    h, w = len(img), len(img[0]); nh = max(1, round(h * nw / w)); sx, sy = w / nw, h / nh

    def spans(n, s, lim):
        res = []
        for i in range(n):
            a, b = i * s, (i + 1) * s; seg = []; k = int(a)
            while k < b and k < lim:
                seg.append((k, min(b, k + 1) - max(a, k))); k += 1
            res.append(seg)
        return res
    xsp, ysp = spans(nw, sx, w), spans(nh, sy, h)
    # horizontal pass then vertical
    tmp = [[tuple(sum(row[k][c] * f for k, f in sp) / sx for c in range(4)) for sp in xsp] for row in img]
    return [[tuple(sum(tmp[k][x][c] * f for k, f in sp) / sy for c in range(4)) for x in range(nw)] for sp in ysp]


def over(dst, logo, ox, oy):
    for y, r in enumerate(logo):
        drow = dst[oy + y]
        for x, (r_, g, b, a) in enumerate(r):
            if a <= 0: continue
            d = drow[ox + x]
            drow[ox + x] = tuple(min(255, round(s + d[i] * (1 - a))) for i, s in enumerate((r_, g, b)))


def erase_box(px, box, feather=14):
    """Hide the old logo: fill the box with the texture directly above it, feathered into the edges."""
    x0, y0, x1, y1 = box; hgt = y1 - y0
    for y in range(y0 - feather, y1 + feather):
        for x in range(x0 - feather, x1 + feather):
            dx = min(x - (x0 - feather), (x1 + feather - 1) - x); dy = min(y - (y0 - feather), (y1 + feather - 1) - y)
            t = min(1.0, min(dx, dy) / feather)
            s = px[y - hgt - feather][x]; d = px[y][x]
            px[y][x] = tuple(round(d[i] * (1 - t) + s[i] * t) for i in range(3))


def read_tga(path):
    d = Path(path).read_bytes(); w, h = struct.unpack_from('<HH', d, 12)
    assert d[2] == 2 and d[16] == 24 and not d[17] & 0x20, 'expected uncompressed bottom-up 24-bit TGA'
    p = 18 + d[0]
    rows = [[(d[p + (y * w + x) * 3 + 2], d[p + (y * w + x) * 3 + 1], d[p + (y * w + x) * 3]) for x in range(w)]
            for y in range(h - 1, -1, -1)]
    return d[:p], d[p + w * h * 3:], rows


def write_tga(path, hdr, tail, rows):
    body = bytearray()
    for r in reversed(rows):
        for c in r: body += bytes((c[2], c[1], c[0]))
    Path(path).write_bytes(hdr + bytes(body) + tail)


def to565(c): return (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3


def quantize(pixels, n):
    """Median cut on a 15-bit histogram -> list of n RGB colours."""
    hist = {}
    for c in pixels:
        k = (c[0] >> 3, c[1] >> 3, c[2] >> 3); hist[k] = hist.get(k, 0) + 1
    boxes = [list(hist.items())]
    while len(boxes) < n:
        boxes.sort(key=lambda b: max(max(k[i] for k, _ in b) - min(k[i] for k, _ in b) for i in range(3)) * (len(b) > 1))
        b = boxes.pop()
        if len(b) < 2: boxes.append(b); break
        ch = max(range(3), key=lambda i: max(k[i] for k, _ in b) - min(k[i] for k, _ in b))
        b.sort(key=lambda kv: kv[0][ch]); tot = sum(v for _, v in b); acc = 0
        for cut, (_, v) in enumerate(b):
            acc += v
            if acc >= tot / 2: break
        cut = max(1, min(len(b) - 1, cut + 1)); boxes += [b[:cut], b[cut:]]
    pal = []
    for b in boxes:
        tot = sum(v for _, v in b)
        pal.append(tuple(min(255, round(sum(k[i] * v for k, v in b) / tot * 8 + 4)) for i in range(3)))
    return pal


def index_pixels(rows, pal, offset=0):
    cache = {}

    def near(c):
        k = (c[0] >> 2, c[1] >> 2, c[2] >> 2)
        if k not in cache:
            cache[k] = offset + min(range(len(pal)), key=lambda i: sum((pal[i][j] - c[j]) ** 2 for j in range(3)))
        return cache[k]
    return [[None if c is None else near(c) for c in r] for r in rows]


def build_load_screen(logo, preview):
    hdr, tail, px = read_tga(ORIG / 'Load Screen.tga')
    erase_box(px, OLD_BOX)
    lg = resize(logo, LOAD_W)
    over(px, lg, (len(px[0]) - LOAD_W) // 2, LOAD_CY - len(lg) // 2)
    write_tga(OUT / 'Load Screen.tga', hdr, tail, px)
    pal = quantize([c for r in px for c in r], 256)
    bif.write(OUT / 'Load Screen.BIF', [to565(c) for c in pal], len(px[0]), len(px), index_pixels(px, pal))
    if preview: bif.to_png(OUT / 'Load Screen.BIF', preview / 'load_screen.png', scale=2)


def build_menu(logo, preview):
    old = bif.read(ORIG / 'Main_Menu' / 'MTW_LOGO.BIF')
    # parchment tone behind the old logo (its corners) for the soft edges of the new one
    corners = [bif.rgb565(old['pal'][old['frames'][0][y][x]]) for y in (2, 3, MENU_H - 4, MENU_H - 3)
               for x in (2, 3, MENU_W - 4, MENU_W - 3)]
    tone = [sum(c[i] for c in corners) / len(corners) for i in range(3)]
    lg = resize(logo, MENU_W - 4); oy = (MENU_H - len(lg)) // 2
    rows = [[None] * MENU_W for _ in range(MENU_H)]
    for y, r in enumerate(lg):
        for x, (r_, g, b, a) in enumerate(r):
            if a < 0.2: continue
            rows[oy + y][2 + x] = tuple(min(255, round(s + tone[i] * (1 - a))) for i, s in enumerate((r_, g, b)))
    pal = quantize([c for r in rows for c in r if c is not None], 255)
    bif.write(OUT / 'Main_Menu' / 'MTW_LOGO.BIF', [KEY] + [to565(c) for c in pal], MENU_W, MENU_H,
              index_pixels(rows, pal, offset=1), unk=old['unk'])
    if preview: bif.to_png(OUT / 'Main_Menu' / 'MTW_LOGO.BIF', preview / 'menu_logo.png')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--preview', type=Path, help='also write PNG previews of the results here')
    a = ap.parse_args()
    if a.preview: a.preview.mkdir(parents=True, exist_ok=True)
    logo = unblend(read_png(SRC)[2])
    build_menu(logo, a.preview); print('wrote Main_Menu/MTW_LOGO.BIF')
    build_load_screen(logo, a.preview); print('wrote Load Screen.tga + Load Screen.BIF')


if __name__ == '__main__':
    main()
