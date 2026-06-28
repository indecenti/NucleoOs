#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NucleoOS — SD Deploy
====================
Sistema di provisioning della SD del Cardputer, con UI Tkinter. Tre operazioni:

  • PROVISION (SD vuova/nuova) : assembla il payload completo da TUTTE le sorgenti
        canoniche e lo scrive su una SD vuota, creando la struttura, le cartelle
        utente e i template di stato puliti (nessuna chiave, nessuna card imparata).
  • UPDATE (SD esistente)      : aggiorna SOLO i file di sistema (app, www, registry,
        conoscenza ANIMA) PRESERVANDO lo stato del device (chiave Groq, card imparate,
        impostazioni, dati utente). Non cancella MAI nulla.
  • VERIFY                     : confronta la SD col master (hash) e segnala mancanti/diversi.

Il "master" viene assemblato in deploy/sd-master/ raccogliendo da:
  registry/  apps/  web/shell/  web/downloads/ (app companion)  tools/sd-sim/data/
  + i pack ANIMA (encoder, index, manifest+shard akb5)  + voce TTS  + evilportal/
  wallpapers/  README — colmando i buchi che la vecchia pipeline (deploy.ps1 / sd-safe)
  lasciava aperti (akb5 fuori pipeline, wallpapers solo-su-SD, downloads). Ogni
  .js/.css/.html (e i portali evilportal) viene gz-ato (il firmware serve i .gz), e i
  manifesti .factory dei giochi di fabbrica vengono (ri)generati dal payload reale.

Solo stdlib (tkinter, ctypes, hashlib, gzip, shutil). Lancio:
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

# ---------------------------------------------------------------- lingua / language
# Bilingue IT/EN. LANG è globale così anche i log del core si traducono; la GUI lo
# commuta con l'interruttore. / Bilingual IT/EN; LANG is global so core logs translate too.
LANG = "it"
def T(it, en):
    return en if LANG == "en" else it

# ---------------------------------------------------------------- source map
# Ogni regola: dest relativo sulla SD <- prima sorgente ESISTENTE tra i candidati.
# kind: 'tree' (cartella ricorsiva) | 'file' (singolo). gz: genera i .gz per js/css/html.
def _src(*cands):
    return [REPO / c for c in cands]

# NB: questa classificazione (SOURCE_MAP = sistema, DEVICE_STATE = stato utente) è la stessa
# che il firmware applica A RUNTIME per impedire la CANCELLAZIONE dei file di sistema dall'SD:
# vedi firmware/components/nucleo_board/include/nucleo_fsprotect.h (nucleo_fs_is_protected), che
# blocca delete/move-away di system/registry, system/web, apps/, www/, della VOCE (clip TTS in
# data/tts/<lang>/ + modelli Vosk sotto apps/) e del cervello ANIMA
# (data/anima/{anima-*,dict-*,commands*,akb5/}) per file-manager, ANIMA, runtime JS e app Files.
# Se aggiungi qui un nuovo albero di sistema, aggiorna anche quel predicato (e viceversa).
SOURCE_MAP = [
    # system + registry (sorgenti repo, freschi)
    dict(dest="system/registry", kind="tree", gz=False, src=_src("registry")),
    # app (sorgenti repo complete: includono tour.js/nlcommand.js che sd-safe perdeva)
    dict(dest="apps",            kind="tree", gz=True,  src=_src("apps")),
    # shell web
    dict(dest="www/shell",       kind="tree", gz=True,  src=_src("web/shell")),
    # app companion scaricabili dallo shell (es. NucleoConnect.exe) — stanno in web/downloads/, non in web/shell/
    dict(dest="www/shell/downloads", kind="tree", gz=False, src=_src("web/downloads")),
    # seed dati utente + base ANIMA (encoder/index/dict/commands) — NON include akb5
    dict(dest="data",            kind="tree", gz=False, src=_src("tools/sd-sim/data")),
    # CONOSCENZA ANIMA akb5 — il buco della vecchia pipeline. 46 shard completi.
    dict(dest="data/anima/akb5",            kind="tree", gz=False,
         src=_src("deploy/sd-safe/data/anima/akb5")),
    dict(dest="data/anima/anima-it-akb5.bin", kind="file", gz=False,
         src=_src("deploy/sd-safe/data/anima/anima-it-akb5.bin", "models/anima-it-akb5.bin")),
    # VOCE che PARLA: banco clip del TTS concatenativo (nucleo_tts), IT+EN. clips.pcm è oversize
    # (ricostruito da oversized-assets/rejoin.mjs); index.bin è committato. Parte di sistema, non
    # stato utente -> sempre scritto, mai cancellato. (La VOCE che ASCOLTA — modelli Vosk a parti
    # split — cavalca la regola 'apps' qui sopra.)
    dict(dest="data/tts",        kind="tree", gz=False, src=_src("deploy/sd-safe/data/tts")),
    # extra solo-payload — i portali captive vanno serviti gz (il firmware preferisce i .gz)
    dict(dest="evilportal",      kind="tree", gz=True,  src=_src("deploy/sd-safe/evilportal")),
    dict(dest="wallpapers",      kind="tree", gz=False, src=_src("deploy/sd-safe/wallpapers")),
    dict(dest="README.md",       kind="file", gz=False, src=_src("deploy/sd-safe/README.md")),
]

