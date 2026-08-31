// Host gate for nucleo_take: compile the REAL segmenter + journal writer
// (firmware/components/nucleo_take/nucleo_take.c) with take-ctest.c, run its assertions, then
// re-read the journal it produced as JSON — the way the browser and the transcription worker will.
// Mirrors ducky-check.mjs / eth-check.mjs: prove the take pipeline on the PC, flash only to confirm.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'takectest.exe');
const journal = join(BUILD, 'take-journal.ndjson');
const wav = join(BUILD, 'take-repair.wav');

const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', 'firmware/components/nucleo_take/include',
  'tools/anima-host/take-ctest.c',
  'firmware/components/nucleo_take/nucleo_take.c',
  '-o', exe, '-lm',
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_take: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const run = spawnSync(exe, [journal, wav], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
if (run.status !== 0) process.exit(run.status === null ? 1 : run.status);

// ── the journal, read back from the outside ────────────────────────────────────────────────────
// The C side asserts the segmenter. This side asserts the CONTRACT: every line is standalone JSON
// (so a reader can stream it, and a truncated last line costs exactly one segment), the timeline
// has no holes, and the totals agree with the segments.
let fails = 0;
const bad = (m) => { console.log(`  FAIL journal-contract: ${m}`); fails++; };

const lines = readFileSync(journal, 'utf8').split('\n').filter((l) => l.length);
const rows = [];
for (const [i, l] of lines.entries()) {
  try { rows.push(JSON.parse(l)); } catch { bad(`line ${i + 1} is not standalone JSON`); }
}

const open = rows.find((r) => r.t === 'open');
const end = rows.find((r) => r.t === 'end');
const segs = rows.filter((r) => r.t === 'seg');

if (!open) bad('no open line');
else {
  if (open.v !== 1) bad(`journal version ${open.v}, expected 1`);
  if (open.rate !== 16000 || open.ch !== 1 || open.bits !== 16) bad('open line lost the audio format');
  if (open.hdr !== 44) bad('open line must record the header size so offsets stay data-relative');
}
if (!end) bad('no end line');
if (rows[0] !== open) bad('the open line must come first');
if (rows[rows.length - 1] !== end) bad('the end line must come last');
if (!segs.length) bad('no segments');

let expect = 0;
for (const [i, s] of segs.entries()) {
  if (s.i !== i) bad(`segment ${i} carries index ${s.i}`);
  if (s.b0 !== expect) bad(`segment ${i} starts at ${s.b0}, previous ended at ${expect}`);
  if (s.b1 <= s.b0) bad(`segment ${i} is empty`);
  if (typeof s.voiced !== 'boolean') bad(`segment ${i} has no voicing flag`);
  // t and b must describe the same instant, or a click-to-seek lands in the wrong place.
  const t0 = s.b0 / 32000, t1 = s.b1 / 32000;
  if (Math.abs(s.t0 - t0) > 0.002 || Math.abs(s.t1 - t1) > 0.002)
    bad(`segment ${i} timestamps disagree with its byte offsets`);
  expect = s.b1;
}
if (end && end.bytes !== expect) bad(`end claims ${end.bytes} bytes, segments cover ${expect}`);
if (end && end.segs !== segs.length) bad(`end claims ${end.segs} segments, journal holds ${segs.length}`);

const voiced = segs.filter((s) => s.voiced).length;
if (voiced < 1) bad('no voiced segment survived — nothing would ever be transcribed');

// The point of the whole exercise: a power cut mid-line must cost one segment, not the take. Tear
// the LAST line in the MIDDLE of a field (not just the trailing newline, which can never fail once
// every line already parsed above): the reader must still recover every earlier line and lose only
// the torn one.
{
  const full = readFileSync(journal, 'utf8');
  const nl = full.lastIndexOf('\n', full.length - 2);            // start of the last non-empty line
  const torn = full.slice(0, nl + 1 + 12);                       // keep 12 bytes of the last line, then cut
  const kept = torn.split('\n').filter((l) => l.length);
  let recovered = 0;
  for (const l of kept) { try { JSON.parse(l); recovered++; } catch { /* the torn tail */ } }
  if (recovered !== kept.length - 1) bad(`a mid-field tear recovered ${recovered} of ${kept.length - 1} intact lines`);
}


// ── uploader invariants (source lint) ──────────────────────────────────────────────────────────
// The persistent-connection uploader in nucleo_anima_online.c cannot be compiled on the host (it is
// ESP-IDF all the way down), but the rules it must obey are simple enough to check in the source —
// and they are exactly the rules an innocent-looking edit breaks. Same spirit as gz:check/ui:lint.
const ONLINE = join(ROOT, 'firmware/components/nucleo_anima/nucleo_anima_online.c');
const src = readFileSync(ONLINE, 'utf8');
const body = (name) => {
  const at = src.indexOf(`static int ${name}(`);
  if (at < 0) return null;
  let i = src.indexOf('\n{', at), depth = 0, j = i + 1;
  for (; j < src.length; j++) {
    if (src[j] === '{') depth++;
    else if (src[j] === '}') { depth--; if (!depth) break; }
  }
  return src.slice(at, j + 1);
};

const slice = body('transcribe_slice');
if (!slice) bad('transcribe_slice not found — did the uploader move?');
else {
  // Rule 1: never tear the connection down between segments. esp_http_client_close() calls
  // esp_transport_close(); calling it here would silently cost 80 handshakes on a 2 h take.
  if (/esp_http_client_close|esp_http_client_cleanup/.test(slice))
    bad('transcribe_slice closes/cleans the client — that kills the persistent connection');
  // Rule 2: the request must go through tx_conn_begin, which re-dials a dirty socket.
  if (!/tx_conn_begin\(/.test(slice)) bad('transcribe_slice does not open through tx_conn_begin');
  // Rule 3: a half-written body must poison the connection, not be handed to the next segment.
  if (!/c->dirty = true/.test(slice)) bad('transcribe_slice never marks the connection dirty on a write failure');
  // Rule 4: ADPCM Content-Length must come from the codec's own size function, never a hand rolled
  // guess — a byte of drift makes the request hang until the timeout.
  if (!/nucleo_take_adpcm_size\(/.test(slice)) bad('the ADPCM Content-Length is not taken from nucleo_take_adpcm_size');
}

const drain = body('tx_read_body');
if (!drain) bad('tx_read_body not found');
else if (!/esp_http_client_is_complete_data_received/.test(drain))
  bad('tx_read_body does not verify the body was fully drained — the next response would desync');

if (!/conn\.codec_locked = true/.test(src))
  bad('the ADPCM probe is not locked after the first segment — it would re-probe all take long');
// The codec lock must be GATED on a definitive answer, not set unconditionally: a transient failure
// on the first slice (TLS-open fail leaves last_status 0; a 5xx/429) must leave the probe armed.
if (/conn\.codec_locked = true;\s*(\/\/[^\n]*)?\s*for \(int rtry/.test(src))
  bad('codec_locked is set unconditionally before the retry loop — a transient first slice permanently pins the codec');
// Durability: the transcript journal (the resume record) and the sidecar must be fsync'd, not just
// fflush'd — on FATFS the dir entry (file size) is committed only by sync/close, so a crash mid-take
// would otherwise make a resume re-pay for every upload already done this run.
if (!/fsync\(fileno\(tx\)\)/.test(src)) bad('transcribe_by_journal does not fsync the transcript journal — a crash loses this run\'s entries');
if (!/fsync\(fileno\(out\)\)/.test(src)) bad('the transcript sidecar is not fsync\'d — a crash loses partial text');

// ── recorder-side durability (source lint) ───────────────────────────────────────────────────────
// The capture loop's whole promise ("a crash costs the last unflushed line") rests on fsync AFTER
// the per-segment journal write: fflush alone leaves the FAT dir entry at 0 bytes. Protect it.
const REC = join(ROOT, 'firmware/components/nucleo_recorder/nucleo_recorder.c');
const rsrc = readFileSync(REC, 'utf8');
if (!/nucleo_take_journal_seg\(jf, &seg\);\s*\n\s*fsync\(fileno\(jf\)\)/.test(rsrc))
  bad('nucleo_recorder: fsync(fileno(jf)) must immediately follow nucleo_take_journal_seg — else a power cut reads back a 0-byte journal');
if (!/fsync\(fileno\(f\)\)/.test(rsrc))
  bad('nucleo_recorder: the WAV is fflush\'d but never fsync\'d — a power cut leaves fsize 0 and the take unreachable');

console.log(fails
  ? `journal contract: ${fails} failed`
  : `journal contract + uploader invariants: ok (${segs.length} segments, ${voiced} voiced, ${expect} bytes)`);
process.exit(fails ? 1 : 0);
