#!/usr/bin/env python3
"""Check Medieval: Total War production files (unit / building tables)
before launching the game.

The game aborts on the first problem it finds (message box, then exit code
1337), and some layout mistakes silently shift every later row. This script
reproduces the game's own tokenizer and parser layout, so it catches the same
problems. Addresses refer to Medieval_TW.exe, SHA-256 23724b03...ace5;
see analysis/MedievalReconquered_overview.md, section 3.

Usage (from the game folder):
    python tools/check_prodfiles.py                  # every prod file used by the campaigns
    python tools/check_prodfiles.py MY_UNITS.TXT     # specific file(s), kind from file name
    python tools/check_prodfiles.py --kind unit X.TXT --buildings Y.TXT

Exit code: 0 = no errors (warnings allowed), 1 = errors found, 2 = bad usage.
Standard library only; works with Python 3.8+.
"""

import argparse
import bisect
import glob
import os
import re
import sys

# --------------------------------------------------------------------------
# Constants recovered from the exe
# --------------------------------------------------------------------------

UNIT_COLUMNS = 52       # handler table at 0x8CBA18
BUILDING_COLUMNS = 28   # handler table at 0x8CBBC0

# String table at 0x8CB564, index stored at unit +0x330
UNIT_TYPES = [
    "ARTILLERY", "LIGHT_ARTILLERY", "HEAVY_ARTILLERY", "CAVALRY",
    "LIGHT_CAVALRY", "HEAVY_CAVALRY", "INFANTRY", "LIGHT_INFANTRY",
    "HEAVY_INFANTRY", "MOUNT", "SHIP", "CRUSADE", "JIHAD", "SPY", "ASSASSIN",
    "EMISSARY", "BISHOP", "CARDINAL", "INQUISITOR", "GRAND_INQUISITOR",
    "PRIEST", "ORTHODOX_BISHOP", "MULLAH", "ULAMA", "PRINCESS",
]
GENERAL_CANDIDATE = ["PREFERRED", "OK", "DISCOURAGED"]   # FUN_007320a0
PERIODS = ["ALL_PERIODS", "EARLY", "HIGH", "LATE"]       # FUN_00733060

UNIT_COL_NAMES = [
    "identifying_label", "type", "cost", "support_cost", "production_time",
    "honour_handicap", "base_strength", "base_unitsize", "honour_step",
    "general_candidate", "troop_mix", "faction_advantage", "region_advantage",
    "dojo_advantage", "preferences", "speciality_building_influences",
    "buildings_needed", "class", "moral_bonus", "periods", "unitsize_scalable",
    "troop_descriptions", "invisibility", "specialities", "tooltips",
    "mount_type", "body_texpage", "faction_leader", "faction_association",
    "region_association", "formation_positional_priorities",
    "formation_task_suitabilities", "culture_association", "ship_stats",
    "supporting_ranks", "discipline", "formation_regularity", "shield_type",
    "melee_shield", "shield_effectiveness", "armour_piercing",
    "cav_attack_bonus", "cav_defend_bonus", "fear_factors", "zeal_modifiers",
    "faith_modifiers", "dismount_type", "can_dismount_in_open_battle",
    "melee_weapon_type", "mercenary_usable", "noncampaign_periods",
    "noncampaign_faction_association",
]
BUILDING_COL_NAMES = [
    "num_upgrades", "identifying_label", "localised_text", "type", "cost",
    "time", "conditions", "infopic_filename", "piece_filename", "preferences",
    "influences", "frontline_specific", "faction_advantage",
    "speciality_influences", "specific_culture", "loyalty_bonus",
    "upgrade_abilities", "tech_level", "required_resources",
    "faction_association", "glorycondition_association", "faith_propogation",
    "income", "happiness_adjuster", "mercenary_magnetism",
    "shady_meeting_place", "invention_requirements", "unit_capacity",
]

# Unit columns parsed with the integer-list template (FUN_0073a0b0):
# empty cell -> error 8 "Too few values" -> game exits.
UNIT_REQUIRED_INT_COLS = [2, 3, 4, 5, 6, 7, 8]
# Integer columns where an empty cell is accepted (stored as 0).
UNIT_OPTIONAL_INT_COLS = [18, 34]

