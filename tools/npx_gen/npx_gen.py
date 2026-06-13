#!/usr/bin/env python3
"""
NPX Generator — NucleoPlayerXtra Audio Analysis Tool
Genera file .npx sidecar per il DJ player di NucleoOS.

Trascina & rilascia file MP3/WAV nella finestra per analizzarli massivamente.
I file .npx vengono salvati accanto agli audio originali sulla SD card.

Requisiti: pip install librosa numpy tkinterdnd2 soundfile
"""
import struct
import os
import threading
import queue
import time
import math
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import librosa

try:
    from tkinterdnd2 import DND_FILES, TkinterDnD
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    HAS_DND = True
except ImportError:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    HAS_DND = False
    print("tkinterdnd2 non trovato – drag & drop disabilitato")

# ── NPX format constants ──────────────────────────────────────────────────────
NPX_MAGIC    = b'NPX1'
NPX_VERSION  = 2           # binary layout unchanged: old firmware still reads these files
# magic(4s), ver,fps,ch,tonic,scale, pad(3s), bpm,dur (ff), frames,sr (II), peak_rms,lufs (ff)
NPX_HDR_FMT  = '<4sBBBBB3sffIIff'   # was '...fffII...' (one float too many) -> pack crashed
NPX_HDR_SIZE = 36
NPX_FRM_FMT  = '<HHHHBBBx'         # rms,bass,mid,high, beat,cue,brightness, pad(x)
NPX_FRM_SIZE = 12

FRAME_RATE   = 10          # frame grid (RAM-cheap). Kick TIMING is sub-frame encoded below.
BASS_HZ      = (20,   250)
MID_HZ       = (250,  4000)
HIGH_HZ      = (4000, 20000)
CROSSFADE_BEATS = 8

# ── Kick timing: sub-frame precision without extra RAM/file cost ───────────────
# The kick analysis runs on a FINE hop (independent of FRAME_RATE) on the SUB
# band, where the kick body slams hard even on brick-walled hardstyle masters.
# Each kick's exact start is encoded into the NpxFrame.beat byte:
#   beat == 0       -> no kick in this frame
#   beat == 1..255  -> kick present; start = (beat-1)/254 of the way into the frame
# => ~0.4 ms kick resolution at 10 fps, zero extra bytes, and byte-compatible:
#    old firmware that reads `if (beat)` still sees every kick as a hit.
ONSET_HOP    = 256         # ~5.8 ms @ 44.1 kHz: kick-start resolution (STFT-window bound)
KICK_BAND_HZ = 160.0       # sub/kick body ceiling for low-band onset detection

# ── Palette ───────────────────────────────────────────────────────────────────
C = {
    'bg'      : '#0c1220',
    'bg2'     : '#111b2e',
    'bg3'     : '#1a2640',
    'acc'     : '#ff7eb6',
    'green'   : '#7cfc9a',
    'orange'  : '#fd8c40',
    'blue'    : '#4fb8ff',
    'dim'     : '#3a4c66',
    'text'    : '#e8ecf4',
    'muted'   : '#6b7fa0',
    'error'   : '#ff5555',
    'done'    : '#7cfc9a',
    'pending' : '#4fb8ff',
    'proc'    : '#fd8c40',
}

# ── Analysis ─────────────────────────────────────────────────────────────────
@dataclass
class NpxResult:
    path: str = ''
    bpm: float = 0.0
    duration_s: float = 0.0
    sample_rate: int = 44100
    channels: int = 1
    key_tonic: int = 0
    key_scale: int = 0
    peak_rms: float = 0.0
    lufs: float = -14.0
    frame_count: int = 0
    frames_rms:  list = field(default_factory=list)
    frames_bass: list = field(default_factory=list)
    frames_mid:  list = field(default_factory=list)
    frames_high: list = field(default_factory=list)
    frames_beat: list = field(default_factory=list)
    frames_cue:  list = field(default_factory=list)
    frames_bright: list = field(default_factory=list)
    drop_s: float = 0.0
    kick_count: int = 0
    error: str = ''


def freq_band_energy(S: np.ndarray, sr: int, lo_hz: float, hi_hz: float) -> np.ndarray:
    """Mean power spectral energy in [lo_hz, hi_hz) per frame."""
    freqs = librosa.fft_frequencies(sr=sr, n_fft=(S.shape[0]-1)*2)
    mask = (freqs >= lo_hz) & (freqs < hi_hz)
    if not mask.any():
        return np.zeros(S.shape[1])
    return np.mean(S[mask, :], axis=0)


# ── DJ-grade kick / drop / loudness analysis ──────────────────────────────────
def approx_lufs(y: np.ndarray, sr: int) -> float:
    """Integrated loudness ≈ ITU-R BS.1770 K-weighting (good to ~±1 LU for
    gain matching). Falls back to plain dBFS power if scipy is unavailable.
    Coeffs are the standard 48 kHz K-weighting, applied as-is (broad curves →
    negligible error at 44.1 kHz). The old code reported raw mean-square dBFS,
    which is NOT LUFS and reads ~5 LU off on loud masters."""
    try:
        from scipy.signal import lfilter
    except Exception:
        return float(10 * np.log10(np.mean(y ** 2) + 1e-10))
    b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]
    a1 = [1.0, -1.69065929318241, 0.73248077421585]
    b2 = [1.0, -2.0, 1.0]
    a2 = [1.0, -1.99004745483398, 0.99007225036621]
    yk = lfilter(b2, a2, lfilter(b1, a1, y.astype(np.float64)))
    return float(-0.691 + 10 * np.log10(float(np.mean(yk * yk)) + 1e-12))


