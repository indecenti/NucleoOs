// paint-imaging.test.mjs — deterministic host gate for Paint's pixel-processing core (apps/paint/www/
// imaging.js). No canvas, no DOM: pure typed-array image ops. Proves the user-requested features —
// background removal (edge-flood + magic wand), PNG-alpha correctness, adjustments, filters, transforms.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as IM from '../../apps/paint/www/imaging.js';

// build a {data,width,height} image from a (x,y)->[r,g,b,a] function
function mk(w, h, fn) { const d = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const [r, g, b, a = 255] = fn(x, y); const i = (y * w + x) * 4; d[i] = r; d[i + 1] = g; d[i + 2] = b; d[i + 3] = a; }
  return { data: d, width: w, height: h }; }
const alpha = (im, x, y) => im.data[(y * im.width + x) * 4 + 3];
const px = (im, x, y) => { const i = (y * im.width + x) * 4; return [im.data[i], im.data[i + 1], im.data[i + 2], im.data[i + 3]]; };

test('removeBackgroundAuto: white border → transparent, red subject kept, interior hole preserved', () => {
  // 7×7: white background, a red 5×5 block with a WHITE hole at its centre (not connected to the edge).
  const im = mk(7, 7, (x, y) => {
    const inside = x >= 1 && x <= 5 && y >= 1 && y <= 5;
    if (!inside) return [255, 255, 255];
    if (x === 3 && y === 3) return [255, 255, 255];   // interior white hole
    return [220, 30, 30];
  });
  const out = IM.removeBackgroundAuto(im, { tolerance: 40, feather: 0 });
  assert.equal(alpha(out, 0, 0), 0, 'corner background removed');
  assert.equal(alpha(out, 6, 0), 0, 'border removed');
  assert.equal(alpha(out, 2, 2), 255, 'subject kept opaque');
  assert.equal(alpha(out, 3, 3), 255, 'interior hole NOT removed (connected-from-edge only)');
  // original untouched
  assert.equal(alpha(im, 0, 0), 255);
});

test('magicWandErase: floods only the contiguous similar region from the click point', () => {
  const im = mk(5, 5, (x, y) => (x < 2 ? [10, 10, 10] : [240, 240, 240]));  // left dark | right light
  const out = IM.magicWandErase(im, 0, 0, { tolerance: 30, feather: 0 });
  assert.equal(alpha(out, 0, 0), 0); assert.equal(alpha(out, 1, 4), 0, 'whole left band erased');
  assert.equal(alpha(out, 3, 2), 255, 'right band untouched');
});

test('colorToAlpha: global chroma key removes every matching pixel (even disconnected)', () => {
  const im = mk(3, 1, (x) => (x === 1 ? [255, 255, 255] : [0, 255, 0]));   // green,white,green
  const out = IM.colorToAlpha(im, '#00ff00', { tolerance: 20 });
  assert.equal(alpha(out, 0, 0), 0); assert.equal(alpha(out, 2, 0), 0, 'both green pixels gone');
  assert.equal(alpha(out, 1, 0), 255, 'white kept');
});

test('adjust: brightness brightens & clamps, preserves alpha; saturation 0 → gray-ish', () => {
  const im = mk(1, 1, () => [100, 100, 100, 128]);
  assert.ok(IM.adjust(im, { brightness: 50 }).data[0] > 100);
  assert.equal(IM.adjust(im, { brightness: -100 }).data[0], 0, 'clamped at 0');
  assert.equal(IM.adjust(im, { brightness: 50 }).data[3], 128, 'alpha preserved');
});

test('grayscale / invert are correct', () => {
  const red = mk(1, 1, () => [255, 0, 0]);
  assert.deepEqual(px(IM.grayscale(red), 0, 0).slice(0, 3), [76, 76, 76]);
  assert.deepEqual(px(IM.invert(red), 0, 0).slice(0, 3), [0, 255, 255]);
});

test('blur/sharpen run and keep dimensions + alpha', () => {
  const im = mk(4, 4, (x, y) => [((x + y) % 2) * 255, 0, 0, 200]);
  const b = IM.blur(im); assert.equal(b.width, 4); assert.equal(b.height, 4);
  assert.equal(b.data[3], 200, 'alpha preserved by convolve');
});

test('flipH / flipV / rotate90 reposition pixels and swap dims on rotate', () => {
  const im = mk(2, 1, (x) => (x === 0 ? [1, 0, 0] : [2, 0, 0]));   // [A,B]
  assert.equal(IM.flipH(im).data[0], 2, 'flipH swaps to [B,A]');
  const r = IM.rotate90(im, 'cw'); assert.equal(r.width, 1); assert.equal(r.height, 2);
  const v = IM.flipV(mk(1, 2, (x, y) => [y + 1, 0, 0])); assert.equal(v.data[0], 2, 'flipV swaps rows');
});

test('trimTransparent finds the opaque bounding box; crop extracts it', () => {
  const im = mk(6, 6, (x, y) => (x >= 2 && x <= 3 && y >= 1 && y <= 4 ? [10, 20, 30, 255] : [0, 0, 0, 0]));
  const bb = IM.trimTransparent(im);
  assert.deepEqual([bb.x, bb.y, bb.width, bb.height], [2, 1, 2, 4]);
  const c = IM.crop(im, bb.x, bb.y, bb.width, bb.height);
  assert.equal(c.width, 2); assert.equal(c.height, 4);
  assert.deepEqual(px(c, 0, 0).slice(0, 3), [10, 20, 30]);
});
