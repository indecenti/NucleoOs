#!/usr/bin/env python3
# build_index.py — packs a folder of <slug>.wav clips (24kHz mono 16-bit) into TWO files that
# the device reads efficiently: index.bin (slug->offset,len, SORTED) + clips.pcm (raw PCM
# concatenated). This way the Cardputer does NOT open 28k files in a FAT folder (slow, O(N)) but does a
# binary search in the index (RAM~0, ~15 reads). See nucleo_tts_index.h.
#
#   python build_index.py --in tools/nucleo-tts/_wav/it --out deploy/sd-safe/data/tts/it
import os, sys, argparse, struct, wave, glob

SLUG = 48
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="indir", required=True)
    ap.add_argument("--out", dest="outdir", required=True)
    ap.add_argument("--rate", type=int, default=24000)
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    wavs = glob.glob(os.path.join(args.indir, "*.wav"))
    # slug -> path; slug = file name without .wav, truncated to 47 (must match the planner)
    items = {}
    for p in wavs:
        slug = os.path.splitext(os.path.basename(p))[0][:SLUG-1]
        if slug: items[slug] = p
    slugs = sorted(items, key=lambda s: s.encode("ascii", "ignore"))   # byte-order == the device strcmp
    if not slugs: sys.exit("nessun .wav in %s" % args.indir)

    pcm_path = os.path.join(args.outdir, "clips.pcm")
    idx_path = os.path.join(args.outdir, "index.bin")
    records, off, bad = [], 0, 0
    with open(pcm_path, "wb") as pcm:
        for slug in slugs:
            try:
                w = wave.open(items[slug], "rb")
                if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != args.rate:
                    bad += 1; w.close(); continue            # non-canonical format -> discard (do not corrupt)
                data = w.readframes(w.getnframes()); w.close()
            except Exception:
                bad += 1; continue
            if not data: bad += 1; continue
            pcm.write(data)
            records.append((slug, off, len(data))); off += len(data)

    with open(idx_path, "wb") as ix:
        ix.write(b"NTI1"); ix.write(struct.pack("<II", args.rate, len(records)))
        for slug, o, l in records:
            ix.write(slug.encode("ascii", "ignore")[:SLUG-1].ljust(SLUG, b"\x00"))
            ix.write(struct.pack("<II", o, l))

    print("index: %d clip, %d scartate -> %s (%.1f MB) + %s (%.1f MB)" % (
        len(records), bad, idx_path, os.path.getsize(idx_path)/1e6, pcm_path, off/1e6))

if __name__ == "__main__":
    main()
