// ===========================================================================
// On-device-mic speech-to-text for the Dictation app — runs entirely in the
// browser. The Cardputer's PDM mic is streamed as raw PCM over /api/rec/stream
// (the device only serves bytes, ~32 KB/s — no ASR load on the MCU) and fed
// straight into Vosk WASM here. No getUserMedia (so it works on the device's
// plain-HTTP origin, no secure context) and no cloud.
//
// Vosk lib + models are REUSED from the ANIMA app on SD (absolute paths) so we
// don't duplicate the ~40 MB model. Online-first model load (browser fetches the
// model directly from a CDN, never through the device) with an SD fallback.
//
// handlers: onloading() · onstart() · onpartial(text) · onfinal(text)
//           onlevel(0..1) · onend() · onerror('model'|'busy'|'audio')
// ===========================================================================

import { streamMic } from '/micgate.js';   // OS-wide single-owner gate for the Cardputer mic

// Shared Vosk assets live next to the ANIMA app (same origin, absolute paths so they
// resolve regardless of which app imports this module).
const VOSK_LIB = '/apps/anima/vosk/vosk.js';
const VOSK_CDN = 'https://cdn.jsdelivr.net/npm/vosk-browser@0.0.8/dist/vosk.js';
const MODELS = {
  it: { local: '/apps/anima/vosk/models/vosk-model-small-it-0.4.tar.gz',    cdn: 'https://ccoreilly.github.io/vosk-browser/models/vosk-model-small-it-0.4.tar.gz' },
  en: { local: '/apps/anima/vosk/models/vosk-model-small-en-us-0.15.tar.gz', cdn: 'https://ccoreilly.github.io/vosk-browser/models/vosk-model-small-en-us-0.15.tar.gz' },
};

function inject(src) {
  return new Promise((res, rej) => {
    const s = document.createElement('script');
    s.src = src; s.async = true;
    s.onload = () => (window.Vosk ? res(window.Vosk) : rej(new Error('vosk-load')));
    s.onerror = () => rej(new Error('vosk-load'));
    document.head.appendChild(s);
  });
}
async function loadVosk() {
  if (window.Vosk) return window.Vosk;
  try { return await inject(VOSK_LIB); } catch { return await inject(VOSK_CDN); }
}

// Reassemble a model split across <8 MB parts (baseUrl.000, .001, ...) into one blob: URL —
// the form the device can hold on SD (its /api/fs/write can't take one huge file). Returns
// null if there are no parts. Each part retries: the device webserver can reset a big read.
async function assembleParts(baseUrl) {
  const getPart = async (url) => {
    let lastErr;
    for (let a = 0; a < 4; a++) {
      try { const r = await fetch(url, { cache: 'force-cache' }); if (r.ok) return await r.arrayBuffer(); if (r.status === 404) return null; }
      catch (e) { lastErr = e; }
      await new Promise((res) => setTimeout(res, 400 * (a + 1)));
    }
    throw lastErr || new Error('part-fetch');
  };
  const parts = [];
  for (let i = 0; ; i++) {
    const buf = await getPart(`${baseUrl}.${String(i).padStart(3, '0')}`);
    if (buf === null) { if (i === 0) return null; break; }
    parts.push(buf);
  }
  return URL.createObjectURL(new Blob(parts, { type: 'application/gzip' }));
}

