// ===========================================================================
// ANIMA voice dictation — runs entirely in the browser, no key, no cloud server.
//
// Two engines, picked at init:
//   • 'vosk'      — on-device streaming ASR (Vosk WASM). Model lives next to the
//                   app on SD (./vosk/models/*.tar.gz), so it works fully offline.
//   • 'webspeech' — the browser SpeechRecognition API (Chrome/Edge route audio to
//                   Google). Fallback when the Vosk lib isn't deployed on SD.
//
// Both need a SECURE CONTEXT (https or localhost): the device's plain-HTTP LAN
// origin blocks the mic, so `available` is false there unless the origin is
// whitelisted (chrome://flags/#unsafely-treat-insecure-origin-as-secure).
//
// Unified handler interface passed to start(lang, h):
//   h.onloading()      model/engine warming up (Vosk first use downloads the model)
//   h.onstart()        actually listening
//   h.onpartial(text)  in-progress words for the current utterance (replaceable)
//   h.onfinal(text)    a finalized chunk to append
//   h.onend()          stopped (manually, on error, or engine ended)
//   h.onerror(kind)    'denied' | 'nomic' | 'model' | 'audio' | 'generic'
// ===========================================================================

// Local-first (served from SD next to the app), with an online CDN fallback so
// dictation still works if the model/lib weren't deployed to SD.
const VOSK_LIB  = './vosk/vosk.js';
const VOSK_CDN  = 'https://cdn.jsdelivr.net/npm/vosk-browser@0.0.8/dist/vosk.js';
const WORKLET   = './vosk/recognizer-processor.js';
const MODELS    = {
  it: { local: './vosk/models/vosk-model-small-it-0.4.tar.gz',    cdn: 'https://ccoreilly.github.io/vosk-browser/models/vosk-model-small-it-0.4.tar.gz' },
  en: { local: './vosk/models/vosk-model-small-en-us-0.15.tar.gz', cdn: 'https://ccoreilly.github.io/vosk-browser/models/vosk-model-small-en-us-0.15.tar.gz' },
};
// Worklet source, inlined as a last-resort fallback if the SD file is missing.
const WORKLET_SRC = `class P extends AudioWorkletProcessor{constructor(o){super(o);this.port.onmessage=e=>{if(e.data.action==='init'){this._id=e.data.recognizerId;this._p=e.ports[0];}};}process(i){const d=i[0][0];if(this._p&&d){const a=d.map(v=>v*0x8000);this._p.postMessage({action:'audioChunk',data:a,recognizerId:this._id,sampleRate},{transfer:[a.buffer]});}return true;}}registerProcessor('recognizer-processor',P);`;

const SR = typeof window !== 'undefined' && (window.SpeechRecognition || window.webkitSpeechRecognition);
const canCapture = () => !!(typeof navigator !== 'undefined' && navigator.mediaDevices && navigator.mediaDevices.getUserMedia
  && (window.isSecureContext || location.hostname === 'localhost' || location.hostname === '127.0.0.1'));

function inject(src) {
  return new Promise((res, rej) => {
    const s = document.createElement('script');
    s.src = src; s.async = true;
    s.onload = () => (window.Vosk ? res(window.Vosk) : rej(new Error('vosk-load')));
    s.onerror = () => rej(new Error('vosk-load'));
    document.head.appendChild(s);
  });
}
async function loadVosk() {                 // local SD copy first, CDN fallback
  if (window.Vosk) return window.Vosk;
  try { return await inject(VOSK_LIB); } catch { return await inject(VOSK_CDN); }
}

// Reassemble a model split across <8MB parts (baseUrl.000, .001, ...) into one blob: URL.
// Returns null if there are no parts (so the caller can fall through to the CDN).
// Each part is fetched with retries: the device's webserver can reset a connection mid
// transfer on large reads, and one dropped part must not abort the whole offline load.
async function assembleParts(baseUrl) {
  const getPart = async (url) => {
    let lastErr;
    for (let a = 0; a < 4; a++) {
      try { const r = await fetch(url, { cache: 'force-cache' }); if (r.ok) return await r.arrayBuffer(); if (r.status === 404) return null; }
      catch (e) { lastErr = e; }
      await new Promise((res) => setTimeout(res, 400 * (a + 1)));   // backoff before retrying a reset
    }
    throw lastErr || new Error('part-fetch');
  };
  const parts = [];
  for (let i = 0; ; i++) {
    const buf = await getPart(`${baseUrl}.${String(i).padStart(3, '0')}`);
    if (buf === null) { if (i === 0) return null; break; }   // no .000 -> no split model present
    parts.push(buf);
  }
  return URL.createObjectURL(new Blob(parts, { type: 'application/gzip' }));
}

