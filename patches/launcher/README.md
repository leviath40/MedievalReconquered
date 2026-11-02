# Launcher

`MedievalReconquered_Launcher.exe`: pick the campaign map resolution, the battle resolution and
the display mode (Fullscreen or Borderless Fullscreen), then start the game.

## Install / uninstall

Requires [Zig](https://ziglang.org/) (`winget install zig.zig`), Python 3 and the borderless
proxy (`patches/borderless/`), which applies the settings.

```
python patches\borderless\build.py          # the d3d9.dll proxy (needed)
python patches\launcher\build.py            # build, install MedievalReconquered_Launcher.exe
python patches\launcher\build.py --remove   # uninstall
```

The icon is `patches/icon/MedievalReconquered.ico`.

## How it works

- Lists the monitor's display modes (800×600 and up) plus "Game setting (Video menu)".
- **Play** writes `CampaignWidth`/`CampaignHeight`, `BattleWidth`/`BattleHeight` and `Borderless`
  to `MedievalReconquered.ini`, then starts `MedievalReconquered.exe` and closes.
- The proxy applies them at startup (see `patches/borderless/README.md` and report §7.3).
