"""Set the Large Address Aware flag on the game exe (2 GB -> 4 GB of address space on 64-bit Windows).

Sets IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20) in the PE file header Characteristics and refreshes the
PE checksum. Writes a copy; never touches the input.
Run from MedievalReconquered/:  python patches\\laa\\apply_laa.py [--exe IN] [--out OUT]
"""
import argparse, struct
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
LAA = 0x20


def pe_checksum(d, csum_off):
    s = 0
    for i in range(0, len(d) - len(d) % 2, 2):
        if csum_off <= i < csum_off + 4: continue
        s += d[i] | d[i + 1] << 8; s = (s & 0xFFFF) + (s >> 16)
    if len(d) % 2: s += d[-1]; s = (s & 0xFFFF) + (s >> 16)
    return ((s & 0xFFFF) + (s >> 16) + len(d)) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', default=str(GAME_DIR / 'MedievalReconquered.exe'))
    ap.add_argument('--out', default=str(GAME_DIR / 'MedievalReconquered_laa.exe'))
    a = ap.parse_args()
    d = bytearray(Path(a.exe).read_bytes())
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', d, pe + 4)[0] == 0x14C, 'not an x86 PE'
    ch_off, csum_off = pe + 22, pe + 24 + 64
    ch = struct.unpack_from('<H', d, ch_off)[0]
    if ch & LAA: print('already Large Address Aware')
    struct.pack_into('<H', d, ch_off, ch | LAA)
    struct.pack_into('<I', d, csum_off, pe_checksum(d, csum_off))
    Path(a.out).write_bytes(d)
    print(f'characteristics {ch:#06x} -> {ch | LAA:#06x}, wrote {a.out}')


if __name__ == '__main__':
    main()
