#!/usr/bin/env python3
"""
NFV Studio GUI — drag-and-drop video converter for the NucleoOS Cardputer.

Two output formats:
  * NFV v3 (default) — tile-delta MJPEG, ONE self-contained file (.nfv) with audio embedded.
    Only the parts of each frame that change are stored, tables are shared, and the quantization
    is tuned for the 1.14" panel: typically 3-5x smaller than v2 with no visible loss. See nfv3.py.
  * NFV v2 — classic full-frame MJPEG + sibling .mp3. Plays on any NucleoOS build.

Optional deps: `pip install tkinterdnd2` (drag&drop), `pip install pillow numpy` (v3 engine).
"""
from __future__ import annotations
import os, sys, struct, shutil, threading, subprocess, traceback, time, tempfile
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
    _HAS_DND = True
except ImportError:
    _HAS_DND = False

try:
    import nfv3
    from PIL import Image, ImageTk, ImageDraw, ImageEnhance, ImageFont
    _HAS_V3 = True
    _V3_ERR = ""
except Exception as e:                                   # numpy / Pillow missing
    _HAS_V3 = False
    _V3_ERR = str(e)

# ── v2 constants (classic full-frame path) ──────────────────────────────────────
V2_W, V2_H = 240, 136
SOI = b"\xff\xd8"
EOI = b"\xff\xd9"

# Format-aware export profiles. v3 tunes quality(2..100)+denoise+panel sharpness; v2 tunes q(2..31).
# "cardputer" is the recommended default: 20fps, aggressive HF drop (invisible on 1.14"), 16kHz audio.
PROFILES_V3 = {
    "cardputer": dict(fps=20, quality=58, hf=2.4, denoise="light",  ar=16000, ab=24, tile=48),
    "compat":    dict(fps=15, quality=50, hf=2.6, denoise="strong", ar=11025, ab=16, tile=48),
    "balanced":  dict(fps=20, quality=68, hf=1.6, denoise="light",  ar=16000, ab=24, tile=48),
    "quality":   dict(fps=24, quality=80, hf=0.8, denoise="off",    ar=22050, ab=32, tile=16),
}
PROFILES_V2 = {
    "compat":   dict(fps=12, q=8, ar=22050, ab=32),
    "balanced": dict(fps=20, q=7, ar=22050, ab=40),
    "quality":  dict(fps=24, q=5, ar=24000, ab=48),
}
DEFAULT_PROFILE = "cardputer"
SAMPLE_RATES   = ["8000", "11025", "16000", "22050", "24000", "44100"]
AUDIO_BITRATES = ["16", "24", "32", "40", "48", "64", "96"]
# File-picker convenience filter. NOT a hard limit: drag&drop and the "Tutti i file" option accept
# anything, and ffmpeg decodes far more — this list just makes the common containers one click away.
VIDEO_EXTS     = ("*.mp4 *.avi *.mkv *.mov *.wmv *.flv *.webm *.ts *.m4v *.mpg *.mpeg "
                  "*.3gp *.3g2 *.m2ts *.mts *.vob *.ogv *.asf *.mxf *.f4v *.divx *.rm *.rmvb *.gif")

C = dict(
    BG="#1c1c2e", PANEL="#26263a", ENTRY="#1e1e30",
    FG="#e8e8f0", DIM="#7777a0", BORDER="#3a3a54",
    ACCENT="#5a7fff", GREEN="#50c878", RED="#ff5555", YELLOW="#ffd166",
)


def human(n: float) -> str:
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.1f} {u}"
        n /= 1024


# ── v2 helpers (classic full-frame MJPEG -> .nfv + sibling .mp3) ─────────────────

def v2_split_jpegs(blob: bytes) -> list:
    frames, n = [], len(blob)
    start = blob.find(SOI)
    while start != -1:
        nxt = blob.find(SOI, start + 2)
        frame = blob[start:(nxt if nxt != -1 else n)]
        e = frame.rfind(EOI)
        if e != -1:
            frames.append(frame[:e + 2])
        start = nxt
    return frames


def v2_write_nfv(path: str, frames: list, fps: int, has_audio: bool):
    if not frames:
        raise RuntimeError("no frames decoded")
    max_frame = max(len(f) for f in frames)
    fc, dur_ms = len(frames), int(len(frames) * 1000 / fps)
    stride = max(1, round(fps * 3))
    with open(path, "wb") as f:
        f.write(b"\0" * 32)
        offsets = []
        for i, fr in enumerate(frames):
            if i % stride == 0:
                offsets.append(f.tell())
            f.write(struct.pack("<I", len(fr))); f.write(fr)
        idx_off = f.tell()
        f.write(struct.pack("<I", len(offsets)))
        for o in offsets:
            f.write(struct.pack("<I", o))
        flags = (1 if has_audio else 0) | 2
        hdr = struct.pack("<4sBBHHHHIIIIH", b"NFV1", 2, flags, V2_W, V2_H, fps, stride,
                          fc, dur_ms, max_frame, idx_off, 0)
        f.seek(0); f.write(hdr)
    return fc, max_frame, dur_ms


# ── App ─────────────────────────────────────────────────────────────────────────

