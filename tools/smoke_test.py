"""Smoke test for the Medieval: Total War exe. Run it after every exe change.

Level 1 (static, always):  the PE parses, the entry point is unchanged, and the Proteus hook
                           sites, the import stubs and the Proteus area (report section 5) are
                           byte-identical to MedievalOriginal.
Level 2 (boot, default):   launches the exe, waits for a responsive "Medieval: Total War" window,
                           and checks the files the game writes at startup. Then it kills the game.
Level 3 (--campaign):      same launch, but you play: start a campaign, then quit the game
                           normally. The script then checks the campaign-map and clean-exit files.

Usage (in MedievalReconquered/):
    python tools\\smoke_test.py                     # levels 1 + 2 on MedievalReconquered.exe
    python tools\\smoke_test.py --static-only       # level 1 only
    python tools\\smoke_test.py --campaign          # levels 1 + 3
    python tools\\smoke_test.py --exe Patched.exe   # test another exe (put it in the game folder)

Exit code: 0 = all checks passed (warnings allowed), 1 = a check failed, 2 = setup error.
Stdlib only. Read-only on MedievalOriginal.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent
ORIGINAL_EXE = GAME_DIR.parent / "MedievalOriginal" / "Medieval Original" / "Medieval_TW.exe"
WINDOW_TITLE = "Medieval: Total War"
ORIGINAL_SHA256 = "23724b034f8c97094cecd5560f053864a475a88adad077c046b2beb79331ace5"
TIME_SLACK = 2.0  # seconds of tolerance on file timestamps

# Report section 5.2: the 23 call/jmp sites in the original .text that enter Proteus (5 bytes each).
HOOK_SITES = [
    0x73EC2E, 0x73EB98, 0x73EC34, 0x73EC3A,             # import stubs
    0x41806E,                                           # CoCreateInstance wrapper
    0x73D47E,                                           # RLE sprite blitter
    0x411E62,                                           # colour key
    0x41E993, 0x41E99A, 0x41F4C6, 0x4C0389, 0x573FEB, 0x57423B,
    0x59707E, 0x5972A6, 0x6DCBB9, 0x6DCBC3, 0x6DCCA6, 0x6DCCBD,  # cursor stubs
    0x41DFCA, 0x779DAF, 0x779DC1,                       # callback swap
    0x76E4B8,                                           # CRT exit
]
IMPORT_STUBS = (0x73EB8C, 0x73EC40)   # [start, end)
PROTEUS_START = 0xF3C000
PROTEUS_END = 0xF45000                # Proteus .text + .data; .rsrc (icons) may change

results = []  # (status, message)


def report(status, msg):
    results.append((status, msg))
    print(f"[{status}] {msg}")


# ---------------------------------------------------------------- level 1: static

class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("no MZ header")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsize = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        self.entry = struct.unpack_from("<I", data, opt + 16)[0]
        self.base = struct.unpack_from("<I", data, opt + 28)[0]
        self.sections = []
        for i in range(nsec):
            o = opt + optsize + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((name, self.base + va, max(vsize, rsize), raw, rsize))

    def read(self, va, n):
        """Read n bytes at va; may span sections. Gaps between sections read as zero."""
        out = bytearray(n)
        covered = 0
        for _, sva, size, raw, rsize in self.sections:
            lo, hi = max(va, sva), min(va + n, sva + size)
            if lo >= hi:
                continue
            covered += hi - lo
            off = lo - sva
            chunk = self.data[raw + off: raw + min(off + hi - lo, rsize)] if off < rsize else b""
            out[lo - va: lo - va + len(chunk)] = chunk
        if covered == 0:
            raise ValueError(f"VA {va:#x} not in any section")
        return bytes(out)

    def end_va(self):
        return max(sva + size for _, sva, size, _, _ in self.sections)


def static_checks(exe):
    data = exe.read_bytes()
    report("INFO", f"{exe.name}: {len(data):,} bytes, SHA-256 {hashlib.sha256(data).hexdigest()}")
    try:
        new = PE(data)
    except ValueError as e:
        report("FAIL", f"PE header: {e}")
        return
    report("PASS", f"PE parses ({len(new.sections)} sections)")
    if not ORIGINAL_EXE.is_file():
        report("WARN", f"original exe not found at {ORIGINAL_EXE}; protected-region checks skipped")
        return
    orig_data = ORIGINAL_EXE.read_bytes()
    if hashlib.sha256(orig_data).hexdigest() != ORIGINAL_SHA256:
        report("WARN", "MedievalOriginal exe hash differs from the analysed one")
    orig = PE(orig_data)

    report("PASS" if new.entry == orig.entry else "FAIL",
           f"entry point {new.base + new.entry:#x} (original {orig.base + orig.entry:#x})")

    bad = []
    for va in HOOK_SITES:
        try:
            if new.read(va, 5) != orig.read(va, 5):
                bad.append(f"{va:#x}")
        except ValueError:
            bad.append(f"{va:#x} (unmapped)")
    report("FAIL" if bad else "PASS",
           f"{len(HOOK_SITES)} Proteus hook sites unchanged" + (f": changed {', '.join(bad)}" if bad else ""))

    lo, hi = IMPORT_STUBS
    same = new.read(lo, hi - lo) == orig.read(lo, hi - lo)
    report("PASS" if same else "FAIL", f"import stubs {lo:#x}-{hi:#x} unchanged")

    end = PROTEUS_END
    try:
        same = new.read(PROTEUS_START, end - PROTEUS_START) == orig.read(PROTEUS_START, end - PROTEUS_START)
    except ValueError:
        same = False
    report("PASS" if same else "FAIL", f"Proteus area {PROTEUS_START:#x}-{end:#x} unchanged")


# ---------------------------------------------------------------- Win32 helpers

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32")
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.IsHungAppWindow.argtypes = [wt.HWND]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetDC.argtypes = [wt.HWND]
user32.GetDC.restype = wt.HDC
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p,
                            ctypes.c_void_p, wt.UINT]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]


def windows(pid=None):
    """(hwnd, title) of visible top-level windows, optionally only those of pid."""
    found = []

    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            p = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if pid is None or p.value == pid:
                buf = ctypes.create_unicode_buffer(256)
                user32.GetWindowTextW(hwnd, buf, 256)
                found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def nonblack_ratio(hwnd):
    """Fraction of non-black pixels in the window's screen area (GDI capture), or None."""
    r = wt.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(r)):
        return None
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    sdc = user32.GetDC(None)
    mdc = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    try:
        gdi32.BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, 0x00CC0020)  # SRCCOPY
        hdr = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        bmi = ctypes.create_string_buffer(hdr, 44)
        buf = ctypes.create_string_buffer(w * h * 4)
        if not gdi32.GetDIBits(mdc, bmp, 0, h, buf, bmi, 0):
            return None
        px = buf.raw
        step = max(1, (w * h) // 20000)
        sample = range(0, w * h, step)
        lit = sum(1 for i in sample if px[i * 4] > 16 or px[i * 4 + 1] > 16 or px[i * 4 + 2] > 16)
        return lit / len(sample)
    finally:
        gdi32.SelectObject(mdc, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(None, sdc)


# ---------------------------------------------------------------- level 2/3: runtime

def fresh(name, start):
    p = GAME_DIR / name
    return p.is_file() and p.stat().st_mtime >= start - TIME_SLACK


def check_error_log(start):
    p = GAME_DIR / "error.log"
    if fresh("error.log", start):
        report("FAIL", "error.log was rewritten (the game logged errors):")
        for line in p.read_text(errors="replace").splitlines()[:20]:
            if line.strip():
                print("       " + line)
    else:
        report("PASS", "error.log not rewritten (no errors logged)")


def check_proteus(start):
    p = GAME_DIR / "proteus.txt"
    if not p.is_file():
        report("FAIL", "proteus.txt missing")
        return
    text = p.read_text(errors="replace")
    sessions = list(re.finditer(r"Session start: (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)", text))
    if not sessions:
        report("FAIL", "proteus.txt has no session header")
        return
    last = sessions[-1]
    t = time.mktime(time.strptime(last.group(1), "%Y-%m-%d %H:%M:%S"))
    if t < start - TIME_SLACK:
        report("FAIL", f"proteus.txt has no new session (last {last.group(1)})")
        return
    m = re.search(r"Depth buffer: (\S+)", text[last.end():])
    report("PASS" if m else "FAIL",
           f"Proteus started (session {last.group(1)}, depth buffer {m.group(1) if m else 'MISSING'})")


def wait_for_window(proc, timeout):
    t_end = time.time() + timeout
    while time.time() < t_end:
        if proc.poll() is not None:
            return None
        for hwnd, title in windows(proc.pid):
            if title == WINDOW_TITLE:
                return hwnd
        time.sleep(0.5)
    return None


def boot_checks(proc, start, args):
    hwnd = wait_for_window(proc, args.timeout)
    if hwnd is None:
        if proc.poll() is not None:
            report("FAIL", f"game exited during startup (exit code {proc.returncode})")
        else:
            report("FAIL", f"no '{WINDOW_TITLE}' window after {args.timeout}s")
        return False
    report("PASS", f"window '{WINDOW_TITLE}' appeared after {time.time() - start:.1f}s")

    time.sleep(args.hold)
    if proc.poll() is not None:
        report("FAIL", f"game exited {args.hold}s after its window appeared (exit code {proc.returncode})")
        return False
    report("PASS", f"still running {args.hold}s after the window appeared")
    report("FAIL" if user32.IsHungAppWindow(hwnd) else "PASS", "window is responding")

    ratio = nonblack_ratio(hwnd)
    if ratio is None:
        report("WARN", "could not capture the window")
    elif ratio < 0.02:
        report("WARN", f"window looks black ({ratio:.0%} lit pixels); capture may not see fullscreen D3D")
    else:
        report("PASS", f"window shows an image ({ratio:.0%} lit pixels)")

    check_proteus(start)
    for name in ("report.txt", "ZVerify.Txt"):
        report("PASS" if fresh(name, start) else "FAIL", f"{name} created at startup")
    return True


def campaign_checks(start):
    report("PASS" if fresh("ZStrat.TXT", start) else "FAIL", "ZStrat.TXT written (campaign map loaded)")
    zp = GAME_DIR / "ZPostStrat.TXT"
    if not fresh("ZPostStrat.TXT", start):
        report("FAIL", "ZPostStrat.TXT not written (campaign map not shut down cleanly)")
    else:
        lines = zp.read_text(errors="replace").splitlines()
        m = re.match(r"Total = (\d+), free = (\d+)", lines[0] if lines else "")
        if not m:
            report("FAIL", "ZPostStrat.TXT: unexpected header")
        else:
            total = int(m.group(1))
            short = [ln for ln in lines[1:]
                     if (n := re.search(r"Newfree = (\d+)", ln)) and int(n.group(1)) != total]
            report("FAIL" if short else "PASS",
                   f"ZPostStrat.TXT: texture memory fully freed ({len(lines) - 1} entries"
                   + (f", {len(short)} short" if short else "") + ")")
    report("PASS" if fresh("Medieval.ver", start) else "FAIL", "Medieval.ver written on exit")
    report("PASS" if fresh(r"SaveGames\escapesave.dat", start) else "WARN",
           "SaveGames\\escapesave.dat written on exit")
    check_error_log(start)


def runtime(exe, args):
    if any(t == WINDOW_TITLE for _, t in windows()):
        report("FAIL", "the game is already running; close it first")
        return
    start = time.time()
    proc = subprocess.Popen([str(exe)], cwd=str(GAME_DIR))
    report("INFO", f"launched {exe.name} (pid {proc.pid})")
    try:
        ok = boot_checks(proc, start, args)
        if args.campaign and ok:
            print(f"\n>>> Start a campaign, wait for the campaign map, then quit the game normally."
                  f"\n>>> Waiting up to {args.wait // 60} min for the game to exit...\n")
            try:
                proc.wait(args.wait)
            except subprocess.TimeoutExpired:
                report("FAIL", "game still running at the end of the wait")
                return
            report("PASS" if proc.returncode == 0 else "WARN", f"game exited with code {proc.returncode}")
            campaign_checks(start)
        elif not args.campaign:
            check_error_log(start)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
            report("INFO", "game killed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", default="MedievalReconquered.exe", help="exe to test (relative to the game folder)")
    ap.add_argument("--static-only", action="store_true", help="level 1 only")
    ap.add_argument("--campaign", action="store_true", help="level 3: you play to the campaign map and quit")
    ap.add_argument("--timeout", type=int, default=30, help="seconds to wait for the window (default 30)")
    ap.add_argument("--hold", type=int, default=10, help="seconds the game must stay up (default 10)")
    ap.add_argument("--wait", type=int, default=1800, help="campaign mode: seconds to wait for exit")
    args = ap.parse_args()

    exe = (GAME_DIR / args.exe).resolve()
    if not exe.is_file():
        print(f"exe not found: {exe}")
        return 2
    if exe.parent != GAME_DIR:
        print("note: the exe is not in the game folder; it is run with the game folder as working dir")

    print("== Level 1: static ==")
    static_checks(exe)
    if not args.static_only:
        print("\n== Level 3: campaign ==" if args.campaign else "\n== Level 2: boot ==")
        runtime(exe, args)

    fails = sum(1 for s, _ in results if s == "FAIL")
    warns = sum(1 for s, _ in results if s == "WARN")
    print(f"\n{'FAILED' if fails else 'PASSED'}: {fails} failed, {warns} warnings")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
