// imaging.js — Paint's pixel-processing core. PURE & DOM-free (operates on {data:Uint8ClampedArray, width,
// height} RGBA images), so every operation is deterministic and host-testable (tools/anima-host/
// paint-imaging.test.mjs). The canvas/UI layer in index.html wraps these with getImageData/putImageData.
//
// Highlights the user asked for: real PNG transparency (alpha is first-class everywhere), background removal
// (edge-flood chroma key with feathering + a click "magic wand"), and a full set of evolved-editor ops
// (adjustments, convolution filters, transforms, transparent-trim).

const img = (data, width, height) => ({ data, width, height });
const clamp = (v) => (v < 0 ? 0 : v > 255 ? 255 : v) | 0;
export function colorDist2(d, i, r, g, b) { const dr = d[i] - r, dg = d[i + 1] - g, db = d[i + 2] - b; return dr * dr + dg * dg + db * db; }
export function hexToRgb(h) { const n = parseInt(String(h).replace('#', ''), 16); return [n >> 16 & 255, n >> 8 & 255, n & 255]; }

// ---- background removal -----------------------------------------------------------------------------------

// Estimate the background colour as the average of the border ring (robust for product/generated images).
export function estimateBackground({ data, width: w, height: h }) {
  let r = 0, g = 0, b = 0, n = 0;
  const add = (i) => { r += data[i]; g += data[i + 1]; b += data[i + 2]; n++; };
  for (let x = 0; x < w; x++) { add((x) * 4); add(((h - 1) * w + x) * 4); }
  for (let y = 0; y < h; y++) { add((y * w) * 4); add((y * w + w - 1) * 4); }
  return n ? [Math.round(r / n), Math.round(g / n), Math.round(b / n)] : [255, 255, 255];
}

// Remove the background by flooding inward from EVERY border pixel through pixels within `tolerance` of the
// background colour, setting their alpha to 0. Connected-from-edge only, so a same-colour region INSIDE the
// subject is preserved. `feather` softens the alpha along the new boundary (anti-aliased cut-out).
// tolerance: 0..255 (perceptual-ish, compared in squared RGB). Returns a NEW image (original untouched).
export function removeBackgroundAuto(src, { tolerance = 48, feather = 1, bg = null } = {}) {
  const { data, width: w, height: h } = src;
  const out = new Uint8ClampedArray(data);
  const [br, bg2, bb] = bg || estimateBackground(src);
  const tol2 = tolerance * tolerance * 3;            // squared distance budget across 3 channels
  const seen = new Uint8Array(w * h);
  const stack = [];
  const pushEdge = (x, y) => { const p = y * w + x; if (!seen[p]) { seen[p] = 1; stack.push(p); } };
  for (let x = 0; x < w; x++) { pushEdge(x, 0); pushEdge(x, h - 1); }
  for (let y = 0; y < h; y++) { pushEdge(0, y); pushEdge(w - 1, y); }
  // re-seed: a border pixel is only a true seed if it IS background; non-bg border pixels stop the flood.
  while (stack.length) {
    const p = stack.pop(), i = p * 4;
    if (colorDist2(data, i, br, bg2, bb) > tol2) continue;   // not background → don't remove, don't expand
    out[i + 3] = 0;                                           // transparent
    const x = p % w, y = (p / w) | 0;
    if (x > 0 && !seen[p - 1]) { seen[p - 1] = 1; stack.push(p - 1); }
    if (x < w - 1 && !seen[p + 1]) { seen[p + 1] = 1; stack.push(p + 1); }
    if (y > 0 && !seen[p - w]) { seen[p - w] = 1; stack.push(p - w); }
    if (y < h - 1 && !seen[p + w]) { seen[p + w] = 1; stack.push(p + w); }
  }
  if (feather > 0) featherAlpha(out, w, h, feather);
  return img(out, w, h);
}