def _sub_energy_env(y: np.ndarray, sr: int, hop: int):
    """Per-frame energy of the SUB band only (≤ KICK_BAND_HZ), peak-normalised.
    Hard/hardcore masters are brick-walled: broadband RMS barely moves, but the
    kick body SLAMS below ~160 Hz. This envelope has one clean spike per kick.
    Returns (env 0..1, env_fps)."""
    n_fft = 2048
    S = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    band = S[freqs <= KICK_BAND_HZ, :]
    if band.shape[0] == 0:
        band = S[:4, :]
    env = band.sum(axis=0).astype(np.float64)
    peak = float(env.max())
    if peak > 0:
        env /= peak
    return env, (sr / float(hop))


def _fft_autocorr(x: np.ndarray) -> np.ndarray:
    """Unbiased-ish autocorrelation via FFT — O(n log n), fast on long tracks
    (plain np.correlate is O(n²) and crawls on a 5-minute song)."""
    x = x - x.mean()
    n = 1 << int(np.ceil(np.log2(max(2, x.size * 2))))
    f = np.fft.rfft(x, n)
    ac = np.fft.irfft(f * np.conj(f))[:x.size]
    return ac


def detect_kicks(y: np.ndarray, sr: int, duration_s: float, bpm_hint: float):
    """Find the precise start of EVERY kick. Returns (kick_times_s, bpm).

    Built for 4-on-the-floor (hardstyle/hardcore/EDM) where the kick IS the
    beat. Why this beats `beat_track + onset_detect` (the old code): that dumped
    every transient (hats, clicks, riffs) into the beat array — and the kick's
    own click gave a SECOND onset per kick, halving the period (we saw 200 BPM
    on a 150 BPM track). Here:
      1. sub-band energy envelope → one clean spike per kick (no click doubles)
      2. period via FFT autocorrelation of that envelope, searched only in the
         95..210 BPM dance window → no octave (half/double-time) error
      3. phase via grid search (the offset whose grid lands on the spikes)
      4. per-beat: snap to the local sub peak, then walk back to the FOOT of the
         attack (25% of peak) = the exact moment the kick starts
      5. breakdowns stay empty: a grid slot with no real energy is skipped
    """
    env, fps = _sub_energy_env(y, sr, ONSET_HOP)
    if env.size < 16:
        return [], bpm_hint

    # --- period: autocorrelation peak inside the dance BPM window ---
    ac = _fft_autocorr(env)
    lo = max(1, int(fps * 60.0 / 210.0))       # 210 BPM
    hi = min(ac.size - 1, int(fps * 60.0 / 95.0))   # 95 BPM
    if hi <= lo:
        return [], bpm_hint
    period_f = lo + int(np.argmax(ac[lo:hi]))
    if period_f < 2:
        return [], bpm_hint
    period_s = period_f / fps
    bpm = round(60.0 / period_s, 1)

    # --- phase: the grid offset whose samples sum the most sub energy ---
    P = int(round(period_f))
    best_off, best_score = 0, -1.0
    for o in range(P):
        score = float(env[o::P].sum())
        if score > best_score:
            best_score, best_off = score, o
    phase_f = best_off

    # --- pass 1: peak of the sub envelope at every grid slot ---
    win = max(1, int(period_f * 0.4))
    slots = []                                  # (peak_idx, peak_val)
    k = 0
    while True:
        center = phase_f + k * period_f
        if center - win >= env.size:
            break
        k += 1
        c = int(round(center))
        a0 = max(0, c - win)
        b0 = min(env.size, c + win + 1)
        if b0 <= a0:
            continue
        loc = a0 + int(np.argmax(env[a0:b0]))
        slots.append((loc, float(env[loc])))
    if not slots:
        return [], (bpm if bpm > 0 else bpm_hint)

    # Threshold off the MEDIAN slot peak: real kicks are the majority and set
    # the level, so a breakdown slot (a fraction of that) falls below 40% and
    # is dropped — no phantom kicks in silent sections.
    med_peak = float(np.median([p for _, p in slots]))
    thr = 0.4 * med_peak

    # --- pass 2: keep loud slots, walk back to the FOOT of the attack ---
    kicks = []
    for loc, peak in slots:
        if peak < thr:
            continue
        floor = 0.25 * peak
        i = loc
        while i > 0 and env[i - 1] > floor:     # the exact moment the kick starts
            i -= 1
        t = i / fps
        if 0.0 <= t <= duration_s:
            kicks.append(round(t, 4))
    kicks = sorted(set(kicks))
    return kicks, (bpm if bpm > 0 else bpm_hint)


