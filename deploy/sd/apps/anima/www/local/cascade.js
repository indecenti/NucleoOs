// ANIMA — offline-resolution policy (pure, side-effect-free, import-free).
//
// This module owns the ONE decision the cascade cares about: when an answer must come from an
// offline brain, do we ask the in-browser WASM engine or the Cardputer first, and which reply
// counts as "answered"? It is deliberately DOM-free, fetch-free and import-free so it can be
// unit-tested in Node (apps/anima/local/cascade.test.mjs) with mocked runners — the browser
// (index.html) injects the real runners. Keeping the policy here means the guarantee the user
// asked for — "the browser WASM brain is tried before we scale onto the Cardputer" — is provable
// in isolation and can't silently regress when ask()'s many tiers move around.
//
// INVARIANT: the device is always reachable. The browser WASM is purely ADDITIVE — when it is not
// provisioned, throws, or abstains, resolveOffline falls through to the device (or returns null so
// the caller's honest-miss tail runs). Nothing here generates text; it only routes + filters.

// answered(r): does this shaped result actually carry a usable answer?
//
// Tier-AGNOSTIC on purpose. The engine.js shaper maps the C tier enum to strings but historically
// drops STITCH/L2 (tier 3 -> 'none') even when the reply is correct and non-empty, so gating on
// `tier !== 'none'` would silently discard good L2 answers (and the old online last-resort guard at
// index.html did exactly that). We gate on the reply plus a real action/domain instead, so a
// stitched descriptive answer (tier 'none' but domain 'knowledge') still counts, while a true
// abstention (empty reply, no action, no domain) correctly does not.
export function answered(r) {
  if (!r || !r.reply) return false;            // no text -> abstention
  if (r.tier && r.tier !== 'none') return true; // command / fact / remote
  if (r.action && r.action !== 'none') return true; // launch / system / answer / tool
  return !!(r.domain && r.domain !== 'none');  // stitch / L1-as-domain still answered
}

// resolveOffline(q, lang, opts, runners): try the browser's own brains and the device, in the preferred
// order, and return the first real answer, or null if none has one. Never throws (a throwing runner is
// treated as an abstention so the next source still gets its turn).
//
// THREE tiers, in this fixed relative order (the "browser exhausts itself before the Cardputer"):
//   browser   — the in-browser WASM offline cascade (grounded, instant, MCU-sparing).
//   webindex  — the in-browser web indexer: when the WASM brain abstains on a knowledge question it
//               fetches LIVE from Wikipedia/Wikidata DIRECTLY from the browser (or serves a cached card),
//               never through the Cardputer. Optional — omit the runner and this tier is simply skipped.
//   device    — the Cardputer's own offline cascade (/api/anima). The always-present safety net.
//
//   opts.prefer  'browser' (default) -> browser, webindex, device.
//                'device'            -> device, browser, webindex  (device chosen; browser as resilience).
//   opts.silent  true (default)  -> mid-chat fallback: the browser WASM is consulted ONLY if already
//                                   provisioned (runners.browserProvisioned() true), so we never trigger a
//                                   surprise ~88 MB pack download mid-turn; unprovisioned -> skip that tier.
//                                   (webindex is light — a few small API calls — so it is NOT silent-gated.)
//                false            -> explicit Browser mode: allow runners.browser's blocking provisioning gate.
//
//   runners.browser(q, lang)        async -> shaped result | null   (in-browser WASM cascade)
//   runners.webindex(q, lang)       async -> shaped result | null   (in-browser web indexer; optional)
//   runners.device(q, lang)         async -> shaped result | null   (/api/anima offline)
//   runners.browserProvisioned()    async -> boolean  (loaded || pack cached; MUST NOT download)
export async function resolveOffline(q, lang, opts = {}, runners = {}) {
  const prefer = opts.prefer === 'device' ? 'device' : 'browser';
  const silent = opts.silent !== false;        // default true
  const order = prefer === 'device' ? ['device', 'browser', 'webindex'] : ['browser', 'webindex', 'device'];
  for (const src of order) {
    // Silent fallback never pulls the browser PACK: if the WASM brain isn't already here, skip it (the
    // web index and device don't need provisioning, so they are never gated this way).
    if (src === 'browser' && silent) {
      let ok = false;
      try { ok = await runners.browserProvisioned(); } catch { ok = false; }
      if (!ok) continue;
    }
    const run = runners[src];
    if (typeof run !== 'function') continue;
    let r = null;
    try { r = await run(q, lang); } catch { r = null; }
    if (answered(r)) return r;
  }
  return null;
}