DEFAULT_UNIT_FILE = "crusaders_unit_prod11.txt"
DEFAULT_BUILDING_FILE = "crusader_build_prod13.txt"

# Token types returned by the game's tokenizer FUN_00739690
T_CELL, T_EMPTY, T_DATASTART, T_EOF, T_LINESTART = 0, 1, 2, 4, 5

TAB, CR, LF, QUOTE, SPACE = 9, 13, 10, 0x22, 0x20


# --------------------------------------------------------------------------
# Emulation of the game's tokenizer (FUN_00739690)
# --------------------------------------------------------------------------

class Token:
    __slots__ = ("type", "pos", "after_cr")

    def __init__(self, type_, pos, after_cr):
        self.type = type_          # T_* value the game would see
        self.pos = pos             # byte offset where the cell starts
        self.after_cr = after_cr   # cell begins a new physical line


def _tokenize_pass(b):
    """One full pass over the buffer, mutating it the way the game does."""
    n = len(b)
    pos = 0
    data_flag = False
    toks = []
    while True:
        if pos >= n:
            toks.append(Token(T_EOF, pos, False))
            return toks
        quote = False
        after_cr = False
        found = False
        # Walk the current cell until its terminating delimiter.
        while True:
            c = b[pos]
            if c == QUOTE:
                quote = not quote
            if c == TAB:
                found = True
                break
            if c == CR:
                if not quote:
                    after_cr = True
                    pos += 1          # the game skips CR and then one more byte
                    found = True
                    break
                b[pos] = 0            # CR inside quotes: blanked
                pos += 1
                if pos < n and b[pos] == LF:
                    b[pos] = 0
                    pos += 1
            else:
                if c == 0:
                    b[pos] = SPACE
                pos += 1
            if pos >= n:
                break
        if not found:
            toks.append(Token(T_EOF, pos, False))
            return toks
        pos += 1                      # step over TAB (or the byte after CR)
        if pos >= n:
            toks.append(Token(T_EOF, pos, False))
            return toks
        # Cell text: up to the next TAB or CR, trimmed of spaces/quotes.
        q = pos
        while q < n and b[q] not in (TAB, CR):
            q += 1
        length = q - pos
        p = q
        while p > 0:
            p -= 1
            if b[p] != QUOTE and b[p] != SPACE:
                break
            length -= 1
        if length <= 0:
            toks.append(Token(T_EMPTY, pos, after_cr))
            continue
        for i in range(pos, pos + length):
            if b[i] in (LF, CR):
                b[i] = SPACE
        lead = 0
        while pos + lead < n and b[pos + lead] in (SPACE, QUOTE):
            lead += 1
        if not data_flag and bytes(b[pos + lead:pos + lead + 10]) == b"DATA START":
            data_flag = True
            toks.append(Token(T_DATASTART, pos, after_cr))
            continue
        toks.append(Token(T_LINESTART if after_cr else T_CELL, pos, after_cr))


def tokenize(raw):
    """The game re-scans the same (mutated) buffer several times; the second
    pass is what the column handlers actually see."""
    b = bytearray(raw)
    _tokenize_pass(b)
    return b, _tokenize_pass(b)


def raw_cell(b, pos):
    """Cell bytes from its start to the next TAB/CR (what handlers read)."""
    q = pos
    while q < len(b) and b[q] not in (TAB, CR):
        q += 1
    return bytes(b[pos:q]).decode("latin-1")


def cell_values(text):
    """Split a cell like the game's value reader FUN_00739f60:
    values are separated by spaces, quotes and commas; LF ends the cell."""
    text = text.split("\n", 1)[0]
    return [v for v in re.split(r'[ ",\x00]+', text) if v]


def game_atoi(s):
    """C atoi: optional sign then digits; anything else -> 0."""
    m = re.match(r"\s*([+-]?\d+)", s)
    return int(m.group(1)) if m else None


# --------------------------------------------------------------------------
# Checking
# --------------------------------------------------------------------------

class Report:
    def __init__(self, path):
        self.path = path
        self.errors = []
        self.warnings = []

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)


