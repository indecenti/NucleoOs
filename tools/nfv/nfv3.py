#!/usr/bin/env python3
"""
NFV v3 — tile-delta MJPEG container for the NucleoOS Cardputer.

WHY v3 EXISTS (the one idea that unlocks everything)
----------------------------------------------------
The Cardputer has NO PSRAM and ~18 KB of usable heap, so the video player draws each JPEG
frame DIRECTLY to the ST7789 panel (see app_video.cpp: `d.drawJpg(buf, len, 0, 0)`); there is
no full-frame buffer in ESP32 RAM. The crucial consequence: **the panel controller's own GRAM
already holds the previous frame for free.** A partial update — redraw only the rectangles that
changed — therefore costs ZERO extra RAM. This is what makes inter-frame coding, normally
impossible on this board, not just possible but *cheaper* on the heap than v2 (one small tile
buffer instead of one full-frame buffer).

So v3 splits each frame into a fixed grid of equal tiles and, per frame, emits only the tiles
that changed vs. the last time that tile was sent ("precompute which parts to redraw"). On a
talking head or claymation short, most tiles are static → most frames carry 1-4 tiny tiles.

THREE COMPRESSION LEVERS, none of which costs the device anything extra:
  1. Tile-delta: skip unchanged tiles (the big win on real content).
  2. Shared JPEG tables: every tile is encoded with the SAME quant + (standard) Huffman tables
     and the SAME 48x48 dimensions, so the ~600-byte JPEG header is identical for every tile.
     We store it ONCE; each tile carries only its entropy scan. The device rebuilds the exact
     original JPEG by `template + scan + FFD9` — byte-identical to what Pillow produced, so the
     panel's baseline decoder is guaranteed to accept it (proven by round-trip on PC).
  3. Panel-tuned quantization: standard JPEG quant tables are calibrated for 1:1 viewing on a
     monitor. On a 1.14" 240x135 panel the high-frequency DCT coefficients are invisible, so we
     scale them up aggressively — smaller files, no perceptible loss at that size.

Audio is embedded as the LAST section of the file (so the Helix MP3 decoder, which reads to EOF,
stops exactly at its end). One file per clip. Sample rate defaults far below v2 because the mono
NS4168 speaker has no usable response above ~6 kHz — 22050 Hz was paying for bandwidth the
hardware throws away.

CONTAINER (little-endian). Header is 64 bytes; magic stays "NFV1", version = 3.
  off  type      field
  0    char[4]   "NFV1"
  4    u8        version (3)
  5    u8        flags        bit0 = embedded audio present
  6    u16       width        visible width  (240)
  8    u16       height       visible height (135)
  10   u16       fps
  12   u16       coded_w      coded width  (240)
  14   u16       coded_h      coded height (144 — padded to a whole number of tile rows)
  16   u8        tile_w       (48)
  17   u8        tile_h       (48)
  18   u8        cols         (5)   coded_w / tile_w
  19   u8        rows         (3)   coded_h / tile_h   -> tile_count = cols*rows
  20   u32       frame_count
  24   u32       duration_ms
  28   u32       max_tile_bytes   largest tile scan (player sizes ONE reusable buffer)
  32   u32       template_off     file offset of the shared JPEG template
  36   u16       template_len
  38   u16       reserved
  40   u32       index_off        file offset of the keyframe seek index
  44   u32       index_count
  48   u32       audio_off        file offset of embedded MP3 (0 = none)
  52   u32       audio_len
  56   u8[8]     reserved -> 64 bytes

  [64 .. 64+template_len)     shared JPEG template (SOI .. end of SOS header)
  [.. )                       frames, back-to-back:
                                u8  n_tiles
                                n_tiles x { u8 tile_idx, u16 scan_len, u8[scan_len] scan }
                              a frame with n_tiles == tile_count is a KEYFRAME (self-contained).
  [index_off ..)              index_count x { u32 frame_idx, u32 file_off }  (keyframes only)
  [audio_off ..)              embedded MP3 (audio_len bytes) — LAST, so read-to-EOF == audio end

Frame 0 is always a keyframe; keyframes also recur every ~3 s (GOP) so seek/resume can jump to a
self-contained frame and replay the few deltas up to the target.
"""
from __future__ import annotations
import io, os, struct, subprocess, sys, shutil, tempfile

