r"""Queue the campaign unit cards in every faction's colours for build_icons.py.

The game recolours campaign/review_panel/Units/*.BIF at load: palette entries 208-255 become two ramps of the
faction's colours, so each faction has its own sprite hash and its own HD file (report section 14). This writes
those recoloured sprites to hd_icons/_pending/, where build_icons.py picks them up:

    python patches\hd_icons\faction_cards.py
    python patches\hd_icons\build_icons.py

Sprites that already have an HD file are skipped.
"""
import glob, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from build_icons import sprite_hash, OUT, PENDING, PARCHMENT

BG = PARCHMENT[:3]
# the exe's table at 0x799F7C, replaced entry by entry by SetFactionCol in the campaign start files
DEFAULTS = ['f0f0f0caceb0', 'ff8a00202020', 'a301f5ffffff', 'ffffffff0000', 'ecb826feffff', 'ff0000ff0000',
            '2402d3ffe400', '010101ffe400', '006f15ffe400', '960000ffffff', '4f8ed2ffe400', 'ffe400ffe400',
            '4f9824ff8a00', 'ff594cfff185', 'a65757c9c9c9', '5c5c5cffffff', '94b4e5fff185', 'a28600a28600',
            '4f8ed2fff185', 'fff185ffffff', 'c9c9c9202020']


def faction_colours():
    """Every colour pair the game can use, as 'rrggbbrrggbb'."""
    pairs = set(DEFAULTS)
    for f in glob.glob(os.path.join(GAME, 'campmap', 'startpos', '*.TXT')):
        for m in re.finditer(r'SetFactionCol::\s+\w+\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)',
                             open(f, errors='replace').read()):
            pairs.add(bytes(int(v) for v in m.groups()).hex())
    return sorted(pairs)


def to565(r, g, b): return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def recoloured_palette(pal, colours):
    """pal: 0x200 bytes of 565 entries. The second faction colour goes to 208-215 and 224-239."""
    e = list(struct.unpack('<256H', pal))
    c0, c1 = bytes.fromhex(colours[:6]), bytes.fromhex(colours[6:])
    for light, dark, c in ((208, 224, c1), (216, 240, c0)):
        for i in range(8):
            e[light + i] = to565(*[(BG[n] * (8 - i) + c[n] * i) // 8 for n in range(3)])
        for i in range(16):
            e[dark + i] = to565(*[c[n] * i // 16 for n in range(3)])
    return struct.pack('<256H', *e)


def main():
    os.makedirs(PENDING, exist_ok=True)
    pairs = faction_colours()
    done = {f.split('_')[0] for f in os.listdir(OUT) if f.endswith('.png')}
    queued = 0
    for f in glob.glob(os.path.join(GAME, 'campmap', 'review_panel', 'Units', '*.[bB][iI][fF]')):
        d = open(f, 'rb').read()
        for colours in pairs:
            s = recoloured_palette(d[:0x200], colours) + d[0x200:]
            h = '%08x' % sprite_hash(s)
            if h not in done:
                open(os.path.join(PENDING, h + '.bif'), 'wb').write(s)
                queued += 1
    print('%d colour pairs, %d cards queued' % (len(pairs), queued))


if __name__ == '__main__':
    main()
