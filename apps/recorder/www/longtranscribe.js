// longtranscribe.js — client-side CHUNKED transcription + summary for LONG recordings (1–2 h+).
//
// Why this exists: the device path (/api/transcribe) streams the WHOLE WAV to Whisper from the
// PSRAM-less ESP32. That caps out fast — Whisper itself rejects any file > 25 MB (a 16 kHz mono WAV
// is ~1.92 MB/min → ~13 min max), and the on-device TLS upload of a multi-MB file is fragile. The
// browser has none of those limits: it pulls the WAV in byte ranges (the device already serves
// /api/fs/read with HTTP Range / 206), slices it into safe segments, transcribes each DIRECTLY against
// Whisper (the user's Groq key, same one AI Chat / Dictation use), then stitches the text. A 2-hour
// take becomes ~15 independent ≤8-min requests — bounded, resumable, and the device never holds the file.
//
// Boundaries are cut on whole audio frames at a fixed wall-clock length. No overlap: Whisper transcribes
// each segment independently and the seam lands mid-pause often enough; a rare split word is a fair price
// for never dropping or duplicating a whole sentence. Summary of a long transcript is map-reduce: summarize
// each segment, then summarize the summaries (one transcript can be 100k+ chars — over any single context).

const WHISPER_MODEL = 'whisper-large-v3';
const CHUNK_SECONDS = 8 * 60;          // 8 min/segment → ~15 MB of 16k mono WAV, safely under Whisper's 25 MB
const SUMMARY_PIECE_CHARS = 6000;      // map step: transcript window per partial summary (well within context)

const readUrl = (p) => '/api/fs/read?path=' + encodeURIComponent(p);

// ── WAV header --------------------------------------------------------------------------------------
// Parse just the fmt + data-chunk location from the file's first bytes (standard 44-byte device header,
// but we scan chunks so a fact/LIST chunk before data doesn't break us).
function parseHeader(buf) {
  const dv = new DataView(buf);
  if (dv.byteLength < 16 || dv.getUint32(0, false) !== 0x52494646 /*RIFF*/) throw new Error('not a WAV');
  let off = 12, fmt = null, dataOff = 0, dataLen = 0;
  while (off + 8 <= dv.byteLength) {
    const id = String.fromCharCode(dv.getUint8(off), dv.getUint8(off + 1), dv.getUint8(off + 2), dv.getUint8(off + 3));
    const sz = dv.getUint32(off + 4, true);
    if (id === 'fmt ') {
      fmt = { channels: dv.getUint16(off + 10, true), sampleRate: dv.getUint32(off + 12, true), bits: dv.getUint16(off + 22, true) };
    } else if (id === 'data') { dataOff = off + 8; dataLen = sz; break; }
    off += 8 + sz + (sz & 1);
  }
  if (!fmt || !dataOff) throw new Error('WAV header incomplete');
  return { fmt, dataOff, dataLen };
}

// Build a 44-byte WAV header for a PCM slice of `dataLen` bytes in the same format.
function wavHeader(dataLen, fmt) {
  const b = new ArrayBuffer(44), v = new DataView(b);
  const bytesPerSec = fmt.sampleRate * fmt.channels * (fmt.bits / 8);
  const wr = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  wr(0, 'RIFF'); v.setUint32(4, 36 + dataLen, true); wr(8, 'WAVE');
  wr(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true);
  v.setUint16(22, fmt.channels, true); v.setUint32(24, fmt.sampleRate, true);
  v.setUint32(28, bytesPerSec, true); v.setUint16(32, fmt.channels * (fmt.bits / 8), true);
  v.setUint16(34, fmt.bits, true); wr(36, 'data'); v.setUint32(40, dataLen, true);
  return new Uint8Array(b);
}

// ── network primitives ------------------------------------------------------------------------------
async function fetchRange(path, start, endInclusive, signal) {
  const r = await fetch(readUrl(path), { headers: { Range: `bytes=${start}-${endInclusive}` }, signal });
  if (!(r.status === 206 || r.status === 200)) throw new Error('range read ' + r.status);
  return new Uint8Array(await r.arrayBuffer());
}

async function whisper(wavBlob, lang, key, base, signal) {
  const fd = new FormData();
  fd.append('model', WHISPER_MODEL);
  fd.append('response_format', 'text');
  if (lang && lang !== 'auto') fd.append('language', lang);
  fd.append('file', wavBlob, 'chunk.wav');
  const r = await fetch(base.replace(/\/$/, '') + '/audio/transcriptions', {
    method: 'POST', headers: { Authorization: 'Bearer ' + key }, body: fd, signal,
  });
  if (!r.ok) throw new Error('whisper ' + r.status + (r.status === 401 ? ' (key?)' : ''));
  return (await r.text()).trim();
}

