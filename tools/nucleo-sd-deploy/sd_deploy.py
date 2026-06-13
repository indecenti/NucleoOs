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
  registry/  apps/  web/shell/  tools/sd-sim/data/  + i pack ANIMA (encoder, index,
  manifest+shard akb5)  + evilportal/  wallpapers/  README — colmando i buchi che la
  vecchia pipeline (deploy.ps1 / sd-safe) lasciava aperti (akb5 fuori pipeline,
  wallpapers solo-su-SD). Ogni .js/.css/.html viene gz-ato (il firmware serve i .gz).

Solo stdlib (tkinter, ctypes, hashlib, gzip, shutil). Lancio:
    python tools/nucleo-sd-deploy/sd_deploy.py
"""
import os, sys, json, gzip, shutil, hashlib, threading, queue, time, fnmatch, string
from pathlib import Path

# ---------------------------------------------------------------- repo layout
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent                       # tools/nucleo-sd-deploy -> repo root
MASTER = REPO / "deploy" / "sd-master"          # assembled, verifiable payload
MANIFEST_NAME = ".deploy-manifest.json"
GZ_EXT = {".js", ".css", ".html"}

# ---------------------------------------------------------------- source map
# Ogni regola: dest relativo sulla SD <- prima sorgente ESISTENTE tra i candidati.
# kind: 'tree' (cartella ricorsiva) | 'file' (singolo). gz: genera i .gz per js/css/html.
def _src(*cands):
    return [REPO / c for c in cands]

# NB: questa classificazione (SOURCE_MAP = sistema, DEVICE_STATE = stato utente) è la stessa
# che il firmware applica A RUNTIME per impedire la CANCELLAZIONE dei file di sistema dall'SD:
# vedi firmware/components/nucleo_board/include/nucleo_fsprotect.h (nucleo_fs_is_protected), che
# blocca delete/move-away di system/registry, system/web, apps/, www/ e del cervello ANIMA
# (data/anima/{anima-*,dict-*,commands*,akb5/}) per file-manager, ANIMA, runtime JS e app Files.
# Se aggiungi qui un nuovo albero di sistema, aggiorna anche quel predicato (e viceversa).
SOURCE_MAP = [
    # system + registry (sorgenti repo, freschi)
    dict(dest="system/registry", kind="tree", gz=False, src=_src("registry")),
    # app (sorgenti repo complete: includono tour.js/nlcommand.js che sd-safe perdeva)
    dict(dest="apps",            kind="tree", gz=True,  src=_src("apps")),
    # shell web
    dict(dest="www/shell",       kind="tree", gz=True,  src=_src("web/shell")),
    # seed dati utente + base ANIMA (encoder/index/dict/commands) — NON include akb5
    dict(dest="data",            kind="tree", gz=False, src=_src("tools/sd-sim/data")),
    # CONOSCENZA ANIMA akb5 — il buco della vecchia pipeline. 46 shard completi.
    dict(dest="data/anima/akb5",            kind="tree", gz=False,
         src=_src("deploy/sd-safe/data/anima/akb5")),
    dict(dest="data/anima/anima-it-akb5.bin", kind="file", gz=False,
         src=_src("deploy/sd-safe/data/anima/anima-it-akb5.bin", "models/anima-it-akb5.bin")),
    # extra solo-payload
    dict(dest="evilportal",      kind="tree", gz=False, src=_src("deploy/sd-safe/evilportal")),
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
]
MIN_AKB5_SHARDS = 40

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
def assemble_master(log, master=MASTER):
    """Raccoglie tutte le sorgenti -> master/, gz, manifest. Ritorna (stats, warnings)."""
    master = Path(master)
    stats = dict(copied=0, gz=0, bytes=0)
    warns = []
    log(f"Master: {master}")
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
        else:
            for f in sorted(src.rglob("*")):
                if f.is_file():
                    rel = f.relative_to(src)
                    d = dest / rel
                    d.parent.mkdir(parents=True, exist_ok=True)
                    _copy_one(f, d, stats)
                    if rule["gz"] and f.suffix.lower() in GZ_EXT:
                        gz_file(f, str(d) + ".gz"); stats["gz"] += 1
        log(f"  ✓ {rule['dest']:<28} <- {src.relative_to(REPO)}")
    # completezza
    for label, rel in COMPLETENESS:
        if not (master / rel.replace("/", os.sep)).exists():
            warns.append(f"CRITICO mancante: {label} ({rel})")
    akb5 = master / "data" / "anima" / "akb5"
    n_shards = len(list(akb5.glob("*.bin"))) if akb5.exists() else 0
    if n_shards < MIN_AKB5_SHARDS:
        warns.append(f"akb5 incompleto: {n_shards} shard (<{MIN_AKB5_SHARDS})")
    log(f"  akb5 shard: {n_shards}")
    # manifest
    man = {}
    for f in master.rglob("*"):
        if f.is_file() and f.name != MANIFEST_NAME:
            rel = f.relative_to(master).as_posix()
            man[rel] = {"size": f.stat().st_size, "hash": sha256(f)}
    (master / MANIFEST_NAME).write_text(json.dumps(man, indent=1), encoding="utf-8")
    log(f"Master pronto: {len(man)} file, {stats['bytes']/2**20:.1f} MB, {stats['gz']} gz")
    return stats, warns

def _copy_one(src, dst, stats):
    shutil.copy2(src, dst)
    stats["copied"] += 1
    stats["bytes"] += src.stat().st_size

# ---------------------------------------------------------------- deploy ops
def _iter_master(master):
    master = Path(master)
    for f in master.rglob("*"):
        if f.is_file() and f.name != MANIFEST_NAME:
            yield f, f.relative_to(master).as_posix()

def provision(root, mode, dry, log, master=MASTER):
    """mode='fresh' (SD nuova) | 'update' (preserva stato). Ritorna stats."""
    master = Path(master)
    if not (master / MANIFEST_NAME).exists():
        raise RuntimeError("master non assemblato — premi prima 'Assembla master'")
    dst_root = Path(_root(root))
    st = dict(written=0, skipped=0, state_kept=0, bytes=0)
    for f, rel in _iter_master(master):
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
    (anima / "teacher.json").write_text(json.dumps(TEACHER_TEMPLATE, indent=2), encoding="utf-8")
    (anima / "learned").mkdir(exist_ok=True)
    for d in USER_DIRS:
        (dst_root / d.replace("/", os.sep)).mkdir(parents=True, exist_ok=True)
    log(f"Stato device fresco: teacher.json (chiave vuota), learned/ vuoto, {len(USER_DIRS)} cartelle utente")

def verify(root, log, master=MASTER):
    """Confronta master vs SD per hash. Ritorna (missing, diff, ok)."""
    master = Path(master)
    man = json.loads((master / MANIFEST_NAME).read_text(encoding="utf-8"))
    dst_root = Path(_root(root))
    missing, diff, ok = [], [], 0
    for rel, meta in man.items():
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


# ================================================================ GUI
def run_gui():
    import tkinter as tk
    from tkinter import ttk, messagebox, scrolledtext

    app = tk.Tk()
    app.title("NucleoOS — SD Deploy")
    app.geometry("760x620")
    app.minsize(700, 560)
    q = queue.Queue()
    busy = {"on": False}

    def log(msg):
        q.put(str(msg))

    def pump():
        try:
            while True:
                line = q.get_nowait()
                txt.configure(state="normal")
                txt.insert("end", line + "\n")
                txt.see("end")
                txt.configure(state="disabled")
        except queue.Empty:
            pass
        app.after(80, pump)

    # ---- header
    top = ttk.Frame(app, padding=10); top.pack(fill="x")
    ttk.Label(top, text="NucleoOS · SD Deploy", font=("Segoe UI", 15, "bold")).pack(anchor="w")
    ttk.Label(top, text="Provisioning SD (anche vuote) e aggiornamento sicuro del Cardputer.",
              foreground="#555").pack(anchor="w")

    # ---- drive row
    drow = ttk.LabelFrame(app, text="1 · Unità SD di destinazione", padding=10); drow.pack(fill="x", padx=10, pady=6)
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
            tag = "★ SD" if t == "Removable" else "  disco"
            txt_i = f"{root}  [{tag}]  {label or '(senza nome)'}  {free:.1f}/{total:.1f} GB"
            items.append(txt_i)
            drive_meta[txt_i] = (root, t)
        drive_box["values"] = items
        if items and not drive_var.get():
            # preferisci la prima rimovibile
            rem = [i for i in items if "★ SD" in i]
            drive_var.set(rem[0] if rem else items[0])
            on_drive_change()
        log(f"Unità trovate: {len(items)}")

    def on_drive_change(*_):
        sel = drive_var.get()
        if sel not in drive_meta:
            return
        root, t = drive_meta[sel]
        det = detect_target(root)
        names = {"blank": "VUOTA → provisioning completo",
                 "nucleoos": "NucleoOS già presente → consigliato UPDATE",
                 "foreign": "non-NucleoOS con dati → ATTENZIONE",
                 "missing": "non accessibile"}
        warn = "" if t == "Removable" else "  ⚠ NON rimovibile (disco fisso!)"
        state_lbl.config(text=f"Stato: {names.get(det, det)}{warn}",
                         foreground="#c00" if (t != "Removable" or det == "foreign") else "#06a")
    drive_box.bind("<<ComboboxSelected>>", on_drive_change)
    ttk.Button(drow, text="↻ Aggiorna", command=lambda: refresh_drives()).grid(row=0, column=1, padx=8)

    # ---- mode
    mrow = ttk.LabelFrame(app, text="2 · Operazione", padding=10); mrow.pack(fill="x", padx=10, pady=6)
    mode_var = tk.StringVar(value="update")
    ttk.Radiobutton(mrow, text="Provisiona (SD vuota: payload completo + template puliti)",
                    variable=mode_var, value="fresh").pack(anchor="w")
    ttk.Radiobutton(mrow, text="Aggiorna (preserva chiave, card imparate, impostazioni, dati utente)",
                    variable=mode_var, value="update").pack(anchor="w")
    ttk.Radiobutton(mrow, text="Verifica (confronta SD col master, nessuna scrittura)",
                    variable=mode_var, value="verify").pack(anchor="w")
    dry_var = tk.BooleanVar(value=False)
    ttk.Checkbutton(mrow, text="Anteprima (dry-run): mostra cosa farebbe senza scrivere",
                    variable=dry_var).pack(anchor="w", pady=(4, 0))

    # ---- actions
    arow = ttk.Frame(app, padding=(10, 0)); arow.pack(fill="x")
    btn_asm = ttk.Button(arow, text="Assembla master", command=lambda: worker("assemble"))
    btn_run = ttk.Button(arow, text="▶ Esegui operazione", command=lambda: worker("run"))
    btn_asm.pack(side="left"); btn_run.pack(side="left", padx=8)
    prog = ttk.Progressbar(arow, mode="indeterminate", length=180); prog.pack(side="right")

    # ---- log
    lf = ttk.LabelFrame(app, text="Log", padding=6); lf.pack(fill="both", expand=True, padx=10, pady=8)
    txt = scrolledtext.ScrolledText(lf, height=16, state="disabled", font=("Consolas", 9))
    txt.pack(fill="both", expand=True)

    def set_busy(on):
        busy["on"] = on
        for b in (btn_asm, btn_run):
            b.config(state="disabled" if on else "normal")
        prog.start(12) if on else prog.stop()

    def worker(kind):
        if busy["on"]:
            return
        if kind == "run":
            sel = drive_var.get()
            if sel not in drive_meta:
                messagebox.showerror("SD", "Seleziona un'unità."); return
            root, t = drive_meta[sel]
            mode = mode_var.get(); dry = dry_var.get()
            if mode != "verify":
                # SAFETY
                if is_system_drive(root):
                    messagebox.showerror("STOP", f"{root} è il disco di sistema. Operazione bloccata."); return
                if t != "Removable":
                    if not messagebox.askyesno("Disco NON rimovibile",
                            f"{root} NON è una SD rimovibile (è un disco fisso).\n"
                            "Scrivere qui è rischioso. Continuare comunque?"):
                        return
                det = detect_target(root)
                if mode == "fresh" and det == "nucleoos" and not messagebox.askyesno(
                        "SD già NucleoOS", "Questa SD ha già NucleoOS. 'Provisiona' riscrive i template "
                        "di stato (chiave/learned restano comunque preservati dal map device-state).\n"
                        "Per un device in uso conviene UPDATE. Continuare con provisioning?"):
                    return
                action = "ANTEPRIMA" if dry else mode.upper()
                if not messagebox.askyesno("Conferma",
                        f"{action} su {root}\n({sel})\n\nProcedere?"):
                    return

        def job():
            set_busy(True)
            t0 = time.time()
            try:
                if kind == "assemble":
                    _, warns = assemble_master(log)
                    if warns:
                        log("AVVISI:"); [log("  ⚠ " + w) for w in warns]
                    log("— master assemblato —")
                else:
                    sel = drive_var.get(); root, t = drive_meta[sel]
                    mode = mode_var.get(); dry = dry_var.get()
                    if mode == "verify":
                        verify(root, log)
                    else:
                        provision(root, mode, dry, log)
                    log(f"— fine ({time.time()-t0:.1f}s) —")
            except Exception as e:
                log("ERRORE: " + str(e))
            finally:
                app.after(0, lambda: set_busy(False))
        threading.Thread(target=job, daemon=True).start()

    log(f"Repo: {REPO}")
    log("1) Assembla master  2) scegli unità + operazione  3) Esegui  (usa Anteprima per sicurezza)")
    refresh_drives()
    pump()
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
