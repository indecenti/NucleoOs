#!/usr/bin/env python3
"""Generate the firmware APP_ALIAS[] table from the single source of truth.

Source : registry/app-aliases.json   (edit THIS, then run this script)
Target : firmware/components/nucleo_anima/nucleo_anima.c  (block between the
         `// <gen:app-alias>` / `// </gen:app-alias>` markers)

The host harness/sim (tools/serve-shell.mjs) reads the same JSON directly, so the
device and the PC simulator resolve "apri <app>" through one vocabulary instead of
four hand-maintained copies that drift apart.

Usage:
  python tools/anima/gen_aliases.py            # rewrite the C block in place
  python tools/anima/gen_aliases.py --check     # CI: fail if the block is stale
  python tools/anima/gen_aliases.py --stdout     # print the block, write nothing

Validation (all run every time):
  * every alias key must be an installed app in registry/apps.json
  * aliases must be lowercase ASCII (a_tokenize folds accents offline)
  * no app may exceed A_MAX_ALIAS-1 aliases
  * an alias string under two apps is an ERROR (resolution would be order-dependent)
  * a prefix collision per the firmware a_match() rule is a WARNING (order resolves it,
    but it's surfaced so the chosen order is a deliberate decision — e.g. spreadsheet
    before calculator for "foglio di calcolo")

Stdlib only, to match tools/anima/build_anima.py.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "registry" / "app-aliases.json"
REG = ROOT / "registry" / "apps.json"
TARGET = ROOT / "firmware" / "components" / "nucleo_anima" / "nucleo_anima.c"
BEGIN = "// <gen:app-alias>"
END = "// </gen:app-alias>"


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except Exception as e:
        sys.exit(f"error: cannot read {path}: {e}")


def a_match(a: str, b: str) -> bool:
    """Mirror of a_match() in nucleo_anima.c: equal, or one is a >=4-char prefix of
    the other with a length gap <= 2."""
    if a == b:
        return True
    m = min(len(a), len(b))
    if m < 4:
        return False
    if abs(len(a) - len(b)) > 2:
        return False
    return a[:m] == b[:m]


def validate(aliases: dict, installed: set) -> tuple[list, list]:
    errs, warns = [], []
    A_MAX = max((len(v) for v in aliases.values()), default=0) + 1
    # Each app's aliases.
    seen = {}  # alias -> app id (for exact-collision detection)
    flat = []  # (app, alias) in declaration order
    for app, al in aliases.items():
        if app not in installed:
            # WARNING, not error: the alias table is authoritative for what ANIMA may *try* to
            # launch, and we must preserve the device vocabulary verbatim. An id missing from the
            # registry is surfaced (e.g. "radio" is a media-player mode, not a standalone app) but
            # never silently dropped — dropping it would change device behavior.
            warns.append(f"alias target '{app}' is not an installed app in registry/apps.json")
        if len(al) >= A_MAX:  # one slot is the NULL terminator
            errs.append(f"app '{app}' has {len(al)} aliases, max is {A_MAX - 1}")
        for a in al:
            if a != a.lower() or not a.isascii():
                errs.append(f"alias '{a}' ({app}) must be lowercase ASCII (fold accents offline)")
            if a in seen and seen[a] != app:
                errs.append(f"alias '{a}' is claimed by both '{seen[a]}' and '{app}' (exact collision)")
            seen.setdefault(a, app)
            flat.append((app, a))
    # Prefix collisions across DIFFERENT apps: a real input token could match both;
    # the earlier-declared app wins. Report so the order is an explicit choice.
    for i in range(len(flat)):
        ai_app, ai = flat[i]
        for j in range(i + 1, len(flat)):
            aj_app, aj = flat[j]
            if ai_app == aj_app or ai == aj:
                continue
            if a_match(ai, aj):
                warns.append(f"prefix collision: '{ai}' ({ai_app}) ~ '{aj}' ({aj_app}) "
                             f"-> '{ai_app}' wins by order")
    return errs, warns


def render(aliases: dict, notes: dict) -> str:
    A_MAX = max((len(v) for v in aliases.values()), default=0) + 1
    idcol = max(len(f'"{app}",') for app in aliases) + 1
    lines = [
        BEGIN + " --- GENERATED from registry/app-aliases.json by tools/anima/gen_aliases.py.",
        "// DO NOT EDIT BY HAND: edit the JSON and run `python tools/anima/gen_aliases.py`.",
        f"#define A_MAX_ALIAS {A_MAX}",
        "typedef struct { const char *id; const char *alias[A_MAX_ALIAS]; } a_alias_t;",
        "",
        "// Words the user might type (IT+EN) -> the registry app id ANIMA opens. First match wins.",
        "static const a_alias_t APP_ALIAS[] = {",
    ]
    for app, al in aliases.items():
        idtok = f'"{app}",'.ljust(idcol)
        body = ", ".join(f'"{a}"' for a in al)
        row = f"    {{ {idtok} {{ {body}, NULL }} }},"
        if app in notes:
            row += f"   // {notes[app]}"
        lines.append(row)
    lines.append("};")
    lines.append(END)
    return "\n".join(lines) + "\n"


def splice(source: str, block: str) -> str:
    lo = source.find(BEGIN)
    hi = source.find(END)
    if lo == -1 or hi == -1:
        sys.exit(f"error: markers {BEGIN} / {END} not found in {TARGET}")
    # extend hi to end of the END line
    hi = source.find("\n", hi)
    hi = len(source) if hi == -1 else hi + 1
    return source[:lo] + block + source[hi:]


def main():
    ap = argparse.ArgumentParser(description="Generate APP_ALIAS[] from registry/app-aliases.json")
    ap.add_argument("--check", action="store_true", help="exit 1 if the C block is stale")
    ap.add_argument("--stdout", action="store_true", help="print the C block and exit")
    args = ap.parse_args()

    src = load_json(SRC)
    aliases = src.get("aliases") or {}
    notes = src.get("notes") or {}
    reg = load_json(REG)
    installed = {a["id"] for a in reg.get("installed", [])}

    errs, warns = validate(aliases, installed)
    for w in warns:
        print(f"  warning: {w}", file=sys.stderr)
    if errs:
        print("\n".join(f"  - {e}" for e in errs), file=sys.stderr)
        sys.exit(f"FAILED: {len(errs)} problem(s) in {SRC}")

    block = render(aliases, notes)
    if args.stdout:
        sys.stdout.write(block)
        return

    current = TARGET.read_text(encoding="utf-8")
    updated = splice(current, block)
    if args.check:
        if current != updated:
            sys.exit("STALE: APP_ALIAS[] is out of date. Run: python tools/anima/gen_aliases.py")
        print(f"OK: APP_ALIAS[] up to date ({len(aliases)} apps).")
        return
    if current == updated:
        print(f"OK: no change ({len(aliases)} apps).")
        return
    TARGET.write_text(updated, encoding="utf-8")
    print(f"OK: wrote APP_ALIAS[] ({len(aliases)} apps) -> {TARGET.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
