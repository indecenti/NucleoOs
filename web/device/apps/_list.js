// Shared Wear OS-style focused list, used by the Files / Music / Photos / Notes apps so
// they all feel like a modern smartwatch: the focused row is enlarged (big, readable),
// neighbours shrink and dim, the list smooth-scrolls to keep focus centred, and a long
// focused label marquees horizontally so the whole name can be read. Mirrors the firmware
// helper app_ui.cpp.

export function makeListState() { return { smoothY: 0, lastSel: -1, marqAt: 0 }; }

// Consistent, readable app header: accent title + a hairline rule. Returns the y below
// the header so callers can lay out their content. Mirrors firmware app_ui_title.
export function title(g, text, accent, right) {
  const top = g.contentTop;
  g.text(text, 10, top + 9, accent || '#4ea1ff', 12, 'bold');
  if (right) g.text(right, g.W - 8, top + 9, g.COL.muted, 8, 'normal', 'right');
  g.roundRect(10, top + 18, g.W - 20, 1, 0, g.COL.line);
  return top + 24;
}

// o: { top, h, count, sel, now, label(i)->str, right?(i)->str, color?(i)->hex, marked?(i)->bool }
export function drawFocusList(g, st, o) {
  const W = g.W, STEP = 26;
  // ease the scroll position toward the selection (smartwatch glide)
  if (Math.abs(st.smoothY - o.sel) > 0.01) st.smoothY += (o.sel - st.smoothY) * 0.3; else st.smoothY = o.sel;
  if (o.sel !== st.lastSel) { st.lastSel = o.sel; st.marqAt = o.now; }   // restart marquee on move

  const center = o.top + o.h / 2;
  for (let i = 0; i < o.count; i++) {
    const y = center + (i - st.smoothY) * STEP;
    if (y < o.top - STEP || y > o.top + o.h + STEP) continue;
    const focus = i === o.sel;
    const color = o.color ? o.color(i) : '#4ea1ff';
    const label = o.label(i);
    const right = o.right ? (o.right(i) || '') : '';
    const mark = o.marked && o.marked(i);

    if (focus) {
      const ph = 22;
      g.roundRect(6, y - ph / 2, W - 12, ph, ph / 2, color);
      let rx = W - 12;
      if (right) { g.text(right, rx, y, '#000', 9, 'bold', 'right'); rx -= right.length * 6 + 8; }
      const px = mark ? 24 : 12;
      if (mark) g.text('▶', 13, y, '#000', 9, 'bold', 'center');
      marquee(g, st, label, px, y, rx - px, '#000', 13, o.now);
    } else {
      const dist = Math.abs(i - st.smoothY);
      const dim = dist > 1.5 ? g.COL.dim : g.COL.muted;
      if (mark) g.text('▶', 13, y, '#7CFC9A', 7, 'bold', 'center');
      g.text(g.clamp(label, 30), mark ? 22 : 14, y, dim, 8);
      if (right) g.text(right, W - 12, y, dim, 7, 'normal', 'right');
    }
  }

  // thin scroll indicator on the right edge
  if (o.count > o.h / STEP) {
    const track = o.h - 8, kh = Math.max(8, track * (o.h / STEP) / o.count);
    const ky = o.top + 4 + (track - kh) * (o.sel / Math.max(1, o.count - 1));
    g.roundRect(W - 3, o.top + 4, 2, track, 1, g.COL.line);
    g.roundRect(W - 3, ky, 2, kh, 1, o.color ? o.color(o.sel) : '#4ea1ff');
  }
}

// Horizontal auto-scroll for an overflowing focused label (ping-pong with end pauses).
function marquee(g, st, text, x, y, availW, color, px, now) {
  const ctx = g.ctx;
  ctx.font = `bold ${px}px ui-monospace, "Segoe UI", system-ui, sans-serif`;
  const tw = ctx.measureText(text).width;
  if (tw <= availW) { g.text(text, x, y, color, px, 'bold'); return; }

  const over = tw - availW + 2;
  const PAUSE = 700, travel = over * 22;           // ms pause at each end + scroll time
  const cycle = travel + PAUSE;
  let t = (now - st.marqAt) % (cycle * 2);
  let off;
  if (t < PAUSE) off = 0;                            // hold at start
  else if (t < cycle) off = ((t - PAUSE) / travel) * over;          // scroll right
  else if (t < cycle + PAUSE) off = over;           // hold at end
  else off = over - ((t - cycle - PAUSE) / travel) * over;          // scroll back
  off = Math.max(0, Math.min(over, off));

  ctx.save();
  ctx.beginPath(); ctx.rect(x, y - px, availW + 2, px * 2); ctx.clip();
  g.text(text, x - off, y, color, px, 'bold');
  ctx.restore();
}
