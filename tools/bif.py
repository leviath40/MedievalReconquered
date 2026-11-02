"""Medieval: Total War .BIF sprite codec (stdlib only).

Layout (from the game's loader, bifstuff.cpp):
  0x000  256 x u16 palette (RGB565)
  0x200  u32 width, 0x204 u32 height, 0x208 u32 (unknown, kept), 0x20c u32 frames,
  0x210  u32 (1), 0x214 u32 frame offsets[frames], relative to 0x214
  frame: per row, u16 row byte length (incl. itself) then RLE ops until width is covered:
    00 n        skip n pixels (transparent)
    c<0x80, v   repeat v (c+1) times
    c>=0x80 ... literal run of (256-c) bytes
Index whose colour is magenta (0xF81F) is also drawn transparent by the game.
"""
import struct, zlib

def read(path):
    d = open(path, 'rb').read()
    pal = struct.unpack_from('<256H', d, 0)
    w, h, unk, frames, f2 = struct.unpack_from('<5I', d, 0x200)
    offs = struct.unpack_from('<%dI' % (frames * f2), d, 0x214)
    out = []
    for fo in offs:
        p = 0x214 + fo; px = []
        for _ in range(h):
            rl = struct.unpack_from('<H', d, p)[0]; q = p + 2; row = []
            while len(row) < w:
                c = d[q]
                if c == 0: row += [None] * d[q + 1]; q += 2
                elif c < 0x80: row += [d[q + 1]] * (c + 1); q += 2
                else: n = 256 - c; row += list(d[q + 1:q + 1 + n]); q += 1 + n
            px.append(row[:w]); p += rl
        out.append(px)
    return dict(pal=pal, w=w, h=h, unk=unk, f2=f2, frames=out)

def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31

def encode_row(row):
    """row: list of palette indices or None (transparent)."""
    o = bytearray(); i = 0; n = len(row)
    while i < n:
        if row[i] is None:
            j = i
            while j < n and row[j] is None and j - i < 255: j += 1
            o += bytes((0, j - i)); i = j; continue
        j = i
        while j < n and row[j] == row[i] and j - i < 128: j += 1
        if j - i >= 3:
            o += bytes((j - i - 1, row[i])); i = j; continue
        j = i
        while j < n and row[j] is not None and j - i < 128:
            if j + 2 < n and row[j] == row[j + 1] == row[j + 2]: break
            j += 1
        if j == i: j = i + 1
        o += bytes((256 - (j - i),)) + bytes(row[i:j]); i = j
    return struct.pack('<H', len(o) + 2) + o

def write(path, pal, w, h, rows, unk=0x6a):
    data = b''.join(encode_row(r) for r in rows)
    pal = list(pal) + [0] * (256 - len(pal))
    hdr = struct.pack('<256H', *pal) + struct.pack('<6I', w, h, unk, 1, 1, 4)
    open(path, 'wb').write(hdr + data)

def save_png(path, w, h, rgba_rows):
    raw = b''.join(b'\0' + bytes(v for px in r for v in px) for r in rgba_rows)
    ch = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))

def to_png(bif, png, frame=0, scale=1):
    b = read(bif); pal = [rgb565(c) for c in b['pal']]
    rows = [[(0, 0, 0, 0) if i is None else pal[i] + (255,) for i in r[::scale]] for r in b['frames'][frame][::scale]]
    save_png(png, len(rows[0]), len(rows), rows)

if __name__ == '__main__':
    import sys
    to_png(sys.argv[1], sys.argv[2], scale=int(sys.argv[3]) if len(sys.argv) > 3 else 1)
