"""End-to-end: synth WAV -> analyse() -> write_npx() -> re-read the binary the
way the firmware does (nucleo_npx.c). Proves the header pack no longer crashes
and that beat/cue bytes decode to the right kick times & drop."""
import sys, struct, wave, tempfile, os
import numpy as np
sys.path.insert(0, ".")
import npx_gen as G
import _test_kick as T   # reuse the hardstyle synth

SR = T.SR


def main():
    np.random.seed(7)
    y, truth = T.synth()
    # write a real 16-bit mono WAV
    wav = os.path.join(tempfile.gettempdir(), "_npx_e2e.wav")
    npx = os.path.join(tempfile.gettempdir(), "_npx_e2e.npx")
    pcm = (np.clip(y, -1, 1) * 32767).astype("<i2")
    with wave.open(wav, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())

    res = G.analyse(wav)
    if res.error:
        print("ANALYSE ERROR:\n", res.error); return
    G.write_npx(res, npx)
    print(f"wrote {npx}: {os.path.getsize(npx)} bytes  "
          f"(hdr {G.NPX_HDR_SIZE} + {res.frame_count}x{G.NPX_FRM_SIZE})")

    # --- re-read exactly like the firmware loader ---
    with open(npx, "rb") as f:
        raw = f.read()
    hdr = struct.unpack(G.NPX_HDR_FMT, raw[:G.NPX_HDR_SIZE])
    (magic, ver, fps, ch, tonic, scale, _pad, bpm, dur, frames, sr, peak, lufs) = hdr
    assert magic == b"NPX1", magic
    assert ver == 2 and fps == G.FRAME_RATE
    print(f"header OK: ver={ver} fps={fps} ch={ch} bpm={bpm:.1f} "
          f"dur={dur:.1f}s frames={frames} sr={sr} lufs={lufs:.1f}")

    # decode beat bytes -> kick times (firmware npx_next_beat math)
    off = G.NPX_HDR_SIZE
    kick_times, drop_t = [], None
    for i in range(frames):
        rms, bass, mid, high, beat, cue, bright, = struct.unpack_from(
            G.NPX_FRM_FMT, raw, off + i * G.NPX_FRM_SIZE)
        if beat:
            kick_times.append((i + (beat - 1) / 254.0) / fps)
        if cue and drop_t is None:
            drop_t = i / fps

    truth_a = np.array(truth)
    errs = np.array([abs(truth_a[np.argmin(np.abs(truth_a - k))] - k)
                     for k in kick_times]) * 1000.0
    print(f"decoded kicks: {len(kick_times)} (truth {len(truth)})  "
          f"timing err ms mean={errs.mean():.1f} max={errs.max():.1f}")
    print(f"decoded drop:  {drop_t}s")
    ok = (abs(len(kick_times) - len(truth)) <= 2 and errs.max() < 30
          and drop_t is not None)
    print("E2E:", "PASS" if ok else "FAIL")
    for p in (wav, npx):
        try: os.remove(p)
        except OSError: pass


if __name__ == "__main__":
    main()
