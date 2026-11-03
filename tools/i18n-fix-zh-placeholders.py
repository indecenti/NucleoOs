#!/usr/bin/env python3
"""Restore {placeholder} tokens in zh catalogs from their en counterparts."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PH = re.compile(r"\{(\w+)\}")
MANGLED = re.compile(r"__\s*PH(\d+)\s*__", re.I)


def discover_pairs():
    for p in (ROOT / "web" / "shell" / "i18n").glob("*.en.json"):
        yield p, p.with_name(p.name.replace(".en.", ".zh."))
    for p in sorted((ROOT / "apps").glob("*/www/i18n.en.json")):
        yield p, p.with_name("i18n.zh.json")


def fix_string(en: str, zh: str) -> str:
    if not isinstance(en, str) or not isinstance(zh, str):
        return zh
    names = PH.findall(en)
    if not names:
        return zh
    out = zh
    for i, name in enumerate(names):
        ph = "{" + name + "}"
        out = MANGLED.sub(lambda m, idx=i, p=ph: p if int(m.group(1)) == idx else m.group(0), out, count=1)
        out = out.replace(f"__PH{i}__", ph)
    for name in names:
        ph = "{" + name + "}"
        if ph not in out:
            # Last resort: keep Chinese prose but inject placeholder from English order
            parts = PH.split(en)
            if len(parts) == 3 and parts[1] == name:
                out = out.rstrip("。") + ph
    return out


def fix_value(en_v, zh_v):
    if isinstance(en_v, str) and isinstance(zh_v, str):
        return fix_string(en_v, zh_v)
    if isinstance(en_v, dict) and isinstance(zh_v, dict):
        return {k: fix_string(en_v[k], zh_v.get(k, en_v[k])) if isinstance(en_v.get(k), str) else zh_v.get(k, en_v[k])
                for k in en_v}
    return zh_v


def main():
    n = 0
    for en_path, zh_path in discover_pairs():
        if not zh_path.is_file():
            continue
        en = json.loads(en_path.read_text(encoding="utf-8-sig"))
        zh = json.loads(zh_path.read_text(encoding="utf-8-sig"))
        changed = False
        for key, en_v in en.items():
            if key.startswith("_") or key not in zh:
                continue
            fixed = fix_value(en_v, zh[key])
            if fixed != zh[key]:
                zh[key] = fixed
                changed = True
        if changed:
            zh_path.write_text(json.dumps(zh, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            n += 1
            print(zh_path.relative_to(ROOT))
    print(f"fixed {n} files")


if __name__ == "__main__":
    main()
