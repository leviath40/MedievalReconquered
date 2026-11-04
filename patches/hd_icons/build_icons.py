"""Build HD copies of the game's 2D sprites for the d3d9 proxy (HDIcons=1).

Sources: every .BIF under the given folders (default: campmap and frontend_files, without
campmap\\pieces, which is drawn in 3D), plus hd_icons/_pending/*.bif: sprites the proxy saw without an
HD copy and saved as they are in memory (portraits converted from .tga, recoloured cards...).
For each frame: decode (tools/bif.py), make transparent pixels (skipped RLE runs, magenta key colour)
alpha 0 with their colour bled in from the neighbours (so the upscaler doesn't darken edges), upscale
with Upscayl (ultrasharp-4x, keeps alpha; 4x, or 2x for sprites of 200 px or more, about their
on-screen scale) and write hd_icons/<hash>_<frame>.png. <hash> = sprite_hash below, as the proxy
computes it from the sprite in memory (report section 14).
Processed pending files are deleted.

    python patches\\hd_icons\\build_icons.py [folders...]

Incremental: existing outputs are skipped. The PNGs are derived from the game's art: never commit them.
"""
import os, struct, subprocess, sys, tempfile, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(GAME, 'tools'))
import bif

UPSCAYL = r'C:\Program Files\Upscayl\resources'
OUT = os.path.join(GAME, 'hd_icons')
PENDING = os.path.join(OUT, '_pending')
SKIP_DIRS = {os.path.join('campmap', 'pieces').lower()}
SKIP_HASHES = {'5d647033'}          # menu logo: has its own HD version (patches/logo, MTW_LOGO_HD.raw)
CARD = (50, 50)                     # card-sized sprites: flattened on the parchment, the proxy keys it out
PARCHMENT = (207, 207, 199, 255)    # (a transparent upscale leaves a fringe around thin figures)
BIG = 200                           # sprites this size or more: 2x, else 4x
THIN = 16                           # sprites thinner than this are skipped

def fnv(b, h=2166136261):
    for c in b: h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h

def sprite_hash(d):
    """d: the sprite (BIF layout). Palette, header and three pixel rows of frame 0: sprite_hash in icons.c."""
    h = fnv(d[:0x214])
    rows = struct.unpack_from('<I', d, 0x204)[0]
    if not 0 < rows <= 2048: return h
    p = 0x214 + struct.unpack_from('<I', d, 0x214)[0]
    for y in range(rows * 3 // 4 + 1):
        n = struct.unpack_from('<H', d, p)[0]
        if not 2 <= n <= 16384: break
        if y in (rows // 4, rows // 2, rows * 3 // 4): h = fnv(d[p:p + n], h)
        p += n
    return h

def bleed(px, w, h):
    """px: list of rows of (r, g, b, a). Give alpha-0 pixels the average colour of opaque neighbours."""
    known = [[p[3] > 0 for p in r] for r in px]
    todo = [(y, x) for y in range(h) for x in range(w) if not known[y][x]]
    for _ in range(4):                                   # a few rings are enough for the upscaler's kernel
        new = []
        for y, x in todo:
            acc = [0, 0, 0]; n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and known[yy][xx]:
                        q = px[yy][xx]; acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; n += 1
            if n: new.append((y, x, (acc[0] // n, acc[1] // n, acc[2] // n, 0)))
        if not new: break
        for y, x, p in new: px[y][x] = p; known[y][x] = True
        todo = [(y, x) for y, x in todo if not known[y][x]]

def upscale(src, scale, out):
    """Upscale a folder of PNGs. One thread per stage: batch mode with parallel threads garbles some outputs."""
    subprocess.run([os.path.join(UPSCAYL, 'bin', 'upscayl-bin.exe'), '-i', src, '-o', out,
                    '-m', os.path.join(UPSCAYL, 'models'), '-n', 'ultrasharp-4x', '-s', str(scale), '-f', 'png',
                    '-j', '1:1:1'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def write_png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(v for p in r for v in p) for r in px)
    ch = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))

def sources(folders):
    for folder in folders:
        for dp, ds, fs in os.walk(os.path.join(GAME, folder)):
            ds[:] = [d for d in ds if os.path.relpath(os.path.join(dp, d), GAME).lower() not in SKIP_DIRS]
            for f in fs:
                if f.lower().endswith('.bif'): yield os.path.join(dp, f), False
    if os.path.isdir(PENDING):
        for f in os.listdir(PENDING):
            if f.lower().endswith('.bif'): yield os.path.join(PENDING, f), True

def main(folders):
    os.makedirs(OUT, exist_ok=True)
    tmp = {s: tempfile.mkdtemp(prefix='hd_icons_%d_' % s) for s in (2, 4)}
    n = {2: 0, 4: 0}; done_pending = []
    for path, pending in sources(folders):
        with open(path, 'rb') as fh: data = fh.read()
        hsh = '%08x' % sprite_hash(data)
        if pending: done_pending.append(path)        # processed or skipped: either way, done
        if hsh in SKIP_HASHES: continue
        try: b = bif.read(path)
        except Exception as e: print('skip', path, e); continue
        if min(b['w'], b['h']) < THIN: continue         # dividers and borders: nothing to gain, and Upscayl hangs on them
        scale = 2 if max(b['w'], b['h']) >= BIG else 4
        pal = [bif.rgb565(c) for c in b['pal']]
        for i, fr in enumerate(b['frames']):
            name = '%s_%d.png' % (hsh, i)
            if os.path.exists(os.path.join(OUT, name)) or os.path.exists(os.path.join(tmp[scale], name)): continue
            px = [[(0, 0, 0, 0) if p is None or b['pal'][p] == 0xF81F else pal[p] + (255,) for p in r] for r in fr]
            if (b['w'], b['h']) == CARD: px = [[p if p[3] else PARCHMENT for p in r] for r in px]
            else: bleed(px, b['w'], b['h'])
            write_png(os.path.join(tmp[scale], name), b['w'], b['h'], px)
            n[scale] += 1
    print('frames to upscale: %d at 4x, %d at 2x' % (n[4], n[2]))
    for s in (4, 2):
        if n[s]: upscale(tmp[s], s, OUT)
        for f in os.listdir(tmp[s]): os.remove(os.path.join(tmp[s], f))
        os.rmdir(tmp[s])
    for p in done_pending: os.remove(p)
    pngs = [f for f in os.listdir(OUT) if f.endswith('.png')]
    print('hd_icons: %d files, %.0f MB' % (len(pngs), sum(os.path.getsize(os.path.join(OUT, f)) for f in pngs) / 2**20))

if __name__ == '__main__':
    main(sys.argv[1:] or ['campmap', 'frontend_files'])