export function createVoice() {
  const v = {
    engine: null,          // 'vosk' | 'webspeech' | null
    available: false,
    recognizing: false,
    reason: '',            // why unavailable: 'insecure' | 'nomedia' | 'noengine'
    source: '',            // where the active vosk model came from: 'sd' | 'online'
    _h: null, _ctx: null, _stream: null, _node: null, _src: null, _rec: null, _sr: null,
  };
  const _modelCache = {};   // lang -> Promise<model>  (kept warm across sessions)
  const _srcCache = {};     // lang -> 'sd' | 'online'  (where the model actually loaded from)

  // Load the model: ONLINE FIRST, always. The browser fetches the ~34 MB model DIRECTLY from the
  // CDN (never through the device), so it can't saturate the device's single-task web server —
  // streaming the model via the device webfs collapses every other request (/api/status, /api/fs
  // reset/time out — the read storm seen in the device logs). Only if the CDN is unreachable do we
  // FALL BACK to the SD copy on the Cardputer (whole file, then split parts). When truly offline the
  // CDN attempt fails fast (navigator offline) and we drop straight to SD. Records source, caches.
  async function getModel(Vosk, lang) {
    if (_modelCache[lang]) { if (_srcCache[lang]) v.source = _srcCache[lang]; return _modelCache[lang]; }
    const m = MODELS[lang] || MODELS.it;
    // Bound an SD load: the device webfs can't sustain a large single read — it resets the connection
    // a few MB into a 34 MB model, so Vosk.createModel(local) STALLS forever (the mic stays stuck
    // "loading"). A timeout abandons that dead load and falls through. The orphaned worker just emits
    // a harmless "...reading 'done'" once and is dropped.
    const withTimeout = (promise, ms) => Promise.race([promise,
      new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), ms))]);
    const fromCdn = async () => { const mod = await Vosk.createModel(m.cdn); _srcCache[lang] = 'online'; return mod; };
    const fromSdWhole = async () => { const mod = await withTimeout(Vosk.createModel(m.local), 12000); _srcCache[lang] = 'sd'; return mod; };
    // Split parts on SD (model.tar.gz.000, .001, ...) reassembled in-memory: lets the model live on
    // SD even when pushed over the air in <8 MB chunks (the device's /api/fs write can't take one
    // large file reliably). assembleParts retries dropped parts.
    const fromSdParts = async () => { const url = await assembleParts(m.local); if (!url) throw new Error('noparts');
      const mod = await Vosk.createModel(url); _srcCache[lang] = 'sd'; return mod; };
    const chain = [fromCdn, fromSdWhole, fromSdParts];   // online first; SD (Cardputer) only as fallback
    const p = (async () => {
      let lastErr;
      for (const step of chain) { try { return await step(); } catch (e) { lastErr = e; } }
      throw lastErr || new Error('model');
    })();
    _modelCache[lang] = p;
    try { const mod = await p; v.source = _srcCache[lang]; return mod; }
    catch { delete _modelCache[lang]; delete _srcCache[lang]; throw new Error('model'); }  // neither SD nor CDN
  }

  // Decide which engine to use. Vosk only if its lib is actually deployed on SD.
  v.init = async function init() {
    const secure = window.isSecureContext || location.hostname === 'localhost' || location.hostname === '127.0.0.1';
    if (!secure) { v.available = false; v.engine = null; v.reason = 'insecure'; return v; }  // mic blocked off https/localhost
    if (!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia)) { v.available = false; v.engine = null; v.reason = 'nomedia'; return v; }
    let local = false;
    // Probe with a 1-byte ranged GET, not HEAD: the device webfs only implements GET and
    // answers HEAD with 405 (a noisy console error), while it honors Range requests.
    try { local = (await fetch(VOSK_LIB, { method: 'GET', headers: { Range: 'bytes=0-0' }, cache: 'force-cache' })).ok; } catch {}
    if (local || navigator.onLine) { v.engine = 'vosk'; v.available = true; v.reason = ''; return v; }  // CDN fallback if no SD copy
    if (SR) { v.engine = 'webspeech'; v.available = true; v.reason = ''; return v; }
    v.available = false; v.engine = null; v.reason = 'noengine';
    return v;
  };

  v.start = function start(lang, h) {
    v._h = h || {};
    return v.engine === 'vosk' ? startVosk(lang) : startWeb(lang);
  };

  v.stop = function stop() {
    if (v.engine === 'vosk') stopVosk();
    else if (v._sr) { try { v._sr.stop(); } catch {} }
  };

  // ---- Vosk (on-device) -----------------------------------------------------
  async function startVosk(lang) {
    const h = v._h;
    try {
      h.onloading && h.onloading();
      const Vosk = await loadVosk();
      const model = await getModel(Vosk, lang);

      const sampleRate = 48000;
      const channel = new MessageChannel();
      model.registerPort(channel.port1);
      const rec = new model.KaldiRecognizer(sampleRate);
      rec.setWords(true);
      rec.on('result', (m) => { const t = (m.result.partial !== undefined ? '' : (m.result.text || '')).trim(); if (t) h.onfinal && h.onfinal(t); });
      rec.on('partialresult', (m) => { h.onpartial && h.onpartial((m.result.partial || '').trim()); });

      let stream;
      try {
        stream = await navigator.mediaDevices.getUserMedia({
          video: false,
          audio: { echoCancellation: true, noiseSuppression: true, channelCount: 1, sampleRate },
        });
      } catch (e) { h.onerror && h.onerror(e && e.name === 'NotAllowedError' ? 'denied' : 'nomic'); h.onend && h.onend(); return; }

      const ctx = new AudioContext();
      try { await ctx.audioWorklet.addModule(WORKLET); }
      catch { await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET_SRC], { type: 'text/javascript' }))); }
      const node = new AudioWorkletNode(ctx, 'recognizer-processor', { channelCount: 1, numberOfInputs: 1, numberOfOutputs: 1 });
      node.port.postMessage({ action: 'init', recognizerId: rec.id }, [channel.port2]);
      node.connect(ctx.destination);           // silent output; keeps the node pulled
      const src = ctx.createMediaStreamSource(stream);
      src.connect(node);

      v._ctx = ctx; v._stream = stream; v._node = node; v._src = src; v._rec = rec;
      v.recognizing = true;
      h.onstart && h.onstart();
    } catch (e) {
      const msg = e && e.message;
      h.onerror && h.onerror(msg === 'model' || msg === 'vosk-load' ? 'model' : 'audio');  // engine/model unreachable vs audio-graph failure
      h.onend && h.onend();
    }
  }

  function stopVosk() {
    const h = v._h;
    try { v._src && v._src.disconnect(); } catch {}
    try { v._node && v._node.disconnect(); } catch {}
    try { v._stream && v._stream.getTracks().forEach((t) => t.stop()); } catch {}
    try { v._ctx && v._ctx.close(); } catch {}
    v._src = v._node = v._stream = v._ctx = v._rec = null;
    if (v.recognizing) { v.recognizing = false; h && h.onend && h.onend(); }
  }

  // ---- Web Speech (fallback) ------------------------------------------------
  function startWeb(lang) {
    const h = v._h;
    const rec = new SR();
    v._sr = rec;
    rec.continuous = true; rec.interimResults = true;
    rec.lang = lang === 'en' ? 'en-US' : 'it-IT';
    rec.onstart = () => { v.recognizing = true; h.onstart && h.onstart(); };
    rec.onerror = (e) => { h.onerror && h.onerror(e && e.error === 'not-allowed' ? 'denied' : 'generic'); };
    rec.onend = () => { v.recognizing = false; v._sr = null; h.onend && h.onend(); };
    rec.onresult = (e) => {
      let interim = '';
      for (let i = e.resultIndex; i < e.results.length; i++) {
        const tr = e.results[i][0].transcript;
        if (e.results[i].isFinal) { const t = tr.trim(); if (t) h.onfinal && h.onfinal(t); }
        else interim += tr;
      }
      if (interim.trim()) h.onpartial && h.onpartial(interim.trim());
    };
    try { rec.start(); } catch { /* already started */ }
  }

  return v;
}
