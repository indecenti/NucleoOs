// Host tests for the Game Center's two pieces of player progress on the SD card, with fetch stubbed
// (no device, no browser):
//   - the gamercard stats (/data/play/stats.json, apps/games/www/profile.js): recording a result is a
//     read-modify-write, so a read that FAILS (device busy 503, 401, network drop, garbled body) must
//     never be mistaken for "no stats yet" — that rewrote the file with this one match and wiped the
//     player's whole win/loss record;
//   - the Costellazioni 3D campaign shared with the Cardputer (constellations-save.js): the save guard
//     re-reads the device copy so a run the Cardputer moved forward wins; when that re-read fails, or
//     the card holds a save from a NEWER firmware, the web must not write blind over it.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const WWW = join(ROOT, 'apps', 'games', 'www');

// profile.js imports the browser-absolute '/apps/games/nucleo-play.js'; load a copy that points at the real file.
const TMP = mkdtempSync(join(tmpdir(), 'games-save-'));
process.on('exit', () => { try { rmSync(TMP, { recursive: true, force: true }); } catch {} });
writeFileSync(join(TMP, 'profile.mjs'), readFileSync(join(WWW, 'profile.js'), 'utf8')
  .split("'/apps/games/nucleo-play.js'").join(JSON.stringify(pathToFileURL(join(WWW, 'nucleo-play.js')).href)));
globalThis.localStorage = { _d: {}, getItem(k) { return this._d[k] ?? null; }, setItem(k, v) { this._d[k] = String(v); } };
const P = await import(pathToFileURL(join(TMP, 'profile.mjs')).href);
const S = await import(pathToFileURL(join(WWW, 'games', 'constellations-save.js')).href);

// A tiny device: a map of SD files + per-path read faults. Records every write.
function device(files = {}) {
  const dev = { files: { ...files }, writes: [], faults: {} };
  globalThis.fetch = async (url, opts = {}) => {
    const u = new URL(url, 'http://dev');
    const path = u.searchParams.get('path') || u.pathname;
    const method = (opts.method || 'GET').toUpperCase();
    const fault = dev.faults[path];
    if (method === 'GET' && fault) {
      if (fault === 'network') throw new TypeError('Failed to fetch');
      if (typeof fault === 'number') return new Response(JSON.stringify({ error: 'sim fault' }), { status: fault });
      return new Response(fault, { status: 200 });                     // a garbled / truncated body
    }
    if (u.pathname === '/api/fs/mkdir') return new Response('{}', { status: 200 });
    if (u.pathname === '/api/fs/write' || (u.pathname === '/api/game/costellazioni/save' && method === 'POST')) {
      const body = typeof opts.body === 'string' ? opts.body : await new Response(opts.body).text();
      dev.writes.push(path); dev.files[path] = body;
      return new Response(JSON.stringify({ ok: true }), { status: 200 });
    }
    if (!(path in dev.files)) return new Response(JSON.stringify({ error: 'nosave' }), { status: 404 });
    return new Response(dev.files[path], { status: 200 });
  };
  return dev;
}

const STATS = '/data/play/stats.json';
const MINE = { tris: { w: 7, l: 2, d: 1, games: 10, streak: 3, best: 4 }, pong: { w: 1, l: 5, d: 0, games: 6, streak: 0, best: 1 },
  _total: { w: 8, l: 7, d: 1, games: 16, streak: 3, best: 4 } };

test('stats: a result is added to the record on the card', async () => {
  const dev = device({ [STATS]: JSON.stringify(MINE) });
  const next = await P.recordResult('tris', 'win');
  assert.deepEqual(next.tris, { w: 8, l: 2, d: 1, games: 11, streak: 4, best: 4 });
  assert.deepEqual(next.pong, MINE.pong, 'other games untouched');
  assert.equal(next._total.games, 17);
  assert.deepEqual(JSON.parse(dev.files[STATS]), next, 'and that is exactly what was written');
});

