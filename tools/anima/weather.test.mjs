// ANIMA weather NLU tests — pure parse assertions, deterministic via a fixed `now`.
// now = Tuesday 2026-02-10 (getDay()=2). Part of `npm test` (node --test).
// (End-to-end live-forecast checks live in weather.e2e.mjs — run manually against the sim.)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseWeather } from './weather.mjs';

const NOW = new Date(2026, 1, 10);   // 2026-02-10, a Tuesday

// Register one node:test per case; `exp` is the subset of parsed fields to assert.
function t(q, lang, exp) {
  test(`[${lang}] ${q}`, () => {
    const r = parseWeather(q, { lang, now: NOW });
    for (const k of Object.keys(exp)) {
      let a = r[k], e = exp[k];
      if (k === 'city') { a = (a || '').toLowerCase(); e = (e || '').toLowerCase(); }
      assert.equal(a, e, `${k}: got ${JSON.stringify(a)}, want ${JSON.stringify(e)}`);
    }
  });
}

// ---- the user's exact examples ----
t('meteo brescia', 'it', { isWeather: true, city: 'brescia', dayOffset: 0 });
t('meteo brescia domani', 'it', { isWeather: true, city: 'brescia', dayOffset: 1 });
t('meteo brescia 24 febbraio', 'it', { isWeather: true, city: 'brescia', dayOffset: 14, tooFar: false });
t('che tempo fara domani a brescia', 'it', { isWeather: true, city: 'brescia', dayOffset: 1 });
t('domani a brescia piove?', 'it', { isWeather: true, city: 'brescia', dayOffset: 1, aspect: 'rain' });
t('ci sara sole domani a brescia', 'it', { isWeather: true, city: 'brescia', dayOffset: 1, aspect: 'sun' });
t('che tempo fa a brescia', 'it', { isWeather: true, city: 'brescia', dayOffset: 0 });
t('domani a brescia piove', 'it', { isWeather: true, city: 'brescia', dayOffset: 1, aspect: 'rain' });
t('a brescia domani ci sara il sole?', 'it', { isWeather: true, city: 'brescia', dayOffset: 1, aspect: 'sun' });

// ---- no-preposition city (the main bug) ----
t('meteo milano', 'it', { isWeather: true, city: 'milano', dayOffset: 0 });
t('meteo roma domani', 'it', { isWeather: true, city: 'roma', dayOffset: 1 });
t('previsioni torino', 'it', { isWeather: true, city: 'torino', dayOffset: 0 });
t('previsioni reggio emilia', 'it', { isWeather: true, city: 'reggio emilia', dayOffset: 0 });
t('meteo san giovanni rotondo', 'it', { isWeather: true, city: 'san giovanni rotondo', dayOffset: 0 });
t('piove a san marino?', 'it', { isWeather: true, city: 'san marino', dayOffset: 0, aspect: 'rain' });

// ---- aspects ----
t('temperatura a napoli oggi', 'it', { isWeather: true, city: 'napoli', dayOffset: 0, aspect: 'temp' });
t('fara freddo a bolzano domani?', 'it', { isWeather: true, city: 'bolzano', dayOffset: 1, aspect: 'temp' });
t('nevica a cortina?', 'it', { isWeather: true, city: 'cortina', dayOffset: 0, aspect: 'snow' });
t('ci sara vento a trieste domani', 'it', { isWeather: true, city: 'trieste', dayOffset: 1, aspect: 'wind' });
t('domani piove a genova', 'it', { isWeather: true, city: 'genova', dayOffset: 1, aspect: 'rain' });

// ---- relative dates ----
t('meteo a milano dopodomani', 'it', { isWeather: true, city: 'milano', dayOffset: 2 });
t('che tempo fa a firenze tra 3 giorni', 'it', { isWeather: true, city: 'firenze', dayOffset: 3 });
t('meteo a venezia fra 20 giorni', 'it', { isWeather: true, city: 'venezia', dayOffset: 20, tooFar: true });

// ---- weekday (now=Tuesday) ----
t('che tempo fara lunedi a torino', 'it', { isWeather: true, city: 'torino', dayOffset: 6 });
t('meteo venerdi a bari', 'it', { isWeather: true, city: 'bari', dayOffset: 3 });
t('meteo martedi a lecce', 'it', { isWeather: true, city: 'lecce', dayOffset: 7 });

// ---- absolute date / out of horizon ----
t('meteo brescia 12 febbraio', 'it', { isWeather: true, city: 'brescia', dayOffset: 2, tooFar: false });
t('meteo brescia 5 gennaio', 'it', { isWeather: true, city: 'brescia', tooFar: true });   // passed -> next year

// ---- English ----
t('weather in new york tomorrow', 'en', { isWeather: true, city: 'new york', dayOffset: 1 });
t('weather london', 'en', { isWeather: true, city: 'london', dayOffset: 0 });
t('will it rain in paris tomorrow?', 'en', { isWeather: true, city: 'paris', dayOffset: 1, aspect: 'rain' });
t('is it sunny in madrid', 'en', { isWeather: true, city: 'madrid', dayOffset: 0, aspect: 'sun' });
t('forecast for berlin in 5 days', 'en', { isWeather: true, city: 'berlin', dayOffset: 5 });
t('temperature in tokyo', 'en', { isWeather: true, city: 'tokyo', dayOffset: 0, aspect: 'temp' });

