#!/usr/bin/env python3
"""
NFV v4 — interleaved MJPEG + IMA-ADPCM for the NucleoOS Cardputer (ESP32-S3, no PSRAM).

WHY v4
------
v3 (tile-delta + an embedded MP3) hit three walls on the device, all measured:
  * TWO readers on one SD card: the MP3 task and the video loop. The card shares the SPI bus with the
    external panel, so the audio task could hold the file system while waiting for the bus the video loop
    held for the panel, and the video loop wait for the file system: a deadlock (picture freezes, keys dead).
  * Tiles: a frame's tiles came off the card one small read at a time and were painted one by one (the
    picture visibly "built up in blocks"), and the per-tile decoder (TJpgDec in LovyanGFX) cost ~3.5 ms per
    64x64 tile, ~65 ms for a whole 320x240 picture.
  * Helix MP3: ~20 KB of RAM and a task of its own.

v4 fixes them by construction:
  * ONE reader. Video frames and audio blocks are interleaved in one file, in presentation order, and the
    player reads it front to back in big sequential reads. Nothing else touches the card while it plays.
  * Whole frames. Every frame is a baseline 4:2:0 JPEG of the full picture, decoded by Espressif's
    esp_new_jpeg (SIMD on the S3: 320x240 at ~66 fps, ~10 KB of RAM) in 16-row strips that go straight to
    the panel. Every frame stands alone: a late frame is simply skipped, a seek lands on any frame.
  * IMA ADPCM audio (4 bits/sample, mono, 16 kHz = 8 KB/s), decoded with a few integer ops per sample and
    written to the I2S, whose DMA queue is the player's clock. No MP3 decoder, no audio task.

CONTAINER (little-endian). 64-byte header, then chunks, then the seek index.
  off  type     field
  0    char[4]  "NFV1"            (shared magic: the player sniffs the version byte)
  4    u8       version (4)
  5    u8       flags            bit0 = audio present, bit1 = video chunks carry a strip mask
  6    u16      width            multiple of 16 (4:2:0 MCU), e.g. 320 or 240
  8    u16      height           multiple of 16, e.g. 240 or 144
  10   u16      fps
  12   u16      audio_rate       Hz
  14   u8       audio_codec      1 = IMA ADPCM (WAV/Microsoft block layout), mono
  15   u8       reserved
  16   u32      frame_count
  20   u32      duration_ms
  24   u32      max_jpeg         largest frame (the player sizes ONE input buffer)
  28   u16      audio_block      ADPCM block size in bytes (all blocks but the last)
  30   u16      audio_block_samples
  32   u32      index_off
  36   u32      index_count
  40   u32      visible_w        the picture inside the coded frame (e.g. 240x135 in 240x144)
  44   u32      visible_h
  48   u8[16]   reserved
CHUNK: u8 type (1 = video, 2 = audio) | u8 0 | u16 mask | u32 len | u32 stamp | payload[len]
       stamp = frame index (video) or first sample index (audio)
       mask (video, flags bit1): bit k = the 16-row strip k changed since it was last shown. The player decodes
       the whole frame (~10 ms) but sends only those strips over the SPI bus — the bus, shared with the SD
       card, is the bottleneck (a full 320x240 frame is ~31 ms at 40 MHz). Every strip is still re-sent at
       least once per REFRESH_S, on its own phase, so nothing stays stale.
INDEX: index_count x { u32 frame_idx, u32 file_off }  one per second; file_off = the first chunk to read for
       that second (the audio block covering it, written up to LEAD before the frame — earlier frames are skipped)
Audio blocks are emitted ahead of the frame they accompany (LEAD), so the I2S queue never runs dry while
a frame decodes.
"""
from __future__ import annotations
import io, os, struct, subprocess, sys, shutil, tempfile
import numpy as np
from PIL import Image

CH_VIDEO, CH_AUDIO = 1, 2
LEAD_S = 0.25                       # audio queued ahead of the picture
REFRESH_S = 2.0                     # every strip re-sent at least this often
STRIP_MAD = 2.0                     # mean abs difference (0..255) that makes a strip "changed"
STRIP_PX, STRIP_FRAC = 18, 0.01     # ...or this share of its pixels moving more than STRIP_PX (local motion)


