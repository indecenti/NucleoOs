// install-flow.js — the resilient "install an offline model" experience for NucleoOS, sitting on top of
// model-store.js. This is THE core of the offline feature: it pulls a model's weights (online CDN first,
// else the Cardputer SD) into the browser Cache API so the model then runs with the network unplugged.
//
// Four product rules live here, enforced (and host-tested), not just intended:
//   • AUTO-RESUME — a dropped connection (the Cardputer went away mid-pull, Wi-Fi blip) is NOT a failure:
//     the driver waits with capped exponential backoff and re-runs download(), which skips every shard
//     already verified in the cache. It keeps resuming until the model is complete OR the user cancels.
//   • WAIT-OR-CANCEL — while a download runs the ONLY two things the user can do are watch it or cancel.
//     The driver owns one AbortController; the modal (and the OS-wide scrim it raises) expose just Cancel.
//   • CLEAR ERRORS — every failure class maps to one plain-language message that says what went wrong AND
//     what to do about it (re-sync the SD, free space, switch to the CPU model, …). No raw stack strings.
//   • ONE AT A TIME — the whole run is wrapped in the OS-wide download lock (dlgate), so two model pulls
//     (or a model pull and a voice-pack pull) can never collide on the single-task device httpd.
//
// The logic (prereq check, error→message map, backoff, the driver loop) is DOM-free and I/O-injected, so it
// runs deterministically host-side (tools/anima-host/forge-install-flow.test.mjs). The DOM modal + the real
// dlgate/store are wired in by install-modal.js / the ANIMA app.

// ---- formatting helpers (pure) -----------------------------------------------------------------------
export function humanBytes(n) {
  if (!Number.isFinite(n) || n <= 0) return '0 B';
  if (n >= 1e9) return (n / 1e9).toFixed(2) + ' GB';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + ' MB';
  if (n >= 1e3) return (n / 1e3).toFixed(0) + ' KB';
  return n + ' B';
}
// Seconds left given bytes done/total and a bytes/sec rate. null when it can't be estimated yet.
export function etaSeconds(bytesDone, bytesTotal, rate) {
  if (!rate || rate <= 0 || !bytesTotal || bytesTotal <= bytesDone) return null;
  return Math.ceil((bytesTotal - bytesDone) / rate);
}
export function etaText(sec, lang) {
  if (sec == null) return '';
  const en = lang === 'en';
  if (sec < 60) return Math.max(1, Math.round(sec)) + (en ? 's left' : 's rimasti');
  const m = Math.round(sec / 60);
  if (m < 60) return m + (en ? ' min left' : ' min rimasti');
  const h = Math.floor(m / 60), mm = m % 60;
  return h + 'h ' + mm + 'm' + (en ? ' left' : ' rimasti');
}

// Capped exponential backoff between auto-resume attempts. Deterministic (no jitter) → host-testable.
export function backoffMs(attempt, { base = 1200, cap = 15000 } = {}) {
  const a = Math.max(1, attempt | 0);
  return Math.min(cap, base * Math.pow(2, a - 1));
}

// ---- prerequisites (pure) ----------------------------------------------------------------------------
// Can the engine this model targets actually RUN in this browser? Downloading a model you can't run is a
// dead end, so we block up-front with a redirect. caps: { webgpu, wasm, online }.
//   reason ∈ null | 'no-webgpu' | 'no-wasm'
export function prereqFor(kind, caps = {}) {
  if (kind === 'webgpu' && !caps.webgpu) return { ok: false, reason: 'no-webgpu' };
  if (kind === 'wasm' && !caps.wasm) return { ok: false, reason: 'no-wasm' };
  return { ok: true, reason: null };
}

// ---- error / status → user-facing copy (pure) --------------------------------------------------------
// One plain message per failure CLASS. `ctx`: { modelLabel, sizeText, kind, attempt }. Returns
// { title, detail, fatal, canRetry }. `fatal` means "this won't fix itself" (the modal offers Retry/Close);
// non-fatal classes are handled by auto-resume and never shown as a dead end.
export function messageFor(kind, ctx = {}, lang = 'it') {
  const en = lang === 'en';
  const size = ctx.sizeText ? (en ? ' (about ' + ctx.sizeText + ')' : ' (circa ' + ctx.sizeText + ')') : '';
  switch (kind) {
    case 'no-webgpu':
      return { fatal: true, canRetry: false,
        title: en ? 'This model needs WebGPU' : 'Questo modello richiede WebGPU',
        detail: en
          ? 'Your browser or GPU doesn’t expose WebGPU, so this GPU model can’t run here. Use the CPU model instead, or open NucleoOS in Chrome/Edge with hardware acceleration on.'
          : 'Il tuo browser o la GPU non espongono WebGPU, quindi questo modello GPU non può girare qui. Usa il modello CPU, oppure apri NucleoOS in Chrome/Edge con l’accelerazione hardware attiva.' };
    case 'no-wasm':
      return { fatal: true, canRetry: false,
        title: en ? 'WebAssembly unavailable' : 'WebAssembly non disponibile',
        detail: en ? 'This browser can’t run WebAssembly, which the CPU model needs.' : 'Questo browser non può eseguire WebAssembly, necessario al modello CPU.' };
    case 'integrity':
      return { fatal: true, canRetry: true,
        title: en ? 'Downloaded data was corrupt' : 'I dati scaricati sono corrotti',
        detail: en
          ? 'A checksum (SHA-256) didn’t match, so the bytes are damaged and were discarded. If you pulled from the Cardputer SD, re-sync the SD card; if from the internet, just retry.'
          : 'Un checksum (SHA-256) non corrisponde: i byte sono danneggiati e sono stati scartati. Se hai scaricato dalla SD del Cardputer, ri-sincronizza la SD; se da internet, riprova.' };
    case 'notfound':
      return { fatal: true, canRetry: true,
        title: en ? 'Model not available at the source' : 'Modello non disponibile alla sorgente',
        detail: en
          ? 'The weights weren’t found. Online: try again later. Offline: this model hasn’t been copied onto the Cardputer SD yet — connect to the internet once to fetch it, or stage it on the SD.'
          : 'I pesi non sono stati trovati. Online: riprova più tardi. Offline: questo modello non è ancora stato copiato sulla SD del Cardputer — collegati a internet una volta per scaricarlo, oppure mettilo sulla SD.' };
    case 'cache':
      return { fatal: true, canRetry: true,
        title: en ? 'Out of browser storage' : 'Spazio del browser esaurito',
        detail: en
          ? 'The browser couldn’t store the model' + size + '. Free space (remove other cached models or site data) and retry.'
          : 'Il browser non è riuscito a salvare il modello' + size + '. Libera spazio (rimuovi altri modelli in cache o i dati del sito) e riprova.' };
    case 'busy':
      return { fatal: true, canRetry: true,
        title: en ? 'Another download is running' : 'Un altro download è in corso',
        detail: en ? 'Only one download runs at a time on the Cardputer. Wait for it to finish, then retry.' : 'Sul Cardputer si scarica una cosa alla volta. Attendi che finisca, poi riprova.' };
    case 'transient':
    default:
      return { fatal: false, canRetry: true,
        title: en ? 'Connection lost — resuming' : 'Connessione persa — riprendo',
        detail: en
          ? 'The source (internet or the Cardputer) became unreachable. Keeping the verified parts and retrying automatically; nothing is lost. Cancel anytime.'
          : 'La sorgente (internet o il Cardputer) non è raggiungibile. Tengo le parti già verificate e riprovo da solo; non si perde nulla. Puoi annullare quando vuoi.' };
  }
}

