#!/usr/bin/env python3
# Add a seek index to OLD NFV v1 clips, in place — so the device resumes/seeks INSTANTLY without
# the slow on-device "Preparing..." scan (a 125 k-frame clip = ~3 min of tiny SD reads on the
# Cardputer; the same chain-walk on a PC is a few seconds).
#
# The encoder (encode.py) already writes the index for new clips (v2). This upgrades clips made
# before that — it does NOT re-encode: it walks the existing [u32 size][JPEG] frame chain once,
# APPENDS the sparse offset table at EOF, and rewrites only the 32-byte header (flags bit1, stride,
# index_offset). Byte-for-byte identical to what encode.py would have produced.
#
# Run it on the SD card's Videos folder (put the card in a reader; big-file Wi-Fi upload to the
# device is unreliable anyway — copy via SD):
#   python tools/nfv/reindex.py X:\data\Videos          # all *.nfv in the folder
#   python tools/nfv/reindex.py "X:\data\Videos\Film.nfv"
#   python tools/nfv/reindex.py X:\data\Videos --dry-run
#
# Safe: skips clips that already have an index; verifies the chain reaches EOF cleanly before
# touching anything; writes the index first, patches the header last (so a crash can't leave a
# clip claiming an index it doesn't have).

import os, sys, struct, glob

HDR = struct.Struct("<4sBBHHHHIIIIH")   # magic,ver,flags,W,H,fps,stride,frames,dur_ms,maxf,idx_off,_

def reindex(path, dry=False):
    size = os.path.getsize(path)
    with open(path, "r+b") as f:
        raw = f.read(32)
        if len(raw) != 32:
            return f"skip (too small): {path}"
        magic, ver, flags, W, H, fps, stride, frames, dur, maxf, idx_off, _ = HDR.unpack(raw)
        if magic != b"NFV1":
            return f"skip (not NFV1): {path}"
        if (flags & 2) and idx_off:
            return f"skip (already indexed, v{ver}): {os.path.basename(path)}"
        if not fps or not frames:
            return f"skip (bad header fps/frames): {path}"

        stride = max(1, round(fps * 3))
        offsets = []
        pos = 32
        for i in range(frames):
            if i % stride == 0:
                offsets.append(pos)
            f.seek(pos)
            b = f.read(4)
            if len(b) != 4:
                return f"ABORT (frame {i}/{frames} truncated): {os.path.basename(path)}"
            (fsz,) = struct.unpack("<I", b)
            pos += 4 + fsz
            if pos > size:
                return f"ABORT (frame {i} runs past EOF): {os.path.basename(path)}"
        if pos != size:
            return f"ABORT (chain ends at {pos}, file is {size} — trailing data?): {os.path.basename(path)}"

        index_offset = pos                      # == EOF: index goes here
        if dry:
            return f"would index {os.path.basename(path)}: {frames} frames, stride {stride}, {len(offsets)} entries (+{4 + 4*len(offsets)} B)"

        # Append the index, THEN patch the header (crash-safe ordering).
        f.seek(index_offset)
        f.write(struct.pack("<I", len(offsets)))
        f.write(b"".join(struct.pack("<I", o) for o in offsets))
        f.flush()
        new_hdr = HDR.pack(b"NFV1", 2, flags | 2, W, H, fps, stride, frames, dur, maxf, index_offset, 0)
        f.seek(0)
        f.write(new_hdr)
        f.flush()
        os.fsync(f.fileno())
        return f"OK  {os.path.basename(path)}: indexed {frames} frames, {len(offsets)} entries"

def main(argv):
    args = [a for a in argv if not a.startswith("-")]
    dry = "--dry-run" in argv or "-n" in argv
    if not args:
        print("usage: reindex.py <file.nfv | folder> [--dry-run]")
        return 2
    target = args[0]
    files = ([target] if target.lower().endswith(".nfv")
             else sorted(glob.glob(os.path.join(target, "*.nfv"))))
    if not files:
        print(f"no .nfv files under {target}")
        return 1
    for p in files:
        try:
            print(reindex(p, dry))
        except Exception as e:
            print(f"ERROR {os.path.basename(p)}: {e}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
