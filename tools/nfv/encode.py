#!/usr/bin/env python3
"""
NFV encoder — convert any video into a NucleoOS on-device clip.

Output is a pair of sibling files in /data/Videos:
  <name>.nfv   MJPEG frames (240x135) + a tiny header   -> native video app decodes with M5GFX drawJpg
  <name>.mp3   mono low-bitrate audio                   -> played by the existing Helix MP3 task

Why this shape (see docs/media.md + firmware/components/nucleo_audio):
  * The Cardputer (ESP32-S3, NO PSRAM) cannot real-time-decode H.264/H.265.
  * M5GFX already has a streaming JPEG decoder, so MJPEG costs ~zero new firmware.
  * The firmware already ships a robust async Helix MP3 task -> reuse it for audio.
  * Audio runs in its own task; video free-runs against the real-time clock. The bad
    little speaker makes sample-accurate lip-sync pointless, so loose sync is fine.

NFV1 container (little-endian), then frames back-to-back:
  off  type     field
  0    char[4]  "NFV1"
  4    u8       version (1)
  5    u8       flags        bit0 = a sibling .mp3 exists
  6    u16      width        (240)
  8    u16      height       (135)
  10   u16      fps
  12   u16      reserved
  14   u32      frame_count
  18   u32      duration_ms
  22   u32      max_frame_bytes   (so the player mallocs ONE buffer, no heap churn)
  26   u8[6]    reserved -> 32-byte header
  32   ...      frames: repeated [u32 size_le][jpeg bytes]

Frames are paced by the player at frame_idx/fps seconds; constant fps means no
in-RAM index is needed (a 2 h clip would otherwise need ~700 KB of index — more
than the whole SRAM budget). The player skip-reads to catch up if it falls behind.
"""
import argparse, os, struct, subprocess, sys, shutil

# Cardputer ST7789 panel (landscape, rotation 1). The panel is 240x135, but MJPEG with
# 4:2:0 chroma needs even dimensions, so we encode 240x136 and let the panel clip the last
# row. The player draws at y=0; M5GFX clips the overflow row automatically.
SCREEN_W, SCREEN_H = 240, 136

# Three export profiles, tuned for a 240x135 screen and a tinny mono speaker.
#   q  = ffmpeg MJPEG quality (2=best/big .. 31=worst/tiny); 5-9 is the useful band here
#   ar = audio sample rate (Hz); ab = audio bitrate
PROFILES = {
    # Smallest files, still watchable. Good default for very long films.
    "compat":   dict(fps=12, q=8, ar=22050, ab="32k"),
    # Best balance of motion vs. size; frames stay small (~5-6 KB) so the decoder has heap
    # headroom on the PSRAM-less board, and 15 fps reads noticeably smoother than 12.
    "balanced": dict(fps=15, q=7, ar=22050, ab="40k"),
    # Smoothest motion + crispest frames; biggest files (watch the heap on long clips).
    "quality":  dict(fps=20, q=5, ar=24000, ab="48k"),
}

SOI = b"\xff\xd8"   # JPEG start-of-image
EOI = b"\xff\xd9"   # JPEG end-of-image


def ffmpeg_bin():
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit("ffmpeg not found in PATH")
    return exe


def build_vf(fit):
    """Video filter: drop to target fps, fit into 240x135 with a sharp (lanczos) downscale."""
    if fit == "crop":
        # Fill the screen, crop overflow (good when content has dead borders).
        return (f"scale={SCREEN_W}:{SCREEN_H}:force_original_aspect_ratio=increase:flags=lanczos,"
                f"crop={SCREEN_W}:{SCREEN_H}")
    # pad (letterbox) — never lose picture, add black bars.
    return (f"scale={SCREEN_W}:{SCREEN_H}:force_original_aspect_ratio=decrease:flags=lanczos,"
            f"pad={SCREEN_W}:{SCREEN_H}:(ow-iw)/2:(oh-ih)/2:color=black")


def trim_args(ss, dur):
    a = []
    if ss:  a += ["-ss", str(ss)]
    if dur: a += ["-t", str(dur)]
    return a


def encode_audio(ff, src, out_mp3, ar, ab, ss=None, dur=None):
    cmd = [ff, "-y"] + trim_args(ss, dur) + ["-i", src, "-vn", "-ac", "1", "-ar", str(ar),
           "-c:a", "libmp3lame", "-b:a", ab, out_mp3]
    print("audio:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def encode_video_frames(ff, src, fps, q, fit, ss=None, dur=None, gpu=False):
    """Run ffmpeg, return the raw concatenated-MJPEG byte stream from stdout.

    With gpu=True the H.264/HEVC source is decoded on the NVIDIA GPU (NVDEC) and frames are
    downloaded to system memory for the software fps/scale/crop + MJPEG encode (NVENC has no
    MJPEG encoder, so the encode always stays on CPU — it's cheap at this tiny resolution)."""
    vf = f"fps={fps}," + build_vf(fit)
    hw = ["-hwaccel", "cuda"] if gpu else []
    cmd = [ff] + hw + trim_args(ss, dur) + ["-i", src, "-an", "-vf", vf,
           "-q:v", str(q), "-c:v", "mjpeg", "-f", "image2pipe", "pipe:1"]
    print("video:", " ".join(cmd))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)
    return p.stdout