// ---- abortable sleep ---------------------------------------------------------------------------------
// Resolves true if the signal aborted during the wait (so the caller stops), false on a clean timeout.
export function abortableSleep(ms, signal, sleep) {
  const nap = sleep || ((m) => new Promise((r) => setTimeout(r, m)));
  return new Promise((resolve) => {
    if (signal && signal.aborted) return resolve(true);
    let settled = false;
    const finish = (v) => { if (settled) return; settled = true; if (signal) try { signal.removeEventListener('abort', onAbort); } catch {} resolve(v); };
    const onAbort = () => finish(true);
    if (signal) try { signal.addEventListener('abort', onAbort, { once: true }); } catch {}
    nap(ms).then(() => finish(false));
  });
}

// ---- the driver --------------------------------------------------------------------------------------
// installModel(opts) → { ok, source } | { ok:false, reason }
//   store     : the model-store ({ download(id,{signal,onProgress}), status, ... })
//   modelId   : registry id;  kind: 'webgpu'|'wasm';  caps: { webgpu, wasm, online }
//   sizeText  : human total size (for the out-of-space message);  lang: 'it'|'en'
//   ui        : the modal surface (see install-modal.js) implementing:
//                 label (string), onCancel(cb), setPhase(name, info?), onProgress(p),
//                 setReconnecting({attempt,delayMs,msg}), setError({title,detail,canRetry}),
//                 setCancelled(), setDone(result)
//   dlLock    : optional withDownloadLock(label, fn) — the OS-wide one-at-a-time gate
//   sleep     : injectable timer (host tests)
export async function installModel(opts) {
  const { store, modelId, kind, caps = {}, sizeText, lang = 'it', ui, dlLock, sleep, controller } = opts;

  const pre = prereqFor(kind, caps);
  if (!pre.ok) { ui.setError(messageFor(pre.reason, { sizeText, kind }, lang)); return { ok: false, reason: pre.reason }; }

  // One AbortController for the whole run; the modal's Cancel (and the OS scrim's) abort it.
  const ac = controller || new AbortController();
  let cancelled = false;
  ui.onCancel(() => { cancelled = true; try { ac.abort(); } catch {} });

  const run = async () => {
    let attempt = 0;
    for (;;) {
      if (cancelled || ac.signal.aborted) { ui.setCancelled(); return { ok: false, reason: 'cancelled' }; }
      ui.setPhase(attempt === 0 ? 'starting' : 'resuming');
      let r;
      // The transient/fatal taxonomy belongs to download I/O ONLY — keep the try around store.download
      // alone, so a defect in a UI method (setPhase/setDone) surfaces normally instead of being mistaken
      // for a dropped connection and retried forever.
      try {
        r = await store.download(modelId, { signal: ac.signal, onProgress: (p) => ui.onProgress(p) });
      } catch (e) {
        const ek = (e && e.kind) || 'transient';
        if (ek === 'cancelled') { ui.setCancelled(); return { ok: false, reason: 'cancelled' }; }
        if (ek === 'transient') {                                   // AUTO-RESUME: wait, then re-run (cache skips done shards)
          attempt++;
          const delayMs = backoffMs(attempt);
          ui.setReconnecting({ attempt, delayMs, msg: messageFor('transient', { sizeText, kind }, lang) });
          const aborted = await abortableSleep(delayMs, ac.signal, sleep);
          if (aborted) { ui.setCancelled(); return { ok: false, reason: 'cancelled' }; }
          continue;
        }
        ui.setError(messageFor(ek, { sizeText, kind }, lang));      // fatal: integrity / notfound / cache / busy
        return { ok: false, reason: ek };
      }
      ui.setDone(r);
      return { ok: true, source: r.source };
    }
  };

  return dlLock ? dlLock(ui.label, run) : run();
}