def ffmpeg():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("ffmpeg not found in PATH")
    return exe


def probe_frames(src, fps, ss, dur):
    try:
        out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", src],
                             capture_output=True, text=True).stdout.strip()
        d = float(out)
        if ss: d -= float(ss)
        if dur: d = min(d, float(dur))
        return max(1, int(d * fps))
    except Exception:
        return 0


def iter_frames(src, w, h, vis_w, vis_h, fps, fit, ss, dur):
    """RGB frames w x h; the picture is fitted into vis_w x vis_h and centred (black around)."""
    if fit == "crop":
        vf = (f"scale={vis_w}:{vis_h}:force_original_aspect_ratio=increase:flags=lanczos,"
              f"crop={vis_w}:{vis_h}")
    else:
        vf = (f"scale={vis_w}:{vis_h}:force_original_aspect_ratio=decrease:flags=lanczos,"
              f"pad={vis_w}:{vis_h}:(ow-iw)/2:(oh-ih)/2:color=black")
    vf += f",pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black,fps={fps}"
    cmd = [ffmpeg(), "-v", "error"]
    if ss: cmd += ["-ss", str(ss)]
    cmd += ["-i", src]
    if dur: cmd += ["-t", str(dur)]
    cmd += ["-map", "0:v:0", "-vf", vf, "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = w * h * 3
    try:
        while True:
            raw = p.stdout.read(n)
            if len(raw) < n:
                break
            yield np.frombuffer(raw, np.uint8).reshape(h, w, 3)
    finally:
        p.stdout.close(); p.wait()


def encode_jpeg(rgb, quality, cap):
    """Baseline 4:2:0 JPEG (optimised Huffman: still baseline). Over `cap` bytes: lower the quality."""
    img = Image.fromarray(rgb)
    q = quality
    while True:
        b = io.BytesIO()
        img.save(b, "JPEG", quality=q, subsampling=2, optimize=True, progressive=False)
        data = b.getvalue()
        if len(data) <= cap or q <= 20:
            return data, q
        q -= 6


LOUD_I, LOUD_TP, LOUD_LRA = -14.0, -1.5, 11.0    # every clip to the same loudness (EBU R128), see loudness_filter


def loudness_filter(src, ss, dur):
    """Two-pass EBU R128 normalisation (ffmpeg loudnorm): measure the clip, then apply a LINEAR gain to
    LOUD_I LUFS (true peak <= LOUD_TP). A quiet film comes up to the same level as a loud one without the
    pumping of a one-pass dynamic normaliser. -14 LUFS (the streaming level) suits a tiny speaker, and a full-
    scale signal also keeps the 4-bit ADPCM's quantisation noise further below the sound. None = no audio."""
    import json, re
    cmd = [ffmpeg(), "-hide_banner", "-nostats"]
    if ss: cmd += ["-ss", str(ss)]
    cmd += ["-i", src]
    if dur: cmd += ["-t", str(dur)]
    cmd += ["-map", "0:a:0", "-ac", "1", "-af", f"loudnorm=I={LOUD_I}:TP={LOUD_TP}:LRA={LOUD_LRA}:print_format=json",
            "-f", "null", "-"]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    m = re.search(r"\{[^{}]*\"input_i\"[^{}]*\}", r.stderr)
    if not m:
        return None
    j = json.loads(m.group(0))
    return (f"loudnorm=I={LOUD_I}:TP={LOUD_TP}:LRA={LOUD_LRA}:measured_I={j['input_i']}:measured_TP={j['input_tp']}"
            f":measured_LRA={j['input_lra']}:measured_thresh={j['input_thresh']}:offset={j['target_offset']}"
            f":linear=true"), float(j["input_i"])


def adpcm_blocks(src, rate, block, ss, dur, tmpdir, normalize=True):
    """IMA ADPCM (WAV layout) blocks of `block` bytes from the source's first audio track, mono."""
    wav = os.path.join(tmpdir, "a.wav")
    af = None
    if normalize:
        lf = loudness_filter(src, ss, dur)
        if lf:
            af, measured = lf
            print(f"  audio: {measured:.1f} LUFS -> {LOUD_I:.0f} LUFS")
    cmd = [ffmpeg(), "-v", "error", "-y"]
    if ss: cmd += ["-ss", str(ss)]
    cmd += ["-i", src]
    if dur: cmd += ["-t", str(dur)]
    cmd += ["-map", "0:a:0", "-ac", "1"]
    if af: cmd += ["-af", af]
    cmd += ["-ar", str(rate), "-c:a", "adpcm_ima_wav", "-block_size", str(block), wav]
    if subprocess.run(cmd).returncode != 0 or not os.path.exists(wav):
        return [], 0
    data = open(wav, "rb").read()
    # Walk the RIFF chunks: fmt (block align) and data.
    pos, align, payload = 12, block, b""
    while pos + 8 <= len(data):
        cid, clen = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + clen]
        if cid == b"fmt ":
            align = struct.unpack_from("<H", body, 12)[0]
        elif cid == b"data":
            payload = body
        pos += 8 + clen + (clen & 1)
    spb = (align - 4) * 2 + 1                       # samples per mono IMA block
    blocks = [payload[i:i + align] for i in range(0, len(payload), align)]
    return blocks, spb


