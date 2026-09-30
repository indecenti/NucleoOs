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
// Can the engine this model targets actually RUN in this browser, and can the browser STORE it? Downloading a
// model you can't run or keep is a dead end, so we block up-front. caps: { webgpu, wasm, online, cacheApi }.
//   reason ∈ null | 'insecure' | 'no-webgpu' | 'no-wasm'
// cacheApi:false = the page is plain http (http://<device-ip>): the Cache API the weights live in does not
// exist there. That used to throw a ReferenceError mid-download which was retried FOREVER as a "dropped
// connection" inside the OS-wide blocking modal.
export function prereqFor(kind, caps = {}) {
  if (caps.cacheApi === false) return { ok: false, reason: 'insecure' };
  if (kind === 'webgpu' && !caps.webgpu) return { ok: false, reason: 'no-webgpu' };
  if (kind === 'wasm' && !caps.wasm) return { ok: false, reason: 'no-wasm' };
  return { ok: true, reason: null };
}

// Errors WITHOUT a download-error kind: only a real network failure is worth auto-resuming. Anything else
// (a ReferenceError, a bug, a quota exception) is shown — never looped on.
export function classifyError(e) {
  if (e && e.kind) return e.kind;
  const name = String((e && e.name) || ''), msg = String((e && e.message) || e || '');
  if (name === 'TimeoutError' || (name === 'AbortError' && /timeout|timed out/i.test(msg))) return 'transient';
  if (name === 'TypeError' && /fetch|network|load failed|ERR_|connection/i.test(msg)) return 'transient';
  if (name === 'QuotaExceededError' || /quota/i.test(msg)) return 'cache';
  if (/caches|Cache|crypto|subtle|SecurityError/.test(name + ' ' + msg) && /not defined|undefined|SecurityError|insecure/i.test(name + ' ' + msg)) return 'insecure';
  return 'unknown';
}