// ---- commands first: device actions and live state -------------------------------------------------
// The engines only PROPOSE a device action (add_event / set_volume / set_brightness / create_file); only
// the Cardputer can perform it, through /api/anima (which executes the tool server-side under the pairing
// gate). Two rules follow, both pinned by cascade.test.mjs:
//   1. such an utterance goes to the device BEFORE the cloud agent / browser LLM, which can't do it and
//      might claim they did (and would guess "che ore sono" instead of reading the RTC);
//   2. the UI calls an action done only when the device says it ran it.
export const DEVICE_TOOLS = new Set(['add_event', 'set_volume', 'set_brightness', 'create_file']);

// classifyCommand(r): what a shaped engine result asks its host to do.
//   'device' - a side-effecting tool the Cardputer must execute (DEVICE_TOOLS)
//   'live'   - a live-state answer (time, date, storage, network, agenda...): the device is the authority
//   'client' - launch an app / open a file: the browser performs the hand-off itself
//   null     - not a command (knowledge, chat, calc, abstention)
export function classifyCommand(r) {
  if (!r || typeof r !== 'object') return null;
  if (r.action === 'tool') {
    const tool = r.tool || r.intent || '';
    if (DEVICE_TOOLS.has(tool)) return 'device';
    if (tool === 'open_file' && r.arg) return 'client';
    return null;
  }
  if (r.action === 'system') return 'live';
  if (r.action === 'launch' && r.arg) return 'client';
  return null;
}

// deviceToolOutcome(r) for a DEVICE_TOOLS result:
//   'proposed'    - from the in-browser engine (r.local): nothing ran on the device
//   'done'        - the device executed it
//   'failed'      - the device refused or failed (pairing / file exists / write error); its reply says why
//   'unsupported' - firmware older than the "done" flag, which never executed set_volume/set_brightness
export function deviceToolOutcome(r) {
  if (!r || r.local) return 'proposed';
  if (r.done === true) return 'done';
  if (r.done === false) return 'failed';
  const tool = r.tool || r.intent || '';
  if (tool === 'set_volume' || tool === 'set_brightness') return 'unsupported';
  if (/associat|pairing|\bpin\b|non sono riuscit|non riesco|couldn'?t|can'?t|esiste|exists/i.test(r.reply || '')) return 'failed';
  if (tool === 'create_file') return r.path ? 'done' : 'failed';
  return 'done';   // add_event: legacy firmware wrote it server-side and replied with the confirmation
}

// actRequest(r, lang): the POST /api/anima/act body that asks the DEVICE to carry out an action the browser's
// engine decided, or null if r is not a device action. For a device that answers /api/anima "busy" (a heap
// too fragmented for the cascade's 30 KB worker, e.g. the ADV with the web OS on): the WASM engine is the
// same cascade, so the decision is already made — only the Cardputer can make it happen. The device
// validates every field again; this just carries the engine's own tool/arg/content/reply.
export function actRequest(r, lang) {
  if (classifyCommand(r) !== 'device') return null;
  return { tool: r.tool || r.intent, arg: String(r.arg || ''), content: String(r.content || ''),
           reply: String(r.reply || ''), lang: lang === 'en' ? 'en' : 'it' };
}

