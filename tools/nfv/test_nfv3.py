#!/usr/bin/env python3
"""Host validation for the NFV v3 tile-delta format. Proves:
  1. Pillow accepts our custom (zig-zag) quant tables and round-trips them.
  2. The shared-template surgery is byte-exact (template + scan + FFD9 == original JPEG).
  3. A whole clip encodes, then decode_v3 reconstructs frames at high PSNR (so the device,
     which rebuilds the identical JPEG bytes, will paint the same pixels).
  4. Real-content compression vs the existing v2 clips, with the static-tile ratio.
No device needed. Run: python tools/nfv/test_nfv3.py
"""
import io, os, sys, struct, subprocess, tempfile
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nfv3

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")


def psnr(a, b):
    a = a.astype(np.float64); b = b.astype(np.float64)
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse < 1e-9 else 10 * np.log10(255 * 255 / mse)


def test_qtable_roundtrip():
    print("\n[1] quant-table ordering round-trip")
    qt = nfv3.build_qtables(quality=70, hf_boost=1.4)
    im = Image.fromarray((np.random.rand(48, 48, 3) * 255).astype(np.uint8), "RGB")
    buf = io.BytesIO()
    im.save(buf, format="JPEG", qtables=qt, subsampling=2, optimize=False)
    back = Image.open(io.BytesIO(buf.getvalue()))
    q = back.quantization
    # Pillow returns tables in the same (zig-zag) order we supplied; they must match exactly.
    ok0 = list(q[0]) == qt[0]
    ok1 = (1 not in q) or list(q[1]) == qt[1]
    print(f"    luma match={ok0}  chroma match={ok1}")
    assert ok0 and ok1, "qtable ordering mismatch — Pillow did not store what we supplied"
    # hf_boost must shrink files (suppresses the detail the panel can't show)
    g = np.tile(np.linspace(0, 255, 48).astype(np.uint8)[:, None, None], (1, 48, 3))
    sharp = (np.random.rand(48, 48, 3) * 255).astype(np.uint8)
    def sz(boost):
        c = nfv3.TileCodec(48, 48, 70, boost, 2, False)
        return len(c.encode_scan(sharp))
    lo, hi = sz(0.0), sz(2.5)
    print(f"    sharp-tile scan: hf_boost=0 -> {lo} B,  hf_boost=2.5 -> {hi} B  (smaller is good)")
    assert hi < lo, "hf_boost did not reduce high-frequency cost"
    print("    PASS")


def test_template_surgery():
    print("\n[2] shared-template byte-exactness across many tiles")
    codec = nfv3.TileCodec(48, 48, 70, 1.4, 2, False)
    for i in range(64):
        tile = (np.random.rand(48, 48, 3) * 255).astype(np.uint8)
        scan = codec.encode_scan(tile)
        full = codec._encode_full(tile)
        assert codec.reassemble(scan) == full, f"reassembly mismatch on tile {i}"
    print(f"    template_len={len(codec.template)} B, 64 tiles all reassembled byte-exact")
    print("    PASS")


def synth_frames(n, cw, ch):
    """A moving white box on a static gradient — most tiles static, a few change each frame."""
    base = np.tile(np.linspace(20, 90, cw).astype(np.uint8)[None, :, None], (ch, 1, 3))
    for i in range(n):
        f = base.copy()
        x = 8 + (i * 6) % (cw - 40)
        y = 30 + (i * 3) % (ch - 40)
        f[y:y + 28, x:x + 28] = 255
        yield np.ascontiguousarray(f)


def encode_from_frames(frames, out_path, opts, audio_blob=b""):
    """Mirror encode_v3's packing but drive it from an in-memory frame iterator (no ffmpeg)."""
    tw, th, cols, rows, coded_h = nfv3._tile_geometry(opts.tile)
    tile_count = cols * rows
    codec = nfv3.TileCodec(tw, th, opts.quality, opts.hf_boost, opts.subsampling, opts.grayscale)
    gop = max(1, int(round(opts.fps * opts.gop_seconds)))
    last_sent = [None] * tile_count
    keyframes = []; max_tile = 0; emitted = 0; possible = 0; frame_count = 0
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as out:
        out.write(b"\0" * 64)
        started = False; template_off = template_len = 0
        for frame in frames:
            is_key = (frame_count % gop == 0)
            dirty = []; tiles = []
            for ti in range(tile_count):
                r, c = divmod(ti, cols)
                t = frame[r*th:(r+1)*th, c*tw:(c+1)*tw]; tiles.append(t)
                if is_key or last_sent[ti] is None:
                    dirty.append(ti)
                elif np.mean(np.abs(t.astype(np.int16) - last_sent[ti].astype(np.int16))) > opts.delta_thresh:
                    dirty.append(ti)
            possible += tile_count
            enc = []
            for ti in dirty:
                scan = codec.encode_scan(tiles[ti]); enc.append((ti, scan))
                last_sent[ti] = tiles[ti]; max_tile = max(max_tile, len(scan))
            if not started:
                template_off = out.tell(); out.write(codec.template); template_len = len(codec.template); started = True
            off = out.tell()
            if is_key: keyframes.append((frame_count, off))
            out.write(struct.pack("<B", len(enc)))
            for ti, scan in enc:
                out.write(struct.pack("<BH", ti, len(scan))); out.write(scan)
            emitted += len(enc); frame_count += 1
        index_off = out.tell()
        for fi, fo in keyframes: out.write(struct.pack("<II", fi, fo))
        audio_off = audio_len = 0
        if audio_blob:
            audio_off = out.tell(); out.write(audio_blob); audio_len = len(audio_blob)
        dur = int(frame_count * 1000 / opts.fps)
        header = struct.pack("<4sBBHHHHHBBBBIIIIHHIIII", b"NFV1", 3, 1 if audio_len else 0,
                             nfv3.VIS_W, nfv3.VIS_H, opts.fps, nfv3.VIS_W, coded_h, tw, th, cols, rows,
                             frame_count, dur, max_tile, template_off, template_len, 0,
                             index_off, len(keyframes), audio_off, audio_len)
        header += b"\0" * (64 - len(header))
        out.seek(0); out.write(header)
    os.replace(tmp, out_path)
    return dict(frames=frame_count, emitted=emitted, possible=possible,
                static_ratio=1 - emitted/possible, size=os.path.getsize(out_path),
                template_len=template_len, max_tile=max_tile, keyframes=len(keyframes))


