# HD icons

Sharper 2D sprites (unit/building/agent cards, info pictures, portraits, shields, buttons, parchment
panels, menus) on the screens the d3d9 proxy upscales: the 800x600 menus and the campaign map when it
runs below the monitor resolution.

    python patches\hd_icons\build_icons.py        then   HDIcons=1 in MedievalReconquered.ini

`build_icons.py` decodes every `.BIF` in `campmap` and `frontend_files` (not `campmap\pieces`, drawn in
3D), upscales each frame with Upscayl's `ultrasharp-4x` (4x, or 2x for sprites of 200 px or more;
sprites thinner than 16 px are skipped) and writes `hd_icons/<hash>_<frame>.png` in the game folder
(~2,600 files, ~540 MB, about 15 minutes). Needs Upscayl (`winget install Upscayl.Upscayl`).
The files are derived from the game's art: they are git-ignored and never committed.

After building, `python patches\hd_icons\check_icons.py` compares each small HD sprite with its original
(`--delete` removes bad ones; Upscayl garbled ~1,150 outputs when run with parallel threads, so the script
now runs it single-threaded).

In game, the proxy hooks the sprite blitter (report §13), and in its upscaling pass draws the HD copy of
each sprite over it, only on pixels that still show the original sprite, so text, highlights and the
cursor drawn on top are kept. Sprites made at runtime (portraits converted from `.tga`, recoloured unit
cards) have no file on disk: the proxy saves them once to `hd_icons/_pending/`, and the next run of the
script upscales them. Run it again after playing to pick those up.

Faction colours: the game recolours unit cards per faction, so each faction needs its own files.

    python patches\hd_icons\faction_cards.py        queues every campaign unit card in all 33 colour pairs
    python patches\hd_icons\build_icons.py          upscales the queue
    python patches\hd_icons\build_card_icons.py     battle unit-card pictures (hd_icons/battle/)

The battle pictures are used with `CardIcons=1` and `HDIcons=1` (report section 14). Sprites the proxy sees with no HD copy still land in
`hd_icons/_pending/`: run `build_icons.py` after playing to pick them up.
