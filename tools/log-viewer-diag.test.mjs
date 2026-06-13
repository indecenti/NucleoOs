// Unit tests for the Log Viewer diagnosis engine (apps/log-viewer/www/diag.js).
// Run: node --test tools/log-viewer-diag.test.mjs   (or: npm run logviewer:test)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import D, {
  buildDigest, normalize, healthFlags, extractErrors, significantEvents,
  rssiBand, heapBand, fmtUptime, fmtKB,
} from '../apps/log-viewer/www/diag.js';

const KB = 1024;
const NOW = 1_750_000_000;

// A healthy /api/diag payload (new firmware shape).
function healthyDiag() {
  return {
    v: 1, ts: NOW,
    sys: { fw: '0.2.0', proj: 'nucleoos', built: 'Jun 9 2026', idf: 'v5.4', uptime_s: 11520,
           reset: 'SW', slot: 'ota_0', ota: 'valid', sd: true, sd_free: 7.1e9, sd_total: 14.8e9 },
    mem: { free: 76 * KB, min: 30 * KB, lblk: 42 * KB, frag: 12, dma_free: 33 * KB, stack_httpd: 8 * KB },
    net: { mode: 'sta', ssid: 'Casa', ip: '192.168.0.166', rssi: -58, tsync: true, clients: 1 },
    anima: { online_en: true, online_only: false, online_avail: true, teacher: true,
             provider: 'anthropic', model: 'claude-opus', l1_mode: 0, l1_serving: true, l1_heap: 31 * KB,
             q: 412, none: 32, cmd: 300, fact: 80, stitch: 0, remote: 40, last_conf: 72, last: 'greet' },
    arb: { busy: false, job: '', grants: 120, denials: 4, yields: 2, hfmin: 30 * KB },
    cpu: { cores: 2, freq: 240, tasks: 24, load: [12, 8] },
    oom: { count: 0, last_size: 0, last_caps: 0 },
  };
}

test('buildDigest: full digest has every section, legend, flags and the AI instruction', () => {
  const out = buildDigest({ diag: healthyDiag(), logsText: 'I (1) boot ok\n', events: [], nowSec: NOW });
  for (const tok of ['NUCLEO-DIAG v1', 'legend:', 'FLAGS', 'SYS', 'MEM', 'NET', 'ANIMA', 'ARB', 'CPU', 'OOM', 'ASK', '═══ end ═══'])
    assert.ok(out.includes(tok), `missing section: ${tok}`);
  assert.ok(out.includes('src=/api/diag'));
  assert.ok(out.includes('FLAGS ok (no alerts)'), 'a healthy device should raise no flags');
  assert.ok(out.includes('fw=0.2.0') && out.includes('up=3h12m') && out.includes('reset=SW'));
  assert.ok(out.includes('teacher=anthropic/claude-opus'));
  assert.ok(out.includes('abstain=8%'), 'abstain rate = none/q = 32/412 ≈ 8%');
  // The digest is meant to be small — a healthy snapshot should stay compact.
  assert.ok(out.length < 1400, `digest too large: ${out.length} bytes`);
});

test('healthFlags: each risk maps to its named flag', () => {
  const f = (over) => healthFlags(normalize({ diag: { ...healthyDiag(), ...over, sys: { ...healthyDiag().sys, ...(over.sys || {}) } } }));
  assert.deepEqual(healthFlags(normalize({ diag: healthyDiag() })), [], 'healthy = no flags');
  assert.ok(f({ mem: { min: 12 * KB, frag: 10 } }).includes('LOW-RAM'));
  assert.ok(f({ mem: { min: 4 * KB } }).includes('CRIT-RAM'));
  assert.ok(f({ mem: { min: 30 * KB, frag: 55 } }).includes('FRAG'));
  assert.ok(f({ oom: { count: 3, last_size: 32768, last_caps: 0 } }).some((x) => x.startsWith('OOM:')));
  assert.ok(f({ net: { mode: 'sta', ip: '1.2.3.4', rssi: -85, tsync: true } }).includes('WEAK-WIFI'));
  assert.ok(f({ net: { mode: 'sta', ip: '', rssi: 0, tsync: true } }).includes('NO-IP'));
  assert.ok(f({ net: { mode: 'sta', ip: '1.2.3.4', rssi: -50, tsync: false } }).includes('TIME-UNSYNCED'));
  assert.ok(f({ sys: { reset: 'PANIC' } }).some((x) => x.startsWith('CRASH:')));
  assert.ok(f({ sys: { reset: 'BROWNOUT' } }).some((x) => x.includes('brownout')));
  assert.ok(f({ anima: { ...healthyDiag().anima, q: 100, none: 60 } }).includes('ABSTAIN-HI'));
  assert.ok(f({ anima: { ...healthyDiag().anima, online_en: true, teacher: false } }).includes('NO-TEACHER'));
  assert.ok(f({ anima: { ...healthyDiag().anima, online_en: true, online_avail: false } }).includes('ONLINE-UNREACHABLE'));
});