# File critici che DEVONO esistere nel master, altrimenti il provisioning è monco.
COMPLETENESS = [
    ("encoder ANIMA",    "data/anima/anima-it-encoder.bin"),
    ("index ANIMA",      "data/anima/anima-it-index.bin"),
    ("manifest akb5",    "data/anima/anima-it-akb5.bin"),
    ("shell index.html", "www/shell/index.html"),
    ("registry apps",    "system/registry/apps.json"),
    ("app companion",    "www/shell/downloads/NucleoConnect.exe"),
    # VOCE (parte integrante del sistema): clip TTS (parla) + modelli Vosk a parti (ascolta).
    # Un clips.pcm mancante = oversize non ricostruito -> 'node oversized-assets/rejoin.mjs'.
    ("voce TTS it (clip)", "data/tts/it/clips.pcm"),
    ("voce TTS en (clip)", "data/tts/en/clips.pcm"),
    ("voce TTS it (idx)",  "data/tts/it/index.bin"),
    ("dettatura Vosk it",  "apps/anima/www/vosk/models/vosk-model-small-it-0.4.tar.gz.000"),
    ("dettatura Vosk en",  "apps/anima/www/vosk/models/vosk-model-small-en-us-0.15.tar.gz.000"),
]
MIN_AKB5_SHARDS = 40

# Manifesti .factory: bloccano la cancellazione dei giochi DI FABBRICA (firmware
# nucleo_fsfactory.h). Generati QUI dal payload reale del master, così il lock
# combacia esattamente con ciò che spedisci (stesse regole di tools/gen-factory-manifests.py).
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
# Path (glob, rel SD root) di STATO DEVICE: su PROVISION si scrive un template pulito;
# su UPDATE si PRESERVANO (mai sovrascritti, mai cancellati).
DEVICE_STATE = [
    "data/anima/teacher.json",      # chiave Groq / config online
    # learned/ NON in blocco: facets.<lang>.jsonl sono SEED firmware-pinned READ-ONLY (devono combaciare
    # con VKL_FACETS_* nel .bin) e DEVONO essere scritti. Proteggi solo i file SCRITTI dal device per nome:
    "data/anima/learned/it.jsonl", "data/anima/learned/en.jsonl",   # cache risposte online
    "data/anima/learned/it.vec", "data/anima/learned/en.vec",       # embedding cache
    "data/anima/learned/mind.*.jsonl",                              # triple KGE runtime (mind_put)
    "data/anima/learned/knowledge.ledger.jsonl", "data/anima/learned/evo/*",   # ledger di evoluzione
    "data/anima/telemetry.ndjson", "data/anima/session.txt", "data/anima/sessions.json",
    "data/anima/workspace.json", "data/anima/*.httptrace",
    "system/config/*",              # impostazioni runtime
    "system/keys/*", "system/sessions/*",
    "config/*", "backups/*", "journal/*",
    "*.vec", "auth.json", "volume.json", "settings.json",
]
# Cartelle utente da creare vuote su una SD nuova.
USER_DIRS = ["data/Music", "data/Videos", "data/Pictures", "data/Documents", "data/Notes",
             "data/Recordings", "data/ROMs", "data/DOS", "data/Transcripts",
             "data/downloads", "data/shared", "data/imports", "data/exports"]
