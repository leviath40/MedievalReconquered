# Logo

Replaces the game logo on the main menu and the loading screen with `logo_src.png`.

    python patches\logo\build_logo.py [--preview DIR]

Reads the untouched originals from `MedievalOriginal/` and writes `frontend_files/Main_Menu/MTW_LOGO.BIF`,
`frontend_files/Main_Menu/parch322x536_no_border.BIF` (menu parchment with the scroll banner replaced by plain parchment),
`frontend_files/Load Screen.tga` and `frontend_files/Load Screen.BIF`. The white background of the source is keyed
out; the old loading-screen logo is covered with background texture first.
It also writes `frontend_files/Main_Menu/MTW_LOGO_HD.raw`: a 4x logo the d3d9 proxy draws at screen resolution
over the menu sprite when menu upscaling is on (only where the frame still shows the sprite, so the cursor and
other screens are left alone). BIF format: report §9, codec `tools/bif.py`.
