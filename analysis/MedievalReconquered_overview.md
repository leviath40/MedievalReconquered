# MedievalReconquered.exe (originally Medieval_TW.exe): first static analysis (REA + Ghidra)

- Date: 2026-11-01
- Tools: REA MCP with Ghidra 12.1.4 (default auto-analysis), REA PE-import parser, a small Python PE-header script
- Scope: analysis only. The exe was not modified, and nothing was written to `MedievalOriginal`.
- Evidence: the decompiled functions are saved in `analysis/decompiled/FUN_xxxxxxxx.c`.

Every claim below is marked:
- **[confirmed]** — seen in decompiled code, disassembly bytes or raw header bytes.
- **[inferred]** — based on names, strings or layout only.

---

## 1. Binary facts

| Item | Value |
|---|---|
| File | `MedievalReconquered\MedievalReconquered.exe` (renamed from `Medieval_TW.exe`; the bytes are unchanged. The name only appears in the version resource, and the game doesn't check it) |
| SHA-256 | `23724b034f8c97094cecd5560f053864a475a88adad077c046b2beb79331ace5` |
| Size | 5,140,480 bytes |
| Format | PE32, i386 (machine 0x14C), GUI subsystem, OS/subsystem version 4.0 **[confirmed]** |
| Linker | 6.0, i.e. Visual C++ 6 era **[confirmed linker version; compiler inferred]** |
| PE timestamp | 0x567DD42F = 2015-12-25 23:41:35 UTC. The game is from 2002, so this was almost certainly rewritten when the binary was patched **[inferred]** |
| Image base / entry | 0x400000 / **0xF3E1B0**. The entry point is in the appended second `.text`, not in the original code **[confirmed]** |
| Rich header / debug dir / relocs / TLS | none present **[confirmed]** |
| imphash (pefile) | `4232f8a10584466804d728aa7461f9a1` |
| Ghidra inventory | 8,480 functions, 8,663 defined strings |

### Sections **[confirmed]**

| Name | VA | VSize | Raw | Notes |
|---|---|---|---|---|
| .text | 0x401000 | 0x38258E | 0x383000 | original game code |
| .rdata | 0x784000 | 0x1419D | 0x15000 | import directory is here |
| .data | 0x799000 | **0x7A2D60** (~8 MB) | 0x13E000 | ~6.7 MB of zero-filled BSS, so large static arrays exist |
| .text (2nd) | 0xF3C000 | 0x8000 | 0x8000 | **appended "Proteus Engine" code** (entry point) |
| .data (2nd) | 0xF44000 | 0x1000 | 0x1000 | Proteus data |
| .rsrc | 0xF45000 | 0x7000 | 0x7000 | resources |

### Imports (13 DLLs, 228 functions, no delay-loads) **[confirmed]**

- **KERNEL32**: file I/O, threads, heap, `GetPrivateProfileStringA`, `CreateProcessA`, `IsDebuggerPresent`
- **USER32**: window, input and message box functions
- **GDI32**: fonts and text
- **ADVAPI32**: registry
- **SHELL32**: `ShellExecuteA`
- **ole32**: COM init and `CoCreateInstance`
- **D3D9**: `Direct3DCreate9`
- **DDRAW**: `DirectDrawCreate`, `DirectDrawEnumerateExA`
- **DSOUND**: ordinal 11 = `DirectSoundCreate8` **[inferred]**
- **WINMM**: mmio and waveOut
- **AVIFIL32**: AVI playback
- **WSOCK32**: 28 functions, imported by ordinal
- **BugslayerUtil.dll**: crash stack traces

### The "Proteus Engine" patch layer

- The string `-=* Proteus Engine *=-` and the log file name `proteus.txt` are in the appended section at 0xF3C614–0xF3C620 **[confirmed]**.
- `proteus.txt` in the game folder logs D3D9 caps: VS/PS 3.0, INTZ depth buffer **[confirmed by file content]**.
- The CRT `exit()` path (`FUN_0076e41b`) ends in `FUN_00f3e230`, which is inside the appended section. That function releases a COM object, frees three HMODULEs, then calls `ExitProcess` **[confirmed]**.
- Conclusion: a third-party D3D9 renderer/wrapper has been grafted onto the original DirectDraw/D3D game. It hooks startup (entry point) and shutdown **[inferred]**.
- The `D3D9.dll` import probably comes from this patch, not from the 2002 code **[inferred]**.

### Network and services

- **GameSpy** hosts are present: `gpcm.gamespy.com`, `peerchat.gamespy.com`, `master.gamespy.com`, `gamestats.gamespy.com`, plus the registry key `Software\GameSpy\GameSpy 3D\Registration` **[confirmed strings]**. These services are offline today **[inferred]**.
- **Steam**: no Steam API import and no "steam" string **[confirmed absence in Ghidra's defined strings and the import table]**.

---

## 2. Subsystem map

The binary embeds debug source paths of the form `E:\Mtw_atvi\<file>.cpp`, which are passed to an allocation tracker (`FUN_006ecf10(file, line)`). The groups below come from those file names. The file names themselves are **[confirmed]**; the groupings are **[inferred]**.

| Subsystem | Source files (examples) |
|---|---|
| Startup / main loop / config | `shogun.cpp` (Shogun engine heritage), `main3d_1.cpp`, `frontend_generic.cpp` (Medieval.Cfg), `errmess.cpp`, `LoadSave.Cpp` |
| Production tables (units/buildings) | `Production.cpp`, `Production.h` |
| Campaign (strategy) map | `strategy.cpp`, `japmap.cpp`, `3dmap.cpp`, `startpos.cpp`, `StartposStuff.cpp`, `Stratmap_Loadsave.cpp`, `carmy.cpp`, `ccastle.cpp`, `cregion.cpp`, `cdaimyo.cpp`, `cunit.cpp`, `CCrusade.cpp`, `events.cpp`, `GloryWhole.cpp`, `throne.cpp`, `cmission.cpp`, `CTradeParchment.cpp`, `CSPManager.cpp` |
| Campaign AI | `mapai.cpp`, `mapctrl.cpp` |
| Campaign UI | `cinfopanel.cpp`, `cparchment.cpp`, `cprodparchment.cpp`, `cminimap.cpp`, `ceventpanel.cpp`, `creviewpanel.cpp`, `cstatuspanel.cpp`, `options_panel.cpp`, `PreBattleScreen.cpp`, `JapMenus.Cpp` |
| Battle engine | `BatleMap.cpp`, `battleparse.cpp`, `units.cpp`, `units_combat.cpp`, `unitplan.cpp`, `format.cpp`, `bmunitformation.cpp`, `bmmarch.cpp`, `projectl.cpp`, `BMCatapult.cpp`, `CBMCastleManager.cpp`, `CBMCastleArea.cpp`, `combatstats.cpp`, `bmreplay.cpp`, `bmchecksum.cpp`, `heights.cpp`, `bmboundary.cpp` |
| Battle AI | `BattleDaimyoAIManager.cpp`, `battledaimyoai.cpp`, `battledetachmentai.cpp`, `battledetachmentcastleai.cpp`, `battleformationai.cpp`, `battlemeleeai.cpp`, `battleterrainai.cpp`, `battlegta.cpp`, `BattleGTACastle.cpp`, `gtaenemyanalysis.cpp`, `BattleAiFastReaction.cpp`, `BMAiScriptControl.cpp`, `rectangle_route.cpp` |
| Battle UI / camera | `battleui.cpp`, `battleinterface.cpp`, `bmcamera.cpp`, `bmradar.cpp`, `bmpanel.cpp`, `bmmonicon.cpp`, `bmcontrol.cpp`, `unitinterface.cpp` |
| Rendering / models / effects | `Model.Cpp`, `NewModel.Cpp`, `AnimModel.cpp`, `ModelDatabase.cpp`, `animplay.cpp`, `drawmons.cpp`, `softdraw.cpp`, `texalloc.cpp`, `surface.cpp`, `BMSkyBox.cpp`, `bmweather.cpp`, `BMWeatherParticle*.cpp`, `particle.cpp`, `FireEngine.Cpp`, `treepage.cpp`, `FogPage.cpp`, `Land_Pal.Cpp` |
| Sound / music | `sound.cpp`, `soundmanager.cpp`, `bmsound3d.cpp`, `jukebox.cpp` |
| Video | `mpeg.cpp`, `sub_mpg.cpp` |
| Front-end menus | `main_menu.cpp`, `frontend_*.cpp`, `FactionSelect.cpp`, `ArmySelect.cpp`, `custombattle.cpp`, `historical_*_menu.cpp`, `battle_results_menu.cpp`, `credits.cpp`, `maplist.cpp`, `tutorials.cpp`, `cru_*_tutorial.cpp` |
| Localisation / text | `localizationprog.cpp`, `localizationmanager_lic.cpp`, `parsemanager_lic.cpp`, `stringmapfile.cpp`, `TextList.Cpp`, `monofont.cpp` |
| Multiplayer | `lan_multiplayer.cpp`, `matchmaker.cpp`, `internet.cpp`, `iserver.cpp`, `isocket.cpp`, `ibattle.cpp`, `icommon.cpp`, `default_chatroom_menu.cpp` (GameSpy) |
| File formats | `bifstuff.cpp`, `iffstuff.cpp`, `decomp.cpp`, `tgasave.cpp`, `fstream_lic.cpp` |

### Data files named in the exe **[confirmed strings]**

- **Production**: `crusaders_unit_prod11.txt`, `crusader_build_prod13.txt` (defaults at 0x7EB1F4 / 0x7EB1D8). `VIKINGS_*` is **not** hard-coded: it comes from the startpos file (see 3.4).
- **Battle**:
  - `projectileStats.txt`
  - `Models\ModelDamage.txt`
  - `formationData\GroupFormations.txt`, `HistoricalFormations.txt`, `AiSpecialFormations.txt`, `AiBasicFormations.txt`
  - `.\battle\weather.txt`
  - `textures\men\deadpage coords.txt`, `shields.txt`, `weapons.txt`, `\Aim.txt`
- **Campaign**:
  - `campmap\names\default_heroes.txt`
  - `default_region_specific.txt`, `default_faction_specific.txt`
  - `startpos.txt`, `HistCampaigns.txt`, `glorygoals.txt`
- **Localisation (`Loc\<lang>\`)**: `names.txt`, `descriptions.txt`, `tooltip.txt`, `system.txt`, `Events.txt`, `Battle.txt`, `Frontend.txt`, `Global.txt`, `Historical.txt`, `Weather.txt`, `variables.txt`, `maps.txt`, `changes.txt`, `WarVoice.Txt`, `%s.txt`
- **Config / diagnostics**:
  - `Medieval.Cfg`
  - `Z*.TXT` (texture/video RAM probes and post-battle/strategy logs)
  - `crash.txt`, `report.txt`, `jm_log.txt`
  - `.\dumplog\checksum.txt`
  - `Credits\Credits.Txt`, `skipcredits.txt`, `unlock_tutorials.txt`

---

## 3. Production files (units and buildings)

### 3.1 Call chain **[confirmed]**

```
FUN_00642140   startpos.cpp: reads the campaign startpos file
  └ FUN_0063e5e0  reads "Prodfiles:: UNITPROD <file> BUILDINGPROD <file>" into two 0x104-byte (MAX_PATH) buffers;
                  defaults to crusaders_unit_prod11.txt / crusader_build_prod13.txt
FUN_006f1690(unitFile, buildFile)   switches the active production files if the names changed
  ├ FUN_00739590(1, buildFile) → FUN_00723500   parse BUILDINGS (Production.cpp)
  └ FUN_00739590(0, unitFile)  → FUN_00723cd0   parse UNITS
    If either load fails, it reloads the previously active pair.
```

- `FUN_00739590` refuses to parse units until buildings have been parsed. It shows the message box "Buildings not parsed, they must be…". The flag at `this+0x24` is set at the end of `FUN_00723500` **[confirmed]**.
- `campmap\startpos\VIKING.TXT` contains `Prodfiles:: UNITPROD "vikings_unit_prod.txt" BUILDINGPROD "vikings_build_prod.txt"`. The comment in that file says that all production files must be in the game root **[confirmed file content]**.
- **Modding consequence:** a campaign can point at its own unit and building files without touching the exe.

### 3.2 Shared tokenizer `FUN_00739690` **[confirmed]**

The whole file is read into memory (`GlobalAlloc`) and walked cell by cell. Tokens:

| Return | Meaning |
|---|---|
| 0 | normal cell (terminated by TAB) |
| 5 | last cell of a line (terminated by CR outside quotes) |
| 1 | empty cell |
| 2 | the cell starts with `DATA START` (first occurrence only) |
| 4 | end of file |

- Cells are **TAB-separated**. A `"` toggles quote mode, and a CR inside quotes does not end the row. That is why the multi-line header cells in the shipped files work.
- Leading and trailing spaces and `"` are trimmed. CR/LF inside a cell becomes a space.
- Values *inside* a cell are split by `FUN_00739f60` on space, `"` and `,`. TAB, CR or LF ends the cell.

### 3.3 Unit file (`CRUSADERS_UNIT_PROD11.TXT`), parser `FUN_00723cd0` **[confirmed]**

#### Algorithm

1. The number of cells on the first line is the **column count**. It must equal the length of the handler table at **0x8CBA18** (pairs of handler and name, ending in 0). That table has **52** entries. On a mismatch the game shows an error and exits.
   - **Bug [confirmed]:** the unit check computes `(fileCols < 52) + 1`. A file with *too few* columns therefore reports error 2, "Too **many** data columns". The building check uses `<=` and gets it right.
2. Row count = total cells ÷ columns (integer division). Every row slot is then checked with `FUN_00739f60`, and only rows whose first cell has text are counted **[confirmed]**.
   - Rows are still indexed sequentially, so a blank row in the middle probably shifts which rows are parsed **[inferred]**.
   - Because of the division, one stray TAB anywhere misaligns every later row **[inferred from the arithmetic]**.
3. The parser skips ahead to the `DATA START` marker (error 4 if it is missing), then allocates **one 0x480-byte unit object per row**.
   - Objects form a doubly linked list: next at `+0x47C`, prev at `+0x478`. The head is `DAT_00f32138`, the count `DAT_00f3214c` **[inferred names, confirmed layout]**.
   - The unit index is at `+0x74`.
4. Columns 0 (label) and 1 (type) are parsed first for all rows. Then `FUN_00724e40` rebuilds the list order, and columns 2–51 are parsed.
5. Every handler returns an error code to `FUN_00739900`. **Any non-zero known code shows a message box (title = column name, body = column/row/text) and calls `exit(1337)` (0x539).** There is no recovery and no skipping of bad lines **[confirmed]**.
6. Post-processing:
   - Builds a unit × unit table (`count² × 4` bytes) **[confirmed]**.
   - Allocates per-unit arrays of 1, 8, 0x60 and 4 bytes × count **[confirmed]**.
   - Registers picture filenames for 12 × 6 and 8 × 6 slots (see limits) **[confirmed loop bounds]**.

#### Columns, in file order (handler table at 0x8CBA18) **[confirmed order]**

Abbreviations: *val-list* = `FUN_0073a0b0`, which splits the cell into values, converts each with `atoi`, and returns how many it found.

| # | Column (exe name) | Handler | Storage / behaviour |
|---|---|---|---|
| 0 | identifying_label | 0x731D10 | String copied to `+0x78`; `+0x464/+0x468/+0x46C` = label + `_Singular` / `_Plural` / `_desc` (localisation keys) **[confirmed]** |
| 1 | type | 0x731E60 | Matched against the string table at 0x8CB564 (starts `ARTILLERY`); index stored at `+0x330`; unknown → error 0x26 "Unknown unit type" **[confirmed]** |
| 2 | cost | 0x731EE0 | val-list; first value → `+0x158`; empty → error 8 "too few values" **[confirmed]** |
| 3 | support_cost | 0x731F20 | same template → `+0x338` **[confirmed, bytes]** |
| 4 | production_time | 0x731F60 | → `+0x7C` **[confirmed, bytes]** |
| 5 | honour_handicap | 0x731FA0 | → `+0x33C` **[confirmed, bytes]** |
| 6 | base_strength | 0x731FE0 | → `+0x340` **[confirmed, bytes]** |
| 7 | base_unitsize | 0x732020 | → `+0x344` **[confirmed, bytes]** |
| 8 | honour_step | 0x732060 | → `+0x348` **[confirmed, bytes]** |
| 9 | general_candidate | 0x7320A0 | `PREFERRED`=0 / `OK`=1 / `DISCOURAGED`=2 → `+0x448`; else error 0x18 **[confirmed]** |
| 10 | troop_mix | 0x732230 | not decompiled |
| 11 | faction_advantage | 0x7323A0 | decompiled (`analysis/decompiled`) |
| 12 | region_advantage | 0x7323F0 | decompiled |
| 13 | dojo_advantage | 0x732440 | not decompiled |
| 14 | preferences | 0x7326C0 | not decompiled |
| 15 | speciality_building_influences | 0x7328D0 | not decompiled |
| 16 | buildings_needed | 0x732B50 | not decompiled |
| 17 | class | 0x732EA0 | not decompiled; error 0x27 "Unknown unit class" **[inferred from error table]** |
| 18 | moral_bonus | 0x733010 | val-list; first value → `+0x34C`; empty allowed (= 0) **[confirmed]** |
| 19 | periods | 0x733060 | `ALL_PERIODS` sets periods 0–2; `EARLY` sets period 0; second keyword sets period 1; third keyword sets flag bit 2 at `+0x380`; unknown → 0x28 **[confirmed]**. The 2nd and 3rd keywords are probably `HIGH`/`LATE` **[inferred]** |
| 20 | unitsize_scalable | 0x733C00 | not decompiled |
| 21 | troop_descriptions | 0x733C70 | not decompiled; errors 0x31–0x36 **[inferred]** |
| 22 | invisibility | 0x734960 | not decompiled |
| 23 | specialities | 0x7349D0 | not decompiled |
| 24 | tooltips | 0x734B40 | not decompiled |
| 25 | mount_type | 0x7365E0 | decompiled |
| 26 | body_texpage | 0x734CA0 | not decompiled |
| 27 | faction_leader | 0x734FF0 | not decompiled; error 5 "No LEADER unit found" **[inferred]** |
| 28 | faction_association | 0x735070 | not decompiled; uses `NO_FACTION` / `ALL_FACTIONS` **[confirmed xref]** |
| 29 | region_association | 0x7353B0 | not decompiled |
| 30 | formation_positional_priorities | 0x735560 | not decompiled |
| 31 | formation_task_suitabilities | 0x7356B0 | not decompiled |
| 32 | culture_association | 0x7359D0 | not decompiled |
| 33 | ship_stats | 0x735B40 | not decompiled |
| 34 | supporting_ranks | 0x735CF0 | `atoi` → `+0x64`; empty = 0 **[confirmed, bytes]** |
| 35 | discipline | 0x735D30 | not decompiled |
| 36 | formation_regularity | 0x735DC0 | not decompiled |
| 37 | shield_type | 0x735E60 | not decompiled |
| 38 | melee_shield | 0x735F00 | not decompiled |
| 39 | shield_effectiveness | 0x735F70 | not decompiled |
| 40 | armour_piercing | 0x735FC0 | not decompiled |
| 41 | cav_attack_bonus | 0x736030 | not decompiled |
| 42 | cav_defend_bonus | 0x736080 | not decompiled |
| 43 | fear_factors | 0x7360D0 | decompiled |
| 44 | zeal_modifiers | 0x7361D0 | not decompiled |
| 45 | faith_modifiers | 0x736360 | not decompiled |
| 46 | dismount_type | 0x7364F0 | not decompiled |
| 47 | can_dismount_in_open_battle | 0x736570 | not decompiled |
| 48 | melee_weapon_type | 0x736660 | decompiled |
| 49 | mercenary_usable | 0x736720 | not decompiled |
| 50 | noncampaign_periods | 0x7331A0 | not decompiled |
| 51 | noncampaign_faction_association | 0x735210 | not decompiled |

- **Why some handlers aren't decompiled:** most handlers are reached only through the data table, so Ghidra's auto-analysis did not create functions for them. Their storage offsets are still open, except for the byte-identical integer templates (cols 2–8, 34), which were read directly from the instruction bytes.
- **Shipped files vs. table [confirmed]:**
  - `CRUSADERS_UNIT_PROD11.TXT` and `VIKINGS_UNIT_PROD.TXT` both have exactly 52 columns, with `DATA START` on row 4 and 195 unit rows.
  - The columns appear in the same order as the handler table.
  - 23 faction labels are used (`FN_*`, `ALL_FACTIONS`, `NO_FACTION`).
  - 19 unit types are used (`INFANTRY`, `CAVALRY`, `ARTILLERY`, `SHIP`, agents, `CRUSADE`/`JIHAD`, …).
- **Integer fields [confirmed]:** only the **first** value is used and extra values are silently ignored. No range checks were seen.
- **Non-numeric text** becomes 0 **[inferred: `FUN_0076eec3` looks like CRT `atoi`]**.

#### Malformed input, summary **[confirmed]**

| Problem | Result |
|---|---|
| Wrong column count | error 1/2 (wording inverted for units), then `exit(1337)` |
| No `DATA START` | error 4, then `exit(1337)` |
| Unknown label / enum / bad format | the error table in `FUN_00739900` has 0x45 codes (unknown faction, region, building, period, class, mount, culture, discipline, shield, weapon, …), then `exit(1337)` |
| Empty mandatory integer | error 8, then `exit(1337)` |
| Empty optional fields | moral_bonus and supporting_ranks become 0 |

- The error text is formatted into a **500-byte heap buffer** with the offending cell text. A very long bad cell could overflow it **[confirmed allocation size; overflow inferred]**.

### 3.4 Building file (`CRUSADER_BUILD_PROD13.TXT`), parser `FUN_00723500` **[confirmed unless noted]**

- Same tokenizer, `DATA START` marker and fatal error path as the unit file.
- The handler table at **0x8CBBC0** has **28 columns**, and both shipped building files have 28 columns.

Column order:

| # | Column | # | Column |
|---|---|---|---|
| 0 | num_upgrades | 14 | specific_culture |
| 1 | identifying_label | 15 | loyalty_bonus |
| 2 | localised_text | 16 | upgrade_abilities |
| 3 | type | 17 | tech_level |
| 4 | cost | 18 | required_resources |
| 5 | time | 19 | faction_association |
| 6 | conditions | 20 | glorycondition_association |
| 7 | infopic_filename | 21 | faith_propogation |
| 8 | piece_filename | 22 | income |
| 9 | preferences | 23 | happiness_adjuster |
| 10 | influences | 24 | mercenary_magnetism |
| 11 | frontline_specific | 25 | shady_meeting_place |
| 12 | faction_advantage | 26 | invention_requirements |
| 13 | speciality_influences | 27 | unit_capacity |

- **Upgrades:** column 0 is read with `atoi`, and the parser creates **num_upgrades + 1 building objects of 0x2D8 bytes** for that row. Each upgrade level is its own object.
  - Objects are linked through `+0x27C`, starting at `DAT_00f3213c`. `+0x248` appears to mark upgrade levels **[inferred]**.
- Columns are parsed column by column, and the parser walks the list skipping upgrade-level objects.
- **Upgrade abilities:** each building has a `std::bitset` at `+0x1FC` that is tested for positions 0–5. That gives **6 troop-upgrade ability slots**; a 7th would throw "invalid bitset<N> position" **[confirmed bound; meaning inferred]**.

### 3.5 `Medieval.Cfg`: `FUN_006e8c40` (load), `FUN_006e9810` (save), `FUN_006ea380` (startup) **[confirmed]**

- **Binary, not text:** a fixed **0x8F0 (2,288)-byte** image, which matches the size of the file on disk.
- The first byte must be `','` (0x2C), the version marker.

| Offset | Size | Destination global |
|---|---|---|
| 0x000 | 1 | marker 0x2C |
| 0x004 | 0x26 dwords | `DAT_00e77a54…` (bytes 1 and 2 are flags that switch features at startup, e.g. `FUN_00675f60`, sound/video **[inferred]**) |
| 0x09C | 0x18D dwords | `DAT_00e77aec…` |
| 0x6D0 | 0x0C dwords | `DAT_00e78120…` |
| 0x700 | 0x06 dwords | `DAT_00e78150…` |
| 0x718 | 0x74 dwords | `DAT_00e78168…` |
| 0x8E8 | 2 dwords | `DAT_00e7833c`, `DAT_00e78340` |

- **No size check.** The loader allocates exactly the file size and then copies 0x8F0 bytes, so a truncated file causes a heap over-read **[confirmed]**.
- If the load fails, the game writes defaults back to `Medieval.Cfg`.
- The loader calls `FUN_00409010(buffer)` when `DAT_0079c8e8 != 0` and the extension matches 0x799E28. This might be a decode or transform step **[open]**.

---

## 4. Hard-coded limits and constants relevant to modding

| Limit | Value | Where | Evidence |
|---|---|---|---|
| Unit file column count | exactly 52 | handler table 0x8CBA18, check in `FUN_00723cd0` | **confirmed** |
| Building file column count | exactly 28 | table 0x8CBBC0, check in `FUN_00723500` | **confirmed** |
| Max number of units / buildings | **no fixed cap in the parser** (heap linked lists) | `FUN_00723cd0` / `FUN_00723500` | **confirmed** for the parser. Other subsystems may have fixed arrays **[open]** |
| Unit-pair table | `units² × 4` bytes | `FUN_00723cd0` (Production.cpp line 0x2550) | **confirmed**; grows quadratically |
| Production file name | 260 chars (0x104) | `FUN_00642140`, `FUN_0063e5e0` | **confirmed** |
| Periods | 3 | `FUN_00733060` (`ALL_PERIODS` loop `< 3`) | **confirmed** |
| Default-unit slots | 96 dwords at 0xF31FB8–0xF32138, filled in groups of 3 | end of `FUN_00723cd0` | **confirmed** layout. Most likely **32 factions × 3 periods** (e.g. faction leaders), i.e. a 32-faction ceiling **[inferred]** |
| Building troop-upgrade abilities | 6 | bitset at building `+0x1FC`, `FUN_00723500` | **confirmed** bound |
| Picture-name registration grids | 12 × 6 (buildings) and 8 × 6 (units, index 1 skipped) | `FUN_00723cd0` loops | **confirmed** bounds, meaning unknown |
| Unit object size | 0x480 bytes | `operator_new(0x480)` | **confirmed** |
| Building object size | 0x2D8 bytes | `operator_new(0x2d8)` | **confirmed** |
| Faction list | dynamic vector `DAT_00b8f57c`/`DAT_00b8f580` | `FUN_0056d820` (label → id) | **confirmed** dynamic, but see the 32-slot array above |
| Hard-coded faction/region rules | crusader states (`ID_ANTIOCH`, `ID_EDESSA`, `ID_JERUSALEM`, `ID_TRIPOLI`), `ID_AQUITAINE`/`ID_TOULOUSE`, and a fixed list of 10 Catholic/Muslim factions | `FUN_00560fe0` (crusade logic) | **confirmed**. Renaming these factions or regions silently breaks this logic |
| Production error message buffer | 500 bytes | `FUN_00739900` | **confirmed** |
| Config file size | 0x8F0 bytes, marker 0x2C | `FUN_006e8c40` | **confirmed** |
| Allocation-tracker slots | 5000 | `FUN_006ea380` | **confirmed** loop. Meaning (memory-tracking table) **[inferred]** |
| Campaign regions | **dynamic**: (region count + 2) records of **0x1400 bytes** | `FUN_0054b6f0` (3dmap.cpp line 0x10D): `DAT_00be6674 = new((DAT_00b95aa4 + 2) * 0x1400)`. Record 0 = "Hidden Rebel Base"; records 1..`DAT_00b95a9c` are land regions; the rest are probably sea zones **[inferred]** | **confirmed** that this array has no fixed cap. Other per-region structures may still have one **[open]** |
| Production parse error exit code | 1337 (0x539) | `FUN_00739900` → `FUN_0076e3f9` | **confirmed** |
| `MAX_INMOTION_TURN`, `MAX_TURNSPEED` | battle tuning labels | strings 0x8CE57C / 0x8CE5A8 | **inferred** (probably read from a data file) |

Not yet found, still open:
- province/region cap
- army size (20 units per army in-game, from game knowledge, not found in code)
- unit-size scaling multipliers
- battle men cap

---

## 5. The Proteus layer in detail ("Total War GFX Engine")

Second pass, 2026-11-01. The `MedievalOriginal` copy of the exe is byte-identical (same SHA-256), so there is no unpatched baseline to compare against. Everything below comes from static analysis.

### 5.1 Startup (`entry` at 0xF3E1B0) **[confirmed]**

1. `FUN_00f3e180`: gets `VirtualProtect`, then `FUN_00f3e0a0` resolves Proteus's **own private import table**:
   - descriptor list at 0xF435E0
   - IAT at 0xF3C000–0xF3C09C
   - PSAPI: `EnumProcessModules`, `GetModuleFileNameExA`
   - 28 KERNEL32 functions
   - USER32: `ClipCursor`, `GetDesktopWindow`, `MessageBoxA`, `SendMessageA`, `LoadIconA`, `DefWindowProcA`

   `VirtualProtect` is referenced **only** inside this loader (xrefs 0xF3E0FC, 0xF3E135, 0xF3E19A), so Proteus does **not** patch game code in memory at runtime.
2. `FUN_00f3f330`: creates its heap and runs its static initialisers.
3. `FUN_00f3df60`: `LoadLibrary` of **ddraw.dll** (DirectDrawCreate, DirectDrawCreateEx, DirectDrawEnumerateExA), **d3d9.dll** (Direct3DCreate9) and **dsound.dll** (DirectSoundCreate8).
4. `FUN_00f3e2d0`: opens `proteus.txt` (truncated when it grows past 16 MB), logs the session header, creates `IDirect3D9`, and probes formats (R5G6B5, R32F, RESZ, depth DS16/DT16/DT24X/DT24S/DT32/DF16/DF24/INTZ/RAWZ/R32F).
   - It **requires an 800×600 display mode and a usable depth format**. Otherwise it shows "Graphics card not supported" (title "Total War GFX Engine") and exits.
5. Calls the game's original CRT entry `FUN_00772542`, then cleans up on return.

### 5.2 Every place the original code enters Proteus **[confirmed: byte scan of the original sections + decompilation]**

There are 23 direct call/jmp sites in the original `.text`, and no data pointers from the original `.rdata`/`.data`.

| Original site | Proteus target | What it does |
|---|---|---|
| 0x73EC2E (import stub) | `FUN_00f3d760` | **DirectDrawCreateEx replacement**: calls the real one, then returns a Proteus wrapper object with its own IDirectDraw7 vtables (0xF3CAA8/0xF3CB08/0xF3CB78), i.e. DirectDraw emulated on D3D9 **[inferred from vtables]** |
| 0x73EB98 (import stub) | `FUN_00f3d830` | Direct3D object request; returns a wrapper around the startup `IDirect3D9` |
| 0x73EC34 (import stub) | `FUN_00f3d800` | DirectDrawEnumerateExA pass-through with a fixed callback |
| 0x73EC3A (import stub) | `FUN_00f3d890` | **DirectSoundCreate8 replacement**: wrapper object (vtable 0xF3CF90) |
| 0x41806E | `FUN_00f3d920` | `CoCreateInstance` wrapper: wraps the object when the CLSID matches a GUID embedded in Proteus |
| 0x73D47E (start of an original function) | `FUN_00f43389` | **16-bit RLE sprite blitter replaced** by a version that clips to the screen size (`DAT_00f44100`/`DAT_00f44104`). A high-resolution fix **[inferred purpose]** |
| 0x411E62 (tail of `FUN_00411e20`) | `FUN_00f4336b` | sets `DAT_00799f2c` (was 0xFFFF) from Proteus's pixel-format query. Probably a colour key **[inferred]** |
| 0x41E993/0x41E99A/0x41F4C6/0x4C0389/0x573FEB/0x57423B/0x59707E/0x5972A6/0x6DCBB9/0x6DCBC3/0x6DCCA6/0x6DCCBD | `ShowCursor`/`SetCursor`/`SetCursorPos` stubs (0xF3D9D0–0xF3D9F0) | plain jumps to the real USER32 functions (no behaviour change seen) |
| 0x41DFCA, 0x779DAF, 0x779DC1 | `FUN_00f3dbe0` | swaps a stored callback (returns the old one). Probably keeps Proteus's crash handler installed **[inferred]** |
| 0x76E4B8 (CRT exit) | `FUN_00f3e230` | Proteus shutdown, then `ExitProcess` |

### 5.3 What this means for patching

- **Do not touch** these 23 sites, the import stubs at 0x73EB8C–0x73EC40, or anything in 0xF3C000–0xF45000.
- Game logic (campaign, battle, AI, production parsing) does **not** go through Proteus. Patches there don't conflict with it.
- **Free space for our own code [confirmed, zero/padding tail]:**
  - 0xA72 bytes at the end of the original `.text` (VA 0x78358E–0x784000, executable)
  - 0x690 bytes at the end of the Proteus `.text`
  - 0x943 bytes at the end of the original `.data`

  Larger additions need a new PE section.
- Graphics problems (resolution, cursor, sprites) live in Proteus and DirectDraw emulation code. Fixing those is a different job from gameplay patches.

---

## 6. Open questions and next steps

1. **Create functions for the 40 undecompiled unit-column handlers** (addresses in 3.3) so Ghidra can decompile them, then finish the field-to-offset map of the 0x480-byte unit struct. This needs a Ghidra function-creation step or a disassembler pass.
2. **Unit-size scaling:** follow the readers of `+0x344` (base_unitsize) and the `unitsize_scalable` handler (0x733C00). Find the Small/Normal/Large/Huge multipliers stored in `Medieval.Cfg`.
3. **Faction cap:** confirm that the 96-dword array at 0xF31FB8 is indexed by faction id × period, and look for other arrays sized 32 / 0x20 (rendering colours, banners, AI personalities).
4. **Province cap:** inspect `cregion.cpp` / `startpos.cpp` code. `startpos.cpp` checks a keyword with id 0x49 and compares a value against **0x32 (50)** in `FUN_00642140`; check whether that is a region or neighbour limit.
5. **Army stack limit (20 units)** and battle reinforcement logic in `carmy.cpp` / `PreBattleScreen.cpp`.
   - Ruled out: `maxunits` (0x7B2678) is a key in the formation data files (read in `FUN_0044f4d0` with `minunits`/`arrangeinrow`).
   - Ruled out: `generals_army_size` (0x7C3DF8) is only a UI text key (`FUN_005908a0`).
   - Next: find the "add unit to army" code (callers around the army object, `vtable+0x38` = location region).
6. ~~Proteus layer~~: done, see section 5.
6b. **Region-dependent fixed arrays:** the region array itself is dynamic (section 4). Check bitsets and arrays indexed by region id (AI, trade, `startpos.cpp` value 0x32) for a hidden cap.
7. **Config transform** `FUN_00409010` / flag `DAT_0079c8e8`: is it encryption, or a no-op?
8. **Unit-column error message:** the inverted text (3.3 step 1) is a known-quirk candidate for modding documentation.
9. **Evidence:** keep `analysis/decompiled/*.c` with this report. The Ghidra session was ephemeral, so there is no saved project; re-running `open_binary` repeats the full auto-analysis.

---

## 7. Display mode and the Video options menu

Third pass, 2026-11-01, for the borderless fullscreen patch (`patches/borderless/`).

### 7.1 Window and device **[confirmed]**

- `FUN_0041f050` registers the window class (window procedure **inside Proteus**, 0xF3DA00) and creates the window with `CreateWindowExA(0, "Medieval: Total War", ..., WS_POPUP, 0, 0, DAT_008d01b4, DAT_008d01b8, ...)`. The window is already borderless; the game's only `MoveWindow` call is at 0x6D6317.
- `D3D9.dll!Direct3DCreate9` is a **static import** of the exe (on top of Proteus's own `LoadLibrary("d3d9.dll")`), so a `d3d9.dll` in the game folder is loaded before the exe's CRT starts.
- Proteus creates the device with `Windowed = FALSE`, `D3DSWAPEFFECT_DISCARD`, R5G6B5 back buffer at the game resolution: 800×600 at startup, 640×480 for the menus, the chosen resolution (e.g. 2560×1440) on the campaign map and in battle. `Present` is always called with NULL rectangles. Proteus never reads the present parameters back (`GetPresentParameters` is not called).
- Mouse: window messages, plus one `GetCursorPos` call at 0x4C02DC; `SetCursorPos` and `ClipCursor` go through Proteus. `GetDeviceCaps(HORZRES/VERTRES)` is read once at startup (`FUN_0041df80`).
- The black bars seen in battle with the interface set to **Widescreen** are by design (game option), not a display problem.

### 7.2 Menu widget tables **[confirmed]**

- Menus are static arrays of **0x134-byte widget records** in `.data`. Known fields: +0x00 name (char*, used by the lookup `FUN_006e1090(name)`), +0x04 enabled, +0x08 type (0x3 button/switch, 0x5 slider, 0x9 text, 0xB label), +0x0C x, +0x10 y, +0x14 visible, +0x20 style pointer, +0x24 callback, +0x2C localised-text record (0xA0-byte records whose +0x00 is the `Loc` key), +0x3C/+0x40 width/height (switch: 16×16).
- A static constructor per menu calls `0x6EB8E0(name, table, count)`; there are 48 such menus, up to 127 widgets each.
- Video menu (`MainMenuGraphicOptions`): table 0x8025E0, **17 records**: title (y 42), Smoke label/switch (120/140, callback 0x70A490), Wildlife label/switch (170/190, 0x70A510), default unit size label/text/slider (260/278, 0x70A590), AGP memory (350/368, 0x70A890), pyrotechnics (440/458, 0x70AA60), two type-0 widgets at y 248, bottom button (239,539, 0x70A2B0). The Dust, Ambient Flora and HQ movies texts exist in `FRONTEND.TXT` but have no widget.
- Users of the table: the constructor call at 0x6A8790 (`push 0x11 / push 0x8025E0`), and the Options dispatcher `FUN_00709200`, which passes `[0x8C1E70]` (table) and `[0xEFF7E0]` (count) to `FUN_006e1350` with handler 0x71ABA0. `[0xEFF7E0]` is copied at startup (0x71CFB9) from `.data` 0x791050, the only reader of that value.
- Switch callback (Smoke, 0x70A490), `cdecl int cb(widget *w)`: `w[0x2A]` = setting byte (0xE78120); `0x6E6D20(w)`; `w[0x29] = 0`; if `w->0x6DF860(0)` (thiscall, "clicked") and `[0x8DCCA8] & 0x800` and not `[0x8DCCB4] & 0x800`: `0x6E9D50(1, 0, 0, "LMB on Graphics Options Menu Smoke toggle")`, then toggle `w[0x2A]`; finally store `w[0x2A]` back to the setting.

### 7.3 Display resolution **[confirmed]** (2026-11-02, for the launcher)

- Current resolution globals: **0x7A2780/84** campaign map width/height, **0x7A2788/8C** battle (defaults 800×600). Startup picks one of them for the window size by game state `[0x8B8C34]` (0x41E620; 1 = battle, else campaign or 800×600).
- Command line: a letter followed by a hex digit 0–B selects from a static table at **0x7847B0** (12 pairs, 320×240 … 2048×1536), parsed at 0x420F10 ("Resolution Values 0-7" text is outdated). One letter case sets campaign, the other battle **[inferred which]**.
- `Medieval.Cfg` stores **indices**, not sizes: offset **0xB4** (0xE77B04) battle, **0xB8** (0xE77B08) campaign. They index per-device mode lists built at startup.
- Device records: `0xE7A1E0 + 0x60 * [0x895F8C]`. +0x10 valid flag, +0x20/+0x44 campaign count/index array, +0x24/+0x54 battle count/index array, +0x34 mode records (32 bytes: +4 width, +8 height).
- 0x6DE4BE–0x6DE565: clamps the cfg indices to the counts (copies in 0xE7D1F0 battle / 0xE7D1F4 campaign) and writes width/height into 0x7A2780/0x7A2788. `FUN_006de850(dev, w, h, battle)` snaps a size to the closest listed mode. The Video menu sliders write the same globals (0x70CC52, 0x70CE21).
- The borderless proxy hooks 0x6DE4BE (`mov edx,[0xE77B04]`, 6 bytes → call + nop) to write the indices matching `CampaignWidth/Height` and `BattleWidth/Height` from the ini, once at startup. Play-tested.

## 8. Boot intro **[confirmed]** (2026-11-02)

- `FUN_006be3e0` (`credits.cpp`, called once at 0x6D5931) plays the boot sequence: `Credits\Legals\TotalWar.Mpg`, then `Credits\Legals\intro.mpg` (another TotalWar.Mpg branch when `[esp+0x14] == 5`), then the `Legals.Bp8` screen. Movies: `new` object, constructor 0x417B70(path), frame loop on 0x417D10 until it returns false or the skip flag `[0x8DCD11]` is set.
- Built-in skip: the function first calls `FUN_00408fc0("skipcredits.txt", 0)` (`_access` != -1); if the file exists in the game folder, the whole sequence is skipped. The repo ships an empty `skipcredits.txt` for that. Play-tested.

