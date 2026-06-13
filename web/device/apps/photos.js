// Photos — device-simulator app. Mirrors firmware app_photos.cpp: a list of the
// images on the SD card (/data/Pictures), then a full-screen, auto-fitted viewer.
// Here the browser decodes the JPEG/PNG (Image element) exactly as M5GFX does on
// the device, drawing through the shared canvas context (g.ctx).
import { makeListState, drawFocusList, title } from './_list.js';
const lst = makeListState();

const PIC_DIR = '/data/Pictures';
const IMG_RE = /\.(png|jpe?g)$/i;

let names = [];        // image filenames
let sel = 0;
let view = false;      // false = list, true = full-screen viewer
const cache = new Map();   // name -> HTMLImageElement (lazy)

function imageFor(name) {
  if (cache.has(name)) return cache.get(name);
  const img = new Image();
  img.src = '/api/fs/read?path=' + encodeURIComponent(PIC_DIR + '/' + name);
  cache.set(name, img);
  return img;
}

async function scan() {
  names = []; sel = 0; view = false;
  try {
    const r = await (await fetch('/api/fs/list?path=' + encodeURIComponent(PIC_DIR))).json();
    names = (r.entries || []).filter(e => e.type === 'file' && IMG_RE.test(e.name))
      .map(e => e.name).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
  } catch { names = []; }
}

export const photosApp = {
  id: 'photos',

  enter() { scan(); return { hint: ';/. move  enter view  esc back' }; },

  key(key, ch) {
    if (key === 'up') { if (names.length) sel = (sel + names.length - 1) % names.length; }
    else if (key === 'down') { if (names.length) sel = (sel + 1) % names.length; }
    else if (key === 'backspace') { view = false; }
    else if (key === 'enter') { if (names.length) { view = true; imageFor(names[sel]); } }
    else if (ch >= '1' && ch <= '9') { const i = +ch - 1; if (i < names.length) { sel = i; view = true; imageFor(names[i]); } }
  },

  draw(g) {
    return view ? drawView(g) : drawList(g);
  },
};

function drawList(g) {
  const top = g.contentTop, h = g.contentH;
  const y0 = title(g, 'Photos', '#ffd166', `${names.length} image${names.length === 1 ? '' : 's'}`);

  if (!names.length) { g.text('No images in /data/Pictures', 12, y0 + 16, g.COL.dim, 9); return { instruction: 'Browse images on the SD card', hint: 'esc back' }; }

  drawFocusList(g, lst, {
    top: y0, h: top + h - y0, count: names.length, sel, now: g.now,
    label: i => '🖼 ' + names[i],
    color: () => '#ffd166',
  });
  return { instruction: 'Browse images on the SD card', hint: ';/. move  enter view  esc back' };
}

function drawView(g) {
  const top = g.contentTop, h = g.contentH;
  g.ctx.fillStyle = '#000'; g.ctx.fillRect(0, top, g.W, h);
  const img = imageFor(names[sel]);
  if (img.complete && img.naturalWidth) {
    const box = h - 12, scale = Math.min(g.W / img.naturalWidth, box / img.naturalHeight);
    const w = img.naturalWidth * scale, dh = img.naturalHeight * scale;
    g.ctx.imageSmoothingEnabled = false;
    g.ctx.drawImage(img, (g.W - w) / 2, top + (box - dh) / 2, w, dh);
  } else {
    g.text('Loading…', g.W / 2, top + h / 2, g.COL.muted, 9, 'normal', 'center');
  }
  g.text(`${g.clamp(names[sel], 26)}  ${sel + 1}/${names.length}`, 6, top + h - 5, '#fff', 8);
  return { instruction: 'Image viewer', hint: ';/. prev/next  del list  esc back' };
}

// Test/inspection hook (preview harness; not part of the app contract).
export function _debugState() { return { view, sel, n: names.length, focused: names[sel] || null }; }
