#!/usr/bin/env python3
"""ANIMA offline factory (PC-side). See tools/anima/README.md and docs/anima.md.

Phase 0 implements the `commands` step: validate an intent table and emit the SD
command pack. Later steps (encoder distillation, FAISS index) are stubbed with the
intended interface so the contract is documented before implementation.

Stdlib only — no dependencies, to match tools/validate.mjs.
"""
import argparse
import json
import sys
from pathlib import Path

VALID_ACTIONS = {"launch", "system", "answer"}


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))  # BOM-safe
    except Exception as e:
        sys.exit(f"error: cannot read {path}: {e}")


def validate_commands(pack: dict) -> list:
    errs = []
    if pack.get("schema") != 1:
        errs.append("schema must be 1")
    if not pack.get("lang"):
        errs.append("missing 'lang'")
    seen = set()
    for i, it in enumerate(pack.get("intents", [])):
        where = f"intents[{i}] ({it.get('id', '?')})"
        if not it.get("id"):
            errs.append(f"{where}: missing id")
        elif it["id"] in seen:
            errs.append(f"{where}: duplicate id")
        else:
            seen.add(it["id"])
        if it.get("action") not in VALID_ACTIONS:
            errs.append(f"{where}: action must be one of {sorted(VALID_ACTIONS)}")
        if not it.get("keywords"):
            errs.append(f"{where}: needs at least one keyword")
        if len(it.get("keywords", [])) > 8:
            errs.append(f"{where}: max 8 keywords (firmware A_MAX_KW)")
        if it.get("action") == "answer" and not it.get("reply"):
            errs.append(f"{where}: answer intent needs a reply")
        for kw in it.get("keywords", []):
            if kw != kw.lower() or not kw.isascii():
                errs.append(f"{where}: keyword '{kw}' must be lowercase ASCII (normalize accents offline)")
    return errs


def cmd_commands(args):
    src = Path(args.intents)
    pack = load_json(src)
    errs = validate_commands(pack)
    if errs:
        print("\n".join(f"  - {e}" for e in errs), file=sys.stderr)
        sys.exit(f"FAILED: {len(errs)} problem(s) in {src}")

    out = Path(args.out or f"commands.{pack['lang']}.json")
    out.write_text(json.dumps(pack, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    n = len(pack.get("intents", []))
    a = len(pack.get("app_aliases", {}))
    print(f"OK: {n} intents, {a} app aliases -> {out}")
    print(f"   deploy to /sd/data/anima/{out.name} (tools/deploy.ps1)")


def cmd_encoder(args):
    sys.exit("not implemented (Phase 2): distill an int8 student from multilingual-e5-large / "
             "bge-m3 on Italian -> encoder.it.bin. Gated on the Phase 0 benchmark.")


def cmd_index(args):
    sys.exit("not implemented (Phase 3): build FAISS IVF + binary-Matryoshka + int8 payload "
             "+ Minerva frozen answers -> it.anima. Gated on the Phase 0 benchmark.")


def main():
    p = argparse.ArgumentParser(description="ANIMA offline factory")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("commands", help="validate intents and emit the SD command pack")
    c.add_argument("intents", help="path to intents.<lang>.json")
    c.add_argument("-o", "--out", help="output path (default commands.<lang>.json)")
    c.set_defaults(func=cmd_commands)

    e = sub.add_parser("encoder", help="(Phase 2) distill the on-device encoder")
    e.set_defaults(func=cmd_encoder)

    i = sub.add_parser("index", help="(Phase 3) build the binary-MRL retrieval index")
    i.set_defaults(func=cmd_index)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
