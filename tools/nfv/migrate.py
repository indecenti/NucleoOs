#!/usr/bin/env python3
"""
Turn an old .nfv (v1/v2: one full JPEG per frame, the sound in a sibling .mp3) into an NFV v4 clip.

The Cardputer's player reads v4 only. A clip whose source video is gone can still be carried over: its frames
are already JPEGs, so they are handed to ffmpeg as an MJPEG stream (no re-decode on our side), muxed with the
.mp3, and the result goes through the normal v4 encoder (nfv4.py): same strip masks, same IMA ADPCM audio
normalised to the same loudness as every other clip. The picture stays at its old 240x135 (upscaling a
240-wide clip only blurs it); on the external panel the player centres it.

  python tools/nfv/migrate.py "Old clip.nfv" [--mp3 "Old clip.mp3"] [-o "Old clip (v4).nfv"]
"""
from __future__ import annotations
import argparse, os, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nfv4  # noqa: E402


def old_frames(path):
    """(fps, iterator of JPEG bytes) of a v1/v2 .nfv: 32-byte header, then [u32 size][JPEG] per frame."""
    f = open(path, "rb")
    head = f.read(32)
    if head[:4] != b"NFV1" or head[4] not in (1, 2):
        f.close()
        raise SystemExit(f"{path}: not a v1/v2 .nfv (version {head[4] if len(head) > 4 else '?'})")
    fps = struct.unpack_from("<H", head, 10)[0] or 12
    count = struct.unpack_from("<I", head, 14)[0]

    def it():
        with f:
            for _ in range(count):
                sz = f.read(4)
                if len(sz) < 4:
                    return
                n = struct.unpack("<I", sz)[0]
                jpg = f.read(n)
                if len(jpg) < n or jpg[:2] != b"\xff\xd8":
                    return
                yield jpg
    return fps, it()


def migrate(src, out, mp3=None, fps_out=15, quality=80):
    fps, frames = old_frames(src)
    with tempfile.TemporaryDirectory() as td:
        mj = os.path.join(td, "v.mjpeg")
        n = 0
        with open(mj, "wb") as o:
            for jpg in frames:
                o.write(jpg); n += 1
        mkv = os.path.join(td, "v.mkv")
        cmd = [nfv4.ffmpeg(), "-v", "error", "-y", "-f", "mjpeg", "-framerate", str(fps), "-i", mj]
        if mp3:
            cmd += ["-i", mp3, "-map", "0:v", "-map", "1:a"]
        cmd += ["-c", "copy", mkv]
        subprocess.run(cmd, check=True)
        st = nfv4.encode_v4(mkv, out, w=240, h=144, vis_w=240, vis_h=135, fps=min(fps, fps_out), quality=quality,
                            audio=bool(mp3), progress=lambda d, t: (sys.stdout.write(f"\r  frame {d}/{t or '?'}"), sys.stdout.flush()))
    print(f"\n  {os.path.basename(src)}: {n} old frames @ {fps} fps -> {st['frames']} @ {min(fps, fps_out)} fps, "
          f"{st['audio_blocks']} audio blocks, {st['size'] / 1048576:.1f} MB")
    return st


def _main():
    ap = argparse.ArgumentParser(description="Convert an old v1/v2 .nfv (+ sibling .mp3) to NFV v4.")
    ap.add_argument("input")
    ap.add_argument("--mp3", help="the clip's sound (default: the sibling .mp3, if any)")
    ap.add_argument("-o", "--output")
    ap.add_argument("--fps", type=int, default=15, help="at most this many fps (the device shows ~15)")
    ap.add_argument("--quality", type=int, default=80)
    a = ap.parse_args()
    mp3 = a.mp3 or (os.path.splitext(a.input)[0] + ".mp3")
    if not os.path.isfile(mp3):
        if a.mp3:
            raise SystemExit(f"{mp3}: not found")
        mp3 = None
    out = a.output or (os.path.splitext(a.input)[0] + " (v4).nfv")
    if os.path.abspath(out) == os.path.abspath(a.input):
        raise SystemExit("output would overwrite the input")
    migrate(a.input, out, mp3, a.fps, a.quality)


if __name__ == "__main__":
    _main()