def test_synthetic_roundtrip():
    print("\n[3] synthetic clip: encode -> decode_v3 -> PSNR + static ratio")
    opts = nfv3.V3Opts(fps=15, quality=72, hf_boost=1.2, tile=48, audio=False)
    _, _, cols, rows, coded_h = nfv3._tile_geometry(opts.tile)
    src = list(synth_frames(45, nfv3.VIS_W, coded_h))
    out = os.path.join(tempfile.gettempdir(), "nfv3_synth.nfv")
    st = encode_from_frames(iter(src), out, opts)
    print(f"    frames={st['frames']} keyframes={st['keyframes']} "
          f"static_ratio={st['static_ratio']*100:.1f}% size={st['size']/1024:.1f} KB")
    dec = list(nfv3.decode_v3(out))
    assert len(dec) == len(src), f"frame count {len(dec)} != {len(src)}"
    ps = [psnr(a, b) for a, b in zip(src, dec)]
    mean = sum(ps) / len(ps)
    print(f"    reconstructed PSNR: min={min(ps):.1f} dB  mean={mean:.1f} dB")
    # A tile-placement / compositing bug tanks the whole clip to ~10-15 dB; the synthetic clip's
    # hard white-on-gradient edges legitimately cost a few frames ~24 dB even when correct.
    assert mean > 32 and min(ps) > 18, "reconstruction broken (tile compositing bug?)"
    assert st['static_ratio'] > 0.4, "tile-delta should skip most tiles on near-static content"
    print("    PASS")


def test_real_content():
    print("\n[4] REAL content vs v2 (decode a Wallace v2 clip, re-encode as v3)")
    clips = [f for f in os.listdir(OUT) if f.endswith(".nfv")] if os.path.isdir(OUT) else []
    pick = next((c for c in clips if "Wallace" in c or "Pantaloni" in c), clips[0] if clips else None)
    if not pick:
        print("    (no sample .nfv in out/ — skipping)")
        return
    v2path = os.path.join(OUT, pick)
    v2size = os.path.getsize(v2path)
    print(f"    source: {pick}  (v2 video {v2size/1024:.0f} KB)")
    # Decode up to ~600 frames of real motion for a representative measurement.
    frames = list(nfv3.read_v2_frames(v2path, limit=600))
    if not frames:
        print("    (could not decode v2 frames — skipping)"); return
    _, _, cols, rows, coded_h = nfv3._tile_geometry(48)
    def pad(fr):
        if fr.shape[0] < coded_h:
            fr = np.vstack([fr, np.repeat(fr[-1:], coded_h - fr.shape[0], 0)])
        return np.ascontiguousarray(fr[:coded_h, :nfv3.VIS_W])
    pframes = [pad(f) for f in frames]
    for label, q, boost, tile in [("v3 q72 t48", 72, 1.2, 48), ("v3 q60 hf+ t48", 60, 1.8, 48),
                                   ("v3 q68 t16", 68, 1.2, 16)]:
        opts = nfv3.V3Opts(fps=15, quality=q, hf_boost=boost, tile=tile, audio=False)
        _, _, c2, r2, ch2 = nfv3._tile_geometry(tile)
        pf = [np.ascontiguousarray(np.vstack([f[:nfv3.VIS_H], np.repeat(f[nfv3.VIS_H-1:nfv3.VIS_H], ch2-nfv3.VIS_H, 0)])) if ch2 > nfv3.VIS_H else f[:ch2] for f in pframes]
        out = os.path.join(tempfile.gettempdir(), f"nfv3_real_{tile}_{q}.nfv")
        st = encode_from_frames(iter(pf), out, opts)
        # v2 bytes for the SAME frame span (header + the size-prefixed frames we read)
        v2span = frame_span_v2_bytes(v2path, len(frames))
        dec = list(nfv3.decode_v3(out, limit=len(pf)))
        ps = sum(psnr(a[:nfv3.VIS_H], b[:nfv3.VIS_H]) for a, b in zip(pf, dec)) / len(dec)
        print(f"    {label:16s} static={st['static_ratio']*100:4.1f}%  "
              f"size={st['size']/1024:6.1f} KB  vs v2 {v2span/1024:6.1f} KB  "
              f"=> {st['size']/v2span*100:4.0f}% of v2   PSNR(vs v2-frame)={ps:.1f} dB")
    print("    NOTE: PSNR is vs the already-lossy v2 frames, so it understates true quality.")


def frame_span_v2_bytes(path, n):
    """Bytes the first n frames occupy in a v2 clip (header + size-prefixed JPEGs)."""
    with open(path, "rb") as f:
        f.seek(32); total = 32
        for _ in range(n):
            sz = f.read(4)
            if len(sz) < 4: break
            s = struct.unpack("<I", sz)[0]; f.seek(s, 1); total += 4 + s
    return total


if __name__ == "__main__":
    test_qtable_roundtrip()
    test_template_surgery()
    test_synthetic_roundtrip()
    test_real_content()
    print("\nAll host checks passed.")
