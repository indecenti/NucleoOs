// tris.js — reference game proving the NucleoOS game contract end-to-end.
// Turn-based, 1-2 players, AI-capable. Adding pong/chess/strategy later follows this exact shape.
import { defineGame } from './shim.mjs';

const LINES = [
  [0, 1, 2], [3, 4, 5], [6, 7, 8],   // rows
  [0, 3, 6], [1, 4, 7], [2, 5, 8],   // cols
  [0, 4, 8], [2, 4, 6],              // diagonals
];

function winnerOf(board) {
  for (const [a, b, c] of LINES) {
    if (board[a] != null && board[a] === board[b] && board[b] === board[c]) return board[a];
  }
  return null;
}

export default defineGame({
  id: 'tris',
  name: 'Tris',
  minPlayers: 2,
  maxPlayers: 2,
  aiCapable: true,
  realtime: false,
  aiThinkMs: 500,

  options: [
    { key: 'mente', label: 'Mente avversario', type: 'select', default: 'classica',
      choices: [{ value: 'classica', label: 'Classica (offline)' }, { value: 'llm', label: 'LLM online (Grok/Claude)' }] },
  ],

  setup(players) {
    return { board: Array(9).fill(null), turn: 0, seats: players.map(p => p.seat), names: players.map(p => p.name), brain: null };
  },

  // action = { cell }. Only the seat whose turn it is, on an empty cell. Typed actions (brain/policy)
  // come from the hub (seat -1) and must never be mistaken for a move.
  reduce(state, action, seat) {
    if (action && action.type) { if (action.type === 'brain') return { ...state, brain: action.brain || null }; return state; }
    if (winnerOf(state.board) != null) return state;
    if (state.seats[state.turn] !== seat) return state;          // not your turn
    const cell = action.cell | 0;
    if (cell < 0 || cell > 8 || state.board[cell] != null) return state;
    const board = state.board.slice();
    board[cell] = seat;
    return { ...state, board, turn: (state.turn + 1) % state.seats.length };
  },

  isOver(state) {
    const w = winnerOf(state.board);
    if (w != null) return { winner: w };
    if (state.board.every(c => c != null)) return { winner: -1 };  // draw
    return null;
  },

  onPointer(x, y, api) {
    const { cell } = hitCell(x, y, api);
    return cell >= 0 ? { cell } : null;
  },

  onKey(key) {
    // Number keys 1..9 map to the grid (numpad layout).
    const n = parseInt(key, 10);
    if (n >= 1 && n <= 9) { const map = [6, 7, 8, 3, 4, 5, 0, 1, 2]; return { cell: map[n - 1] }; }
    return null;
  },

  // Gamepad: D-pad/stick moves a cursor over the 3x3 grid, A drops the mark.
  padMode: 'cursor',
  onPadDir(dx, dy, api) {
    let cur = api.cursor(); if (cur == null) cur = 4;
    const col = Math.max(0, Math.min(2, (cur % 3) + (dx | 0)));
    const row = Math.max(0, Math.min(2, ((cur / 3) | 0) + (dy | 0)));
    api.setCursor(row * 3 + col);
    return null;
  },
  onPadButton(name, api) { if (name === 'A') { const c = api.cursor(); return { cell: c == null ? 4 : c }; } return null; },

  // Heuristic opponent (instant, offline): win > block > center > corner > random.
  ai(state, seat) {
    const b = state.board;
    const me = seat, opp = state.seats.find(s => s !== seat);
    const tryWin = (who) => {
      for (const [a, c, d] of LINES) {
        const line = [a, c, d], marks = line.map(i => b[i]);
        const empty = line[marks.indexOf(null)];
        if (marks.filter(m => m === who).length === 2 && marks.includes(null)) return empty;
      }
      return -1;
    };
    let cell = tryWin(me); if (cell < 0) cell = tryWin(opp);
    if (cell < 0 && b[4] == null) cell = 4;
    if (cell < 0) { const corners = [0, 2, 6, 8].filter(i => b[i] == null); if (corners.length) cell = corners[0]; }
    if (cell < 0) { const free = b.map((v, i) => v == null ? i : -1).filter(i => i >= 0); cell = free[0] ?? -1; }
    return cell >= 0 ? { cell } : null;
  },

  // LLM mover (turn-based): the model literally chooses the move. prompt() returns null off-turn so we
  // only spend a call when it's the AI's turn; parse() validates the cell (illegal → the brain retries).
  llm: {
    prompt(state, seat) {
      if (winnerOf(state.board) != null || state.seats[state.turn] !== seat) return null;
      const b = state.board, g = i => b[i] == null ? '.' : (b[i] === seat ? 'X' : 'O');
      const legal = b.map((v, i) => v == null ? i : -1).filter(i => i >= 0);
      return {
        system: "Giochi a Tris (tic-tac-toe) e sei 'X'. Le celle sono numerate 0-8, riga per riga (0,1,2 in alto). Gioca la mossa MIGLIORE: vinci se puoi, altrimenti blocca 'O', poi centro/angolo. Rispondi SOLO con JSON: {\"cell\":<una cella LIBERA>,\"taunt\":\"frase brevissima in italiano\"}.",
        user: `Griglia:\n${g(0)}${g(1)}${g(2)}\n${g(3)}${g(4)}${g(5)}\n${g(6)}${g(7)}${g(8)}\nCelle libere: ${legal.join(', ')}. Tocca a te (X).`,
        maxTokens: 80,
      };
    },
    parse(text, state) {
      const j = exj(text); if (!j) return null;
      const cell = Number(j.cell);
      if (!Number.isInteger(cell) || cell < 0 || cell > 8 || state.board[cell] != null) return null;
      return { action: { cell }, taunt: j.taunt ? String(j.taunt).slice(0, 120) : null };
    },
  },

  render(api, state) {
    const ctx = api.ctx, W = api.width, H = api.height;
    const S = Math.min(W, H) * 0.9, ox = (W - S) / 2, oy = (H - S) / 2, c = S / 3;
    ctx.fillStyle = '#0e1116'; ctx.fillRect(0, 0, W, H);

    // grid
    ctx.strokeStyle = '#39414d'; ctx.lineWidth = 3;
    for (let i = 1; i < 3; i++) {
      ctx.beginPath(); ctx.moveTo(ox + i * c, oy); ctx.lineTo(ox + i * c, oy + S); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(ox, oy + i * c); ctx.lineTo(ox + S, oy + i * c); ctx.stroke();
    }
    // gamepad cursor highlight
    const cur = api.cursor();
    if (cur != null && winnerOf(state.board) == null) {
      const hx = ox + (cur % 3) * c, hy = oy + ((cur / 3) | 0) * c;
      ctx.strokeStyle = '#fbbf24'; ctx.lineWidth = 3; ctx.strokeRect(hx + 4, hy + 4, c - 8, c - 8);
    }
    // marks (seat 0 = X cyan, seat 1 = O magenta)
    ctx.lineWidth = Math.max(4, c * 0.06);
    for (let i = 0; i < 9; i++) {
      const v = state.board[i]; if (v == null) continue;
      const cx = ox + (i % 3) * c + c / 2, cy = oy + ((i / 3) | 0) * c + c / 2, r = c * 0.28;
      if (v === state.seats[0]) {
        ctx.strokeStyle = '#22d3ee';
        ctx.beginPath(); ctx.moveTo(cx - r, cy - r); ctx.lineTo(cx + r, cy + r);
        ctx.moveTo(cx + r, cy - r); ctx.lineTo(cx - r, cy + r); ctx.stroke();
      } else {
        ctx.strokeStyle = '#e879f9';
        ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.stroke();
      }
    }
    // status
    const over = winnerOf(state.board);
    ctx.fillStyle = '#cbd5e1'; ctx.font = '16px system-ui, sans-serif'; ctx.textAlign = 'center';
    let msg;
    if (over != null) msg = `Vince ${state.names[state.seats.indexOf(over)] || 'Seat ' + over}`;
    else if (state.board.every(x => x != null)) msg = 'Pareggio';
    else { const t = state.seats[state.turn]; msg = (t === api.mySeat() ? 'Tocca a te' : `Turno: ${state.names[state.turn]}`); }
    ctx.fillText(msg, W / 2, oy - 10 > 16 ? oy - 10 : 16);
    drawBrain(ctx, state.brain, H);
  },
});

