# HD battle textures

AI-upscaled battle ground, skies, trees, buildings and castles. `build_textures.py` writes them to `hd_textures/` in
the game folder (about 700 MB, generated, never committed), and the d3d9 proxy swaps them in with `HDTextures=1`.
Soldiers, horses, shields, weapons, flags and effects are left as they are.

## Build

Needs Upscayl and Pillow (`pip install pillow`), run from the repo root:

```
python patches\hd_battle\build_textures.py
```

About 15 minutes. Finished pictures are skipped, so it can be stopped and rerun; `--force` rebuilds them all.

## What it does

| Set | Source | Output |
|---|---|---|
| Ground | `Textures/Ground`, 256x256 | 1024x1024 JPEG, edges blended |
| Skies | `Textures/Backdrops`, 512x512 | 2048x2048 JPEG, edges blended |
| Models | `Models`, `NewModels`, mostly 64x64 | up to 128x128: two passes to at most 1024, larger: 4x; PNG if they have see-through parts |
| Castle pages | `0caspg1/2`, `0Iaspg1/2`, one 256x256 page for all walls | 2048x2048, two passes, alpha scaled separately |
| Trees | four 128x128 files in a 2x2 page, as the game builds it | 1024x1024 PNG |

Ground and sky pictures fade to the plain low-res picture at their edges, so neighbouring tiles meet on the same
colours. The hash the proxy looks for is worked out from the file, see `to_game()`. How it all works: report section 15.