def split_jpegs(blob):
    """Split a concatenated-MJPEG byte stream into individual JPEG frames.

    FF D8 FF only occurs at a JPEG SOI (inside entropy data FF is escaped as FF 00),
    so scanning for SOI markers is a safe frame split.
    """
    frames, i, n = [], 0, len(blob)
    start = blob.find(SOI)
    while start != -1:
        nxt = blob.find(SOI, start + 2)
        end = nxt if nxt != -1 else n
        frame = blob[start:end]
        # trim anything after the last EOI in this slice
        e = frame.rfind(EOI)
        if e != -1:
            frame = frame[:e + 2]
            frames.append(frame)
        start = nxt
    return frames


def write_nfv(out_nfv, frames, fps, has_audio):
    frame_count = len(frames)
    if frame_count == 0:
        sys.exit("no frames decoded — check the source file")
    max_frame = max(len(f) for f in frames)
    duration_ms = int(frame_count * 1000 / fps)
    # Sparse seek index: record the file offset of every Nth frame (~every 3 s) so the player
    # can jump to any time by reading ONE offset from the file then skip-reading <stride frames
    # — instant seek/resume with zero RAM index on the PSRAM-less device. Index lives at EOF.
    stride = max(1, round(fps * 3))
    with open(out_nfv, "wb") as f:
        f.write(b"\0" * 32)                          # placeholder header (patched at the end)
        offsets = []
        for i, fr in enumerate(frames):
            if i % stride == 0:
                offsets.append(f.tell())
            f.write(struct.pack("<I", len(fr)))
            f.write(fr)
        index_offset = f.tell()
        f.write(struct.pack("<I", len(offsets)))
        for off in offsets:
            f.write(struct.pack("<I", off))
        flags = (1 if has_audio else 0) | 2          # bit1 = has seek index
        header = struct.pack("<4sBBHHHHIIIIH",
                             b"NFV1", 2, flags, SCREEN_W, SCREEN_H, fps, stride,
                             frame_count, duration_ms, max_frame, index_offset, 0)
        assert len(header) == 32, len(header)
        f.seek(0)
        f.write(header)
    return frame_count, max_frame, duration_ms


def human(nbytes):
    for unit in ("B", "KB", "MB", "GB"):
        if nbytes < 1024 or unit == "GB":
            return f"{nbytes:.1f} {unit}"
        nbytes /= 1024


def main():
    ap = argparse.ArgumentParser(description="Encode a video to NucleoOS .nfv + .mp3")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="output .nfv path (default: alongside, .nfv)")
    ap.add_argument("-p", "--profile", choices=PROFILES, default="balanced")
    ap.add_argument("--fps", type=int)
    ap.add_argument("--q", type=int, help="MJPEG quality 2(best)..31(worst)")
    ap.add_argument("--ar", type=int, help="audio sample rate Hz")
    ap.add_argument("--ab", help="audio bitrate, e.g. 40k")
    # Default to crop (cover): fill the whole 240x135 panel, centred, no black bars. Use
    # --fit pad if you'd rather letterbox and keep every pixel of the source frame.
    ap.add_argument("--fit", choices=("pad", "crop"), default="crop")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--ss", help="start offset (seconds or HH:MM:SS) — trim")
    ap.add_argument("--duration", help="clip duration in seconds — trim")
    ap.add_argument("--gpu", action="store_true", help="decode on the NVIDIA GPU (NVDEC/cuda)")
    args = ap.parse_args()

    prof = dict(PROFILES[args.profile])
    if args.fps: prof["fps"] = args.fps
    if args.q:   prof["q"] = args.q
    if args.ar:  prof["ar"] = args.ar
    if args.ab:  prof["ab"] = args.ab

    src = args.input
    if not os.path.isfile(src):
        sys.exit(f"input not found: {src}")
    out_nfv = args.output or (os.path.splitext(src)[0] + ".nfv")
    out_mp3 = os.path.splitext(out_nfv)[0] + ".mp3"

    ff = ffmpeg_bin()
    print(f"== NFV encode: profile={args.profile} fps={prof['fps']} q={prof['q']} "
          f"audio={prof['ar']}Hz/{prof['ab']} fit={args.fit} ==")

    has_audio = not args.no_audio
    if has_audio:
        encode_audio(ff, src, out_mp3, prof["ar"], prof["ab"], args.ss, args.duration)

    blob = encode_video_frames(ff, src, prof["fps"], prof["q"], args.fit, args.ss, args.duration, args.gpu)
    frames = split_jpegs(blob)
    fc, maxf, dur = write_nfv(out_nfv, frames, prof["fps"], has_audio)

    vsize = os.path.getsize(out_nfv)
    asize = os.path.getsize(out_mp3) if has_audio and os.path.isfile(out_mp3) else 0
    print("\n== done ==")
    print(f"  {out_nfv}")
    print(f"     frames={fc}  fps={prof['fps']}  duration={dur/1000:.1f}s  "
          f"max_frame={human(maxf)}  size={human(vsize)}")
    if has_audio:
        print(f"  {out_mp3}\n     size={human(asize)}")
    print(f"  TOTAL on device: {human(vsize + asize)}")
    if maxf > 28000:
        print(f"  WARNING: max frame {human(maxf)} is large for the device heap "
              f"(~21 KB free block). Consider a higher --q (lower quality).")


if __name__ == "__main__":
    main()