class NfvStudio:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("NFV Studio — Cardputer Video Converter")
        root.configure(bg=C["BG"])
        root.minsize(700, 860)
        root.geometry("760x940")
        self._abort = threading.Event()
        self._procs = []
        self._applying = False           # True while a profile is populating fields (suppresses custom-mark)
        self._apply_theme()
        self._build()
        threading.Thread(target=self._detect_gpu, daemon=True).start()

    # ── GPU detection ──────────────────────────────────────────────────────────
    def _detect_gpu(self):
        """Background: probe ffmpeg hwaccels for cuda/nvdec; update checkbox & label."""
        has_gpu = False
        ff = shutil.which("ffmpeg")
        if ff:
            try:
                flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
                r = subprocess.run([ff, "-hwaccels"], capture_output=True, timeout=5, **flags)
                out = (r.stdout + r.stderr).lower()
                has_gpu = b"cuda" in out or b"nvdec" in out
            except Exception:
                pass
        def _apply(found=has_gpu):
            self.v_gpu.set(found)
            if found:
                self.lbl_gpu.config(text="  ✅ NVIDIA rilevata", foreground=C["GREEN"])
            else:
                self.lbl_gpu.config(text="  ⚠ NVIDIA non trovata — disattivata", foreground=C["YELLOW"])
        self.root.after(0, _apply)

    # ── theme ──────────────────────────────────────────────────────────────────
    def _apply_theme(self):
        s = ttk.Style(); s.theme_use("clam")
        s.configure(".", background=C["BG"], foreground=C["FG"], fieldbackground=C["ENTRY"],
                    font=("Segoe UI", 10))
        for cls in ("TFrame", "TLabel", "TCheckbutton", "TRadiobutton"):
            s.configure(cls, background=C["BG"], foreground=C["FG"])
        s.map("TCheckbutton", background=[("active", C["BG"])])
        s.map("TRadiobutton", background=[("active", C["BG"])])
        s.configure("TLabelframe", background=C["BG"], foreground=C["DIM"], relief="groove", borderwidth=1)
        s.configure("TLabelframe.Label", background=C["BG"], foreground=C["DIM"], font=("Segoe UI", 9, "bold"))
        s.configure("TCombobox", fieldbackground=C["ENTRY"], foreground=C["FG"])
        s.configure("TEntry", fieldbackground=C["ENTRY"], foreground=C["FG"])
        s.configure("TScale", background=C["BG"], troughcolor=C["PANEL"])
        s.configure("TProgressbar", troughcolor=C["PANEL"], background=C["ACCENT"])
        s.configure("TButton", background=C["PANEL"], foreground=C["FG"])
        s.map("TButton", background=[("active", C["BORDER"])])
        s.configure("Accent.TButton", background=C["ACCENT"], foreground="#ffffff",
                    font=("Segoe UI", 11, "bold"), relief="flat")
        s.map("Accent.TButton", background=[("active", "#4060df"), ("disabled", C["PANEL"])],
              foreground=[("disabled", C["DIM"])])
        s.configure("Stop.TButton", background="#993333", foreground="#ffffff", font=("Segoe UI", 10, "bold"))
        s.map("Stop.TButton", background=[("active", "#bb2222"), ("disabled", C["PANEL"])],
              foreground=[("disabled", C["DIM"])])

    # ── build ──────────────────────────────────────────────────────────────────
    def _build(self):
        outer = ttk.Frame(self.root, padding=12); outer.pack(fill="both", expand=True)
        self._build_drop(outer)
        self._build_input_row(outer)
        self.lbl_probe = ttk.Label(outer, text="", foreground=C["DIM"], font=("Consolas", 8))
        self.lbl_probe.pack(fill="x", pady=(0, 4))
        self._build_format(outer)
        self._build_video(outer)
        self._build_v3opts(outer)
        self._build_audio(outer)
        self._build_trim(outer)
        self._build_output(outer)
        self._build_controls(outer)
        self._build_log(outer)
        self.v_status = tk.StringVar(value="Pronto.")
        tk.Label(self.root, textvariable=self.v_status, bg="#111122", fg=C["DIM"],
                 font=("Segoe UI", 8), anchor="w", padx=8).pack(fill="x", side="bottom")
        self._on_format_change()

    def _build_drop(self, p):
        self.drop_f = tk.Frame(p, bg=C["PANEL"], relief="ridge", bd=2, cursor="hand2", height=90)
        self.drop_f.pack(fill="x"); self.drop_f.pack_propagate(False)
        self.lbl_drop = tk.Label(self.drop_f,
            text=u"⬇   Trascina qui il video   (MP4 · AVI · MKV · MOV · WMV · FLV · WebM)\noppure clicca per sfogliare",
            font=("Segoe UI", 11), bg=C["PANEL"], fg=C["DIM"], justify="center")
        self.lbl_drop.place(relx=.5, rely=.5, anchor="center")
        for w in (self.drop_f, self.lbl_drop):
            w.bind("<Button-1>", lambda _e: self._browse_input())
        dnd_ok = False
        if _HAS_DND:
            try:                                          # needs a TkinterDnD root; degrade if absent
                self.drop_f.drop_target_register(DND_FILES)
                self.drop_f.dnd_bind("<<Drop>>", self._on_drop)
                dnd_ok = True
            except Exception:
                dnd_ok = False
        if not dnd_ok:
            tk.Label(self.drop_f, text="(pip install tkinterdnd2 per il drag&drop)",
                     bg=C["PANEL"], fg=C["RED"], font=("Segoe UI", 7)).pack(side="bottom", pady=2)

    def _build_input_row(self, p):
        r = ttk.Frame(p); r.pack(fill="x", pady=(6, 2))
        ttk.Label(r, text="Input:", width=8).pack(side="left")
        self.v_input = tk.StringVar(); self.v_input.trace_add("write", self._on_input_change)
        ttk.Entry(r, textvariable=self.v_input).pack(side="left", fill="x", expand=True, padx=4)
        ttk.Button(r, text="Sfoglia…", command=self._browse_input).pack(side="left")

    def _build_format(self, p):
        fr = ttk.LabelFrame(p, text="FORMATO", padding=(10, 6)); fr.pack(fill="x", pady=(0, 4))
        self.v_format = tk.StringVar(value="v3" if _HAS_V3 else "v2")
        self.v_format.trace_add("write", lambda *_: self._on_format_change())
        rb3 = ttk.Radiobutton(fr, text="NFV v3 — tile-delta, file unico con audio (consigliato, 3-5× più piccolo)",
                              variable=self.v_format, value="v3")
        rb3.pack(anchor="w")
        if not _HAS_V3:
            rb3.config(state="disabled")
            ttk.Label(fr, text=f"   v3 non disponibile: {_V3_ERR}.  pip install pillow numpy",
                      foreground=C["RED"], font=("Segoe UI", 8)).pack(anchor="w")
        ttk.Radiobutton(fr, text="NFV v2 — classico full-frame + .mp3 separato (massima compatibilità)",
                        variable=self.v_format, value="v2").pack(anchor="w")

    def _build_video(self, p):
        vf = ttk.LabelFrame(p, text="VIDEO", padding=(10, 6)); vf.pack(fill="x", pady=(0, 4))
        vf.columnconfigure(3, weight=1)
        ttk.Label(vf, text="Profilo:").grid(row=0, column=0, sticky="w")
        self.v_profile = tk.StringVar(value=DEFAULT_PROFILE)
        ttk.Combobox(vf, textvariable=self.v_profile, values=list(PROFILES_V3.keys()) + ["custom"],
                     state="readonly", width=12).grid(row=0, column=1, sticky="w", padx=(4, 0))
        self.v_profile.trace_add("write", self._sync_profile)
        self.lbl_prof = ttk.Label(vf, text="", foreground=C["DIM"], font=("Segoe UI", 8))
        self.lbl_prof.grid(row=0, column=2, columnspan=2, sticky="w")

        ttk.Label(vf, text="FPS:").grid(row=1, column=0, sticky="w", pady=(6, 2))
        fr = ttk.Frame(vf); fr.grid(row=1, column=1, columnspan=3, sticky="w", padx=(4, 0))
        self.v_fps = tk.IntVar(value=20)
        for val in (12, 15, 20, 24, 30):
            ttk.Radiobutton(fr, text=str(val), variable=self.v_fps, value=val,
                            command=self._mark_custom).pack(side="left")
        ttk.Label(fr, text=" custom:").pack(side="left")
        self.v_fps_txt = tk.StringVar(); self.v_fps_txt.trace_add("write", lambda *_: self._mark_custom())
        ttk.Entry(fr, textvariable=self.v_fps_txt, width=4).pack(side="left")

        ttk.Label(vf, text="Qualità:").grid(row=2, column=0, sticky="w", pady=(4, 2))
        qr = ttk.Frame(vf); qr.grid(row=2, column=1, columnspan=3, sticky="ew", padx=(4, 0))
        self.v_q = tk.IntVar(value=70)
        self.q_scale = ttk.Scale(qr, from_=20, to=95, orient="horizontal", variable=self.v_q,
                                 command=self._on_q_change)
        self.q_scale.pack(side="left", fill="x", expand=True)
        self.lbl_q = ttk.Label(qr, text="", width=34, font=("Segoe UI", 8)); self.lbl_q.pack(side="left", padx=(6, 0))

        ttk.Label(vf, text="Adattam.:").grid(row=3, column=0, sticky="w", pady=(4, 2))
        fr2 = ttk.Frame(vf); fr2.grid(row=3, column=1, columnspan=3, sticky="w", padx=(4, 0))
        self.v_fit = tk.StringVar(value="crop")
        ttk.Radiobutton(fr2, text="Crop — riempi schermo", variable=self.v_fit, value="crop").pack(side="left")
        ttk.Radiobutton(fr2, text="Pad — letterbox", variable=self.v_fit, value="pad").pack(side="left", padx=(16, 0))

        self.v_gpu = tk.BooleanVar(value=True)   # will be corrected after detection
        gpu_row = ttk.Frame(vf); gpu_row.grid(row=4, column=0, columnspan=4, sticky="w", pady=(6, 0))
        ttk.Checkbutton(gpu_row, text="GPU NVIDIA (NVDEC) — decodifica H.264/HEVC più in fretta",
                        variable=self.v_gpu).pack(side="left")
        self.lbl_gpu = ttk.Label(gpu_row, text="  ⏳ verifica GPU…", foreground=C["DIM"],
                                 font=("Segoe UI", 8))
        self.lbl_gpu.pack(side="left")

    def _build_v3opts(self, p):
        self.v3f = ttk.LabelFrame(p, text="OPZIONI V3 (tile-delta)", padding=(10, 6))
        self.v3f.pack(fill="x", pady=(0, 4))
        r = ttk.Frame(self.v3f); r.pack(fill="x")
        ttk.Label(r, text="Denoise:").pack(side="left")
        self.v_denoise = tk.StringVar(value="light")
        for lab, val in (("Off", "off"), ("Leggero", "light"), ("Forte", "strong")):
            ttk.Radiobutton(r, text=lab, variable=self.v_denoise, value=val,
                            command=self._mark_custom).pack(side="left")
        ttk.Label(r, text="   Griglia:").pack(side="left")
        self.v_tile = tk.IntVar(value=48)
        ttk.Radiobutton(r, text="Media 48", variable=self.v_tile, value=48,
                        command=self._mark_custom).pack(side="left")
        ttk.Radiobutton(r, text="Fine 16", variable=self.v_tile, value=16,
                        command=self._mark_custom).pack(side="left")
        r2 = ttk.Frame(self.v3f); r2.pack(fill="x", pady=(4, 0))
        ttk.Label(r2, text="Nitidezza pannello:").pack(side="left")
        self.v_hf = tk.DoubleVar(value=1.4)
        ttk.Scale(r2, from_=0.0, to=3.0, orient="horizontal", variable=self.v_hf,
                  command=lambda _e: self._mark_custom()).pack(side="left", fill="x", expand=True, padx=(4, 6))
        ttk.Label(r2, text="↑ = più compressione (taglia il dettaglio che il mini-schermo non mostra)",
                  foreground=C["DIM"], font=("Segoe UI", 8)).pack(side="left")
        self.v_grayscale = tk.BooleanVar(value=False)
        ttk.Checkbutton(self.v3f, text="Bianco e nero (sorgenti monocromatiche → file più piccolo)",
                        variable=self.v_grayscale).pack(anchor="w", pady=(4, 0))

    def _build_audio(self, p):
        af = ttk.LabelFrame(p, text="AUDIO", padding=(10, 6)); af.pack(fill="x", pady=(0, 4))
        self.v_audio = tk.BooleanVar(value=True)
        ttk.Checkbutton(af, text="Includi audio (mono — il Cardputer ha un solo speaker)",
                        variable=self.v_audio, command=self._toggle_audio).pack(anchor="w")
        self.audio_row = ttk.Frame(af); self.audio_row.pack(fill="x", pady=(4, 0))
        ttk.Label(self.audio_row, text="Sample rate:").pack(side="left")
        self.v_ar = tk.StringVar(value="16000")
        ttk.Combobox(self.audio_row, textvariable=self.v_ar, values=SAMPLE_RATES,
                     state="readonly", width=8).pack(side="left", padx=(4, 0))
        ttk.Label(self.audio_row, text=" Hz   Bitrate:").pack(side="left")
        self.v_ab = tk.StringVar(value="24")
        ttk.Combobox(self.audio_row, textvariable=self.v_ab, values=AUDIO_BITRATES,
                     state="readonly", width=5).pack(side="left", padx=(4, 0))
        ttk.Label(self.audio_row, text="kbps").pack(side="left")
        ttk.Label(af, text="Lo speaker NS4168 rende fino a ~6 kHz: 16000 Hz/24k suona come 22050 Hz/40k ma pesa ~40% in meno.",
                  foreground=C["DIM"], font=("Segoe UI", 8)).pack(anchor="w", pady=(2, 0))

    def _build_trim(self, p):
        tf = ttk.LabelFrame(p, text="TAGLIA  (opzionale)", padding=(10, 6)); tf.pack(fill="x", pady=(0, 4))
        ttk.Label(tf, text="Inizio:").grid(row=0, column=0, sticky="w")
        self.v_ss = tk.StringVar(); ttk.Entry(tf, textvariable=self.v_ss, width=12).grid(row=0, column=1, padx=(4, 0))
        ttk.Label(tf, text="sec o HH:MM:SS", foreground=C["DIM"], font=("Segoe UI", 8)).grid(row=0, column=2, padx=(4, 16))
        ttk.Label(tf, text="Durata:").grid(row=0, column=3, sticky="w")
        self.v_dur = tk.StringVar(); ttk.Entry(tf, textvariable=self.v_dur, width=12).grid(row=0, column=4, padx=(4, 0))
        ttk.Label(tf, text="sec o HH:MM:SS", foreground=C["DIM"], font=("Segoe UI", 8)).grid(row=0, column=5, sticky="w", padx=(4, 0))

    def _build_output(self, p):
        of = ttk.LabelFrame(p, text="OUTPUT", padding=(10, 6)); of.pack(fill="x", pady=(0, 4))
        r1 = ttk.Frame(of); r1.pack(fill="x")
        ttk.Label(r1, text="Cartella:", width=9).pack(side="left")
        self.v_outdir = tk.StringVar()
        ttk.Entry(r1, textvariable=self.v_outdir).pack(side="left", fill="x", expand=True, padx=4)
        ttk.Button(r1, text="Sfoglia…", command=self._browse_outdir).pack(side="left")
        r2 = ttk.Frame(of); r2.pack(fill="x", pady=(4, 0))
        ttk.Label(r2, text="Nome:", width=9).pack(side="left")
        self.v_stem = tk.StringVar()
        ttk.Entry(r2, textvariable=self.v_stem, width=30).pack(side="left", padx=4)
        self.lbl_out = ttk.Label(r2, text="", foreground=C["DIM"], font=("Segoe UI", 8)); self.lbl_out.pack(side="left")
        ttk.Label(of, text="(vuoti = stessa cartella e nome del file input)",
                  foreground=C["DIM"], font=("Segoe UI", 8)).pack(anchor="w")

    def _build_controls(self, p):
        row = ttk.Frame(p); row.pack(fill="x", pady=(6, 4))
        self.btn_go = ttk.Button(row, text="⚡  CONVERTI", style="Accent.TButton", command=self._start)
        self.btn_go.pack(side="left", ipadx=24, ipady=8)
        self.btn_stop = ttk.Button(row, text="■  Stop", style="Stop.TButton", command=self._stop, state="disabled")
        self.btn_stop.pack(side="left", padx=(8, 0), ipadx=10, ipady=8)
        self.btn_prev = ttk.Button(row, text="▶  Anteprima", command=self._open_preview)
        self.btn_prev.pack(side="left", padx=(8, 0), ipadx=10, ipady=8)
        if not _HAS_V3:
            self.btn_prev.config(state="disabled")
        ttk.Button(row, text="↺  Preset Cardputer", command=self._reset_preset).pack(
            side="right", ipadx=8, ipady=8)
        self.pbar = ttk.Progressbar(p, mode="indeterminate"); self.pbar.pack(fill="x", pady=(0, 4))
        self._last_output = None

    def _build_log(self, p):
        lf = ttk.LabelFrame(p, text="LOG", padding=(4, 4)); lf.pack(fill="both", expand=True)
        self.log = scrolledtext.ScrolledText(lf, height=9, bg=C["PANEL"], fg=C["FG"],
            font=("Consolas", 9), relief="flat", state="disabled", selectbackground=C["ACCENT"])
        self.log.pack(fill="both", expand=True)
        for tag, col in (("dim", C["DIM"]), ("accent", C["ACCENT"]), ("green", C["GREEN"]),
                         ("red", C["RED"]), ("yellow", C["YELLOW"])):
            self.log.tag_configure(tag, foreground=col)

    # ── callbacks ────────────────────────────────────────────────────────────────
    def _on_drop(self, event):
        raw = event.data.strip()
        if raw.startswith("{") and raw.endswith("}"):
            path = raw[1:-1]
        elif " " in raw and not os.path.isfile(raw):
            path = raw.split()[0].strip("{}")
        else:
            path = raw
        self.v_input.set(path)

    def _on_input_change(self, *_):
        path = self.v_input.get().strip()
        if os.path.isfile(path):
            self.drop_f.config(bg="#1a2f20")
            self.lbl_drop.config(bg="#1a2f20", fg=C["GREEN"], text=u"✅  " + os.path.basename(path))
            if not self.v_stem.get():
                self.v_stem.set(os.path.splitext(os.path.basename(path))[0])
            threading.Thread(target=self._probe, args=(path,), daemon=True).start()
        else:
            self.drop_f.config(bg=C["PANEL"])
            self.lbl_drop.config(bg=C["PANEL"], fg=C["DIM"],
                text=u"⬇   Trascina qui il video   (MP4 · AVI · MKV · MOV · WMV · FLV · WebM)\noppure clicca per sfogliare")
            self.lbl_probe.config(text="")

    def _probe(self, path):
        probe = shutil.which("ffprobe")
        if not probe:
            return
        flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
        try:
            import json
            r = subprocess.run([probe, "-v", "quiet", "-print_format", "json",
                                "-show_streams", "-show_format", path], capture_output=True, text=True, **flags)
            d = json.loads(r.stdout)
            vs = next((s for s in d.get("streams", []) if s.get("codec_type") == "video"), None)
            fmt = d.get("format", {}); parts = []
            if vs:
                parts.append(f"{vs.get('width','?')}×{vs.get('height','?')}")
                fr = vs.get("r_frame_rate", "")
                if "/" in fr:
                    a, b = fr.split("/")
                    if int(b): parts.append(f"{int(a)/int(b):.2f} fps")
                if vs.get("codec_name"): parts.append(vs["codec_name"].upper())
            dur = float(fmt.get("duration", 0) or 0)
            if dur:
                m, s = divmod(int(dur), 60); h, m = divmod(m, 60)
                parts.append(f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}")
            if int(fmt.get("size", 0) or 0): parts.append(human(int(fmt["size"])))
            text = "   " + "  ·  ".join(parts) if parts else ""
            self.root.after(0, lambda t=text: self.lbl_probe.config(text=t))
        except Exception:
            pass

    def _browse_input(self):
        p = filedialog.askopenfilename(title="Seleziona video",
                                       filetypes=[("Video", VIDEO_EXTS), ("Tutti i file", "*.*")])
        if p: self.v_input.set(p)

    def _browse_outdir(self):
        d = filedialog.askdirectory(title="Cartella output")
        if d: self.v_outdir.set(d)

    def _is_v3(self):
        return self.v_format.get() == "v3"

    def _on_format_change(self):
        v3 = self._is_v3()
        if v3:
            anchor = self._find_after()
            if anchor is not None:
                self.v3f.pack(fill="x", pady=(0, 4), after=anchor)
            else:
                self.v3f.pack(fill="x", pady=(0, 4))
            self.q_scale.config(from_=20, to=95)
            if self.v_q.get() < 20 or self.v_q.get() > 95: self.v_q.set(70)
            self.lbl_out.config(text="→ <nome>.nfv  (audio incluso)")
        else:
            self.v3f.pack_forget()
            self.q_scale.config(from_=2, to=31)
            if self.v_q.get() > 31: self.v_q.set(7)
            self.lbl_out.config(text="→ <nome>.nfv + <nome>.mp3")
        self._sync_profile()
        self._on_q_change()

    def _find_after(self):
        # v3 options sit right under the VIDEO frame; pack(after=) needs that sibling widget.
        for w in self.root.winfo_children()[0].winfo_children():
            if isinstance(w, ttk.LabelFrame) and w.cget("text") == "VIDEO":
                return w
        return None

    def _sync_profile(self, *_):
        name = self.v_profile.get()
        if name == "custom":
            self.lbl_prof.config(text="impostazioni personalizzate")
            return
        # Populate fields with _applying set, so the widgets' own change callbacks don't flip the
        # profile back to "custom" (and so the hf-slider command can't re-enter this method).
        self._applying = True
        try:
            if self._is_v3():
                c = PROFILES_V3[name]
                self.v_fps.set(c["fps"]); self.v_fps_txt.set("")
                self.v_q.set(c["quality"]); self.v_hf.set(c["hf"]); self.v_denoise.set(c["denoise"])
                self.v_tile.set(c["tile"]); self.v_ar.set(str(c["ar"])); self.v_ab.set(str(c["ab"]))
                self.lbl_prof.config(text=f"q{c['quality']} · denoise {c['denoise']} · {c['ar']}Hz/{c['ab']}k")
            else:
                c = PROFILES_V2[name]
                self.v_fps.set(c["fps"]); self.v_fps_txt.set("")
                self.v_q.set(c["q"]); self.v_ar.set(str(c["ar"])); self.v_ab.set(str(c["ab"]))
                self.lbl_prof.config(text=f"q{c['q']} (MJPEG) · {c['ar']}Hz/{c['ab']}k")
        finally:
            self._applying = False
        self._on_q_change()

    def _reset_preset(self):
        self.v_format.set("v3" if _HAS_V3 else "v2")
        self.v_fit.set("crop")
        self.v_grayscale.set(False)
        self.v_profile.set(DEFAULT_PROFILE)   # triggers _sync_profile

    def _mark_custom(self, *_):
        if self._applying:               # ignore field changes we made ourselves while loading a profile
            return
        self.v_profile.set("custom")

    def _on_q_change(self, *_):
        v = int(float(self.v_q.get()))
        if self._is_v3():
            desc = ("massima qualità" if v >= 85 else "alta qualità" if v >= 72 else
                    "buona qualità" if v >= 55 else "compressa")
            self.lbl_q.config(text=f"qualità={v}  ({desc})")
        else:
            desc = ("ottima, file grandi" if v <= 4 else "buona" if v <= 7 else
                    "ridotta" if v <= 12 else "bassa, file piccoli")
            self.lbl_q.config(text=f"q={v}  ({desc})  [2=best..31=worst]")

    def _toggle_audio(self):
        st = "normal" if self.v_audio.get() else "disabled"
        for w in self.audio_row.winfo_children():
            try: w.config(state=st)
            except Exception: pass

    # ── resolve + run ──────────────────────────────────────────────────────────
    def _resolve(self):
        src = self.v_input.get().strip()
        if not os.path.isfile(src):
            raise ValueError("File input non trovato.")
        fps_t = self.v_fps_txt.get().strip()
        if fps_t:
            try: fps = int(fps_t)
            except ValueError: raise ValueError(f"FPS custom non valido: {fps_t!r}")
            if not (1 <= fps <= 60): raise ValueError("FPS custom fuori range (1–60).")
        else:
            fps = self.v_fps.get()
        outdir = self.v_outdir.get().strip() or os.path.dirname(os.path.abspath(src))
        stem = self.v_stem.get().strip() or os.path.splitext(os.path.basename(src))[0]
        return dict(
            src=src, fps=fps, q=int(float(self.v_q.get())), fit=self.v_fit.get(),
            gpu=self.v_gpu.get(), has_audio=self.v_audio.get(),
            ar=int(self.v_ar.get()), ab=int(self.v_ab.get()),
            ss=self.v_ss.get().strip() or None, dur=self.v_dur.get().strip() or None,
            denoise=self.v_denoise.get(), tile=self.v_tile.get(),
            hf=float(self.v_hf.get()), grayscale=self.v_grayscale.get(),
            out_nfv=os.path.join(outdir, stem + ".nfv"),
            out_mp3=os.path.join(outdir, stem + ".mp3"),
            fmt=self.v_format.get(),
        )

    def _log(self, txt, tag=""):
        def _do():
            self.log.config(state="normal"); self.log.insert("end", txt + "\n", tag)
            self.log.see("end"); self.log.config(state="disabled")
        self.root.after(0, _do)

    def _status(self, txt):
        self.root.after(0, lambda t=txt: self.v_status.set(t))

    def _start(self):
        try:
            o = self._resolve()
        except ValueError as e:
            messagebox.showerror("Errore input", str(e)); return
        self._abort.clear(); self._procs = []
        self.btn_go.config(state="disabled"); self.btn_stop.config(state="normal")
        self.log.config(state="normal"); self.log.delete("1.0", "end"); self.log.config(state="disabled")
        self.pbar.config(mode="indeterminate"); self.pbar.start(12)
        threading.Thread(target=self._encode_thread, args=(o,), daemon=True).start()

    def _stop(self):
        self._abort.set()
        for pr in self._procs:
            try: pr.kill()
            except Exception: pass
        self._log("⚠ Interruzione richiesta.", "yellow")

    def _open_preview(self):
        if not _HAS_V3:
            messagebox.showerror("Anteprima", f"Serve Pillow/numpy: {_V3_ERR}"); return
        path = self._last_output if (self._last_output and os.path.isfile(self._last_output)) else None
        if not path:
            path = filedialog.askopenfilename(title="Apri un .nfv da vedere",
                                              filetypes=[("Clip NFV", "*.nfv"), ("Tutti", "*.*")])
        if not path or not os.path.isfile(path):
            return
        try:
            PreviewPlayer(self.root, path)
        except Exception as e:
            self._log(traceback.format_exc(), "red")
            messagebox.showerror("Anteprima", str(e))

    def _finish(self, ok, msg):
        def _do():
            self.pbar.stop(); self.pbar.config(mode="indeterminate")
            self.btn_go.config(state="normal"); self.btn_stop.config(state="disabled")
            self._log(msg, "green" if ok else "red")
            self.v_status.set(("✅ " if ok else "❌ ") + msg.split("\n")[0])
        self.root.after(0, _do)

    def _set_progress(self, done, total):
        def _do():
            if total and total > 0:
                if str(self.pbar.cget("mode")) != "determinate":
                    self.pbar.stop(); self.pbar.config(mode="determinate", maximum=total)
                self.pbar["value"] = done
                self.v_status.set(f"Frame {done}/{total}  ({done*100//total}%)")
            else:
                self.v_status.set(f"Frame {done}…")
        self.root.after(0, _do)

    def _encode_thread(self, o):
        try:
            if not shutil.which("ffmpeg"):
                raise RuntimeError("ffmpeg non trovato nel PATH.")
            if o["fmt"] == "v3":
                self._encode_v3(o)
            else:
                self._encode_v2(o)
        except InterruptedError as e:
            self._finish(False, str(e) or "Interrotto.")
        except Exception as e:
            self._log(traceback.format_exc(), "red")
            self._finish(False, f"Errore: {e}")

    # ── v3 path ──────────────────────────────────────────────────────────────────
    def _encode_v3(self, o):
        if not _HAS_V3:
            raise RuntimeError(f"Engine v3 non disponibile: {_V3_ERR}")
        self._log(u"╔══ NFV v3 (tile-delta) ══════════════╗", "accent")
        self._log(f"  {os.path.basename(o['src'])}", "accent")
        self._log(f"  fps={o['fps']} q={o['q']} hf={o['hf']:.1f} denoise={o['denoise']} "
                  f"tile={o['tile']} gray={o['grayscale']} fit={o['fit']} gpu={o['gpu']}", "dim")
        self._log(f"  audio={'on '+str(o['ar'])+'Hz/'+str(o['ab'])+'k' if o['has_audio'] else 'off'} (embedded)", "dim")
        opts = nfv3.V3Opts(
            fps=o["fps"], quality=o["q"], hf_boost=o["hf"], fit=o["fit"], denoise=o["denoise"],
            grayscale=o["grayscale"], tile=o["tile"], gpu=o["gpu"], audio=o["has_audio"],
            audio_rate=o["ar"], audio_kbps=o["ab"], ss=o["ss"], dur=o["dur"])
        st = nfv3.encode_v3(o["src"], o["out_nfv"], opts,
                            progress=self._set_progress, abort=self._abort.is_set, log=self._log)
        warn = ""
        # device JPEG buffer = template + max tile + 2; keep it comfortably within the heap budget
        if st["max_tile"] + st["template_len"] > 24000:
            warn = f"\n  ⚠ buffer tile ~{human(st['max_tile']+st['template_len'])} elevato — alza la nitidezza pannello o abbassa q"
        msg = (f"Done — {st['frames']} frame · {st['duration_ms']/1000:.1f}s · "
               f"tile {st['tile']}px ({st['cols']}×{st['rows']})\n"
               f"  📹 {os.path.basename(o['out_nfv'])}  {human(st['size'])}  "
               f"(audio {human(st['audio_len'])} incluso)\n"
               f"  tile statici saltati: {st['static_ratio']*100:.1f}%  ·  keyframe: {st['keyframes']}  ·  "
               f"buffer device: {human(st['max_tile']+st['template_len'])}{warn}")
        self._last_output = o["out_nfv"]
        self._finish(True, msg)

    # ── v2 path (classic) ──────────────────────────────────────────────────────
    def _run_ff(self, cmd, binary_stdout=False):
        flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
        self._log("$ " + " ".join(str(x) for x in cmd), "dim")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **flags)
        self._procs.append(proc)
        if binary_stdout:
            lines = []
            def _drain():
                for raw in proc.stderr:
                    lines.append(1); self._log(raw.decode("utf-8", "replace").rstrip())
            t = threading.Thread(target=_drain, daemon=True); t.start()
            blob = proc.stdout.read(); proc.stdout.close(); t.join(); proc.wait()
            if self._abort.is_set(): raise InterruptedError("Interrotto.")
            if proc.returncode != 0: raise RuntimeError(f"ffmpeg video rc={proc.returncode}")
            return blob
        for raw in proc.stderr:
            if self._abort.is_set(): proc.kill(); raise InterruptedError("Interrotto.")
            self._log(raw.decode("utf-8", "replace").rstrip())
        proc.wait()
        if self._abort.is_set(): raise InterruptedError("Interrotto.")
        if proc.returncode != 0: raise RuntimeError(f"ffmpeg audio rc={proc.returncode}")
        return None

    def _encode_v2(self, o):
        ff = shutil.which("ffmpeg")
        self._log(u"╔══ NFV v2 (classico) ════════════════╗", "accent")
        self._log(f"  {os.path.basename(o['src'])}", "accent")
        trim = []
        if o["ss"]:  trim += ["-ss", o["ss"]]
        if o["dur"]: trim += ["-t", o["dur"]]
        hw = ["-hwaccel", "cuda"] if o["gpu"] else []
        if o["has_audio"]:
            self._log("\n[1/3] Audio MP3…", "accent")
            self._run_ff([ff, "-y"] + hw + trim + ["-i", o["src"], "-vn", "-ac", "1",
                          "-ar", str(o["ar"]), "-c:a", "libmp3lame", "-b:a", f"{o['ab']}k",
                          "-map_metadata", "-1", "-id3v2_version", "0", o["out_mp3"]])  # bare MP3, no ID3 (exact CBR seek)
            self._log(f"  ✓ {os.path.basename(o['out_mp3'])}  {human(os.path.getsize(o['out_mp3']))}", "green")
        if self._abort.is_set(): raise InterruptedError("Interrotto.")
        self._log(f"\n[2/3] MJPEG {V2_W}×{V2_H-1} @ {o['fps']}fps q={o['q']}…", "accent")
        if o["fit"] == "crop":
            vf = (f"fps={o['fps']},scale={V2_W}:{V2_H}:force_original_aspect_ratio=increase:flags=lanczos,"
                  f"crop={V2_W}:{V2_H}:trunc((iw-{V2_W})/2):trunc((ih-{V2_H})/2)")
        else:
            vf = (f"fps={o['fps']},scale={V2_W}:{V2_H}:force_original_aspect_ratio=decrease:flags=lanczos,"
                  f"pad={V2_W}:{V2_H}:trunc((ow-iw)/2):trunc((oh-ih)/2):color=black")
        blob = self._run_ff([ff] + hw + trim + ["-i", o["src"], "-an", "-vf", vf,
                             "-q:v", str(o["q"]), "-c:v", "mjpeg", "-f", "image2pipe", "pipe:1"], binary_stdout=True)
        self._log("\n[3/3] Assemblaggio NFV…", "accent")
        frames = v2_split_jpegs(blob)
        fc, maxf, dur = v2_write_nfv(o["out_nfv"], frames, o["fps"], o["has_audio"])
        vsize = os.path.getsize(o["out_nfv"])
        asize = os.path.getsize(o["out_mp3"]) if o["has_audio"] and os.path.isfile(o["out_mp3"]) else 0
        warn = f"\n  ⚠ frame max {human(maxf)} > 28 KB — alza q" if maxf > 28000 else ""
        self._last_output = o["out_nfv"]
        self._finish(True, f"Done — {fc} frame · {dur/1000:.1f}s\n"
                           f"  📹 {os.path.basename(o['out_nfv'])}  {human(vsize)}\n"
                           f"  🔊 {os.path.basename(o['out_mp3'])}  {human(asize)}\n"
                           f"  TOTALE su SD: {human(vsize+asize)}{warn}")