// ---- error / status → user-facing copy (pure, it/en/es/fr/de) -------------------------------------------
// One plain message per failure CLASS. `ctx`: { sizeText, kind, error }. Returns { title, detail, fatal,
// canRetry }. `fatal` = "this won't fix itself"; non-fatal classes auto-resume and are never a dead end.
const MSG = {
  insecure: {
    it: ['Questa pagina non può salvare modelli', 'La pagina è in http: il browser disattiva l’archivio (Cache) in cui vivono i modelli. Apri “Cosa può fare questo browser” e segui i passi per il tuo browser, poi riprova.'],
    en: ['This page can’t store models', 'The page is plain http: the browser disables the storage (Cache) models live in. Open “What this browser can do” and follow the steps for your browser, then retry.'],
    es: ['Esta página no puede guardar modelos', 'La página es http: el navegador desactiva el almacenamiento (Cache) donde viven los modelos. Abre «Qué puede hacer este navegador» y sigue los pasos para tu navegador; luego reintenta.'],
    fr: ['Cette page ne peut pas stocker de modèles', 'La page est en http : le navigateur désactive le stockage (Cache) des modèles. Ouvrez « Ce que ce navigateur peut faire », suivez les étapes pour votre navigateur, puis réessayez.'],
    de: ['Diese Seite kann keine Modelle speichern', 'Die Seite ist nur http: Der Browser deaktiviert den Speicher (Cache), in dem Modelle liegen. Öffne „Was dieser Browser kann“, folge den Schritten für deinen Browser und versuche es erneut.'] },
  'no-webgpu': {
    it: ['Questo modello richiede WebGPU', 'Il browser o la GPU non espongono WebGPU, quindi questo modello GPU qui non può girare. Usa il modello CPU, oppure apri “Cosa può fare questo browser” per sapere come attivarla.'],
    en: ['This model needs WebGPU', 'Your browser or GPU doesn’t expose WebGPU, so this GPU model can’t run here. Use the CPU model, or open “What this browser can do” to see how to turn it on.'],
    es: ['Este modelo necesita WebGPU', 'El navegador o la GPU no ofrecen WebGPU, así que este modelo GPU no puede funcionar aquí. Usa el modelo CPU o abre «Qué puede hacer este navegador» para ver cómo activarlo.'],
    fr: ['Ce modèle nécessite WebGPU', 'Le navigateur ou le GPU n’exposent pas WebGPU : ce modèle GPU ne peut pas tourner ici. Utilisez le modèle CPU, ou ouvrez « Ce que ce navigateur peut faire » pour l’activer.'],
    de: ['Dieses Modell braucht WebGPU', 'Browser oder GPU bieten kein WebGPU, daher läuft dieses GPU-Modell hier nicht. Nutze das CPU-Modell oder öffne „Was dieser Browser kann“, um zu sehen, wie du es aktivierst.'] },
  'no-wasm': {
    it: ['WebAssembly non disponibile', 'Questo browser non può eseguire WebAssembly, necessario al modello CPU.'],
    en: ['WebAssembly unavailable', 'This browser can’t run WebAssembly, which the CPU model needs.'],
    es: ['WebAssembly no disponible', 'Este navegador no puede ejecutar WebAssembly, necesario para el modelo CPU.'],
    fr: ['WebAssembly indisponible', 'Ce navigateur ne peut pas exécuter WebAssembly, nécessaire au modèle CPU.'],
    de: ['WebAssembly nicht verfügbar', 'Dieser Browser kann kein WebAssembly ausführen, das das CPU-Modell braucht.'] },
  integrity: {
    it: ['I dati scaricati sono corrotti', 'Un checksum (SHA-256) non corrisponde: i byte danneggiati sono stati scartati. Se hai scaricato dalla SD del Cardputer, ri-sincronizza la SD; se da internet, riprova.'],
    en: ['Downloaded data was corrupt', 'A checksum (SHA-256) didn’t match, so the damaged bytes were discarded. If you pulled from the Cardputer SD, re-sync the SD card; if from the internet, just retry.'],
    es: ['Los datos descargados están dañados', 'Un checksum (SHA-256) no coincide: los bytes dañados se descartaron. Si descargaste de la SD del Cardputer, vuelve a sincronizarla; si fue de internet, reintenta.'],
    fr: ['Les données téléchargées sont corrompues', 'Une somme de contrôle (SHA-256) ne correspond pas : les octets abîmés ont été écartés. Depuis la SD du Cardputer, resynchronisez la SD ; depuis internet, réessayez.'],
    de: ['Die heruntergeladenen Daten sind beschädigt', 'Eine Prüfsumme (SHA-256) stimmte nicht, die beschädigten Bytes wurden verworfen. Von der Cardputer-SD: SD neu synchronisieren; aus dem Internet: erneut versuchen.'] },
  notfound: {
    it: ['Modello non disponibile alla sorgente', 'I pesi non sono stati trovati. Online: riprova più tardi. Offline: il modello non è sulla SD del Cardputer — collegati a internet una volta per scaricarlo.'],
    en: ['Model not available at the source', 'The weights weren’t found. Online: try again later. Offline: this model isn’t on the Cardputer SD — connect to the internet once to fetch it.'],
    es: ['Modelo no disponible en el origen', 'No se encontraron los pesos. Con conexión: reintenta más tarde. Sin conexión: el modelo no está en la SD del Cardputer; conéctate a internet una vez para descargarlo.'],
    fr: ['Modèle indisponible à la source', 'Les poids sont introuvables. En ligne : réessayez plus tard. Hors ligne : ce modèle n’est pas sur la SD du Cardputer — connectez-vous une fois à internet pour le récupérer.'],
    de: ['Modell an der Quelle nicht verfügbar', 'Die Gewichte wurden nicht gefunden. Online: später erneut versuchen. Offline: Das Modell ist nicht auf der Cardputer-SD – verbinde dich einmal mit dem Internet, um es zu laden.'] },
  cache: {
    it: ['Spazio del browser esaurito', 'Il browser non è riuscito a salvare il modello{size}. Libera spazio (rimuovi altri modelli o i dati del sito) e riprova.'],
    en: ['Out of browser storage', 'The browser couldn’t store the model{size}. Free space (remove other cached models or site data) and retry.'],
    es: ['Sin espacio en el navegador', 'El navegador no pudo guardar el modelo{size}. Libera espacio (quita otros modelos o datos del sitio) y reintenta.'],
    fr: ['Stockage du navigateur plein', 'Le navigateur n’a pas pu enregistrer le modèle{size}. Libérez de l’espace (autres modèles ou données du site) et réessayez.'],
    de: ['Browserspeicher voll', 'Der Browser konnte das Modell{size} nicht speichern. Gib Speicher frei (andere Modelle oder Websitedaten entfernen) und versuche es erneut.'] },
  busy: {
    it: ['Un altro download è in corso', 'Si scarica una cosa alla volta per non caricare il Cardputer. Attendi che finisca, poi riprova.'],
    en: ['Another download is running', 'One download runs at a time so the Cardputer is never overloaded. Wait for it to finish, then retry.'],
    es: ['Hay otra descarga en curso', 'Se descarga una cosa a la vez para no sobrecargar el Cardputer. Espera a que termine y reintenta.'],
    fr: ['Un autre téléchargement est en cours', 'Un seul téléchargement à la fois pour ne pas surcharger le Cardputer. Attendez la fin, puis réessayez.'],
    de: ['Ein anderer Download läuft', 'Es wird immer nur eine Sache geladen, damit der Cardputer nie überlastet wird. Warte, bis er fertig ist, und versuche es erneut.'] },
  unknown: {
    it: ['Installazione non riuscita', 'Errore inatteso: {error}. Nessun dato è stato perso; puoi riprovare.'],
    en: ['Install failed', 'Unexpected error: {error}. Nothing was lost; you can retry.'],
    es: ['La instalación falló', 'Error inesperado: {error}. No se perdió nada; puedes reintentar.'],
    fr: ['Échec de l’installation', 'Erreur inattendue : {error}. Rien n’a été perdu ; vous pouvez réessayer.'],
    de: ['Installation fehlgeschlagen', 'Unerwarteter Fehler: {error}. Es ging nichts verloren; du kannst es erneut versuchen.'] },
  transient: {
    it: ['Connessione persa — riprendo', 'La sorgente (internet o il Cardputer) non è raggiungibile. Tengo le parti già verificate e riprovo da solo; non si perde nulla. Puoi annullare quando vuoi.'],
    en: ['Connection lost — resuming', 'The source (internet or the Cardputer) became unreachable. Keeping the verified parts and retrying automatically; nothing is lost. Cancel anytime.'],
    es: ['Conexión perdida — reanudando', 'El origen (internet o el Cardputer) no responde. Conservo las partes verificadas y reintento solo; no se pierde nada. Puedes cancelar cuando quieras.'],
    fr: ['Connexion perdue — reprise', 'La source (internet ou le Cardputer) est injoignable. Je garde les parties vérifiées et je réessaie seul ; rien n’est perdu. Annulez quand vous voulez.'],
    de: ['Verbindung verloren – setze fort', 'Die Quelle (Internet oder Cardputer) ist nicht erreichbar. Ich behalte die geprüften Teile und versuche es automatisch erneut; nichts geht verloren. Jederzeit abbrechen.'] },
};
const FLAGS = { insecure: [true, false], 'no-webgpu': [true, false], 'no-wasm': [true, false], integrity: [true, true], notfound: [true, true], cache: [true, true], busy: [true, true], unknown: [true, true], transient: [false, true] };
const SIZE = { it: ' (circa {s})', en: ' (about {s})', es: ' (unos {s})', fr: ' (environ {s})', de: ' (etwa {s})' };
export function messageFor(kind, ctx = {}, lang = 'it') {
  const k = MSG[kind] ? kind : 'transient';
  const L = MSG[k][lang] ? lang : 'en';
  const size = ctx.sizeText ? SIZE[L].replace('{s}', ctx.sizeText) : '';
  const err = String((ctx.error && (ctx.error.message || ctx.error)) || '?').slice(0, 160);
  const [title, detail] = MSG[k][L];
  const [fatal, canRetry] = FLAGS[k];
  return { fatal, canRetry, title, detail: detail.replace('{size}', size).replace('{error}', err) };
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
        const ek = classifyError(e);   // only a REAL network drop auto-resumes; anything else is shown
        if (ek === 'cancelled') { ui.setCancelled(); return { ok: false, reason: 'cancelled' }; }
        if (ek === 'transient') {                                   // AUTO-RESUME: wait, then re-run (cache skips done shards)
          attempt++;
          const delayMs = backoffMs(attempt);
          ui.setReconnecting({ attempt, delayMs, msg: messageFor('transient', { sizeText, kind }, lang) });
          const aborted = await abortableSleep(delayMs, ac.signal, sleep);
          if (aborted) { ui.setCancelled(); return { ok: false, reason: 'cancelled' }; }
          continue;
        }
        ui.setError(messageFor(ek, { sizeText, kind, error: e }, lang));   // fatal: insecure / integrity / notfound / cache / busy / unknown
        return { ok: false, reason: ek };
      }
      ui.setDone(r);
      return { ok: true, source: r.source };
    }
  };

  return dlLock ? dlLock(ui.label, run) : run();
}