// commandHint(q): a cheap lexical gate, NOT the classifier. It only decides whether an utterance is worth
// asking the real classifier (the in-browser engine, else the device) BEFORE the cloud/LLM rungs, so
// ordinary questions never pay a device round-trip. Italian + English, like the engine.
//   'act'    - asks the device to change something (setting, reminder, event, timer)
//   'live'   - asks for device state (time, date, battery, storage, RAM, network, version, agenda)
//   'launch' - opens an app
// File creation is deliberately NOT here: with a key the agent writes real files into the workspace.
const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[\u2018\u2019`]/g, "'");
// ROUTING vocabulary. The deterministic engine answers in Italian and English; these lists only decide that an
// utterance is a device command / live question, so they also carry Spanish, French and German — otherwise
// "sube el volumen" or "öffne den Rechner" reached a tool-less chat model that could claim it was done.
const SETTING_NOUN = /\b(volume|audio|suono|luminosita|brightness|schermo|screen|display|retroilluminazione|backlight|volumen|brillo|pantalla|sonido|luminosite|ecran|lautstarke|helligkeit|bildschirm)\b/;
const SETTING_VERB = /\b(alza|abbassa|aumenta|diminuisci|riduci|imposta|metti|porta|regola|cambia|setta|modifica|muta|silenzia|azzera|raise|lower|increase|decrease|set|turn|mute|unmute|dim|brighten|change|adjust|fai|rendi|make|sube|baja|reduce|pon|ajusta|silencia|monte|baisse|augmente|diminue|regle|mets|coupe|erhohe|verringere|stelle|mach|dreh|schalte|reduziere)\b/;
const SETTING_AMOUNT = /\b(piu|meno|more|less|max|massimo|massima|minimo|minima|meta|half|zero|muto|alto|alta|basso|bassa|up|down|mas|menos|plus|moins|mehr|weniger|maximo|lauter|leiser|heller|dunkler)\b|\d/;
const GEOMETRY = /\b(cubo|sfera|cilindro|cono|piramide|prisma|lato|raggio|altezza|diametro|densita|massa|litri|cube|sphere|cylinder|cone|pyramid|prism|side|radius|height|diameter|density|mass|liters|litres|vendite|sales)\b/;
const REMIND = /\b(ricordami|ricordamelo|ricordatemi|promemoria|remind me|reminder|recuerdame|recordatorio|rappelle-moi|rappelle moi|rappel|erinnere mich|erinnerung)\b/;
const EVENT_VERB = /\b(aggiungi|crea|segna|metti|fissa|programma|pianifica|prenota|inserisci|add|create|schedule|book|put|new|nuovo|nuova|anade|agrega|apunta|ajoute|cree|planifie|fuge|erstelle|trage|plane)\b/;
const EVENT_NOUN = /\b(evento|eventi|appuntamento|appuntamenti|impegno|riunione|incontro|event|appointment|meeting|cita|reunion|evenement|rendez-vous|termin|besprechung|treffen)\b|\b(in|nel|al|to|on|en|a|dans|au|im|in den) (my |mio |il |nel |mi |el |mon |le |meinen |den )?(calendario|calendar|agenda|calendrier|kalender)\b/;
const TIMER = /\b(timer|sveglia|alarm|temporizador|alarma|minuteur|reveil|wecker)\b/;
const TIMER_CUE = /\b(metti|imposta|avvia|punta|fai partire|set|start|pon|programa|mets|lance|stelle|starte)\b|\b\d+\s*(s|sec|secondi|seconds|min|minuti|minutes|h|ore|hours|segundos|minutos|horas|secondes|heures|sekunden|minuten|stunden)\b|\b(alle|at|a las|a|um)\s+\d/;
// Live state is matched as the WHOLE utterance (plus polite fillers), never as a fragment: "che versione di
// python devo usare" or "che giorno è natale" are questions for a brain, not for the RTC, and must not be
// answered by the device ahead of the cloud.
const LEAD = String.raw`(?:(?:ehi |hey |ciao |hola |salut |hallo )?anima,? )?(?:(?:mi )?(?:dici|sai dirmi|puoi dirmi) |dimmi |(?:can|could) you tell me |tell me |please |dime |dis-moi |dis moi |sag mir |bitte )?`;
const TAIL = String.raw` ?(?:adesso|ora|oggi|now|today|please|per favore|grazie|ahora|hoy|por favor|maintenant|aujourd'hui|s'il te plait|jetzt|heute|bitte)?`;
// Spanish / French / German forms of each LIVE entry below (same order), already accent-folded.
const LIVE_XL = [
  String.raw`que hora es|quelle heure (?:est-il|il est|est il)|il est quelle heure|wie spat ist es|wie ?viel uhr ist es`,
  String.raw`que dia es(?: hoy)?|a que (?:dia|fecha) estamos|que fecha es(?: hoy)?|quel jour (?:sommes-nous|sommes nous|on est|est-on)|quelle (?:est la )?date(?: aujourd'hui)?|welcher tag ist(?: heute)?|welches datum (?:ist|haben wir)(?: heute)?|der wievielte ist heute`,
  String.raw`en que ano estamos|que ano es|en quelle annee (?:sommes-nous|sommes nous|on est)|welches jahr (?:ist|haben wir)|en que estacion estamos|quelle saison(?: sommes-nous| est-ce)?|welche jahreszeit(?: ist| haben wir)?`,
  String.raw`(?:cuanta |nivel de )?bateria(?: me queda| queda| tengo)?|(?:niveau de )?batterie(?: restante)?|combien de batterie(?: il me reste)?|(?:wie viel )?akku(?:stand)?|batteriestand`,
  String.raw`cuanto espacio (?:libre )?(?:tengo|queda|hay)(?: en la sd)?|espacio (?:libre|disponible)(?: en la sd)?|combien d'espace (?:libre )?(?:il me reste|reste|ai-je|j'ai)?(?: sur la sd)?|espace (?:libre|disponible)(?: sur la sd)?|wie viel (?:freier )?(?:speicher|speicherplatz|platz)(?: habe ich| ist frei| ist noch frei)?(?: auf der sd)?|freier speicher(?:platz)?`,
  String.raw`cuanta (?:ram|memoria)(?: libre)?(?: tengo)?|memoria libre|combien de (?:ram|memoire)(?: libre)?|memoire libre|wie viel (?:ram|arbeitsspeicher)(?: ist frei)?|freier arbeitsspeicher`,
  String.raw`cuanto tiempo llevas encendid[oa]|depuis combien de temps es-tu allume|wie lange laufst du schon`,
  String.raw`que version (?:eres|tienes|de nucleoos|del firmware)|version del firmware|quelle version(?: de nucleoos| du firmware| es-tu)?|welche version(?: von nucleoos| hast du| ist das)?|firmware-version`,
  String.raw`a que (?:red|wifi) estoy conectado|que (?:red|wifi)(?: es| uso)?|estoy conectado(?: a internet)?|mi (?:direccion )?ip|a quel (?:reseau|wifi) suis-je connecte|quel (?:reseau|wifi)(?: est-ce)?|suis-je connecte(?: a internet)?|mon adresse ip|mit welchem (?:wlan|netz|wifi) bin ich verbunden|welches (?:wlan|netz|wifi)|bin ich verbunden|meine ip(?:-adresse)?`,
  String.raw`que citas tengo(?: hoy| manana)?|(?:mi|la) agenda(?: de hoy)?|qu'est-ce que j'ai (?:aujourd'hui|demain)(?: a l'agenda)?|mon agenda|mes rendez-vous(?: d'aujourd'hui)?|welche termine habe ich(?: heute| morgen)?|meine termine(?: heute)?|mein kalender`,
];
const LIVE = [
  String.raw`che or[ae] (?:e|sono)|che ora e|l'ora|ora esatta|what time is it|what'?s the time|the time|current time`,
  String.raw`che giorno (?:e|siamo)(?: oggi)?|oggi che giorno e|che data e(?: oggi)?|(?:la )?data(?: di oggi)?|what day is (?:it|today)|what'?s the date|what is the date|today'?s date|the date`,
  String.raw`che anno (?:e|siamo)|in che anno siamo|what year is it|che stagione e|in che stagione siamo|what season is it`,
  String.raw`(?:quanta|livello(?: della)?|stato(?: della)?|carica(?: della)?) batteria(?: ho| hai| c'e| rimane| resta)?|batteria|battery(?: level| left| status)?|how much battery(?: is left| do i have| left)?`,
  String.raw`quanto spazio (?:libero |rimasto )?(?:ho|hai|c'e|resta|rimane)(?: sulla sd| su sd)?|spazio (?:libero|rimasto|disponibile|su sd|sulla sd)|(?:free|disk|sd) space|how much (?:free )?space(?: is left| do i have| left)?|storage left`,
  String.raw`quanta (?:ram|memoria)(?: libera)?(?: ho| hai| c'e)?|(?:ram|memoria) (?:libera|disponibile)|free (?:ram|memory)|how much (?:ram|memory)(?: is free| do you have| left)?`,
  String.raw`uptime|da quanto (?:tempo )?(?:sei|e) acces[oa](?: il (?:cardputer|dispositivo|device))?|how long (?:have you been|has the (?:cardputer|device) been) (?:on|up|running)`,
  String.raw`(?:che|quale) versione (?:sei|hai|e|di nucleoos|del firmware|del sistema)|versione(?: del)? firmware|firmware version|what version (?:are you|is this|of nucleoos)`,
  String.raw`(?:a che|a quale) (?:rete|wi-?fi) sono connesso|(?:che|quale) (?:rete|wi-?fi)(?: e| uso| stai usando)?|sono connesso(?: a internet)?|am i connected|(?:which|what) (?:network|wi-?fi)(?: am i on| is this)?|(?:qual e )?(?:il mio )?indirizzo ip|(?:what'?s )?my ip(?: address)?|ip address`,
  String.raw`(?:che|quali) (?:impegni|appuntamenti) ho(?: oggi| domani)?|i miei impegni|impegni(?: di)? oggi|cosa ho (?:in agenda|oggi|domani)|agenda(?: di)? oggi|(?:what'?s|what is) on (?:today|my calendar)|my (?:schedule|agenda|appointments)(?: today)?`,
].map((re, i) => new RegExp('^' + LEAD + '(?:' + re + (LIVE_XL[i] ? '|' + LIVE_XL[i] : '') + ')' + TAIL + '$'));
// "how do I raise the volume on my PC?" is a how-to, not an order: interrogative openers never trigger 'act'.
const HOWTO = /^(come|perche|quando|dove|chi|cosa|che cosa|quale|how|why|when|where|who|what|which|can i|posso|como|por que|cuando|donde|quien|puedo|comment|pourquoi|quand|ou|qui|est-ce que|puis-je|wie|warum|wann|wo|wer|kann ich)\b/;
const LAUNCH = /^(apri|avvia|lancia|open|launch|abre|abrir|inicia|ouvre|ouvrir|lance|lancer|offne|offnen|starte)\s+(?!source\b)\S/;

// The language a short question is written in (it/en/es/fr/de), or null when it does not show — for the
// answers composed HERE without a model (liveFromStatus): a German question on an Italian desktop gets German.
// Cheap and deliberately conservative: distinctive function words + a few letters only one language uses.
const LANG_CUES = {
  it: /\b(che|quanto|quanta|quanti|sono|ho|hai|della|sulla|oggi|adesso|mi|dimmi|batteria|spazio|ore)\b/g,
  en: /\b(what|how|much|many|is|the|my|do|have|left|time|today|battery|space|which|am)\b/g,
  es: /\b(que|cuanto|cuanta|tengo|queda|hay|estoy|hoy|es|mi|la|el|bateria|espacio|hora|dime)\b/g,
  fr: /\b(quel|quelle|combien|est|il|reste|suis|aujourd'hui|mon|ma|de|batterie|espace|heure|sommes)\b/g,
  de: /\b(wie|viel|ist|es|habe|ich|bin|welche|welches|heute|mein|meine|akku|speicherplatz|spat|uhr|haben|wir)\b/g,
};
export function guessLang(q) {
  const raw = String(q || '').toLowerCase();
  if (/[¿¡ñ]/.test(raw)) return 'es';
  if (/[äöüß]/.test(raw)) return 'de';
  const t = fold(q).replace(/[?!.,;:¿¡]+/g, ' ');
  let best = null, bestN = 0, second = 0;
  for (const [l, re] of Object.entries(LANG_CUES)) {
    const n = (t.match(re) || []).length;
    if (n > bestN) { second = bestN; bestN = n; best = l; } else if (n > second) second = n;
  }
  return bestN > 0 && bestN > second ? best : null;       // a tie is "unclear": the caller keeps the OS language
}

// Which live value a 'live' question asks for — same order as LIVE above.
const LIVE_KINDS = ['time', 'date', 'year', 'battery', 'space', 'ram', 'uptime', 'version', 'network', 'agenda'];
export function liveKind(q) {
  const t = fold(q).replace(/[?!.,;:¿¡]+/g, ' ').replace(/\s+/g, ' ').trim();
  for (let i = 0; i < LIVE.length; i++) if (LIVE[i].test(t)) return LIVE_KINDS[i] === 'year' && /stagion|season|estacion|saison|jahreszeit/.test(t) ? 'season' : LIVE_KINDS[i];
  return null;
}
// The EXACT answer to a live question, from the device's own /api/status — for a Cardputer whose brain is off
// (web mode) or unreachable. A language model handed this question guessed ("1,4 GB free" on a card with 18.7),
// so these values never go through one. null = not answerable from the status (agenda, season): the caller
// hands it on. GB are decimal (1e9), like the desktop's own "SD … GB free".
const LIVE_T = {
  it: { space: 'Sulla SD hai {free} GB liberi su {total} GB.', nosd: 'La scheda SD non è montata.', battery: 'Batteria al {pct}% ({v} V).', nobat: 'Questo Cardputer non riporta il livello della batteria.', ram: 'RAM libera: {kb} KB (il blocco più grande è {blk} KB).', uptime: 'Il Cardputer è acceso da {d}.', version: 'NucleoOS {v}.', net: 'Connesso alla rete Wi-Fi «{ssid}», indirizzo {ip}.', ap: 'Il Cardputer è in modalità access point ({ip}).', nonet: 'Il Cardputer non è connesso a nessuna rete.', time: 'Sono le {t}.', date: 'Oggi è {d}.', year: 'Siamo nel {y}.', h: 'h', m: 'min' },
  en: { space: 'Your SD card has {free} GB free of {total} GB.', nosd: 'The SD card is not mounted.', battery: 'Battery at {pct}% ({v} V).', nobat: 'This Cardputer does not report its battery level.', ram: 'Free RAM: {kb} KB (largest block {blk} KB).', uptime: 'The Cardputer has been on for {d}.', version: 'NucleoOS {v}.', net: 'Connected to the Wi-Fi network “{ssid}”, address {ip}.', ap: 'The Cardputer is in access-point mode ({ip}).', nonet: 'The Cardputer is not connected to any network.', time: 'It is {t}.', date: 'Today is {d}.', year: 'It is {y}.', h: 'h', m: 'min' },
  es: { space: 'Tu SD tiene {free} GB libres de {total} GB.', nosd: 'La tarjeta SD no está montada.', battery: 'Batería al {pct}% ({v} V).', nobat: 'Este Cardputer no informa del nivel de batería.', ram: 'RAM libre: {kb} KB (bloque mayor {blk} KB).', uptime: 'El Cardputer lleva encendido {d}.', version: 'NucleoOS {v}.', net: 'Conectado a la red Wi-Fi «{ssid}», dirección {ip}.', ap: 'El Cardputer está en modo punto de acceso ({ip}).', nonet: 'El Cardputer no está conectado a ninguna red.', time: 'Son las {t}.', date: 'Hoy es {d}.', year: 'Estamos en {y}.', h: 'h', m: 'min' },
  fr: { space: 'Votre carte SD a {free} Go libres sur {total} Go.', nosd: 'La carte SD n’est pas montée.', battery: 'Batterie à {pct} % ({v} V).', nobat: 'Ce Cardputer n’indique pas le niveau de sa batterie.', ram: 'RAM libre : {kb} Ko (plus grand bloc {blk} Ko).', uptime: 'Le Cardputer est allumé depuis {d}.', version: 'NucleoOS {v}.', net: 'Connecté au réseau Wi-Fi « {ssid} », adresse {ip}.', ap: 'Le Cardputer est en mode point d’accès ({ip}).', nonet: 'Le Cardputer n’est connecté à aucun réseau.', time: 'Il est {t}.', date: 'Nous sommes le {d}.', year: 'Nous sommes en {y}.', h: 'h', m: 'min' },
  de: { space: 'Deine SD-Karte hat {free} GB frei von {total} GB.', nosd: 'Die SD-Karte ist nicht eingebunden.', battery: 'Akku bei {pct} % ({v} V).', nobat: 'Dieser Cardputer meldet keinen Akkustand.', ram: 'Freier RAM: {kb} KB (größter Block {blk} KB).', uptime: 'Der Cardputer läuft seit {d}.', version: 'NucleoOS {v}.', net: 'Verbunden mit dem WLAN „{ssid}“, Adresse {ip}.', ap: 'Der Cardputer ist im Access-Point-Modus ({ip}).', nonet: 'Der Cardputer ist mit keinem Netz verbunden.', time: 'Es ist {t}.', date: 'Heute ist {d}.', year: 'Wir haben {y}.', h: 'Std.', m: 'Min.' },
};
const LOCALE = { it: 'it-IT', en: 'en-GB', es: 'es-ES', fr: 'fr-FR', de: 'de-DE' };
export function liveFromStatus(kind, st, lang = 'it', now = new Date()) {
  if (!st || typeof st !== 'object') return null;
  const T = LIVE_T[lang] || LIVE_T.en, loc = LOCALE[lang] || 'en-GB';
  const fill = (s, v) => s.replace(/\{(\w+)\}/g, (m, k) => (v[k] != null ? String(v[k]) : m));
  const num = (x, d = 1) => Number(x).toLocaleString(loc, { minimumFractionDigits: d, maximumFractionDigits: d });
  const net = st.network || {};
  const clock = (typeof net.time === 'number' && net.time > 1672531200) ? new Date(net.time * 1000) : now;   // the device clock is the authority when set
  switch (kind) {
    case 'space': { const s = st.storage; if (!s || !s.mounted || !s.total_bytes) return T.nosd;
      return fill(T.space, { free: num(s.free_bytes / 1e9), total: num(s.total_bytes / 1e9) }); }
    case 'battery': { const b = st.battery; if (!b || typeof b.pct !== 'number') return T.nobat;
      return fill(T.battery, { pct: Math.round(b.pct), v: num((b.mv || 0) / 1000, 2) }); }
    case 'ram': if (typeof st.free_heap !== 'number') return null;
      return fill(T.ram, { kb: Math.round(st.free_heap / 1024), blk: Math.round((st.largest_free_block || 0) / 1024) });
    case 'uptime': { if (typeof st.uptime_s !== 'number') return null; const h = Math.floor(st.uptime_s / 3600), m = Math.floor((st.uptime_s % 3600) / 60);
      return fill(T.uptime, { d: (h ? h + ' ' + T.h + ' ' : '') + m + ' ' + T.m }); }
    case 'version': return st.version ? fill(T.version, { v: st.version }) : null;
    case 'network': if (net.mode === 'sta' && net.ip) return fill(T.net, { ssid: net.ssid || '?', ip: net.ip });
      if (net.mode === 'ap') return fill(T.ap, { ip: net.ip || '192.168.4.1' });
      return T.nonet;
    case 'time': return fill(T.time, { t: clock.toLocaleTimeString(loc, { hour: '2-digit', minute: '2-digit' }) });
    case 'date': return fill(T.date, { d: clock.toLocaleDateString(loc, { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' }) });
    case 'year': return fill(T.year, { y: clock.getFullYear() });
    default: return null;                          // agenda, season: not in the status — hand it on
  }
}

// A question that asks SEVERAL live values at once ("quanto spazio c'è sulla SD e da quanto è acceso?") is not
// one whole-utterance LIVE match: split it on and/e/y/et/und/commas and answer every clause from the status.
// → ['space','uptime'] when every clause is a live question, else null.
export function liveKinds(q) {
  const one = liveKind(q); if (one) return [one];
  const clauses = String(q || '').split(/\s*(?:[,;?]|\s(?:e|ed|and|y|et|und|o)\s)\s*/i).map((c) => c.trim()).filter(Boolean);
  if (clauses.length < 2) return null;
  const kinds = [];
  for (const c of clauses) {
    const k = liveKind(c);                                  // no guessing: every clause must be a live question on its own
    if (!k) return null;
    if (!kinds.includes(k)) kinds.push(k);
  }
  return kinds;
}
// One compact line of the Cardputer's live state for EVERY engine's prompt (contextkit `device`), ~40 tokens.
export function deviceLine(st, lang = 'it') {
  if (!st || typeof st !== 'object') return '';
  const it = lang === 'it', parts = [];
  const s = st.storage;
  if (s && s.mounted && s.total_bytes) parts.push((it ? 'SD ' : 'SD ') + (s.free_bytes / 1e9).toFixed(1) + (it ? ' GB liberi su ' : ' GB free of ') + (s.total_bytes / 1e9).toFixed(1));
  if (st.battery && typeof st.battery.pct === 'number') parts.push((it ? 'batteria ' : 'battery ') + Math.round(st.battery.pct) + '%');
  if (typeof st.uptime_s === 'number') { const h = Math.floor(st.uptime_s / 3600), m = Math.floor((st.uptime_s % 3600) / 60); parts.push((it ? 'acceso da ' : 'up ') + (h ? h + ' h ' : '') + m + ' min'); }
  const n = st.network || {};
  if (n.mode === 'sta' && n.ip) parts.push('Wi-Fi «' + (n.ssid || '?') + '» ' + n.ip);
  if (typeof st.free_heap === 'number') parts.push('RAM ' + Math.round(st.free_heap / 1024) + ' KB');
  if (st.profile) parts.push(st.profile === 'web' ? (it ? 'modalità web (cervello offline in pausa)' : 'web mode (offline brain paused)') : (it ? 'OS completo' : 'full OS'));
  if (st.version) parts.push('NucleoOS ' + st.version);
  return parts.join(', ');
}

export function commandHint(q) {
  const t = fold(q).replace(/[?!.,;:¿¡]+/g, ' ').replace(/\s+/g, ' ').trim();
  if (!t) return null;
  if (!HOWTO.test(t)) {
    // A setting needs an imperative verb, or a SHORT "noun + amount" phrase ("volume al 50", "più luce"):
    // like the engine's own guard, a longer verb-less sentence is a statement ("l'audio del film era basso").
    const words = t.split(' ').length;
    if (SETTING_NOUN.test(t) && !GEOMETRY.test(t) &&
        (SETTING_VERB.test(t) || (SETTING_AMOUNT.test(t) && words <= 4))) return 'act';
    if (REMIND.test(t) || (EVENT_VERB.test(t) && EVENT_NOUN.test(t)) || (TIMER.test(t) && TIMER_CUE.test(t))) return 'act';
  }
  for (const re of LIVE) if (re.test(t)) return 'live';
  if (LAUNCH.test(t)) return 'launch';
  return null;
}

// ---- personal memory: ONE owner --------------------------------------------------------------------
// What the user teaches ("ricorda che X è Y", "mi chiamo Marco") and asks back ("come mi chiamo", "qual è
// il mio colore preferito") belongs to the DEVICE (user.tsv / profile.tsv), not to whichever browser
// happened to be open. The host routes these utterances to the device first; the in-browser store is only
// the fallback while the device is unreachable, and what it learns there is replayed later.
//   'teach'   - an explicit teach frame: a teach lead + a binding copula. The copula is the accented "è"
//               (the engine's own frame) or, after a possessive ("il mio colore preferito e il blu"), the
//               unaccented "e" people type on a keyboard without accents; sono/significa/is/are/means.
//   'profile' - a typed personal fact: "mi chiamo X", "my name is X", "vivo a X", "ho 30 anni", ...
//   'recall'  - asking any of it back: "come mi chiamo", "qual è il mio …", "what's my …", "cosa sai di me".
// Mirrors the leads in the engine (nucleo_anima.c tool_teach, nucleo_anima_profile.c SET/RECALL); a lexical
// router, not the parser — the device decides what is actually stored.
const TEACH_LEAD = /(?:^|\s)(?:ricorda|ricordati|ricordare|impara|imparare|memorizza|tieni a mente|annota|segnati|sappi|remember|teach anima|teach you|teach|learn|note|keep in mind) che\s|(?:^|\s)(?:remember|teach anima|teach you|teach|learn|note|keep in mind) that\s/;
const TEACH_COPULA = /\s(?:e|sono|significa|vuol dire|is|are|means)\s/;   // "e" is the folded "è"
const POSSESSIVE = /(?:^|\s)(?:il mio|la mia|i miei|le mie|mio|mia|my)\s/;
const PROFILE_SET = new RegExp('(?:^|\\s)(?:' + [
  'mi chiamo', 'il mio nome e', 'puoi chiamarmi', 'chiamami', 'my name is', 'you can call me', 'call me', "i'm called", 'i am called',
  'abito a', 'abito in', 'vivo a', 'vivo in', 'i live in', 'i live at',
  'di lavoro faccio', 'il mio lavoro e', 'lavoro come', 'di mestiere faccio', 'my job is', 'i work as',
  'la mia e-?mail e', 'la mia mail e', 'my e-?mail(?: address)? is',
  'il mio compleanno e', 'sono nat[oa] il', 'my birthday is', 'i was born on',
].join('|') + ')\\s');
const PROFILE_AGE = /(?:^|\s)(?:ho \d{1,3} anni|i am \d{1,3}(?: years? old)?|i'm \d{1,3}(?: years? old)?|my age is \d{1,3})(?:\s|$)/;
const MEM_RECALL = new RegExp('(?:^|\\s)(?:' + [
  'come mi chiamo', 'cosa sai di me', 'che cosa sai di me', 'che sai di me', 'cosa sai su di me', 'il mio profilo',
  'quanti anni ho', 'che eta ho', 'dove abito', 'dove vivo', 'in che citta vivo', 'in quale citta abito',
  'che lavoro faccio', 'che lavoro ho', 'quando sono nat[oa]',
  'qual e il mio', 'qual e la mia', 'quale e il mio', 'quale e la mia', 'quali sono i miei', 'quali sono le mie',
  'ti ricordi (?:il mio|la mia|i miei|le mie|come mi chiamo|di me)', 'quando e il mio compleanno',
  "what(?:'s| is) my", 'what are my', 'what do you know about me', 'tell me about (?:myself|me)', 'my profile',
  'what am i called', 'do you (?:know|remember) my', 'how old am i', 'where do i live', 'what city do i live in',
  'what do i do for (?:work|a living)', "when(?:'s| is) my birthday",
].join('|') + ')(?:\\s|$)');
export function memoryHint(q) {
  const t = fold(q).replace(/[?!.,;:¿¡]+/g, ' ').replace(/\s+/g, ' ').trim();
  if (!t) return null;
  const lead = TEACH_LEAD.exec(t);
  if (lead) {
    const rest = t.slice(lead.index + lead[0].length - 1);
    // fold() strips accents, so the engine's copula "è" is looked for on the raw text; the folded "e"
    // (a conjunction as often as a verb) binds only after a possessive subject.
    const accented = /\s[èé]\s/.test(' ' + String(q).toLowerCase().replace(/\s+/g, ' ') + ' ');
    // A lead with no copula is a reminder ("ricordami di comprare il latte"), not a fact: not ours.
    if (accented || /\s(?:sono|significa|vuol dire|is|are|means)\s/.test(rest) || (POSSESSIVE.test(rest) && TEACH_COPULA.test(rest))) return 'teach';
  }
  if (MEM_RECALL.test(t)) return 'recall';
  if (!HOWTO.test(t) && (PROFILE_SET.test(t + ' ') || PROFILE_AGE.test(t))) return 'profile';
  return null;
}
