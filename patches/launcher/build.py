"""Build the launcher with Zig and install it in the game folder.

    python patches\\launcher\\build.py            # build + install MedievalReconquered_Launcher.exe
    python patches\\launcher\\build.py --remove   # uninstall

The launcher writes Width/Height/Borderless in MedievalReconquered.ini; the d3d9.dll proxy
(patches/d3d9_proxy) applies them, so install that too.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "borderless"))
from build import find_zig, GAME_DIR  # noqa: E402

OUT = HERE / "build" / "MedievalReconquered_Launcher.exe"
ICON = HERE.parent / "icon" / "MedievalReconquered.ico"


def main():
    target = GAME_DIR / OUT.name
    if "--remove" in sys.argv:
        if target.exists():
            target.unlink()
        print(f"removed {target}")
        return
    OUT.parent.mkdir(exist_ok=True)
    # resources: 1 = icon, 2 = title logo (copied next to the .rc: the resource compiler
    # resolves plain file names only)
    (OUT.parent / "app.ico").write_bytes(ICON.read_bytes())
    (OUT.parent / "logo.bmp").write_bytes((HERE / "logo.bmp").read_bytes())
    rc = OUT.parent / "launcher.rc"
    rc.write_text('1 ICON "app.ico"\n2 RCDATA "logo.bmp"\n')
    srcs = [str(HERE / "launcher.c"), str(rc)]
    cmd = [find_zig(), "cc", "-target", "x86-windows-gnu", "-O2", "-s", "-Wl,--subsystem,windows",
           "-o", str(OUT), *srcs, "-luser32", "-lgdi32", "-lmsimg32"]
    subprocess.run(cmd, check=True)
    target.write_bytes(OUT.read_bytes())
    print(f"installed {target}")


if __name__ == "__main__":
    main()