TEACHER_TEMPLATE = {"provider": "groq", "model": "llama-3.3-70b-versatile", "key": ""}

def is_state(rel):
    rel = rel.replace("\\", "/")
    return any(fnmatch.fnmatch(rel, p) or rel.startswith(p.rstrip("*")) and p.endswith("*")
               for p in DEVICE_STATE)

# ---------------------------------------------------------------- helpers
def sha256(path, buf=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(buf), b""):
            h.update(b)
    return h.hexdigest()

def gz_file(src, dst):
    with open(src, "rb") as fi, gzip.open(dst, "wb", compresslevel=9) as fo:
        shutil.copyfileobj(fi, fo)

# ---------------------------------------------------------------- Windows drives
def list_drives():
    """Ritorna [(root, type_str, label, free_gb, total_gb)] per le unità presenti."""
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
        if t not in (2, 3):           # solo rimovibili e fisse (mai network/cd)
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
    """Normalizza un drive-root: 'H:' / 'H:\\' -> 'H:\\'. Senza la barra, Path('H:') è
    drive-RELATIVO (cwd su H:) e ogni join punta nel posto sbagliato."""
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
    # vuota o quasi (solo System Volume Information / metadati FS)
    entries = [e for e in p.iterdir() if e.name not in ("System Volume Information", "$RECYCLE.BIN")]
    return "blank" if not entries else "foreign"

# ---------------------------------------------------------------- assemble master
def assemble_master(log, master=MASTER, progress=None):
    """Raccoglie tutte le sorgenti -> master/, gz, manifest. Ritorna (stats, warnings).
    progress(frac 0..1, testo) opzionale per la barra di avanzamento."""
    master = Path(master)
    stats = dict(copied=0, gz=0, bytes=0, factory=0, akb5=0, files=0)
    warns = []
    log(f"Master: {master}")
    # conteggio sorgenti per la percentuale (la copia è ~80%, il manifest ~20%)
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
    # .factory: blocca i giochi di fabbrica contro la cancellazione (dopo la copia, prima del manifest)
    nfact = write_factory_manifests(master, log)
    stats["factory"] = nfact
    log(f"  .factory: {nfact} giochi pinnati")
    # completezza
    for label, rel in COMPLETENESS:
        if not (master / rel.replace("/", os.sep)).exists():
            warns.append(f"CRITICO mancante: {label} ({rel})")
    akb5 = master / "data" / "anima" / "akb5"
    n_shards = len(list(akb5.glob("*.bin"))) if akb5.exists() else 0
    stats["akb5"] = n_shards
    if n_shards < MIN_AKB5_SHARDS:
        warns.append(f"akb5 incompleto: {n_shards} shard (<{MIN_AKB5_SHARDS})")
    log(f"  akb5 shard: {n_shards}")
    # manifest (con avanzamento sull'hashing, la parte lenta)
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
    """Crea i .factory nei giochi di fabbrica del master (anti-cancellazione).
    Replica tools/gen-factory-manifests.py ma sul payload appena assemblato."""
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
    """mode='fresh' (SD nuova) | 'update' (preserva stato). Ritorna stats."""
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
            # stato device: in update si preserva sempre; in fresh si scrive il template dopo
            st["state_kept"] += 1
            continue
        dst = dst_root / rel.replace("/", os.sep)
        if dst.exists() and dst.stat().st_size == f.stat().st_size and sha256(dst) == sha256(f):
            st["skipped"] += 1
            continue
        if not dry:
            dst.parent.mkdir(parents=True, exist_ok=True)
            tmp = dst.with_suffix(dst.suffix + ".nctmp")
            shutil.copy2(f, tmp)
            os.replace(tmp, dst)
        st["written"] += 1; st["bytes"] += f.stat().st_size
    if mode == "fresh":
        _write_fresh_state(dst_root, dry, log, st)
    # manifest sulla SD
    if not dry:
        shutil.copy2(master / MANIFEST_NAME, dst_root / MANIFEST_NAME)
    log(f"{'[DRY] ' if dry else ''}{mode}: scritti {st['written']}, invariati {st['skipped']}, "
        f"stato-device {'creato' if mode=='fresh' else 'preservato'} {st['state_kept']}, "
        f"{st['bytes']/2**20:.1f} MB")
    return st

