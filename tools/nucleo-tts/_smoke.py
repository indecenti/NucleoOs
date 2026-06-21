#!/usr/bin/env python3
# _smoke.py — smoke-test della pipeline voce su SCRATCH (non tocca i pack reali):
#   edge-tts -> ffmpeg (trim+loudnorm) -> build_index (index.bin + clips.pcm). Usa le funzioni REALI.
import asyncio, os, sys, struct, argparse, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--word", default="prova")
    ap.add_argument("--lang", default="it")
    ap.add_argument("--edge-only", action="store_true", help="testa solo edge-tts (niente ffmpeg)")
    a = ap.parse_args()

    import edge_tts
    from gen_edge import VOICES
    voice = VOICES.get(a.lang)
    stage = os.path.join(HERE, "_smoke_wav", a.lang)
    out = os.path.join(HERE, "_smoke_out", a.lang)
    os.makedirs(stage, exist_ok=True)

    if a.edge_only:
        mp3 = os.path.join(stage, a.word + ".mp3")
        asyncio.run(edge_tts.Communicate(a.word, voice).save(mp3))
        print("EDGE OK: '%s' -> %d byte mp3" % (a.word, os.path.getsize(mp3)))
        return

    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg NON nel PATH di questo processo")
    from gen_edge import synth_clip
    wav = os.path.join(stage, a.word + ".wav")
    if not asyncio.run(synth_clip(a.word, voice, wav)):
        sys.exit("synth_clip FALLITO")
    print("SYNTH OK: '%s' -> %d byte wav (24kHz mono s16)" % (a.word, os.path.getsize(wav)))

    import build_index
    sys.argv = ["build_index", "--in", stage, "--out", out, "--rate", "24000"]
    build_index.main()

    idx = os.path.join(out, "index.bin"); pcm = os.path.join(out, "clips.pcm")
    h = open(idx, "rb").read(12)
    magic, rate, count = h[:4], struct.unpack("<I", h[4:8])[0], struct.unpack("<I", h[8:12])[0]
    print("PACK OK: magic=%r rate=%d count=%d  index=%dB clips=%dB" % (
        magic, rate, count, os.path.getsize(idx), os.path.getsize(pcm)))


if __name__ == "__main__":
    main()
