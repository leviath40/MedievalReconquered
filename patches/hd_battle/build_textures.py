"""Build hd_textures/: HD versions of the battle ground, skies, trees, buildings and castles (report section 15).

Run from anywhere: python patches/hd_battle/build_textures.py [--force]
Needs Pillow (pip install pillow) and Upscayl. Already built textures are skipped, so it can be stopped and rerun.

The proxy finds a picture by the hash of the texture the game loaded, and that texture can be worked out from the
file: see to_game(). Run on the game's own files, nothing is committed. Soldier pages and effects are left alone.
"""
import glob, os, shutil, struct, subprocess, sys, tempfile, time
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(GAME, 'hd_textures')
UPSCAYL = r'C:\Program Files\Upscayl\resources'

Z = 4
PAD = 12                            # edge copied around each picture, gives the upscaler context
BAND = 16                           # tiles fade to the low-res picture over this many HD pixels: neighbours meet there
CASTLE_SIZE = 2048                  # one 256x256 page covers every wall of a castle: two passes
CASTLE_PAGES = ('0caspg1', '0caspg2', '0iaspg1', '0iaspg2')
MODEL_SIZE = 1024                   # small model textures cover whole buildings: two passes up to this size
SKIP = ('flags',)                   # recoloured per faction
TREES = ('ariddec', 'conifer', 'despalm', 'lushdec')
JPEG = dict(quality=92, subsampling=0)


def lbm(path):
    d = open(path, 'rb').read()
    if d[:4] != b'FORM' or d[8:12] != b'PBM ': return None
    i = 12; w = h = comp = 0; pal = body = None
    while i + 8 <= len(d):
        tag = d[i:i+4]; n = struct.unpack('>I', d[i+4:i+8])[0]; b = d[i+8:i+8+n]
        if tag == b'BMHD': w, h = struct.unpack('>HH', b[:4]); comp = b[10]
        elif tag == b'CMAP': pal = b
        elif tag == b'BODY': body = b
        i += 8 + n + (n & 1)
    if not pal or body is None: return None
    stride = w + (w & 1)
    if comp:
        o = bytearray(); j = 0
        while j < len(body) and len(o) < stride * h:
            c = body[j]; j += 1
            if c < 128: o += body[j:j+c+1]; j += c + 1
            elif c > 128: o += bytes([body[j]]) * (257 - c); j += 1
        body = bytes(o)
    px = bytearray()
    for y in range(h):
        for v in body[y*stride:y*stride+w]: px += pal[v*3:v*3+3]
    return Image.frombytes('RGB', (w, h), bytes(px))


def load(path):
    try:
        return lbm(path) if path.lower().endswith('.lbm') else Image.open(path).convert('RGB')
    except OSError:
        return None


def to_game(im, keyed):
    """The A1R5G5B5 texture the game makes. LBM (keyed): black is transparent, any other colour that rounds to 0
    becomes 1 to stay visible. TGA: every pixel is opaque and keeps its rounded value."""
    o = bytearray()
    for r, g, b in zip(*[iter(im.tobytes())] * 3):
        c = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
        if keyed:
            if r | g | b == 0:
                o += b'\0\0'
                continue
            c = c or 1
        c |= 0x8000
        o += bytes((c & 255, c >> 8))
    return bytes(o)


def fnv(b):
    h = 2166136261
    for x in b: h = (h ^ x) * 16777619 & 0xffffffff
    return h


