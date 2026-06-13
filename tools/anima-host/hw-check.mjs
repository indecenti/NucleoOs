// Host gate for the hardware capability manifest (nucleo-hw.js). Mirrors the agent-tools.js
// host-test pattern: the manifest + callCapability are pure (fetch injected), so we verify — with a
// MOCK fetch, no device — that each capability maps to the right /api request and that the derived
// agent tools are well-formed. Wired as `npm run hw:test`.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  HW_CAPABILITIES, capabilityIds, isCapability, callCapability,
  toAgentTools, toolName, capabilityForTool, HW_MUTATING,
  validateArgs, resolveOffline, GPIO_WRITE_PINS, GPIO_READ_PINS,
} from '../../apps/code-runner/www/nucleo-hw.js';

// A fetch double that records the call and returns a canned JSON body.
function mockFetch(jsonBody = { ok: true }) {
  const calls = [];
  const fn = async (url, init) => {
    calls.push({ url, init: init || {} });
    return { ok: true, status: 200, text: async () => JSON.stringify(jsonBody) };
  };
  fn.calls = calls;
  return fn;
}

test('manifest has the expected capabilities', () => {
  const ids = capabilityIds();
  for (const id of ['ir.send', 'ir.tvbgone', 'ir.jammer', 'wifi.scan', 'gpio.write', 'gpio.read', 'sys.status'])
    assert.ok(ids.includes(id), 'missing capability ' + id);
  assert.ok(isCapability('ir.send'));
  assert.ok(!isCapability('ir.nope'));
});

test('POST capability sends a JSON body to the mapped endpoint', async () => {
  const f = mockFetch({ ok: true });
  await callCapability('ir.send', { protocol: 'nec', address: 4, command: 8 }, f);
  assert.equal(f.calls.length, 1);
  assert.equal(f.calls[0].url, '/api/ir/send');
  assert.equal(f.calls[0].init.method, 'POST');
  assert.deepEqual(JSON.parse(f.calls[0].init.body), { protocol: 'nec', address: 4, command: 8 });
});

test('GET capability appends query params', async () => {
  const f = mockFetch({ ok: true, pin: 2, value: 1 });
  const r = await callCapability('gpio.read', { pin: 2 }, f);
  assert.equal(f.calls[0].url, '/api/gpio?pin=2');
  assert.equal(f.calls[0].init.method, undefined);   // GET (no method override)
  assert.equal(r.value, 1);
});

test('wifi.scan reshapes the response via pick()', async () => {
  const f = mockFetch({ networks: [{ ssid: 'home', rssi: -40 }] });
  const r = await callCapability('wifi.scan', {}, f);
  assert.ok(Array.isArray(r));
  assert.equal(r[0].ssid, 'home');
});

test('a non-OK response throws', async () => {
  const f = async () => ({ ok: false, status: 503, text: async () => '' });
  await assert.rejects(() => callCapability('ir.send', {}, f), /HTTP 503/);
});

test('agent tools derive from the manifest (names, schemas, mutating set)', () => {
  const tools = toAgentTools();
  const names = tools.map((t) => t.name);
  assert.ok(names.includes('ir_send'));
  assert.ok(names.includes('gpio_write'));
  assert.ok(names.includes('wifi_scan'));
  for (const t of tools) {
    assert.equal(typeof t.description, 'string');
    assert.equal(t.input_schema.type, 'object');
  }
  assert.equal(toolName('ir.send'), 'ir_send');
  assert.equal(capabilityForTool('gpio_write'), 'gpio.write');
  // side-effect tools are gated; read tools are not.
  assert.ok(HW_MUTATING.has('ir_send'));
  assert.ok(HW_MUTATING.has('gpio_write'));
  assert.ok(!HW_MUTATING.has('wifi_scan'));
});

test('every capability is well-formed', () => {
  for (const c of HW_CAPABILITIES) {
    assert.ok(c.id && c.ns && c.action, 'id/ns/action');
    assert.ok(['read', 'act'].includes(c.kind), c.id + ' kind');
    assert.ok(c.endpoint && c.endpoint.path && c.endpoint.method, c.id + ' endpoint');
    assert.ok(c.args && c.args.type === 'object', c.id + ' args schema');
  }
});