export function createASR() {
  const a = { recognizing: false, source: '', _h: null, _ac: null, _reader: null, _rec: null, groqKey: '', groqBase: '', engine: 'auto' };
  const _modelCache = {};

  a.setGroq = function(key, base) {
    a.groqKey = key || '';
    a.groqBase = base || 'https://api.groq.com/openai/v1';
  };

  a.setEngine = function(engine) {
    a.engine = engine || 'auto';
  };

  // ONLINE FIRST: the browser pulls the ~34 MB model straight from the CDN so it never
  // saturates the device's single-task web server; only if the CDN is unreachable do we fall
  // back to the SD copy (whole file, then split parts) served by the Cardputer.
  async function getModel(Vosk, lang) {
    if (_modelCache[lang]) return _modelCache[lang];
    const m = MODELS[lang] || MODELS.it;
    const withTimeout = (p, ms) => Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), ms))]);
    const fromCdn = async () => { const mod = await Vosk.createModel(m.cdn); a.source = 'online'; return mod; };
    const fromSdWhole = async () => { const mod = await withTimeout(Vosk.createModel(m.local), 12000); a.source = 'sd'; return mod; };
    const fromSdParts = async () => { const url = await assembleParts(m.local); if (!url) throw new Error('noparts');
      const mod = await Vosk.createModel(url); a.source = 'sd'; return mod; };
    const p = (async () => { let last; for (const step of [fromCdn, fromSdWhole, fromSdParts]) { try { return await step(); } catch (e) { last = e; } } throw last || new Error('model'); })();
    _modelCache[lang] = p;
    try { return await p; } catch { delete _modelCache[lang]; throw new Error('model'); }
  }

  // The mic itself (exclusivity, the /api/rec/stream fetch, odd-byte tails, reconnect across the
  // firmware's 5-min cap) is owned by the OS-wide gate (micgate.js). Here we only stand up the
  // recognizer (Vosk) or the Groq batcher and transform the raw frames the gate hands us.
  a.start = function start(lang, h) {
    a._h = h || {};
    const handlers = a._h;
    const ac = new AbortController();
    a._ac = ac;
    (async () => {
      let useGroq = (a.engine === 'groq');
      let Vosk = null, model = null;
      try {
        handlers.onloading && handlers.onloading();
        if (!useGroq) {
          try { Vosk = await loadVosk(); model = await getModel(Vosk, lang); }
          catch (err) {
            if (a.engine !== 'vosk' && a.groqKey) { useGroq = true; a.source = 'online'; }
            else throw err;
          }
        } else { a.source = 'online'; }
      } catch (e) {
        const msg = e && e.message;
        if (!ac.signal.aborted) handlers.onerror && handlers.onerror((msg === 'model' || msg === 'vosk-load') ? 'model' : 'audio');
        stop(); return;
      }

      // Vosk recognizer (offline path) — stays alive across the gate's transparent reconnects.
      let rec = null, channel = null;
      if (!useGroq) {
        channel = new MessageChannel();
        model.registerPort(channel.port1);
        rec = new model.KaldiRecognizer(16000);
        rec.setWords(true);
        rec.on('result', (m) => { const t = (m.result.partial !== undefined ? '' : (m.result.text || '')).trim(); if (t) handlers.onfinal && handlers.onfinal(t); });
        rec.on('partialresult', (m) => { handlers.onpartial && handlers.onpartial((m.result.partial || '').trim()); });
        a._rec = rec;
      }

      // Groq (online) path: accumulate gained PCM bytes and ship a WAV segment every few seconds.
      let audioBuffer = [], lastSendTime = Date.now();
      const SEND_INTERVAL = 4000;
      const sendBuffer = async () => {
        if (!audioBuffer.length) return;
        const chunks = audioBuffer; audioBuffer = [];
        const totalBytes = chunks.reduce((acc, c) => acc + c.length, 0);
        const pcm = new Uint8Array(totalBytes); let off = 0;
        for (const c of chunks) { pcm.set(c, off); off += c.length; }
        const wavBlob = new Blob([writeWavHeader(totalBytes), pcm], { type: 'audio/wav' });
        try { const text = await transcribeAudioSegment(wavBlob, lang, a.groqKey, a.groqBase); if (text && text.trim()) handlers.onfinal && handlers.onfinal(text); }
        catch (err) { console.error('Groq transcription failed:', err); }
      };

      a.recognizing = true;
      streamMic({
        label: 'Dettatura', signal: ac.signal,
        onstart() { a.recognizing = true; handlers.onstart && handlers.onstart(); },
        onstatus(state) { handlers.onstatus && handlers.onstatus(state); },   // 'live' | 'reconnecting'
        onlevel(v) { handlers.onlevel && handlers.onlevel(v); },
        onframe(s16, info) {
          if (useGroq) {
            audioBuffer.push(info.gainedBytes());                 // gained int16 LE bytes for the WAV
            if (Date.now() - lastSendTime > SEND_INTERVAL) { sendBuffer(); lastSendTime = Date.now(); }
          } else {
            const f = new Float32Array(s16.length);
            for (let i = 0; i < s16.length; i++) f[i] = Math.max(-32768, Math.min(32767, s16[i] * 2.5));
            channel.port2.postMessage({ action: 'audioChunk', data: f, recognizerId: rec.id, sampleRate: 16000 }, [f.buffer]);
          }
        },
        onerror(kind, who) { if (!ac.signal.aborted) handlers.onerror && handlers.onerror(kind, who); },   // 'busy'(+holder) | 'auth' | 'audio'
        onend() { if (useGroq) sendBuffer(); stop(); },
      });
    })();
  };

  function stop() {
    const h = a._h;
    try { a._reader && a._reader.cancel(); } catch {}
    try { a._ac && a._ac.abort(); } catch {}
    a._ac = null; a._reader = null; a._rec = null;
    if (a.recognizing) { a.recognizing = false; h && h.onend && h.onend(); }
  }
  a.stop = stop;

  return a;
}

