/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-pack-worker.js — the archive engine off the page's thread, for nucleo-pack.js. One request at a time per
// message id: { id, op: 'list' | 'open' | 'next' | 'close' | 'write' | 'version', ... } → { id, ok, result } or
// { id, ok: false, code, detail }. 'open' keeps a reader; each 'next' hands over one entry (its bytes transferred, not
// copied), so the page decides when the next one is decompressed.
import createLibarchive from './vendor/libarchive/libarchive.mjs';
import { createEngine } from './nucleo-pack-engine.js';

let engine = null;
const readers = new Map();
let seq = 0;

self.onmessage = async (ev) => {
  const { id, op, bytes, files, opts, handle } = ev.data || {};
  const reply = (result, transfer = []) => self.postMessage({ id, ok: true, result }, transfer);
  try {
    engine = engine || createEngine(await createLibarchive());
    switch (op) {
      case 'version': return reply(engine.version());
      case 'list': return reply(engine.list(bytes, opts));
      case 'open': { const h = ++seq; const rd = engine.reader(bytes, opts); readers.set(h, rd); return reply({ handle: h, info: rd.info }); }
      case 'next': {
        const rd = readers.get(handle);
        // a reader this worker does not hold (it was restarted): an error, never a silent end of the archive
        if (!rd) throw Object.assign(new Error('reader gone'), { code: 'aborted' });
        const e = rd.next();
        if (!e) readers.delete(handle);
        return reply(e, e && e.data ? [e.data.buffer] : []);
      }
      case 'close': { const rd = readers.get(handle); if (rd) rd.close(); readers.delete(handle); return reply(true); }
      case 'write': { const out = engine.write(files, opts); return reply(out, [out.buffer]); }
      default: throw Object.assign(new Error('unknown op'), { code: 'bad-input' });
    }
  } catch (err) {
    if (handle) { const rd = readers.get(handle); if (rd) rd.close(); readers.delete(handle); }
    self.postMessage({ id, ok: false, code: (err && err.code) || 'corrupt', detail: (err && (err.detail || err.message)) || String(err) });
  }
};