def parse_table(path, kind, rep):
    """Reproduce the layout steps of FUN_00723cd0 / FUN_00723500.
    Returns (buffer, rows) where rows is a list of (line_no, [cell texts]),
    or None if the game would not get that far."""
    expected = UNIT_COLUMNS if kind == "unit" else BUILDING_COLUMNS
    raw = open(path, "rb").read()

    if not raw:
        rep.error("file is empty")
        return None
    if b"\r\n" not in raw:
        rep.error("no Windows (CRLF) line endings: the game only ends a row at CR, "
                  "so the whole file reads as a single row. Re-save with CRLF.")
        return None
    if not raw.endswith(b"\r\n"):
        rep.warn("file does not end with CRLF: the game reads past the end of "
                 "the buffer when scanning the last cell")

    line_starts = [0] + [m.end() for m in re.finditer(rb"\r\n|\r(?!\n)|(?<!\r)\n", raw)]

    def line_of(pos):
        return bisect.bisect_right(line_starts, pos)

    b, toks = tokenize(raw)

    # FUN_00739890: find DATA START, then skip empty cells.
    ds = next((i for i, t in enumerate(toks) if t.type == T_DATASTART), None)
    if ds is None:
        rep.error("no 'DATA START' cell: the game shows 'Counted 0 columns…' "
                  "and then crashes (division by zero)")
        return None
    first = ds + 1
    while first < len(toks) and toks[first].type == T_EMPTY:
        first += 1
    if first >= len(toks) or toks[first].type == T_EOF:
        rep.error("no data after 'DATA START'")
        return None

    # Column count = cells in the first data row (up to the next line start).
    cols = 1
    i = first + 1
    while i < len(toks) and toks[i].type != T_LINESTART:
        if toks[i].type == T_EOF:
            rep.error("only one data row and no row after it: the game's column "
                      "count loop never finds a line start and hangs")
            return None
        cols += 1
        i += 1
    if toks[i - 1].type == T_EMPTY and toks[i - 1].after_cr:
        pass  # handled by the physical-row check below
    if cols != expected:
        what = "few" if cols < expected else "many"
        rep.error(f"first data row (line {line_of(toks[first].pos)}) has {cols} cells, "
                  f"the game requires exactly {expected} (too {what}). "
                  f"Game exits with a 'Too ... data columns' box"
                  + (" (for unit files the box wrongly says 'Too many')"
                     if kind == "unit" and cols < expected else ""))
        return None

    # Physical rows (as the game splits them) from the first data cell on.
    data = toks[first:-1]                      # drop final EOF
    phys = []                                  # list of token lists
    for t in data:
        if t.after_cr or not phys:
            phys.append([t])
        else:
            phys[-1].append(t)

    def first_cell_nonempty(t):
        txt = raw_cell(b, t.pos)
        return txt.strip(' ",\x00') != "" and not txt.lstrip(' ",\x00').startswith("\n")

    # Last physical row that holds real data.
    last_data = -1
    for k, row in enumerate(phys):
        if any(tt.type != T_EMPTY for tt in row):
            last_data = k

    ok_layout = True
    for k, row in enumerate(phys[:last_data + 1]):
        ln = line_of(row[0].pos)
        if len(row) == 1 and row[0].type == T_EMPTY:
            rep.error(f"line {ln}: blank line inside the data. The game counts it as "
                      f"one cell, so every following row is shifted by one column")
            ok_layout = False
        elif len(row) != cols:
            hint = ""
            nq = sum(raw_cell(b, tt.pos).count('"') for tt in row)
            if nq % 2:
                hint = " (odd number of quotes: an unclosed quote hides the line end)"
            rep.error(f"line {ln}: row has {len(row)} cells, expected {cols}"
                      f"{hint}. All following rows are misaligned")
            ok_layout = False
        elif not first_cell_nonempty(row[0]):
            rep.error(f"line {ln}: data row with an empty first cell. The game "
                      f"stops counting rows correctly (later units are dropped)")
            ok_layout = False
        if not ok_layout:
            break
    if not ok_layout:
        return None

    # Game's own row arithmetic, as a final consistency check.
    total = len(data)
    n_rows = total // cols
    valid = 0
    for r in range(n_rows):
        if first_cell_nonempty(data[r * cols]):
            valid += 1
    rows = []
    for r in range(valid):
        cells = data[r * cols:(r + 1) * cols]
        rows.append((line_of(cells[0].pos), [raw_cell(b, t.pos) for t in cells]))
    if valid != last_data + 1:
        rep.error(f"the game would read {valid} rows but the file has {last_data + 1} "
                  f"data rows; check for stray TABs or blank lines")
        return None
    return rows


