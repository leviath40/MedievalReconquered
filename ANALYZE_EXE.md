# Task: first REA + Ghidra analysis of Medieval_TW.exe

Work in this folder: this repository (the working game install)

## Rules
- Analyse `MedievalReconquered\Medieval_TW.exe` only.
- Never write anything in `../MedievalOriginal` (read-only reference copy of the game).
- Do not modify `Medieval_TW.exe`. This task is analysis only, no patching.
- Keep Ghidra project/cache files outside both game folders (REA's default location is fine).
- Save all results under `MedievalReconquered\analysis\`.

## Steps
1. Confirm the REA MCP tools are available in this session. If not, stop and say so.
2. Record the binary's basics: SHA-256, size, PE headers (32/64-bit, compiler/linker version, timestamp, sections), and imported DLLs/functions.
3. Open it with `provider_id` = `ghidra`. The first call waits for Ghidra's full auto-analysis of a 5 MB, 2002-era x86 binary and can take a long time. Do not cancel or retry it partway through: cancelling shuts Ghidra down and the next call starts over.
4. Map the program:
   - Function count, and the main subsystems you can identify (campaign map, battle, AI, UI/menus, sound, rendering, file loading, multiplayer/GameSpy, Steam).
   - Notable strings, especially the names of data files the game reads (`CRUSADERS_UNIT_PROD11.TXT`, `CRUSADER_BUILD_PROD13.TXT`, `VIKINGS_*`, `PROJECTILESTATS.TXT`, `MODELDAMAGE.TXT`, `FormationData\*`, `Loc\Eng\*`, `Medieval.Cfg`, etc.).
5. For the unit file (`CRUSADERS_UNIT_PROD11.TXT`), find the function(s) that load and parse it. Decompile them and explain:
   - which columns/fields are read, in what order, and their types;
   - any hard-coded limits (maximum number of units, factions, string lengths, array sizes);
   - what happens on a malformed line.
6. Do the same, more briefly, for `CRUSADER_BUILD_PROD13.TXT` and `Medieval.Cfg`.
7. List hard-coded limits or constants that are likely to matter for modding (unit/faction/province/building caps, army size, unit size scaling, etc.), each with its address and the evidence for it.
8. Close the analysis session, and confirm that no Ghidra or Java process is left running.

## Deliverable
Write `analysis\MedievalReconquered_overview.md` containing:
- binary facts (step 2);
- subsystem map (step 4);
- file-format notes for each data file (steps 5–6), with function addresses and short decompiled excerpts as evidence;
- table of hard-coded limits (step 7);
- open questions and suggested next investigations.

Mark every claim as **confirmed** (seen in decompiled code) or **inferred** (from names/strings only).
