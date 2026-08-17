// search-rank.js — the pure ranking rules behind the OS search box (taskbar + Start menu).
//
// Split out of shell.js on purpose: like shortcuts.js and boot-fetch.js, this is decision logic with
// no DOM and no fetch, so it is unit-tested on the host (tools/shell-search-rank.test.mjs) instead of
// being verified by hand in a browser. shell.js owns the rendering and the side effects; this file
// only answers "does it match, and how well".
//
// Before this existed the search matched apps with a bare `name.includes(q)`: unranked (an exact
// prefix could sort below an incidental substring), blind to app IDs, and it never covered the
// settings/system commands the Start placeholder has always advertised.

// Case-fold and treat -, _ and . as word separators, so "file-commander" is one phrase for matching
// purposes and an app ID reads like its display name.
export const normMatch = (s) => String(s == null ? '' : s).toLowerCase().replace(/[-_.]+/g, ' ');

// Score a candidate against a query. -1 = no match. BOTH sides are normalised here, so callers
// cannot half-apply the rule (an un-normalised "file-commander" used to match nothing at all).
//   100 exact · 80 prefix · 60 start of a later word · 20…40 anywhere (earlier is better)
export function matchScore(text, query) {
  const ql = normMatch(query).trim();
  if (!ql) return 0;
  const s = normMatch(text);
  if (s === ql) return 100;
  if (s.startsWith(ql)) return 80;
  if (s.includes(' ' + ql)) return 60;
  const i = s.indexOf(ql);
  return i >= 0 ? Math.max(20, 40 - i) : -1;
}

// Keywords are a SYNONYM LIST, so only a word-boundary hit counts. Matching anywhere made the query
// "tema" hit "sis*tema*" inside the settings keywords — noise, and it pushed a wrong row to the top.
export function kwScore(kw, query) {
  const s = matchScore(kw, query);
  return s >= 60 ? s : -1;
}

// Apps: the display name leads, the ID is a slightly weaker alias (so "file-commander" finds the app
// even when the name is localised). Ties break alphabetically for a stable, predictable list.
export function rankApps(apps, query) {
  const ql = normMatch(query).trim();
  if (!ql) return [];
  return (apps || [])
    .map((app) => ({ app, name: app.name, s: Math.max(matchScore(app.name, ql), matchScore(app.id, ql) - 5) }))
    .filter((r) => r.s >= 0)
    .sort((a, b) => b.s - a.s || String(a.name).localeCompare(String(b.name)));
}

// System actions. Minimum 2 characters: a bare "a" must not push a dozen OS commands above the app
// the user is actually reaching for. Capped, because this list is a shortcut, not a menu.
export function rankActions(actions, query, limit = 6) {
  const ql = normMatch(query).trim();
  if (ql.length < 2) return [];
  return (actions || [])
    .map((act) => ({ act, name: act.name, s: Math.max(matchScore(act.name, ql), kwScore(act.kw, ql) - 10) }))
    .filter((r) => r.s >= 0)
    .sort((a, b) => b.s - a.s || String(a.name).localeCompare(String(b.name)))
    .slice(0, limit);
}

// Does the query read like a natural-language question/command (→ pre-select "Ask ANIMA") rather
// than a short name lookup (→ pre-select the first hit)? Covers all FIVE OS languages: with only
// it/en, a Spanish "abre la calculadora" or a German "wie spät ist es" fell through to the file
// matcher and the ANIMA row was never the default.
const NL_OPENERS = new RegExp('^(?:'
  + 'apri|aprimi|crea|ricord|cerca |che |come |chi |cosa |quando |dove |perch|quanto|meteo|calcola'            // it
  + '|open |create |remind|search |what |who |how |when |where |why |weather |tell me'                          // en
  + '|abre|abrir|crea[r]? |busca|qué |que |cómo |quién |cuándo |dónde |por qué|cuánto|tiempo en|recuérda'       // es
  + '|ouvre|ouvrir|crée|créer|cherche|quoi |que[l]{1,2}[le]* |comment |qui |quand |où |pourquoi|combien|météo'  // fr
  + '|öffne|öffnen|erstelle|suche|was |wie |wer |wann |wo |warum|wieviel|wie viel|wetter|erinnere'              // de
  + ')', 'i');
export function looksLikeNL(q) {
  const s = String(q || '').trim();
  if (/\?$/.test(s)) return true;
  if (s.split(/\s+/).length >= 3) return true;
  return NL_OPENERS.test(s);
}
