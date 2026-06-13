// NucleoOS — Log Viewer diagnosis engine (pure, DOM-free, unit-tested by tools/log-viewer-diag.test.mjs).
//
// The device stays dumb and on-demand; THIS is where the intelligence lives. It takes the cheap
// read-only telemetry the firmware already exposes (/api/diag, or the legacy /api/status+/api/heap+
// /api/cpu trio on un-flashed devices) plus the console ring (/api/logs) and the live event tail, and
// condenses them into a tiny, self-describing "opcode" digest a human pastes to an AI for a health
// readout of the Cardputer + ANIMA. Two design rules:
//   1. Never assume a field exists — older firmware has no /api/diag. normalize() folds either source
//      into one shape and marks what's missing as null, so the digest degrades honestly ("n/a").
//   2. Pre-compute the verdict. healthFlags() turns raw numbers into named risks (LOW-RAM, FRAG, OOM,
//      WEAK-WIFI, ABSTAIN-HI, CRASH…) so the diagnosis is instant and the token cost is minimal.

const KB = 1024;
const kb = (b) => (b == null ? null : Math.round(b / KB));
const has = (v) => v != null;

// ── formatting ─────────────────────────────────────────────────────────────────────────────────
export function fmtKB(b) { return has(b) ? kb(b) + 'K' : 'n/a'; }
export function fmtUptime(s) {
  if (!has(s) || s < 0) return 'n/a';
  s = Math.floor(s);
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
        m = Math.floor((s % 3600) / 60), sec = s % 60;
  if (d) return `${d}d${h}h`;
  if (h) return `${h}h${m}m`;
  if (m) return `${m}m${sec}s`;
  return `${sec}s`;
}
export function fmtAge(deltaSec) {
  if (!has(deltaSec)) return '?';
  const s = Math.max(0, Math.floor(deltaSec));
  if (s < 60) return `-${s}s`;
  if (s < 3600) return `-${Math.floor(s / 60)}m`;
  if (s < 86400) return `-${Math.floor(s / 3600)}h`;
  return `-${Math.floor(s / 86400)}d`;
}
const fmtGB = (b) => has(b) ? (b / 1e9).toFixed(1) + 'G' : '?';

// Wi-Fi link quality band from dBm (0/none = not associated).
export function rssiBand(rssi) {
  if (!has(rssi) || rssi === 0) return 'n/a';
  if (rssi >= -60) return 'good';
  if (rssi >= -70) return 'ok';
  if (rssi >= -80) return 'weak';
  return 'bad';
}
// Coarse heap headroom band on the worst-ever watermark — the number that actually predicts OOM on a
// no-PSRAM board. Thresholds mirror the device's known ~17 KB safe floor (see stability notes).
export function heapBand(minFreeBytes) {
  if (!has(minFreeBytes)) return 'n/a';
  if (minFreeBytes < 8 * KB) return 'crit';
  if (minFreeBytes < 20 * KB) return 'low';
  if (minFreeBytes < 40 * KB) return 'ok';
  return 'good';
}
const L1_MODE = { 0: 'auto', 1: 'on', 2: 'off' };

// ── normalize: fold /api/diag OR the legacy trio into one internal shape ──────────────────────────
export function normalize({ diag, status, heap, cpu } = {}) {
  if (diag && diag.sys) {                       // new firmware: /api/diag is already the unified shape
    const a = diag.anima || {};
    return {
      partial: false,
      sys: { ...diag.sys },
      mem: { ...diag.mem },
      net: { ...diag.net },
      anima: { ...a, known: true },
      arb: { ...(diag.arb || {}) },
      cpu: { ...(diag.cpu || {}) },
      oom: { known: true, ...(diag.oom || {}) },
    };
  }
  // Legacy fallback (device not yet flashed with /api/diag): assemble from status+heap+cpu. The
  // ANIMA telemetry, reset cause and OOM watermark don't exist here -> left null and shown as n/a.
  status = status || {}; heap = heap || {}; cpu = cpu || {};
  const st = status.storage || {}, nw = status.network || {}, ab = status.arbiter || {}, ot = status.ota || {};
  const hi = heap.internal || {}, hd = heap.dma || {};
  return {
    partial: true,
    sys: {
      fw: status.version || null, built: null,
      uptime_s: has(status.uptime_s) ? status.uptime_s : cpu.uptime_s,
      reset: null, sd: st.mounted, sd_free: st.free_bytes, sd_total: st.total_bytes,
      ota: ot.state || null, clients: null,
    },
    mem: {
      free: status.free_heap, min: status.min_free_heap, lblk: status.largest_free_block,
      frag: has(hi.frag_pct) ? hi.frag_pct : null,
      dma_free: hd.free_bytes, stack_httpd: heap.httpd_stack_free_min,
    },
    net: { mode: nw.mode, ssid: nw.ssid, ip: nw.ip, rssi: null, tsync: nw.time_synced, clients: null },
    anima: { known: false },
    arb: { busy: ab.busy, job: ab.job, grants: ab.grants, denials: ab.denials, yields: ab.yields, hfmin: ab.heap_free_min },
    cpu: { cores: cpu.cores, freq: cpu.freq_mhz, tasks: cpu.tasks, load: cpu.load },
    oom: { known: false, count: null },
  };
}

