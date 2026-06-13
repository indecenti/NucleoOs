// Host-side logic test for forza4.js (mounts the real file via a defineGame shim).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
mkdirSync(join(here, '_t'), { recursive: true });
writeFileSync(join(here, '_t', 'shim.mjs'), 'export const defineGame = d => d;\nexport default defineGame;\n');
let src = readFileSync(join(root, 'apps/games/www/games/forza4.js'), 'utf8').replace("'/apps/games/nucleo-game.js'", "'./shim.mjs'");
writeFileSync(join(here, '_t', 'forza4.mjs'), src);
const g = (await import(pathToFileURL(join(here, '_t', 'forza4.mjs')).href)).default;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };
const players = [{ seat: 0, name: 'A' }, { seat: 1, name: 'B' }];

// setup + gravity
let s = g.setup(players, 1, { level: 'medio' });
ok(s.board.length === 42 && s.board.every(x => x == null), 'empty 7x6 board');
s = g.reduce(s, { col: 3 }, 0);
ok(s.board[5 * 7 + 3] === 0 && s.turn === 1, 'disc falls to bottom row of column');
s = g.reduce(s, { col: 3 }, 1);
ok(s.board[4 * 7 + 3] === 1, 'second disc stacks above');

// out of turn rejected
ok(g.reduce(s, { col: 0 }, 1) === s, 'out-of-turn rejected');

// vertical win for seat 0
let v = g.setup(players, 1, {});
for (let i = 0; i < 3; i++) { v = g.reduce(v, { col: 0 }, 0); v = g.reduce(v, { col: 1 }, 1); }
ok(g.isOver(v) === null, 'no win after 3 stacked');
v = g.reduce(v, { col: 0 }, 0);   // 4th in column 0
const ov = g.isOver(v); ok(ov && ov.winner === 0, 'vertical four wins');

// horizontal win
let h = g.setup(players, 1, {});
h = g.reduce(h, { col: 0 }, 0); h = g.reduce(h, { col: 0 }, 1);
h = g.reduce(h, { col: 1 }, 0); h = g.reduce(h, { col: 1 }, 1);
h = g.reduce(h, { col: 2 }, 0); h = g.reduce(h, { col: 2 }, 1);
h = g.reduce(h, { col: 3 }, 0);
const oh = g.isOver(h); ok(oh && oh.winner === 0, 'horizontal four wins');

// AI takes immediate win
let aw = g.setup(players, 1, { level: 'facile' });
aw.board[5 * 7 + 0] = 1; aw.board[5 * 7 + 1] = 1; aw.board[5 * 7 + 2] = 1; aw.turn = 1; aw.seats = [0, 1];
const aiW = g.ai(aw, 1); ok(aiW && aiW.col === 3, 'AI completes its own horizontal win (col 3)');

// AI blocks opponent's immediate win
let ab = g.setup(players, 1, { level: 'facile' });
ab.board[5 * 7 + 0] = 0; ab.board[5 * 7 + 1] = 0; ab.board[5 * 7 + 2] = 0; ab.turn = 1; ab.seats = [0, 1];
const aiB = g.ai(ab, 1); ok(aiB && aiB.col === 3, 'AI blocks opponent win (col 3)');

// input mapping
ok(g.onKey('4').col === 3 && g.onKey('7').col === 6, 'numpad keys map to columns');
ok(g.onKey('8') === null, 'out-of-range key ignored');

// options surfaced for the waiting room
ok(Array.isArray(g.options) && g.options[0].key === 'level', 'declares level option');

// --- gamepad cursor navigation ---
ok(g.padMode === 'cursor', 'forza4 padMode cursor');
const fc = (() => { let c = null; return { cursor: () => c, setCursor: v => c = v }; })();
g.onPadDir(1, 0, fc); ok(fc.cursor() === 4, 'pad: default col 3 + right -> 4');
g.onPadDir(-1, 0, fc); ok(fc.cursor() === 3, 'pad: left -> 3');
for (let i = 0; i < 9; i++) g.onPadDir(-1, 0, fc); ok(fc.cursor() === 0, 'pad: clamps at left edge');
ok(g.onPadButton('A', fc).col === 0, 'pad: A drops in cursor column');

// mente option for the LLM brain
ok(g.options.some(o => o.key === 'mente'), 'declares mente option (classic/LLM)');

