// Host tests for the FIRST real enforcement of a manifest `permissions` entry (web/shell/wm.js).
//
// Background: `permissions` was declarative-only — a grep across the whole repo found no consumer
// except app-publish.js copying the array into the registry. Because every app runs SAME-ORIGIN in
// the shell, the browser's default Feature Policy allowlist of `self` handed the microphone, the
// camera and geolocation to all 47 apps, including any app the ANIMA agent writes and installs.
// allowAttr() turns the declaration into the iframe's `allow` attribute, so an app gets a device
// capability only when its manifest asked for it.
//
// wm.js touches the DOM at import time (`document.getElementById('windows')`), so stub just enough
// of it — allowAttr itself is pure.

import { test } from 'node:test';
import assert from 'node:assert/strict';

globalThis.document = { getElementById: () => null, addEventListener() {}, createElement: () => ({ style: {}, classList: { add() {}, remove() {} } }) };
globalThis.window = { addEventListener() {} };
const { allowAttr, frameHtml } = await import('../web/shell/wm.js');

const has = (s, feature) => new RegExp('(^|;\\s*)' + feature + '(;|$)').test(s);
const denied = (s, feature) => s.includes(feature + " 'none'");

test('an app that declares no device capability is denied all three', () => {
  const a = allowAttr({ id: 'calculator', permissions: ['storage.app'] });
  assert.ok(denied(a, 'microphone'), 'microphone must be explicitly denied, not merely unused');
  assert.ok(denied(a, 'camera'));
  assert.ok(denied(a, 'geolocation'));
});

test('device.mic grants the microphone — and nothing else', () => {
  const a = allowAttr({ id: 'dictation', permissions: ['storage.shared', 'device.audio', 'device.mic'] });
  assert.ok(has(a, 'microphone'), 'the app that actually records must keep working');
  assert.ok(denied(a, 'camera'));
  assert.ok(denied(a, 'geolocation'));
});

test('device.location grants geolocation only', () => {
  const a = allowAttr({ id: 'weather', permissions: ['device.location'] });
  assert.ok(has(a, 'geolocation'));
  assert.ok(denied(a, 'microphone'));
});

test('camera is denied to everyone — no app in the OS uses it', () => {
  for (const perms of [[], ['device.mic'], ['device.location'], ['device.ir', 'net.wifi']]) {
    assert.ok(denied(allowAttr({ permissions: perms }), 'camera'), JSON.stringify(perms));
  }
});

test('the benign features stay granted for every app', () => {
  for (const app of [{ permissions: [] }, { permissions: ['device.mic'] }, {}]) {
    const a = allowAttr(app);
    for (const f of ['fullscreen', 'gamepad', 'autoplay']) assert.ok(has(a, f), f + ' in ' + a);
  }
});

test('unknown permissions are ignored, never granted', () => {
  const a = allowAttr({ permissions: ['made.up', 'device.microphone', 'MICROPHONE'] });
  assert.ok(denied(a, 'microphone'), 'only the exact device.mic capability grants the mic');
});

test('permissions UNKNOWN → keep the old permissive behaviour, do not mute the OS', () => {
  // The registry read can fail on a busy device. Failing closed there would break dictation, the
  // recorder and ANIMA's voice on a transient error; failing open is exactly the status quo.
  for (const app of [{ id: 'x' }, { id: 'x', permissions: null }, { id: 'x', permissions: 'nope' }, null]) {
    const a = allowAttr(app);
    assert.ok(!a.includes("'none'"), 'must not deny anything when the declaration is unknown: ' + a);
    assert.ok(has(a, 'fullscreen'));
  }
});

test('the attribute is a well-formed Feature Policy list', () => {
  const a = allowAttr({ permissions: ['device.mic'] });
  assert.ok(!a.includes(';;') && !a.trim().endsWith(';'), a);
  for (const part of a.split(';')) assert.ok(part.trim().length, 'no empty entries: ' + a);
});

// ── the wiring, not just the policy ───────────────────────────────────────────────────────────
// allowAttr() being correct is worthless if the window never uses it. These assert the rendered
// iframe actually carries the policy — the link a browser test kept failing to pin down reliably.

test('the rendered iframe carries the app-specific policy', () => {
  const mic = frameHtml({ id: 'dictation', name: 'Dictation', route: '/apps/dictation/', permissions: ['device.mic'] }, '/apps/dictation/');
  assert.match(mic, /<iframe /);
  assert.match(mic, /allow="[^"]*(^|;|\s)microphone(;|"|\s)/, 'the app that records keeps the mic: ' + mic);
  assert.ok(mic.includes("camera 'none'"), mic);

  const plain = frameHtml({ id: 'calculator', name: 'Calculator', route: '/apps/calculator/', permissions: ['storage.app'] }, '/apps/calculator/');
  assert.ok(plain.includes("microphone 'none'"), 'an app that never asked must not get the mic: ' + plain);
  assert.ok(plain.includes("geolocation 'none'"), plain);
});

test('an app with no web route renders a placeholder, not a frame', () => {
  const html = frameHtml({ id: 'x', name: 'Ghost' }, '');
  assert.ok(!html.includes('<iframe'), html);
  assert.match(html, /No web route declared/);
});

// ── the bypass ────────────────────────────────────────────────────────────────────────────────
// frameHtml built the tag by concatenation and interpolated app.name into title="…" BEFORE `allow`.
// A name containing a double quote closes the title and injects its own attributes — and by the HTML
// duplicate-attribute rule the FIRST `allow` wins, so the app chose its own Feature Policy. The agent
// publishes apps with names it picks, which made this a self-service bypass of this very gate.

test('an app NAME cannot inject attributes into the iframe tag', () => {
  const evil = { id: 'evil', route: '/apps/evil/', permissions: [],
    name: 'X" allow="microphone; camera" data-x="' };
  const html = frameHtml(evil, '/apps/evil/');
  const allows = html.match(/allow="/g) || [];
  assert.equal(allows.length, 1, 'exactly one allow attribute must survive: ' + html);
  assert.ok(html.includes("microphone 'none'"), 'the computed policy must be the one that applies: ' + html);
  assert.ok(!/title="[^"]*"\s+allow="microphone; camera"/.test(html), html);
});

test('a hostile name cannot break out of the title attribute at all', () => {
  const html = frameHtml({ id: 'x', route: '/apps/x/', permissions: [], name: '"><script>alert(1)</script>' }, '/apps/x/');
  assert.ok(!html.includes('<script'), html);
  assert.ok(html.includes('&quot;'), 'the quote must be encoded, not passed through: ' + html);
});

test('the src is escaped too — a query value must not open a second attribute', () => {
  const html = frameHtml({ id: 'x', name: 'X', route: '/apps/x/', permissions: ['device.mic'] }, '/apps/x/?q=a"b');
  assert.equal((html.match(/allow="/g) || []).length, 1, html);
  assert.ok(html.includes('&quot;'), html);
});

test('a placeholder (no route) escapes the name as well', () => {
  const html = frameHtml({ id: 'x', name: '<img src=x onerror=1>' }, '');
  assert.ok(!html.includes('<img'), html);
});