// ── health flags: raw numbers -> named, ranked risks (the pre-computed verdict) ───────────────────
export function healthFlags(u) {
  const f = [];
  const m = u.mem || {}, n = u.net || {}, a = u.anima || {}, o = u.oom || {}, s = u.sys || {}, arb = u.arb || {};

  // Crash posture — the loudest signal. A clean SW/POWERON is fine; the rest are real.
  const badReset = { PANIC: 'crash', INT_WDT: 'int-watchdog', TASK_WDT: 'task-hung', WDT: 'watchdog', BROWNOUT: 'brownout(battery)' };
  if (s.reset && badReset[s.reset]) f.push(`CRASH:${badReset[s.reset]}`);

  // RAM — the #1 constraint (no PSRAM). Watermark first, live free second, then fragmentation.
  if (has(m.min)) {
    if (m.min < 8 * KB) f.push('CRIT-RAM');
    else if (m.min < 20 * KB) f.push('LOW-RAM');
  }
  if (has(m.frag) && m.frag >= 40) f.push('FRAG');
  if (o.known && o.count > 0) f.push(`OOM:${o.count}`);
  if (has(arb.denials) && has(arb.grants) && arb.denials > 5 && arb.denials > (arb.grants + 1) * 0.2) f.push('ARB-PRESSURE');

  // Network / time.
  if (n.mode === 'sta') {
    if (!n.ip || n.ip === '0.0.0.0' || n.ip === '') f.push('NO-IP');
    else if (has(n.rssi) && n.rssi !== 0 && n.rssi <= -78) f.push('WEAK-WIFI');
  }
  if (n.tsync === false) f.push('TIME-UNSYNCED');

  // ANIMA health (only when telemetry is real).
  if (a.known) {
    if (has(a.q) && a.q >= 10 && a.none / a.q >= 0.4) f.push('ABSTAIN-HI');
    if (a.online_en && a.teacher === false) f.push('NO-TEACHER');
    if (a.online_en && a.online_avail === false) f.push('ONLINE-UNREACHABLE');
  }
  return f;
}

// ── console-ring error extraction: keep only the lines that matter, deduped + clipped ─────────────
export function extractErrors(logsText, cap = 18) {
  if (!logsText) return [];
  const out = []; let last = '';
  const sig = /(^[EW] \(\d+\))|panic|abort|Backtrace|Guru Meditation|StoreProhibited|LoadProhibited|stack overflow|rst:0x|Brownout|assert failed|heap corrupt|alloc fail/i;
  for (let line of logsText.split('\n')) {
    line = line.replace(/\s+$/, '');
    if (!line || !sig.test(line)) continue;
    if (line.length > 180) line = line.slice(0, 177) + '…';
    if (line === last) continue;            // collapse identical consecutive lines
    last = line;
    out.push(line);
  }
  return out.slice(-cap);                    // keep the most recent
}

// ── significant events from the live tail (drop pure UI plumbing) ─────────────────────────────────
const NOISE = /^(system\.(focus|display|remote|clients)|fs\.changed|apps\.changed)$/;
export function significantEvents(events, nowSec, cap = 14) {
  if (!Array.isArray(events)) return [];
  const out = [];
  for (let i = events.length - 1; i >= 0 && out.length < cap; i--) {
    const e = events[i];
    if (!e || !e.t) continue;
    const t = String(e.t);
    const interesting = /boot|oom|error|fail|panic|warn|crash|brownout|wdt|launch|learned|busy|notify/i.test(t) || !NOISE.test(t);
    if (!interesting) continue;
    out.push({ t, age: has(e.ts) && has(nowSec) ? fmtAge(nowSec - e.ts) : '?', d: compactData(e.d) });
  }
  return out;
}
function compactData(d) {
  if (d == null) return '';
  if (typeof d === 'string') return d.slice(0, 80);
  try { let s = JSON.stringify(d); return s.length > 80 ? s.slice(0, 79) + '…' : s; } catch { return ''; }
}

