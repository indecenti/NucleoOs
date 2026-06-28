// Radio endpoint verifier for NucleoOS.
//
// The Cardputer plays a live MP3 stream over PLAIN HTTP (no TLS — see app_radio.cpp). The most
// common failure is "track progress advances but no audio": the progress bar is driven by the
// /api/nowplaying metadata, NOT by decoded audio, so it keeps moving even when the audio stream
// is unreachable (e.g. an internal-only port). This tool checks what the DEVICE actually needs:
//
//   1) stream     : HTTP 200, Content-Type audio/mpeg, a valid MPEG-1/2 Layer III sync word,
//                   and a sustained throughput >= ~100 kbps (a 128 kbps station, read live).
//   2) nowplaying : HTTP 200 and parseable JSON with a track.
//
// It reads the in-repo seed catalog (tools/sd-sim/system/config/radio.json) — the single source kept
// in lock-step with the web app's SEED and the firmware BUILTIN[] — so it tests the exact URLs the
// device will use. The file is
// schema 2 — a { "default", "stations": [ {name,stream,nowplaying}, ... ] } list — and EVERY station
// is checked, so a bad URL added from the web app is caught here before it reaches the Cardputer.
// A legacy schema-1 { "stream", "name" } file still works, as does --stream / --nowplaying (which
// override the file and check a single endpoint).
//
// Usage:
//   node tools/radio-check.mjs                 # check every station in radio.json
//   node tools/radio-check.mjs --stream http://host/stream --nowplaying http://host/api/nowplaying
//
// Exit code 0 = all checks pass, 1 = a check failed (CI-friendly).

import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(fileURLToPath(import.meta.url), '..', '..');
const CFG = join(REPO, 'tools', 'sd-sim', 'system', 'config', 'radio.json');

const STREAM_MIN_KBPS = 100;   // 128 kbps nominal; allow headroom for read-timing overhead
const STREAM_SECONDS = 6;      // how long to sample the live stream
const STREAM_TARGET_BYTES = (128_000 / 8) * STREAM_SECONDS;   // ~96 KB for 6 s @ 128 kbps

function parseArgs(argv) {
  const a = { stream: null, nowplaying: null };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--stream') a.stream = argv[++i];
    else if (argv[i] === '--nowplaying' || argv[i] === '--np') a.nowplaying = argv[++i];
  }
  return a;
}

const ok = (m) => console.log(`  \x1b[32m✓\x1b[0m ${m}`);
const bad = (m) => console.log(`  \x1b[31m✗\x1b[0m ${m}`);
const info = (m) => console.log(`    ${m}`);

// MPEG audio frame sync: 11 bits set (0xFFE..). Layer III only (the decoder is MP3-only).
// b1: AAAAAAAA, b2: AAABBCCD -> A=sync, B=version, C=layer(01=Layer III).
function mp3FrameInfo(buf) {
  for (let i = 0; i + 4 <= buf.length && i < 1024; i++) {
    if (buf[i] !== 0xff || (buf[i + 1] & 0xe0) !== 0xe0) continue;
    const ver = (buf[i + 1] >> 3) & 0x03;     // 11=MPEG1, 10=MPEG2, 00=MPEG2.5
    const layer = (buf[i + 1] >> 1) & 0x03;   // 01 = Layer III
    if (layer !== 0x01) continue;
    const brIdx = (buf[i + 2] >> 4) & 0x0f;
    const srIdx = (buf[i + 2] >> 2) & 0x03;
    const chMode = (buf[i + 2 + 1] >> 6) & 0x03;
    if (brIdx === 0 || brIdx === 0x0f || srIdx === 0x03) continue;   // invalid -> keep scanning
    const V1 = { rates: [0,32,40,48,56,64,80,96,112,128,160,192,224,256,320], sr: [44100,48000,32000] };
    const V2 = { rates: [0,8,16,24,32,40,48,56,64,80,96,112,128,144,160],     sr: [22050,24000,16000] };
    const t = ver === 0x03 ? V1 : V2;
    return {
      offset: i,
      version: ver === 0x03 ? 'MPEG-1' : ver === 0x02 ? 'MPEG-2' : 'MPEG-2.5',
      kbps: t.rates[brIdx],
      hz: (ver === 0x03 ? V1.sr : V2.sr)[srIdx] / (ver === 0x00 ? 2 : 1),
      channels: chMode === 0x03 ? 1 : 2,
      mode: ['stereo', 'joint-stereo', 'dual', 'mono'][chMode],
    };
  }
  return null;
}