def check_enum(rep, line, col, colname, text, allowed, multi):
    values = cell_values(text) if multi else cell_values(text)[:1]
    for v in values:
        if v in allowed:
            continue
        if v.upper() in allowed:
            rep.warn(f"line {line}, col {col + 1} ({colname}): '{v}' differs only in "
                     f"case from a known value; the game may reject it")
        else:
            rep.error(f"line {line}, col {col + 1} ({colname}): unknown value '{v}' "
                      f"(allowed: {', '.join(allowed)}). Game exits")


def check_units(path, buildings, rep):
    rows = parse_table(path, "unit", rep)
    if rows is None:
        return
    seen = {}
    for line, cells in rows:
        label = cell_values(cells[0])[0] if cell_values(cells[0]) else ""
        if label in seen:
            rep.warn(f"line {line}: unit label '{label}' already used on line {seen[label]} "
                     f"(the file header says labels must be unique)")
        seen.setdefault(label, line)

        check_enum(rep, line, 1, "type", cells[1], UNIT_TYPES, multi=False)
        check_enum(rep, line, 9, "general_candidate", cells[9], GENERAL_CANDIDATE, multi=False)
        if cell_values(cells[19]):
            check_enum(rep, line, 19, "periods", cells[19], PERIODS, multi=True)

        for c in UNIT_REQUIRED_INT_COLS + UNIT_OPTIONAL_INT_COLS:
            vals = cell_values(cells[c])
            name = UNIT_COL_NAMES[c]
            if not vals:
                if c in UNIT_REQUIRED_INT_COLS:
                    rep.error(f"line {line}, col {c + 1} ({name}): empty, the game "
                              f"requires a number (error 'Too few values'). Game exits")
                continue
            if game_atoi(vals[0]) is None:
                rep.warn(f"line {line}, col {c + 1} ({name}): '{vals[0]}' is not a "
                         f"number; the game reads it as 0")
            if len(vals) > 1 and c in UNIT_REQUIRED_INT_COLS:
                rep.warn(f"line {line}, col {c + 1} ({name}): {len(vals)} values; "
                         f"the game only uses the first ({vals[0]})")

        if buildings is not None:
            for ref in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", cells[16]):
                m = re.match(r"^(.*?)(\d+)$", ref)
                base, level = (m.group(1), int(m.group(2))) if m else (ref, None)
                if ref in buildings:
                    continue
                if base in buildings:
                    max_level = buildings[base] + 1
                    if level is not None and not 1 <= level <= max_level:
                        rep.warn(f"line {line}, col 17 (buildings_needed): '{ref}' asks "
                                 f"for level {level} but {base} has {max_level} levels")
                    continue
                rep.warn(f"line {line}, col 17 (buildings_needed): '{ref}' is not a "
                         f"building in the building file")
    rep.summary = f"{len(rows)} units"


def check_buildings(path, rep):
    """Returns {label: num_upgrades} for cross-checking, or None."""
    rows = parse_table(path, "building", rep)
    if rows is None:
        return None
    result = {}
    for line, cells in rows:
        n = game_atoi(cells[0])
        if n is None:
            rep.warn(f"line {line}, col 1 (num_upgrades): '{cells[0].strip()}' is not "
                     f"a number; the game reads it as 0")
            n = 0
        elif n < 0:
            rep.error(f"line {line}, col 1 (num_upgrades): negative ({n}); the game "
                      f"would create no building objects for this row")
        vals = cell_values(cells[1])
        label = vals[0] if vals else ""
        if not label:
            rep.error(f"line {line}, col 2 (identifying_label): empty")
        elif label in result:
            rep.warn(f"line {line}: building label '{label}' used twice")
        result[label] = n
    rep.summary = f"{len(rows)} buildings"
    return result