// Honest LLM badge for turn-based games: green only when the model just moved, red when blocked.
function drawBrain(ctx, brain, H) {
  if (!brain) return;
  ctx.textAlign = 'left'; ctx.font = '12px system-ui, sans-serif';
  ctx.fillStyle = brain.status === 'live' ? '#34d399' : brain.status === 'down' ? '#fb7185' : '#fbbf24';
  const icon = brain.status === 'live' ? '🟢' : brain.status === 'down' ? '⛔' : '…';
  const tail = brain.status === 'down' ? ' — in attesa' : (brain.ms ? ' ' + brain.ms + 'ms' : '');
  ctx.fillText(`${icon} ${brain.label || 'LLM'}${tail}`, 8, H - 8);
}
function exj(t) { if (!t) return null; const a = t.indexOf('{'), b = t.lastIndexOf('}'); if (a < 0 || b <= a) return null; try { return JSON.parse(t.slice(a, b + 1)); } catch { return null; } }

function hitCell(x, y, api) {
  const W = api.width, H = api.height, S = Math.min(W, H) * 0.9, ox = (W - S) / 2, oy = (H - S) / 2, c = S / 3;
  if (x < ox || x > ox + S || y < oy || y > oy + S) return { cell: -1 };
  const col = Math.min(2, Math.floor((x - ox) / c)), row = Math.min(2, Math.floor((y - oy) / c));
  return { cell: row * 3 + col };
}
