#!/usr/bin/env python3
"""
diag_drift.py — figure out WHERE the "image creeps up/down" in an .nfv comes from.

Three independent measurements, each isolating one suspect:

  [A] Player faithfulness   — does V3Reader (what the preview/firmware run) produce the
                              EXACT pixels of the reference decoder decode_v3()? If yes, the
                              player is innocent: any motion you see is in the stored frames.
  [B] Vertical drift        — best vertical pixel-shift aligning frame N -> N+1, accumulated.
                              Tells real content motion from a systematic creep.
  [C] Tile-delta staleness  — keyframe "snap": how much the picture jumps when a GOP boundary
                              re-sends every tile. A big periodic snap = visible bobbing every
                              GOP, caused by the change-threshold freezing tiles too long.

Usage:  python diag_drift.py <clip.nfv> [num_frames]
"""
import sys, os, io, struct
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nfv3


def best_vshift(a, b, rng=8):
    """Vertical integer shift (px) that best aligns b onto a. +ve => b moved DOWN vs a."""
    A = a.astype(np.float32).mean(2)
    B = b.astype(np.float32).mean(2)
    H = A.shape[0]
    best_s, best_e = 0, 1e30
    for s in range(-rng, rng + 1):
        if s >= 0:
            aa, bb = A[s:H], B[0:H - s]
        else:
            aa, bb = A[0:H + s], B[-s:H]
        if aa.shape[0] < 4:
            continue
        e = float(np.mean(np.abs(aa - bb)))
        if e < best_e:
            best_e, best_s = e, s
    return best_s


