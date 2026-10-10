#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NucleoOS — SD Deploy
====================
Provisioning system for the Cardputer SD card, with a Tkinter UI. Three operations:

  • PROVISION (blank/new SD)   : assemble the full payload from ALL the canonical
        sources and write it to a blank SD, creating the structure, the user
        folders and clean state templates (no key, no learned cards).
  • UPDATE (existing SD)    : update ONLY the system files (apps, www, registry,
        ANIMA knowledge) PRESERVING the device state (Groq key, learned cards,
        settings, user data). NEVER deletes anything.
  • VERIFY                     : compare the SD against the master (hash) and report missing/different files.

The "master" is assembled in deploy/sd-master/ by gathering from:
  registry/  apps/  web/shell/  web/downloads/ (app companion)  tools/sd-sim/data/
  + the ANIMA packs (encoder, index, manifest+akb5 shards)  + TTS voice  + evilportal/
  wallpapers/  README — filling the gaps the old pipeline (deploy.ps1 / sd-safe)
  left open (akb5 outside the pipeline, SD-only wallpapers, downloads). Every
  .js/.css/.html (and the evilportal portals) is gzipped (the firmware serves the .gz), and the
  .factory manifests of the factory games are (re)generated from the real payload.

stdlib only (tkinter, ctypes, hashlib, gzip, shutil). Launch:
    python tools/nucleo-sd-deploy/sd_deploy.py
