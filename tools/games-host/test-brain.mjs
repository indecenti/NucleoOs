// Integration test: the generic LLM Brain (mover mode) actually drives a turn-based game (Tris)
// through the real GameHarness, with a MOCKED LLM. Proves "Grok plays other games" end-to-end:
// harness brain-hook → Brain._decide → llm.parse → applyInput, plus the no-fallback status + blocking.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url)), root = join(here, '..', '..'), t = join(here, '_t');
mkdirSync(t, { recursive: true });

// Controllable LLM stub (replaces /apps/games/llm.js inside llm-brain.js).
writeFileSync(join(t, 'llm-stub.mjs'), `
export async function loadCfg(){ return { provider:'openai', base:'https://api.x.ai/v1', model:'grok-2-latest', key:'x' }; }
export function brandOf(){ return 'Grok'; }
export function providerLabel(){ return 'Grok (grok-2-latest)'; }
export function endpointHost(){ return 'api.x.ai'; }
export function extractJson(x){ if(!x) return null; const a=x.indexOf('{'),b=x.lastIndexOf('}'); if(a<0||b<=a) return null; try{return JSON.parse(x.slice(a,b+1));}catch{return null;} }
export async function ask(){ return (globalThis.__ASK ? globalThis.__ASK() : '{"cell":0}'); }
`);
writeFileSync(join(t, 'shim.mjs'), 'export const defineGame = d => d;\nexport default defineGame;\n');
writeFileSync(join(t, 'nucleo-game.mjs'), readFileSync(join(root, 'apps/games/www/nucleo-game.js'), 'utf8'));
writeFileSync(join(t, 'llm-brain.mjs'), readFileSync(join(root, 'apps/games/www/llm-brain.js'), 'utf8').replace("'/apps/games/llm.js'", "'./llm-stub.mjs'"));
writeFileSync(join(t, 'tris2.mjs'), readFileSync(join(root, 'apps/games/www/games/tris.js'), 'utf8').replace("'/apps/games/nucleo-game.js'", "'./shim.mjs'"));

const { GameHarness } = await import(pathToFileURL(join(t, 'nucleo-game.mjs')).href);
const { Brain, brainMode } = await import(pathToFileURL(join(t, 'llm-brain.mjs')).href);
const tris = (await import(pathToFileURL(join(t, 'tris2.mjs')).href)).default;

let pass = 0, fail = 0; const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Minimal host "play" with one human (seat 0) and one AI (seat 1).
const play = {
  isHost: true, seat: 0, roomId: 'r_test', spectator: false, peerId: 'p0',
  roster: [{ seat: 0, name: 'You', ai: false, peerId: 'p0' }, { seat: 1, name: 'Grok', ai: true, peerId: 'p1' }],
  _h: {}, on(e, f) { (this._h[e] = this._h[e] || []).push(f); }, send() {},
};
const canvas = { width: 480, height: 480, getContext: () => ({}) };   // tris is 2D; we never run the RAF loop
const harness = new GameHarness(play, tris, canvas);

ok(brainMode(tris) === 'mover', 'brainMode(tris) === mover');

// Start a match (no start() → no requestAnimationFrame needed in node).
harness.begin({ mente: 'llm' });
ok(harness.state && harness.state.board.length === 9, 'match started, board ready');

// Attach the generic Brain in mover mode.
const taunts = [];
const ui = { sys() {}, taunt: x => taunts.push(x), note() {}, ok() {}, fail() {} };
const brain = new Brain({ harness, def: tris, play, ui, cfg: await (await import(pathToFileURL(join(t, 'llm-stub.mjs')).href)).loadCfg() });
globalThis.__ASK = () => '{"cell":0,"taunt":"troppo facile"}';
brain.start({ mente: 'llm' });
await sleep(50);
ok(harness.state.brain && ['connecting', 'live'].includes(harness.state.brain.status), 'brain status attached to state');

// Human (seat 0) plays center; the AI's turn then belongs to the LLM.
harness._applyInput({ cell: 4 }, 0);
ok(harness.state.board[4] === 0 && harness.state.turn === 1, 'human played center, AI to move');

// Wait for the Brain to decide (aiThinkMs 500 + stubbed ask).
await sleep(900);
ok(harness.state.board[0] === 1, 'LLM (mover) actually placed seat-1 move at cell 0');
ok(harness.state.turn === 0, 'turn returned to human after the LLM move');
ok(harness.state.brain.status === 'live', 'brain status = live after a confirmed move');
ok(taunts.includes('troppo facile'), 'LLM taunt surfaced to chat');

// Now simulate the LLM going DOWN: it must NOT fall back to the local heuristic.
globalThis.__ASK = () => '';                         // empty = failure
const boardBefore = harness.state.board.slice();
harness._applyInput({ cell: 8 }, 0);                 // human moves; AI's turn again
await sleep(900);
ok(harness.state.brain.status === 'down', 'brain status = down when LLM unreachable');
const aiCells = harness.state.board.filter((v, i) => v === 1).length;
ok(aiCells === 1, 'NO offline fallback move was played while the LLM was down (still 1 AI mark)');

// Recover: the LLM responds again → the move lands and status returns to live.
globalThis.__ASK = () => '{"cell":2}';
harness.kickAI();
await sleep(900);
ok(harness.state.board[2] === 1, 'after recovery the LLM move lands');
ok(harness.state.brain.status === 'live', 'status back to live after recovery');

brain.stop(); harness.stop();
console.log(`\nBrain integration: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