def _write_fresh_state(dst_root, dry, log, st):
    """SD nuova: template puliti (chiave vuota, learned vuoto) + cartelle utente."""
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
    """Confronta master vs SD per hash. Ritorna (missing, diff, ok)."""
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


# ---------------------------------------------------------------- format (opzionale)
def format_fat32(root, label, log):
    """Formatta una SD in FAT32 (quick format). Solo Windows. Ritorna True se riuscito.
    Le guardie di sicurezza (rimovibile, non disco di sistema, conferma) stanno nel chiamante."""
    if os.name != "nt":
        log("Formattazione disponibile solo su Windows.")
        return False
    drive = _root(root).rstrip("\\")                 # 'H:'
    label = "".join(c for c in (label or "NUCLEOOS") if c.isalnum())[:11] or "NUCLEOOS"
    cmd = f'format {drive} /FS:FAT32 /Q /V:{label} /Y'
    log(f"$ {cmd}")
    try:
        # 'format' su unità rimovibili chiede "Premi INVIO quando pronto": gli diamo l'INVIO via stdin.
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

    def prog_cb(frac, text):           # chiamata dai worker (thread): instrada via coda
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

    # ---- tooltip (Tkinter non ne ha uno nativo): comparsa ritardata, testo via getter (rilocalizza)
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

    # ---- registro testi statici per il cambio lingua / static-text registry for language switch
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
        on_drive_change()                       # rilocalizza lo stato unità

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

    # ---- report: auto-salvato in reports/ + esportabile "Salva con nome"
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

    # ---- formattazione FAT32 (opzionale, fortemente protetta)
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

    # ---- tooltip esplicativi (bilingue) su ogni controllo
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

    set_lang("it")                            # imposta stili lingua + applica i testi
    log(T(f"Repo: {REPO}", f"Repo: {REPO}"))
    log(T("1) Assembla master  2) scegli unità + operazione  3) Esegui  (usa Anteprima per sicurezza)",
          "1) Assemble master  2) pick drive + operation  3) Run  (use Dry-run to be safe)"))
    refresh_drives()
    pump()
    if os.environ.get("SDDEPLOY_SMOKE"):     # smoke: costruisci la GUI e chiudi (no interazione)
        seq = os.environ.get("SDDEPLOY_SMOKE")
        if seq == "en":
            app.after(400, lambda: set_lang("en"))
        app.after(900, app.destroy)
    app.mainloop()


# ================================================================ CLI fallback
def main():
    try:                                  # la console Windows (cp1252) non digerisce ✓/⚠: forza UTF-8
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
    else:
        print(__doc__)

if __name__ == "__main__":
    main()