def parse_frame_tilesets(path):
    """Return (header, list-of-sets): which tile indices each frame transmits."""
    H = nfv3.read_header(path)
    sets = []
    with open(path, "rb") as f:
        f.seek(H["template_off"] + H["template_len"])
        end = H["index_off"]
        while f.tell() < end:
            nb = f.read(1)
            if not nb:
                break
            n = nb[0]
            s = set()
            for _ in range(n):
                hdr = f.read(3)
                if len(hdr) < 3:
                    break
                ti = hdr[0]; slen = hdr[1] | (hdr[2] << 8)
                f.read(slen)
                s.add(ti)
            sets.append(s)
    return H, sets


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    path = sys.argv[1]
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 200

    H = nfv3.read_header(path)
    if H.get("version") != 3:
        print(f"[!] {path} is not a v3 clip (version={H.get('version')}) — drift test is v3-only.")
        sys.exit(2)
    fps = H["fps"]; fc = H["frame_count"]
    N = min(N, fc)
    print(f"== {os.path.basename(path)} ==")
    print(f"   {H['width']}x{H['height']}  coded {H['coded_w']}x{H['coded_h']}  "
          f"tile {H['tile_w']}x{H['tile_h']} ({H['cols']}x{H['rows']})  "
          f"{fps}fps  {fc} frames  analyzing {N}")

    # ---- [A] player faithfulness: V3Reader vs decode_v3 ------------------------------
    print("\n[A] Player faithfulness (V3Reader == reference decoder)")
    ref = []
    for i, fr in enumerate(nfv3.decode_v3(path, limit=N)):
        ref.append(fr[:H["height"], :H["width"]].copy())
    rdr = nfv3.V3Reader(path)
    seq_mis = 0
    for i in range(len(ref)):
        got = rdr.frame_at(i)
        if not np.array_equal(ref[i], got):
            seq_mis += 1
    # random seeks (exercise keyframe + delta replay path)
    rng = np.random.default_rng(12345)
    jumps = [int(x) for x in rng.integers(0, len(ref), size=min(20, len(ref)))]
    seek_mis = 0
    for t in jumps:
        if not np.array_equal(ref[t], rdr.frame_at(t)):
            seek_mis += 1
    rdr.close()
    print(f"    sequential mismatches: {seq_mis}/{len(ref)}")
    print(f"    random-seek mismatches: {seek_mis}/{len(jumps)}")
    verdict_player = "INNOCENT — pixels are byte-identical to the reference" if (seq_mis == 0 and seek_mis == 0) \
        else "SUSPECT — reader diverges from reference decoder!"
    print(f"    -> player: {verdict_player}")

    # ---- [B] vertical drift in the stored frames -------------------------------------
    print("\n[B] Vertical drift in stored frames (frame N -> N+1)")
    shifts = []
    for i in range(1, len(ref)):
        shifts.append(best_vshift(ref[i - 1], ref[i]))
    shifts = np.array(shifts)
    cum = np.cumsum(shifts)
    nz = int(np.count_nonzero(shifts))
    print(f"    per-frame shift  mean={shifts.mean():+.3f}px  std={shifts.std():.3f}  "
          f"nonzero={nz}/{len(shifts)}")
    print(f"    net drift over {len(ref)} frames: {cum[-1]:+d}px   (range {cum.min():+d}..{cum.max():+d})")
    bias = shifts.mean()
    if abs(bias) > 0.15:
        verdict_drift = f"SYSTEMATIC CREEP {bias:+.2f}px/frame — encoder/scale, baked into the file"
    elif shifts.std() > 1.2:
        verdict_drift = "JITTERY (high variance, ~zero mean) — sub-pixel scale wobble / shimmer"
    else:
        verdict_drift = "stable — vertical motion is genuine content, not an artifact"
    print(f"    -> {verdict_drift}")

    # ---- [C] tile-delta staleness: keyframe snap -------------------------------------
    print("\n[C] Tile-delta staleness (keyframe snap = jump when a GOP re-sends all tiles)")
    Hh, sets = parse_frame_tilesets(path)
    tile_count = Hh["cols"] * Hh["rows"]
    full = [i for i, s in enumerate(sets[:N]) if len(s) == tile_count]
    gop = (full[1] - full[0]) if len(full) >= 2 else None
    # per-boundary picture change, split into keyframe vs non-keyframe boundaries
    diffs = []
    for i in range(1, len(ref)):
        diffs.append(float(np.mean(np.abs(ref[i].astype(np.int16) - ref[i - 1].astype(np.int16)))))
    diffs = np.array(diffs)
    keyset = set(full)
    key_b = [diffs[i - 1] for i in range(1, len(ref)) if i in keyset]
    non_b = [diffs[i - 1] for i in range(1, len(ref)) if i not in keyset]
    # flip-flop: tiles updated, skipped, then updated again within a short window
    flips = 0
    last_state = [False] * tile_count
    run_skip = [0] * tile_count
    for s in sets[:N]:
        for ti in range(tile_count):
            if ti in s:
                if last_state[ti] is False and 0 < run_skip[ti] <= 3:
                    flips += 1
                last_state[ti] = True; run_skip[ti] = 0
            else:
                last_state[ti] = False; run_skip[ti] += 1
    emitted = sum(len(s) for s in sets[:N])
    print(f"    keyframe interval (GOP): {gop} frames  ({gop/fps:.1f}s)" if gop else "    GOP: unknown")
    print(f"    static ratio: {(1 - emitted/(tile_count*N))*100:.1f}%  "
          f"(emitted {emitted}/{tile_count*N} tiles)")
    if key_b and non_b:
        km, nm = float(np.mean(key_b)), float(np.mean(non_b))
        ratio = km / nm if nm else 0
        print(f"    mean picture change at keyframe boundary: {km:.2f}")
        print(f"    mean picture change at normal boundary:   {nm:.2f}")
        print(f"    snap ratio (key/normal): {ratio:.2f}x   flip-flop tile updates: {flips}")
        if ratio > 1.8:
            verdict_tile = (f"STALENESS — every {gop/fps:.1f}s the picture jumps {ratio:.1f}x harder "
                            f"than usual. This is the visible bob. Shorten GOP / lower threshold.")
        elif flips > N * 0.5:
            verdict_tile = "SHIMMER — many tiles flip-flop around the threshold (patchwork updates)."
        else:
            verdict_tile = "tile-delta healthy — keyframes don't cause a visible jump."
        print(f"    -> {verdict_tile}")

    # ---- [D] is the creep in the SOURCE too? (decisive: content vs pipeline) ---------
    verdict_src = None
    src = sys.argv[3] if len(sys.argv) > 3 else None
    if src and os.path.isfile(src):
        print(f"\n[D] Same-window drift straight from source ({os.path.basename(src)})")
        srcf = []
        for fr in nfv3.iter_source_frames(src, fps, H_fit(H), "off", H["tile_h"]):
            srcf.append(fr[:H["height"], :H["width"]].copy())
            if len(srcf) >= len(ref):
                break
        sh = np.array([best_vshift(srcf[i - 1], srcf[i]) for i in range(1, len(srcf))])
        scum = int(np.cumsum(sh)[-1]) if len(sh) else 0
        print(f"    source per-frame shift mean={sh.mean():+.3f}px  net drift {scum:+d}px over {len(srcf)} frames")
        if abs(sh.mean() - bias) < 0.15:
            verdict_src = "creep is IN THE SOURCE — genuine camera/content motion, NOT a bug"
        elif abs(sh.mean()) < 0.15 < abs(bias):
            verdict_src = "source is STABLE but the .nfv creeps — PIPELINE BUG (scale/fps/tiling)"
        else:
            verdict_src = f"source mean={sh.mean():+.2f} vs nfv mean={bias:+.2f} — partial pipeline contribution"
        print(f"    -> {verdict_src}")

    print("\n== CONCLUSION ==")
    print(f"  player    : {verdict_player}")
    print(f"  drift     : {verdict_drift}")
    if verdict_src:
        print(f"  drift src : {verdict_src}")
    if key_b and non_b:
        print(f"  tile-delta: {verdict_tile}")


def H_fit(H):
    # we don't store fit in the header; crop is the default the GUI uses for these clips
    return "crop"


if __name__ == "__main__":
    main()