"""
import os, sys, json, gzip, shutil, hashlib, threading, queue, time, fnmatch, string, subprocess
from pathlib import Path

# ---------------------------------------------------------------- repo layout
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent                       # tools/nucleo-sd-deploy -> repo root
MASTER = REPO / "deploy" / "sd-master"          # assembled, verifiable payload
MANIFEST_NAME = ".deploy-manifest.json"
GZ_EXT = {".js", ".css", ".html"}

# ---------------------------------------------------------------- language
# Bilingual IT/EN. LANG is global so the core logs are translated too; the GUI
# toggles it with the switch.
LANG = "it"
def T(it, en):
    return en if LANG == "en" else it

# ---------------------------------------------------------------- source map
# Each rule: destination relative to the SD <- first EXISTING source among the candidates.
# kind: 'tree' (recursive folder) | 'file' (single). gz: generate the .gz for js/css/html.
def _src(*cands):
    return [REPO / c for c in cands]

# NB: this classification (SOURCE_MAP = system, DEVICE_STATE = user state) is the same one
# the firmware applies AT RUNTIME to prevent DELETION of system files from the SD:
# see firmware/components/nucleo_board/include/nucleo_fsprotect.h (nucleo_fs_is_protected), which
# blocks delete/move-away of system/registry, system/web, apps/, www/, of the VOICE (TTS clips in
# data/tts/<lang>/ + Vosk models under apps/) and of the ANIMA brain
# (data/anima/{anima-*,dict-*,commands*,akb5/}) for the file manager, ANIMA, the JS runtime and the Files app.
# If you add a new system tree here, update that predicate too (and vice versa).
SOURCE_MAP = [
    # system + registry (repo sources, fresh)
    dict(dest="system/registry", kind="tree", gz=False, src=_src("registry")),
    # app (complete repo sources: they include tour.js/nlcommand.js, which sd-safe used to miss)
    dict(dest="apps",            kind="tree", gz=True,  src=_src("apps")),
    # web shell
    dict(dest="www/shell",       kind="tree", gz=True,  src=_src("web/shell")),
    # companion apps downloadable from the shell (e.g. NucleoConnect.exe) — they live in web/downloads/, not in web/shell/
    dict(dest="www/shell/downloads", kind="tree", gz=False, src=_src("web/downloads")),
    # user-data seed + ANIMA base (encoder/index/dict/commands) — does NOT include akb5
    dict(dest="data",            kind="tree", gz=False, src=_src("tools/sd-sim/data")),
    # ANIMA akb5 KNOWLEDGE — the gap in the old pipeline. 46 complete shards.
    dict(dest="data/anima/akb5",            kind="tree", gz=False,
         src=_src("deploy/sd-safe/data/anima/akb5")),
    dict(dest="data/anima/anima-it-akb5.bin", kind="file", gz=False,
         src=_src("deploy/sd-safe/data/anima/anima-it-akb5.bin", "models/anima-it-akb5.bin")),
    # IR remote presets (read-only, firmware nucleo_ir reads /system/ir/presets.bin). Built by
    # tools/ir-pack.mjs into the sd-sim tree; deploy.ps1 already shipped it, this map had missed it.
    dict(dest="system/ir",       kind="tree", gz=False, src=_src("tools/sd-sim/system/ir")),
    # The native games' es/fr/de text (firmware game_text.cpp reads /system/i18n/games/<game>.<lang>).
    # Built by tools/game-i18n/build.mjs from tools/game-i18n/*.json; system part, never user state.
    dict(dest="system/i18n",     kind="tree", gz=False, src=_src("tools/sd-sim/system/i18n")),
    # SPOKEN voice: concatenative TTS clip bank (nucleo_tts), IT+EN. clips.pcm is oversized
    # (fetched by oversized-assets/rejoin.mjs); index.bin is committed. System part, not user
    # state -> always written, never deleted. (The LISTENING voice — split-part Vosk models —
    # rides the 'apps' rule above.)
    dict(dest="data/tts",        kind="tree", gz=False, src=_src("deploy/sd-safe/data/tts")),
    # payload-only extras — captive portals are served gz (the firmware prefers .gz)
    dict(dest="evilportal",      kind="tree", gz=True,  src=_src("deploy/sd-safe/evilportal")),
    dict(dest="wallpapers",      kind="tree", gz=False, src=_src("deploy/sd-safe/wallpapers")),
    dict(dest="README.md",       kind="file", gz=False, src=_src("deploy/sd-safe/README.md")),
]

# Critical files that MUST exist in the master, otherwise provisioning is incomplete.
COMPLETENESS = [
    ("encoder ANIMA",    "data/anima/anima-it-encoder.bin"),
    ("index ANIMA",      "data/anima/anima-it-index.bin"),
    ("manifest akb5",    "data/anima/anima-it-akb5.bin"),
    ("shell index.html", "www/shell/index.html"),
    ("registry apps",    "system/registry/apps.json"),
    ("app companion",    "www/shell/downloads/NucleoConnect.exe"),
    # VOICE (integral system part): TTS clips (speaks) + split-part Vosk models (listens).
    # A missing clips.pcm = oversized asset not fetched -> 'node oversized-assets/rejoin.mjs'.
    ("TTS voice it (clips)", "data/tts/it/clips.pcm"),
    ("TTS voice en (clips)", "data/tts/en/clips.pcm"),
    ("voce TTS it (idx)",  "data/tts/it/index.bin"),
    ("dettatura Vosk it",  "apps/anima/www/vosk/models/vosk-model-small-it-0.4.tar.gz.000"),
    ("dettatura Vosk en",  "apps/anima/www/vosk/models/vosk-model-small-en-us-0.15.tar.gz.000"),
]
MIN_AKB5_SHARDS = 40

# .factory manifests: block deletion of the FACTORY games (firmware
# nucleo_fsfactory.h). Generated HERE from the real master payload, so the lock
# matches exactly what you ship (same rules as tools/gen-factory-manifests.py).
FACTORY_NAME = ".factory"
FACTORY_HEADER = ("# Bundled factory games -- pinned against deletion. "
                  "Generated by tools/gen-factory-manifests.py. Do not edit.\n")
FACTORY_TARGETS = [
    ("data/DOS",      {".jsdos", ".com", ".exe", ".bat", ".zip"}),
    ("data/ROMs/gb",  {".gb"}),
    ("data/ROMs/gbc", {".gbc"}),
    ("data/ROMs/gg",  {".gg"}),
    ("data/ROMs/nes", {".nes"}),
    ("data/ROMs/sms", {".sms"}),
]

# ---------------------------------------------------------------- device state
# DEVICE / USER STATE (API key, learned caches, settings, keys, the user's documents...): on PROVISION a
# clean template is written; on UPDATE it is PRESERVED (never overwritten, never deleted). The table is
# tools/lib/sd-policy.json, shared with deploy.ps1, sd-sync.ps1, push-ota and sd-net-sync (see its _doc);
# tools/sd-policy.test.mjs holds every implementation to the same vectors.
import re
SD_POLICY = json.loads((REPO / "tools" / "lib" / "sd-policy.json").read_text(encoding="utf-8"))

def _glob_re(glob):
    """'**/' = zero or more segments, '**' = anything, '*' = within one segment; case-insensitive."""
    out, i = "", 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out += "(?:.*/)?"; i += 3
        elif glob.startswith("**", i):
            out += ".*"; i += 2
        elif glob[i] == "*":
            out += "[^/]*"; i += 1
        else:
            out += re.escape(glob[i]); i += 1
    return re.compile("^" + out + "$", re.IGNORECASE)

_STATE_RE = [_glob_re(g) for g in SD_POLICY["state"]]
_ANIMA_SHIP_RE = [_glob_re(g) for g in SD_POLICY["animaShip"]]
DEVICE_STATE = SD_POLICY["state"]          # kept for readers of the old name
# User folders to create empty on a new SD.
USER_DIRS = ["data/Music", "data/Videos", "data/Pictures", "data/Documents", "data/Notes",
             "data/Recordings", "data/ROMs", "data/DOS", "data/Transcripts",
             "data/downloads", "data/shared", "data/imports", "data/exports"]
TEACHER_TEMPLATE = {"provider": "groq", "model": "llama-3.3-70b-versatile", "key": ""}

# ---------------------------------------------------------------- registry merge
# system/registry/apps.json on a card in use also lists the user's own web apps (published by the Agent app,
# "created_by": "agent"). Overwriting it with the release copy uninstalled them. Twin of
# tools/lib/registry-merge.mjs mergeRegistryText(); both are held to tools/lib/registry-merge-vectors.json.
REGISTRY_REL = "system/registry/apps.json"

def _registry_parse(text):
    if text is None:
        return None
    try:
        doc = json.loads(text.lstrip("﻿"))
    except Exception:
        return None
    return doc if isinstance(doc, dict) and isinstance(doc.get("installed"), list) else None

def merge_registry_text(release_text, device_text):
    """-> (text, kept_ids, shadowed_ids, device_readable). Release text returned byte-exact when nothing
    is carried over; raises ValueError if the RELEASE registry is malformed."""
    release = _registry_parse(release_text)
    if release is None:
        raise ValueError("release registry is malformed")
    device = _registry_parse(device_text)
    sys_ids = {e.get("id") for e in release["installed"] if isinstance(e, dict) and isinstance(e.get("id"), str)}
    kept, shadowed, extra = [], [], []
    for e in (device or {}).get("installed", []):
        if not (isinstance(e, dict) and e.get("created_by") == "agent" and isinstance(e.get("id"), str)):
            continue
        if e["id"] in sys_ids:
            if e["id"] not in shadowed:
                shadowed.append(e["id"])
            continue
        if e["id"] in kept:
            continue
        kept.append(e["id"]); extra.append(e)
    if not kept:
        return release_text, kept, shadowed, device is not None
    doc = dict(release); doc["installed"] = release["installed"] + extra
    eol = "\r\n" if "\r\n" in release_text else "\n"
    text = json.dumps(doc, indent=2, ensure_ascii=False)
    if eol != "\n":
        text = text.replace("\n", eol)
    if release_text.endswith("\n"):
        text += eol
    return text, kept, shadowed, device is not None

# ---------------------------------------------------------------- stale .gz twins
# The device's webfs serves "<file>.gz" instead of "<file>" in /www/shell and /apps/<id>/www, so a stale twin
# shadows new code. The firmware drops it when <file> is written over the API (nucleo_fsapi fstwin.c); a
# card written directly must do the same: a twin the payload does not ship, next to a raw file it does, is
# stale by definition. Same scope as the firmware, held to tools/lib/twin-scope-vectors.json.
import re as _re
_TWIN_RE = _re.compile(r"^(www/shell|apps/[^/]+/www)/.", _re.IGNORECASE)

def twin_scope(rel):
    return bool(_TWIN_RE.match(rel or ""))

def stale_twins(staged, card_has):
    """staged: set of SD-relative paths shipped; card_has(rel)->bool. Returns twin paths to remove."""
    return sorted(r + ".gz" for r in staged
                  if twin_scope(r) and not r.lower().endswith(".gz") and (r + ".gz") not in staged
                  and card_has(r + ".gz"))

def is_state(rel):
    """Device/user state per tools/lib/sd-policy.json (same semantics as tools/lib/sd-policy.mjs)."""
    p = (rel or "").replace("\\", "/").lstrip("/")
    if any(r.match(p) for r in _STATE_RE):
        return True
    if p.lower().startswith("data/anima/"):
        return not any(r.match(p) for r in _ANIMA_SHIP_RE)
    return False

# ---------------------------------------------------------------- helpers
def sha256(path, buf=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(buf), b""):
            h.update(b)
    return h.hexdigest()

def gz_file(src, dst):
    # Deterministic: no source name and mtime 0 in the gzip header, so an unchanged file always yields
    # byte-identical .gz — otherwise every build changes every twin's hash and a delta sync (the device
    # SD-content installer, push-ota --sync) would re-send all of them.
    with open(src, "rb") as fi, open(dst, "wb") as raw, \
         gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=raw, mtime=0) as fo:
        shutil.copyfileobj(fi, fo)

# ---------------------------------------------------------------- Windows drives
def list_drives():
    """Return [(root, type_str, label, free_gb, total_gb)] for the drives present."""
    out = []
    if os.name != "nt":
        return out
    import ctypes
    k = ctypes.windll.kernel32
    DRIVE = {2: "Removable", 3: "Fixed", 4: "Network", 5: "CD-ROM"}
    bitmask = k.GetLogicalDrives()
    for i, L in enumerate(string.ascii_uppercase):
        if not (bitmask & (1 << i)):
            continue
        root = f"{L}:\\"
        t = k.GetDriveTypeW(ctypes.c_wchar_p(root))
        if t not in (2, 3):           # removable and fixed only (never network/cd)
            continue
        label = _vol_label(root)
        free, total = _free_total(root)
        out.append((root, DRIVE.get(t, str(t)), label, free, total))
    return out

def _vol_label(root):
    import ctypes
    buf = ctypes.create_unicode_buffer(256)
    fsbuf = ctypes.create_unicode_buffer(256)
    try:
        ctypes.windll.kernel32.GetVolumeInformationW(
            ctypes.c_wchar_p(root), buf, 256, None, None, None, fsbuf, 256)
        return buf.value or ""
    except Exception:
        return ""

def _free_total(root):
    import ctypes
    free = ctypes.c_ulonglong(0); total = ctypes.c_ulonglong(0)
    try:
        ctypes.windll.kernel32.GetDiskFreeSpaceExW(
            ctypes.c_wchar_p(root), None, ctypes.byref(total), ctypes.byref(free))
        return free.value / 2**30, total.value / 2**30
    except Exception:
        return 0.0, 0.0

def _root(p):
    """Normalize a drive root: 'H:' / 'H:\\' -> 'H:\\'. Without the backslash, Path('H:') is
    drive-RELATIVE (cwd on H:) and every join points to the wrong place."""
    p = str(p)
    if len(p) == 2 and p[1] == ":":
        p += os.sep
    return p

def is_system_drive(root):
    sysroot = os.environ.get("SystemDrive", "C:")
    return root.rstrip("\\").upper().startswith(sysroot.upper())

def drive_is_removable(root):
    for r, t, *_ in list_drives():
        if r.upper() == root.upper():
            return t == "Removable"
    return False

# ---------------------------------------------------------------- detection
def detect_target(root):
    """blank | nucleoos | non-sd. Cardputer SD = ha system/ o .deploy-manifest.json."""
    p = Path(root)
    if not p.exists():
        return "missing"
    has_sys = (p / "system").exists()
    has_man = (p / MANIFEST_NAME).exists()
    has_anima = (p / "data" / "anima").exists()
    if has_sys or has_man or has_anima:
        return "nucleoos"
    # empty or nearly so (only System Volume Information / FS metadata)
    entries = [e for e in p.iterdir() if e.name not in ("System Volume Information", "$RECYCLE.BIN")]
    return "blank" if not entries else "foreign"

# ---------------------------------------------------------------- assemble master
def assemble_master(log, master=MASTER, progress=None):
    """Gather all the sources -> master/, gz, manifest. Returns (stats, warnings).
    Optional progress(frac 0..1, text) for the progress bar."""
    master = Path(master)
    stats = dict(copied=0, gz=0, bytes=0, factory=0, akb5=0, files=0)
    warns = []
    log(f"Master: {master}")
    # source count for the percentage (the copy is ~80%, the manifest ~20%)
    total_src = 0
    for rule in SOURCE_MAP:
        src = next((s for s in rule["src"] if s.exists()), None)
        if src is None:
            continue
        total_src += 1 if rule["kind"] == "file" else sum(1 for f in src.rglob("*") if f.is_file())
    done = 0
    def tick(text):
        if progress and total_src:
            progress(0.80 * done / total_src, text)
    for rule in SOURCE_MAP:
        src = next((s for s in rule["src"] if s.exists()), None)
        if src is None:
            warns.append(f"sorgente mancante per '{rule['dest']}' (provati: "
                         + ", ".join(str(s.relative_to(REPO)) for s in rule["src"]) + ")")
            log(f"  ⚠ {rule['dest']}: nessuna sorgente")
            continue
        dest = master / rule["dest"].replace("/", os.sep)
        if rule["kind"] == "file":
            dest.parent.mkdir(parents=True, exist_ok=True)
            _copy_one(src, dest, stats)
            done += 1; tick(f"Copia {done}/{total_src}")
        else:
            for f in sorted(src.rglob("*")):
                if f.is_file():
                    rel = f.relative_to(src)
                    d = dest / rel
                    d.parent.mkdir(parents=True, exist_ok=True)
                    _copy_one(f, d, stats)
                    if rule["gz"] and f.suffix.lower() in GZ_EXT:
                        gz_file(f, str(d) + ".gz"); stats["gz"] += 1
                    done += 1; tick(f"Copia {done}/{total_src}")
        log(f"  ✓ {rule['dest']:<28} <- {src.relative_to(REPO)}")
    # .factory: pins the factory games against deletion (after the copy, before the manifest)
    nfact = write_factory_manifests(master, log)
    stats["factory"] = nfact
    log(f"  .factory: {nfact} giochi pinnati")
    # completeness
    for label, rel in COMPLETENESS:
        if not (master / rel.replace("/", os.sep)).exists():
            warns.append(f"CRITICO mancante: {label} ({rel})")
    akb5 = master / "data" / "anima" / "akb5"
    n_shards = len(list(akb5.glob("*.bin"))) if akb5.exists() else 0
    stats["akb5"] = n_shards
    if n_shards < MIN_AKB5_SHARDS:
        warns.append(f"akb5 incompleto: {n_shards} shard (<{MIN_AKB5_SHARDS})")
    log(f"  akb5 shard: {n_shards}")
    # manifest (with progress on the hashing, the slow part)
    files = [f for f in master.rglob("*") if f.is_file() and f.name != MANIFEST_NAME]
    man = {}
    for i, f in enumerate(files):
        rel = f.relative_to(master).as_posix()
        man[rel] = {"size": f.stat().st_size, "hash": sha256(f)}
        if progress and files:
            progress(0.80 + 0.20 * (i + 1) / len(files), f"Manifest {i + 1}/{len(files)}")
    (master / MANIFEST_NAME).write_text(json.dumps(man, indent=1), encoding="utf-8")
    stats["files"] = len(man)
    log(f"Master pronto: {len(man)} file, {stats['bytes']/2**20:.1f} MB, {stats['gz']} gz")
    return stats, warns

def _copy_one(src, dst, stats):
    shutil.copy2(src, dst)
    stats["copied"] += 1
    stats["bytes"] += src.stat().st_size

def write_factory_manifests(master, log):
    """Create the .factory files in the master's factory games (anti-deletion).
    Mirrors tools/gen-factory-manifests.py but on the freshly assembled payload."""
    master = Path(master)
    total = 0
    for rel, exts in FACTORY_TARGETS:
        folder = master / rel.replace("/", os.sep)
        if not folder.is_dir():
            continue
        names = sorted((p.name for p in folder.iterdir()
                        if p.is_file() and p.name != FACTORY_NAME
                        and p.suffix.lower() in exts), key=str.lower)
        if not names:
            continue
        body = FACTORY_HEADER + "".join(n + "\n" for n in names)
        (folder / FACTORY_NAME).write_text(body, encoding="utf-8", newline="\n")
        total += len(names)
        log(f"  ✓ .factory {rel:<24} ({len(names)} giochi pinnati)")
    return total

# ---------------------------------------------------------------- deploy ops
def _iter_master(master):
    master = Path(master)
    for f in master.rglob("*"):
        if f.is_file() and f.name != MANIFEST_NAME:
            yield f, f.relative_to(master).as_posix()

def provision(root, mode, dry, log, master=MASTER, progress=None):
    """mode='fresh' (new SD) | 'update' (preserves state). Returns stats."""
    master = Path(master)
    if not (master / MANIFEST_NAME).exists():
        raise RuntimeError("master non assemblato — premi prima 'Assembla master'")
    dst_root = Path(_root(root))
    st = dict(written=0, skipped=0, state_kept=0, bytes=0)
    files = list(_iter_master(master))
    total = len(files)
    for i, (f, rel) in enumerate(files):
        if progress and total:
            progress((i + 1) / total, f"{'Anteprima' if dry else 'Scrittura'} {i + 1}/{total}")
        if is_state(rel):
            # device state: always preserved on update; on fresh the template is written afterwards
            st["state_kept"] += 1
            continue
        dst = dst_root / rel.replace("/", os.sep)
        if rel == REGISTRY_REL:
            # Merge, never overwrite: keep the user's Agent-published apps (see merge_registry_text).
            # bytes, not read_text(): text mode would rewrite CRLF and break the byte-exact release copy
            rel_text = f.read_bytes().decode("utf-8")
            dev_text = dst.read_bytes().decode("utf-8", errors="replace") if dst.exists() else None
            text, kept, shadowed, _ = merge_registry_text(rel_text, dev_text)
            if kept or shadowed:
                log(f"  registry: kept user apps {kept}" + (f", shadowed by system apps {shadowed}" if shadowed else ""))
            if dev_text == text:
                st["skipped"] += 1
                continue
            if not dry:
                dst.parent.mkdir(parents=True, exist_ok=True)
                tmp = dst.with_suffix(dst.suffix + ".nctmp")
                tmp.write_bytes(text.encode("utf-8"))
                os.replace(tmp, dst)
            st["written"] += 1; st["bytes"] += len(text.encode("utf-8"))
            continue
        if dst.exists() and dst.stat().st_size == f.stat().st_size and sha256(dst) == sha256(f):
            st["skipped"] += 1
            continue
        if not dry:
            dst.parent.mkdir(parents=True, exist_ok=True)
            tmp = dst.with_suffix(dst.suffix + ".nctmp")
            shutil.copy2(f, tmp)
            os.replace(tmp, dst)
        st["written"] += 1; st["bytes"] += f.stat().st_size
    # stale .gz twins the payload no longer ships (see twin_scope): the only files this tool removes
    staged = {rel for _, rel in files}
    for twin in stale_twins(staged, lambda r: (dst_root / r.replace("/", os.sep)).is_file()):
        if not dry:
            (dst_root / twin.replace("/", os.sep)).unlink()
        log(f"  {'[DRY] ' if dry else ''}stale twin removed: {twin}")
        st["twins"] = st.get("twins", 0) + 1
    if mode == "fresh":
        _write_fresh_state(dst_root, dry, log, st)
    # manifest on the SD
    if not dry:
        shutil.copy2(master / MANIFEST_NAME, dst_root / MANIFEST_NAME)
    log(f"{'[DRY] ' if dry else ''}{mode}: scritti {st['written']}, invariati {st['skipped']}, "
        f"stato-device {'creato' if mode=='fresh' else 'preservato'} {st['state_kept']}, "
        f"{st['bytes']/2**20:.1f} MB")
    return st

def _write_fresh_state(dst_root, dry, log, st):
    """New SD: clean templates (empty key, empty learned) + user folders."""
    if dry:
        log("[DRY] creerei: teacher.json template, learned/ vuoto, "
            + str(len(USER_DIRS)) + " cartelle utente")
        return
    anima = dst_root / "data" / "anima"
    anima.mkdir(parents=True, exist_ok=True)
    # NEVER clobber an existing API key: write the empty template ONLY on a card that has none.
    # 'fresh' may be run on a card that already has NucleoOS (the GUI warns), and the user's Groq/
    # Claude key must survive — matching the dialog's promise that the key is preserved.
    tj = anima / "teacher.json"
    if tj.exists():
        log("teacher.json gia' presente -> chiave API preservata (non sovrascritta)")
    else:
        tj.write_text(json.dumps(TEACHER_TEMPLATE, indent=2), encoding="utf-8")
        log("teacher.json creato (template, chiave vuota)")
    (anima / "learned").mkdir(exist_ok=True)
    for d in USER_DIRS:
        (dst_root / d.replace("/", os.sep)).mkdir(parents=True, exist_ok=True)
    log(f"Stato device fresco: learned/ + {len(USER_DIRS)} cartelle utente assicurate")

def verify(root, log, master=MASTER, progress=None):
    """Compare master vs SD by hash. Returns (missing, diff, ok)."""
    master = Path(master)
    man = json.loads((master / MANIFEST_NAME).read_text(encoding="utf-8"))
    dst_root = Path(_root(root))
    missing, diff, ok = [], [], 0
    items = list(man.items())
    for i, (rel, meta) in enumerate(items):
        if progress and items:
            progress((i + 1) / len(items), f"Verifica {i + 1}/{len(items)}")
        if is_state(rel):
            continue
        dst = dst_root / rel.replace("/", os.sep)
        if not dst.exists():
            missing.append(rel)
        elif dst.stat().st_size != meta["size"] or sha256(dst) != meta["hash"]:
            diff.append(rel)
        else:
            ok += 1
    log(f"VERIFY: ok={ok}  mancanti={len(missing)}  diversi={len(diff)} "
        f"(stato-device escluso)")
    for m in missing[:30]:
        log("  MANCA  " + m)
    for d in diff[:30]:
        log("  DIVERSO " + d)
    return missing, diff, ok

# ---------------------------------------------------------------- release payload (headless, CI)
# The PUBLIC SD payload: what the release zip carries and what the device's SD-content installer
# downloads file by file (docs/sd-content-install.md). Same SOURCE_MAP / DEVICE_STATE as every other
# path here, plus release-only filters. Built straight from the sources (never from deploy/sd-safe as a
# whole), filtered while copying, and checked against the SAME write allow-list the device enforces.
RELEASE_MANIFEST = "sd-manifest.txt"
RELEASE_MARKER = ".nucleo-release"     # <out>.nucleo-release NEXT TO the out dir: "build_release made this"
RELEASE_MANIFEST_VERSION = 1
# Heavy OPTIONAL assets a first install doesn't need (installable in-app) — same list package-release.mjs
# used for the zip: image-diffusion + speech + WebLLM models and the big runtime wasm.
RELEASE_HEAVY = ["/models/", "/vendor/onnxruntime-web/", "/vendor/ffmpeg/", "/vendor/wllama/", "/forge/vendor/"]
RELEASE_HEAVY_EXT = (".pcm", ".gguf", ".npy", ".onnx")
# Payload files that aren't needed ON THE CARD: the stale sd-safe README (would also collide with other
# firmwares' README on a shared M5Launcher card) and the dev sd-sim's user-area scratch file.
# data/tts/**: the voice index only matches the clip bank it was built with; clips.pcm is never released
# (oversized), so shipping index.bin alone could desync a card's older, hand-installed clips. The voice
# ships as a PAIR in a future voice pack, never half. (Dropped in build_release's filter.)
RELEASE_DROP = {"README.md", "data/apps/test.lua",
                "data/ir/nucleo-remotes.ir"}   # a dev-sim user remote (IR app user data), not payload
# Packs: everything is 'core' except the companion installers the web shell links (phone APK, Windows
# exe) — useful to the browser, not to the Cardputer — and the arcade emulator cores. Optional packs are
# installed only on request.
RELEASE_PACKS = [("www/shell/downloads/", "downloads"),
                 # the Arcade app's emulator cores (~20 MB of wasm): only needed to play arcade ROMs
                 ("apps/arcade/www/emulatorjs/", "arcade")]
# Files the release MUST contain (a subset of COMPLETENESS: TTS clips / Vosk / exe are not in git).
RELEASE_REQUIRED = [
    "www/shell/index.html",
    "system/registry/apps.json",
    "system/ir/presets.bin",
    "data/anima/anima-it-encoder.bin",
    "data/anima/anima-it-index.bin",
    "data/anima/anima-it-akb5.bin",
    "data/anima/learned/facets.it.jsonl",
    "data/anima/learned/facets.en.jsonl",
]
# apps.json is MERGED on the device (the Agent app adds the user's own apps to it), never overwritten.
RELEASE_MERGE = {"system/registry/apps.json"}

def release_path_allowed(rel):
    """The write allow-list, mirrored by the firmware installer's policy (defense in depth: a manifest
    line outside it is refused on the device too). Returns '' if allowed, else the reason."""
    if not rel or len(rel) > 200:
        return "length"
    if any(ord(c) < 0x20 or ord(c) > 0x7e for c in rel) or "\\" in rel:
        return "charset"
    if rel.startswith("/") or "//" in rel or rel.endswith("/"):
        return "shape"
    parts = rel.split("/")
    if any(p in ("", ".", "..") for p in parts):
        return "dot-segment"
    if is_state(rel):
        return "device-state"
    top = parts[0]
    if top == "www":
        return "" if len(parts) > 2 and parts[1] == "shell" else "root"
    if top == "apps":
        if len(parts) < 3:
            return "apps-root"                          # apps/theme.cfg and friends are device state
        if parts[2] == "data":
            return "app-data"                           # apps/<id>/data/** is the app's user data
        return ""
    if top == "system":
        return "" if len(parts) > 2 and parts[1] in ("registry", "ir", "i18n") else "system"
    if top == "data":
        if len(parts) >= 3 and parts[1] == "anima":
            name = parts[2]
            if len(parts) == 3 and (name.startswith(("anima-", "dict-", "commands"))):
                return ""
            if len(parts) == 4 and name == "akb5" and parts[3].endswith(".bin"):
                return ""
            if len(parts) == 4 and name == "learned" and parts[3] in ("facets.it.jsonl", "facets.en.jsonl"):
                return ""
            return "anima-state"
        if len(parts) == 4 and parts[1] == "tts" and parts[2] in ("it", "en"):
            return ""
        # a game's arcade sound pack (data/<game>/pack/<cue>.wav, rendered by tools/sfx-gen): shipped
        # content, never the game's own saves/config next to it
        if len(parts) == 4 and parts[1] and parts[2] == "pack" and parts[3].lower().endswith(".wav"):
            return ""
        # a native game's tile atlas (data/<game>/atlas.bin, assets/*/build_atlas): read-only art
        if len(parts) == 3 and parts[1] and parts[2].lower() == "atlas.bin":
            return ""
        return "data"
    if top in ("wallpapers", "evilportal"):
        return "" if len(parts) > 1 else "root"
    return "root"

def _akb5_shards(list_path):
    """Shard file names referenced by an AKB5 manifest (tools/anima/build_akb5.py MANIFEST layout:
    'AKB5' | u32 D | u32 n | n x {u8 namelen, name, u32 ncards, u32 N, u16 K} | centroid block)."""
    import struct
    b = Path(list_path).read_bytes()
    if b[:4] != b"AKB5":
        raise ValueError(f"{list_path}: not an AKB5 manifest")
    _, n = struct.unpack_from("<II", b, 4)
    o, names = 12, []
    for _ in range(n):
        ln = b[o]; o += 1
        names.append(b[o:o + ln].decode("utf-8")); o += ln + 4 + 4 + 2
    return names

def _pack_of(rel):
    for prefix, pack in RELEASE_PACKS:
        if rel.startswith(prefix):
            return pack
    return "core"

def build_release(out, tag, log, manifest_path=None):
    """Assemble the public SD payload into `out` + write the device manifest. Returns (stats, errors);
    any error means the payload must not ship. Never touches deploy/sd-master or a card."""
    out = Path(out).resolve()
    # `out` is wiped and rebuilt: never let that be a card, a drive root or anything outside a build dir.
    if out.parent == out or len(out.parts) < 3:
        raise RuntimeError(f"release: refusing to use {out} (drive root / too shallow)")
    if out.exists():
        looks_like_card = any((out / m).exists() for m in (MANIFEST_NAME, "system", "data", "apps", "www"))
        if looks_like_card and not out.with_name(out.name + RELEASE_MARKER).exists():
            raise RuntimeError(f"release: {out} looks like an SD card or a foreign tree, refusing to wipe it")
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # The marker sits BESIDE the tree, never inside it (it would ship in the zip onto users' cards).
    out.with_name(out.name + RELEASE_MARKER).write_text("sd_deploy.py release output - safe to delete\n",
                                                        encoding="ascii")
    # 1) plan: rel -> ('copy', src) | ('gz', raw_src). Later rules win, exactly like assemble_master's
    #    copy order (e.g. the sd-safe akb5 list replaces the sd-sim one).
    plan = {}
    for rule in SOURCE_MAP:
        src = next((s for s in rule["src"] if s.exists()), None)
        if src is None:
            log(f"  - {rule['dest']}: no source (skipped)")
            continue
        if rule["kind"] == "file":
            plan[rule["dest"]] = ("copy", src)
            continue
        for f in sorted(src.rglob("*")):
            if not f.is_file():
                continue
            rel = f"{rule['dest']}/{f.relative_to(src).as_posix()}"
            raw = f.with_name(f.name[:-3]) if f.name.endswith(".gz") else None
            if raw is not None and raw.is_file():
                # A committed twin keeps the EOL of the machine that gzipped it (check-gz.mjs compares
                # EOL-normalized for that reason). Regenerate it from the raw source instead: the pair
                # is then byte-exact. (Hashes are reproducible per checkout; the published payload is
                # built on the Linux CI runner, where sources are LF.)
                plan[rel] = ("gz", raw)
                continue
            plan[rel] = ("copy", f)
            if rule["gz"] and f.suffix.lower() in GZ_EXT:
                plan.setdefault(rel + ".gz", ("gz", f))
    errors, dropped = [], dict(heavy=0, state=0, orphan=0, other=0)
    # 2) AKB5: ship exactly the shards the shipped manifest routes to (the sd-sim tree carries extra,
    #    unreferenced shards — ~72 MB nobody reads).
    list_rel = "data/anima/anima-it-akb5.bin"
    wanted = None
    if list_rel in plan:
        try:
            wanted = set(_akb5_shards(plan[list_rel][1]))
        except Exception as e:
            errors.append(f"akb5 manifest unreadable: {e}")
    # 3) filter
    keep = {}
    for rel, entry in sorted(plan.items()):
        n = "/" + rel
        if rel in RELEASE_DROP or rel.endswith(MANIFEST_NAME) or rel.startswith("data/tts/"):
            dropped["other"] += 1; continue
        if any(h in n for h in RELEASE_HEAVY) or rel.lower().endswith(RELEASE_HEAVY_EXT):
            dropped["heavy"] += 1; continue
        if is_state(rel):
            dropped["state"] += 1; continue
        if rel.startswith("data/anima/akb5/") and wanted is not None and rel.split("/")[-1] not in wanted:
            dropped["orphan"] += 1; continue
        why = release_path_allowed(rel)
        if why:
            errors.append(f"path outside the device allow-list ({why}): {rel}")
            continue
        keep[rel] = entry
    if wanted is not None:
        for s in sorted(wanted):
            if f"data/anima/akb5/{s}" not in keep:
                errors.append(f"akb5 shard referenced by the manifest but missing: {s}")
    for rel in RELEASE_REQUIRED:
        if rel not in keep:
            errors.append(f"required file missing: {rel}")
    # 4) write + hash
    lines, packs = [], {}
    total = 0
    for rel, (kind, src) in sorted(keep.items()):
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if kind == "gz":
            gz_file(src, dst)
        else:
            shutil.copyfile(src, dst)
        size = dst.stat().st_size
        pack = _pack_of(rel)
        mode = "m" if rel in RELEASE_MERGE else "w"
        lines.append(f"{sha256(dst)} {size} {pack} {mode} {rel}")
        p = packs.setdefault(pack, [0, 0]); p[0] += 1; p[1] += size
        total += size
    # 5) .gz twins must decompress to their sibling (webfs serves .gz first: a stale twin ships old code)
    for rel in keep:
        if rel.endswith(".gz") and rel[:-3] in keep:
            try:
                with gzip.open(out / rel, "rb") as g:
                    if g.read() != (out / rel[:-3]).read_bytes():
                        errors.append(f"stale .gz twin: {rel}")
            except Exception as e:
                errors.append(f"bad .gz {rel}: {e}")
    head = [f"#nucleoos-sd {RELEASE_MANIFEST_VERSION} {tag} {len(lines)} {total}"]
    head += [f"#pack {k} {v[0]} {v[1]}" for k, v in sorted(packs.items())]
    mp = Path(manifest_path) if manifest_path else out.parent / RELEASE_MANIFEST
    mp.write_text("\n".join(head + lines) + "\n", encoding="ascii", newline="\n")
    stats = dict(files=len(lines), bytes=total, packs=packs, dropped=dropped, manifest=str(mp))
    log(f"release {tag}: {len(lines)} files, {total/2**20:.1f} MB "
        + " ".join(f"[{k} {v[0]} / {v[1]/2**20:.1f} MB]" for k, v in sorted(packs.items()))
        + f"  dropped {dropped}")
    return stats, errors


# ---------------------------------------------------------------- format (optional)
def format_fat32(root, label, log):
    """Format an SD as FAT32 (quick format). Windows only. Returns True on success.
    The safety guards (removable, not the system disk, confirmation) live in the caller."""
    if os.name != "nt":
        log("Formattazione disponibile solo su Windows.")
        return False
    drive = _root(root).rstrip("\\")                 # 'H:'
    label = "".join(c for c in (label or "NUCLEOOS") if c.isalnum())[:11] or "NUCLEOOS"
    cmd = f'format {drive} /FS:FAT32 /Q /V:{label} /Y'
    log(f"$ {cmd}")
    try:
        # 'format' on removable drives asks "Press ENTER when ready": we feed it ENTER via stdin.
        p = subprocess.run(cmd, input="\n\n", capture_output=True, text=True,
                           encoding="utf-8", errors="replace", shell=True)
        for line in (p.stdout or "").splitlines():
            if line.strip():
                log("  " + line.strip())
        if p.returncode != 0:
            for line in (p.stderr or "").splitlines():
                if line.strip():
                    log("  ! " + line.strip())
            log(f"format: codice di uscita {p.returncode} (oltre 32 GB FAT32 viene rifiutato da Windows)")
            return False
        log(f"Formattazione FAT32 completata (etichetta {label}).")
        return True
    except Exception as e:
        log("ERRORE formattazione: " + str(e))
        return False


# ================================================================ GUI
def run_gui():
    import tkinter as tk
    from tkinter import ttk, messagebox, scrolledtext, simpledialog, filedialog

    app = tk.Tk()
    app.title("NucleoOS — SD Deploy")
    app.geometry("780x660")
    app.minsize(720, 580)
    q = queue.Queue()
    busy = {"on": False}
    last_report = {"data": None}

    def log(msg):
        q.put(str(msg))

    def prog_cb(frac, text):           # called by the workers (threads): routes via the queue
        q.put(("prog", frac, text))

    def pump():
        try:
            while True:
                item = q.get_nowait()
                if isinstance(item, tuple) and item and item[0] == "prog":
                    prog["value"] = max(0, min(100, item[1] * 100))
                    prog_lbl.config(text=item[2])
                else:
                    txt.configure(state="normal")
                    txt.insert("end", str(item) + "\n")
                    txt.see("end")
                    txt.configure(state="disabled")
        except queue.Empty:
            pass
        app.after(60, pump)

    # ---- tooltip (Tkinter has no native one): delayed appearance, text via getter (re-localizes)
    class _Tip:
        def __init__(self, widget, getter):
            self.w = widget; self.getter = getter; self.tip = None; self.id = None
            widget.bind("<Enter>", lambda _: self._schedule(), add="+")
            widget.bind("<Leave>", lambda _: self._cancel(), add="+")
            widget.bind("<ButtonPress>", lambda _: self._cancel(), add="+")
        def _schedule(self):
            self._cancel(); self.id = self.w.after(450, self._show)
        def _cancel(self):
            if self.id: self.w.after_cancel(self.id); self.id = None
            if self.tip: self.tip.destroy(); self.tip = None
        def _show(self):
            if self.tip:
                return
            try:
                x = self.w.winfo_rootx() + 18
                y = self.w.winfo_rooty() + self.w.winfo_height() + 6
            except Exception:
                return
            txt = self.getter()
            if not txt:
                return
            self.tip = tw = tk.Toplevel(self.w)
            tw.wm_overrideredirect(True)
            tw.wm_geometry(f"+{x}+{y}")
            tk.Label(tw, text=txt, justify="left", bg="#fffbe6", fg="#222",
                     relief="solid", bd=1, font=("Segoe UI", 9), wraplength=380,
                     padx=9, pady=6).pack()

    def tip(widget, it, en):
        _Tip(widget, lambda: T(it, en))
        return widget

    # ---- static-text registry for language switch
    i18n = []
    def TW(widget, it, en):
        i18n.append((widget, it, en))
        widget.config(text=T(it, en))
        return widget

    def set_lang(lang):
        global LANG
        LANG = lang
        for code, b in lang_btns.items():
            b.config(style="Lang.TButton" if code != lang else "LangSel.TButton")
        for w, it, en in i18n:
            try:
                w.config(text=T(it, en))
            except Exception:
                pass
        on_drive_change()                       # re-localizes the drive status

    # ---- header
    top = ttk.Frame(app, padding=10); top.pack(fill="x")
    ttk.Label(top, text="NucleoOS · SD Deploy", font=("Segoe UI", 15, "bold")).pack(side="left")
    langbar = ttk.Frame(top); langbar.pack(side="right")
    try:
        _stl = ttk.Style()
        _stl.configure("Lang.TButton", font=("Segoe UI", 9))
        _stl.configure("LangSel.TButton", font=("Segoe UI", 9, "bold"))
    except Exception:
        pass
    lang_btns = {}
    for _code in ("it", "en"):
        b = ttk.Button(langbar, text=_code.upper(), width=4,
                       command=lambda c=_code: set_lang(c))
        b.pack(side="left", padx=2)
        lang_btns[_code] = b
        tip(b, "Cambia lingua dell'interfaccia (Italiano / Inglese)",
            "Switch interface language (Italian / English)")
    subtitle_lbl = ttk.Label(top, foreground="#777")
    subtitle_lbl.pack(side="left", padx=10)
    TW(subtitle_lbl, "Provisioning SD (anche vuote) e aggiornamento sicuro del Cardputer.",
       "Provision SD cards (even blank) and safely update the Cardputer.")

    # ---- drive row
    drow = ttk.LabelFrame(app, padding=10); drow.pack(fill="x", padx=10, pady=6)
    TW(drow, "1 · Unità SD di destinazione", "1 · Target SD drive")
    drive_var = tk.StringVar()
    drive_box = ttk.Combobox(drow, textvariable=drive_var, width=58, state="readonly")
    drive_box.grid(row=0, column=0, sticky="w")
    state_lbl = ttk.Label(drow, text="", foreground="#06f"); state_lbl.grid(row=1, column=0, sticky="w", pady=(6, 0))
    drive_meta = {}

    def refresh_drives():
        rows = list_drives()
        items = []
        drive_meta.clear()
        for root, t, label, free, total in rows:
            tag = "★ SD" if t == "Removable" else T("  disco", "  disk")
            txt_i = f"{root}  [{tag}]  {label or T('(senza nome)', '(no name)')}  {free:.1f}/{total:.1f} GB"
            items.append(txt_i)
            drive_meta[txt_i] = (root, t)
        drive_box["values"] = items
        if items and not drive_var.get():
            rem = [i for i in items if "★ SD" in i]
            drive_var.set(rem[0] if rem else items[0])
            on_drive_change()
        log(T(f"Unità trovate: {len(items)}", f"Drives found: {len(items)}"))

    def on_drive_change(*_):
        sel = drive_var.get()
        if sel not in drive_meta:
            return
        root, t = drive_meta[sel]
        det = detect_target(root)
        names = {"blank":    T("VUOTA → provisioning completo", "BLANK → full provisioning"),
                 "nucleoos": T("NucleoOS già presente → consigliato UPDATE",
                               "NucleoOS already present → UPDATE recommended"),
                 "foreign":  T("non-NucleoOS con dati → ATTENZIONE", "non-NucleoOS with data → CAUTION"),
                 "missing":  T("non accessibile", "not accessible")}
        warn = "" if t == "Removable" else T("  ⚠ NON rimovibile (disco fisso!)",
                                             "  ⚠ NOT removable (fixed disk!)")
        state_lbl.config(text=f"{T('Stato', 'Status')}: {names.get(det, det)}{warn}",
                         foreground="#c00" if (t != "Removable" or det == "foreign") else "#06a")
    drive_box.bind("<<ComboboxSelected>>", on_drive_change)
    btn_refresh = ttk.Button(drow, command=lambda: refresh_drives()); btn_refresh.grid(row=0, column=1, padx=8)
    TW(btn_refresh, "↻ Aggiorna", "↻ Refresh")
    btn_fmt = ttk.Button(drow, command=lambda: do_format()); btn_fmt.grid(row=0, column=2)
    TW(btn_fmt, "⚠ Formatta FAT32…", "⚠ Format FAT32…")

    # ---- mode
    mrow = ttk.LabelFrame(app, padding=10); mrow.pack(fill="x", padx=10, pady=6)
    TW(mrow, "2 · Operazione", "2 · Operation")
    mode_var = tk.StringVar(value="update")
    rb_fresh = ttk.Radiobutton(mrow, variable=mode_var, value="fresh"); rb_fresh.pack(anchor="w")
    TW(rb_fresh, "Provisiona (SD vuota: payload completo + template puliti)",
       "Provision (blank SD: full payload + clean templates)")
    rb_update = ttk.Radiobutton(mrow, variable=mode_var, value="update"); rb_update.pack(anchor="w")
    TW(rb_update, "Aggiorna (preserva chiave, card imparate, impostazioni, dati utente)",
       "Update (preserves API key, learned cards, settings, user data)")
    rb_verify = ttk.Radiobutton(mrow, variable=mode_var, value="verify"); rb_verify.pack(anchor="w")
    TW(rb_verify, "Verifica (confronta SD col master, nessuna scrittura)",
       "Verify (compare SD against master, no writes)")
    dry_var = tk.BooleanVar(value=False)
    chk_dry = ttk.Checkbutton(mrow, variable=dry_var); chk_dry.pack(anchor="w", pady=(4, 0))
    TW(chk_dry, "Anteprima (dry-run): mostra cosa farebbe senza scrivere",
       "Dry-run: shows what it would do without writing")

    # ---- actions
    arow = ttk.Frame(app, padding=(10, 0)); arow.pack(fill="x")
    btn_asm = ttk.Button(arow, command=lambda: worker("assemble"))
    btn_run = ttk.Button(arow, command=lambda: worker("run"))
    btn_rep = ttk.Button(arow, command=lambda: save_report_as())
    TW(btn_asm, "Assembla master", "Assemble master")
    TW(btn_run, "▶ Esegui operazione", "▶ Run operation")
    TW(btn_rep, "💾 Salva report…", "💾 Save report…")
    btn_asm.pack(side="left"); btn_run.pack(side="left", padx=8); btn_rep.pack(side="left")
    prog_lbl = ttk.Label(arow, text="", width=18, foreground="#06a"); prog_lbl.pack(side="right")
    prog = ttk.Progressbar(arow, mode="determinate", maximum=100, length=180); prog.pack(side="right", padx=6)

    # ---- log
    lf = ttk.LabelFrame(app, padding=6); lf.pack(fill="both", expand=True, padx=10, pady=8)
    TW(lf, "Log", "Log")
    txt = scrolledtext.ScrolledText(lf, height=16, state="disabled", font=("Consolas", 9))
    txt.pack(fill="both", expand=True)

    def set_busy(on):
        busy["on"] = on
        for b in (btn_asm, btn_run):
            b.config(state="disabled" if on else "normal")
        if on:
            prog["value"] = 0; prog_lbl.config(text=T("avvio…", "starting…"))
        else:
            prog_lbl.config(text=(T("completato", "done") if prog["value"] else ""))

    # ---- report: auto-saved in reports/ + exportable via "Save as"
    def _report_text(d):
        L = ["NucleoOS · SD Deploy — report",
             f"quando    : {d.get('timestamp')}",
             f"operazione: {d.get('operation')}"]
        if d.get("drive"):    L.append(f"unità     : {d['drive']}")
        if "dry" in d:        L.append(f"anteprima : {d['dry']}")
        if d.get("stats"):    L.append("stats     : " + json.dumps(d['stats'], ensure_ascii=False))
        if "ok" in d:
            L.append(f"verify    : ok={d['ok']} mancanti={len(d.get('missing',[]))} diversi={len(d.get('different',[]))}")
            for m in d.get("missing", [])[:80]:   L.append("  MANCA   " + m)
            for x in d.get("different", [])[:80]: L.append("  DIVERSO " + x)
        if d.get("warnings"):
            L.append("avvisi    :")
            for w in d["warnings"]: L.append("  ! " + w)
        L.append(f"durata    : {d.get('duration_s')} s")
        return "\n".join(L) + "\n"

    def build_report(op, **kw):
        d = {"timestamp": time.strftime("%Y-%m-%d %H:%M:%S"), "operation": op, "repo": str(REPO)}
        d.update(kw)
        last_report["data"] = d
        try:
            rdir = HERE / "reports"; rdir.mkdir(exist_ok=True)
            fn = rdir / f"{op}-{time.strftime('%Y%m%d-%H%M%S')}.json"
            fn.write_text(json.dumps(d, indent=2, ensure_ascii=False), encoding="utf-8")
            log(T(f"Report salvato: {fn}", f"Report saved: {fn}"))
        except Exception as e:
            log(T("Report non salvato: ", "Report not saved: ") + str(e))

    def save_report_as():
        if not last_report["data"]:
            messagebox.showinfo("Report", T("Nessun report: esegui prima un'operazione.",
                                            "No report yet: run an operation first.")); return
        d = last_report["data"]
        p = filedialog.asksaveasfilename(
            defaultextension=".json", filetypes=[("JSON", "*.json"), (T("Testo", "Text"), "*.txt")],
            initialfile=f"sd-deploy-{d['operation']}.json")
        if not p:
            return
        try:
            content = _report_text(d) if p.lower().endswith(".txt") \
                else json.dumps(d, indent=2, ensure_ascii=False)
            Path(p).write_text(content, encoding="utf-8")
            log(T("Report esportato: ", "Report exported: ") + p)
        except Exception as e:
            messagebox.showerror("Report", str(e))

    # ---- FAT32 formatting (optional, heavily guarded)
    def do_format():
        if busy["on"]:
            return
        sel = drive_var.get()
        if sel not in drive_meta:
            messagebox.showerror(T("Formatta", "Format"),
                                 T("Seleziona prima un'unità.", "Select a drive first.")); return
        root, t = drive_meta[sel]
        if is_system_drive(root):
            messagebox.showerror("STOP", T(f"{root} è il disco di sistema. Formattazione bloccata.",
                                           f"{root} is the system drive. Format blocked.")); return
        if t != "Removable":
            messagebox.showerror("STOP", T(f"{root} NON è rimovibile. Da qui si formattano SOLO le SD rimovibili.",
                                           f"{root} is NOT removable. Only removable SD cards can be formatted here.")); return
        free, total = _free_total(root)
        if total > 32 and not messagebox.askyesno(T("Dimensione", "Size"),
                T(f"{root} è {total:.0f} GB. Windows rifiuta FAT32 oltre 32 GB e la formattazione fallirà.\nProcedere comunque?",
                  f"{root} is {total:.0f} GB. Windows refuses FAT32 above 32 GB and the format will fail.\nProceed anyway?")):
            return
        letter = root.rstrip(":\\")
        if not messagebox.askyesno(T("FORMATTAZIONE — DISTRUTTIVA", "FORMAT — DESTRUCTIVE"),
                T(f"Verrà CANCELLATO TUTTO su {root}  ({sel}).\nL'operazione è IRREVERSIBILE.\n\nContinuare?",
                  f"EVERYTHING on {root}  ({sel}) will be ERASED.\nThis is IRREVERSIBLE.\n\nContinue?"), icon="warning"):
            return
        typed = simpledialog.askstring(T("Conferma finale", "Final confirmation"),
                T(f"Per confermare, digita la lettera dell'unità da formattare:  {letter}",
                  f"To confirm, type the drive letter to format:  {letter}"), parent=app)
        if not typed or typed.strip().rstrip(":").upper() != letter.upper():
            log(T("Formattazione annullata (conferma non corrispondente).",
                  "Format cancelled (confirmation did not match).")); return

        def job():
            set_busy(True)
            try:
                if format_fat32(root, "NUCLEOOS", log):
                    log(T("— SD formattata in FAT32. Ora puoi 'Provisiona'. —",
                          "— SD formatted to FAT32. You can now 'Provision'. —"))
                    app.after(0, refresh_drives)
            finally:
                app.after(0, lambda: set_busy(False))
        threading.Thread(target=job, daemon=True).start()

    def worker(kind):
        if busy["on"]:
            return
        if kind == "run":
            sel = drive_var.get()
            if sel not in drive_meta:
                messagebox.showerror("SD", T("Seleziona un'unità.", "Select a drive.")); return
            root, t = drive_meta[sel]
            mode = mode_var.get(); dry = dry_var.get()
            if mode != "verify":
                # SAFETY
                if is_system_drive(root):
                    messagebox.showerror("STOP", T(f"{root} è il disco di sistema. Operazione bloccata.",
                                                   f"{root} is the system drive. Operation blocked.")); return
                if t != "Removable":
                    if not messagebox.askyesno(T("Disco NON rimovibile", "NON-removable disk"),
                            T(f"{root} NON è una SD rimovibile (è un disco fisso).\nScrivere qui è rischioso. Continuare comunque?",
                              f"{root} is NOT a removable SD (it's a fixed disk).\nWriting here is risky. Continue anyway?")):
                        return
                det = detect_target(root)
                if mode == "fresh" and det == "nucleoos" and not messagebox.askyesno(
                        T("SD già NucleoOS", "SD already NucleoOS"),
                        T("Questa SD ha già NucleoOS. 'Provisiona' riscrive i template di stato "
                          "(chiave/learned restano comunque preservati dal map device-state).\n"
                          "Per un device in uso conviene UPDATE. Continuare con provisioning?",
                          "This SD already has NucleoOS. 'Provision' rewrites the state templates "
                          "(key/learned are still preserved by the device-state map).\n"
                          "For a device in use, UPDATE is better. Continue with provisioning?")):
                    return
                action = T("ANTEPRIMA", "DRY-RUN") if dry else mode.upper()
                if not messagebox.askyesno(T("Conferma", "Confirm"),
                        T(f"{action} su {root}\n({sel})\n\nProcedere?",
                          f"{action} on {root}\n({sel})\n\nProceed?")):
                    return

        def job():
            set_busy(True)
            t0 = time.time()
            try:
                if kind == "assemble":
                    stats, warns = assemble_master(log, progress=prog_cb)
                    if warns:
                        log("AVVISI:"); [log("  ⚠ " + w) for w in warns]
                    log("— master assemblato —")
                    build_report("assemble", stats=stats, warnings=warns,
                                 duration_s=round(time.time() - t0, 1))
                else:
                    sel = drive_var.get(); root, t = drive_meta[sel]
                    mode = mode_var.get(); dry = dry_var.get()
                    if mode == "verify":
                        missing, diff, ok = verify(root, log, progress=prog_cb)
                        build_report("verify", drive=root, ok=ok, missing=missing,
                                     different=diff, duration_s=round(time.time() - t0, 1))
                    else:
                        st = provision(root, mode, dry, log, progress=prog_cb)
                        build_report(mode, drive=root, dry=dry, stats=st,
                                     duration_s=round(time.time() - t0, 1))
                    log(T(f"— fine ({time.time()-t0:.1f}s) —", f"— done ({time.time()-t0:.1f}s) —"))
            except Exception as e:
                log(T("ERRORE: ", "ERROR: ") + str(e))
            finally:
                app.after(0, lambda: set_busy(False))
        threading.Thread(target=job, daemon=True).start()

    # ---- explanatory (bilingual) tooltips on every control
    tip(drive_box,
        "Unità SD su cui lavorare. Le rimovibili (★ SD) sono preferite e selezionate in automatico. "
        "Mostra lettera, etichetta e spazio libero/totale.",
        "The SD drive to work on. Removable ones (★ SD) are preferred and auto-selected. "
        "Shows letter, label and free/total space.")
    tip(btn_refresh,
        "Ri-scansiona le unità collegate. Usalo dopo aver inserito o tolto una SD.",
        "Re-scan the connected drives. Use it after inserting or removing an SD.")
    tip(btn_fmt,
        "Formatta la SD in FAT32 (rapida). DISTRUTTIVO: cancella TUTTO. Solo unità rimovibili, con doppia "
        "conferma (devi digitare la lettera). Da usare su una SD nuova prima di 'Provisiona'.",
        "Format the SD to FAT32 (quick). DESTRUCTIVE: erases EVERYTHING. Removable drives only, with double "
        "confirmation (you must type the letter). Use it on a new SD before 'Provision'.")
    tip(state_lbl,
        "Diagnosi dell'unità scelta: VUOTA (pronta al provisioning), NucleoOS già presente (meglio Aggiorna) "
        "oppure non-NucleoOS con dati (attenzione, contiene altri file).",
        "Diagnosis of the selected drive: BLANK (ready to provision), NucleoOS already present (prefer Update) "
        "or non-NucleoOS with data (caution, it holds other files).")
    tip(rb_fresh,
        "SD NUOVA: scrive l'intero sistema e crea le cartelle utente + i template puliti (chiave API vuota, "
        "learned vuoto, 13 cartelle dati). Se una chiave esiste già NON viene toccata.",
        "FRESH SD: writes the whole system and creates the user folders + clean templates (empty API key, "
        "empty learned, 13 data folders). An existing key is NOT touched.")
    tip(rb_update,
        "Aggiorna SOLO i file di sistema (app, www, registry, conoscenza ANIMA, voce). PRESERVA chiave, card "
        "imparate, impostazioni e dati utente. Non cancella mai nulla.",
        "Updates ONLY the system files (apps, www, registry, ANIMA knowledge, voice). PRESERVES key, learned "
        "cards, settings and user data. Never deletes anything.")
    tip(rb_verify,
        "Confronta la SD col master file-per-file (hash) e segnala mancanti/diversi. Nessuna scrittura. "
        "Lo stato-device (chiave, learned, ecc.) è escluso dal confronto.",
        "Compares the SD against the master file-by-file (hash) and reports missing/different. No writes. "
        "Device state (key, learned, etc.) is excluded from the comparison.")
    tip(chk_dry,
        "Anteprima (dry-run): simula l'operazione e mostra cosa farebbe, senza scrivere nulla sulla SD.",
        "Dry-run: simulates the operation and shows what it would do, without writing anything to the SD.")
    tip(btn_asm,
        "Assembla in deploy/sd-master/ il 'master' completo da tutte le sorgenti del repo (registry, app, "
        "shell, ANIMA, voce TTS, giochi + .factory, evilportal, wallpaper) e calcola il manifest con gli hash. "
        "Va fatto PRIMA di provisionare o verificare.",
        "Assembles in deploy/sd-master/ the full 'master' from every repo source (registry, apps, shell, "
        "ANIMA, TTS voice, games + .factory, evilportal, wallpapers) and computes the hashed manifest. "
        "Do it BEFORE provisioning or verifying.")
    tip(btn_run,
        "Esegue l'operazione selezionata (Provisiona / Aggiorna / Verifica) sull'unità scelta. Per le "
        "scritture chiede conferma; usa l'Anteprima se non sei sicuro.",
        "Runs the selected operation (Provision / Update / Verify) on the chosen drive. Writes ask for "
        "confirmation; use Dry-run if unsure.")
    tip(btn_rep,
        "Esporta l'ultimo report come JSON o testo leggibile. Ogni operazione ne salva comunque uno in "
        "automatico nella cartella reports/.",
        "Exports the last report as JSON or readable text. Every operation also auto-saves one in the "
        "reports/ folder.")
    tip(prog,
        "Avanzamento reale dell'operazione in corso (copia, manifest, scrittura o verifica).",
        "Real progress of the running operation (copy, manifest, write or verify).")

    set_lang("it")                            # set language styles + apply the texts
    log(T(f"Repo: {REPO}", f"Repo: {REPO}"))
    log(T("1) Assembla master  2) scegli unità + operazione  3) Esegui  (usa Anteprima per sicurezza)",
          "1) Assemble master  2) pick drive + operation  3) Run  (use Dry-run to be safe)"))
    refresh_drives()
    pump()
    if os.environ.get("SDDEPLOY_SMOKE"):     # smoke: build the GUI and close (no interaction)
        seq = os.environ.get("SDDEPLOY_SMOKE")
        if seq == "en":
            app.after(400, lambda: set_lang("en"))
        app.after(900, app.destroy)
    app.mainloop()


# ================================================================ CLI fallback
def main():
    try:                                  # the Windows console (cp1252) chokes on ✓/⚠: force UTF-8
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass
    args = sys.argv[1:]
    if not args or args[0] == "gui":
        run_gui(); return
    log = print
    if args[0] == "assemble":
        _, warns = assemble_master(log)
        for w in warns: log("WARN " + w)
    elif args[0] == "drives":
        for r in list_drives(): log(r)
    elif args[0] in ("provision", "update") and len(args) > 1:
        provision(args[1], "fresh" if args[0] == "provision" else "update",
                  "--dry" in args, log)
    elif args[0] == "verify" and len(args) > 1:
        verify(args[1], log)
    elif args[0] == "release" and len(args) > 1:
        # release <out-dir> [--tag vX.Y.Z] [--manifest <path>]   (headless; exit 1 on any error)
        tag = "v" + (REPO / "firmware" / "version" / "VERSION").read_text(encoding="utf-8").strip()
        man = None
        if "--tag" in args:
            tag = args[args.index("--tag") + 1]
        if "--manifest" in args:
            man = args[args.index("--manifest") + 1]
        try:
            _, errors = build_release(args[1], tag, log, man)
        except RuntimeError as e:
            log("ERROR " + str(e))
            sys.exit(1)
        for e in errors:
            log("ERROR " + e)
        if errors:
            sys.exit(1)
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