// ── the digest: a compact, legend-led, opcode-style block. Small on purpose. ──────────────────────
export function buildDigest({ diag, status, heap, cpu, logsText, events, nowSec } = {}) {
  const u = normalize({ diag, status, heap, cpu });
  const m = u.mem, n = u.net, a = u.anima, s = u.sys, arb = u.arb, c = u.cpu, o = u.oom;
  const flags = healthFlags(u);
  const L = [];
  const dt = has(nowSec) ? new Date(nowSec * 1000).toISOString().replace('T', ' ').slice(0, 19) : '?';

  L.push('═══ NUCLEO-DIAG v1 ═══  (paste to AI → health diagnosis: Cardputer + ANIMA)');
  L.push(`# ${dt}  src=${u.partial ? 'legacy(/api/status+heap+cpu — flash for full ANIMA/reset/OOM)' : '/api/diag'}`);
  L.push('# legend: KB=free/min/largest internal SRAM · frag=% non-contiguous · rssi dBm · ANIMA tier cmd/fact/stitch/remote, none=abstain · arb=heavy-work token');
  L.push(`FLAGS ${flags.length ? flags.join(' ') : 'ok (no alerts)'}`);

  L.push(`SYS   fw=${s.fw || '?'}${s.built ? ' built="' + s.built + '"' : ''} up=${fmtUptime(s.uptime_s)} reset=${s.reset || 'n/a'} ` +
         `sd=${s.sd ? 'ok(' + fmtGB(s.sd_free) + '/' + fmtGB(s.sd_total) + ')' : 'no'} ota=${s.ota || '?'}` +
         (has(s.clients) ? ` clients=${s.clients}` : ''));

  L.push(`MEM   free=${fmtKB(m.free)} min=${fmtKB(m.min)}(${heapBand(m.min)}) lblk=${fmtKB(m.lblk)} frag=${has(m.frag) ? m.frag + '%' : 'n/a'} ` +
         `dma=${fmtKB(m.dma_free)} stack.httpd=${fmtKB(m.stack_httpd)}`);

  L.push(`NET   ${n.mode || '?'}${n.ssid ? ' "' + n.ssid + '"' : ''} ${n.ip || ''} ` +
         `rssi=${has(n.rssi) && n.rssi !== 0 ? n.rssi + '(' + rssiBand(n.rssi) + ')' : 'n/a'} tsync=${n.tsync === true ? 'yes' : n.tsync === false ? 'NO' : '?'}` +
         (has(n.clients) ? ` clients=${n.clients}` : ''));

  if (a.known) {
    const mode = a.online_only ? 'online-only' : a.online_en ? 'hybrid' : 'offline';
    const teach = a.teacher ? `${a.provider || '?'}/${a.model || '?'}` : 'none';
    L.push(`ANIMA mode=${mode} online=${a.online_en ? (a.online_avail ? 'on(reachable)' : 'on(UNREACHABLE)') : 'off'} ` +
           `teacher=${teach} l1=${L1_MODE[a.l1_mode] ?? '?'}${a.l1_serving ? '+serving' : '+standby'}(${fmtKB(a.l1_heap)})`);
    const ab = has(a.q) && a.q > 0 ? Math.round((a.none / a.q) * 100) + '%' : 'n/a';
    L.push(`      q=${a.q ?? 0} cmd=${a.cmd ?? 0} fact=${a.fact ?? 0} stitch=${a.stitch ?? 0} remote=${a.remote ?? 0} none=${a.none ?? 0} ` +
           `abstain=${ab} lastconf=${a.last_conf ?? '?'} last=${a.last || '-'}`);
  } else {
    L.push('ANIMA n/a (firmware without /api/diag — flash to get tier mix / abstain rate / teacher)');
  }

  L.push(`ARB   ${arb.busy ? 'BUSY:' + (arb.job || '?') : 'idle'} grants=${arb.grants ?? '?'} deny=${arb.denials ?? '?'} ` +
         `yield=${arb.yields ?? '?'} hfmin=${fmtKB(arb.hfmin)}`);

  L.push(`CPU   load=${Array.isArray(c.load) ? c.load.join('/') + '%' : 'n/a'} tasks=${c.tasks ?? '?'}` +
         (has(c.freq) ? ` @${c.freq}MHz` : ''));

  if (o.known) L.push(`OOM   count=${o.count}${o.count ? ' last=' + o.last_size + 'B caps=0x' + (o.last_caps || 0).toString(16) : ''}`);

  const evs = significantEvents(events, nowSec);
  if (evs.length) {
    L.push('EVT   (live tail, newest first)');
    for (const e of evs) L.push(`  ${e.age.padStart(5)} ${e.t}${e.d ? ' ' + e.d : ''}`);
  }

  const errs = extractErrors(logsText);
  if (errs.length) {
    L.push('ERR   (console ring — warnings & errors)');
    for (const e of errs) L.push('  ' + e);
  }

  L.push('ASK   Diagnose Cardputer + ANIMA health from the above. Rank risks (RAM/frag/OOM/RSSI/abstain/crash),');
  L.push('      explain the likely root cause of each FLAG, and give targeted fixes. Be concise.');
  L.push('═══ end ═══');
  return L.join('\n');
}

export default { buildDigest, normalize, healthFlags, extractErrors, significantEvents, rssiBand, heapBand, fmtKB, fmtUptime, fmtAge };