// Click "magic wand": flood from (sx,sy) through pixels within `tolerance` of the SEED colour, alpha→0.
export function magicWandErase(src, sx, sy, { tolerance = 40, feather = 1 } = {}) {
  const { data, width: w, height: h } = src;
  const out = new Uint8ClampedArray(data);
  sx = clamp(sx) % w | 0; sy = (sy < 0 ? 0 : sy >= h ? h - 1 : sy) | 0;
  const si = (sy * w + sx) * 4, sr = data[si], sg = data[si + 1], sb = data[si + 2];
  const tol2 = tolerance * tolerance * 3;
  const seen = new Uint8Array(w * h), stack = [sy * w + sx];
  seen[sy * w + sx] = 1;
  while (stack.length) {
    const p = stack.pop(), i = p * 4;
    if (colorDist2(data, i, sr, sg, sb) > tol2) continue;
    out[i + 3] = 0;
    const x = p % w, y = (p / w) | 0;
    if (x > 0 && !seen[p - 1]) { seen[p - 1] = 1; stack.push(p - 1); }
    if (x < w - 1 && !seen[p + 1]) { seen[p + 1] = 1; stack.push(p + 1); }
    if (y > 0 && !seen[p - w]) { seen[p - w] = 1; stack.push(p - w); }
    if (y < h - 1 && !seen[p + w]) { seen[p + w] = 1; stack.push(p + w); }
  }
  if (feather > 0) featherAlpha(out, w, h, feather);
  return img(out, w, h);
}

// Global chroma key: every pixel within `tolerance` of `color` (hex or [r,g,b]) becomes transparent.
export function colorToAlpha(src, color, { tolerance = 40 } = {}) {
  const { data, width: w, height: h } = src;
  const out = new Uint8ClampedArray(data);
  const [r, g, b] = Array.isArray(color) ? color : hexToRgb(color);
  const tol2 = tolerance * tolerance * 3;
  for (let i = 0; i < out.length; i += 4) if (colorDist2(data, i, r, g, b) <= tol2) out[i + 3] = 0;
  return img(out, w, h);
}

// Soften the alpha boundary: any pixel adjacent to a fully-transparent one gets the 3×3 average alpha.
function featherAlpha(d, w, h, radius) {
  for (let r = 0; r < radius; r++) {
    const a = new Uint8ClampedArray(w * h);
    for (let i = 0; i < w * h; i++) a[i] = d[i * 4 + 3];
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const p = y * w + x; if (a[p] === 0) continue;
      let edge = false;
      if (x > 0 && a[p - 1] === 0) edge = true; if (x < w - 1 && a[p + 1] === 0) edge = true;
      if (y > 0 && a[p - w] === 0) edge = true; if (y < h - 1 && a[p + w] === 0) edge = true;
      if (!edge) continue;
      let s = 0, n = 0;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        const xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
        s += a[yy * w + xx]; n++;
      }
      d[p * 4 + 3] = (s / n) | 0;
    }
  }
}

// ---- adjustments (all return a NEW image; alpha preserved) ------------------------------------------------

// brightness/contrast/saturation in [-100,100]; gamma > 0 (1 = no-op).
export function adjust(src, { brightness = 0, contrast = 0, saturation = 0, gamma = 1 } = {}) {
  const { data, width: w, height: h } = src; const out = new Uint8ClampedArray(data);
  const c = (contrast / 100) + 1, cf = c * c;                       // perceptual-ish contrast curve
  const sb = brightness * 2.55, sat = (saturation / 100) + 1, ig = 1 / (gamma || 1);
  for (let i = 0; i < out.length; i += 4) {
    let r = data[i], g = data[i + 1], b = data[i + 2];
    r = (r - 128) * cf + 128 + sb; g = (g - 128) * cf + 128 + sb; b = (b - 128) * cf + 128 + sb;
    if (sat !== 1) { const gray = 0.299 * r + 0.587 * g + 0.114 * b; r = gray + (r - gray) * sat; g = gray + (g - gray) * sat; b = gray + (b - gray) * sat; }
    if (ig !== 1) { r = 255 * Math.pow(Math.max(0, r) / 255, ig); g = 255 * Math.pow(Math.max(0, g) / 255, ig); b = 255 * Math.pow(Math.max(0, b) / 255, ig); }
    out[i] = clamp(r); out[i + 1] = clamp(g); out[i + 2] = clamp(b);
  }
  return img(out, w, h);
}
export function grayscale(src) { const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data);
  for (let i = 0; i < out.length; i += 4) { const v = clamp(0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2]); out[i] = out[i + 1] = out[i + 2] = v; } return img(out, w, h); }
export function invert(src) { const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data);
  for (let i = 0; i < out.length; i += 4) { out[i] = 255 - data[i]; out[i + 1] = 255 - data[i + 1]; out[i + 2] = 255 - data[i + 2]; } return img(out, w, h); }