async function checkStream(url) {
  console.log(`\n[stream] ${url}`);
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), (STREAM_SECONDS + 6) * 1000);
  let pass = true;
  try {
    const t0 = Date.now();
    // redirect:'manual' — the device's esp_http_client (open+read) does NOT follow 30x.
    // A 301 here is the exact "no audio, progress still moves" failure, so we must catch it
    // (plain fetch auto-follows redirects and would hide it with a false 200).
    const r = await fetch(url, { headers: { 'user-agent': 'NucleoOS-Radio/1.0' }, redirect: 'manual', signal: ctrl.signal });
    if (r.status >= 300 && r.status < 400) {
      bad(`HTTP ${r.status} redirect -> ${r.headers.get('location') || '?'} — the device won't follow this; point at a direct-200 HTTP URL`);
      return false;
    }
    if (r.status !== 200) { bad(`HTTP ${r.status} (expected 200)`); return false; }
    ok(`HTTP 200 (no redirect)`);
    const ct = (r.headers.get('content-type') || '').toLowerCase();
    if (ct.includes('audio/mpeg') || ct.includes('audio/mp3')) ok(`Content-Type: ${ct}`);
    else { bad(`Content-Type: ${ct || '(none)'} (expected audio/mpeg)`); pass = false; }

    // Read ~STREAM_SECONDS of the live stream, measuring sustained throughput.
    const reader = r.body.getReader();
    let total = 0, first = null;
    while (total < STREAM_TARGET_BYTES && (Date.now() - t0) < STREAM_SECONDS * 1000 + 4000) {
      const { value, done } = await reader.read();
      if (done) break;
      if (!first) first = Buffer.from(value.slice(0, Math.min(value.length, 64)));
      total += value.length;
    }
    reader.cancel().catch(() => {});
    const secs = (Date.now() - t0) / 1000;
    const kbps = (total * 8) / 1000 / secs;

    const fi = mp3FrameInfo(first || Buffer.alloc(0));
    if (fi) ok(`valid MP3 frame @+${fi.offset}: ${fi.version} Layer III, ${fi.kbps} kbps, ${fi.hz} Hz, ${fi.mode}`);
    else { bad(`no MPEG Layer III sync word in first bytes`); pass = false; }

    if (kbps >= STREAM_MIN_KBPS) ok(`sustained ${kbps.toFixed(0)} kbps over ${secs.toFixed(1)}s (>= ${STREAM_MIN_KBPS})`);
    else { bad(`only ${kbps.toFixed(0)} kbps over ${secs.toFixed(1)}s (< ${STREAM_MIN_KBPS}) — too slow / stalls`); pass = false; }
    if (fi && fi.hz === 44100 && fi.channels === 2)
      info(`note: 44.1 kHz stereo is the heaviest case for the on-device decoder; a mono feed would be lighter.`);
  } catch (e) {
    bad(`unreachable: ${e.name === 'AbortError' ? 'timeout' : e.message}`);
    info(`a timeout here is exactly the "no audio, progress still moves" failure — pick a reachable HTTP host/port.`);
    pass = false;
  } finally { clearTimeout(t); }
  return pass;
}