function writeWavHeader(dataLength) {
  const buffer = new ArrayBuffer(44);
  const view = new DataView(buffer);
  
  view.setUint8(0, 0x52); // 'R'
  view.setUint8(1, 0x49); // 'I'
  view.setUint8(2, 0x46); // 'F'
  view.setUint8(3, 0x46); // 'F'
  view.setUint32(4, 36 + dataLength, true);
  view.setUint8(8, 0x57); // 'W'
  view.setUint8(9, 0x41); // 'A'
  view.setUint8(10, 0x56); // 'V'
  view.setUint8(11, 0x45); // 'E'
  view.setUint8(12, 0x66); // 'f'
  view.setUint8(13, 0x6d); // 'm'
  view.setUint8(14, 0x74); // 't'
  view.setUint8(15, 0x20); // ' '
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true); // PCM
  view.setUint16(22, 1, true); // Mono
  view.setUint32(24, 16000, true); // Sample Rate 16kHz
  view.setUint32(28, 16000 * 2, true); // Byte Rate (16000 * 1 channel * 2 bytes/sample)
  view.setUint16(32, 2, true); // Block Align
  view.setUint16(34, 16, true); // Bits per sample
  view.setUint8(36, 0x64); // 'd'
  view.setUint8(37, 0x61); // 'a'
  view.setUint8(38, 0x74); // 't'
  view.setUint8(39, 0x61); // 'a'
  view.setUint32(40, dataLength, true);
  
  return new Uint8Array(buffer);
}

async function transcribeAudioSegment(wavBlob, lang, key, base) {
  const formData = new FormData();
  formData.append('file', wavBlob, 'audio.wav');
  formData.append('model', 'whisper-large-v3');
  if (lang) formData.append('language', lang);
  formData.append('response_format', 'json');

  const apiBase = (base && base.includes('api.groq.com')) ? 'https://api.groq.com/v1' : (base || 'https://api.groq.com/v1');
  const url = apiBase.replace(/\/openai\/v1$/, '/v1').replace(/\/$/, '') + '/audio/transcriptions';
  
  const response = await fetch(url, {
    method: 'POST',
    headers: {
      'Authorization': 'Bearer ' + key
    },
    body: formData
  });

  if (!response.ok) {
    const errText = await response.text().catch(() => '');
    throw new Error(`Groq Whisper error: HTTP ${response.status} - ${errText}`);
  }

  const result = await response.json();
  return result.text || '';
}