// ---- harder real-world phrasings ----
t('che tempo fara questo weekend a roma', 'it', { isWeather: true, city: 'roma' });
t('meteo a milano oggi pomeriggio', 'it', { isWeather: true, city: 'milano', dayOffset: 0 });
t('domani che tempo fa a brescia?', 'it', { isWeather: true, city: 'brescia', dayOffset: 1 });
t('mi dici se piove domani a brescia', 'it', { isWeather: true, city: 'brescia', dayOffset: 1, aspect: 'rain' });
t('vorrei sapere il meteo di brescia per domani', 'it', { isWeather: true, city: 'brescia', dayOffset: 1 });
t('fa caldo a palermo?', 'it', { isWeather: true, city: 'palermo', dayOffset: 0, aspect: 'temp' });
t('che tempo fara a reggio calabria sabato', 'it', { isWeather: true, city: 'reggio calabria', dayOffset: 4 });
t('neve sulle dolomiti domani', 'it', { isWeather: true, city: 'dolomiti', dayOffset: 1, aspect: 'snow' });
t('puoi dirmi il meteo a torino per favore', 'it', { isWeather: true, city: 'torino', dayOffset: 0 });

// ---- exotic / stress phrasings ----
t('temperatura massima a milano domani', 'it', { isWeather: true, city: 'milano', dayOffset: 1, aspect: 'temp' });
t('che tempo fa adesso a bologna', 'it', { isWeather: true, city: 'bologna', dayOffset: 0 });
t('fa freddo stamattina a aosta', 'it', { isWeather: true, city: 'aosta', dayOffset: 0, aspect: 'temp' });
t('mi sai dire il meteo di napoli per dopodomani', 'it', { isWeather: true, city: 'napoli', dayOffset: 2 });
t('previsioni meteo per roma', 'it', { isWeather: true, city: 'roma', dayOffset: 0 });
t('che tempo è previsto a verona', 'it', { isWeather: true, city: 'verona', dayOffset: 0 });
t('umido a venezia?', 'it', { isWeather: true, city: 'venezia', dayOffset: 0 });
t('meteo new york', 'it', { isWeather: true, city: 'new york', dayOffset: 0 });
t('what is the weather like in dublin', 'en', { isWeather: true, city: 'dublin', dayOffset: 0 });
t('how cold is it in oslo right now', 'en', { isWeather: true, city: 'oslo', dayOffset: 0, aspect: 'temp' });

// ---- NEW batch: colloquial, negations, proxies, weekend ----
t('domani pioverà a milano?', 'it', { isWeather: true, city: 'milano', dayOffset: 1, aspect: 'rain' });
t('fa bel tempo a napoli?', 'it', { isWeather: true, city: 'napoli', dayOffset: 0 });
t('fa brutto tempo a torino domani', 'it', { isWeather: true, city: 'torino', dayOffset: 1 });
t("c'è il sole a bari?", 'it', { isWeather: true, city: 'bari', dayOffset: 0, aspect: 'sun' });
t('quanti gradi ci sono a roma', 'it', { isWeather: true, city: 'roma', dayOffset: 0, aspect: 'temp' });
t('nevicherà a livigno questo weekend', 'it', { isWeather: true, city: 'livigno', aspect: 'snow' });
t('che tempo farà nel weekend a roma', 'it', { isWeather: true, city: 'roma' });
t('non piove a venezia oggi?', 'it', { isWeather: true, city: 'venezia', dayOffset: 0, aspect: 'rain' });
t('previsioni del tempo per domani a bologna', 'it', { isWeather: true, city: 'bologna', dayOffset: 1 });
t('mi serve l ombrello domani a genova?', 'it', { isWeather: true, city: 'genova', dayOffset: 1, aspect: 'rain' });
t('do i need an umbrella in london tomorrow', 'en', { isWeather: true, city: 'london', dayOffset: 1, aspect: 'rain' });
t('how hot will it be in seville on saturday', 'en', { isWeather: true, city: 'seville', aspect: 'temp' });
t('is it going to snow in helsinki', 'en', { isWeather: true, city: 'helsinki', dayOffset: 0, aspect: 'snow' });

// ---- negatives (must NOT be weather) ----
t('che ora e', 'it', { isWeather: false });
t('quanto tempo ci vuole per arrivare', 'it', { isWeather: false });
t("cos'e il clima", 'it', { isWeather: false });
t('apri le foto', 'it', { isWeather: false });
t('chi e einstein', 'it', { isWeather: false });
t('what time is it', 'en', { isWeather: false });
t('what is rain', 'en', { isWeather: false });

// ---- city missing (weather but no place) ----
t('che tempo fa', 'it', { isWeather: true, city: '' });
t('meteo domani', 'it', { isWeather: true, city: '', dayOffset: 1 });
t('piove?', 'it', { isWeather: true, city: '', aspect: 'rain' });