async function checkNowPlaying(url) {
  console.log(`\n[nowplaying] ${url}`);
  try {
    const r = await fetch(url, { redirect: 'manual', signal: AbortSignal.timeout(8000) });
    if (r.status >= 300 && r.status < 400) {
      bad(`HTTP ${r.status} redirect -> ${r.headers.get('location') || '?'} — device won't follow; use a direct-200 HTTP URL`);
      return false;
    }
    if (r.status !== 200) { bad(`HTTP ${r.status} (expected 200)`); return false; }
    ok(`HTTP 200 (no redirect)`);
    const j = await r.json();
    const soma = Array.isArray(j.songs) && j.songs[0] && [j.songs[0].artist, j.songs[0].title].filter(Boolean).join(' — ');
    const track = j.track || (j.track_obj && `${j.track_obj.artist} — ${j.track_obj.title}`) || soma;
    if (track) { ok(`JSON parsed, now playing: "${track}"`); return true; }
    bad(`JSON parsed but no track field`); return false;
  } catch (e) {
    bad(`failed: ${e.name === 'TimeoutError' ? 'timeout' : e.message}`);
    return false;
  }
}

// Build the list of stations to check: a --stream override wins; otherwise the schema-2 stations[]
// array; otherwise a legacy single { stream, name, nowplaying }.
function stationsFrom(args, cfg) {
  if (args.stream) return [{ name: 'cli', stream: args.stream, nowplaying: args.nowplaying }];
  if (Array.isArray(cfg.stations)) return cfg.stations.filter(s => s && s.stream);
  if (cfg.stream) return [{ name: cfg.name || '?', stream: cfg.stream, nowplaying: cfg.nowplaying }];
  return [];
}

async function checkOne(st) {
  console.log(`\n\x1b[1m── ${st.name || '?'} ──\x1b[0m`);
  if (/^https:/i.test(st.stream))
    console.log(`\x1b[33m⚠ stream is HTTPS — on the PSRAM-less device, TLS beside the decoder causes stutter/silence; it won't play on the Cardputer. Prefer http://.\x1b[0m`);
  const streamOk = await checkStream(st.stream);
  // now-playing is web-only polish (the device shows no metadata), so a miss is a warning, not a fail.
  let npOk = null;
  if (st.nowplaying) npOk = await checkNowPlaying(st.nowplaying);
  return { name: st.name || '?', streamOk, npOk };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  let cfg = {};
  try { cfg = JSON.parse(await readFile(CFG, 'utf8')); } catch {}
  const stations = stationsFrom(args, cfg);
  if (!stations.length) { console.error('No stations (radio.json missing/empty and no --stream).'); return 2; }

  console.log(`Radio check — ${stations.length} station${stations.length > 1 ? 's' : ''} from ${args.stream ? '--stream' : 'radio.json'}`);
  const results = [];
  for (const st of stations) results.push(await checkOne(st));

  console.log(`\n\x1b[1mSummary\x1b[0m`);
  for (const r of results) {
    const s = r.streamOk ? '\x1b[32mok\x1b[0m' : '\x1b[31mFAIL\x1b[0m';
    const n = r.npOk === null ? '-' : r.npOk ? '\x1b[32mok\x1b[0m' : '\x1b[33mwarn\x1b[0m';
    console.log(`  ${r.streamOk ? '\x1b[32m✓\x1b[0m' : '\x1b[31m✗\x1b[0m'} ${(r.name + '').padEnd(20)} stream:${s}  nowplaying:${n}`);
  }
  const allPass = results.every(r => r.streamOk);   // only the stream (what the device needs) gates the exit code
  const failed = results.filter(r => !r.streamOk).length;
  console.log(`\n${allPass ? '\x1b[32mPASS\x1b[0m — every station streams over plain HTTP' : `\x1b[31mFAIL\x1b[0m — ${failed} station(s) the device can't play`}`);
  return allPass ? 0 : 1;
}

main().then((c) => { process.exitCode = c; }).catch((e) => { console.error(e); process.exitCode = 1; });
