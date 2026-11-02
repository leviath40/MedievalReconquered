"""Write a copy of the game exe with the Medieval Reconquered icon.

    python patches\\icon\\apply_icon.py [--exe MedievalReconquered.exe] [--out MedievalReconquered_icon.exe]

The exe has one icon group (RT_GROUP_ICON 111, language 2057) with one 32x32 16-colour image
(RT_ICON 1). Both are replaced by the images of MedievalReconquered.ico (16 to 256 px) through the
Windows resource API, which rewrites .rsrc. .rsrc is the last section with no data after it, and
is outside the Proteus range (0xF3C000-0xF45000). The input exe is never modified.
"""
import argparse
import ctypes
import shutil
import struct
import sys
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME_DIR = HERE.parent.parent
ICO = HERE / "MedievalReconquered.ico"
RT_ICON, RT_GROUP_ICON, GROUP_ID, LANG = 3, 14, 111, 2057

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.BeginUpdateResourceW.restype = wintypes.HANDLE
k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD,
                                ctypes.c_void_p, wintypes.DWORD]
k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]


def read_ico(path):
    """Return [(entry fields without offset, image bytes)] from an .ico file."""
    data = path.read_bytes()
    _, kind, count = struct.unpack_from("<HHH", data, 0)
    if kind != 1:
        sys.exit(f"{path} is not an icon file")
    out = []
    for i in range(count):
        w, h, colors, res, planes, bpp, size, off = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        out.append(((w, h, colors, res, planes, bpp, size), data[off:off + size]))
    return out


def update(h, rtype, rid, data):
    buf = ctypes.create_string_buffer(data, len(data)) if data else None
    if not k32.UpdateResourceW(h, rtype, rid, LANG, buf, len(data) if data else 0):
        raise OSError(ctypes.get_last_error(), f"UpdateResource type {rtype} id {rid}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(GAME_DIR / "MedievalReconquered.exe"))
    ap.add_argument("--out", default=str(GAME_DIR / "MedievalReconquered_icon.exe"))
    a = ap.parse_args()
    src, out = Path(a.exe).resolve(), Path(a.out).resolve()
    if src == out:
        sys.exit("--out must differ from --exe (the exe is never patched in place)")
    shutil.copy2(src, out)

    images = read_ico(ICO)
    h = k32.BeginUpdateResourceW(str(out), False)
    if not h:
        raise OSError(ctypes.get_last_error(), "BeginUpdateResource")
    try:
        update(h, RT_ICON, 1, None)                      # drop the old 32x32 image
        group = struct.pack("<HHH", 0, 1, len(images))
        for n, (fields, img) in enumerate(images, start=1):
            update(h, RT_ICON, n, img)
            group += struct.pack("<BBBBHHIH", *fields, n)
        update(h, RT_GROUP_ICON, GROUP_ID, group)
    except Exception:
        k32.EndUpdateResourceW(h, True)                  # discard
        out.unlink()
        raise
    if not k32.EndUpdateResourceW(h, False):
        raise OSError(ctypes.get_last_error(), "EndUpdateResource")
    print(f"wrote {out} ({len(images)} icon sizes)")


if __name__ == "__main__":
    main()
