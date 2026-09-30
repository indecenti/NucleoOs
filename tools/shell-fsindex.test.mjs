// web/shell/fsindex.js — the file-search index must cost the Cardputer NOTHING until someone searches.
// Measured 2026-09-30 on the simulator: every cold boot walked /data (10 /api/fs/list on a fresh SD, up to
// 600 on a real one) during the session restore, and every save under /data re-walked it 1.5 s later.
// Pins: boot = no request; fs.changed = no request (unless a search is on screen); the first search crawls
// once and every later keystroke is answered from RAM; a change marks it stale for the next search.
import { test } from 'node:test';
import assert from 'node:assert/strict';

const SD = {
  '/data': [{ name: 'Documents', type: 'dir' }, { name: 'notes.txt', type: 'file', size: 10 }],
  '/data/Documents': [{ name: 'report.md', type: 'file', size: 99 }],
};
const lists = [];
globalThis.fetch = async (url) => {
  const p = decodeURIComponent(String(url).split('path=')[1] || '');
  lists.push(p);
  return new Response(JSON.stringify({ entries: SD[p] || [] }), { status: 200 });
};
const mem = new Map();
globalThis.localStorage = { getItem: (k) => mem.get(k) ?? null, setItem: (k, v) => mem.set(k, String(v)), removeItem: (k) => mem.delete(k) };
globalThis.location = { host: 'nucleo.test' };
const settle = () => new Promise((r) => setTimeout(r, 30));

test('no crawl at boot, one crawl on the first search, RAM afterwards, stale after a change', async () => {
  const F = await import('../web/shell/fsindex.js');
  let live = false;
  F.init({ live: () => live });
  await settle();
  assert.deepEqual(lists, [], 'boot must not touch the device');
  F.invalidate(); await settle();
  assert.deepEqual(lists, [], 'a change with no search open must not touch the device');

  F.warm(); F.warm(); F.warm();                     // three keystrokes while the crawl runs
  await settle();
  assert.deepEqual(lists, ['/data', '/data/Documents'], 'exactly one crawl');
  assert.deepEqual(F.search('report', 5).map((x) => x.path), ['/data/Documents/report.md']);
  F.warm(); await settle();
  assert.equal(lists.length, 2, 'the next search is answered from RAM');

  SD['/data'].push({ name: 'new.txt', type: 'file' });
  F.invalidate(); await settle();
  assert.equal(lists.length, 2, 'still no request: only marked stale');
  F.warm(); await settle();
  assert.equal(lists.length, 4, 'the next search re-crawls the stale index');
  assert.ok(F.search('new', 5).length === 1);

  live = true;                                      // a search is on screen: changes refresh it live
  F.invalidate();
  await new Promise((r) => setTimeout(r, 1700));
  assert.equal(lists.length, 6, 'live search: re-crawled after the debounce');
});

test('a returning browser searches instantly from the kept index, whatever its age', async () => {
  const F = await import('../web/shell/fsindex.js?again');
  mem.set('nucleo.fsindex.v1', JSON.stringify({ host: 'nucleo.test', builtAt: 1, items: [{ path: '/data/old.txt', name: 'old.txt', lower: 'old.txt', dir: '/data', isDir: false, ext: 'txt', cat: 'doc' }] }));
  const before = lists.length;
  F.init();
  assert.equal(F.search('old', 5)[0].path, '/data/old.txt', 'instant, before any request');
  assert.equal(lists.length, before, 'loading the kept index costs nothing');
});
