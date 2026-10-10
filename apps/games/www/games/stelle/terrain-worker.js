// stelle/terrain-worker.js — builds terrain chunks and flora cells off the main thread (module worker).
// The main thread lends its typed arrays with every request (transferred, not copied) and gets them back filled:
// in the steady state nothing is allocated on either side. The maths lives in chunk.js / planet.js (pure, tested).
import { buildChunk } from './chunk.js';
import { cubeDir, scatterFlora } from './planet.js';

const surfs = new Map(), C = [0, 0, 0];
self.onmessage = (ev) => {
  const m = ev.data;
  if (m.op === 'init') { surfs.set(m.S.key, m.S); return; }
  if (m.op === 'drop') { surfs.delete(m.key); return; }
  const S = surfs.get(m.key);
  if (m.op === 'chunk') {
    const out = m.bufs;
    if (!S) { self.postMessage({ op: 'chunk', id: m.id, ok: false, bufs: out }, [out.pos.buffer, out.nrm.buffer, out.mor.buffer, out.srf.buffer]); return; }
    const info = buildChunk(S, m.f, m.L, m.x, m.y, out);
    self.postMessage({ op: 'chunk', id: m.id, ok: true, info, bufs: out }, [out.pos.buffer, out.nrm.buffer, out.mor.buffer, out.srf.buffer]);
  } else if (m.op === 'flora') {
    const buf = m.buf;
    if (!S) { self.postMessage({ op: 'flora', id: m.id, ok: false, n: 0, buf }, [buf.buffer]); return; }
    const n = 1 << m.L; cubeDir(m.f, -1 + 2 * (m.x + 0.5) / n, -1 + 2 * (m.y + 0.5) / n, C);
    const cx = C[0] * S.R, cy = C[1] * S.R, cz = C[2] * S.R;
    const cnt = scatterFlora(S, m.f, m.L, m.x, m.y, m.density, buf, cx, cy, cz);
    self.postMessage({ op: 'flora', id: m.id, ok: true, n: cnt, cx, cy, cz, buf }, [buf.buffer]);
  }
};
