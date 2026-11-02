# d3d9.dll proxy

A `d3d9.dll` in the game folder that sits between the game and Direct3D 9. The exe is not modified:
everything is done in memory at load time. Features:

- borderless fullscreen, with an on/off switch in the in-game **Options → Video** menu;
- campaign/battle resolution override (set by the launcher);
- FPS counter and frame rate cap;
- battle performance fixes (`BatchUI`, `FastDraw`, report §11) and an always-on profiler (`profile.log`);
- smooth shader upscaling of the 800x600 menus, with the menu logo drawn at full resolution.

Source: one file per feature (`proxy.c` core, `fps.c`, `profile.c`, `fastdraw.c`, `upscale.c`,
`game_patches.c`, shared `proxy.h`); the overview comment at the top of `proxy.c` lists them.

## Install / uninstall

Requires [Zig](https://ziglang.org/) (`winget install zig.zig`) and Python 3.

```
python patches\d3d9_proxy\build.py            # build, install d3d9.dll, set the menu text
python patches\d3d9_proxy\build.py --remove   # uninstall, restore the menu text
python tools\smoke_test.py                    # check the game still boots
```

`build.py` also renames the unused "Dust" entry in `Loc/Eng/FRONTEND.TXT` to
"Borderless Fullscreen (restart)", the label of the new switch, and creates
`MedievalReconquered.ini` if it is missing.

## Settings (`MedievalReconquered.ini`, next to the exe)

```
[Display]
Borderless=1   ; 1 = borderless fullscreen, 0 = original exclusive fullscreen (also set by the Video menu switch)
KeepAspect=1   ; 1 = keep the game's aspect ratio (black bars), 0 = stretch to the whole screen
ShowFPS=0      ; 1 = framerate counter at the top right (both display modes)
FPSLimit=0     ; frame rate cap, e.g. 60 (0 = off; both display modes)
BatchUI=1      ; 1 = battle UI panels: one back-buffer lock per panel group (report §11)
Upscale=2      ; borderless: upscale low-res screens (the 800x600 menus) with a shader: 0 = plain stretch, 1 = sharp, 2 = smooth (default)
DumpFrame=0    ; debug: 1 = save one raw back buffer to frame.raw
FastDraw=1     ; 1 = DrawIndexedPrimitiveUP replayed through dynamic buffers (report §11)
Profile=1      ; 1 = write profile.log (frame-time breakdown; on by default, restarts past 4 MB)
Log=0          ; 1 = write borderless.log
CampaignWidth=0    ; campaign map resolution, 0 = keep the game's Video menu setting
CampaignHeight=0
BattleWidth=0      ; battle resolution, 0 = keep the game's Video menu setting
BattleHeight=0
```

Changes made in the Video menu apply at the next start. The resolution keys are normally set by
the launcher (`patches/launcher/`). They are applied once at startup, so the Video menu sliders
still work during a session. A resolution the game does not offer is ignored (logged).

## How it works

- The exe imports `D3D9.dll!Direct3DCreate9`, so Windows loads this proxy from the game folder.
  It forwards to the real `System32\d3d9.dll`.
- The game window is already a borderless `WS_POPUP` window. Exclusive fullscreen comes from
  Proteus creating the D3D9 device with `Windowed = FALSE`. The proxy hooks
  `IDirect3D9::CreateDevice` and `IDirect3DDevice9::Reset` to create a windowed device instead,
  and keeps the window at the size of its monitor.
- `IDirect3DDevice9::Present` scales the game-resolution back buffer to the monitor (letterboxed
  with `KeepAspect=1`). Mouse input is converted between monitor and game pixels: window mouse
  messages, `GetCursorPos`, `SetCursorPos` and `ClipCursor` (exe and Proteus import tables).
- Video menu: the menu is a static table of 17 widget records. Before the exe's static
  initialisers run, the proxy copies it with a label and a switch appended (cloned from the
  Wildlife ones) and points the game at the copy. The switch callback mirrors the Smoke switch
  and writes `Borderless=` to the ini. Details and addresses: `analysis/MedievalReconquered_overview.md` §7.
- Resolution override: `Medieval.Cfg` stores the campaign/battle resolutions as indices into
  mode lists the game builds at startup. The proxy hooks the instruction that reads them
  (0x6DE4BE) and writes the indices of the ini resolutions first. Details: report §7.3.
- The proxy checks the exe layout first. On a different exe it leaves the menu alone and logs it.
