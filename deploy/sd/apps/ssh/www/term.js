// NucleoOS SSH — compact VT100/ANSI terminal emulator (no external lib, so it ships on the SD as-is).
//
// Handles the common cases well: text, colors (16/256/truecolor), bold/dim/inverse/underline, cursor
// moves, erase, scroll regions, alt-screen, save/restore — i.e. shells, ls --color, git, tail, package
// managers, light editing. Full-screen TUIs (vim/htop) render acceptably but not pixel-perfect; for
// full fidelity one could later load xterm.js from the bridge. Renders a DOM grid (rAF-batched).

const PALETTE = (() => {
  const base = ['#000000','#cd0000','#00cd00','#cdcd00','#1e90ff','#cd00cd','#00cdcd','#e5e5e5',
                '#7f7f7f','#ff0000','#00ff00','#ffff00','#5c9fff','#ff00ff','#00ffff','#ffffff'];
  const p = base.slice();
  const lv = [0,95,135,175,215,255];
  for (let r = 0; r < 6; r++) for (let g = 0; g < 6; g++) for (let b = 0; b < 6; b++)
    p.push('#' + [lv[r], lv[g], lv[b]].map((v) => v.toString(16).padStart(2, '0')).join(''));
  for (let i = 0; i < 24; i++) { const v = (8 + i * 10).toString(16).padStart(2, '0'); p.push('#' + v + v + v); }
  return p;
})();
const colorCss = (c) => (c == null ? null : (typeof c === 'string' ? c : (PALETTE[c] || null)));
const escHtml = (s) => s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));

