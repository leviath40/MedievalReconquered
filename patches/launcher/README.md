# Launcher

`MedievalReconquered_Launcher.exe`: pick the campaign map resolution, the battle resolution and
the display mode (Fullscreen or Borderless Fullscreen) and the FPS counter, then start the game.

## Install / uninstall

Requires [Zig](https://ziglang.org/) (`winget install zig.zig`), Python 3 and the borderless
proxy (`patches/borderless/`), which applies the settings.

```
python patches\borderless\build.py          # the d3d9.dll proxy (needed)
python patches\launcher\build.py            # build, install MedievalReconquered_Launcher.exe
python patches\launcher\build.py --remove   # uninstall
```

The icon is `patches/icon/MedievalReconquered.ico`. The title logo is `logo.bmp` (32-bit BGRA with alpha, 300×74),
embedded as resource 2.

## How it works

- Background: the game's main-menu parchment (`frontend_files/default_parchment_wide.tga`), read
  from the game folder at run time, so no game art is in the repo or the launcher binary. Without
  the file the launcher uses the plain Windows look.
- Lists the monitor's display modes (800×600 and up) plus "Game setting (Video menu)".
- **FPS counter** checkbox (off by default) for the proxy's framerate counter.
- **Play** writes `CampaignWidth`/`CampaignHeight`, `BattleWidth`/`BattleHeight`, `Borderless` and `ShowFPS`
  to `MedievalReconquered.ini`, then starts `MedievalReconquered.exe` and closes.
- The proxy applies them at startup (see `patches/borderless/README.md` and report §7.3).