// --- LLM mover contract (Grok plays Forza 4) ---
ok(typeof g.llm.prompt === 'function' && typeof g.llm.parse === 'function', 'forza4 exposes the llm mover contract');
let L = g.setup(players, 1, {});                            // seat 0's turn
ok(g.llm.prompt(L, 1) === null, 'llm.prompt null off-turn');
const req = g.llm.prompt(L, 0);
ok(req && req.system.includes('JSON') && req.user.includes('Colonne giocabili'), 'llm.prompt lists playable columns');
ok(g.llm.parse('{"col":3,"taunt":"qui"}', L).action.col === 3, 'llm.parse accepts a legal column + taunt');
ok(g.llm.parse('{"col":9}', L) === null, 'llm.parse rejects out-of-range column');
ok(g.llm.parse('nope', L) === null, 'llm.parse rejects non-JSON');
// fill column 3 to the top, then it must be rejected as full
let full = g.setup(players, 1, {});
for (let i = 0; i < 6; i++) full = g.reduce(full, { col: 3 }, full.seats[full.turn]);
ok(full.board[0 * 7 + 3] != null, 'column 3 filled to the top');
ok(g.llm.parse('{"col":3}', full) === null, 'llm.parse rejects a full column');
// brain action sets status, never a move
const br = g.reduce(L, { type: 'brain', brain: { status: 'down', label: 'Grok' } }, -1);
ok(br.brain && br.brain.status === 'down' && br.board.every(x => x == null), 'brain action sets state.brain, board untouched');

// --- evolved: last/moves/win tracking (drives the 3D renderer's drop animation + win highlight) ---
let e1 = g.setup(players, 1, {});
e1 = g.reduce(e1, { col: 3 }, 0);
ok(e1.moves === 1 && e1.last && e1.last.col === 3 && e1.last.row === 5 && e1.last.seat === 0, 'reduce stamps last move + move counter');
ok(e1.win === null, 'win null mid-game');
let wl = g.setup(players, 1, {});
wl.board[5 * 7 + 0] = 0; wl.board[5 * 7 + 1] = 0; wl.board[5 * 7 + 2] = 0; wl.turn = 0; wl.seats = [0, 1];
const wls = g.reduce(wl, { col: 3 }, 0);
ok(Array.isArray(wls.win) && wls.win.length === 4, 'reduce stamps the winning 4-cell line');
ok(wls.win.every(c => wls.board[c.r * 7 + c.c] === 0), 'winning cells all belong to the winner');

// --- "difficile" negamax opponent ---
let dw = g.setup(players, 1, { level: 'difficile' });
dw.board[5 * 7 + 0] = 0; dw.board[5 * 7 + 1] = 0; dw.board[5 * 7 + 2] = 0; dw.turn = 0; dw.seats = [0, 1];
ok(g.ai(dw, 0).col === 3, 'negamax takes the immediate win');
let db = g.setup(players, 1, { level: 'difficile' });
db.board[5 * 7 + 0] = 1; db.board[5 * 7 + 1] = 1; db.board[5 * 7 + 2] = 1; db.turn = 0; db.seats = [0, 1];
ok(g.ai(db, 0).col === 3, 'negamax blocks the opponent immediate win');
const t0 = Date.now();
const dOpen = g.ai(g.setup(players, 1, { level: 'difficile' }), 0);
ok(dOpen && dOpen.col >= 0 && dOpen.col < 7, 'negamax returns a legal column on the empty board');
ok(Date.now() - t0 < 2000, 'negamax responds well within the think budget');
ok(g.options.some(o => o.key === 'level' && o.choices.some(c => c.value === 'difficile')), 'level option offers difficile');

// --- 3D render contract: the hub hands forza4 a Three.js surface (same engine as Pong) ---
ok(g.renderMode === '3d' && g.fillViewport === true && g.capturesPointer === false, 'forza4 declares the 3D surface contract');
ok(['mount', 'render', 'resize', 'unmount'].every(k => typeof g[k] === 'function'), 'forza4 exposes the renderer lifecycle hooks');
// onPointer is renderer-driven: with no renderer loaded (host/Node) it must no-op, never throw
ok(g.onPointer(100, 200, { width: 800, height: 500 }) === null, 'onPointer no-ops until the renderer is ready');

// --- 3D renderer parses + exports (catches syntax/identifier regressions; WebGL itself is browser-verified) ---
let r3dOK = false;
try {
  const rsrc = readFileSync(join(root, 'apps/games/www/games/forza4-3d.js'), 'utf8')
    .replace("import { SFX } from '/apps/games/games/pong-sfx.js';", 'const SFX = class { constructor() {} };');
  writeFileSync(join(here, '_t', 'forza4-3d.mjs'), rsrc);
  const r3d = await import(pathToFileURL(join(here, '_t', 'forza4-3d.mjs')).href);
  r3dOK = typeof r3d.createForza4Renderer === 'function';
} catch (err) { console.log('  forza4-3d load error:', err.message); }
ok(r3dOK, 'forza4-3d.js parses and exports createForza4Renderer');

console.log(`\nForza4 logic: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
