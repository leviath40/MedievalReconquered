"""Build the borderless d3d9.dll proxy with Zig and install it in the game folder.

    python patches\\borderless\\build.py            # build + install d3d9.dll, create the ini if missing
    python patches\\borderless\\build.py --remove   # uninstall (delete d3d9.dll from the game folder)
"""
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME_DIR = HERE.parent.parent
OUT = HERE / "build" / "d3d9.dll"
INI = GAME_DIR / "MedievalReconquered.ini"
DEFAULT_INI = """[Display]
; 1 = borderless fullscreen (desktop resolution, instant alt-tab), 0 = original exclusive fullscreen
Borderless=1
; 1 = keep the game's aspect ratio (black bars), 0 = stretch to the whole screen
KeepAspect=1
; 1 = write borderless.log (for debugging)
Log=0
; campaign map / battle resolution, 0 = keep the game's Video menu setting (set by the launcher)
CampaignWidth=0
CampaignHeight=0
BattleWidth=0
BattleHeight=0
"""


def find_zig():
    z = shutil.which("zig")
    if z:
        return z
    pat = os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\zig.zig_*\*\zig.exe")
    hits = sorted(glob.glob(pat))
    if hits:
        return hits[-1]
    sys.exit("zig not found (winget install zig.zig)")


FRONTEND = GAME_DIR / "Loc" / "Eng" / "FRONTEND.TXT"
DUST_OLD = b'["Graphics_Menu_Dustswitch_Title"]\r\n{"Dust"}'
DUST_NEW = b'["Graphics_Menu_Dustswitch_Title"]\r\n{"Borderless Fullscreen (restart)"}'


def set_menu_text(install):
    """The Video menu switch reuses the unused Dust text; rename it (or restore it)."""
    data = FRONTEND.read_bytes()
    old, new = (DUST_OLD, DUST_NEW) if install else (DUST_NEW, DUST_OLD)
    if old in data:
        FRONTEND.write_bytes(data.replace(old, new, 1))
        print(f"{'renamed' if install else 'restored'} the Dust text in {FRONTEND.name}")
    elif new not in data:
        print(f"warning: Dust entry not found in {FRONTEND}")


def main():
    target = GAME_DIR / "d3d9.dll"
    if "--remove" in sys.argv:
        if target.exists():
            target.unlink()
        set_menu_text(False)
        print("removed d3d9.dll (game back to original exclusive fullscreen)")
        return
    OUT.parent.mkdir(exist_ok=True)
    cmd = [find_zig(), "cc", "-target", "x86-windows-gnu", "-shared", "-O2", "-s",
           "-o", str(OUT), str(HERE / "d3d9_proxy.c"), str(HERE / "d3d9.def"), "-luser32", "-lgdi32", "-lwinmm"]
    subprocess.run(cmd, check=True)
    shutil.copy2(OUT, target)
    print(f"installed {target}")
    set_menu_text(True)
    if not INI.exists():
        INI.write_text(DEFAULT_INI)
        print(f"created {INI}")


if __name__ == "__main__":
    main()