async function chat(messages, key, base, signal) {
  const model = localStorage.getItem('groq.model') || 'llama-3.3-70b-versatile';
  const r = await fetch(base.replace(/\/$/, '') + '/chat/completions', {
    method: 'POST', headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + key },
    body: JSON.stringify({ model, temperature: 0.3, messages }), signal,
  });
  if (!r.ok) throw new Error('summary ' + r.status);
  const j = await r.json();
  return (j.choices?.[0]?.message?.content || '').trim();
}

// ── public API --------------------------------------------------------------------------------------
// Transcribe a (possibly hours-long) WAV on the device, chunk by chunk, entirely in the browser.
// onProgress({phase:'transcribe', done, total}) fires per segment. Returns the full transcript string.
export async function transcribeLong({ path, lang = 'auto', onProgress, signal } = {}) {
  const key = (localStorage.getItem('groq.key') || '').trim();
  const base = (localStorage.getItem('groq.base') || 'https://api.groq.com/openai/v1').trim();
  if (!key) throw new Error('no-key');

  const head = await fetchRange(path, 0, 255, signal);          // enough for any sane header
  const { fmt, dataOff, dataLen } = parseHeader(head.buffer);
  const bytesPerSec = fmt.sampleRate * fmt.channels * (fmt.bits / 8);
  const frame = fmt.channels * (fmt.bits / 8);
  let chunkBytes = CHUNK_SECONDS * bytesPerSec;
  chunkBytes -= chunkBytes % frame;                            // align to whole frames
  const total = Math.max(1, Math.ceil(dataLen / chunkBytes));

  let out = '';
  for (let i = 0; i < total; i++) {
    if (signal?.aborted) throw new Error('aborted');
    const start = dataOff + i * chunkBytes;
    const end = Math.min(dataOff + dataLen, start + chunkBytes) - 1;
    if (end < start) break;
    onProgress?.({ phase: 'transcribe', done: i, total });
    const pcm = await fetchRange(path, start, end, signal);
    const blob = new Blob([wavHeader(pcm.byteLength, fmt), pcm], { type: 'audio/wav' });
    const text = await whisper(blob, lang, key, base, signal);
    if (text) out += (out ? ' ' : '') + text;
  }
  onProgress?.({ phase: 'transcribe', done: total, total });
  return out;
}

// Map-reduce summary of an arbitrarily long transcript. onProgress({phase:'summarize', done, total}).
export async function summarizeLong({ text, lang = 'it', onProgress, signal } = {}) {
  const key = (localStorage.getItem('groq.key') || '').trim();
  const base = (localStorage.getItem('groq.base') || 'https://api.groq.com/openai/v1').trim();
  if (!key) throw new Error('no-key');
  const L = (lang === 'en') ? 'English' : (lang === 'it') ? 'Italian' : lang;

  // Short transcript: one pass.
  if (text.length <= SUMMARY_PIECE_CHARS) {
    onProgress?.({ phase: 'summarize', done: 0, total: 1 });
    const s = await chat([
      { role: 'system', content: `Summarize the user's transcript in ${L}, as concise bullet points capturing decisions, facts and action items.` },
      { role: 'user', content: text },
    ], key, base, signal);
    onProgress?.({ phase: 'summarize', done: 1, total: 1 });
    return s;
  }

  // MAP: summarize each window. REDUCE: summarize the joined partials.
  const pieces = [];
  for (let i = 0; i < text.length; i += SUMMARY_PIECE_CHARS) pieces.push(text.slice(i, i + SUMMARY_PIECE_CHARS));
  const partials = [];
  for (let i = 0; i < pieces.length; i++) {
    if (signal?.aborted) throw new Error('aborted');
    onProgress?.({ phase: 'summarize', done: i, total: pieces.length + 1 });
    partials.push(await chat([
      { role: 'system', content: `This is part ${i + 1}/${pieces.length} of a long transcript. Summarize THIS part in ${L} as terse notes (facts, decisions, action items). No preamble.` },
      { role: 'user', content: pieces[i] },
    ], key, base, signal));
  }
  onProgress?.({ phase: 'summarize', done: pieces.length, total: pieces.length + 1 });
  const final = await chat([
    { role: 'system', content: `Merge these section notes of one long recording into a single coherent summary in ${L}: clear bullet points for decisions, key facts, and a final "Action items" list. Remove duplicates.` },
    { role: 'user', content: partials.join('\n\n') },
  ], key, base, signal);
  onProgress?.({ phase: 'summarize', done: pieces.length + 1, total: pieces.length + 1 });
  return final;
}

// Heuristic: is this file big enough that the device single-shot path would choke? (~12 min of 16k WAV.)
export function isLongWav(sizeBytes) { return sizeBytes > 23 * 1024 * 1024; }
