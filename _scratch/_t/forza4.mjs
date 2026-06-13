// forza4.js — Connect Four / Forza 4, evolved. PROOF of transversality: this file is ONLY game logic +
// a thin bridge to its renderer. No chat, no lobby, no networking — the shared layer provides all that.
//
// TWO LAYERS, like Pong:
//  · LOGIC (this file): pure, host-authoritative. The reducer drops discs, tracks the last move + a move
//    counter (so the renderer can animate a single fall), and stamps the winning 4-cell line on victory.
//    A depth-limited negamax (alpha-beta) powers the "difficile" opponent; a heuristic powers facile/medio.
//  · RENDER (games/forza4-3d.js): the SAME Three.js/WebGL engine that drives Pong — a neon 3D board,
//    glowing discs that drop and bounce, particle bursts, a winning-line tube, camera shake and the
//    procedural Web-Audio SFX. Falls back to an enhanced 2D canvas where WebGL is missing.
import { defineGame } from './shim.mjs';

const COLS = 7, ROWS = 6;
const idx = (r, c) => r * COLS + c;
const GEOM = { COLS, ROWS };

function legalCols(board) { const out = []; for (let c = 0; c < COLS; c++) if (board[idx(0, c)] == null) out.push(c); return out; }

// Drop a disc for `seat` into column c. Returns {board, row} or null if the column is full.
function drop(board, c, seat) {
  for (let r = ROWS - 1; r >= 0; r--) {
    if (board[idx(r, c)] == null) { const b = board.slice(); b[idx(r, c)] = seat; return { board: b, row: r }; }
  }
  return null;
}

const DIRS = [[0, 1], [1, 0], [1, 1], [1, -1]];
function winnerOf(board) {
  for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) {
    const v = board[idx(r, c)]; if (v == null) continue;
    for (const [dr, dc] of DIRS) {
      let k = 1;
      while (k < 4) { const nr = r + dr * k, nc = c + dc * k; if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS || board[idx(nr, nc)] !== v) break; k++; }
      if (k === 4) return v;
    }
  }
  return null;
}
// The four cells of a winning run (for the renderer's celebration), or null.
function winningLine(board) {
  for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) {
    const v = board[idx(r, c)]; if (v == null) continue;
    for (const [dr, dc] of DIRS) {
      const cells = [{ r, c }];
      let k = 1;
      while (k < 4) { const nr = r + dr * k, nc = c + dc * k; if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS || board[idx(nr, nc)] !== v) break; cells.push({ r: nr, c: nc }); k++; }
      if (cells.length === 4) return cells;
    }
  }
  return null;
}

