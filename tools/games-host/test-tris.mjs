// Host-side logic test for the reference game (no device, no browser).
// Mounts the REAL apps/games/www/games/tris.js by shimming the defineGame import.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
mkdirSync(join(here, '_t'), { recursive: true });

// Shim: defineGame just returns the def object so we can poke its pure functions.
writeFileSync(join(here, '_t', 'shim.mjs'), 'export const defineGame = d => d;\nexport default defineGame;\n');

let src = readFileSync(join(root, 'apps/games/www/games/tris.js'), 'utf8');
src = src.replace("'/apps/games/nucleo-game.js'", "'./shim.mjs'");
writeFileSync(join(here, '_t', 'tris.mjs'), src);

const g = (await import(pathToFileURL(join(here, '_t', 'tris.mjs')).href)).default;

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; } else { fail++; console.log('  FAIL:', m); } };

// --- setup ---
const players = [{ seat: 0, name: 'Ada' }, { seat: 1, name: 'Bob' }];
let s = g.setup(players);
ok(s.board.length === 9 && s.board.every(x => x === null), 'empty board');
ok(s.turn === 0, 'seat 0 starts');

// --- reduce: legal move by current seat ---
s = g.reduce(s, { cell: 4 }, 0);
ok(s.board[4] === 0 && s.turn === 1, 'seat0 plays center, turn->1');

// --- reduce: wrong seat ignored ---
const s2 = g.reduce(s, { cell: 0 }, 0);   // not seat0's turn
ok(s2 === s, 'out-of-turn move rejected');

// --- reduce: occupied cell ignored ---
const s3 = g.reduce(s, { cell: 4 }, 1);
ok(s3 === s, 'occupied cell rejected');

// --- win detection ---
let w = g.setup(players);
w = g.reduce(w, { cell: 0 }, 0); // X
w = g.reduce(w, { cell: 3 }, 1); // O
w = g.reduce(w, { cell: 1 }, 0); // X
w = g.reduce(w, { cell: 4 }, 1); // O
ok(g.isOver(w) === null, 'not over mid-game');
w = g.reduce(w, { cell: 2 }, 0); // X completes top row
const over = g.isOver(w);
ok(over && over.winner === 0, 'seat0 wins top row');

// --- draw detection ---
// X O X / X O O / O X X  -> full, no line
let d = g.setup(players);
const moves = [[0,0],[1,1],[2,0],[4,1],[3,0],[5,1],[7,0],[6,1],[8,0]];
for (const [cell, seat] of moves) d = g.reduce(d, { cell }, seat);
const dr = g.isOver(d);
ok(d.board.every(x => x != null), 'board full for draw case');
ok(dr && dr.winner === -1, 'draw detected');

// --- AI: takes the winning move ---
let a = g.setup(players);
a = g.reduce(a, { cell: 0 }, 0); // seat0
a = g.reduce(a, { cell: 8 }, 1); // seat1
a = g.reduce(a, { cell: 1 }, 0); // seat0 now threatens 0,1,2
// AI for seat1 should BLOCK at cell 2 (opponent about to win)
const aimove = g.ai(a, 1);
ok(aimove && aimove.cell === 2, 'AI blocks opponent win at cell 2 (got ' + JSON.stringify(aimove) + ')');

// --- AI: prefers its own win over a block ---
let b = g.setup(players);
b.board = [1, 1, null, 0, 0, null, null, null, null]; b.turn = 1;
const aiwin = g.ai(b, 1);
ok(aiwin && aiwin.cell === 2, 'AI takes its own win at cell 2 (got ' + JSON.stringify(aiwin) + ')');

// --- onKey numpad mapping (key '1' -> bottom-left cell 6) ---
ok(g.onKey('1').cell === 6 && g.onKey('9').cell === 2 && g.onKey('5').cell === 4, 'numpad key mapping');
ok(g.onKey('x') === null, 'non-digit key ignored');

// --- gamepad cursor navigation ---
ok(g.padMode === 'cursor', 'tris padMode cursor');
const tc = (() => { let c = null; return { cursor: () => c, setCursor: v => c = v }; })();
g.onPadDir(1, 0, tc); ok(tc.cursor() === 5, 'pad: center(4) + right -> cell 5 (got ' + tc.cursor() + ')');
g.onPadDir(0, -1, tc); ok(tc.cursor() === 2, 'pad: up -> cell 2');
g.onPadDir(1, 0, tc); ok(tc.cursor() === 2, 'pad: right clamped at edge');
ok(g.onPadButton('A', tc).cell === 2, 'pad: A confirms cursor cell');
ok(g.onPadButton('B', tc) === null, 'pad: non-A button ignored');

// --- LLM mover contract (Grok plays Tris) ---
ok(typeof g.llm.prompt === 'function' && typeof g.llm.parse === 'function', 'tris exposes the llm mover contract');
let L = g.setup(players);                                  // seat 0's turn
ok(g.llm.prompt(L, 1) === null, 'llm.prompt null when it is not the seat turn');
const req = g.llm.prompt(L, 0);
ok(req && req.system.includes('JSON') && req.user.includes('Celle libere'), 'llm.prompt builds a request listing legal cells');
ok(g.llm.parse('{"cell":4,"taunt":"facile"}', L).action.cell === 4, 'llm.parse accepts a legal cell + taunt');
ok(g.llm.parse('{"cell":4}', L).taunt === null, 'llm.parse: missing taunt -> null');
ok(g.llm.parse('non-json', L) === null, 'llm.parse rejects non-JSON');
ok(g.llm.parse('{"cell":99}', L) === null, 'llm.parse rejects out-of-range cell');
const occ = g.reduce(L, { cell: 4 }, 0);                   // cell 4 now taken, turn -> 1
ok(g.llm.parse('{"cell":4}', occ) === null, 'llm.parse rejects an occupied cell');
ok(g.llm.prompt(occ, 0) === null, 'llm.prompt null off-turn after a move');
// brain/policy actions must update status, never count as a move
const br = g.reduce(L, { type: 'brain', brain: { status: 'live', label: 'Grok', ms: 200 } }, -1);
ok(br.brain && br.brain.status === 'live' && br.board.every(x => x === null), 'brain action sets state.brain, board untouched');
ok(g.reduce(L, { type: 'policy', seat: 1, params: {} }, -1) === L, 'typed policy action is a no-op (never a move)');

console.log(`\nTris logic: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