def sources():
    """(kind, name, image, keyed): ground, sky, model, castle or tree; keyed = black means transparent."""
    for kind, roots in (('ground', ['Textures/Ground']), ('sky', ['Textures/Backdrops']), ('model', ['Models', 'NewModels'])):
        for root in roots:
            for dp, _, fs in os.walk(os.path.join(GAME, root)):
                for f in sorted(fs):
                    base, ext = os.path.splitext(f.lower())
                    if ext not in ('.tga', '.lbm') or base in SKIP: continue
                    p = os.path.join(dp, f); im = load(p)
                    if im is None: continue
                    k = 'castle' if base in CASTLE_PAGES else kind
                    yield k, p, im, ext == '.lbm'
    for folder in ('Textures/Trees', 'Textures/Trees/winter'):         # four tree pictures make one page
        for name in TREES:
            cells = [load(os.path.join(GAME, folder, '%s%d.lbm' % (name, i))) for i in range(4)]
            if None in cells: continue
            page = Image.new('RGB', (256, 256))
            for i, c in enumerate(cells): page.paste(c, (i % 2 * 128, i // 2 * 128))
            yield 'tree', folder + '/' + name, page, True


def target(kind, size):
    """Final width. Two passes (16x) when it is more than one pass can give."""
    if kind == 'castle': return CASTLE_SIZE
    if kind == 'model' and size <= 128: return min(MODEL_SIZE, size * Z * Z)
    return size * Z


def built(h, want):
    for ext in ('.jpg', '.png'):
        p = os.path.join(OUT, h + ext)
        if os.path.exists(p): return Image.open(p).size[0] == want
    return False


def rgba(im):
    a = im.convert('L').point(lambda v: 255).copy()
    a.putdata([0 if p == (0, 0, 0) else 255 for p in im.getdata()])
    o = im.convert('RGBA'); o.putalpha(a)
    return o


def bleed(im):
    """Opaque copy: hidden pixels take the colour of the nearest visible ones, so no black gets upscaled in."""
    w, h = im.size; px = list(im.getdata())
    known = [p != (0, 0, 0) for p in px]
    for _ in range(16):
        new = []
        for i in range(w * h):
            if known[i]: continue
            x, y = i % w, i // w; acc = [0, 0, 0]; n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < w and 0 <= yy < h and known[yy * w + xx]:
                        q = px[yy * w + xx]; acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; n += 1
            if n: new.append((i, (acc[0] // n, acc[1] // n, acc[2] // n)))
        if not new: break
        for i, c in new: px[i] = c; known[i] = True
    o = Image.new('RGB', (w, h)); o.putdata(px)
    return o


def padded(im):
    w, h = im.size
    c = im.resize((w + 2 * PAD, h + 2 * PAD), Image.NEAREST)
    c.paste(im, (PAD, PAD))
    return c


def upscale(src, dst):
    subprocess.run([os.path.join(UPSCAYL, 'bin', 'upscayl-bin.exe'), '-i', src, '-o', dst,
                    '-m', os.path.join(UPSCAYL, 'models'), '-n', 'ultrasharp-4x', '-s', str(Z), '-f', 'png',
                    '-j', '1:1:1'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def edge_mask(size, cache={}):
    if size not in cache:
        w, h = size
        m = Image.new('L', size)
        m.putdata([max(0, 255 - 255 * min(x, y, w - 1 - x, h - 1 - y) // BAND) for y in range(h) for x in range(w)])
        cache[size] = m
    return cache[size]


def first_png(folder, name):
    p = glob.glob(os.path.join(folder, name + '*.png'))
    return Image.open(p[0]) if p else None


def main():
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='hd_textures_')
    a, b, c, d = (os.path.join(tmp, n) for n in 'abcd')
    for n in (a, b, c, d): os.makedirs(n)
    force = '--force' in sys.argv
    t0 = time.time(); jobs = {}; seen = 0
    for kind, name, im, keyed in sources():
        seen += 1
        h = '%08x' % fnv(to_game(im, keyed))
        want = target(kind, im.size[0])
        if h in jobs or (not force and built(h, want)): continue
        cut = keyed and (0, 0, 0) in dict((p, 1) for _, p in im.getcolors(1 << 24))
        jobs[h] = (kind, cut, want)
        im.save(os.path.join(tmp, h + '.src.png'))
        if want > im.size[0] * Z: padded(bleed(im) if cut else im).save(os.path.join(a, h + '.png'))
        else: padded(rgba(im) if cut else im).save(os.path.join(a, h + '.png'))
    print('%d pictures, %d to build (%.0f s)' % (seen, len(jobs), time.time() - t0), flush=True)
    if not jobs: return
    upscale(a, b)
    print('first pass done (%.0f s)' % (time.time() - t0), flush=True)
    two = [h for h, j in jobs.items() if j[2] > Image.open(os.path.join(tmp, h + '.src.png')).size[0] * Z]
    for h in two:
        im = first_png(b, h).convert('RGB')
        im.crop((PAD * Z, PAD * Z, im.width - PAD * Z, im.height - PAD * Z)).save(os.path.join(c, h + '.png'))
    if two: upscale(c, d)
    done = 0
    for h, (kind, cut, want) in jobs.items():
        lo = Image.open(os.path.join(tmp, h + '.src.png')).convert('RGB')
        w, ht = lo.size
        if h in two:
            u = first_png(d, h).convert('RGB').resize((want, want * ht // w), Image.LANCZOS)
            if cut:
                al = rgba(lo).getchannel('A').resize(u.size, Image.BICUBIC).point(lambda v: max(0, min(255, (v - 110) * 7)))
                u = u.convert('RGBA'); u.putalpha(al)
        else:
            u = first_png(b, h).convert('RGBA').crop((PAD * Z, PAD * Z, (PAD + w) * Z, (PAD + ht) * Z))
            if not cut: u = u.convert('RGB')
            if kind in ('ground', 'sky'):
                u = Image.composite(lo.resize(u.size, Image.BICUBIC).convert(u.mode), u, edge_mask(u.size))
        for ext in ('.jpg', '.png'):               # a rebuilt picture may change type
            if os.path.exists(os.path.join(OUT, h + ext)): os.remove(os.path.join(OUT, h + ext))
        if cut: u.save(os.path.join(OUT, h + '.png'), compress_level=1)
        else: u.save(os.path.join(OUT, h + '.jpg'), **JPEG)
        done += 1
        if not done % 200: print('%d/%d written (%.0f s)' % (done, len(jobs), time.time() - t0), flush=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print('done: %d pictures in %.0f s' % (done, time.time() - t0))


if __name__ == '__main__':
    main()
