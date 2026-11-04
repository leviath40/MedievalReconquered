# MedievalReconquered
Small project to make a better old school medieval game: fixes and improvements for
Medieval: Total War (+ Viking Invasion). No game files are in this repository, only our tools,
patch scripts and sources; they are applied to your own install.

## Changes so far

### Display and launcher
- **Borderless fullscreen** (`patches/d3d9_proxy/`, a `d3d9.dll` proxy): instant alt-tab, aspect
  ratio kept, mouse mapped correctly; on/off switch added to the game's Video options menu.
- **Launcher** (`patches/launcher/`): campaign map and battle resolution (any monitor mode),
  display mode, FPS counter, 60 FPS cap. (Fixed: campaign and battle resolutions were applied
  the wrong way round.)
- **Sharper menus**: the 800x600 menus are upscaled with a smooth GPU shader instead of a plain
  stretch (borderless mode).
- **No more black bars beside the menus**: the sides of the 4:3 menus are filled with the game's
  dark wood texture, with a soft shadow (`MenuSides=1`).
- **FPS counter** at the top right of the screen (optional).
- **Frame rate cap** at 60 FPS (optional).

### Performance
- **Battles about 3x faster**: big battles (16 v 16, max unit size, 1440p) went from ~26 FPS
  (dips under 20) to ~80 FPS (never under 60):
  - battle UI panels drawn under one back-buffer lock instead of one each (`BatchUI`);
  - the engine's slow indexed draws replayed through GPU buffers (`FastDraw`).
- **Large Address Aware** exe (`patches/laa/`): up to 4 GB of memory instead of 2 GB.
- **Built-in profiler** (`profile.log`), always on, to catch regressions and find new bottlenecks.

### Look and feel
- **New logo** on the main menu (drawn at full screen resolution with menu upscaling) and the loading screen; main-menu parchment without the scroll
  banner (`patches/logo/`).
- **New icon** for the game and the launcher (`patches/icon/`).
- **New campaign map** painting, AI-upscaled to the game's 4096x3072 (`patches/campmap/`).
- **HD icons** on the menus and the campaign map: unit/building cards, portraits, info pictures,
  shields, buttons and panels are AI-upscaled and drawn at full resolution (`patches/hd_icons/`, `HDIcons=1`).
- **Bigger soldier icons** on the battle unit cards: the widescreen layout bug that squeezed them is fixed and the pictures are AI-upscaled (`CardIcons=1`, `patches/hd_icons/`).
- **Boot intro skipped** (the game's own `skipcredits.txt` switch).

### Tools and research
- **Exe analysis report** (`analysis/MedievalReconquered_overview.md`): file formats, limits,
  the Proteus graphics layer, display, performance.
- `tools/check_prodfiles.py`: checks unit/building production files the way the game reads them.
- `tools/smoke_test.py`: boot test after any exe change.
- `tools/bif.py`: reads and writes the game's `.BIF` sprite format.
- **Code clean-up**: the `d3d9.dll` proxy (`patches/d3d9_proxy/`) split into one module per feature.