export function sepia(src) { const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data);
  for (let i = 0; i < out.length; i += 4) { const r = data[i], g = data[i + 1], b = data[i + 2];
    out[i] = clamp(0.393 * r + 0.769 * g + 0.189 * b); out[i + 1] = clamp(0.349 * r + 0.686 * g + 0.168 * b); out[i + 2] = clamp(0.272 * r + 0.534 * g + 0.131 * b); } return img(out, w, h); }

// 3×3 convolution (alpha preserved). Presets below.
export function convolve(src, kernel, { divisor = null, offset = 0 } = {}) {
  const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data);
  const div = divisor != null ? divisor : (kernel.reduce((a, b) => a + b, 0) || 1);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    let r = 0, g = 0, b = 0, k = 0;
    for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++, k++) {
      const xx = Math.min(w - 1, Math.max(0, x + dx)), yy = Math.min(h - 1, Math.max(0, y + dy)), j = (yy * w + xx) * 4, kv = kernel[k];
      r += data[j] * kv; g += data[j + 1] * kv; b += data[j + 2] * kv;
    }
    const o = (y * w + x) * 4;
    out[o] = clamp(r / div + offset); out[o + 1] = clamp(g / div + offset); out[o + 2] = clamp(b / div + offset);
  }
  return img(out, w, h);
}
export const blur = (src) => convolve(src, [1, 2, 1, 2, 4, 2, 1, 2, 1]);
export const sharpen = (src) => convolve(src, [0, -1, 0, -1, 5, -1, 0, -1, 0]);
export const edges = (src) => convolve(grayscale(src), [-1, -1, -1, -1, 8, -1, -1, -1, -1], { divisor: 1, offset: 0 });

// ---- transforms (dims may change) ------------------------------------------------------------------------
export function flipH(src) { const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data.length);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const s = (y * w + x) * 4, d = (y * w + (w - 1 - x)) * 4; out[d] = data[s]; out[d + 1] = data[s + 1]; out[d + 2] = data[s + 2]; out[d + 3] = data[s + 3]; } return img(out, w, h); }
export function flipV(src) { const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data.length);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const s = (y * w + x) * 4, d = ((h - 1 - y) * w + x) * 4; out[d] = data[s]; out[d + 1] = data[s + 1]; out[d + 2] = data[s + 2]; out[d + 3] = data[s + 3]; } return img(out, w, h); }
// rotate 90°: dir 'cw' | 'ccw'. Output dims are swapped.
export function rotate90(src, dir = 'cw') { const { data, width: w, height: h } = src, out = new Uint8ClampedArray(data.length);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const s = (y * w + x) * 4; let nx, ny;
    if (dir === 'cw') { nx = h - 1 - y; ny = x; } else { nx = y; ny = w - 1 - x; }
    const d = (ny * h + nx) * 4; out[d] = data[s]; out[d + 1] = data[s + 1]; out[d + 2] = data[s + 2]; out[d + 3] = data[s + 3];
  }
  return img(out, h, w); }

// Bounding box of pixels with alpha > threshold (for "trim transparent edges" / auto-crop).
export function trimTransparent(src, { threshold = 0 } = {}) {
  const { data, width: w, height: h } = src;
  let minX = w, minY = h, maxX = -1, maxY = -1;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (data[(y * w + x) * 4 + 3] > threshold) {
    if (x < minX) minX = x; if (x > maxX) maxX = x; if (y < minY) minY = y; if (y > maxY) maxY = y;
  }
  if (maxX < 0) return { x: 0, y: 0, width: w, height: h, empty: true };
  return { x: minX, y: minY, width: maxX - minX + 1, height: maxY - minY + 1, empty: false };
}

// Crop to a rectangle (clamped to bounds). Returns a NEW image.
export function crop(src, x, y, cw, ch) {
  const { data, width: w, height: h } = src;
  x = Math.max(0, x | 0); y = Math.max(0, y | 0); cw = Math.min(w - x, Math.max(1, cw | 0)); ch = Math.min(h - y, Math.max(1, ch | 0));
  const out = new Uint8ClampedArray(cw * ch * 4);
  for (let yy = 0; yy < ch; yy++) for (let xx = 0; xx < cw; xx++) {
    const s = ((y + yy) * w + (x + xx)) * 4, d = (yy * cw + xx) * 4;
    out[d] = data[s]; out[d + 1] = data[s + 1]; out[d + 2] = data[s + 2]; out[d + 3] = data[s + 3];
  }
  return img(out, cw, ch);
}
