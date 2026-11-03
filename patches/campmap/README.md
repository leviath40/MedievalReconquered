# Campaign map

Replaces the campaign map painting with `map_src.jpg` (1200x896).

    python patches\campmap\build_map.py [--low]

Upscales the source 4x with Upscayl's `ultrasharp-4x` model, resizes it to 4096x3072 and writes
`textures/campmap/MapTex2.tga` (24-bit uncompressed TGA, bottom-up, BGR: the game's own format).
`--low` also writes the 2048x1536 `MapTex.tga`, which the game only uses as a low-memory fallback
(report §12). Takes ~12 s.

Needs Upscayl (`winget install Upscayl.Upscayl`); the script runs its command-line upscaler
(`upscayl-bin.exe`, `--upscayl` to point at another install). Of the Upscayl models, ultrasharp gave
the sharpest result on this art (high-fidelity, remacri and digital-art are softer).

Only the painting changes: provinces, coastlines and land/sea come from the lookup maps
(`LukMap*.Lbm`, `LUKUPMAP*.LBM`), so the new art is not guaranteed to line up exactly.
The Viking Invasion map (`textures/campmap/Vikings/`) is unchanged.
Originals: `MedievalOriginal/Medieval Original/textures/campmap/`.