// ───────────────────────── grounding / validateArgs ─────────────────────────
test('validateArgs enforces required, type and enum', () => {
  assert.ok(validateArgs('gpio.write', { pin: 2, value: 1 }).ok);
  assert.ok(validateArgs('ir.send', { protocol: 'nec', command: 8 }).ok);          // address optional
  assert.equal(validateArgs('gpio.write', { pin: 2 }).ok, false);                  // missing value
  assert.match(validateArgs('gpio.write', { pin: 2 }).error, /required "value"/);
  assert.equal(validateArgs('gpio.write', { pin: 2, value: 5 }).ok, false);        // value enum 0|1
  assert.equal(validateArgs('ir.send', { command: 'x' }).ok, false);              // wrong type
  assert.equal(validateArgs('ir.tvbgone', { action: 'boom' }).ok, false);         // enum
  assert.equal(validateArgs('nope.x', {}).ok, false);                             // unknown capability
  assert.ok(validateArgs('wifi.scan', {}).ok);
});

test('validateArgs enforces the GPIO safe-pin allowlist (mirrors firmware)', () => {
  for (const p of GPIO_WRITE_PINS) assert.ok(validateArgs('gpio.write', { pin: p, value: 1 }).ok, 'write pin ' + p);
  assert.equal(validateArgs('gpio.write', { pin: 44, value: 1 }).ok, false);       // IR LED pin — never writable
  assert.match(validateArgs('gpio.write', { pin: 44, value: 1 }).error, /not writable/);
  for (const p of GPIO_READ_PINS) assert.ok(validateArgs('gpio.read', { pin: p }).ok, 'read pin ' + p);
  assert.equal(validateArgs('gpio.read', { pin: 5 }).ok, false);
});

test('callCapability VALIDATES before it ever touches the network', async () => {
  let hit = 0;
  const f = async () => { hit++; return { ok: true, status: 200, text: async () => '{}' }; };
  await assert.rejects(() => callCapability('gpio.write', { pin: 44, value: 1 }, f), /not writable/);
  assert.equal(hit, 0, 'a bad command must NOT reach the device');
});

// ───────────────────────── offline NL resolver ─────────────────────────
const NL_OFFLINE = [
  ['spegni la tv', 'ir.tvbgone'],
  ['spegni tutte le tv', 'ir.tvbgone'],
  ['tv-b-gone', 'ir.tvbgone'],
  ['scansiona le reti wifi', 'wifi.scan'],
  ['scan wifi', 'wifi.scan'],
  ['accendi gpio 2', 'gpio.write'],
  ['gpio 2 alto', 'gpio.write'],
  ['spegni gpio 1', 'gpio.write'],
  ['leggi gpio 0', 'gpio.read'],
  ['stato del dispositivo', 'sys.status'],
  ['ferma il jammer', 'ir.jammer'],
];

test('resolveOffline maps the high-value phrases (offline)', () => {
  for (const [phrase, id] of NL_OFFLINE) {
    const r = resolveOffline(phrase, {});
    assert.ok(r, 'no match for: ' + phrase);
    assert.equal(r.id, id, phrase + ' -> ' + (r && r.id));
  }
  assert.equal(resolveOffline('accendi gpio 2', {}).args.value, 1);
  assert.equal(resolveOffline('spegni gpio 1', {}).args.value, 0);
  assert.equal(resolveOffline('leggi gpio 0', {}).args.pin, 0);
  assert.equal(resolveOffline('ciao come stai oggi', {}), null);   // not a hardware command
  assert.equal(resolveOffline('', {}), null);
});

// THE contract the user demanded: offline NL must NEVER step on the online LLM.
test('resolveOffline NEVER fires online (the LLM owns online)', () => {
  for (const [phrase] of NL_OFFLINE)
    assert.equal(resolveOffline(phrase, { online: true }), null, 'must defer to LLM: ' + phrase);
  // even an explicit, unambiguous command yields to the LLM when online
  assert.equal(resolveOffline('accendi gpio 2', { online: true }), null);
});

test('NL resolution still passes through validation (layered safety)', () => {
  const r = resolveOffline('accendi gpio 44', {});      // parses to a write on the forbidden pin 44
  assert.ok(r && r.id === 'gpio.write' && r.args.pin === 44);
  assert.equal(validateArgs(r.id, r.args).ok, false);   // …and validation refuses it
});
