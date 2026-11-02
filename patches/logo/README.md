# Logo

Replaces the game logo on the main menu and the loading screen with `logo_src.png`.

    python patches\logo\build_logo.py [--preview DIR]

Reads the untouched originals from `MedievalOriginal/` and writes `frontend_files/Main_Menu/MTW_LOGO.BIF`,
`frontend_files/Load Screen.tga` and `frontend_files/Load Screen.BIF`. The white background of the source is keyed
out; the old loading-screen logo is covered with background texture first. BIF format: report §9, codec `tools/bif.py`.
