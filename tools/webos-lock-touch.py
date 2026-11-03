#!/usr/bin/env python3
"""Refresh webos.lock digests for managed SOURCE files (not .gz twins) after web/shell or apps edits."""
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "webos.lock")
MANAGED_APP = re.compile(r"^apps/[^/]+/(manifest\.json|www/.+)$")


def digest(path: str) -> str:
    with open(path, "rb") as f:
        data = f.read()
    if b"\0" not in data[:8000]:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def tracked_sources() -> dict[str, str]:
    out = subprocess.check_output(["git", "-C", ROOT, "ls-files", "-z", "--", "web/shell", "apps"]).decode()
    files = {}
    for p in out.split("\0"):
        if not p or p.endswith(".gz"):
            continue
        if p.startswith("web/shell/") or MANAGED_APP.match(p):
            full = os.path.join(ROOT, p)
            if os.path.isfile(full):
                files[p] = full
    return files


def main() -> int:
    with open(LOCK, encoding="utf-8") as f:
        lock = json.load(f)
    files = lock.setdefault("files", {})
    kept = set(lock.get("kept") or [])
    for rel, full in tracked_sources().items():
        if rel in kept:
            continue
        files[rel] = digest(full)
    # Drop stale entries removed from the tree (keeps lock aligned with git ls-files).
    live = set(tracked_sources())
    for rel in list(files):
        if rel not in live and rel not in kept:
            del files[rel]
    with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
        json.dump(lock, f, indent=1, sort_keys=True, ensure_ascii=False)
        f.write("\n")
    print(f"webos.lock: refreshed {len(files)} source digests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