test('normalize: legacy trio (un-flashed device) folds in with partial=true and anima unknown', () => {
  const u = normalize({
    status: { version: '0.1.0', uptime_s: 99, free_heap: 80 * KB, min_free_heap: 18 * KB,
              largest_free_block: 40 * KB, storage: { mounted: true, free_bytes: 7e9, total_bytes: 14e9 },
              network: { mode: 'sta', ssid: 'X', ip: '10.0.0.2', time_synced: true },
              arbiter: { busy: false, grants: 5, denials: 0, yields: 0, heap_free_min: 18 * KB }, ota: { state: 'valid' } },
    heap: { internal: { frag_pct: 22, free_bytes: 80 * KB }, dma: { free_bytes: 20 * KB }, httpd_stack_free_min: 9 * KB },
    cpu: { cores: 2, freq_mhz: 240, tasks: 20, load: [5, 7] },
  });
  assert.equal(u.partial, true);
  assert.equal(u.anima.known, false);
  assert.equal(u.oom.known, false);
  assert.equal(u.mem.min, 18 * KB);
  assert.equal(u.mem.frag, 22);
  assert.equal(u.sys.reset, null, 'legacy has no reset cause');
  // The digest must say so honestly and still render the Cardputer half.
  const out = buildDigest({ status: { version: '0.1.0', free_heap: 80 * KB, min_free_heap: 18 * KB, network: {}, storage: {}, arbiter: {} },
                            heap: { internal: { frag_pct: 22 } }, cpu: { load: [5, 7] }, nowSec: NOW });
  assert.ok(out.includes('src=legacy'));
  assert.ok(out.includes('ANIMA n/a'), 'ANIMA telemetry absent on legacy firmware');
});

test('extractErrors: keeps E/W + panic lines, collapses dups, clips length, caps count', () => {
  const log = [
    'I (10) boot: ok', 'I (11) wifi: connected',
    'W (20) oom: alloc fail: 32768 B caps=0x4 in tls',
    'E (21) anima: L1 stack overflow', 'E (21) anima: L1 stack overflow',  // dup -> collapse
    'Guru Meditation Error: Core 0 panic', 'Backtrace: 0x4008 0x4009',
    'I (30) noise that should not appear',
  ].join('\n');
  const errs = extractErrors(log);
  assert.ok(errs.some((l) => l.includes('oom')));
  assert.ok(errs.some((l) => l.includes('L1 stack overflow')));
  assert.ok(errs.some((l) => l.includes('Guru Meditation')));
  assert.equal(errs.filter((l) => l.includes('L1 stack overflow')).length, 1, 'consecutive dup collapsed');
  assert.ok(!errs.some((l) => l.includes('noise')), 'plain INFO dropped');
  // long line clipped
  const big = extractErrors('E (1) tag: ' + 'x'.repeat(300));
  assert.ok(big[0].length <= 181 && big[0].endsWith('…'));
});

test('significantEvents: drops UI plumbing, keeps boot/errors, newest-first with age', () => {
  const events = [
    { t: 'system.focus', seq: 1, ts: NOW - 300, d: { active: true } },   // noise
    { t: 'fs.changed', seq: 2, ts: NOW - 250, d: { op: 'write' } },        // noise
    { t: 'system.boot', seq: 3, ts: NOW - 200, d: { rst: 'SW', free: 88000 } },
    { t: 'voice.launch', seq: 4, ts: NOW - 60, d: { app: 'radio' } },
  ];
  const ev = significantEvents(events, NOW);
  assert.equal(ev[0].t, 'voice.launch', 'newest first');
  assert.ok(ev.some((e) => e.t === 'system.boot'));
  assert.ok(!ev.some((e) => e.t === 'system.focus'), 'system.focus is plumbing');
  assert.ok(!ev.some((e) => e.t === 'fs.changed'), 'fs.changed is plumbing');
  assert.equal(ev[0].age, '-1m');
});

test('formatters and bands', () => {
  assert.equal(fmtUptime(45), '45s');
  assert.equal(fmtUptime(11520), '3h12m');
  assert.equal(fmtUptime(200000), '2d7h');
  assert.equal(fmtKB(17 * KB), '17K');
  assert.equal(fmtKB(null), 'n/a');
  assert.equal(rssiBand(-50), 'good');
  assert.equal(rssiBand(-65), 'ok');
  assert.equal(rssiBand(-77), 'weak');
  assert.equal(rssiBand(-90), 'bad');
  assert.equal(rssiBand(0), 'n/a');
  assert.equal(heapBand(4 * KB), 'crit');
  assert.equal(heapBand(15 * KB), 'low');
  assert.equal(heapBand(30 * KB), 'ok');
  assert.equal(heapBand(60 * KB), 'good');
});

test('default export exposes the same surface', () => {
  for (const k of ['buildDigest', 'normalize', 'healthFlags', 'extractErrors', 'significantEvents', 'rssiBand', 'heapBand'])
    assert.equal(typeof D[k], 'function');
});