def detect_drop(bass_norm: np.ndarray, fps: float):
    """First real drop: sub energy explodes after a breakdown. The 1 s-smoothed
    sub envelope is re-normalised to its own peak (so impulsive kick energy and
    sustained walls compare alike), then we look for: a return to ≥0.65 within
    15 s of a valley <0.35, with ≥2 s of sustain. Returns the frame index, or
    None (no club-style drop)."""
    e = np.asarray(bass_norm, dtype=np.float64)
    if e.size < int(10 * fps):
        return None
    win = max(3, int(round(1.0 * fps)))
    sm = np.convolve(e, np.ones(win) / win, mode="same")
    peak = float(sm.max())
    if peak <= 0:
        return None
    sm = sm / peak
    look = int(15.0 * fps)
    i0 = int(8.0 * fps)
    i_end = sm.size - int(2.0 * fps)
    for i in range(i0, max(i0, i_end)):
        if sm[i] < 0.65:
            continue
        j0 = max(0, i - look)
        if sm[j0:i].size and float(sm[j0:i].min()) < 0.35:
            j1 = min(sm.size, i + int(2.0 * fps))
            if j1 > i and float(sm[i:j1].mean()) > 0.55:
                return i
    return None


def analyse(path: str, progress_cb=None) -> NpxResult:
    res = NpxResult(path=path)
    try:
        if progress_cb: progress_cb(0, "Caricamento audio…")
        y, sr = librosa.load(path, sr=None, mono=False)

        # Channels
        if y.ndim == 2:
            res.channels = y.shape[0]
            y_mono = librosa.to_mono(y)
        else:
            res.channels = 1
            y_mono = y

        res.sample_rate = sr
        res.duration_s  = float(len(y_mono)) / sr

        if progress_cb: progress_cb(10, "Rilevamento BPM…")
        tempo, _ = librosa.beat.beat_track(y=y_mono, sr=sr, units='frames')
        bpm_hint = float(np.atleast_1d(tempo)[0])

        if progress_cb: progress_cb(24, "Analisi spettrale…")
        hop = max(1, sr // FRAME_RATE)            # samples per NPX frame
        n_fft = min(2048, hop * 2)
        S = np.abs(librosa.stft(y_mono, n_fft=n_fft, hop_length=hop)) ** 2

        if progress_cb: progress_cb(38, "RMS + bande + luminosità…")
        rms_frames  = librosa.feature.rms(y=y_mono, hop_length=hop)[0]
        bass_frames = freq_band_energy(S, sr, *BASS_HZ)
        mid_frames  = freq_band_energy(S, sr, *MID_HZ)
        high_frames = freq_band_energy(S, sr, *HIGH_HZ)
        centroid = librosa.feature.spectral_centroid(y=y_mono, sr=sr, hop_length=hop)[0]

        n = min(len(rms_frames), len(bass_frames), len(mid_frames),
                len(high_frames), len(centroid))
        rms_frames, bass_frames = rms_frames[:n], bass_frames[:n]
        mid_frames, high_frames = mid_frames[:n], high_frames[:n]
        centroid = centroid[:n]

        if progress_cb: progress_cb(52, "Rilevamento tonalità (Key)…")
        chroma = librosa.feature.chroma_stft(y=y_mono, sr=sr, hop_length=hop)
        chroma_sum = np.sum(chroma, axis=1)
        # Krumhansl-Schmuckler key profiles
        major_prof = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
        minor_prof = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]
        best_corr = -2.0
        for i in range(12):
            maj_c = np.corrcoef(chroma_sum, np.roll(major_prof, i))[0, 1]
            min_c = np.corrcoef(chroma_sum, np.roll(minor_prof, i))[0, 1]
            if maj_c > best_corr: best_corr, res.key_tonic, res.key_scale = maj_c, i, 0
            if min_c > best_corr: best_corr, res.key_tonic, res.key_scale = min_c, i, 1

        if progress_cb: progress_cb(66, "Rilevamento casse (kick)…")
        kicks, bpm = detect_kicks(y_mono, sr, res.duration_s, bpm_hint)
        res.bpm = bpm if bpm > 0 else bpm_hint
        res.kick_count = len(kicks)

        if progress_cb: progress_cb(84, "Drop + loudness…")
        res.lufs = approx_lufs(y_mono, sr)

        # Per-band quantisation: each band on its OWN peak so the VU is readable
        # (the old code squashed bass/mid/high against the global RMS peak).
        def to_u16(arr):
            peak = float(np.max(arr)) if arr.size and np.max(arr) > 0 else 1.0
            return np.clip(arr / peak, 0.0, 1.0)
        bass_n = to_u16(bass_frames)
        res.peak_rms    = float(np.max(rms_frames)) if n else 1.0
        res.frame_count = n
        res.frames_rms  = (to_u16(rms_frames)  * 65535).astype(np.uint16).tolist()
        res.frames_bass = (bass_n              * 65535).astype(np.uint16).tolist()
        res.frames_mid  = (to_u16(mid_frames)  * 65535).astype(np.uint16).tolist()
        res.frames_high = (to_u16(high_frames) * 65535).astype(np.uint16).tolist()
        c_norm = np.clip(centroid / 8000.0, 0, 1)
        res.frames_bright = (c_norm * 255).astype(np.uint8).tolist()

        # beat byte = sub-frame kick offset (1..255); 0 = no kick in this frame.
        beat_arr = [0] * n
        for kt in kicks:
            fi = int(kt * FRAME_RATE)
            if 0 <= fi < n and beat_arr[fi] == 0:
                frac = kt * FRAME_RATE - fi          # 0..1 position inside the frame
                val = 1 + int(round(254 * frac))
                beat_arr[fi] = 1 if val < 1 else (255 if val > 255 else val)
        res.frames_beat = beat_arr

        # cue byte = 1 at the auto-detected drop frame, 0 elsewhere.
        cue_arr = [0] * n
        drop_fi = detect_drop(bass_n, float(FRAME_RATE))
        if drop_fi is not None and 0 <= drop_fi < n:
            cue_arr[drop_fi] = 1
            res.drop_s = round(drop_fi / float(FRAME_RATE), 2)
        res.frames_cue = cue_arr

        if progress_cb: progress_cb(95, "Scrittura .npx…")

    except Exception:
        res.error = traceback.format_exc()

    return res


