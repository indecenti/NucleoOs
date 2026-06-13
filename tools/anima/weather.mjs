// ANIMA weather NLU — shared, pure, testable.
//
// The native firmware mirrors this exact logic (nucleo_anima_online.c). The hard part of a
// weather assistant on an ESP32 is NOT the HTTP call — it's understanding the question with no
// NER, no LLM, no parser generator. So instead of the fragile "city must follow a preposition"
// rule (which fails on the single most common phrasing, "meteo brescia"), we use a RESIDUAL
// strip: peel away everything we DO recognise — the weather words, the time words, the dates,
// the fillers, the prepositions — and whatever contiguous run of words is left standing IS the
// place. Robust by construction: it doesn't need to know city names, it knows everything that
// isn't one.
//
// parseWeather(q, {lang, now}) -> {
//   isWeather, city, dayOffset, tooFar, aspect, dateLabel
// }

// ---- normalisation ---------------------------------------------------------
// Lowercase, strip accents, drop apostrophes, collapse whitespace. Keep digits (dates).
export function normWeather(s) {
  return String(s || '')
    .toLowerCase()
    .normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/['’`]/g, ' ')
    .replace(/[^a-z0-9]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

// ---- lexicons --------------------------------------------------------------
// STRONG weather words: their presence makes it a weather question. "tempo" is deliberately NOT
// here (it means time/duration too) — only the fixed phrase "che tempo" counts.
const WX_STRONG = ['meteo','previsioni','previsione','clima','climatic',
  'piove','piover','piova','pioggia','piovoso','piovera','pioggie',
  'sole','soleggiat','nuvol','nubi','sereno','coperto','nebbia','foschia',
  'neve','nevica','nevicher','grandine','temporale','temporali','rovesci',
  'temperatura','temperature','gradi','caldo','freddo','afa','umidita','umid','vento','ventoso',
  'weather','forecast','rain','raining','rainy','sunny','cloud','cloudy','snow','snowing','snowy',
  'storm','thunderstorm','fog','foggy','windy','temperature','degrees','hot','cold','chilly','humidity',
  'ombrello','umbrella','bel tempo','brutto tempo'];

// Prepositions (a subset of PEEL). A run immediately following one of these is almost always the
// place ("a roma", "in london") — this beats "longest run" when a stray verb precedes the city.
const PREP = new Set(['a','ad','al','allo','alla','ai','agli','alle','in','nel','nello','nella','nei','negli','nelle',
  'di','del','dello','della','dei','degli','delle','da','dal','dallo','dalla','dai','dagli','dalle',
  'su','sul','sullo','sulla','sui','sugli','sulle','per','con','tra','fra','presso',
  'at','on','for','near','around','of','to','from','with','into']);

// Words to PEEL during residual place extraction. Superset of weather words plus fillers,
// question words, verbs of "to be/to do/to make", prepositions, articles. None of these are
// plausible city names, so removing them can't eat a place.
const PEEL = new Set([
  // weather surface forms (broad, since these never name a city)
  'meteo','previsioni','previsione','clima','climatico','climatica',
  'piove','piover','piovera','piovere','piova','pioggia','piovoso','piovosa','piovera','piogge',
  'sole','soleggiato','soleggiata','nuvoloso','nuvolosa','nuvole','nubi','sereno','serena','coperto','coperta',
  'nebbia','foschia','neve','nevica','nevicara','nevichera','grandine','temporale','temporali','rovesci',
  'temperatura','temperature','gradi','grado','caldo','calda','freddo','fredda','afa','umidita','vento','ventoso',
  'weather','forecast','rain','raining','rainy','sun','sunny','cloud','clouds','cloudy','overcast','clear',
  'snow','snowing','snowy','hail','storm','storms','thunderstorm','fog','foggy','mist','windy','wind',
  'temperature','degrees','degree','hot','warm','cold','chilly','humidity','humid','umido','umida',
  'massima','minima','massime','minime','max','min','meteorologica','meteorologiche',
  'ombrello','umbrella','bel','bello','brutto','brutta','buono','buon','buona','cattivo','cattiva','giornata',
  // time words (relative + units) — dates handled separately but peel the surface too
  'oggi','domani','dopodomani','stamattina','stamani','stasera','stanotte','stamane','domattina',
  'adesso','ora','ore','attualmente','attuale','poi','prossimo','prossima','prossimi','prossime','scorso','scorsa',
  'today','tomorrow','tonight','now','currently','later','next','this','coming','upcoming',
  'giorno','giorni','settimana','weekend','week','day','days','morning','afternoon','evening','night',
  // question / filler / verbs / connectors
  'che','cosa','come','quanto','quanta','quanti','quante','quale','quali','dove','quando','perche',
  'ci','c','si','e','ed','o','ma','se','non','mi','ti','sara','sarra','saranno','fa','fara','faranno','farra',
  'fare','essere','avere','avremo','avra','avranno','stara','staranno','tempo','meteorologico','meteorologiche',
  'previsto','prevista','previste','previsti','aspetta','aspettano','dimmi','dammi','sapere','vorrei','voglio',
  'puoi','potresti','mostrami','mostra','controlla','dico','dice','allora','insomma','citta','paese','zona',
  'what','whats','will','would','is','are','be','being','the','a','an','do','does','going','gonna','tell','me',
  'give','know','show','check','please','about','like','there','it','its','expected','forecasted','any',
  'sai','dire','vuoi','how','right',
  'dici','dirmi','dimmelo','dillo','favore','grazie','sapere','potrebbe','pomeriggio','mattina','sera','notte',
  // articles & demonstratives (Italian bare forms — never a city head)
  'il','lo','la','i','gli','le','un','uno','una','un',
  'questo','questa','questi','queste','quello','quella','quei','quegli','quelle','quel',
  // prepositions
  'a','ad','al','allo','alla','ai','agli','alle','in','nel','nello','nella','nei','negli','nelle',
  'di','del','dello','della','dei','degli','delle','da','dal','dallo','dalla','dai','dagli','dalle',
  'su','sul','sullo','sulla','sui','sugli','sulle','per','con','tra','fra','presso',
  'at','on','for','near','around','of','to','from','with','into',
]);

// IT / EN month names -> 1..12
const MONTHS = {
  gennaio:1, febbraio:2, marzo:3, aprile:4, maggio:5, giugno:6, luglio:7, agosto:8,
  settembre:9, ottobre:10, novembre:11, dicembre:12,
  january:1, february:2, march:3, april:4, may:5, june:6, july:7, august:8,
  september:9, october:10, november:11, december:12,
  gen:1, feb:2, mar:3, apr:4, mag:5, giu:6, lug:7, ago:8, set:9, sett:9, ott:10, nov:11, dic:12,
  jan:1, jun:6, jul:7, aug:8, sep:9, sept:9, oct:10, dec:12,
};
// Weekday name -> 0(Sun)..6(Sat) to match Date.getDay()
const WEEKDAYS = {
  domenica:0, lunedi:1, martedi:2, mercoledi:3, giovedi:4, venerdi:5, sabato:6,
  sunday:0, monday:1, tuesday:2, wednesday:3, thursday:4, friday:5, saturday:6,
  lun:1, mar:2, mer:3, gio:4, ven:5, sab:6, dom:0,
};

// Open-Meteo free forecast horizon (days ahead). Beyond this we honestly refuse.
export const FORECAST_HORIZON = 15;

// ---- weather intent --------------------------------------------------------
// A definition question ("cos'è il clima", "what is rain") is knowledge, not a live lookup.
function isDefinition(nf) {
  return /\bcos\b|\bcosa e\b|\bche cos|\bche cosa\b|\bsignifica\b|\bdefinizione\b|\bspiega\b/.test(nf)
      || /\bwhat is\b|\bwhat does\b|\bwhat are\b|\bmeaning of\b|\bdefine\b/.test(nf);
}
export function isWeatherQuery(nf) {
  // An explicit live-REPORT word ("meteo"/"weather"/"forecast"/"che tempo") makes it a lookup even
  // in a "what is …" frame ("what is the weather like" is NOT a definition of the noun "weather").
  const report = /\bmeteo\b|\bprevision|\bforecast\b|\bweather\b|\bche tempo\b|\btempo fa\b|\btempo fara\b/.test(nf);
  if (report) return true;
  if (isDefinition(nf)) return false;   // "cos'è il clima", "what is rain" -> knowledge, not a lookup
  for (const w of WX_STRONG) if (nf.includes(w)) return true;
  return false;
}

// What facet did they ask about? Lets us answer the actual question, not dump a bulletin.
function detectAspect(nf) {
  if (/\bpiov|\bpioggia\b|\brain|\bumbrella\b|\bombrello\b/.test(nf)) return 'rain';
  if (/\bsole\b|\bsoleggiat|\bsereno\b|\bsunny\b|\bsun\b|\bclear\b/.test(nf)) return 'sun';
  if (/\bnev|\bsnow/.test(nf)) return 'snow';
  if (/\btemperatur|\bgradi\b|\bcaldo\b|\bfreddo\b|\bhot\b|\bcold\b|\bwarm\b|\bchilly\b|\bdegrees?\b/.test(nf)) return 'temp';
  if (/\bvento\b|\bventoso\b|\bwind|\bwindy\b/.test(nf)) return 'wind';
  return 'general';
}

// ---- date parsing ----------------------------------------------------------
// Returns {dayOffset, tooFar, dateLabel, hitTokens:Set} from a token list. Pure given `now`.
function parseDate(tokens, lang, now) {
  const en = lang === 'en';
  const today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
  const dayMs = 86400000;
  const offsetOf = (d) => Math.round((d - today) / dayMs);
  const labelDate = (d) => {
    const md = en
      ? ['January','February','March','April','May','June','July','August','September','October','November','December']
      : ['gennaio','febbraio','marzo','aprile','maggio','giugno','luglio','agosto','settembre','ottobre','novembre','dicembre'];
    return en ? `${md[d.getMonth()]} ${d.getDate()}` : `${d.getDate()} ${md[d.getMonth()]}`;
  };
  const wkLabel = (dow) => (en
    ? ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'][dow]
    : ['domenica','lunedi','martedi','mercoledi','giovedi','venerdi','sabato'][dow]);

  const hit = new Set();
  const has = (w) => tokens.includes(w);

  // explicit relative words (longest/most-specific first)
  if (has('dopodomani') || (tokens.join(' ').includes('day after tomorrow'))) {
    ['dopodomani','day','after','tomorrow'].forEach(t => hit.add(t));
    return { dayOffset: 2, tooFar: false, dateLabel: en ? 'the day after tomorrow' : 'dopodomani', hitTokens: hit };
  }
  if (has('domani') || has('tomorrow') || has('domattina')) {
    ['domani','tomorrow','domattina'].forEach(t => hit.add(t));
    return { dayOffset: 1, tooFar: false, dateLabel: en ? 'tomorrow' : 'domani', hitTokens: hit };
  }

  // "fra/tra/in N giorni|days"
  for (let i = 0; i < tokens.length - 1; i++) {
    if (['fra','tra','in'].includes(tokens[i])) {
      const n = parseInt(tokens[i + 1], 10);
      const unit = tokens[i + 2];
      if (Number.isFinite(n) && (unit === 'giorni' || unit === 'giorno' || unit === 'days' || unit === 'day')) {
        hit.add(tokens[i]); hit.add(tokens[i + 1]); if (unit) hit.add(unit);
        const d = new Date(today.getTime() + n * dayMs);
        return { dayOffset: n, tooFar: n > FORECAST_HORIZON, dateLabel: labelDate(d), hitTokens: hit };
      }
    }
  }

  // "DD <month>" or "<month> DD" (with optional year ignored)
  for (let i = 0; i < tokens.length; i++) {
    const m = MONTHS[tokens[i]];
    if (!m) continue;
    let day = null;
    if (i > 0 && /^\d{1,2}$/.test(tokens[i - 1])) { day = parseInt(tokens[i - 1], 10); hit.add(tokens[i - 1]); }
    else if (i < tokens.length - 1 && /^\d{1,2}$/.test(tokens[i + 1])) { day = parseInt(tokens[i + 1], 10); hit.add(tokens[i + 1]); }
    if (day && day >= 1 && day <= 31) {
      hit.add(tokens[i]);
      let year = now.getFullYear();
      let d = new Date(year, m - 1, day);
      if (offsetOf(d) < 0) { d = new Date(year + 1, m - 1, day); }   // a passed date means next year
      const off = offsetOf(d);
      return { dayOffset: off, tooFar: off > FORECAST_HORIZON, dateLabel: labelDate(d), hitTokens: hit };
    }
  }

  // weekday name -> next occurrence (today's weekday means next week, not today)
  for (const t of tokens) {
    if (t in WEEKDAYS) {
      const target = WEEKDAYS[t];
      let off = (target - today.getDay() + 7) % 7;
      if (off === 0) off = 7;
      hit.add(t);
      return { dayOffset: off, tooFar: off > FORECAST_HORIZON, dateLabel: wkLabel(target), hitTokens: hit };
    }
  }

  // weekend -> upcoming Saturday
  if (has('weekend')) {
    hit.add('weekend');
    let off = (6 - today.getDay() + 7) % 7; if (off === 0) off = 7;
    return { dayOffset: off, tooFar: false, dateLabel: en ? 'this weekend' : 'questo weekend', hitTokens: hit };
  }

  // default: today
  ['oggi','today','adesso','ora','now','stamattina','stamani','stasera','stanotte'].forEach(t => { if (has(t)) hit.add(t); });
  return { dayOffset: 0, tooFar: false, dateLabel: en ? 'today' : 'oggi', hitTokens: hit };
}

// ---- place extraction (residual) -------------------------------------------
// Peel every recognised token; whatever contiguous runs of survivors remain are place candidates.
// Selection: a run immediately following a preposition ("a roma", "in london") is the place — this
// beats "longest" when a stray verb/filler precedes the city ("ci sono a roma" -> roma, not "sono").
// With no preposition anywhere ("meteo brescia", "previsioni reggio emilia"), the longest run wins.
function extractCity(tokens, dateHits) {
  const runs = [];                       // { text, prep, len }
  let run = [], runPrep = false, prevDropPrep = false;
  const flush = () => { if (run.length) runs.push({ text: run.join(' '), prep: runPrep, len: run.length }); run = []; };
  for (const t of tokens) {
    const drop = PEEL.has(t) || dateHits.has(t) || /^\d+$/.test(t) || t.length < 2 || (t in MONTHS) || (t in WEEKDAYS);
    if (drop) { flush(); prevDropPrep = PREP.has(t); }
    else { if (!run.length) runPrep = prevDropPrep; run.push(t); }
  }
  flush();
  if (!runs.length) return '';
  const prepRun = runs.find(r => r.prep);          // first place introduced by a preposition
  if (prepRun) return prepRun.text;
  return runs.slice().sort((a, b) => b.len - a.len)[0].text;   // else the longest surviving run
}

// ---- top-level -------------------------------------------------------------
export function parseWeather(q, opts = {}) {
  const lang = opts.lang === 'en' ? 'en' : 'it';
  const now = opts.now instanceof Date ? opts.now : new Date();
  const nf = normWeather(q);
  const out = { isWeather: false, city: '', dayOffset: 0, tooFar: false, aspect: 'general', dateLabel: lang === 'en' ? 'today' : 'oggi' };
  if (!isWeatherQuery(nf)) return out;
  out.isWeather = true;
  out.aspect = detectAspect(nf);
  const tokens = nf.split(' ').filter(Boolean);
  const dt = parseDate(tokens, lang, now);
  out.dayOffset = dt.dayOffset;
  out.tooFar = dt.tooFar;
  out.dateLabel = dt.dateLabel;
  out.city = extractCity(tokens, dt.hitTokens);
  return out;
}

// ---- reply formatting (pure: parsed result + forecast row -> sentence) -----
// fc = { code, tmax, tmin, tcur(optional), precipProb(optional) }
const WMO = {
  it: { 0:'sereno', 1:'poco nuvoloso', 2:'poco nuvoloso', 3:'coperto', 45:'nebbia', 48:'nebbia',
    51:'pioggerella', 53:'pioggerella', 55:'pioggerella', 56:'pioggerella gelata', 57:'pioggerella gelata',
    61:'pioggia', 63:'pioggia', 65:'pioggia forte', 66:'pioggia gelata', 67:'pioggia gelata',
    71:'neve', 73:'neve', 75:'neve abbondante', 77:'nevischio', 80:'rovesci', 81:'rovesci', 82:'rovesci forti',
    85:'rovesci di neve', 86:'rovesci di neve', 95:'temporale', 96:'temporale con grandine', 99:'temporale con grandine' },
  en: { 0:'clear sky', 1:'partly cloudy', 2:'partly cloudy', 3:'overcast', 45:'fog', 48:'fog',
    51:'drizzle', 53:'drizzle', 55:'drizzle', 56:'freezing drizzle', 57:'freezing drizzle',
    61:'rain', 63:'rain', 65:'heavy rain', 66:'freezing rain', 67:'freezing rain',
    71:'snow', 73:'snow', 75:'heavy snow', 77:'snow grains', 80:'showers', 81:'showers', 82:'heavy showers',
    85:'snow showers', 86:'snow showers', 95:'thunderstorm', 96:'thunderstorm with hail', 99:'thunderstorm with hail' },
};
export function wmoText(code, lang) {
  const tbl = lang === 'en' ? WMO.en : WMO.it;
  return tbl[code] || (lang === 'en' ? 'variable' : 'variabile');
}
const isRainCode = (c) => (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;
const isSnowCode = (c) => (c >= 71 && c <= 77) || c === 85 || c === 86;
const isSunCode  = (c) => c === 0 || c === 1;

// Build the human reply for a resolved place + day + forecast row, answering the asked aspect.
export function formatWeather(parsed, place, fc, lang) {
  const en = lang === 'en';
  const desc = wmoText(fc.code, lang);
  const hi = Math.round(fc.tmax), lo = Math.round(fc.tmin);
  const when = parsed.dayOffset === 0 ? (en ? 'today' : 'oggi')
            : parsed.dayOffset === 1 ? (en ? 'tomorrow' : 'domani')
            : parsed.dayOffset === 2 ? (en ? 'the day after tomorrow' : 'dopodomani')
            : parsed.dateLabel;
  const at = en ? `In ${place}` : `A ${place}`;
  const pp = (typeof fc.precipProb === 'number') ? fc.precipProb : null;

  // Targeted yes/no for a specific aspect.
  if (parsed.aspect === 'rain') {
    const willRain = isRainCode(fc.code) || (pp !== null && pp >= 50);
    const head = en
      ? (willRain ? `Yes — rain is expected ${when} in ${place}` : `No — no rain expected ${when} in ${place}`)
      : (willRain ? `Sì, ${when} a ${place} è prevista pioggia` : `No, ${when} a ${place} non è prevista pioggia`);
    const tail = pp !== null ? (en ? ` (${pp}% chance, ${desc}, ${lo}–${hi}°C).` : ` (probabilità ${pp}%, ${desc}, ${lo}–${hi}°C).`)
                             : (en ? ` (${desc}, ${lo}–${hi}°C).` : ` (${desc}, ${lo}–${hi}°C).`);
    return head + tail;
  }
  if (parsed.aspect === 'sun') {
    const sunny = isSunCode(fc.code);
    return en
      ? (sunny ? `Yes — ${when} looks sunny in ${place} (${desc}, ${lo}–${hi}°C).` : `Not really — ${when} in ${place}: ${desc}, ${lo}–${hi}°C.`)
      : (sunny ? `Sì, ${when} a ${place} ci sarà sole (${desc}, ${lo}–${hi}°C).` : `Non proprio: ${when} a ${place} ${desc}, ${lo}–${hi}°C.`);
  }
  if (parsed.aspect === 'snow') {
    const snowy = isSnowCode(fc.code);
    return en
      ? (snowy ? `Yes — snow is expected ${when} in ${place} (${desc}, ${lo}–${hi}°C).` : `No — no snow ${when} in ${place} (${desc}, ${lo}–${hi}°C).`)
      : (snowy ? `Sì, ${when} a ${place} è prevista neve (${desc}, ${lo}–${hi}°C).` : `No, ${when} a ${place} non è prevista neve (${desc}, ${lo}–${hi}°C).`);
  }
  if (parsed.aspect === 'temp') {
    if (parsed.dayOffset === 0 && typeof fc.tcur === 'number')
      return en ? `In ${place} now it's ${Math.round(fc.tcur)}°C (${desc}, ${lo}–${hi}°C today).`
                : `A ${place} ora ci sono ${Math.round(fc.tcur)}°C (${desc}, min ${lo} / max ${hi}°C).`;
    return en ? `${at} ${when}: ${lo}–${hi}°C (${desc}).` : `${at} ${when}: ${lo}–${hi}°C (${desc}).`;
  }
  // general bulletin
  if (parsed.dayOffset === 0 && typeof fc.tcur === 'number')
    return en ? `In ${place} now: ${desc}, ${Math.round(fc.tcur)}°C (low ${lo} / high ${hi}°C).`
              : `A ${place} ora: ${desc}, ${Math.round(fc.tcur)}°C (min ${lo} / max ${hi}°C).`;
  return en ? `${at} ${when}: ${desc}, low ${lo}°C / high ${hi}°C.`
            : `${at} ${when}: ${desc}, min ${lo}°C / max ${hi}°C.`;
}