def encode_v4(src, out_path, w=320, h=240, vis_w=None, vis_h=None, fps=15, quality=80, cap=24000,
              fit="crop", audio=True, rate=16000, block=256, ss=None, dur=None, progress=None, normalize=True):
    vis_w, vis_h = vis_w or w, vis_h or h
    assert w % 16 == 0 and h % 16 == 0, "coded size must be a multiple of 16 (4:2:0 MCU)"
    with tempfile.TemporaryDirectory() as td:
        ablocks, spb = adpcm_blocks(src, rate, block, ss, dur, td, normalize) if audio else ([], 0)
    total = probe_frames(src, fps, ss, dur)
    tmp = out_path + ".tmp"
    index, max_jpeg, nfr, qmin, ai = [], 0, 0, 100, 0
    masks_sent = []
    assert h // 16 <= 16, "strip mask is 16 bits: at most 256 rows"
    with open(tmp, "wb") as out:
        out.write(b"\0" * 64)

        def chunk(kind, stamp, payload, mask=0):
            off = out.tell()
            out.write(struct.pack("<BBHII", kind, 0, mask, len(payload), stamp))
            out.write(payload)
            return off

        aoffs = []                                  # file offset of every audio block (for the index)

        def audio_until(t_s):                       # every block that starts before t_s
            nonlocal ai
            while ai < len(ablocks) and ai * spb < t_s * rate:
                aoffs.append(chunk(CH_AUDIO, ai * spb, ablocks[ai])); ai += 1

        nstrips = h // 16
        shown = [None] * nstrips                    # what each strip last showed (source pixels)
        refresh = max(2, int(round(fps * REFRESH_S)))
        for frame in iter_frames(src, w, h, vis_w, vis_h, fps, fit, ss, dur):
            audio_until(nfr / fps + LEAD_S)
            jpg, q = encode_jpeg(frame, quality, cap)
            qmin = min(qmin, q)
            mask = 0
            for k in range(nstrips):
                cur = frame[k * 16:(k + 1) * 16]
                if shown[k] is None or (nfr - k * refresh // nstrips) % refresh == 0:
                    changed = True
                else:
                    diff = np.abs(cur.astype(np.int16) - shown[k].astype(np.int16))
                    changed = diff.mean() > STRIP_MAD or (diff.max(axis=2) > STRIP_PX).mean() > STRIP_FRAC
                if changed:
                    mask |= 1 << k
                    shown[k] = cur
            masks_sent.append(bin(mask).count("1"))
            off = chunk(CH_VIDEO, nfr, jpg, mask)
            if nfr % fps == 0:
                # A seek point must start reading early enough to meet the audio of this instant: its block
                # was written up to LEAD earlier than the frame. Point at whichever comes first.
                k = min(int(nfr / fps * rate) // spb, len(aoffs) - 1) if aoffs else -1
                index.append((nfr, min(off, aoffs[k]) if k >= 0 else off))
            max_jpeg = max(max_jpeg, len(jpg))
            nfr += 1
            if progress:
                progress(nfr, total)
        audio_until(1e9)
        index_off = out.tell()
        for fi, off in index:
            out.write(struct.pack("<II", fi, off))
        duration_ms = int(nfr * 1000 / fps)
        hdr = struct.pack("<4sBBHHHHBBIIIHHIIII", b"NFV1", 4, (1 if ablocks else 0) | 2, w, h, fps, rate if ablocks else 0,
                          1 if ablocks else 0, 0, nfr, duration_ms, max_jpeg, block, spb, index_off, len(index),
                          vis_w, vis_h)
        out.seek(0); out.write(hdr + b"\0" * (64 - len(hdr)))
    os.replace(tmp, out_path)
    return dict(frames=nfr, max_jpeg=max_jpeg, qmin=qmin, audio_blocks=len(ablocks), samples_per_block=spb,
                size=os.path.getsize(out_path), index=len(index),
                strips_sent=sum(masks_sent) / max(1, len(masks_sent)), strips=h // 16)


# ── reference reader (PC preview / tests): the same parsing the firmware does ─────────────────────────────
IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
            97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
            724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
            4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
            18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_decode_block(b):
    pred = struct.unpack_from("<h", b, 0)[0]; idx = b[2]
    out = [pred]
    for byte in b[4:]:
        for nib in (byte & 15, byte >> 4):
            step = IMA_STEP[idx]
            diff = step >> 3
            if nib & 1: diff += step >> 2
            if nib & 2: diff += step >> 1
            if nib & 4: diff += step
            pred = pred - diff if nib & 8 else pred + diff
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IMA_IDX[nib & 7]))
            out.append(pred)
    return out


def read_v4(path):
    with open(path, "rb") as f:
        h = f.read(64)
        (magic, ver, flags, w, hh, fps, rate, codec, _r, nfr, dur, mj, blk, spb, ioff, icnt, vw, vh) = \
            struct.unpack_from("<4sBBHHHHBBIIIHHIIII", h)
        assert magic == b"NFV1" and ver == 4
        hdr = dict(w=w, h=hh, fps=fps, rate=rate, codec=codec, frames=nfr, duration_ms=dur, max_jpeg=mj,
                   block=blk, spb=spb, index_off=ioff, index_count=icnt, vis_w=vw, vis_h=vh, flags=flags)
        chunks = []
        while f.tell() < ioff:
            ch = f.read(12)
            if len(ch) < 12: break
            kind, _, _, ln, stamp = struct.unpack("<BBHII", ch)
            chunks.append((kind, stamp, f.read(ln)))
        return hdr, chunks


def _main():
    import argparse
    ap = argparse.ArgumentParser(description="Encode a video to a NucleoOS NFV v4 clip (MJPEG + IMA ADPCM).")
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--target", choices=("external", "cardputer"), default="external",
                    help="external = 320x240 (the 2.8\" panel); cardputer = 240x135 (coded 240x144)")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--quality", type=int, default=80)
    ap.add_argument("--cap", type=int, default=24000, help="max bytes per frame (the device's one input buffer)")
    ap.add_argument("--fit", choices=("crop", "pad"), default="crop")
    ap.add_argument("--rate", type=int, default=16000)
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--no-normalize", action="store_true", help="keep the source loudness (default: -14 LUFS)")
    ap.add_argument("--ss"); ap.add_argument("--duration")
    a = ap.parse_args()
    if a.target == "external": w, h, vw, vh = 320, 240, 320, 240
    else: w, h, vw, vh = 240, 144, 240, 135
    out = a.output or (os.path.splitext(a.input)[0] + ".nfv")

    def prog(d, t):
        sys.stdout.write(f"\r  frame {d}" + (f"/{t}" if t else "")); sys.stdout.flush()
    st = encode_v4(a.input, out, w, h, vw, vh, a.fps, a.quality, a.cap, a.fit, not a.no_audio, a.rate,
                   ss=a.ss, dur=a.duration, progress=prog, normalize=not a.no_normalize)
    print(f"\n  {st['frames']} frames, max frame {st['max_jpeg']} B (min quality {st['qmin']}), "
          f"{st['strips_sent']:.1f}/{st['strips']} strips sent per frame, "
          f"{st['audio_blocks']} audio blocks x {st['samples_per_block']} samples, {st['size']/1024:.0f} KB")


if __name__ == "__main__":
    _main()