def write_npx(res: NpxResult, out_path: str) -> None:
    hdr = struct.pack(
        NPX_HDR_FMT,
        NPX_MAGIC,          # magic
        NPX_VERSION,        # version
        FRAME_RATE,         # frame_rate
        res.channels,       # channels
        res.key_tonic,      # key_tonic
        res.key_scale,      # key_scale
        b'\x00\x00\x00',    # _pad[3]
        res.bpm,            # bpm
        res.duration_s,     # duration_s
        res.frame_count,    # frame_count
        res.sample_rate,    # sample_rate
        res.peak_rms,       # peak_rms
        res.lufs,           # lufs
    )
    assert len(hdr) == NPX_HDR_SIZE, f"Header {len(hdr)} != {NPX_HDR_SIZE}"

    with open(out_path, 'wb') as f:
        f.write(hdr)
        for i in range(res.frame_count):
            f.write(struct.pack(
                NPX_FRM_FMT,
                res.frames_rms[i],
                res.frames_bass[i],
                res.frames_mid[i],
                res.frames_high[i],
                res.frames_beat[i],
                res.frames_cue[i] if i < len(res.frames_cue) else 0,  # 1 = drop
                res.frames_bright[i],
            ))


# ── GUI ───────────────────────────────────────────────────────────────────────
class FileItem:
    def __init__(self, path: str):
        self.path    = path
        self.name    = Path(path).name
        self.status  = 'pending'   # pending | running | done | error
        self.progress = 0
        self.msg     = ''
        self.bpm     = 0.0
        self.dur     = 0.0
        self.size_kb = 0
        self.npx_path = ''


