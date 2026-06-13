"""Generate a real .npx from the hardstyle synth, for the JS loader test."""
import sys, os
sys.path.insert(0, ".")
import numpy as np
import npx_gen as G
import _test_kick as T

np.random.seed(7)
y, truth = T.synth()
import wave, tempfile
wav = os.path.join(tempfile.gettempdir(), "_npx_sample.wav")
pcm = (np.clip(y, -1, 1) * 32767).astype("<i2")
with wave.open(wav, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(T.SR)
    w.writeframes(pcm.tobytes())
res = G.analyse(wav)
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_sample.npx")
G.write_npx(res, out)
print(f"wrote {out}  bpm={res.bpm:.1f} kicks={res.kick_count} "
      f"drop={res.drop_s}s key={res.key_tonic}/{res.key_scale}")
os.remove(wav)
