# HD icons

Sharper 2D sprites (unit/building/agent cards, info pictures, portraits, shields, buttons, parchment
panels, menus) on the screens the d3d9 proxy upscales: the 800x600 menus and the campaign map when it
runs below the monitor resolution.

    python patches\hd_icons\build_icons.py        then   HDIcons=1 in MedievalReconquered.ini

`build_icons.py` decodes every `.BIF` in `campmap` and `frontend_files` (not `campmap\pieces`, drawn in
3D), upscales each frame with Upscayl's `ultrasharp-4x` (4x, or 2x for sprites of 200 px or more;
sprites thinner than 16 px are skipped) and writes `hd_icons/<hash>_<frame>.png` in the game folder
(~2,600 files, ~130 MB, a few minutes). Needs Upscayl (`winget install Upscayl.Upscayl`).
The files are derived from the game's art: they are git-ignored and never committed.

In game, the proxy hooks the sprite blitter (report §13), and in its upscaling pass draws the HD copy of
each sprite over it, only on pixels that still show the original sprite, so text, highlights and the
cursor drawn on top are kept. Sprites made at runtime (portraits converted from `.tga`, recoloured unit
cards) have no file on disk: the proxy saves them once to `hd_icons/_pending/`, and the next run of the
script upscales them. Run it again after playing to pick those up.