# --------------------------------------------------------------------------
# Finding the files the campaigns use
# --------------------------------------------------------------------------

PRODFILES_RE = re.compile(
    r'^\s*Prodfiles::\s*UNITPROD\s+"([^"]+)"\s+BUILDINGPROD\s+"([^"]+)"', re.I | re.M)


def find_file(root, name):
    """Case-insensitive lookup in the game root (Windows is case-insensitive)."""
    p = os.path.join(root, name)
    if os.path.exists(p):
        return p
    for f in os.listdir(root):
        if f.lower() == name.lower():
            return os.path.join(root, f)
    return None


def campaign_pairs(root):
    pairs = [(DEFAULT_UNIT_FILE, DEFAULT_BUILDING_FILE, "default (hard-coded)")]
    for sp in sorted(glob.glob(os.path.join(root, "campmap", "startpos", "*"))):
        try:
            text = open(sp, encoding="latin-1").read()
        except (IsADirectoryError, PermissionError):
            continue
        for m in PRODFILES_RE.finditer(text):
            pairs.append((m.group(1), m.group(2), os.path.relpath(sp, root)))
    return pairs


def guess_kind(path):
    n = os.path.basename(path).lower()
    if "unit" in n:
        return "unit"
    if "build" in n:
        return "building"
    return None


def print_report(rep, verbose):
    status = "FAIL" if rep.errors else ("WARN" if rep.warnings else "OK")
    extra = getattr(rep, "summary", "")
    print(f"[{status}] {rep.path}" + (f"  ({extra})" if extra else ""))
    for e in rep.errors:
        print(f"    ERROR   {e}")
    shown = rep.warnings if verbose else rep.warnings[:15]
    for w in shown:
        print(f"    warning {w}")
    if len(rep.warnings) > len(shown):
        print(f"    ... {len(rep.warnings) - len(shown)} more warnings (use -v)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("files", nargs="*", help="production files to check")
    ap.add_argument("--kind", choices=["unit", "building"],
                    help="file kind (default: guessed from the file name)")
    ap.add_argument("--buildings", help="building file to cross-check unit "
                    "'buildings_needed' against (unit files only)")
    ap.add_argument("--root", default=".", help="game folder (default: current)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all warnings")
    args = ap.parse_args(argv)

    reports = []
    if args.files:
        bmap = None
        if args.buildings:
            brep = Report(args.buildings)
            bmap = check_buildings(args.buildings, brep)
            reports.append(brep)
        for f in args.files:
            kind = args.kind or guess_kind(f)
            if kind is None:
                print(f"cannot tell whether {f} is a unit or building file; use --kind",
                      file=sys.stderr)
                return 2
            rep = Report(f)
            if kind == "unit":
                check_units(f, bmap, rep)
            else:
                check_buildings(f, rep)
            reports.append(rep)
    else:
        root = args.root
        done = {}
        for unit_name, build_name, source in campaign_pairs(root):
            up, bp = find_file(root, unit_name), find_file(root, build_name)
            print(f"-- {source}: UNITPROD {unit_name}, BUILDINGPROD {build_name}")
            if bp is None or up is None:
                rep = Report(f"{source}")
                rep.error(f"missing file: {build_name if bp is None else unit_name} "
                          f"(must be in the game root folder)")
                reports.append(rep)
                print_report(rep, args.verbose)
                continue
            key = (os.path.normcase(up), os.path.normcase(bp))
            if key in done:
                print("   (already checked above)")
                continue
            brep = Report(bp)
            bmap = check_buildings(bp, brep)
            urep = Report(up)
            check_units(up, bmap, urep)
            done[key] = True
            for r in (brep, urep):
                reports.append(r)
                print_report(r, args.verbose)
        n_err = sum(len(r.errors) for r in reports)
        print(f"\n{n_err} error(s), {sum(len(r.warnings) for r in reports)} warning(s)")
        return 1 if n_err else 0

    for r in reports:
        print_report(r, args.verbose)
    n_err = sum(len(r.errors) for r in reports)
    print(f"\n{n_err} error(s), {sum(len(r.warnings) for r in reports)} warning(s)")
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())
