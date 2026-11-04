r"""Build HD copies of the battle unit-card soldier icons for the d3d9 proxy (CardIcons=1, report section 14).

Battle\UnitIcons\*.lbm are 8-bit images, opaque on the card's parchment colour. The game loads them with
palette entries 208-255 rewritten to the player's two faction colours (callback 0x4DA5B0),
so each icon is rendered here once per colour pair (faction_cards.py), upscaled with Upscayl (ultrasharp-4x, 3x: 150 px, about
the on-screen size up to 4K) and written to hd_icons/battle/<name>_<rrggbbrrggbb>.png (name in lower case).

    python patches\hd_icons\build_card_icons.py

Incremental: existing outputs are skipped. The PNGs are derived from the game's art: never commit them.
"""
import glob, os, shutil, struct, subprocess, sys, tempfile, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from faction_cards import BG, faction_colours

UPSCAYL = r'C:\Program Files\Upscayl\resources'
OUT = os.path.join(GAME, 'hd_icons', 'battle')


def load_lbm(path):
    """PBM-format IFF: returns width, height, palette (bytearray), pixels (stride rounded up to even)."""
    d = open(path, 'rb').read()
    i, comp, pal, body = 12, 0, None, b''
    while i < len(d) - 8:
        tag, size = d[i:i + 4], struct.unpack('>I', d[i + 4:i + 8])[0]
        c = d[i + 8:i + 8 + size]
        if tag == b'BMHD': w, h = struct.unpack('>HH', c[:4]); comp = c[10]
        elif tag == b'CMAP': pal = bytearray(c)
        elif tag == b'BODY': body = c
        i += 8 + size + (size & 1)
    if comp:
        o, j = bytearray(), 0
        while j < len(body):
            b = body[j]; j += 1
            if b < 128: o += body[j:j + b + 1]; j += b + 1
            elif b > 128: o += bytes([body[j]]) * (257 - b); j += 1
        body = bytes(o)
    return w, h, pal, body


def recolour(pal, colours):
    """Same as the game's callback: two ramps per faction colour."""
    pal = bytearray(pal)
    for k, (light, dark) in enumerate(((208, 224), (216, 240))):
        c = bytes.fromhex(colours[k * 6:k * 6 + 6])
        for i in range(8):
            t = i / 7
            for n in range(3): pal[(light + i) * 3 + n] = (round((1 - t) * BG[n]) + round(c[n] * t)) & 255
        for i in range(16):
            for n in range(3): pal[(dark + i) * 3 + n] = round(c[n] * i / 15)
    return pal


def write_png(path, w, h, rgb):
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    sets = faction_colours()
    os.makedirs(OUT, exist_ok=True)
    icons = [(os.path.splitext(os.path.basename(f))[0].lower(), load_lbm(f))
             for f in sorted(glob.glob(os.path.join(GAME, 'Battle', 'UnitIcons', '*.lbm')))]
    with tempfile.TemporaryDirectory() as tmp:
        todo = 0
        for name, (w, h, pal, body) in icons:
            stride = w + (w & 1)
            for s in sets:
                out = f'{name}_{s}.png'
                if os.path.exists(os.path.join(OUT, out)): continue
                p = recolour(pal, s)
                rgb = b''.join(bytes(p[v * 3:v * 3 + 3]) for y in range(h) for v in body[y * stride:y * stride + w])
                write_png(os.path.join(tmp, out), w, h, rgb)
                todo += 1
        print(todo, 'icons to upscale', flush=True)
        if todo:
            subprocess.run([os.path.join(UPSCAYL, 'bin', 'upscayl-bin.exe'), '-i', tmp, '-o', OUT,
                            '-m', os.path.join(UPSCAYL, 'models'), '-n', 'ultrasharp-4x', '-s', '3', '-f', 'png',
                            '-j', '1:1:1'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(len(glob.glob(os.path.join(OUT, '*.png'))), 'files in', OUT)


main()
