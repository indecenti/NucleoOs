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
const SETTING_NOUN = /\b(volume|audio|suono|luminosita|brightness|schermo|screen|display|retroilluminazione|backlight)\b/;
const SETTING_VERB = /\b(alza|abbassa|aumenta|diminuisci|riduci|imposta|metti|porta|regola|cambia|setta|modifica|muta|silenzia|azzera|raise|lower|increase|decrease|set|turn|mute|unmute|dim|brighten|change|adjust|fai|rendi|make)\b/;
const SETTING_AMOUNT = /\b(piu|meno|more|less|max|massimo|massima|minimo|minima|meta|half|zero|muto|alto|alta|basso|bassa|up|down)\b|\d/;
const GEOMETRY = /\b(cubo|sfera|cilindro|cono|piramide|prisma|lato|raggio|altezza|diametro|densita|massa|litri|cube|sphere|cylinder|cone|pyramid|prism|side|radius|height|diameter|density|mass|liters|litres|vendite|sales)\b/;
const REMIND = /\b(ricordami|ricordamelo|ricordatemi|promemoria|remind me|reminder)\b/;
const EVENT_VERB = /\b(aggiungi|crea|segna|metti|fissa|programma|pianifica|prenota|inserisci|add|create|schedule|book|put|new|nuovo|nuova)\b/;
const EVENT_NOUN = /\b(evento|eventi|appuntamento|appuntamenti|impegno|riunione|incontro|event|appointment|meeting)\b|\b(in|nel|al|to|on) (my |mio |il |nel )?(calendario|calendar|agenda)\b/;
const TIMER = /\b(timer|sveglia|alarm)\b/;
const TIMER_CUE = /\b(metti|imposta|avvia|punta|fai partire|set|start)\b|\b\d+\s*(s|sec|secondi|seconds|min|minuti|minutes|h|ore|hours)\b|\b(alle|at)\s+\d/;
// Live state is matched as the WHOLE utterance (plus polite fillers), never as a fragment: "che versione di
// python devo usare" or "che giorno è natale" are questions for a brain, not for the RTC, and must not be
// answered by the device ahead of the cloud.
const LEAD = String.raw`(?:(?:ehi |hey |ciao )?anima,? )?(?:(?:mi )?(?:dici|sai dirmi|puoi dirmi) |dimmi |(?:can|could) you tell me |tell me |please )?`;
const TAIL = String.raw` ?(?:adesso|ora|oggi|now|today|please|per favore|grazie)?`;
const LIVE = [
  String.raw`che or[ae] (?:e|sono)|che ora e|l'ora|ora esatta|what time is it|what'?s the time|the time|current time`,
  String.raw`che giorno (?:e|siamo)(?: oggi)?|oggi che giorno e|che data e(?: oggi)?|(?:la )?data(?: di oggi)?|what day is (?:it|today)|what'?s the date|what is the date|today'?s date|the date`,
  String.raw`che anno (?:e|siamo)|in che anno siamo|what year is it|che stagione e|in che stagione siamo|what season is it`,
  String.raw`(?:quanta|livello(?: della)?|stato(?: della)?|carica(?: della)?) batteria(?: ho| hai| c'e| rimane| resta)?|batteria|battery(?: level| left| status)?|how much battery(?: is left| do i have| left)?`,
  String.raw`quanto spazio (?:libero |rimasto )?(?:ho|hai|c'e|resta|rimane)(?: sulla sd| su sd)?|spazio (?:libero|rimasto|disponibile|su sd|sulla sd)|(?:free|disk|sd) space|how much (?:free )?space(?: is left| do i have| left)?|storage left`,
  String.raw`quanta (?:ram|memoria)(?: libera)?(?: ho| hai| c'e)?|(?:ram|memoria) (?:libera|disponibile)|free (?:ram|memory)|how much (?:ram|memory)(?: is free| do you have| left)?`,
  String.raw`uptime|da quanto (?:tempo )?(?:sei|e) acces[oa]|how long have you been (?:on|up|running)`,
  String.raw`(?:che|quale) versione (?:sei|hai|e|di nucleoos|del firmware|del sistema)|versione(?: del)? firmware|firmware version|what version (?:are you|is this|of nucleoos)`,
  String.raw`(?:a che|a quale) (?:rete|wi-?fi) sono connesso|(?:che|quale) (?:rete|wi-?fi)(?: e| uso| stai usando)?|sono connesso(?: a internet)?|am i connected|(?:which|what) (?:network|wi-?fi)(?: am i on| is this)?|(?:qual e )?(?:il mio )?indirizzo ip|(?:what'?s )?my ip(?: address)?|ip address`,
  String.raw`(?:che|quali) (?:impegni|appuntamenti) ho(?: oggi| domani)?|i miei impegni|impegni(?: di)? oggi|cosa ho (?:in agenda|oggi|domani)|agenda(?: di)? oggi|(?:what'?s|what is) on (?:today|my calendar)|my (?:schedule|agenda|appointments)(?: today)?`,
].map((re) => new RegExp('^' + LEAD + '(?:' + re + ')' + TAIL + '$'));
// "how do I raise the volume on my PC?" is a how-to, not an order: interrogative openers never trigger 'act'.
const HOWTO = /^(come|perche|quando|dove|chi|cosa|che cosa|quale|how|why|when|where|who|what|which|can i|posso)\b/;
const LAUNCH = /^(apri|avvia|lancia|open|launch)\s+(?!source\b)\S/;
export function commandHint(q) {
  const t = fold(q).replace(/[?!.,;:]+/g, ' ').replace(/\s+/g, ' ').trim();
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
  const t = fold(q).replace(/[?!.,;:]+/g, ' ').replace(/\s+/g, ' ').trim();
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
