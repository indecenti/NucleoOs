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

import { streamMic } from '/micgate.js';   // OS-wide single-owner gate for the Cardputer mic

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
    engine: null,          // 'vosk' | 'webspeech' | null  (the BROWSER-mic engine)
    available: false,      // either path usable
    browserAvailable: false, // browser-mic path usable (secure context + getUserMedia + engine)
    deviceAvailable: false,  // Cardputer-mic path usable (/api/rec/stream + a loadable Vosk model)
    recognizing: false,
    reason: '',            // why the BROWSER path is unavailable: 'insecure' | 'nomedia' | 'noengine'
    source: '',            // active source: vosk model origin 'sd'|'online', or 'device' for the Cardputer mic
    _h: null, _ctx: null, _stream: null, _node: null, _src: null, _rec: null, _sr: null,
    _active: '', _ac: null, _reader: null,   // _active: 'cardputer'|'vosk'|'webspeech'
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

  // Decide which engines are usable. Two independent paths:
  //   • BROWSER mic — getUserMedia + Vosk (or the Web Speech fallback). Needs a secure context.
  //   • CARDPUTER mic — the device's PDM mic streamed over /api/rec/stream, fed into Vosk. No
  //     getUserMedia, so it works on the device's plain-HTTP origin (no secure context needed).
  v.init = async function init() {
    const secure = window.isSecureContext || location.hostname === 'localhost' || location.hostname === '127.0.0.1';
    const media  = !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia);
    // Probe for the Vosk lib on SD with a 1-byte ranged GET, not HEAD: the device webfs only
    // implements GET and answers HEAD with 405 (a noisy console error) while honoring Range.
    let local = false;
    try { local = (await fetch(VOSK_LIB, { method: 'GET', headers: { Range: 'bytes=0-0' }, cache: 'force-cache' })).ok; } catch {}
    const voskLoadable = local || navigator.onLine;   // SD copy first, CDN otherwise

    // The Cardputer path needs Vosk (Web Speech can't accept arbitrary PCM). The app is served BY
    // the device, so /api/rec/stream is same-origin and present — availability hinges on the model.
    v.deviceAvailable = voskLoadable;

    if (secure && media && voskLoadable) { v.engine = 'vosk'; v.browserAvailable = true; v.reason = ''; }
    else if (secure && media && SR)      { v.engine = 'webspeech'; v.browserAvailable = true; v.reason = ''; }
    else { v.engine = voskLoadable ? 'vosk' : null; v.browserAvailable = false;
           v.reason = !secure ? 'insecure' : !media ? 'nomedia' : 'noengine'; }

    v.available = v.browserAvailable || v.deviceAvailable;
    return v;
  };

  // source: 'cardputer' uses the on-device mic stream; otherwise the browser mic (auto engine).
  v.start = function start(lang, h, source) {
    v._h = h || {};
    if (source === 'cardputer') { v._active = 'cardputer'; return startCardputer(lang); }
    v._active = v.engine === 'vosk' ? 'vosk' : 'webspeech';
    return v.engine === 'vosk' ? startVosk(lang) : startWeb(lang);
  };

  v.stop = function stop() {
    if (v._active === 'cardputer') stopCardputer();
    else if (v._active === 'webspeech') { if (v._sr) { try { v._sr.stop(); } catch {} } }
    else stopVosk();
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

  // ---- Cardputer (on-device mic, streamed) ----------------------------------
  // Feed the device's 16 kHz mono PCM straight into Vosk — no getUserMedia, no AudioWorklet — so
  // dictation works from the device's plain-HTTP origin. The mic is acquired through the OS-wide gate
  // (micgate.js): exclusivity, the /api/rec/stream fetch, odd-byte tails and reconnect-across-the-cap
  // all live there, so two apps can never fight over the one Cardputer mic. The recognizer is built at
  // the native 16 kHz (no resampling).
  async function startCardputer(lang) {
    const h = v._h;
    const ac = new AbortController();
    v._ac = ac;
    let rec = null, channel = null;
    try {
      h.onloading && h.onloading();
      const Vosk = await loadVosk();
      const model = await getModel(Vosk, lang);

      const sampleRate = 16000;                 // matches REC_RATE_HZ in the firmware
      channel = new MessageChannel();
      model.registerPort(channel.port1);
      rec = new model.KaldiRecognizer(sampleRate);
      rec.setWords(true);
      rec.on('result', (m) => { const t = (m.result.partial !== undefined ? '' : (m.result.text || '')).trim(); if (t) h.onfinal && h.onfinal(t); });
      rec.on('partialresult', (m) => { h.onpartial && h.onpartial((m.result.partial || '').trim()); });
      v._rec = rec;
    } catch (e) {
      const msg = e && e.message;
      h.onerror && h.onerror(msg === 'model' || msg === 'vosk-load' ? 'model' : 'audio');
      h.onend && h.onend();
      return;
    }

    v.recognizing = true; v.source = 'device';
    streamMic({
      label: 'ANIMA', signal: ac.signal,
      onstart() { h.onstart && h.onstart(); },
      onstatus(state) { h.onstatus && h.onstatus(state); },   // 'live' | 'reconnecting'
      onframe(s16) {                            // raw int16 from the gate → lift ×2.5 → float for Vosk
        const f = new Float32Array(s16.length);
        for (let i = 0; i < s16.length; i++) f[i] = Math.max(-32768, Math.min(32767, s16[i] * 2.5));
        channel.port2.postMessage({ action: 'audioChunk', data: f, recognizerId: rec.id, sampleRate: 16000 }, [f.buffer]);
      },
      onerror(kind, who) { h.onerror && h.onerror(kind, who); },   // 'busy'(+holder) | 'auth' | 'audio'
      onend() { v._ac = null; v._rec = null; if (v.recognizing) { v.recognizing = false; h.onend && h.onend(); } },
    });
  }

  function stopCardputer() { try { v._ac && v._ac.abort(); } catch {} }   // the gate's onend does the rest

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
