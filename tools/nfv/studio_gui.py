#!/usr/bin/env python3
"""
NFV Studio GUI — drag-and-drop video converter for the NucleoOS Cardputer (NFV v4).

Any video ffmpeg can read becomes one .nfv v4 file: whole-frame baseline JPEGs + IMA ADPCM sound, normalised
to the same loudness as every other clip (EBU R128, -14 LUFS). The engine is nfv4.py; this is its front end,
with a queue (drop several files), the two screen targets and a live progress bar. Copy the result to the SD
under /data/Videos (or push it with push-resume.mjs).

Deps: ffmpeg on PATH, `pip install pillow numpy`; optional `pip install tkinterdnd2` (drag&drop — without it
the Browse button still works).
"""
from __future__ import annotations
import os, sys, threading, subprocess, traceback, time
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
    _HAS_DND = True
except ImportError:
    _HAS_DND = False

try:
    import nfv4
    _ENGINE_ERR = ""
except Exception as e:                                   # numpy / Pillow missing
    nfv4 = None
    _ENGINE_ERR = str(e)

# (w, h, visible w, visible h): coded sizes are multiples of 16 (4:2:0 MCU)
TARGETS = {
    "Universale 320×240 (schermo esterno + Cardputer)": (320, 240, 320, 240),
    "Solo Cardputer 240×135": (240, 144, 240, 135),
}
VIDEO_EXTS = ("*.mp4 *.avi *.mkv *.mov *.wmv *.flv *.webm *.ts *.m4v *.mpg *.mpeg "
              "*.3gp *.3g2 *.m2ts *.mts *.vob *.ogv *.asf *.mxf *.f4v *.divx *.rm *.rmvb *.gif")

C = dict(
    BG="#1c1c2e", PANEL="#26263a", ENTRY="#1e1e30",
    FG="#e8e8f0", DIM="#7777a0", BORDER="#3a3a54",
    ACCENT="#5a7fff", GREEN="#50c878", RED="#ff5555", YELLOW="#ffd166",
)


class Aborted(Exception):
    pass


def human(n: float) -> str:
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.0f} {u}" if u == "B" else f"{n:.1f} {u}"
        n /= 1024
    return f"{n:.1f} TB"