export function createTerminal(container, opts = {}) {
  const onInput = opts.onInput || (() => {});
  const onResize = opts.onResize || (() => {});
  let cols = opts.cols || 80, rows = opts.rows || 24;

  // screen state
  let main = null, alt = null, screen = null, usingAlt = false;
  let cx = 0, cy = 0, savedX = 0, savedY = 0;
  let fg = null, bg = null, fl = 0;                 // current SGR (fl bits: 1 bold 2 inverse 4 underline 8 dim)
  let top = 0, bot = rows - 1;                      // scroll region
  let cursorVisible = true, wrapNext = false;
  const dec = new TextDecoder('utf-8');

  // parser
  let state = 0;                                    // 0 ground, 1 esc, 2 csi, 3 osc
  let params = '', priv = '';

  const blank = () => ({ ch: ' ', fg: null, bg: null, fl: 0 });
  function makeGrid() { const g = []; for (let y = 0; y < rows; y++) { const r = []; for (let x = 0; x < cols; x++) r.push(blank()); g.push(r); } return g; }
  function reset() { main = makeGrid(); alt = makeGrid(); screen = main; usingAlt = false; cx = cy = 0; top = 0; bot = rows - 1; fg = bg = null; fl = 0; wrapNext = false; cursorVisible = true; }
  reset();

  // ── DOM ──
  const root = document.createElement('div');
  root.className = 'nt-screen'; root.tabIndex = 0;
  container.appendChild(root);

  function cellStyle(c) {
    let f = c.fg, b = c.bg, bits = c.fl;
    if (bits & 2) { const t = f; f = (b == null ? '__bg' : b); b = (t == null ? '__fg' : t); }   // inverse
    const st = [];
    const fc = f === '__bg' ? 'var(--nt-bg)' : (f === '__fg' ? 'var(--nt-fg)' : colorCss(f));
    const bc = b === '__fg' ? 'var(--nt-fg)' : (b === '__bg' ? 'var(--nt-bg)' : colorCss(b));
    if (fc) st.push('color:' + fc);
    if (bc) st.push('background:' + bc);
    if (bits & 1) st.push('font-weight:700');
    if (bits & 8) st.push('opacity:.65');
    if (bits & 4) st.push('text-decoration:underline');
    return st.join(';');
  }
  let raf = 0;
  function schedule() { if (!raf) raf = requestAnimationFrame(render); }
  function render() {
    raf = 0;
    let html = '';
    for (let y = 0; y < rows; y++) {
      const r = screen[y]; let line = ''; let run = '', runStyle = null, runStart = 0;
      const flush = (style, text) => { if (!text) return; line += style ? '<span style="' + style + '">' + escHtml(text) + '</span>' : escHtml(text); };
      for (let x = 0; x < cols; x++) {
        const c = r[x];
        const isCur = cursorVisible && !document.hidden && y === cy && x === cx && root === document.activeElement;
        const style = cellStyle(c) + (isCur ? ';outline:1px solid var(--nt-cur);background:var(--nt-cur);color:var(--nt-bg)' : '');
        if (style === runStyle) { run += c.ch; }
        else { flush(runStyle, run); run = c.ch; runStyle = style; }
      }
      flush(runStyle, run);
      html += line + '\n';
    }
    root.innerHTML = html;
  }

  // ── scrolling within [top,bot] ──
  function scrollUp(n = 1) { for (let i = 0; i < n; i++) { screen.splice(top, 1); const r = []; for (let x = 0; x < cols; x++) r.push(blank()); screen.splice(bot, 0, r); } }
  function scrollDown(n = 1) { for (let i = 0; i < n; i++) { screen.splice(bot, 1); const r = []; for (let x = 0; x < cols; x++) r.push(blank()); screen.splice(top, 0, r); } }
  function newline() { if (cy === bot) scrollUp(1); else cy = Math.min(rows - 1, cy + 1); }

  function putChar(ch) {
    if (wrapNext) { cx = 0; newline(); wrapNext = false; }
    if (cx >= cols) { cx = 0; newline(); }
    const cell = screen[cy][cx];
    cell.ch = ch; cell.fg = fg; cell.bg = bg; cell.fl = fl;
    if (cx === cols - 1) wrapNext = true; else cx++;
  }

  // ── SGR ──
  function sgr(parts) {
    if (!parts.length) parts = [0];
    for (let i = 0; i < parts.length; i++) {
      const n = parts[i] | 0;
      if (n === 0) { fg = bg = null; fl = 0; }
      else if (n === 1) fl |= 1; else if (n === 2) fl |= 8; else if (n === 4) fl |= 4; else if (n === 7) fl |= 2;
      else if (n === 22) fl &= ~9; else if (n === 24) fl &= ~4; else if (n === 27) fl &= ~2;
      else if (n >= 30 && n <= 37) fg = n - 30;
      else if (n >= 40 && n <= 47) bg = n - 40;
      else if (n >= 90 && n <= 97) fg = n - 90 + 8;
      else if (n >= 100 && n <= 107) bg = n - 100 + 8;
      else if (n === 39) fg = null; else if (n === 49) bg = null;
      else if (n === 38 || n === 48) {
        const mode = parts[i + 1] | 0;
        if (mode === 5) { const idx = parts[i + 2] | 0; if (n === 38) fg = idx; else bg = idx; i += 2; }
        else if (mode === 2) { const hex = '#' + [parts[i + 2], parts[i + 3], parts[i + 4]].map((v) => ((v | 0) & 255).toString(16).padStart(2, '0')).join(''); if (n === 38) fg = hex; else bg = hex; i += 4; }
      }
    }
  }

  function eraseInLine(mode) { const r = screen[cy]; if (mode === 0) for (let x = cx; x < cols; x++) r[x] = blank(); else if (mode === 1) for (let x = 0; x <= cx; x++) r[x] = blank(); else for (let x = 0; x < cols; x++) r[x] = blank(); }
  function eraseInDisplay(mode) {
    if (mode === 0) { eraseInLine(0); for (let y = cy + 1; y < rows; y++) screen[y] = screen[y].map(blank); }
    else if (mode === 1) { eraseInLine(1); for (let y = 0; y < cy; y++) screen[y] = screen[y].map(blank); }
    else { for (let y = 0; y < rows; y++) screen[y] = screen[y].map(blank); cx = cy = 0; }
  }

  function csi(final) {
    const p = params.split(';').map((s) => (s === '' ? 0 : parseInt(s, 10)));
    const a = (i, d) => (p[i] == null || isNaN(p[i]) ? d : p[i]);
    if (priv === '?') {                              // DEC private modes
      const m = a(0, 0);
      if (final === 'h' || final === 'l') {
        const set = final === 'h';
        if (m === 25) cursorVisible = set;
        else if (m === 1049 || m === 47 || m === 1047) { if (set && !usingAlt) { usingAlt = true; screen = alt; eraseInDisplay(2); cx = cy = 0; } else if (!set && usingAlt) { usingAlt = false; screen = main; } }
      }
      return;
    }
    switch (final) {
      case 'A': cy = Math.max(top, cy - a(0, 1)); break;
      case 'B': cy = Math.min(bot, cy + a(0, 1)); break;
      case 'C': cx = Math.min(cols - 1, cx + a(0, 1)); wrapNext = false; break;
      case 'D': cx = Math.max(0, cx - a(0, 1)); wrapNext = false; break;
      case 'E': cx = 0; cy = Math.min(bot, cy + a(0, 1)); break;
      case 'F': cx = 0; cy = Math.max(top, cy - a(0, 1)); break;
      case 'G': case '`': cx = Math.min(cols - 1, Math.max(0, a(0, 1) - 1)); wrapNext = false; break;
      case 'd': cy = Math.min(rows - 1, Math.max(0, a(0, 1) - 1)); break;
      case 'H': case 'f': cy = Math.min(rows - 1, Math.max(0, a(0, 1) - 1)); cx = Math.min(cols - 1, Math.max(0, a(1, 1) - 1)); wrapNext = false; break;
      case 'J': eraseInDisplay(a(0, 0)); break;
      case 'K': eraseInLine(a(0, 0)); break;
      case 'm': sgr(p); break;
      case 'r': top = Math.max(0, a(0, 1) - 1); bot = Math.min(rows - 1, a(1, rows) - 1); cx = cy = 0; break;
      case 'S': scrollUp(a(0, 1)); break;
      case 'T': scrollDown(a(0, 1)); break;
      case 'L': { const n = a(0, 1); for (let i = 0; i < n; i++) { screen.splice(bot, 1); screen.splice(cy, 0, screen[0].map(blank)); } break; }
      case 'M': { const n = a(0, 1); for (let i = 0; i < n; i++) { screen.splice(cy, 1); screen.splice(bot, 0, screen[0].map(blank)); } break; }
      case 'P': { const n = a(0, 1); const r = screen[cy]; r.splice(cx, n); while (r.length < cols) r.push(blank()); break; }
      case '@': { const n = a(0, 1); const r = screen[cy]; for (let i = 0; i < n; i++) r.splice(cx, 0, blank()); r.length = cols; break; }
      case 'X': { const n = a(0, 1); for (let x = cx; x < Math.min(cols, cx + n); x++) screen[cy][x] = blank(); break; }
      case 's': savedX = cx; savedY = cy; break;
      case 'u': cx = savedX; cy = savedY; break;
    }
  }

  function feed(str) {
    for (const ch of str) {
      const code = ch.codePointAt(0);
      if (state === 0) {
        if (code === 0x1b) state = 1;
        else if (code === 0x0a || code === 0x0b || code === 0x0c) { newline(); }
        else if (code === 0x0d) { cx = 0; wrapNext = false; }
        else if (code === 0x08) { cx = Math.max(0, cx - 1); wrapNext = false; }
        else if (code === 0x09) { cx = Math.min(cols - 1, (Math.floor(cx / 8) + 1) * 8); }
        else if (code === 0x07) { /* BEL */ }
        else if (code >= 0x20) putChar(ch);
      } else if (state === 1) {
        if (ch === '[') { state = 2; params = ''; priv = ''; }
        else if (ch === ']') { state = 3; params = ''; }
        else if (ch === '7') { savedX = cx; savedY = cy; state = 0; }
        else if (ch === '8') { cx = savedX; cy = savedY; state = 0; }
        else if (ch === 'M') { if (cy === top) scrollDown(1); else cy = Math.max(0, cy - 1); state = 0; }   // reverse index
        else if (ch === 'c') { reset(); state = 0; }
        else if (ch === '=' || ch === '>') { state = 0; }    // keypad mode (ignore)
        else if (ch === '(' || ch === ')') { state = 4; }    // charset designate: consume next char
        else state = 0;
      } else if (state === 4) { state = 0; }                 // swallow charset id
      else if (state === 2) {
        if ((ch >= '0' && ch <= '9') || ch === ';') params += ch;
        else if (ch === '?' || ch === '>' || ch === '!') priv = ch;
        else { csi(ch); state = 0; }
      } else if (state === 3) {
        if (code === 0x07) state = 0;                        // OSC terminated by BEL
        else if (code === 0x1b) state = 5;                   // maybe ST (ESC \)
      } else if (state === 5) { state = 0; }                 // OSC ST tail
    }
    schedule();
  }

  // ── public API ──
  function write(data) { feed(typeof data === 'string' ? data : dec.decode(data, { stream: true })); }
  function clear() { eraseInDisplay(2); schedule(); }
  function focus() { root.focus(); }

  // size to the container using the measured cell box
  function fit() {
    const probe = document.createElement('span'); probe.textContent = 'M'; probe.style.cssText = 'visibility:hidden;position:absolute';
    root.appendChild(probe); const cw = probe.getBoundingClientRect().width || 8; const chh = probe.getBoundingClientRect().height || 16; probe.remove();
    const w = container.clientWidth, h = container.clientHeight;
    const nc = Math.max(20, Math.floor((w - 8) / cw)), nr = Math.max(6, Math.floor((h - 6) / chh));
    if (nc === cols && nr === rows) return { cols, rows };
    cols = nc; rows = nr; top = 0; bot = rows - 1; cx = Math.min(cx, cols - 1); cy = Math.min(cy, rows - 1);
    main = makeGrid(); alt = makeGrid(); screen = usingAlt ? alt : main;   // simplest: reset grids on resize (server repaints)
    schedule(); onResize(cols, rows); return { cols, rows };
  }

  // ── keyboard → bytes ──
  function keymap(e) {
    const k = e.key;
    if (e.altKey && k.length === 1) return '\x1b' + k;
    if (e.ctrlKey && !e.altKey) {
      if (k.length === 1) { const u = k.toUpperCase().charCodeAt(0); if (u >= 64 && u <= 95) return String.fromCharCode(u - 64); if (k >= 'a' && k <= 'z') return String.fromCharCode(k.charCodeAt(0) - 96); }
      if (k === ' ') return '\x00';
      return null;
    }
    switch (k) {
      case 'Enter': return '\r'; case 'Backspace': return '\x7f'; case 'Tab': return '\t'; case 'Escape': return '\x1b';
      case 'ArrowUp': return '\x1b[A'; case 'ArrowDown': return '\x1b[B'; case 'ArrowRight': return '\x1b[C'; case 'ArrowLeft': return '\x1b[D';
      case 'Home': return '\x1b[H'; case 'End': return '\x1b[F'; case 'PageUp': return '\x1b[5~'; case 'PageDown': return '\x1b[6~';
      case 'Insert': return '\x1b[2~'; case 'Delete': return '\x1b[3~';
      case 'F1': return '\x1bOP'; case 'F2': return '\x1bOQ'; case 'F3': return '\x1bOR'; case 'F4': return '\x1bOS';
    }
    if (k.length === 1) return k;
    return null;
  }
  root.addEventListener('keydown', (e) => { const b = keymap(e); if (b != null) { e.preventDefault(); onInput(b); } });
  root.addEventListener('paste', (e) => { e.preventDefault(); const t = (e.clipboardData || window.clipboardData).getData('text'); if (t) onInput(t); });
  root.addEventListener('blur', schedule); root.addEventListener('focus', schedule);

  return {
    write, clear, focus, fit,
    get cols() { return cols; }, get rows() { return rows; },
    dispose() { if (raf) cancelAnimationFrame(raf); root.remove(); },
  };
}
