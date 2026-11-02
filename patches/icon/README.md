# Icon

`MedievalReconquered.ico` (16 to 256 px) is the icon of the launcher and of the game exe.

```
python patches\icon\apply_icon.py                          # writes MedievalReconquered_icon.exe
python tools\smoke_test.py --exe MedievalReconquered_icon.exe
```

`apply_icon.py` copies the exe and replaces its only icon group (RT_GROUP_ICON 111, one 32×32
16-colour image) with the images of the `.ico`, using the Windows resource API. Only `.rsrc`
changes: it is the last section, with nothing after it, and lies outside the Proteus range
(0xF3C000–0xF45000). The input exe is never modified; swap the copy in yourself once tested.

Windows caches icons: Explorer may show the old one until its icon cache refreshes.
