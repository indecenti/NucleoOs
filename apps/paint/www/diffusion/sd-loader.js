// sd-loader.js — reassembles chunked ONNX model files served from the Cardputer SD.
// Isolated from ANIMA; Paint owns this (no /apps/anima/ dependency).
//
// loadComponent({ base, parts, totalBytes?, readPart, onProgress? })
//   → Promise<{ ok:true, buffer:ArrayBuffer } | { ok:false, error, status? }>
//
//   base      : path prefix on the SD, e.g. '/apps/paint/models/.../fused.onnx'
//   parts     : number of chunks (files are base.000, base.001, …)
//   totalBytes: optional total-bytes hint for progress display (from manifest component.bytes)
//   readPart  : (path) → Promise<{ok:bool, status:number, bytes:Uint8Array}>
//               may be model-cache.js's wrap() for transparent local caching
//   onProgress: ({phase, index, total, bytes, totalBytes}) → void

const pad3 = n => String(n).padStart(3, '0');

export async function loadComponent({ base, parts, totalBytes, readPart, onProgress = () => {} } = {}) {
  const chunks = [];
  let soFar = 0;
  for (let i = 0; i < parts; i++) {
    const res = await readPart(`${base}.${pad3(i)}`);
    if (!res || !res.ok) return { ok: false, error: `part-${i}`, status: res && res.status };
    chunks.push(res.bytes);
    soFar += res.bytes.byteLength;
    onProgress({ phase: 'fetch', index: i + 1, total: parts, bytes: soFar, totalBytes });
  }
  const out = new Uint8Array(soFar);
  let off = 0;
  for (const ch of chunks) { out.set(ch, off); off += ch.byteLength; }
  return { ok: true, buffer: out.buffer };
}