# ── Cardputer preview player ────────────────────────────────────────────────────
# Renders the .nfv with the SAME engine the firmware uses (nfv3 readers), at the real 240x135
# panel resolution, paced like the device (wall clock as the audio master), with the on-device
# controller overlay so you see exactly what the Cardputer shows. Audio via ffplay (if present).

def _rgb565(v):
    return ((v >> 11 & 0x1f) * 255 // 31, (v >> 5 & 0x3f) * 255 // 63, (v & 0x1f) * 255 // 31)

# device design tokens (from app_video.cpp), as RGB
_CO = {k: _rgb565(v) for k, v in dict(
    BG=0x0861, LINE=0x2945, TXT=0xFFFF, MUT=0x8C71, TRK=0x2A4B, ACC=0x4D9C, GRN=0x8FF3,
    CHIP=0x1926, INK=0x0420, WARM=0xFE8C).items()}
BARY, BARH = 93, 42                                       # controller bar geometry (135-42)


def _font(sz):
    try:
        return ImageFont.load_default(size=sz)
    except TypeError:
        return ImageFont.load_default()


class PreviewPlayer(tk.Toplevel):
    def __init__(self, master, path):
        super().__init__(master)
        self.reader = nfv3.open_nfv(path)
        self.fps = self.reader.fps or 12
        self.frame_count = self.reader.frame_count
        self.vw, self.vh = self.reader.vis_w, self.reader.vis_h
        self.dur_ms = self.reader.duration_ms or int(self.frame_count * 1000 / self.fps)
        ver = "v3 tile-delta" if isinstance(self.reader, nfv3.V3Reader) else "v2"
        self.title(f"Cardputer · {os.path.basename(path)}  [{ver}]")
        self.configure(bg="#0c0c16")
        self.resizable(False, False)

        self.scale = 3
        self.playing = False
        self.pos_ms = 0.0
        self.base_ms = 0.0
        self.play_start = 0.0
        self.volume = 80
        self.brightness = 100
        self.muted = False
        self.ffplay = None
        self._slider_guard = False
        self._was_playing = False
        self._after_id = None
        self.font = _font(8)

        self.audio_path, self._audio_tmp = nfv3.audio_source(path, self.reader, tempfile.gettempdir())
        self.has_audio = self.audio_path is not None and shutil.which("ffplay") is not None
        self.show_overlay = tk.BooleanVar(value=True)
        self.smooth = tk.BooleanVar(value=False)

        self._build()
        self.protocol("WM_DELETE_WINDOW", self._close)
        self._render(0)
        self._bind_keys()
        self.after(60, lambda: (self.focus_force(), self._update_transport()))

    # ── UI ─────────────────────────────────────────────────────────────────────
    def _build(self):
        wrap = tk.Frame(self, bg="#0c0c16", padx=12, pady=12); wrap.pack(fill="both", expand=True)
        # the "panel": a dark bezel around the scaled framebuffer
        self.bezel = tk.Frame(wrap, bg="#000000", bd=0, highlightthickness=6, highlightbackground="#2a2a3a")
        self.bezel.pack()
        self.canvas = tk.Canvas(self.bezel, width=self.vw * self.scale, height=self.vh * self.scale,
                                bg="#000000", highlightthickness=0, cursor="hand2")
        self.canvas.pack()
        self.cv_img = self.canvas.create_image(0, 0, anchor="nw")
        self.canvas.bind("<Button-1>", lambda _e: self._toggle())

        # transport
        tr = tk.Frame(wrap, bg="#0c0c16"); tr.pack(fill="x", pady=(10, 2))
        self.btn_play = ttk.Button(tr, text="▶", width=4, command=self._toggle); self.btn_play.pack(side="left")
        self.seek = tk.Scale(tr, from_=0, to=max(1, self.frame_count - 1), orient="horizontal",
                             showvalue=False, bg="#0c0c16", fg=C["FG"], troughcolor="#26263a",
                             highlightthickness=0, bd=0, sliderrelief="flat", command=self._on_seek)
        self.seek.pack(side="left", fill="x", expand=True, padx=8)
        self.seek.bind("<Button-1>", self._seek_press)
        self.seek.bind("<ButtonRelease-1>", self._seek_release)
        self.lbl_time = tk.Label(tr, text="0:00 / 0:00", bg="#0c0c16", fg=C["DIM"],
                                 font=("Consolas", 9), width=13); self.lbl_time.pack(side="left")

        # options row
        op = tk.Frame(wrap, bg="#0c0c16"); op.pack(fill="x", pady=(6, 0))
        tk.Label(op, text="Vol", bg="#0c0c16", fg=C["DIM"], font=("Segoe UI", 8)).pack(side="left")
        tk.Scale(op, from_=0, to=100, orient="horizontal", length=80, showvalue=False, bg="#0c0c16",
                 troughcolor="#26263a", highlightthickness=0, bd=0, command=self._on_vol).pack(side="left")
        self.seek_vol = op.winfo_children()[-1]; self.seek_vol.set(self.volume)
        tk.Label(op, text="Lum", bg="#0c0c16", fg=C["DIM"], font=("Segoe UI", 8)).pack(side="left", padx=(8, 0))
        b = tk.Scale(op, from_=20, to=100, orient="horizontal", length=80, showvalue=False, bg="#0c0c16",
                     troughcolor="#26263a", highlightthickness=0, bd=0, command=self._on_bri)
        b.set(self.brightness); b.pack(side="left")
        tk.Label(op, text="Zoom", bg="#0c0c16", fg=C["DIM"], font=("Segoe UI", 8)).pack(side="left", padx=(8, 0))
        self.zoom = ttk.Combobox(op, values=["2×", "3×", "4×", "5×"], state="readonly", width=4)
        self.zoom.set(f"{self.scale}×"); self.zoom.pack(side="left"); self.zoom.bind("<<ComboboxSelected>>", self._on_zoom)
        ttk.Checkbutton(op, text="Overlay", variable=self.show_overlay,
                        command=lambda: self._render(self._cur())).pack(side="left", padx=(8, 0))
        ttk.Checkbutton(op, text="Liscio", variable=self.smooth,
                        command=lambda: self._render(self._cur())).pack(side="left")

        hint = "spazio play/pausa · ←/→ -/+5s · ↑/↓ volume · m muto · esc chiudi"
        if not self.has_audio:
            hint += "   (audio non disponibile)"
        tk.Label(wrap, text=hint, bg="#0c0c16", fg=C["DIM"], font=("Segoe UI", 8)).pack(anchor="w", pady=(8, 0))

    def _bind_keys(self):
        self.bind("<space>", lambda _e: self._toggle())
        self.bind("<Left>",  lambda _e: self._seek_rel(-5000))
        self.bind("<Right>", lambda _e: self._seek_rel(+5000))
        self.bind("<Up>",    lambda _e: self._set_vol(self.volume + 10))
        self.bind("<Down>",  lambda _e: self._set_vol(self.volume - 10))
        self.bind("m",       lambda _e: self._toggle_mute())
        self.bind("<Escape>", lambda _e: self._close())

    # ── helpers ──────────────────────────────────────────────────────────────────
    def _cur(self):
        return max(0, min(self.frame_count - 1, int(self.pos_ms * self.fps / 1000)))

    @staticmethod
    def _mmss(ms):
        s = int(ms // 1000); return f"{s // 60}:{s % 60:02d}"

    # ── rendering ──────────────────────────────────────────────────────────────
    def _render(self, target):
        arr = self.reader.frame_at(target)                # (vh, vw, 3) — exactly the device pixels
        img = Image.fromarray(arr, "RGB").convert("RGB")
        if self.show_overlay.get():
            self._draw_overlay(img, target)
        if self.brightness != 100:                        # emulate the panel backlight level
            img = ImageEnhance.Brightness(img).enhance(self.brightness / 100.0)
        resample = Image.LANCZOS if self.smooth.get() else Image.NEAREST
        big = img.resize((self.vw * self.scale, self.vh * self.scale), resample)
        self.photo = ImageTk.PhotoImage(big)
        self.canvas.itemconfig(self.cv_img, image=self.photo)

    def _draw_overlay(self, img, target):
        d = ImageDraw.Draw(img)
        cur_s, tot_s = target // self.fps, max(1, (self.frame_count - 1) // self.fps)
        d.rectangle([0, BARY, self.vw - 1, 134], fill=_CO["BG"])
        d.line([0, BARY, self.vw - 1, BARY], fill=_CO["LINE"])
        # play/pause chip
        d.rounded_rectangle([24, 98, 39, 113], radius=4, fill=_CO["ACC"] if self.playing else _CO["CHIP"])
        d.text((28 if self.playing else 29, 99), "||" if self.playing else ">",
               font=self.font, fill=_CO["INK"] if self.playing else _CO["TXT"])
        # time (right) + title (left, truncated to fit)
        tfield = f"{self._mmss(cur_s*1000)} / {self._mmss(tot_s*1000)}"
        tw = int(d.textlength(tfield, font=self.font))
        d.text((self.vw - 2 - tw, 100), tfield, font=self.font, fill=_CO["MUT"])
        title = os.path.splitext(os.path.basename(self.reader.path))[0]
        avail = (self.vw - 4 - tw) - 62
        while title and d.textlength(title, font=self.font) > avail:
            title = title[:-1]
        d.text((62, 100), title, font=self.font, fill=_CO["TXT"])
        # scrubber + playhead
        px, pw, py = 6, 228, 118
        d.rectangle([px, py - 1, px + pw, py + 1], fill=_CO["TRK"])
        fw = int(pw * cur_s / tot_s) if tot_s else 0
        if fw > 0:
            d.rectangle([px, py - 1, px + fw, py + 1], fill=_CO["ACC"])
        d.ellipse([px + fw - 3, py - 3, px + fw + 3, py + 3], fill=_CO["TXT"])
        # VOL meter
        d.text((6, 124), "VOL" if not self.muted else "MUTE", font=self.font,
               fill=_CO["MUT"] if not self.muted else _CO["WARM"])
        on = 0 if self.muted else round(self.volume * 6 / 100)
        for i in range(6):
            x = 28 + i * 10
            d.rounded_rectangle([x, 125, x + 8, 132], radius=1, fill=_CO["GRN"] if i < on else _CO["TRK"])

    def _update_transport(self):
        self._slider_guard = True
        self.seek.set(self._cur())
        self._slider_guard = False
        self.lbl_time.config(text=f"{self._mmss(self.pos_ms)} / {self._mmss(self.dur_ms)}")
        self.btn_play.config(text="⏸" if self.playing else "▶")

    # ── transport ────────────────────────────────────────────────────────────────
    def _toggle(self):
        self._pause() if self.playing else self._play()

    def _play(self):
        if self.pos_ms >= self.dur_ms - 30:
            self.pos_ms = 0.0
        self.base_ms = self.pos_ms
        self.play_start = time.monotonic()
        self.playing = True
        self._start_audio(self.pos_ms)
        self._update_transport()
        self._tick()

    def _pause(self):
        self.playing = False
        if self._after_id:
            self.after_cancel(self._after_id); self._after_id = None
        self._stop_audio()
        self._update_transport()

    def _tick(self):
        if not self.playing:
            return
        self.pos_ms = self.base_ms + (time.monotonic() - self.play_start) * 1000.0
        if self.pos_ms >= self.dur_ms:
            self.pos_ms = self.dur_ms
            self._render(self.frame_count - 1)
            self._pause()
            return
        self._render(self._cur())
        self._update_transport()
        self._after_id = self.after(max(8, int(1000 / self.fps)), self._tick)

    def _seek_rel(self, dms):
        self.pos_ms = max(0.0, min(self.dur_ms, self.pos_ms + dms))
        if self.playing:
            self.base_ms = self.pos_ms; self.play_start = time.monotonic()
            self._start_audio(self.pos_ms)
        self._render(self._cur()); self._update_transport()

    def _on_seek(self, val):
        if self._slider_guard:
            return
        self.pos_ms = float(int(float(val))) * 1000.0 / self.fps
        self._render(self._cur())
        self.lbl_time.config(text=f"{self._mmss(self.pos_ms)} / {self._mmss(self.dur_ms)}")

    def _seek_press(self, _e):
        self._was_playing = self.playing
        if self.playing:
            self._pause()

    def _seek_release(self, _e):
        if self._was_playing:
            self._play()

    def _on_vol(self, val):
        self._set_vol(int(float(val)), from_slider=True)

    def _set_vol(self, v, from_slider=False):
        self.volume = max(0, min(100, v)); self.muted = False
        if not from_slider:
            self.seek_vol.set(self.volume)
        if self.playing:                                  # ffplay volume is start-time only -> restart
            self._start_audio(self.pos_ms)
        if self.show_overlay.get():
            self._render(self._cur())

    def _toggle_mute(self):
        self.muted = not self.muted
        if self.playing:
            self._start_audio(self.pos_ms)
        if self.show_overlay.get():
            self._render(self._cur())

    def _on_bri(self, val):
        self.brightness = int(float(val)); self._render(self._cur())

    def _on_zoom(self, _e):
        self.scale = int(self.zoom.get().rstrip("×"))
        self.canvas.config(width=self.vw * self.scale, height=self.vh * self.scale)
        self._render(self._cur())

    # ── audio (ffplay; loose sync like the device — both run on the wall clock) ──
    def _start_audio(self, pos_ms):
        self._stop_audio()
        if not self.has_audio or self.muted:
            return
        ff = shutil.which("ffplay")
        flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
        cmd = [ff, "-nodisp", "-autoexit", "-loglevel", "quiet", "-ss", f"{pos_ms/1000:.3f}",
               "-volume", str(self.volume), self.audio_path]
        try:
            self.ffplay = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, **flags)
        except Exception:
            self.ffplay = None

    def _stop_audio(self):
        if self.ffplay:
            try: self.ffplay.kill()
            except Exception: pass
            self.ffplay = None

    def _close(self):
        self.playing = False
        if self._after_id:
            self.after_cancel(self._after_id)
        self._stop_audio()
        try: self.reader.close()
        except Exception: pass
        if self._audio_tmp and self.audio_path and os.path.isfile(self.audio_path):
            try: os.remove(self.audio_path)
            except OSError: pass
        self.destroy()


def main():
    root = TkinterDnD.Tk() if _HAS_DND else tk.Tk()
    app = NfvStudio(root)
    if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]):
        app.v_input.set(sys.argv[1])
    root.mainloop()


if __name__ == "__main__":
    main()
