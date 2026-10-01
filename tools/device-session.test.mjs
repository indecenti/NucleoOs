// tools/lib/device-session.mjs — the dev tools share ONE paired session per device instead of minting a new
// token per run (32 LRU slots on the device: a day of pushes evicted the user's browser session).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { rmSync, existsSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { deviceSession } from './lib/device-session.mjs';

const CACHE = fileURLToPath(new URL('./.device-sessions.json', import.meta.url));
const HOST = 'http://test-device.invalid';

function fakeDevice() {
  let n = 0; const valid = new Set(); const calls = [];
  const f = async (url, opt = {}) => {
    const p = new URL(url).pathname; calls.push(p);
    if (p === '/api/pair') { const tok = 'nucleo_session=t' + (++n); valid.add(tok); return new Response('{}', { status: 200, headers: { 'set-cookie': tok + '; Path=/; HttpOnly' } }); }
    if (p === '/api/cpu') return new Response('{}', { status: valid.has(opt.headers && opt.headers.cookie) ? 200 : 401 });
    return new Response('', { status: 404 });
  };
  return { f, calls, valid, pairs: () => calls.filter((c) => c === '/api/pair').length };
}

test('pairs once, then reuses the cached session; re-pairs only when the device refuses it', async () => {
  const keep = existsSync(CACHE) ? readFileSync(CACHE, 'utf8') : null;
  try {
    if (keep) { const o = JSON.parse(keep); delete o[HOST]; writeFileSync(CACHE, JSON.stringify(o)); }
    const d = fakeDevice();
    const a = await deviceSession(HOST, '123456', { fetch: d.f });
    const b = await deviceSession(HOST, '123456', { fetch: d.f });
    assert.equal(a, b); assert.equal(d.pairs(), 1, 'the second run reused the session');
    d.valid.clear();                                           // evicted / device reset
    const c = await deviceSession(HOST, '123456', { fetch: d.f });
    assert.notEqual(c, a); assert.equal(d.pairs(), 2);
  } finally {
    if (keep != null) writeFileSync(CACHE, keep); else rmSync(CACHE, { force: true });
  }
});
