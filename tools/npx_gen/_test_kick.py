"""Smoke test: synth a hardstyle-ish track (150 BPM, brick-walled kicks +
pitched sub, a breakdown, then a drop) and check detect_kicks / detect_drop /
the sub-frame beat encoding against ground truth. No real audio needed."""
import sys, struct
import numpy as np
sys.path.insert(0, ".")
import npx_gen as G

SR = 44100
BPM = 150.0
BEAT = 60.0 / BPM            # 0.4 s
DUR = 24.0                    # seconds


def hard_kick(n, sr):
    """A punchy kick: click transient + decaying pitched sub (~50 Hz)."""
    t = np.arange(n) / sr
    sub_f = 150.0 * np.exp(-t * 30.0) + 48.0          # pitch envelope
    phase = 2 * np.pi * np.cumsum(sub_f) / sr
    body = np.sin(phase) * np.exp(-t * 8.0)
    click = (np.random.randn(n) * np.exp(-t * 120.0)) * 0.6
    k = body + click
    return k / (np.max(np.abs(k)) + 1e-9)


def synth():
    n = int(DUR * SR)
    y = np.zeros(n, dtype=np.float32)
    klen = int(0.18 * SR)
    kick = hard_kick(klen, SR).astype(np.float32)
    truth = []
    t = 0.30                                          # first kick at 300 ms
    while t < DUR - 0.2:
        # breakdown 9.0..13.0 s: no kick (drop lands right after)
        in_break = 9.0 <= t < 13.0
        if not in_break:
            i = int(t * SR)
            m = min(klen, n - i)
            y[i:i+m] += kick[:m]
            truth.append(round(t, 4))
        t += BEAT
    # light pad OUT of the bass band (900 Hz) so the breakdown sub is truly empty
    tt = np.arange(n) / SR
    y += (0.05 * np.sin(2*np.pi*900*tt) * np.sin(2*np.pi*0.3*tt)).astype(np.float32)
    # brick-wall limiter (the hardstyle master killer for broadband detection)
    y = np.tanh(y * 3.0)
    y = (y / (np.max(np.abs(y)) + 1e-9)).astype(np.float32)
    return y, truth


def main():
    np.random.seed(7)
    y, truth = synth()
    kicks, bpm = G.detect_kicks(y, SR, DUR, 0.0)
    print(f"ground-truth kicks: {len(truth)}   detected: {len(kicks)}")
    print(f"ground-truth BPM:   {BPM}          detected: {bpm}")

    # Match each detected kick to nearest truth; report timing error.
    truth_a = np.array(truth)
    errs = []
    for k in kicks:
        j = int(np.argmin(np.abs(truth_a - k)))
        errs.append(abs(truth_a[j] - k))
    errs = np.array(errs) * 1000.0  # ms
    if errs.size:
        print(f"timing error ms:    mean={errs.mean():.1f}  median={np.median(errs):.1f}  max={errs.max():.1f}")

    # Recall: how many true kicks have a detection within 60 ms.
    kk = np.array(kicks)
    hit = sum(1 for tg in truth if kk.size and np.min(np.abs(kk - tg)) < 0.06)
    print(f"recall:             {hit}/{len(truth)} = {100*hit/len(truth):.0f}%")

    # False kicks inside the breakdown (9..13 s) — must be ZERO.
    fp = sum(1 for k in kicks if 9.0 <= k < 13.0)
    print(f"phantom kicks in breakdown: {fp}")

    # Sub-frame encoding round-trip: pick a kick, encode like analyse(), decode.
    if kicks:
        kt = kicks[len(kicks)//2]
        fi = int(kt * G.FRAME_RATE)
        frac = kt * G.FRAME_RATE - fi
        beat_val = 1 + int(round(254 * frac))
        beat_val = max(1, min(255, beat_val))
        decoded_t = (fi + (beat_val - 1) / 254.0) / G.FRAME_RATE
        print(f"sub-frame round-trip: kick={kt:.4f}s  byte={beat_val}  "
              f"decoded={decoded_t:.4f}s  err={abs(decoded_t-kt)*1000:.2f}ms")

    # Drop detection on the sub envelope (mirror analyse()).
    hop = max(1, SR // G.FRAME_RATE)
    Sx = np.abs(__import__("librosa").stft(y, n_fft=min(2048, hop*2), hop_length=hop)) ** 2
    bass = G.freq_band_energy(Sx, SR, *G.BASS_HZ)
    bass_n = np.clip(bass / (np.max(bass)+1e-9), 0, 1)
    df = G.detect_drop(bass_n, float(G.FRAME_RATE))
    print(f"drop frame: {df}  ->  {None if df is None else round(df/G.FRAME_RATE,2)}s "
          f"(expected ~13s)")


if __name__ == "__main__":
    main()
