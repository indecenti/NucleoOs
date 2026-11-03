#!/usr/bin/env python3
"""Generate zh i18n catalogs from English bases (MyMemory, deduped, parallel)."""
from __future__ import annotations

import json
import re
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE_PATH = ROOT / "tools" / ".i18n-zh-cache.json"
PH_RE = re.compile(r"\{(\w+)\}")
WORKERS = 6


def discover_en_files() -> list[Path]:
    out: list[Path] = []
    shell_i18n = ROOT / "web" / "shell" / "i18n"
    for name in ("core", "shell"):
        p = shell_i18n / f"{name}.en.json"
        if p.is_file():
            out.append(p)
    for app_dir in sorted((ROOT / "apps").iterdir()):
        p = app_dir / "www" / "i18n.en.json"
        if p.is_file():
            out.append(p)
    return out


def protect(s: str) -> tuple[str, dict[str, str]]:
    mapping: dict[str, str] = {}

    def repl(m: re.Match[str]) -> str:
        tok = f"__PH{len(mapping)}__"
        mapping[tok] = m.group(0)
        return tok

    return PH_RE.sub(repl, s), mapping


def restore(s: str, mapping: dict[str, str]) -> str:
    for tok, orig in mapping.items():
        s = s.replace(tok, orig)
    return s


def mymemory_translate(text: str) -> str:
    protected, mapping = protect(text)
    q = urllib.parse.quote(protected[:480], safe="")
    url = f"https://api.mymemory.translated.net/get?q={q}&langpair=en|zh-CN"
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=45) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            out = data.get("responseData", {}).get("translatedText") or text
            return restore(out, mapping)
        except Exception:
            time.sleep(0.8 * (attempt + 1))
    return text


def collect_strings(obj, out: set[str]) -> None:
    if isinstance(obj, str):
        out.add(obj)
    elif isinstance(obj, dict):
        for v in obj.values():
            collect_strings(v, out)


def translate_value(val, cache: dict[str, str]):
    if isinstance(val, str):
        return cache.get(val, val)
    if isinstance(val, dict):
        return {k: cache.get(v, v) if isinstance(v, str) else v for k, v in val.items()}
    return val


def main() -> int:
    files = discover_en_files()
    cache: dict[str, str] = {}
    if CACHE_PATH.is_file():
        cache = json.loads(CACHE_PATH.read_text(encoding="utf-8"))
    all_strings: set[str] = set()
    catalogs = []
    for p in files:
        data = json.loads(p.read_text(encoding="utf-8-sig"))
        catalogs.append((p, data))
        for k, v in data.items():
            if not k.startswith("_"):
                collect_strings(v, all_strings)
    todo = [s for s in sorted(all_strings) if s not in cache and re.search(r"[A-Za-z]", s)]
    print(f"{len(files)} catalogs, {len(all_strings)} unique, {len(todo)} to translate", flush=True)

    def work(s: str) -> tuple[str, str]:
        time.sleep(0.05)
        return s, mymemory_translate(s)

    done = 0
    with ThreadPoolExecutor(max_workers=WORKERS) as ex:
        futs = {ex.submit(work, s): s for s in todo}
        for fut in as_completed(futs):
            src, out = fut.result()
            cache[src] = out
            done += 1
            if done % 80 == 0:
                CACHE_PATH.write_text(json.dumps(cache, ensure_ascii=False) + "\n", encoding="utf-8")
                print(f"  translated {done}/{len(todo)}", flush=True)
    CACHE_PATH.write_text(json.dumps(cache, ensure_ascii=False) + "\n", encoding="utf-8")

    for p, data in catalogs:
        zh_path = p.with_name(p.name.replace(".en.", ".zh."))
        out_obj: dict = {}
        for key, val in data.items():
            if key.startswith("_"):
                out_obj[key] = "zh" if key == "_lang" else val
                continue
            out_obj[key] = translate_value(val, cache)
        zh_path.write_text(json.dumps(out_obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {zh_path.relative_to(ROOT)}", flush=True)
    print("done", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