class NfvStudio:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("NFV Studio — Cardputer Video Converter")
        root.configure(bg=C["BG"])
        root.minsize(640, 640)
        root.geometry("720x720")
        self._abort = threading.Event()
        self._queue: list[str] = []
        self._busy = False
        self._apply_theme()
        self._build()
        if nfv4 is None:
            self._log(f"Motore non disponibile: {_ENGINE_ERR}\n→ pip install pillow numpy", "err")
            self.btn_go.config(state="disabled")

    # ── theme ──────────────────────────────────────────────────────────────────
    def _apply_theme(self):
        s = ttk.Style(); s.theme_use("clam")
        s.configure(".", background=C["BG"], foreground=C["FG"], fieldbackground=C["ENTRY"], font=("Segoe UI", 10))
        for cls in ("TFrame", "TLabel", "TCheckbutton"):
            s.configure(cls, background=C["BG"], foreground=C["FG"])
        s.map("TCheckbutton", background=[("active", C["BG"])])
        s.configure("TLabelframe", background=C["BG"], foreground=C["DIM"], relief="groove", borderwidth=1)
        s.configure("TLabelframe.Label", background=C["BG"], foreground=C["DIM"], font=("Segoe UI", 9, "bold"))
        s.configure("TCombobox", fieldbackground=C["ENTRY"], foreground=C["FG"])
        s.configure("TEntry", fieldbackground=C["ENTRY"], foreground=C["FG"])
        s.configure("TProgressbar", troughcolor=C["PANEL"], background=C["ACCENT"])
        s.configure("TButton", background=C["PANEL"], foreground=C["FG"])
        s.map("TButton", background=[("active", C["BORDER"])])
        s.configure("Accent.TButton", background=C["ACCENT"], foreground="#ffffff", font=("Segoe UI", 11, "bold"))
        s.map("Accent.TButton", background=[("active", "#4060df"), ("disabled", C["PANEL"])],
              foreground=[("disabled", C["DIM"])])
        s.configure("Stop.TButton", background="#993333", foreground="#ffffff", font=("Segoe UI", 10, "bold"))
        s.map("Stop.TButton", background=[("active", "#bb2222"), ("disabled", C["PANEL"])],
              foreground=[("disabled", C["DIM"])])

    # ── build ──────────────────────────────────────────────────────────────────
    def _build(self):
        o = ttk.Frame(self.root, padding=12); o.pack(fill="both", expand=True)

        self.drop = tk.Frame(o, bg=C["PANEL"], relief="ridge", bd=2, cursor="hand2", height=84)
        self.drop.pack(fill="x"); self.drop.pack_propagate(False)
        tip = "⬇   Trascina qui uno o più video" if _HAS_DND else "Clicca per scegliere uno o più video"
        self.lbl_drop = tk.Label(self.drop, text=tip + "\n(MP4 · MKV · AVI · MOV · WebM … tutto ciò che legge ffmpeg)",
                                 bg=C["PANEL"], fg=C["DIM"], font=("Segoe UI", 11), cursor="hand2")
        self.lbl_drop.pack(expand=True)
        for w in (self.drop, self.lbl_drop):
            w.bind("<Button-1>", lambda e: self._browse())
        if _HAS_DND:
            self.drop.drop_target_register(DND_FILES)
            self.drop.dnd_bind("<<Drop>>", self._on_drop)

        f = ttk.LabelFrame(o, text="  Uscita  ", padding=8); f.pack(fill="x", pady=(10, 0))
        self.v_target = tk.StringVar(value=next(iter(TARGETS)))
        self.v_fps = tk.StringVar(value="15")
        self.v_q = tk.StringVar(value="80")
        self.v_fit = tk.StringVar(value="crop")
        self.v_norm = tk.BooleanVar(value=True)
        self.v_audio = tk.BooleanVar(value=True)
        r = ttk.Frame(f); r.pack(fill="x")
        ttk.Label(r, text="Schermo").pack(side="left")
        ttk.Combobox(r, textvariable=self.v_target, values=list(TARGETS), state="readonly", width=44).pack(side="left", padx=6)
        r = ttk.Frame(f); r.pack(fill="x", pady=(6, 0))
        ttk.Label(r, text="FPS").pack(side="left")
        ttk.Combobox(r, textvariable=self.v_fps, values=["10", "12", "15"], state="readonly", width=4).pack(side="left", padx=(6, 14))
        ttk.Label(r, text="Qualità JPEG").pack(side="left")
        ttk.Combobox(r, textvariable=self.v_q, values=["60", "70", "75", "80", "85", "90"], width=4).pack(side="left", padx=(6, 14))
        ttk.Label(r, text="Adatta").pack(side="left")
        ttk.Combobox(r, textvariable=self.v_fit, values=["crop", "pad"], state="readonly", width=5).pack(side="left", padx=6)
        r = ttk.Frame(f); r.pack(fill="x", pady=(6, 0))
        ttk.Checkbutton(r, text="Audio", variable=self.v_audio).pack(side="left")
        ttk.Checkbutton(r, text="Volume normalizzato (-14 LUFS, uguale per tutti i video)", variable=self.v_norm).pack(side="left", padx=12)
        ttk.Label(f, text="Il Cardputer mostra ~15 fps al massimo: di più pesa solo sulla SD. Qualità 80 va bene quasi sempre;\n"
                          "abbassala se le scene molto mosse scattano.", foreground=C["DIM"], font=("Segoe UI", 8)).pack(anchor="w", pady=(6, 0))

        f = ttk.LabelFrame(o, text="  Taglio (opzionale) e cartella  ", padding=8); f.pack(fill="x", pady=(10, 0))
        r = ttk.Frame(f); r.pack(fill="x")
        self.v_ss = tk.StringVar(); self.v_dur = tk.StringVar()
        ttk.Label(r, text="Inizio (s)").pack(side="left"); ttk.Entry(r, textvariable=self.v_ss, width=8).pack(side="left", padx=(6, 14))
        ttk.Label(r, text="Durata (s)").pack(side="left"); ttk.Entry(r, textvariable=self.v_dur, width=8).pack(side="left", padx=6)
        r = ttk.Frame(f); r.pack(fill="x", pady=(6, 0))
        self.v_out = tk.StringVar(value="")
        ttk.Label(r, text="Salva in").pack(side="left")
        ttk.Entry(r, textvariable=self.v_out).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(r, text="…", width=3, command=self._browse_out).pack(side="left")
        ttk.Label(f, text="(vuoto = accanto al video sorgente)", foreground=C["DIM"], font=("Segoe UI", 8)).pack(anchor="w")

        r = ttk.Frame(o); r.pack(fill="x", pady=(12, 0))
        self.btn_go = ttk.Button(r, text="▶  CONVERTI", style="Accent.TButton", command=self._start)
        self.btn_go.pack(side="left", fill="x", expand=True, ipady=4)
        self.btn_stop = ttk.Button(r, text="■ Stop", style="Stop.TButton", command=self._stop, state="disabled")
        self.btn_stop.pack(side="left", padx=(8, 0), ipady=4)
        self.btn_open = ttk.Button(r, text="Apri cartella", command=self._open_dir, state="disabled")
        self.btn_open.pack(side="left", padx=(8, 0), ipady=4)
        self.pb = ttk.Progressbar(o, mode="determinate", maximum=100); self.pb.pack(fill="x", pady=(10, 0))
        self.v_status = tk.StringVar(value="Pronto.")
        ttk.Label(o, textvariable=self.v_status, foreground=C["DIM"]).pack(anchor="w", pady=(2, 0))

        self.log = scrolledtext.ScrolledText(o, height=10, bg=C["ENTRY"], fg=C["FG"], insertbackground=C["FG"],
                                             relief="flat", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, pady=(8, 0))
        self.log.tag_config("ok", foreground=C["GREEN"]); self.log.tag_config("err", foreground=C["RED"])
        self.log.tag_config("dim", foreground=C["DIM"])
        self._last_out = None

    # ── input ──────────────────────────────────────────────────────────────────
    def _on_drop(self, event):
        for p in self.root.tk.splitlist(event.data):
            if os.path.isfile(p):
                self._enqueue(p)

    def _browse(self):
        for p in filedialog.askopenfilenames(title="Scegli i video",
                                             filetypes=[("Video", VIDEO_EXTS), ("Tutti i file", "*.*")]):
            self._enqueue(p)

    def _browse_out(self):
        d = filedialog.askdirectory(title="Cartella di uscita")
        if d:
            self.v_out.set(d)

    def _enqueue(self, p):
        self._queue.append(p)
        self._log(f"+ {os.path.basename(p)}  ({human(os.path.getsize(p))})", "dim")
        self.v_status.set(f"{len(self._queue)} in coda — premi CONVERTI")

    def _open_dir(self):
        if self._last_out:
            d = os.path.dirname(self._last_out)
            if sys.platform == "win32":
                subprocess.Popen(["explorer", "/select,", os.path.normpath(self._last_out)])
            else:
                subprocess.Popen(["xdg-open", d])

    # ── run ────────────────────────────────────────────────────────────────────
    def _log(self, txt, tag=""):
        def do():
            self.log.insert("end", txt + "\n", tag); self.log.see("end")
        self.root.after(0, do)

    def _start(self):
        if self._busy:
            return
        if not self._queue:
            self._browse()
            if not self._queue:
                return
        try:
            q = int(self.v_q.get()); assert 20 <= q <= 95
            ss = float(self.v_ss.get()) if self.v_ss.get().strip() else None
            dur = float(self.v_dur.get()) if self.v_dur.get().strip() else None
        except Exception:
            messagebox.showerror("NFV Studio", "Qualità 20-95; inizio e durata in secondi."); return
        w, h, vw, vh = TARGETS[self.v_target.get()]
        opts = dict(w=w, h=h, vis_w=vw, vis_h=vh, fps=int(self.v_fps.get()), quality=q, fit=self.v_fit.get(),
                    audio=self.v_audio.get(), normalize=self.v_norm.get(), ss=ss, dur=dur)
        self._busy = True; self._abort.clear()
        self.btn_go.config(state="disabled"); self.btn_stop.config(state="normal")
        threading.Thread(target=self._worker, args=(opts,), daemon=True).start()

    def _stop(self):
        self._abort.set()
        self.v_status.set("Interruzione…")

    def _worker(self, opts):
        done = 0
        while self._queue and not self._abort.is_set():
            src = self._queue.pop(0)
            outdir = self.v_out.get().strip() or os.path.dirname(src)
            out = os.path.join(outdir, os.path.splitext(os.path.basename(src))[0] + ".nfv")
            if os.path.abspath(out) == os.path.abspath(src):
                out = os.path.splitext(out)[0] + " (v4).nfv"
            self._log(f"▶ {os.path.basename(src)}")
            t0 = time.time()

            def prog(d, total, name=os.path.basename(src)):
                if self._abort.is_set():
                    raise Aborted()
                if d % 15 == 0:
                    pct = d * 100 / total if total else 0
                    el = time.time() - t0
                    eta = f" · ~{(el / d * (total - d)) / 60:.0f} min" if total and d > 30 else ""
                    self.root.after(0, lambda: (self.pb.config(value=pct),
                                                self.v_status.set(f"{name}: frame {d}/{total or '?'}{eta}")))
            try:
                if opts["audio"]:
                    self.root.after(0, lambda: self.v_status.set("Audio: misura e normalizzazione volume…"))
                st = nfv4.encode_v4(src, out, opts["w"], opts["h"], opts["vis_w"], opts["vis_h"], opts["fps"],
                                    opts["quality"], 24000, opts["fit"], opts["audio"], ss=opts["ss"], dur=opts["dur"],
                                    progress=prog, normalize=opts["normalize"])
                self._last_out = out; done += 1
                self._log(f"  ✓ {os.path.basename(out)} · {st['frames']} frame · {human(st['size'])} · "
                          f"{st['strips_sent']:.1f}/{st['strips']} strisce per frame · {time.time() - t0:.0f} s", "ok")
            except Aborted:
                for p in (out + ".tmp",):
                    try: os.remove(p)
                    except OSError: pass
                self._log("  ■ interrotto", "err")
            except Exception as e:
                self._log(f"  ✗ {e}", "err"); self._log(traceback.format_exc(), "dim")
        self.root.after(0, self._finish, done)

    def _finish(self, done):
        self._busy = False
        self.btn_go.config(state="normal"); self.btn_stop.config(state="disabled")
        self.btn_open.config(state="normal" if self._last_out else "disabled")
        self.pb.config(value=100 if done else 0)
        self.v_status.set(f"Fatto: {done} video. Copiali in /data/Videos sulla SD." if done else "Pronto.")


def main():
    root = TkinterDnD.Tk() if _HAS_DND else tk.Tk()
    NfvStudio(root)
    root.mainloop()


if __name__ == "__main__":
    main()
