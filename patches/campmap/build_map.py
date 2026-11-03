"""Build the campaign map texture (textures/campmap/MapTex2.tga) from map_src.jpg.

The source is a 1200x896 painting. Upscayl's "ultrasharp" model upscales it 4x
(clearly the sharpest of its models on this art) and resizes to the game's
4096x3072, then this script writes the 24-bit bottom-up TGA the game loads.

    python patches\\campmap\\build_map.py            # writes MapTex2.tga
    python patches\\campmap\\build_map.py --low      # also the 2048x1536 MapTex.tga fallback

Needs Upscayl (winget install Upscayl.Upscayl). Stdlib only otherwise.
The original textures are in ../MedievalOriginal/Medieval Original/textures/campmap/.
"""
import argparse, os, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
UPSCAYL = r'C:\Program Files\Upscayl\resources'

def read_png(path):
    """Minimal PNG reader: 8-bit RGB/RGBA, non-interlaced. Returns (w, h, bpp, rows)."""
    d = open(path, 'rb').read()
    p, idat = 8, b''
    while p < len(d):
        n, t = struct.unpack_from('>I4s', d, p)
        c = d[p + 8:p + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', c)
            assert depth == 8 and ctype in (2, 6) and not inter, 'unsupported PNG'
            bpp = 3 if ctype == 2 else 4
        elif t == b'IDAT':
            idat += c
        p += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, r = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): r[i] = (r[i] + r[i - bpp]) & 255
        elif f == 2:
            r = bytearray((a + b) & 255 for a, b in zip(r, prev))
        elif f == 3:
            for i in range(stride): r[i] = (r[i] + ((r[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = r[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                r[i] = (r[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(r); prev = r
    return w, h, bpp, rows

def write_tga(path, w, h, bpp, rows):
    """24-bit uncompressed TGA, bottom-up, BGR (same layout as the game's own)."""
    with open(path, 'wb') as f:
        f.write(struct.pack('<3B2HB4H2B', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 24, 0))
        for r in reversed(rows):
            o = bytearray(w * 3)
            o[0::3], o[1::3], o[2::3] = r[2::bpp], r[1::bpp], r[0::bpp]
            f.write(o)

def build(src, out, size, upscayl):
    tmp = os.path.join(tempfile.mkdtemp(), 'up.png')
    subprocess.run([os.path.join(upscayl, 'bin', 'upscayl-bin.exe'), '-i', src, '-o', tmp,
                    '-m', os.path.join(upscayl, 'models'), '-n', 'ultrasharp-4x',
                    '-r', '%dx%d' % size, '-f', 'png'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    w, h, bpp, rows = read_png(tmp)
    assert (w, h) == size, (w, h)
    write_tga(out, w, h, bpp, rows)
    os.remove(tmp)
    print('wrote', out, '%dx%d' % size)

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=os.path.join(HERE, 'map_src.jpg'))
    ap.add_argument('--game', default=GAME, help='game folder (default: this repo)')
    ap.add_argument('--upscayl', default=UPSCAYL, help='Upscayl "resources" folder')
    ap.add_argument('--low', action='store_true', help='also write the low-detail MapTex.tga')
    a = ap.parse_args()
    d = os.path.join(a.game, 'textures', 'campmap')
    build(a.src, os.path.join(d, 'MapTex2.tga'), (4096, 3072), a.upscayl)
    if a.low:
        build(a.src, os.path.join(d, 'MapTex.tga'), (2048, 1536), a.upscayl)