test('stats: the very first result (no stats.json yet) creates the file', async () => {
  const dev = device();
  const next = await P.recordResult('forza4', 'draw');
  assert.deepEqual(next.forza4, { w: 0, l: 0, d: 1, games: 1, streak: 0, best: 0 });
  assert.deepEqual(dev.writes, [STATS]);
});

for (const [why, fault] of [['device busy (503)', 503], ['not paired (401)', 401], ['network drop', 'network'], ['garbled body', '{"tris":{"w":7,']]) {
  test(`stats: a failed read (${why}) never overwrites the record`, async () => {
    const dev = device({ [STATS]: JSON.stringify(MINE) });
    dev.faults[STATS] = fault;
    await assert.rejects(P.recordResult('tris', 'loss'), 'the caller is told the result was not recorded');
    assert.deepEqual(dev.writes, [], 'nothing written');
    assert.deepEqual(JSON.parse(dev.files[STATS]), MINE, 'the record on the card is intact');
  });
}

const SAVE = '/api/game/costellazioni/save';
const run = (o) => ({ ...S.newSave(0xC0FFEE), ...o });

test('costellazioni: a save is written (normalized) when the device copy is not ahead', async () => {
  const dev = device({ [SAVE]: JSON.stringify(run({ epoch: 3, credits: 100 })) });
  const r = await S.storeSave(run({ epoch: 4, credits: 900 }));
  assert.equal(r.ok, true);
  assert.deepEqual(dev.writes, [SAVE]);
  const body = JSON.parse(dev.files[SAVE]);
  assert.equal(body.epoch, 4); assert.equal(body.credits, 900); assert.equal(body.ver, S.SAVE_VER);
  assert.equal(body.cargo.length, 8); assert.equal(body.rep.length, 4);
});

test('costellazioni: the Cardputer moved the run forward -> conflict, the device copy wins, nothing written', async () => {
  const disk = run({ sector: 2, epoch: 1 });
  const dev = device({ [SAVE]: JSON.stringify(disk) });
  const r = await S.storeSave(run({ sector: 1, epoch: 40, credits: 99999 }));
  assert.equal(r.ok, false); assert.equal(r.conflict, true);
  assert.equal(r.disk.sector, 2);
  assert.deepEqual(dev.writes, []);
});

test('costellazioni: no save on the card yet (404) -> a new run is written', async () => {
  const dev = device();
  assert.equal(await S.loadSave(), null, 'no run to continue');
  const r = await S.storeSave(S.newSave(1));
  assert.equal(r.ok, true);
  assert.deepEqual(dev.writes, [SAVE]);
});

for (const [why, fault] of [['device busy (503)', 503], ['network drop', 'network'], ['garbled body', '{"ver":3,"sec']]) {
  test(`costellazioni: the guard re-read fails (${why}) -> refuse to write over the Cardputer run`, async () => {
    const disk = JSON.stringify(run({ sector: 5, epoch: 80, kills: 300 }));
    const dev = device({ [SAVE]: disk });
    dev.faults[SAVE] = fault;
    await assert.rejects(S.storeSave(S.newSave(7)), 'the save is refused, not silently written');
    assert.deepEqual(dev.writes, []);
    assert.equal(dev.files[SAVE], disk, 'the progressed run on the card is intact');
  });
}

test('costellazioni: a save from a NEWER firmware is not continued here, and never overwritten by this older format', async () => {
  const future = JSON.stringify({ ...run({ sector: 9, epoch: 300 }), ver: S.SAVE_VER + 1, newfield: [1, 2, 3] });
  const dev = device({ [SAVE]: future });
  assert.equal(await S.loadSave(), null, 'nothing to continue in this version');
  await assert.rejects(S.storeSave(S.newSave(3)));
  assert.deepEqual(dev.writes, []);
  assert.equal(dev.files[SAVE], future);
});