export default defineGame({
  id: 'forza4',
  name: 'Forza 4',
  minPlayers: 2,
  maxPlayers: 2,
  aiCapable: true,
  aiCoach: false,         // turn-based: the LLM PLAYS the moves (mover), it does not coach
  realtime: false,
  aiThinkMs: 480,
  renderMode: '3d',       // games/forza4-3d.js owns a Three.js WebGL surface (same engine as Pong)
  fillViewport: true,     // use the whole board area, responsive
  capturesPointer: false, // turn-based: keep the cursor free to click/hover columns

  // Declarative settings — the waiting room renders this as a form; values arrive in setup().
  options: [
    { key: 'level', label: 'Difficoltà avversario', type: 'select', default: 'medio',
      choices: [{ value: 'facile', label: 'Facile' }, { value: 'medio', label: 'Medio' }, { value: 'difficile', label: 'Difficile (negamax)' }] },
    { key: 'mente', label: 'Mente avversario', type: 'select', default: 'classica',
      choices: [{ value: 'classica', label: 'Classica (offline)' }, { value: 'llm', label: 'LLM online (Grok/Claude)' }] },
  ],

  setup(players, seed, options) {
    return {
      board: Array(COLS * ROWS).fill(null), turn: 0,
      seats: players.map(p => p.seat), names: players.map(p => p.name),
      opts: options || {}, brain: null,
      last: null, moves: 0, win: null,   // last={col,row,seat} + counter drive the drop animation; win=4 cells
    };
  },

  reduce(state, action, seat) {
    if (action && action.type) { if (action.type === 'brain') return { ...state, brain: action.brain || null }; return state; }
    if (winnerOf(state.board) != null) return state;
    if (state.seats[state.turn] !== seat) return state;
    const res = drop(state.board, action.col | 0, seat);
    if (!res) return state;
    return {
      ...state, board: res.board, turn: (state.turn + 1) % state.seats.length,
      last: { col: action.col | 0, row: res.row, seat }, moves: state.moves + 1, win: winningLine(res.board),
    };
  },

  isOver(state) {
    const w = winnerOf(state.board);
    if (w != null) return { winner: w };
    if (legalCols(state.board).length === 0) return { winner: -1 };
    return null;
  },

  // Pointer: the 3D renderer raycasts screen→column (it knows the live camera); fall back to nothing until
  // the renderer is ready so a click never lands on a wrong column during the brief load window.
  onPointer(x, y, api) {
    if (!(R3D && R3D.screenToCol)) return null;
    const col = R3D.screenToCol(x / api.width, y / api.height);
    return (col >= 0 && col < COLS) ? { col } : null;
  },
  onKey(key) { const n = parseInt(key, 10); return (n >= 1 && n <= COLS) ? { col: n - 1 } : null; },

  // Gamepad: D-pad/stick left-right moves a column cursor, A drops the disc.
  padMode: 'cursor',
  onPadDir(dx, dy, api) { let cur = api.cursor(); if (cur == null) cur = 3; api.setCursor(Math.max(0, Math.min(COLS - 1, cur + (dx | 0)))); return null; },
  onPadButton(name, api) { if (name === 'A') { const c = api.cursor(); return { col: c == null ? 3 : c }; } return null; },

  ai(state, seat) {
    const cols = legalCols(state.board);
    if (!cols.length) return null;
    const level = (state.opts && state.opts.level) || 'medio';
    if (level === 'difficile') { const c = bestNegamax(state.board, seat, state.seats); return c == null ? null : { col: c }; }

    const b = state.board, me = seat, opp = state.seats.find(s => s !== seat);
    // 1) win now
    for (const c of cols) { const r = drop(b, c, me); if (r && winnerOf(r.board) === me) return { col: c }; }
    // 2) block opponent's immediate win
    for (const c of cols) { const r = drop(b, c, opp); if (r && winnerOf(r.board) === opp) return { col: c }; }
    // 3) on 'medio', avoid columns that hand the opponent a win on top of our move
    let pool = cols;
    if (level === 'medio') {
      const safe = cols.filter(c => {
        const r = drop(b, c, me); if (!r) return false;
        for (const c2 of legalCols(r.board)) { const r2 = drop(r.board, c2, opp); if (r2 && winnerOf(r2.board) === opp) return false; }
        return true;
      });
      if (safe.length) pool = safe;
    }
    // 4) center preference
    pool = pool.slice().sort((a, c) => Math.abs(a - 3) - Math.abs(c - 3));
    return { col: pool[0] };
  },

  // LLM mover (turn-based): the model picks the column. Off-turn → null (no wasted call); illegal/full
  // column → null so the brain retries. This is where an LLM genuinely PLAYS the game.
  llm: {
    prompt(state, seat) {
      if (winnerOf(state.board) != null || state.seats[state.turn] !== seat) return null;
      const cols = legalCols(state.board), rows = [];
      for (let r = 0; r < ROWS; r++) { let line = ''; for (let c = 0; c < COLS; c++) { const v = state.board[idx(r, c)]; line += v == null ? '.' : (v === seat ? 'X' : 'O'); } rows.push(line); }
      return {
        system: "Giochi a Forza 4 (Connect Four) su 7 colonne × 6 righe e sei 'X'. Le colonne sono numerate 0-6 e i dischi cadono in basso. Gioca per vincere: 4 in fila (orizzontale, verticale o diagonale) oppure blocca 'O'. Rispondi SOLO con JSON: {\"col\":<colonna NON piena 0-6>,\"taunt\":\"frase brevissima in italiano\"}.",
        user: `Griglia (alto→basso), '.'=vuoto 'X'=tu 'O'=avversario:\n${rows.join('\n')}\nColonne giocabili: ${cols.join(', ')}. Tocca a te (X).`,
        maxTokens: 80,
      };
    },
    parse(text, state) {
      const j = exj(text); if (!j) return null;
      const col = Number(j.col);
      if (!Number.isInteger(col) || col < 0 || col >= COLS || state.board[idx(0, col)] != null) return null;
      return { action: { col }, taunt: j.taunt ? String(j.taunt).slice(0, 120) : null };
    },
  },

  // ── 3D render surface (delegates to games/forza4-3d.js; lazy-loads the shared Three.js engine) ──
  mount(canvas, api) { ensureRenderer(canvas, api); },
  render(api, state) { if (R3D) R3D.frame(state, api); },
  resize(w, h) { if (R3D) R3D.resize(w, h); },
  unmount() { if (R3D) { try { R3D.dispose(); } catch {} R3D = null; } },
});

