// sd-model-loader.js — reassemble a large model component cached on the Cardputer SD from its 7 MB parts
// (basename.000, .001, …) back into a single ArrayBuffer the inference runtime can consume. This is the
// generalised form of voice.js:assembleParts() (the Vosk rig): the device httpd resets on sustained
// multi-MB reads, so every model bigger than ~7 MB is stored split and stitched in the browser. Reused here
// to feed Stable Diffusion XS ONNX components (text-encoder / controlnet / unet / vae) into onnxruntime-web.
//
// Differences from the Vosk loader, all to make a 100+ MB model safe and resumable:
//   - manifest-driven part count (no fragile fetch-until-404 that stops at the first gap);
//   - per-part retry with exponential backoff (survives an httpd reset mid-stream);
//   - optional per-part SHA-256 check (rejects a corrupt/partial part instead of feeding bad bytes to WebGPU);
//   - onProgress(part, total, bytes) so the UI can show "componente 8/15".
// Pure & DOM-free → host-testable. The concrete reader (/api/fs/read) is injected.

const PART_RETRIES = 4;
const BACKOFF_MS = [0, 400, 800, 1200, 1600];

export function partName(base, i) { return `${base}.${String(i).padStart(3, '0')}`; }

// loadComponent({ base, parts, bytes?, partSha?, readPart, sha256?, onProgress, sleep })
//   base      : SD path of the component without the .NNN suffix (e.g. /apps/paint/www/models/sdxs/unet.onnx)
//   parts     : number of parts (manifest-driven). If omitted, discover by reading until a part 404s.
//   bytes     : total expected bytes (optional — pre-allocates exactly; else grows from parts).
//   partSha   : optional [sha256,…] per part for integrity (length must equal parts when given).
//   readPart  : async (path) -> { ok, status, bytes:Uint8Array }   (the ONLY transport; wraps /api/fs/read)
//   sha256    : async (Uint8Array) -> hex                          (required only if partSha is given)
//   onProgress: ({ part, parts, bytes, total }) -> void
//   sleep     : async (ms) -> void                                 (injected; real default below)
// Returns { ok:true, buffer:ArrayBuffer, bytes } | { ok:false, error, part? }.
export async function loadComponent({
  base, parts, bytes, partSha, readPart, sha256, onProgress, sleep,
} = {}) {
  if (typeof readPart !== 'function') throw new Error('readPart must be injected');
  if (partSha && partSha.length && typeof sha256 !== 'function') throw new Error('sha256 required when partSha given');
  const nap = typeof sleep === 'function' ? sleep : (ms) => new Promise((r) => setTimeout(r, ms));
  const emit = typeof onProgress === 'function' ? onProgress : () => {};

  const chunks = [];
  let totalBytes = 0;
  const limit = Number.isInteger(parts) && parts > 0 ? parts : Infinity;

  for (let i = 0; i < limit; i++) {
    const path = partName(base, i);
    let got = null, lastStatus = 0;
    for (let attempt = 0; attempt < PART_RETRIES; attempt++) {
      if (attempt) await nap(BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)]);
      const res = await readPart(path).catch(() => null);
      if (res && res.ok && res.bytes) { got = res.bytes; break; }
      lastStatus = (res && res.status) || 0;
      if (lastStatus === 404) break;   // a 404 is "no more parts", not a transient error → stop retrying
    }
    if (!got) {
      if (lastStatus === 404) {
        if (limit === Infinity) break;                                   // discovery mode: clean end of parts
        return { ok: false, error: 'missing-part', part: i, path };      // manifest mode: a declared part is gone
      }
      return { ok: false, error: 'read-failed:' + lastStatus, part: i, path };
    }
    if (partSha && partSha[i]) {
      const d = await sha256(got);
      if (d !== partSha[i]) return { ok: false, error: 'sha-mismatch', part: i, path };
    }
    chunks.push(got);
    totalBytes += got.length;
    emit({ part: i + 1, parts: Number.isInteger(parts) ? parts : i + 1, bytes: totalBytes, total: bytes || 0 });
  }

  if (!chunks.length) return { ok: false, error: 'no-parts', path: partName(base, 0) };

  // Stitch the parts into one contiguous buffer (ORT wants a single ArrayBuffer/Uint8Array).
  const out = new Uint8Array(bytes && bytes >= totalBytes ? bytes : totalBytes);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return { ok: true, buffer: out.buffer, bytes: off };
}