class NpxApp:
    FONT_MONO = ('Consolas', 9)
    FONT_SM   = ('Segoe UI', 9)
    FONT_MD   = ('Segoe UI', 11)
    FONT_LG   = ('Segoe UI', 14, 'bold')
    FONT_TITLE= ('Segoe UI', 18, 'bold')

    def __init__(self, root):
        self.root = root
        root.title("NPX Generator  —  NucleoOS DJ Sidecar Tool")
        root.geometry("1060x680")
        root.minsize(860, 560)
        root.configure(bg=C['bg'])
        root.resizable(True, True)

        self.files: list[FileItem] = []
        self.running = False
        self.work_q: queue.Queue = queue.Queue()
        self._sel_idx: Optional[int] = None

        # Output mode: 'same' = next to audio, 'folder' = custom dir
        self.out_mode = tk.StringVar(value='same')
        self.out_folder = tk.StringVar(value='')

        self._build_ui()
        self._start_ui_poll()

        if HAS_DND:
            self.root.drop_target_register(DND_FILES)
            self.root.dnd_bind('<<Drop>>', self._on_drop)

    # ── UI build ──────────────────────────────────────────────────────────────
    def _build_ui(self):
        root = self.root

        # ── Header ────────────────────────────────────────────────────────────
        hdr = tk.Frame(root, bg=C['bg'], pady=8)
        hdr.pack(fill='x', padx=16)
        tk.Label(hdr, text="🎛  NPX Generator", font=self.FONT_TITLE,
                 bg=C['bg'], fg=C['acc']).pack(side='left')
        tk.Label(hdr, text="NucleoOS DJ Sidecar Tool  |  Drag & Drop MP3 / WAV",
                 font=self.FONT_SM, bg=C['bg'], fg=C['muted']).pack(side='left', padx=14)

        # ── Main layout ───────────────────────────────────────────────────────
        main = tk.Frame(root, bg=C['bg'])
        main.pack(fill='both', expand=True, padx=12, pady=4)

        # Left: drop zone + queue list
        left = tk.Frame(main, bg=C['bg'])
        left.pack(side='left', fill='both', expand=True)

        # Drop zone
        self._build_dropzone(left)

        # Queue list
        self._build_queue(left)

        # Right: settings + preview
        right = tk.Frame(main, bg=C['bg2'], width=300)
        right.pack(side='right', fill='y', padx=(10,0))
        right.pack_propagate(False)
        self._build_right(right)

        # ── Footer toolbar ────────────────────────────────────────────────────
        self._build_footer(root)

    def _build_dropzone(self, parent):
        frame = tk.Frame(parent, bg=C['bg2'], bd=0, highlightthickness=2,
                         highlightbackground=C['dim'])
        frame.pack(fill='x', pady=(0,8))

        inner = tk.Frame(frame, bg=C['bg2'], padx=16, pady=14)
        inner.pack(fill='x')

        tk.Label(inner, text="⬇  Trascina qui file MP3 / WAV",
                 font=('Segoe UI', 13, 'bold'), bg=C['bg2'], fg=C['acc']).pack()
        tk.Label(inner, text="oppure usa il pulsante Aggiungi File",
                 font=self.FONT_SM, bg=C['bg2'], fg=C['muted']).pack()

        btns = tk.Frame(inner, bg=C['bg2'])
        btns.pack(pady=8)
        self._btn(btns, "📁  Aggiungi File", self._browse_files).pack(side='left', padx=4)
        self._btn(btns, "📂  Aggiungi Cartella", self._browse_folder).pack(side='left', padx=4)

    def _build_queue(self, parent):
        tk.Label(parent, text="Coda file", font=('Segoe UI', 10, 'bold'),
                 bg=C['bg'], fg=C['text']).pack(anchor='w', padx=4)

        frame = tk.Frame(parent, bg=C['bg2'], bd=0)
        frame.pack(fill='both', expand=True)

        cols = ('name','status','bpm','dur','size','npx')
        self.tree = ttk.Treeview(frame, columns=cols, show='headings',
                                  selectmode='browse', style='NPX.Treeview')
        self.tree.heading('name',   text='File')
        self.tree.heading('status', text='Stato')
        self.tree.heading('bpm',    text='BPM')
        self.tree.heading('dur',    text='Durata')
        self.tree.heading('size',   text='NPX KB')
        self.tree.heading('npx',    text='Output')

        self.tree.column('name',   width=240, stretch=True)
        self.tree.column('status', width=90,  stretch=False)
        self.tree.column('bpm',    width=60,  stretch=False)
        self.tree.column('dur',    width=70,  stretch=False)
        self.tree.column('size',   width=65,  stretch=False)
        self.tree.column('npx',    width=160, stretch=True)

        sb = ttk.Scrollbar(frame, orient='vertical', command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')

        # Per-item progress bar overlay via canvas
        self.tree.tag_configure('pending', foreground=C['pending'])
        self.tree.tag_configure('running', foreground=C['proc'])
        self.tree.tag_configure('done',    foreground=C['done'])
        self.tree.tag_configure('error',   foreground=C['error'])

        self.tree.bind('<Delete>', self._remove_selected)
        self.tree.bind('<<TreeviewSelect>>', self._on_select)

        self._apply_treeview_style()

    def _build_right(self, parent):
        pad = dict(padx=12, pady=6)

        tk.Label(parent, text="⚙  Impostazioni", font=('Segoe UI',11,'bold'),
                 bg=C['bg2'], fg=C['acc']).pack(anchor='w', **pad)

        # Output mode
        tk.Label(parent, text="Output:", font=self.FONT_SM, bg=C['bg2'], fg=C['muted']
                 ).pack(anchor='w', padx=12)
        f = tk.Frame(parent, bg=C['bg2'])
        f.pack(anchor='w', padx=12)
        tk.Radiobutton(f, text="Accanto all'audio (SD card)", variable=self.out_mode,
                       value='same', bg=C['bg2'], fg=C['text'],
                       selectcolor=C['bg3'], activebackground=C['bg2'],
                       font=self.FONT_SM).pack(anchor='w')
        tk.Radiobutton(f, text="Cartella personalizzata", variable=self.out_mode,
                       value='folder', bg=C['bg2'], fg=C['text'],
                       selectcolor=C['bg3'], activebackground=C['bg2'],
                       font=self.FONT_SM).pack(anchor='w')
        bf = tk.Frame(parent, bg=C['bg2'])
        bf.pack(fill='x', padx=12, pady=2)
        self._btn(bf, "📂 Scegli", self._choose_out_folder, small=True).pack(side='left')
        self.lbl_folder = tk.Label(bf, textvariable=self.out_folder, font=('Consolas',8),
                                   bg=C['bg2'], fg=C['muted'], wraplength=180, justify='left')
        self.lbl_folder.pack(side='left', padx=4)

        tk.Frame(parent, bg=C['dim'], height=1).pack(fill='x', padx=12, pady=8)

        # Preview panel
        tk.Label(parent, text="📊  Preview", font=('Segoe UI',11,'bold'),
                 bg=C['bg2'], fg=C['acc']).pack(anchor='w', padx=12)

        self.lbl_preview = tk.Label(parent, text="Seleziona un file dalla lista",
                                    font=self.FONT_SM, bg=C['bg2'], fg=C['muted'],
                                    justify='left', wraplength=270)
        self.lbl_preview.pack(anchor='w', padx=12, pady=4)

        # Mini waveform canvas
        self.cv_wave = tk.Canvas(parent, bg=C['bg3'], height=80, bd=0,
                                 highlightthickness=1, highlightbackground=C['dim'])
        self.cv_wave.pack(fill='x', padx=12, pady=4)

        # Energy bands legend
        leg = tk.Frame(parent, bg=C['bg2'])
        leg.pack(anchor='w', padx=12)
        for c, lbl in [(C['orange'],'Bass'), (C['green'],'Mid'), (C['acc'],'High')]:
            tk.Label(leg, text='■', fg=c, bg=C['bg2'], font=self.FONT_SM).pack(side='left')
            tk.Label(leg, text=lbl, fg=C['muted'], bg=C['bg2'], font=self.FONT_SM).pack(side='left', padx=(0,6))

        tk.Frame(parent, bg=C['dim'], height=1).pack(fill='x', padx=12, pady=8)

        # Stats
        self.lbl_stats = tk.Label(parent, text='', font=self.FONT_MONO,
                                  bg=C['bg2'], fg=C['text'], justify='left')
        self.lbl_stats.pack(anchor='w', padx=12)

    def _build_footer(self, parent):
        footer = tk.Frame(parent, bg=C['bg3'], pady=8)
        footer.pack(fill='x', padx=12, pady=(4,8))

        # Global progress
        pb_frame = tk.Frame(footer, bg=C['bg3'])
        pb_frame.pack(fill='x', padx=8, pady=(0,6))
        self.lbl_progress = tk.Label(pb_frame, text='Pronto', font=self.FONT_SM,
                                     bg=C['bg3'], fg=C['muted'])
        self.lbl_progress.pack(side='left')
        self.pb_total = ttk.Progressbar(pb_frame, mode='determinate',
                                         style='NPX.Horizontal.TProgressbar', length=300)
        self.pb_total.pack(side='right')

        # Buttons
        btns = tk.Frame(footer, bg=C['bg3'])
        btns.pack()
        self.btn_start = self._btn(btns, "▶  Avvia Analisi",    self._start_all, accent=True)
        self.btn_start.pack(side='left', padx=6)
        self._btn(btns, "⏹  Stop",              self._stop).pack(side='left', padx=6)
        self._btn(btns, "✅  Rimuovi Completati",self._clear_done).pack(side='left', padx=6)
        self._btn(btns, "🗑  Svuota Lista",      self._clear_all).pack(side='left', padx=6)

    # ── Widget helpers ────────────────────────────────────────────────────────
    def _btn(self, parent, text, cmd, accent=False, small=False):
        fg  = C['bg']  if accent else C['text']
        bg  = C['acc'] if accent else C['bg3']
        hbg = '#ff9ccc' if accent else C['bg2']
        f   = ('Segoe UI', 9, 'bold') if accent else self.FONT_SM
        b = tk.Button(parent, text=text, command=cmd, font=f,
                      bg=bg, fg=fg, activebackground=hbg, activeforeground=fg,
                      relief='flat', padx=8 if small else 14,
                      pady=2 if small else 6, cursor='hand2', bd=0)
        b.bind('<Enter>', lambda e: b.config(bg=hbg))
        b.bind('<Leave>', lambda e: b.config(bg=bg))
        return b

    def _apply_treeview_style(self):
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('NPX.Treeview',
                         background=C['bg2'], foreground=C['text'],
                         fieldbackground=C['bg2'], rowheight=26,
                         borderwidth=0, font=self.FONT_SM)
        style.configure('NPX.Treeview.Heading',
                         background=C['bg3'], foreground=C['acc'],
                         relief='flat', font=('Segoe UI',9,'bold'))
        style.map('NPX.Treeview', background=[('selected', C['bg3'])],
                  foreground=[('selected', C['acc'])])
        style.configure('NPX.Horizontal.TProgressbar',
                         troughcolor=C['bg3'], background=C['acc'],
                         borderwidth=0, thickness=8)

    # ── File management ───────────────────────────────────────────────────────
    def _add_paths(self, paths: list[str]):
        existing = {f.path for f in self.files}
        added = 0
        for p in paths:
            p = p.strip().strip('{}')
            if not os.path.isfile(p): continue
            if not p.lower().endswith(('.mp3','.wav','.flac','.ogg')): continue
            if p in existing: continue
            fi = FileItem(p)
            fi.size_kb = os.path.getsize(p) // 1024
            self.files.append(fi)
            existing.add(p)
            added += 1
        if added:
            self._refresh_tree()
            self._update_stats()

    def _on_drop(self, event):
        raw = event.data.strip()
        # Handle multiple paths: can be space-separated or newline-separated
        if '\n' in raw:
            paths = [p.strip().strip('{}') for p in raw.splitlines()]
        else:
            # Space-separated but paths may have spaces — use {} grouping
            paths = []
            cur = ''
            inside = False
            for ch in raw:
                if ch == '{': inside = True
                elif ch == '}': inside = False; paths.append(cur); cur = ''
                elif ch == ' ' and not inside:
                    if cur: paths.append(cur); cur = ''
                else:
                    cur += ch
            if cur: paths.append(cur)
        # Expand directories
        expanded = []
        for p in paths:
            if os.path.isdir(p):
                for root_, _, files in os.walk(p):
                    for f in files:
                        expanded.append(os.path.join(root_, f))
            else:
                expanded.append(p)
        self._add_paths(expanded)

    def _browse_files(self):
        paths = filedialog.askopenfilenames(
            title='Seleziona file audio',
            filetypes=[('Audio', '*.mp3 *.wav *.flac *.ogg'), ('Tutti', '*.*')])
        if paths: self._add_paths(list(paths))

    def _browse_folder(self):
        folder = filedialog.askdirectory(title='Seleziona cartella')
        if not folder: return
        paths = []
        for root_, _, files in os.walk(folder):
            for f in files:
                paths.append(os.path.join(root_, f))
        self._add_paths(paths)

    def _choose_out_folder(self):
        folder = filedialog.askdirectory(title='Cartella di output')
        if folder:
            self.out_folder.set(folder)
            self.out_mode.set('folder')

    def _remove_selected(self, event=None):
        sel = self.tree.selection()
        if not sel: return
        idx = self.tree.index(sel[0])
        if self.files[idx].status == 'running': return
        self.files.pop(idx)
        self._refresh_tree()
        self._update_stats()

    def _clear_done(self):
        self.files = [f for f in self.files if f.status not in ('done','error')]
        self._refresh_tree()
        self._update_stats()

    def _clear_all(self):
        if self.running:
            messagebox.showwarning("Stop richiesto", "Ferma l'analisi prima di svuotare la lista.")
            return
        self.files.clear()
        self._refresh_tree()
        self._update_stats()

    def _refresh_tree(self):
        self.tree.delete(*self.tree.get_children())
        STATUS_ICON = {'pending':'⏳','running':'⚙','done':'✅','error':'❌'}
        for fi in self.files:
            icon = STATUS_ICON.get(fi.status, '?')
            bpm  = f"{fi.bpm:.0f}" if fi.bpm > 0 else '—'
            dur  = f"{int(fi.dur//60)}:{int(fi.dur%60):02d}" if fi.dur > 0 else '—'
            npx_kb = f"{os.path.getsize(fi.npx_path)//1024} KB" if fi.npx_path and os.path.exists(fi.npx_path) else '—'
            npx_name = Path(fi.npx_path).name if fi.npx_path else ''
            self.tree.insert('', 'end', values=(fi.name, f"{icon} {fi.status}", bpm, dur, npx_kb, npx_name),
                             tags=(fi.status,))

    def _update_stats(self):
        total  = len(self.files)
        done   = sum(1 for f in self.files if f.status == 'done')
        errors = sum(1 for f in self.files if f.status == 'error')
        self.lbl_stats.config(
            text=f"Totale:  {total}\nCompletati:  {done}\nErrori:  {errors}")

    # ── Processing ────────────────────────────────────────────────────────────
    def _start_all(self):
        if self.running:
            messagebox.showinfo("Info", "Analisi già in corso.")
            return
        pending = [f for f in self.files if f.status in ('pending','error')]
        if not pending:
            messagebox.showinfo("Info", "Nessun file da elaborare.")
            return
        self.running = True
        self.btn_start.config(state='disabled')
        t = threading.Thread(target=self._worker, args=(pending,), daemon=True)
        t.start()

    def _stop(self):
        self.running = False
        self.lbl_progress.config(text='Fermato.', fg=C['error'])

    def _worker(self, items: list[FileItem]):
        total = len(items)
        for i, fi in enumerate(items):
            if not self.running: break
            fi.status = 'running'
            self._queue_refresh()

            def progress_cb(pct, msg, _fi=fi):
                _fi.progress = pct
                _fi.msg = msg
                self.work_q.put(('msg', f"[{_fi.name}]  {pct:3d}%  {msg}"))

            res = analyse(fi.path, progress_cb)

            if res.error:
                fi.status = 'error'
                fi.msg = res.error.splitlines()[-1]
                self.work_q.put(('msg', f"❌ Errore: {fi.name}"))
            else:
                fi.bpm  = res.bpm
                fi.dur  = res.duration_s

                # Determine output path
                if self.out_mode.get() == 'folder' and self.out_folder.get():
                    npx_out = os.path.join(self.out_folder.get(),
                                           Path(fi.path).stem + '.npx')
                else:
                    npx_out = str(Path(fi.path).with_suffix('.npx'))

                try:
                    write_npx(res, npx_out)
                    fi.npx_path = npx_out
                    fi.status = 'done'
                    self.work_q.put(('preview', res))
                except Exception as e:
                    fi.status = 'error'
                    fi.msg = str(e)

            self._queue_refresh()
            pct_total = int((i+1)/total*100)
            self.work_q.put(('total_pct', pct_total))

        self.running = False
        self.work_q.put(('done', None))

    def _queue_refresh(self):
        self.work_q.put(('refresh', None))

    # ── UI polling (thread-safe) ───────────────────────────────────────────────
    def _start_ui_poll(self):
        self._last_res: Optional[NpxResult] = None
        self.root.after(100, self._poll)

    def _poll(self):
        try:
            while True:
                kind, data = self.work_q.get_nowait()
                if kind == 'refresh':
                    self._refresh_tree()
                    self._update_stats()
                elif kind == 'msg':
                    self.lbl_progress.config(text=str(data), fg=C['muted'])
                elif kind == 'total_pct':
                    self.pb_total['value'] = data
                elif kind == 'preview':
                    self._last_res = data
                    self._draw_preview(data)
                elif kind == 'done':
                    self.btn_start.config(state='normal')
                    self.lbl_progress.config(text='✅ Completato', fg=C['done'])
                    self._refresh_tree()
                    self._update_stats()
        except queue.Empty:
            pass
        self.root.after(80, self._poll)

    def _on_select(self, event=None):
        sel = self.tree.selection()
        if not sel: return
        idx = self.tree.index(sel[0])
        fi = self.files[idx]
        if self._last_res and self._last_res.path == fi.path:
            self._draw_preview(self._last_res)
            return
        # Show file info without preview
        self.lbl_preview.config(
            text=f"{fi.name}\nStato: {fi.status}\nBPM: {fi.bpm:.1f}\n"
                 f"Durata: {int(fi.dur//60)}:{int(fi.dur%60):02d}\n"
                 f"{fi.msg if fi.msg else ''}",
            fg=C['muted'])
        self.cv_wave.delete('all')

    def _draw_preview(self, res: NpxResult):
        if not res or res.frame_count == 0: return
        w = self.cv_wave.winfo_width() or 270
        h = 80
        self.cv_wave.config(width=w)
        self.cv_wave.delete('all')

        n = res.frame_count
        step = max(1, n // w)

        def draw_band(frames, color, scale=1.0):
            pts = []
            for x in range(w):
                fi_idx = min(int(x * n / w), n-1)
                v = frames[fi_idx] / 65535.0 * h * scale
                pts.extend([x, h - v])
            if len(pts) >= 4:
                self.cv_wave.create_line(pts, fill=color, width=1)

        draw_band(res.frames_bass, C['orange'], 0.8)
        draw_band(res.frames_mid,  C['green'],  0.7)
        draw_band(res.frames_high, C['acc'],    0.6)

        # Kick markers (tick height encodes the sub-frame offset: a faint hint
        # that the exact start is known to the millisecond, not just the frame).
        for i, beat in enumerate(res.frames_beat):
            if beat:
                x = int(i * w / n)
                self.cv_wave.create_line(x, h-9, x, h, fill='#ff4488', width=1)
        # Drop marker (auto-detected): full-height cyan line.
        for i, cue in enumerate(res.frames_cue):
            if cue:
                x = int(i * w / n)
                self.cv_wave.create_line(x, 0, x, h, fill=C['blue'], width=2)

        bpm_s   = f"{res.bpm:.1f} BPM" if res.bpm > 0 else "BPM n/d"
        dur_min = int(res.duration_s // 60)
        dur_sec = int(res.duration_s % 60)
        dur_s   = f"{dur_min}:{dur_sec:02d}"
        cf_s    = 60 / res.bpm * CROSSFADE_BEATS if res.bpm > 0 else 0
        
        TONICS = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
        key_str = f"{TONICS[res.key_tonic]} {'Minor' if res.key_scale == 1 else 'Major'}"

        npx_size = 0
        if res.path:
            npx_path = str(Path(res.path).with_suffix('.npx'))
            if os.path.exists(npx_path):
                npx_size = os.path.getsize(npx_path)

        drop_s = (f"{int(res.drop_s//60)}:{int(res.drop_s%60):02d}"
                  if res.drop_s > 0 else "—")
        self.lbl_preview.config(
            text=(f"📁 {Path(res.path).name}\n"
                  f"⏱  {dur_s}  •  🎵 {bpm_s}  •  🎹 {key_str}\n"
                  f"🥁 {res.kick_count} casse  •  💥 drop {drop_s}\n"
                  f"🔊 LUFS: {res.lufs:.1f}  •  {res.sample_rate} Hz  "
                  f"{'Stereo' if res.channels==2 else 'Mono'}\n"
                  f"↔  Crossfade suggerito: {cf_s:.1f}s\n"
                  f"💾 .npx v2: {npx_size//1024} KB ({res.frame_count} frame)"),
            fg=C['text'])


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    if HAS_DND:
        root = TkinterDnD.Tk()
    else:
        root = tk.Tk()

    app = NpxApp(root)

    # Center window
    root.update_idletasks()
    w, h = 1060, 680
    sw = root.winfo_screenwidth()
    sh = root.winfo_screenheight()
    root.geometry(f"{w}x{h}+{(sw-w)//2}+{(sh-h)//2}")

    root.mainloop()


if __name__ == '__main__':
    main()