// ── renderer bootstrap (module-scope: one active match at a time, mirrors pong.js) ───────────────
let R3D = null, r3dLoading = false;
async function ensureRenderer(canvas, api) {
  if (R3D || r3dLoading) return;
  r3dLoading = true;
  try {
    const mod = await import('/apps/games/games/forza4-3d.js');
    R3D = await mod.createForza4Renderer(canvas, api, GEOM);
    const wrap = canvas.parentElement; if (wrap) { const r = wrap.getBoundingClientRect(); if (r.width > 1 && r.height > 1) R3D.resize(r.width, r.height); }
  } catch (e) { console.error('forza4-3d load failed', e); }
  finally { r3dLoading = false; }
}

// ── "difficile" opponent: depth-limited negamax with alpha-beta over Connect Four windows ────────
const DIFF_DEPTH = 6;
function bestNegamax(board, me, seats) {
  const opp = seats.find(s => s !== me), cols = legalCols(board);
  if (!cols.length) return null;
  for (const c of cols) { const r = drop(board, c, me); if (r && winnerOf(r.board) === me) return c; }   // take a win
  const order = cols.slice().sort((a, b) => Math.abs(a - 3) - Math.abs(b - 3));   // center-first improves pruning
  let best = order[0], bestV = -Infinity, alpha = -Infinity;
  for (const c of order) {
    const r = drop(board, c, me); if (!r) continue;
    const v = search(r.board, DIFF_DEPTH - 1, alpha, Infinity, opp, me, opp);
    if (v > bestV) { bestV = v; best = c; }
    if (v > alpha) alpha = v;
  }
  return best;
}
function search(board, depth, alpha, beta, player, me, opp) {
  const w = winnerOf(board);
  if (w === me) return 100000 + depth;          // prefer faster wins / slower losses
  if (w === opp) return -(100000 + depth);
  const cols = legalCols(board);
  if (!cols.length) return 0;                    // draw
  if (depth === 0) return evalBoard(board, me, opp);
  const order = cols.slice().sort((a, b) => Math.abs(a - 3) - Math.abs(b - 3));
  if (player === me) {
    let best = -Infinity;
    for (const c of order) { const r = drop(board, c, me); const v = search(r.board, depth - 1, alpha, beta, opp, me, opp); if (v > best) best = v; if (v > alpha) alpha = v; if (alpha >= beta) break; }
    return best;
  } else {
    let best = Infinity;
    for (const c of order) { const r = drop(board, c, opp); const v = search(r.board, depth - 1, alpha, beta, me, me, opp); if (v < best) best = v; if (v < beta) beta = v; if (alpha >= beta) break; }
    return best;
  }
}
// Static eval from `me`'s view: weight 4-windows (3-in-a-row threats heavily) + centre control.
function evalBoard(board, me, opp) {
  let score = 0;
  for (let r = 0; r < ROWS; r++) if (board[idx(r, 3)] === me) score += 3;
  for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) for (const [dr, dc] of DIRS) {
    const er = r + 3 * dr, ec = c + 3 * dc; if (er < 0 || er >= ROWS || ec < 0 || ec >= COLS) continue;
    let mine = 0, his = 0, empty = 0;
    for (let k = 0; k < 4; k++) { const v = board[idx(r + dr * k, c + dc * k)]; if (v === me) mine++; else if (v === opp) his++; else empty++; }
    if (mine && his) continue;                   // mixed window can't complete → dead
    if (mine === 3 && empty === 1) score += 50; else if (mine === 2 && empty === 2) score += 8; else if (mine === 1 && empty === 3) score += 1;
    if (his === 3 && empty === 1) score -= 60; else if (his === 2 && empty === 2) score -= 8;   // value blocking a bit higher
  }
  return score;
}

function exj(t) { if (!t) return null; const a = t.indexOf('{'), b = t.lastIndexOf('}'); if (a < 0 || b <= a) return null; try { return JSON.parse(t.slice(a, b + 1)); } catch { return null; } }