import numpy as np
from PIL import Image

# ── Panel geometry ────────────────────────────────────────────────────────────
VIS_W, VIS_H = 240, 135            # what the ST7789 actually shows
SOI = b"\xff\xd8"
EOI = b"\xff\xd9"

# ── Standard JPEG quantization tables (Annex K), NATURAL 8x8 order ──────────────
STD_LUMA = np.array([
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99,
], dtype=np.float64)
STD_CHROMA = np.array([
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
], dtype=np.float64)

# Natural-order -> zig-zag permutation (Pillow/libjpeg want qtables in zig-zag order).
ZIGZAG = np.array([
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
], dtype=np.int64)


def _scale_table(base: np.ndarray, quality: int, hf_boost: float) -> list:
    """Scale a standard quant table to `quality` (libjpeg formula), then push high frequencies
    up by `hf_boost` (0 = none) because the tiny panel can't resolve them. Returns zig-zag order."""
    q = max(1, min(100, quality))
    s = 5000 / q if q < 50 else 200 - 2 * q
    t = np.floor((base * s + 50) / 100)
    # radial frequency weight: corner (high-freq) coefficients get multiplied the most
    idx = np.arange(64)
    r = (idx // 8).astype(np.float64)
    c = (idx % 8).astype(np.float64)
    freq = np.sqrt(r * r + c * c) / np.sqrt(2 * 49)        # 0 at DC .. 1 at the HF corner
    t = t * (1.0 + hf_boost * freq)
    t = np.clip(np.round(t), 1, 255).astype(np.int64)
    return [int(x) for x in t[ZIGZAG]]                     # natural -> zig-zag for Pillow


def build_qtables(quality: int, hf_boost: float) -> list:
    return [_scale_table(STD_LUMA, quality, hf_boost),
            _scale_table(STD_CHROMA, quality, hf_boost)]


# ── Per-tile JPEG encode + shared-template surgery ──────────────────────────────

def _split_jpeg(data: bytes):
    """Return (template, scan) where template == bytes up to & incl the SOS header, scan == the
    entropy data, and template + scan + b'\\xff\\xd9' reconstructs `data` exactly."""
    if data[:2] != SOI or data[-2:] != EOI:
        raise ValueError("not a JFIF stream")
    i = 2
    n = len(data)
    while i < n:
        if data[i] != 0xFF:
            raise ValueError(f"expected marker at {i}")
        m = data[i + 1]
        if m == 0xDA:                                      # SOS: scan starts after its segment
            seg = (data[i + 2] << 8) | data[i + 3]
            scan_start = i + 2 + seg
            template = data[:scan_start]
            scan = data[scan_start:n - 2]                  # drop trailing FFD9
            assert template + scan + EOI == data
            return template, scan
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7 or m == 0x01:
            i += 2                                         # standalone marker, no length
        else:
            seg = (data[i + 2] << 8) | data[i + 3]
            i += 2 + seg
    raise ValueError("no SOS marker")


class TileCodec:
    """Encodes equal-size RGB tiles to JPEG with FIXED tables so every tile shares one header."""
    def __init__(self, tile_w, tile_h, quality, hf_boost, subsampling, grayscale):
        self.tw, self.th = tile_w, tile_h
        self.qtables = build_qtables(quality, hf_boost)
        self.subsampling = subsampling                     # 0=4:4:4 1=4:2:2 2=4:2:0
        self.grayscale = grayscale
        self.template = None                               # set on first encode; identical after

    def _encode_full(self, tile_rgb: np.ndarray) -> bytes:
        im = Image.fromarray(tile_rgb, "RGB")
        if self.grayscale:
            im = im.convert("L")
        buf = io.BytesIO()
        # optimize=False -> standard Huffman tables (identical for every tile, so shareable).
        kw = dict(format="JPEG", qtables=self.qtables, optimize=False)
        if not self.grayscale:
            kw["subsampling"] = self.subsampling
        im.save(buf, **kw)
        return buf.getvalue()

    def encode_scan(self, tile_rgb: np.ndarray) -> bytes:
        """Return the tile's entropy scan; lock in / verify the shared template on the way."""
        data = self._encode_full(tile_rgb)
        template, scan = _split_jpeg(data)
        if self.template is None:
            self.template = template
        elif template != self.template:
            # Should never happen with fixed tables + fixed dims; guards against a Pillow change.
            raise RuntimeError("tile JPEG header drifted — shared-template assumption broken")
        return scan

    def reassemble(self, scan: bytes) -> bytes:
        return self.template + scan + EOI


# ── Frame source: ffmpeg -> raw rgb24 -> padded numpy frames ────────────────────

def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        raise RuntimeError("ffmpeg not found in PATH")
    return exe


def build_vf(fit, fps, denoise):
    if fit == "crop":
        # explicit trunc() avoids 1-px vertical jitter on odd-dimension sources
        scale = (f"scale={VIS_W}:{VIS_H}:force_original_aspect_ratio=increase:flags=lanczos,"
                 f"crop={VIS_W}:{VIS_H}:trunc((iw-{VIS_W})/2):trunc((ih-{VIS_H})/2)")
    else:
        scale = (f"scale={VIS_W}:{VIS_H}:force_original_aspect_ratio=decrease:flags=lanczos,"
                 f"pad={VIS_W}:{VIS_H}:trunc((ow-iw)/2):trunc((oh-ih)/2):color=black")
    vf = f"fps={fps},{scale}"
    if denoise == "light":
        vf += ",hqdn3d=2:1.5:3:3"
    elif denoise == "strong":
        vf += ",hqdn3d=4:3:6:6"
    return vf


def iter_source_frames(src, fps, fit, denoise, tile_h, ss=None, dur=None, gpu=False, log=None):
    """Yield padded (coded_h, coded_w, 3) uint8 frames. coded_h is VIS_H rounded UP to tile_h."""
    ff = ffmpeg_bin()
    coded_h = ((VIS_H + tile_h - 1) // tile_h) * tile_h
    trim = []
    if ss:  trim += ["-ss", str(ss)]
    if dur: trim += ["-t", str(dur)]
    hw = ["-hwaccel", "cuda"] if gpu else []
    cmd = ([ff, "-hide_banner", "-loglevel", "error"] + hw + trim +
           ["-i", src, "-an", "-vf", build_vf(fit, fps, denoise),
            "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"])
    if log:
        log("$ " + " ".join(cmd))
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **flags)
    fsz = VIS_W * VIS_H * 3
    pad = coded_h - VIS_H
    try:
        while True:
            raw = proc.stdout.read(fsz)
            if len(raw) < fsz:
                break
            frame = np.frombuffer(raw, np.uint8).reshape(VIS_H, VIS_W, 3)
            if pad:                                        # replicate the last visible row downward
                frame = np.vstack([frame, np.repeat(frame[-1:], pad, axis=0)])
            yield np.ascontiguousarray(frame)
    finally:
        try:
            proc.stdout.close()
            err = proc.stderr.read().decode("utf-8", "replace")
            proc.wait()
            if proc.returncode not in (0, None) and log and err.strip():
                log(err.strip())
        except Exception:
            pass


def probe_frame_count(src, fps, ss=None, dur=None):
    """Best-effort total output frames, for progress reporting (0 if unknown)."""
    pb = shutil.which("ffprobe")
    if not pb:
        return 0
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
    try:
        r = subprocess.run([pb, "-v", "quiet", "-show_entries", "format=duration",
                            "-of", "default=nk=1:nw=1", src], capture_output=True, text=True, **flags)
        d = float(r.stdout.strip() or 0)
        if dur:
            d = min(d, float(dur))
        if ss:
            d = max(0.0, d - float(ss))
        return int(d * fps)
    except Exception:
        return 0


# ── Encoder ─────────────────────────────────────────────────────────────────────

class V3Opts:
    def __init__(self, fps=15, quality=70, hf_boost=1.4, fit="crop", denoise="light",
                 subsampling=2, grayscale=False, tile=48, gop_seconds=2.0, delta_thresh=6.0,
                 delta_px=14, delta_frac=0.03,
                 gpu=False, audio=True, audio_rate=16000, audio_kbps=24, ss=None, dur=None):
        self.fps = fps
        self.quality = quality               # 2..100-ish (Pillow-style); higher = better/bigger
        self.hf_boost = hf_boost             # extra high-frequency suppression for the tiny panel
        self.fit = fit                       # crop | pad
        self.denoise = denoise               # off | light | strong
        self.subsampling = subsampling       # 0=4:4:4 1=4:2:2 2=4:2:0
        self.grayscale = grayscale
        self.tile = tile                     # 16 or 48 (must divide 240 and tile the padded height)
        self.gop_seconds = gop_seconds       # forced-keyframe interval
        # A tile is "dirty" if its average changed (delta_thresh) OR enough pixels moved a lot
        # (delta_frac of pixels past delta_px). The 2nd test catches LOCALIZED motion — a face
        # drifting inside a big flat tile — that a pure mean would freeze until the next keyframe.
        self.delta_thresh = delta_thresh     # mean abs diff over a tile to call it "changed"
        self.delta_px = delta_px             # per-pixel (max channel) move that counts as motion
        self.delta_frac = delta_frac         # fraction of moved pixels that forces a refresh
        self.gpu = gpu
        self.audio = audio
        self.audio_rate = audio_rate
        self.audio_kbps = audio_kbps
        self.ss = ss
        self.dur = dur


def _tile_geometry(tile):
    if VIS_W % tile != 0:
        raise ValueError(f"tile {tile} does not divide width {VIS_W}")
    coded_h = ((VIS_H + tile - 1) // tile) * tile
    return tile, tile, VIS_W // tile, coded_h // tile, coded_h    # tw,th,cols,rows,coded_h


def encode_audio_blob(src, out_dir, rate, kbps, ss=None, dur=None, gpu=False, log=None) -> bytes:
    ff = ffmpeg_bin()
    tmp = os.path.join(out_dir, ".nfv3_audio_tmp.mp3")
    trim = []
    if ss:  trim += ["-ss", str(ss)]
    if dur: trim += ["-t", str(dur)]
    hw = ["-hwaccel", "cuda"] if gpu else []
    # -map_metadata -1 + -id3v2_version 0: bare MP3, no ID3 tag (see encode.py). The embedded audio
    # is seeked by a CBR byte map relative to audio_off; a leading ID3 tag would skew it.
    cmd = ([ff, "-hide_banner", "-loglevel", "error", "-y"] + hw + trim +
           ["-i", src, "-vn", "-ac", "1", "-ar", str(rate),
            "-c:a", "libmp3lame", "-b:a", f"{kbps}k", "-map_metadata", "-1", "-id3v2_version", "0", tmp])
    if log:
        log("$ " + " ".join(cmd))
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
    subprocess.run(cmd, check=True, **flags)
    with open(tmp, "rb") as f:
        blob = f.read()
    try:
        os.remove(tmp)
    except OSError:
        pass
    return blob


def encode_v3(src, out_path, opts: V3Opts, progress=None, abort=None, log=None):
    """Full pipeline: source -> tile-delta frames + (optional) embedded audio -> .nfv (v3).
    `progress(done, total)` is called per frame; `abort()` truthy aborts; `log(str)` for diagnostics.
    Returns a stats dict."""
    tw, th, cols, rows, coded_h = _tile_geometry(opts.tile)
    tile_count = cols * rows
    codec = TileCodec(tw, th, opts.quality, opts.hf_boost, opts.subsampling, opts.grayscale)

    total = probe_frame_count(src, opts.fps, opts.ss, opts.dur)
    gop = max(1, int(round(opts.fps * opts.gop_seconds)))

    # Audio first (small, and lets us fail fast before the long video pass).
    audio_blob = b""
    if opts.audio:
        try:
            audio_blob = encode_audio_blob(src, os.path.dirname(os.path.abspath(out_path)),
                                           opts.audio_rate, opts.audio_kbps, opts.ss, opts.dur,
                                           opts.gpu, log)
        except subprocess.CalledProcessError:
            audio_blob = b""                               # no audio track -> silent clip, fine

    # Stream frames straight to disk; never hold the whole clip in RAM.
    tmp_path = out_path + ".tmp"
    last_sent = [None] * tile_count                        # last SOURCE tile we emitted, per slot
    age = [0] * tile_count                                 # frames since each slot was last sent
    # No tile may go staler than ~half a GOP; the per-tile phase staggers these forced refreshes
    # across frames so they never land in lockstep (a synchronized refresh would itself look like a
    # snap). Result: by the time a keyframe re-sends everything, every tile is already current, so
    # the keyframe paints identical pixels and there is no periodic bob.
    refresh_age = max(2, gop // 2)
    frame_count = 0
    keyframes = []                                         # (frame_idx, file_off)
    max_tile = 0
    emitted_tiles = 0
    total_tiles_possible = 0

    with open(tmp_path, "wb") as out:
        out.write(b"\0" * 64)                              # header placeholder (patched at the end)
        # template is written after the first tile defines it; reserve nothing — we splice later.
        # To keep offsets simple we buffer frames into memory? No — we write template right after
        # the first keyframe is encoded, BEFORE any frame bytes. So: encode frame 0 fully first.
        frames_started = False
        template_off = template_len = 0
        frames_begin = 0

        for frame in iter_source_frames(src, opts.fps, opts.fit, opts.denoise, th,
                                        opts.ss, opts.dur, opts.gpu, log):
            if abort and abort():
                raise InterruptedError("aborted")
            is_key = (frame_count % gop == 0)
            # Decide dirty tiles
            dirty = []
            tiles_rgb = []
            for ti in range(tile_count):
                r, c = divmod(ti, cols)
                tile = frame[r * th:(r + 1) * th, c * tw:(c + 1) * tw]
                tiles_rgb.append(tile)
                if is_key or last_sent[ti] is None:
                    dirty.append(ti)
                else:
                    diff = np.abs(tile.astype(np.int16) - last_sent[ti].astype(np.int16))
                    mad = diff.mean()
                    moved = (diff.max(axis=2) > opts.delta_px).mean()   # localized-motion fraction
                    stale = age[ti] >= refresh_age + ((ti * 7) % refresh_age)
                    if mad > opts.delta_thresh or moved > opts.delta_frac or stale:
                        dirty.append(ti)
            total_tiles_possible += tile_count

            # Encode dirty tiles (this also locks the shared template on the very first one)
            encoded = []
            for ti in dirty:
                scan = codec.encode_scan(tiles_rgb[ti])
                encoded.append((ti, scan))
                last_sent[ti] = tiles_rgb[ti]
                if len(scan) > max_tile:
                    max_tile = len(scan)

            # Now that the template exists, flush it + remember where frames start (once).
            if not frames_started:
                template_off = out.tell()
                out.write(codec.template)
                template_len = len(codec.template)
                frames_begin = out.tell()
                frames_started = True

            this_off = out.tell()
            if is_key:
                keyframes.append((frame_count, this_off))
            out.write(struct.pack("<B", len(encoded)))
            dirty_set = set(dirty)
            for ti, scan in encoded:
                out.write(struct.pack("<BH", ti, len(scan)))
                out.write(scan)
            for ti in range(tile_count):               # age slots: reset the ones we just sent
                age[ti] = 0 if ti in dirty_set else age[ti] + 1
            emitted_tiles += len(encoded)
            frame_count += 1
            if progress:
                progress(frame_count, total)

        if frame_count == 0:
            raise RuntimeError("no frames decoded — check the source file / trim range")

        # ---- index + audio (audio LAST so the MP3 decoder's read-to-EOF stops at its end) ----
        index_off = out.tell()
        for fidx, foff in keyframes:
            out.write(struct.pack("<II", fidx, foff))
        index_count = len(keyframes)

        audio_off = audio_len = 0
        if audio_blob:
            audio_off = out.tell()
            out.write(audio_blob)
            audio_len = len(audio_blob)

        # ---- patch header ----
        duration_ms = int(frame_count * 1000 / opts.fps) if opts.fps else 0
        flags = 1 if audio_len else 0
        header = struct.pack("<4sBBHHHHHBBBBIIIIHHIIII",
                             b"NFV1", 3, flags,
                             VIS_W, VIS_H, opts.fps,
                             VIS_W, coded_h,
                             tw, th, cols, rows,
                             frame_count, duration_ms, max_tile,
                             template_off, template_len, 0,
                             index_off, index_count,
                             audio_off, audio_len)
        # pad to 64
        header += b"\0" * (64 - len(header))
        assert len(header) == 64, len(header)
        out.seek(0)
        out.write(header)

    os.replace(tmp_path, out_path)
    vsize = os.path.getsize(out_path)
    return dict(frames=frame_count, tile_count=tile_count, cols=cols, rows=rows,
                tile=opts.tile, coded_h=coded_h, duration_ms=duration_ms,
                emitted_tiles=emitted_tiles, total_tiles=total_tiles_possible,
                static_ratio=(1 - emitted_tiles / total_tiles_possible) if total_tiles_possible else 0,
                max_tile=max_tile, template_len=template_len,
                size=vsize, audio_len=audio_len, keyframes=index_count)


# ── Decoder / validator (host-side proof the device will render correctly) ──────

def read_header(path):
    with open(path, "rb") as f:
        head = f.read(64)
    if head[:4] != b"NFV1":
        raise ValueError("not an NFV file")
    ver = head[4]
    if ver != 3:
        return dict(version=ver)
    fmt = "<4sBBHHHHHBBBBIIIIHHIIII"
    (_, _, flags, w, h, fps, cw, ch, tw, th, cols, rows,
     fc, dur, maxt, toff, tlen, _r, ioff, icnt, aoff, alen) = struct.unpack(
        fmt, head[:struct.calcsize(fmt)])
    return dict(version=3, flags=flags, width=w, height=h, fps=fps, coded_w=cw, coded_h=ch,
                tile_w=tw, tile_h=th, cols=cols, rows=rows, frame_count=fc, duration_ms=dur,
                max_tile_bytes=maxt, template_off=toff, template_len=tlen,
                index_off=ioff, index_count=icnt, audio_off=aoff, audio_len=alen)


def decode_v3(path, limit=None):
    """Yield reconstructed (coded_h, coded_w, 3) uint8 frames, exactly as the device would paint
    them — reassembling each tile via the shared template and compositing onto a persistent canvas
    (the stand-in for the panel's GRAM)."""
    H = read_header(path)
    if H.get("version") != 3:
        raise ValueError("not a v3 clip")
    tw, th, cols, rows = H["tile_w"], H["tile_h"], H["cols"], H["rows"]
    cw, ch = H["coded_w"], H["coded_h"]
    with open(path, "rb") as f:
        f.seek(H["template_off"])
        template = f.read(H["template_len"])
        canvas = np.zeros((ch, cw, 3), np.uint8)
        f.seek(H["template_off"] + H["template_len"])
        # frames run until index_off
        end = H["index_off"]
        n = 0
        while f.tell() < end:
            ntiles = f.read(1)
            if not ntiles:
                break
            nt = ntiles[0]
            for _ in range(nt):
                ti, slen = struct.unpack("<BH", f.read(3))
                scan = f.read(slen)
                jpg = template + scan + EOI
                tile = np.asarray(Image.open(io.BytesIO(jpg)).convert("RGB"))
                r, c = divmod(ti, cols)
                canvas[r * th:(r + 1) * th, c * tw:(c + 1) * tw] = tile
            yield canvas.copy()
            n += 1
            if limit and n >= limit:
                return


def extract_audio(path, out_mp3):
    H = read_header(path)
    if H.get("version") != 3 or not H.get("audio_len"):
        return False
    with open(path, "rb") as f:
        f.seek(H["audio_off"])
        blob = f.read(H["audio_len"])
    with open(out_mp3, "wb") as f:
        f.write(blob)
    return True


# ── v2 reader (so we can benchmark v3 against existing real content) ─────────────

def read_v2_frames(path, limit=None):
    """Yield RGB frames from a v1/v2 .nfv (each frame is one full JPEG). For testing only."""
    with open(path, "rb") as f:
        head = f.read(32)
        if head[:4] != b"NFV1":
            raise ValueError("not NFV")
        fps = struct.unpack("<H", head[10:12])[0]
        frame_count = struct.unpack("<I", head[14:18])[0]
        f.seek(32)
        n = 0
        for _ in range(frame_count):
            sz = f.read(4)
            if len(sz) < 4:
                break
            s = struct.unpack("<I", sz)[0]
            jpg = f.read(s)
            if len(jpg) < s:
                break
            yield np.asarray(Image.open(io.BytesIO(jpg)).convert("RGB"))
            n += 1
            if limit and n >= limit:
                return


# ── Stateful readers — the firmware playback engine, in Python ──────────────────
# These mirror app_video.cpp exactly so a PC preview shows the SAME pixels the device paints:
# v3 composites tiles onto a persistent canvas (= the panel GRAM) and seeks via the keyframe
# index + delta replay; v2 decodes one independent full-frame JPEG per index.

class V3Reader:
    def __init__(self, path):
        self.path = path
        H = read_header(path)
        if H.get("version") != 3:
            raise ValueError("not a v3 clip")
        self.H = H
        self.fps = H["fps"] or 12
        self.frame_count = H["frame_count"]
        self.vis_w, self.vis_h = H["width"], H["height"]
        self.coded_w, self.coded_h = H["coded_w"], H["coded_h"]
        self.tw, self.th, self.cols, self.rows = H["tile_w"], H["tile_h"], H["cols"], H["rows"]
        self.tile_count = self.cols * self.rows
        self.duration_ms = H["duration_ms"]
        self.audio_off, self.audio_len = H["audio_off"], H["audio_len"]
        self.f = open(path, "rb")
        self.f.seek(H["template_off"]); self.template = self.f.read(H["template_len"])
        self.frames_begin = H["template_off"] + H["template_len"]
        self.index_off, self.index_count = H["index_off"], H["index_count"]
        self.canvas = np.zeros((self.coded_h, self.coded_w, 3), np.uint8)
        self.cur = -1
        self.next_to_read = 0

    def has_audio(self):
        return self.audio_len > 0

    def _seek_keyframe(self, target):
        off, kf = self.frames_begin, 0
        if self.index_count:
            lo, hi, a = 0, self.index_count - 1, 0
            while lo <= hi:
                mid = (lo + hi) // 2
                self.f.seek(self.index_off + mid * 8)
                fi = struct.unpack("<I", self.f.read(4))[0]
                if fi <= target: a = mid; lo = mid + 1
                else: hi = mid - 1
            self.f.seek(self.index_off + a * 8)
            kf, off = struct.unpack("<II", self.f.read(8))
        self.f.seek(off); self.next_to_read = kf
        return kf

    def _render_one(self):
        b = self.f.read(1)
        if not b:
            return False
        for _ in range(b[0]):
            hdr = self.f.read(3)
            if len(hdr) < 3:
                return False
            ti = hdr[0]; slen = hdr[1] | (hdr[2] << 8)
            scan = self.f.read(slen)
            if len(scan) < slen:
                return False
            tile = np.asarray(Image.open(io.BytesIO(self.template + scan + EOI)).convert("RGB"))
            r, c = divmod(ti, self.cols)
            self.canvas[r*self.th:(r+1)*self.th, c*self.tw:(c+1)*self.tw] = tile
        self.next_to_read += 1
        return True

    def frame_at(self, target):
        target = max(0, min(int(target), self.frame_count - 1))
        if target != self.cur:
            if self.cur < 0 or target < self.cur:
                self.cur = self._seek_keyframe(target) - 1
            while self.cur < target:
                if not self._render_one():
                    break
                self.cur += 1
        return self.canvas[:self.vis_h, :self.vis_w]

    def close(self):
        try: self.f.close()
        except Exception: pass


class V2Reader:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        head = self.f.read(32)
        if head[:4] != b"NFV1":
            raise ValueError("not an NFV file")
        self.fps = struct.unpack("<H", head[10:12])[0] or 12
        self.vis_w = struct.unpack("<H", head[6:8])[0] or 240
        coded_h = struct.unpack("<H", head[8:10])[0] or 136
        self.vis_h = min(135, coded_h)                    # panel shows 135, last row clipped
        self.frame_count = struct.unpack("<I", head[14:18])[0]
        self.duration_ms = struct.unpack("<I", head[18:22])[0]
        self.offsets = []                                 # byte offset of each [u32 size][jpeg]
        pos = 32
        for _ in range(self.frame_count):
            self.f.seek(pos)
            sz = self.f.read(4)
            if len(sz) < 4:
                break
            self.offsets.append(pos)
            pos += 4 + struct.unpack("<I", sz)[0]
        self.frame_count = len(self.offsets)
        self.audio_len = 0

    def has_audio(self):
        return False                                      # sibling .mp3 handled by the caller

    def frame_at(self, target):
        target = max(0, min(int(target), len(self.offsets) - 1))
        self.f.seek(self.offsets[target])
        s = struct.unpack("<I", self.f.read(4))[0]
        img = np.asarray(Image.open(io.BytesIO(self.f.read(s))).convert("RGB"))
        return img[:self.vis_h, :self.vis_w]

    def close(self):
        try: self.f.close()
        except Exception: pass


def open_nfv(path):
    """Return a stateful reader (V3Reader or V2Reader) for any .nfv, picking the version."""
    H = read_header(path)
    return V3Reader(path) if H.get("version") == 3 else V2Reader(path)


def audio_source(path, reader, tmp_dir):
    """Return a playable .mp3 path for `path` (extracting embedded v3 audio to tmp_dir), or None.
    Returns (mp3_path, is_temp): is_temp True means the caller should delete it when done."""
    if isinstance(reader, V3Reader) and reader.audio_len:
        out = os.path.join(tmp_dir, "nfv3_preview_audio.mp3")
        if extract_audio(path, out):
            return out, True
    sib = os.path.splitext(path)[0] + ".mp3"
    if os.path.isfile(sib):
        return sib, False
    return None, False


# ── CLI (the GUI is studio_gui.py; this is for scripting / batch jobs) ───────────

def _main():
    import argparse
    ap = argparse.ArgumentParser(description="Encode a video to a NucleoOS NFV v3 (tile-delta) clip.")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="output .nfv (default: alongside input)")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--quality", type=int, default=70, help="2..95 (higher = better/bigger)")
    ap.add_argument("--hf", type=float, default=1.4, help="panel high-frequency suppression 0..3")
    ap.add_argument("--fit", choices=("crop", "pad"), default="crop")
    ap.add_argument("--denoise", choices=("off", "light", "strong"), default="light")
    ap.add_argument("--tile", type=int, default=48, choices=(16, 48), help="tile size px")
    ap.add_argument("--grayscale", action="store_true")
    ap.add_argument("--gpu", action="store_true", help="NVIDIA NVDEC source decode")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--ar", type=int, default=16000, help="audio sample rate Hz")
    ap.add_argument("--ab", type=int, default=24, help="audio bitrate kbps")
    ap.add_argument("--ss", help="trim start (sec or HH:MM:SS)")
    ap.add_argument("--duration", help="trim duration")
    a = ap.parse_args()
    out = a.output or (os.path.splitext(a.input)[0] + ".nfv")
    opts = V3Opts(fps=a.fps, quality=a.quality, hf_boost=a.hf, fit=a.fit, denoise=a.denoise,
                  tile=a.tile, grayscale=a.grayscale, gpu=a.gpu, audio=not a.no_audio,
                  audio_rate=a.ar, audio_kbps=a.ab, ss=a.ss, dur=a.duration)
    def prog(d, t):
        sys.stdout.write(f"\r  frame {d}" + (f"/{t} ({d*100//t}%)" if t else "")); sys.stdout.flush()
    print(f"== NFV v3: {os.path.basename(a.input)} -> {out} ==")
    st = encode_v3(a.input, out, opts, progress=prog, log=lambda s: None)
    print(f"\n  {st['frames']} frames, {st['duration_ms']/1000:.1f}s, tile {a.tile}px "
          f"({st['cols']}x{st['rows']})")
    print(f"  static tiles skipped: {st['static_ratio']*100:.1f}%   keyframes: {st['keyframes']}")
    print(f"  size {st['size']/1024:.1f} KB (audio {st['audio_len']/1024:.1f} KB)   "
          f"device buffer {(st['max_tile']+st['template_len'])/1024:.1f} KB")


if __name__ == "__main__":
    _main()
