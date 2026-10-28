/* SPDX-License-Identifier: AGPL-3.0-or-later */
// ANIMA — abilità di CONSOLE: le domande su QUESTA console le risolve chi ha i dati veri.
//
// Perché esiste. ANIMA veniva da un altro dispositivo: il suo cervello offline (WASM) sa aprire una
// manciata di app e risponde "Batteria: {value}." con il segnaposto ancora dentro, perché il valore lo
// metteva il firmware di quel dispositivo, che qui non c'è. Un modello in cloud, poi, non può sapere
// quanta batteria ha questa console, che giochi ci sono sopra, a cosa giochi da mesi o quale BIOS manca.
// Erano proprio le domande che chi ha in mano una console da gioco fa per prime:
//   "apri la console" apriva le Foto · "gioca a zelda" non faceva niente · "manca il bios?" nemmeno.
//
// Qui quelle domande si riconoscono con regole strette (meglio tacere che rispondere a vanvera: in caso
// di dubbio si torna null e la domanda prosegue verso il motore normale) e si rispondono con i dati del
// device, letti dalle sue API. Nelle 5 lingue di NucleoOS: le domande si riconoscono in it/en/es/fr/de e
// le risposte vengono dal catalogo console-i18n.js. Il modulo è PURO: niente DOM e niente fetch — `api`
// è iniettata, così si prova in Node con una console finta (tools/tests/anima/test.mjs).
//
// Contratto: consoleSkill(q, lang, api) -> risultato nella forma di /api/anima (reply, action, intent,
// domain, tier, card/cards...) oppure null. api = {
//   status()            -> /api/status
//   aliases()           -> registry/app-aliases.json ({aliases:{id:[...]}})
//   apps()              -> /api/apps ({apps:[{id,name}]})              (facoltativa: nomi da mostrare)
//   search(q, n)        -> /api/library/search ({games:[...]})
//   recent(n)           -> /api/library/recent ({games:[...]})
//   bios()              -> /api/library/bios
//   doctor()            -> /api/doctor
//   gamemode()          -> /api/gamemode ({gaming, since, game})
//   library()           -> /api/library ({systems:[{id,name,count}]})
//   games(sys)          -> /api/library/games?sys= ({games:[...scheda, plays, last, secs]})
//   saveHistory(s, f)   -> /api/saves/history?sys=&file= ({files:[{name,kind,mtime,versions}]})
//   now()               -> Date                                         (facoltativa: per i test)
//   rand()              -> [0,1)                                        (facoltativa: per i test)
// }
// Una API che fallisce (401 senza abbinamento, device spento) diventa una risposta onesta, mai un'eccezione.

import { say, LOCALE, fold } from './console-i18n.js';
import * as KB from './game-kb.js';

export { fold };
export const SKILLS_VERSION = 3;

// ---- testo -------------------------------------------------------------------------------------------
const has = (f, re) => re.test(f);
const words = (f) => String(f || '').split(' ').filter(Boolean);

// articoli, preposizioni e riempitivi da togliere ai due capi di un nome ("apri LA calcolatrice", "gioca A
// zelda", "ich will zelda SPIELEN"), nelle 5 lingue
const FILLER = new Set((
  'il lo la i gli le l un uno una al allo alla ai agli alle a ad con col coi di del dello della dei degli delle d ' +
  'per su sul sulla mi ti ci me te gioco giochino videogioco partita app applicazione programma ' +
  'please the an to my game videogame app ' +
  'el los las unos unas de mi tu con para en juego videojuego jugar aplicacion ' +
  'les une des du au aux avec pour mon ma mes jeu jeux jouer appli ' +
  'der die das den dem des ein eine einen einem mit fur von zu im am mein meine spiel spielen gespielt zocken').split(' '));
function strip(f) {
  const w = words(f);
  while (w.length && FILLER.has(w[0])) w.shift();
  while (w.length && FILLER.has(w[w.length - 1])) w.pop();
  return w.join(' ');
}

function fmtBytes(n) {
  if (!(n >= 0)) return '?';
  const g = n / 1073741824;
  if (g >= 100) return Math.round(g) + ' GB';
  if (g >= 1) return g.toFixed(1).replace(/\.0$/, '') + ' GB';
  return Math.max(1, Math.round(n / 1048576)) + ' MB';
}

// Durate e date dette come le direbbe una persona: "2 h 15 min", "ieri", "3 giorni fa".
export function fmtDur(secs, lang) {
  const s = Math.max(0, Math.round(secs || 0));
  if (s < 60) return say(lang, 'dur_under_min');
  const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
  if (!h) return m + ' min';
  return m ? h + ' h ' + m + ' min' : h + ' h';
}
const DAY = 86400000;
function dayStart(d) { return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime(); }
function nowOf(api) { return api && api.now ? api.now() : new Date(); }
function ymdDate(ymd) { const m = String(ymd || '').match(/^(\d{4})(\d{2})(\d{2})$/); return m ? new Date(+m[1], +m[2] - 1, +m[3]) : null; }
function ymdOf(d) { return '' + d.getFullYear() + String(d.getMonth() + 1).padStart(2, '0') + String(d.getDate()).padStart(2, '0'); }
function ageDays(ymd, api) { const d = ymdDate(ymd); return d ? Math.round((dayStart(nowOf(api)) - d.getTime()) / DAY) : 0; }
// "last" di nucleos-playstats.txt: YYYYMMDD (giorno, senza ora). bare=true: la data senza "il"/"on".
function fmtDay(ymd, lang, api, bare) {
  const d = ymdDate(ymd);
  if (!d) return '';
  const days = ageDays(ymd, api);
  if (days <= 0) return say(lang, 'today');
  if (days === 1) return say(lang, 'yesterday');
  if (days < 30) return say(lang, 'days_ago', { n: days });
  const s = d.toLocaleDateString(LOCALE[lang] || 'en-GB', { day: '2-digit', month: '2-digit', year: 'numeric' });
  return bare ? s : say(lang, 'on_date', { d: s });
}
// un istante in secondi epoch (mtime di un salvataggio, versione della cronologia)
function fmtAgo(epoch, lang, api) {
  if (!(epoch > 0)) return '';
  const now = nowOf(api), min = Math.round((now.getTime() - epoch * 1000) / 60000);
  if (min < 1) return say(lang, 'just_now');
  if (min < 60) return say(lang, 'min_ago', { n: min });
  if (min < 24 * 60 && dayStart(new Date(epoch * 1000)) === dayStart(now)) return say(lang, 'h_ago', { n: Math.round(min / 60) });
  return fmtDay(ymdOf(new Date(epoch * 1000)), lang, api);
}
function fmtIdle(days, lang) {
  const d = Math.round(days);
  if (d < 14) return say(lang, 'idle_days', { n: d });
  if (d < 60) return say(lang, 'idle_weeks', { n: Math.round(d / 7) });
  return say(lang, 'idle_months', { n: Math.round(d / 30) });
}
// "1-2", "2", "1-4" -> il massimo dei giocatori; 0 se la scheda non lo dice
function maxPlayers(p) { const n = String(p || '').match(/\d+/g); return n ? Math.max(...n.map(Number)) : 0; }
function clip(s, n) { s = String(s || '').replace(/\s+/g, ' ').trim(); return s.length > n ? s.slice(0, n).replace(/\s+\S*$/, '') + '…' : s; }

function mk(reply, extra) {
  return Object.assign({ reply, tier: 'command', action: 'answer', intent: 'console', domain: 'console',
    confidence: 90, local: true, skill: true, trace: 'NucleoOS · console' }, extra || {});
}

// ---- riconoscimento ----------------------------------------------------------------------------------
// Ogni gruppo di frasi è nelle 5 lingue, nell'ordine it · en · es · fr · de, già "piegate" da fold().
const OPEN_VERB = new RegExp('^(' + [
  'apri|aprimi|apra|avvia|avviami|lancia|lanciami|esegui|mostra|mostrami|fammi vedere|vai a|vai su|vai in|portami a|portami su',
  'open|start|launch|run|show me|show|go to|take me to',
  'abre|abreme|abrir|inicia|iniciar|lanza|ejecuta|muestra|muestrame|ve a|llevame a',
  'ouvre|ouvrir|lance|lancer|demarre|demarrer|affiche|montre moi|montre|va a|va sur|emmene moi',
  'offne|offnen|oeffne|starte|starten|zeig mir|zeige|zeig|gehe zu|geh zu'].join('|') + ')\\b');
const PLAY_VERB = new RegExp('^(' + [
  'gioca|giochiamo|giocare|fammi giocare|voglio giocare|vorrei giocare|metti su|avvia il gioco|lancia il gioco|apri il gioco',
  'play|lets play|let s play|i want to play|start the game|launch the game|open the game',
  'juega|jugar|juguemos|quiero jugar|me gustaria jugar|vamos a jugar|pon el juego',
  'joue|jouer|jouons|je veux jouer|j ai envie de jouer|lance le jeu',
  'spiele|lass uns|ich will|ich mochte|ich moechte|starte das spiel'].join('|') + ')\\b');

// Genere e numero di giocatori chiesti in un consiglio ("un gioco di corse in due"). Il secondo pattern è il
// campo genre delle schede (in inglese, dagli scraper), il primo il modo di dirlo nelle 5 lingue.
const GENRES = [
  ['racing', /\b(corse|corsa|guida|macchine|racing|race|driving|kart|carreras|coches|course|courses|voitures|rennspiele?|rennen|autorennen)\b/, /rac|driv|kart/],
  ['fighting', /\b(picchiaduro|combattimento|lotta|fighting|fighter|fight|lucha|peleas|combate|combat|baston|kampfspiele?|prugel\w*|beat em up)\b/, /fight|versus|beat/],
  ['platform', /\b(piattaforme?|platform|platformer|plataformas|plateformes?|jump n run|jump and run)\b/, /platform/],
  ['rpg', /\b(rpg|gdr|ruolo|jrpg|role playing|rol|jeu de role|jeux de role|rollenspiele?)\b/, /rpg|role/],
  ['shooter', /\b(sparatutto|shooter|shoot em up|shmup|navicelle|disparos|naves|tir|shoot them up|ballerspiele?)\b/, /shoot|shmup|gun/],
  ['puzzle', /\b(puzzle|rompicapo|rompicapi|puzles|rompecabezas|reflexion|ratsel\w*|denkspiele?)\b/, /puzzle/],
  ['sports', /\b(sport|sportivi|calcio|tennis|basket|golf|soccer|football|sports|deportes|futbol|sportspiele?|fussball)\b/, /sport|soccer|football|tennis|basket|golf|hockey/],
  ['adventure', /\b(avventura|avventure|adventure|aventura|aventure|abenteuer\w*)\b/, /adventure/],
  ['strategy', /\b(strategia|strategico|strategy|tattico|estrategia|strategie|strategiespiele?)\b/, /strateg|tactic/],
];
function parseFilters(f) {
  const out = {};
  for (const [id, said, field] of GENRES) if (said.test(f)) { out.genre = id; out.genreRe = field; }
  if (/\b(in due|in 2|2 giocatori|due giocatori|con un amico|multiplayer|multigiocatore|cooperativ\w*|co op|coop|two players?|2 players?|with a friend|for two|a dos|para dos|dos jugadores|2 jugadores|con un amigo|a deux|2 joueurs|deux joueurs|avec un ami|zu zweit|2 spieler|zwei spieler|mit einem freund)\b/.test(f)) out.players = 2;
  return out;
}
const SUGGEST = new RegExp('\\b(' + [
  'cosa (posso |potrei )?(giocare|gioco)|a cosa gioco|che gioco mi (consigli|suggerisci)|non so a cosa giocare',
  'what should i play|what can i play|what to play|something to play',
  'a que (puedo )?(jugar|juego)|que juego me (recomiendas|sugieres)|no se a que jugar|algo para jugar',
  'a quoi (je peux |puis je )?jouer|quel jeu (me )?(conseilles|recommandes)|je ne sais pas a quoi jouer|quelque chose a jouer',
  'was soll ich spielen|was kann ich spielen|nicht was ich spielen soll|etwas zum spielen'].join('|') + ')\\b');
const ADVICE_VERB = /\b(consigliami|suggeriscimi|proponimi|consigli|suggerisci|recommend|suggest|recomiendame|sugiereme|recomienda|conseille moi|recommande moi|conseille|recommande|empfiehl|schlag mir)\b/;
const GAME_WORD = /\b(gio\w*|games?|videogames?|juegos?|videojuegos?|jeux|jeu|spiele?|videospiele?)\b/;
const NOWPLAYING = new RegExp('\\b(' + [
  'a cosa sto giocando|cosa sto giocando|che gioco (e|c e) (aperto|in corso)|c e (un|qualche) gioco (aperto|in corso)|gioco in corso|partita in corso',
  'what am i playing|what game is (running|open)|is a game (running|open)|game in progress',
  'a que estoy jugando|que estoy jugando|hay (algun|un) juego abierto|juego en curso',
  'a quoi je joue|a quoi suis je en train de jouer|quel jeu est ouvert|y a t il un jeu (ouvert|en cours)|jeu en cours',
  'was spiele ich gerade|welches spiel (lauft|ist offen|ist geoffnet)|lauft (gerade )?ein spiel'].join('|') + ')\\b');
const RECENT = new RegExp('\\b(' + [
  'a cosa stavo giocando|cosa stavo giocando|ultimo gioco|ultima partita|riprendi (la )?partita|continua (a giocare|la partita)',
  'what was i playing|last game|resume (my )?game|continue (my )?game',
  'a que estaba jugando|ultimo juego|ultima partida|continuar (la |mi )?partida|retomar (la |mi )?partida',
  'a quoi je jouais|dernier jeu|derniere partie|reprendre (la |ma )?partie',
  'was habe ich zuletzt gespielt|letztes spiel|zuletzt gespielt|weiterspielen'].join('|') + ')\\b');
const PLAYTIME = new RegExp('\\b(' + [
  'quanto (tempo )?ho giocato|quante (ore|partite) ho (giocato|fatto)|quante volte ho giocato|ore di gioco|tempo di gioco|tempo giocato|statistiche (di gioco|dei giochi|dei miei giochi)|giochi piu giocati|gioco piu giocato|a cosa gioco di piu',
  'how long have i played|how much have i played|how many hours have i played|how many times have i played|play ?time|most played|gaming stats|play stats',
  'cuanto (tiempo )?he jugado|cuantas horas he jugado|cuantas veces he jugado|tiempo de juego|juegos mas jugados|juego mas jugado|estadisticas de juego',
  'combien (de temps )?j ai joue|combien d heures j ai joue|combien de fois j ai joue|temps de jeu|jeux les plus joues|jeu le plus joue|statistiques de jeu',
  'wie lange habe ich|wie viele stunden habe ich|wie oft habe ich|spielzeit|meistgespielte?n?|am meisten gespielt|spielstatistik\\w*'].join('|') + ')\\b');
// "<verbo> X"  e  "what is X about" / "wer hat X gemacht" (il nome sta in mezzo)
const INFO_HEAD = new RegExp('^(' + [
  'di cosa parla|di che parla|di cosa tratta|parlami (di|del|dello|della|dei)|dimmi (qualcosa )?(di|su|sul|sulla|del)|info (su|di|sul|sulla)|informazioni (su|di|sul|sulla)|chi ha (fatto|sviluppato|creato|pubblicato)|quando e uscito|in che anno e uscito|che genere e|trama di',
  'tell me about|who made|who developed',
  'de que (va|trata)|hablame (de|del|sobre)|dime algo (de|del|sobre)|informacion (sobre|de)|quien (hizo|desarrollo|creo)|cuando salio|en que ano salio',
  'de quoi parle|parle moi (de|du|des)|dis moi quelque chose sur|infos? sur|informations sur|qui a (fait|developpe|cree)|quand est sorti|en quelle annee est sorti',
  'worum geht es in|erzahl mir (von|etwas uber)|infos? (zu|uber)|informationen (zu|uber)'].join('|') + ')\\b');
const INFO_MID = /^(?:what is|whats|what s) (.+) about$|^when did (.+) come out$|^when was (.+) released$|^wer hat (.+) (?:gemacht|entwickelt)$|^wann (?:ist|kam) (.+) (?:erschienen|raus)$|^was ist (.+) fur ein spiel$/;
const SAVES_WORD = '(salvataggi|salvataggio|saves?|savestates?|stati salvati|partite salvate|partidas guardadas|partidas|guardados|sauvegardes?|spielstande?|speicherstande?)';
const SAVES_OF = new RegExp('\\b' + SAVES_WORD + '\\s+(di|del|dello|della|dei|degli|delle|per|a|for|of|in|de|del|para|du|des|pour|von|fur|in)\\s+(.+)$');
const SAVES_ANY = new RegExp('\\b' + SAVES_WORD.replace(/\|partidas\|/, '|') + '\\b|\\b(salvare|salvo|guardar partida|sauvegarder|speichern)\\b');
const TAIL = /(?:^|\s+)(in tutto|in totale|finora|fino ad ora|sulla console|questa settimana|questo mese|in total|so far|on the console|this week|this month|en total|hasta ahora|en la consola|au total|jusqu ici|sur la console|insgesamt|bisher|auf der konsole)$/;
const cutTail = (s) => { let x = s, y; while ((y = x.replace(TAIL, '')) !== x) x = y; return x; };
const NOT_A_GAME_REST = /^(in due|in 2|in coppia|con un amico|with a friend|together|in multiplayer|a dos|con un amigo|a deux|avec un ami|zu zweit|mit einem freund)$/;

// ---- trucchi: la frase ----------------------------------------------------------------------------
const CHEAT_RE = /\b(trucchi|trucco|cheats?|cheat codes?|trucos?|trampas|codes? de triche|triche|cheatcodes?|schummelcodes?)\b/;
const CHEAT_ON = /^(attiva|attivami|abilita|accendi|metti|enable|turn on|activate|switch on|activa|habilita|pon|active|activer|aktiviere|aktivieren|schalte)\b/;
const CHEAT_OFF = /^(disattiva|disabilita|togli|spegni|rimuovi|disable|turn off|deactivate|switch off|remove|desactiva|deshabilita|quita|desactive|desactiver|enleve|retire|deaktiviere|deaktivieren|entferne)\b/;
const CHEAT_HOW = /^(come|dove|how|where|como|donde|comment|ou|wie|wo|cosa sono|what are|que son|c est quoi|was sind)\b/;
// le preposizioni che separano "quale trucco" da "in quale gioco", nelle 5 lingue
const JOIN = new Set('in per su di del della dello dei degli for of on en para de del dans pour du des sur fur von bei zu im'.split(' '));
const ALL_WORD = /\b(tutti|tutte|all|todos|todas|tous|toutes|alle)\b/;
function splitJoin(rest) {
  const w = words(rest);
  if (!w.length) return { what: '', game: '' };
  if (JOIN.has(w[0])) return { what: '', game: strip(w.slice(1).join(' ')) };
  const i = w.findIndex((x, k) => k > 0 && JOIN.has(x));
  return i > 0 ? { what: w.slice(0, i).join(' '), game: strip(w.slice(i + 1).join(' ')) } : { what: rest, game: '' };
}
function parseCheat(f) {
  // "code" da solo è anche il codice da scrivere: vale come trucco solo dopo attiva/disattiva ("active le code …")
  const cm = f.match(CHEAT_RE) || ((CHEAT_ON.test(f) || CHEAT_OFF.test(f)) && f.match(/\bcodes?\b/));
  if (!cm || /\b(file|nota|documento|fichier|datei)\b/.test(f)) return null;
  // "trucco" è anche il trucco del viso, "truco"/"trampas" un trucco qualsiasi: valgono solo se si parla di giochi
  if (/^(trucco|truco|trampas)$/.test(cm[0]) && !CHEAT_ON.test(f) && !CHEAT_OFF.test(f) && !GAME_WORD.test(f.replace(cm[0], ''))) return null;
  if (CHEAT_HOW.test(f)) return { kind: 'kb', id: 'cheats_help' };
  const on = CHEAT_ON.test(f), off = !on && CHEAT_OFF.test(f);
  const before = f.slice(0, cm.index).replace(CHEAT_ON, '').replace(CHEAT_OFF, '').trim();
  const sp = splitJoin(f.slice(cm.index + cm[0].length).trim());
  const what = cleanWhat(sp.what || before);
  if (on || off) return { kind: 'cheat_set', on, off, what, game: sp.game, all: ALL_WORD.test(f) };
  return { kind: 'cheats', game: sp.game || strip(sp.what) || strip(before) };
}
// "il trucco delle vite infinite" -> "vite infinite": via articoli, la parola trucco e "tutti"
function cleanWhat(s) {
  return strip(words(s).filter((x) => !CHEAT_RE.test(x) && !ALL_WORD.test(x) && !FILLER.has(x)).join(' '));
}

export function parseIntent(q) {
  const f = fold(q);
  if (!f) return null;
  const isRomTalk = has(f, /\b(rom|roms|gioc\w*|game|games|juegos?|jeux|jeu|spiele?|bios|salvat\w*|save\w*|partidas?|sauvegardes?|spielstande?)\b/);

  // il gioco aperto ADESSO sulla console (prima di "a cosa stavo giocando": sto non è stavo)
  if (NOWPLAYING.test(f)) return { kind: 'nowplaying' };
  // trucchi: elenco, accendi, spegni ("trucchi per contra", "attiva il trucco vite infinite in contra")
  const ch = parseCheat(f);
  if (ch) return ch;
  // quali port ho / che emulatore usa la snes
  if (/\b(port|ports)\b/.test(f) && !/\bport ?master\b/.test(f) &&
      /\b(quali|che|quanti|elenco|lista|mostra\w*|which|what|how many|list|show|cuales|que|cuantos|muestra\w*|quels|quelles|combien|liste|montre\w*|welche|wie viele|zeig\w*)\b/.test(f) &&
      !/^(come|how|como|comment|wie|cosa sono|what are|que son|c est quoi|was sind)\b/.test(f))
    return { kind: 'ports' };
  if (/\b(che|quale|quali) (emulator\w*|core)\b|\b(which|what) (emulator|core)s?\b|\bque (emulador|core)(es)?\b|\bquels? (emulateur|core)s?\b|\bwelche[nrs]? (emulator|core)(en)?\b/.test(f) &&
      !/\b(cambi\w*|scegli\w*|change|choose|switch|cambiar|elegir|changer|choisir|wechseln|andern)\b/.test(f))
    return { kind: 'core', q: f };
  // tempo giocato, in tutto o per un gioco ("quanto ho giocato a zelda", "wie lange habe ich zelda gespielt")
  const tm = f.match(PLAYTIME);
  if (tm) {
    const rest = strip(cutTail(f.slice(tm.index + tm[0].length).trim()));
    return { kind: 'playtime', q: rest && !/^(giochi|games|juegos|jeux|spiele|insgesamt|in total|en total|au total)$/.test(rest) ? rest : '' };
  }
  // consigli ("cosa gioco?", "un gioco di corse in due") - prima di "gioca a X": "un gioco di corse" non è un titolo
  const pm = f.match(PLAY_VERB);
  const after = pm ? f.slice(pm[0].length) : '';
  const vague = pm && /^\s*(a |ad |to |al |a un |un |une |ein |etwas )?(qualcosa|something|un gioco|a game|una partita|algo|un juego|quelque chose|un jeu|etwas|ein spiel)\b/.test(after);
  const askAdvice = ADVICE_VERB.test(f) && (GAME_WORD.test(f) || Object.keys(parseFilters(f)).length > 0);
  if (SUGGEST.test(f) || askAdvice || vague) return Object.assign({ kind: 'suggest' }, parseFilters(f));

  // gioca a X  (prima di "apri": "apri il gioco X" è un gioco)
  if (pm) {
    const rest = strip(after);
    // "fammi giocare in due": non è un titolo, è un consiglio con un filtro
    if (NOT_A_GAME_REST.test(rest)) return Object.assign({ kind: 'suggest' }, parseFilters(f));
    if (rest && !/^(spielen|jugar|jouer)$/.test(rest)) return { kind: 'play', q: rest };
  }
  // la scheda di un gioco ("di cosa parla zelda", "chi ha fatto metroid", "wer hat metroid gemacht")
  const ih = f.match(INFO_HEAD), imid = !ih && f.match(INFO_MID);
  if (ih || imid) {
    const raw = ih ? f.slice(ih[0].length) : imid.slice(1).find(Boolean);
    const name = strip(cutTail(raw.trim()));
    if (name) return { kind: 'info', q: name, game: GAME_WORD.test(f) };
  }
  // i salvataggi di un gioco preciso
  const sm = f.match(SAVES_OF);
  if (sm && !has(f, /\b(file|nota|note|documento|documento|fichier|datei)\b/)) {
    const name = strip(cutTail(sm[3]));
    if (name) return { kind: 'saves', q: name };
  }
  // continua / a cosa stavo giocando
  if (RECENT.test(f)) return { kind: 'recent' };
  // BIOS
  if (has(f, /\bbios\b/)) return { kind: 'bios', q: f };
  // come carico le rom
  if (has(f, /\b(come|dove|how|where|como|donde|comment|ou|wie|wo)\b/) &&
      has(f, /\b(carico|caricare|metto|mettere|aggiungo|aggiungere|copio|copiare|installo|installare|trasferisco|trasferire|add|copy|load|put|install|transfer|cargo|cargar|pongo|poner|anado|anadir|copiar|instalo|instalar|mettre|mets|ajouter|ajoute|copier|installer|charger|kopiere|kopieren|lade|laden|installiere|installieren|hinzufugen|spiele ich)\b/) &&
      has(f, /\b(rom|roms|giochi|gioco|games|game|juegos|juego|jeux|jeu|spiele|spiel)\b/))
    return { kind: 'addroms' };
  // salvataggi (in generale)
  if (SAVES_ANY.test(f) && !has(f, /\b(file|nota|note|documento|fichier|datei)\b/)) return { kind: 'saves' };
  // diagnosi
  if (has(f, new RegExp('\\b(' + [
    'cosa non va|che problemi ha|ci sono problemi|controlla la console|controlla il sistema|fai (una|la) diagnosi|diagnosi|diagnostica|check ?up|stato del sistema|nucleos doctor',
    'what s wrong|whats wrong|any problems|check the console|diagnose|system check',
    'que le pasa a la consola|hay algun problema|revisa la consola|diagnostico',
    'qu est ce qui ne va pas avec la console|y a t il des problemes|verifie la console|diagnostic',
    'was stimmt nicht mit der konsole|gibt es probleme|prufe die konsole|uberprufe die konsole|diagnose|systemcheck'].join('|') + ')\\b')))
    return { kind: 'doctor' };
  // batteria (non "carica una rom")
  if (!isRomTalk && (has(f, /\b(batteria|battery|autonomia|bateria|batterie|autonomie|akku|ladestand)\b/) ||
      has(f, /\b(quanta|livello|percentuale|how much)\b.*\b(carica|charge)\b/) ||
      has(f, /\b(e|sta) in carica\b|\bis it charging\b|\besta cargando\b|\best elle en charge\b|\bladt (sie|es)\b/)))
    return { kind: 'battery' };
  // spazio
  if (!has(f, /\b(spazio(si)? tra|space between|espacio entre|espace entre)\b/) &&
      (has(f, /\b(quanto|quanta|how much|cuanto|combien|wie viel)\b.*\b(spazio|memoria|space|storage|espacio|espace|speicher|platz)\b/) ||
       has(f, /\b(spazio|space|storage|espacio|espace|speicher|platz)\b.*\b(libero|liberi|rimasto|rimane|free|left|disponibile|available|libre|libres|disponible|frei)\b/) ||
       has(f, /\b(scheda|sd|disco|tarjeta|carte|karte) (piena|full|llena|pleine|voll)\b/)))
    return { kind: 'storage' };
  // rete
  if (has(f, new RegExp('\\b(' + [
    'indirizzo ip|il mio ip|che ip|sono connesso|sei connessa|connesso a internet|quale wifi|che rete',
    'ip address|my ip|what ip|connected to|which wifi|what network',
    'direccion ip|mi ip|que ip|estoy conectado|a que wifi|que red',
    'adresse ip|mon ip|quelle ip|suis je connecte|quel wifi|quel reseau',
    'ip adresse|meine ip|welche ip|bin ich verbunden|welches wlan|welches netz'].join('|') + ')\\b')))
    return { kind: 'network' };
  // ora / data
  if (has(f, /^(che ora e|che ore sono|che ora sono|ora attuale|dimmi l ora|what time is it|what s the time|whats the time|current time|que hora es|quelle heure est il|il est quelle heure|wie spat ist es|wie viel uhr ist es)$/))
    return { kind: 'time' };
  if (has(f, /^(che giorno e( oggi)?|che data e( oggi)?|quanti ne abbiamo( oggi)?|data di oggi|what day is it( today)?|what s the date|whats the date|today s date|todays date|que dia es hoy|que fecha es( hoy)?|a cuantos estamos|quel jour sommes nous|quel jour on est|quelle est la date|welcher tag ist heute|welches datum ist heute|der wievielte ist heute)$/))
    return { kind: 'date' };
  // versione
  if (has(f, /\b(che versione|quale versione|versione di nucleoos|versione del sistema|what version|which version|nucleoos version|que version|quelle version|welche version)\b/))
    return { kind: 'version' };
  // apri X
  const om = f.match(OPEN_VERB);
  if (om) {
    const rest = strip(f.slice(om[0].length));
    if (rest) return { kind: 'open', q: rest };
  }
  return null;
}

// L'app per un nome: alias esatto del registro, o la sequenza più lunga di parole che coincide con un alias.
export function matchApp(name, aliases) {
  const f = fold(name);
  if (!f || !aliases) return null;
  let best = null;
  for (const [id, list] of Object.entries(aliases)) {
    for (const a0 of [id.replace(/-/g, ' '), ...(list || [])]) {
      const a = fold(a0);
      if (!a) continue;
      const exact = f === a;
      const inside = !exact && (' ' + f + ' ').includes(' ' + a + ' ');
      if (!exact && !inside) continue;
      const score = (exact ? 1000 : 0) + a.length;
      if (!best || score > best.score) best = { id, alias: a, score, exact };
    }
  }
  // "dentro" vale solo se l'alias copre gran parte della frase: "apri la musica di ieri" non è l'app Note
  if (best && !best.exact && best.alias.length < f.length * 0.5) return null;
  return best;
}

// ---- abilità -----------------------------------------------------------------------------------------
async function safe(fn) { try { return await fn(); } catch (e) { return { __err: e && (e.status || e.message) || 'error' }; } }
const failed = (x) => !x || x.__err;
const unreachable = (L, x) => L(x && x.__err === 401 ? 'unreach_pin' : 'unreach');
const named = (g) => '**' + g.name + '** (' + (g.sysname || g.system) + ')';
function gameCard(g) { return { kind: 'game', arg: g.system + '/' + g.file, title: g.name, sub: g.sysname || g.system }; }

async function battery(L, api) {
  const s = await safe(() => api.status());
  const x = { intent: 'battery', action: 'system', arg: 'battery' };
  if (failed(s)) return mk(unreachable(L, s), x);
  const b = s.battery || {};
  if (!b.present || !(b.pct >= 0)) return mk(L('bat_na'), x);
  // plugged senza charging: alimentatore attaccato, batteria piena (o il caricatore in pausa)
  let r = L(b.charging ? 'bat_charging' : b.plugged ? (b.status === 'Full' ? 'bat_full' : 'bat_plugged') : 'bat', { p: b.pct });
  if (!b.charging && !b.plugged && b.pct <= 15) r += ' ' + L('bat_low');
  return mk(r, Object.assign(x, { value: b.pct }));
}

async function storage(L, api) {
  const s = await safe(() => api.status());
  const x = { intent: 'storage', action: 'system', arg: 'storage' };
  if (failed(s)) return mk(unreachable(L, s), x);
  const c = s.cards || {};
  // La cartella media è una SECONDA scheda solo se sta davvero sulla SD2 (media_src = sd2). Con una
  // scheda sola sta sulla card delle ROM (media_src = roms): stessa scheda, stesso spazio, e contarla
  // due volte farebbe credere all'utente di avere una seconda scheda che non c'è.
  const ent = (c.entries || []).filter((e) => e.id === 'roms' || (e.id === 'media' && (c.media_src === 'sd2' || !c.media_src)));
  const lines = [], seen = new Set();
  for (const e of ent) {
    const key = e.total_bytes + ':' + e.free_bytes;    // stesso filesystem visto da due percorsi
    if (seen.has(e.path) || seen.has(key)) continue;
    seen.add(e.path); seen.add(key);
    lines.push('- ' + L(e.id === 'roms' ? 'card_games' : 'card_media') + ': ' + L('free_of', { f: fmtBytes(e.free_bytes), t: fmtBytes(e.total_bytes) }));
  }
  if (!lines.length && s.storage) lines.push('- ' + L('free_of', { f: fmtBytes(s.storage.free_bytes), t: fmtBytes(s.storage.total_bytes) }));
  if (!lines.length) return mk(L('space_na'), x);
  let tail = '';
  if (c.sd2_state === 'empty') tail = '\n\n' + L('sd2_empty');
  if (c.warn) tail += '\n\n⚠ ' + c.warn;
  return mk(L('space_head') + '\n' + lines.join('\n') + tail, x);
}

async function network(L, api) {
  const s = await safe(() => api.status());
  const x = { intent: 'network', action: 'system', arg: 'network' };
  if (failed(s)) return mk(unreachable(L, s), x);
  const n = s.network || {};
  let r = n.mode === 'sta' ? L(n.ip ? 'net_wifi_ip' : 'net_wifi', { ssid: n.ssid, ip: n.ip })
        : n.mode === 'usb' ? L('net_usb', { ip: n.ip }) : L('net_none');
  if (n.mode && n.mode !== 'none' && n.time_synced === false) r += ' ' + L('net_unsynced');
  return mk(r, x);
}

function timeOf(L, api, lang, kind) {
  const d = nowOf(api), loc = LOCALE[lang] || 'en-GB';
  if (kind === 'time') return mk(L('time_is', { t: d.toLocaleTimeString(loc, { hour: '2-digit', minute: '2-digit' }) }), { intent: 'time', action: 'system', arg: 'time' });
  return mk(L('date_is', { d: d.toLocaleDateString(loc, { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' }) }), { intent: 'date', action: 'system', arg: 'date' });
}

async function version(L, api) {
  const s = await safe(() => api.status());
  if (failed(s) || !s.version) return mk(unreachable(L, s), { intent: 'version' });
  return mk(L('version', { v: s.version }), { intent: 'version' });
}

async function play(L, api, want) {
  const res = await safe(() => api.search(want, 5));
  if (failed(res)) return mk(unreachable(L, res), { intent: 'play', action: 'game' });
  const g = res.games || [];
  if (!g.length) return mk(L('play_none', { q: want }), { intent: 'play', action: 'game', confidence: 60 });
  const top = g[0];
  // Un titolo che risponde meglio degli altri si nomina; gli altri restano come schede sotto, perché
  // "zelda" può voler dire Zelda II come Minish Cap e scegliere al posto dell'utente è peggio che chiedere.
  const strong = g.length === 1 || (g[1] && g[1].score < top.score);
  const reply = strong ? L('play_found', { g: named(top) }) + (g.length > 1 ? ' ' + L('play_more') : '')
                       : L('play_pick', { n: Math.min(g.length, 3) });
  return mk(reply, { intent: 'play', action: 'game', arg: top.system + '/' + top.file, cards: g.slice(0, 3).map(gameCard) });
}

async function recent(L, api) {
  const res = await safe(() => api.recent(3));
  if (failed(res)) return mk(unreachable(L, res), { intent: 'recent', action: 'game' });
  const g = res.games || [];
  if (!g.length) return mk(L('recent_none'), { intent: 'recent', action: 'game' });
  return mk(L('recent_last', { g: named(g[0]) }), { intent: 'recent', action: 'game', arg: g[0].system + '/' + g[0].file, cards: g.slice(0, 3).map(gameCard) });
}

async function bios(L, api, f) {
  const res = await safe(() => api.bios());
  if (failed(res)) return mk(unreachable(L, res), { intent: 'bios' });
  const sys = res.systems || [];
  // un sistema nominato nella domanda ("bios della playstation", "bios psx")
  const named0 = sys.filter((x) => {
    const n = fold(x.name), id = fold(x.system);
    return (' ' + f + ' ').includes(' ' + id + ' ') || (n && f.includes(n)) || n.split(' ').some((w) => w.length >= 5 && f.includes(w));
  });
  const files = (x) => x.missing.map((m) => '`' + m.path + '`').join(', ');
  const where = L('bios_where', { dir: res.dir });
  if (named0.length) {
    const lines = named0.map((x) => '- **' + x.name + '**: ' + (
      x.status === 'missing' ? L('bios_missing', { f: files(x) })
      : x.status === 'unknown' ? L('bios_unknown', { c: x.core })
      : x.status === 'none' ? L('bios_none')
      : L('bios_ok') + (x.optional_missing ? ' ' + L('bios_opt') : '')));
    return mk(lines.join('\n') + (named0.some((x) => x.status === 'missing') ? '\n\n' + where : ''), { intent: 'bios' });
  }
  const pick = sys.filter((x) => x.status === 'missing');
  if (!pick.length) return mk(L('bios_all_ok'), { intent: 'bios' });
  return mk(L('bios_head') + '\n' + pick.map((x) => '- **' + x.name + '**: ' + files(x)).join('\n') + '\n\n' + where, { intent: 'bios' });
}

async function doctor(L, api) {
  const d = await safe(() => api.doctor());
  if (failed(d)) return mk(unreachable(L, d), { intent: 'doctor' });
  if (!d.available) return mk(L('doc_na'), { intent: 'doctor' });
  if (d.ok) return mk(L('doc_ok'), { intent: 'doctor' });
  const items = [], lines = String(d.report || '').split('\n');
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(/^\s*\d+\)\s+(.*)$/);
    if (!m) continue;
    const fix = (lines[i + 1] || '').match(/^\s*→\s*(.*)$/);
    items.push('- ' + m[1] + (fix ? '\n  ' + L('doc_fix') + ': `' + fix[1] + '`' : ''));
  }
  if (!items.length) return mk(L('doc_unreadable'), { intent: 'doctor' });
  return mk(L(items.length === 1 ? 'doc_found_1' : 'doc_found_n', { n: items.length }) + '\n' + items.join('\n'), { intent: 'doctor' });
}

async function openApp(L, api, want) {
  const al = await safe(() => api.aliases());
  const m = matchApp(want, failed(al) ? null : (al.aliases || al));
  if (m) {
    let name = m.id;
    if (api.apps) {
      const ap = await safe(() => api.apps());
      const hit = !failed(ap) && (ap.apps || []).find((a) => a.id === m.id);
      if (hit && hit.name) name = hit.name;
    }
    return mk(L('opening', { a: name }), { intent: 'open_app', action: 'launch', arg: m.id, domain: 'app' });
  }
  // "apri zelda": non è un'app, forse è un gioco - ma solo se il titolo risponde bene
  const res = await safe(() => api.search(want, 3));
  const g = failed(res) ? [] : (res.games || []);
  if (g.length && g[0].score >= 60) return play(L, api, want);
  return null;                              // né app né gioco: decide il motore normale
}

// ---- il gioco, con la sua scheda e le sue statistiche ------------------------------------------------
// La ricerca dà il titolo; la scheda completa (descrizione, genere, partite, tempo giocato) sta nella
// lista del suo sistema. NON nei recenti: sul R36S /api/library/recent scorre tutti i sistemi e costa
// ~3,3 s, la lista di un sistema ~0,15 s (misurato il 20/10/2026). `min` = punteggio minimo perché il
// titolo valga come "quello che intendevi".
async function findGame(api, want, min) {
  const res = await safe(() => api.search(want, 5));
  if (failed(res)) return { err: res };
  const g = res.games || [];
  if (!g.length || g[0].score < (min || 0)) return { list: g };
  const top = g[0];
  let detail = null;
  if (api.games) {
    const all = await safe(() => api.games(top.system));
    if (!failed(all)) detail = (all.games || []).find((x) => x.file === top.file) || null;
  }
  if (!detail && api.recent) {                       // niente lista per sistema: ripiego sui recenti
    const rec = await safe(() => api.recent(200));
    if (!failed(rec)) detail = (rec.games || []).find((x) => x.system === top.system && x.file === top.file) || null;
  }
  return { top: Object.assign({}, top, detail || {}, { system: top.system, file: top.file, sysname: top.sysname || top.system }), list: g };
}
function statsLine(L, lang, g, api) {
  if (!(g.plays > 0)) return L('played_never');
  const last = fmtDay(g.last, lang, api);
  return L('played', { times: g.plays === 1 ? L('played_once') : L('played_n', { n: g.plays }),
    dur: g.secs > 0 ? L('played_dur', { d: fmtDur(g.secs, lang) }) : '', last: last ? L('played_last', { w: last }) : '' });
}

async function nowPlaying(L, lang, api) {
  const s = await safe(() => api.gamemode());
  const x = { intent: 'nowplaying', action: 'game' };
  if (failed(s)) return mk(unreachable(L, s), x);
  if (!s.gaming) return mk(L('none_open'), x);
  const g = s.game || {};
  const secs = s.since > 0 ? (nowOf(api).getTime() / 1000 - s.since) : 0;
  const since = secs > 0 ? L('since', { d: fmtDur(secs, lang) }) : '';
  if (!g.name) return mk(L('playing_noname', { since }), x);
  return mk(L('playing', { g: named(g), since }), Object.assign(x, { arg: g.system + '/' + g.file, value: Math.round(secs) }));
}

async function playtime(L, lang, api, want) {
  if (want) {
    const r = await findGame(api, want, 40);
    if (r.err) return mk(unreachable(L, r.err), { intent: 'playtime' });
    if (!r.top) return mk(L('not_in_lib', { q: want }), { intent: 'playtime', confidence: 60 });
    const g = r.top;
    // dopo i due punti la frase continua in minuscolo; in tedesco no (lì la maiuscola è grammatica)
    const line = statsLine(L, lang, g, api);
    return mk(named(g) + ': ' + (lang === 'de' ? line : line.charAt(0).toLowerCase() + line.slice(1)), { intent: 'playtime', action: 'game', arg: g.system + '/' + g.file, value: g.secs || 0, cards: [gameCard(g)] });
  }
  const res = await safe(() => api.recent(200));
  if (failed(res)) return mk(unreachable(L, res), { intent: 'playtime' });
  const g = (res.games || []).filter((x) => x.plays > 0);
  if (!g.length) return mk(L('pt_nothing'), { intent: 'playtime' });
  const total = g.reduce((a, x) => a + (x.secs || 0), 0);
  const plays = g.reduce((a, x) => a + (x.plays || 0), 0);
  const top = g.slice().sort((a, b) => (b.secs || 0) - (a.secs || 0) || b.plays - a.plays).slice(0, 3);
  const sys = {};
  for (const x of g) sys[x.sysname || x.system] = (sys[x.sysname || x.system] || 0) + (x.secs || 0);
  const fav = Object.entries(sys).sort((a, b) => b[1] - a[1])[0];
  const lines = top.map((x) => '- ' + named(x) + ': ' + fmtDur(x.secs, lang) + ' · ' + L(x.plays === 1 ? 'sessions_1' : 'sessions_n', { n: x.plays }));
  return mk(L('pt_total', { d: fmtDur(total, lang), s: plays, g: g.length }) + (fav && fav[1] > 0 ? ' ' + L('pt_fav', { s: fav[0] }) : '') +
            '\n\n' + L('pt_top') + '\n' + lines.join('\n'), { intent: 'playtime', value: total, cards: top.map(gameCard) });
}

async function info(L, lang, api, want, saidGame) {
  // senza "gioco" nella frase vale solo un titolo che risponde bene: "parlami di te" non è un gioco
  if (!saidGame && fold(want).length < 4) return null;
  const r = await findGame(api, want, saidGame ? 40 : 60);
  if (r.err) return saidGame ? mk(unreachable(L, r.err), { intent: 'gameinfo' }) : null;
  if (!r.top) return null;                                // non è nella libreria: lo sa (forse) il modello
  const g = r.top;
  if (!saidGame && !(' ' + fold(g.name) + ' ').includes(' ' + fold(want) + ' ')) return null;
  const meta = [g.year, g.genre, g.dev || g.pub, maxPlayers(g.players) > 1 ? L('up_to_players', { n: maxPlayers(g.players) }) : ''].filter(Boolean).join(' · ');
  const reply = named(g) + (meta ? '\n' + meta : '') + '\n\n' + (g.desc ? clip(g.desc, 520) : L('no_desc')) + '\n\n' + statsLine(L, lang, g, api);
  return mk(reply, { intent: 'gameinfo', action: 'game', arg: g.system + '/' + g.file, cards: [gameCard(g)] });
}

async function savesOf(L, lang, api, want, general) {
  const r = await findGame(api, want, 40);
  if (r.err) return mk(unreachable(L, r.err), { intent: 'saves' });
  if (!r.top) return general();                          // "salvataggi di ieri": non è un gioco, risposta generale
  const g = r.top;
  const h = await safe(() => api.saveHistory(g.system, g.file));
  if (failed(h)) return mk(unreachable(L, h), { intent: 'saves', arg: g.system + '/' + g.file });
  const files = (h.files || []).filter((x) => x.exists || (x.versions || []).length);
  const card = Object.assign(gameCard(g), { open: L('saves_btn') });
  const x = { intent: 'saves', action: 'game', arg: g.system + '/' + g.file, cards: [card] };
  if (!files.length) return mk(L('saves_none', { g: named(g) }), x);
  let versions = 0;
  const lines = files.map((f) => {
    const v = (f.versions || []).length;
    versions += v;
    const when = f.exists ? fmtAgo(f.mtime, lang, api) : '';
    return '- ' + L(f.kind === 'state' ? 'kind_state' : 'kind_srm') + ' `' + f.name + '`' + (when ? L('saved_when', { w: when }) : L('deleted')) +
      (v ? ' · ' + L(v === 1 ? 'versions_1' : 'versions_n', { n: v }) : '');
  });
  return mk(L('saves_of', { g: named(g) }) + '\n' + lines.join('\n') + (versions ? '\n\n' + L('restore_hint') : ''), Object.assign(x, { value: versions }));
}

// Consigli dalla TUA libreria: un gioco lasciato a metà da riprendere e titoli mai provati sui sistemi che
// usi di più, filtrati per genere e giocatori quando la domanda li dice. Due/tre letture, mai la libreria
// intera: sul device ogni lista di sistema è una scansione della cartella.
async function suggest(L, lang, api, p) {
  const rnd = api.rand || Math.random;
  const rec = await safe(() => api.recent(200));
  if (failed(rec)) return mk(unreachable(L, rec), { intent: 'suggest' });
  const played = (rec.games || []).filter((x) => x.plays > 0);
  // i sistemi da cui pescare: quelli dove giochi di più, altrimenti quelli con più giochi
  const weight = {};
  for (const x of played) weight[x.system] = (weight[x.system] || 0) + (x.secs || 0) + 60 * (x.plays || 0);
  const systems = Object.keys(weight).sort((a, b) => weight[b] - weight[a]);
  if (systems.length < 2 && api.library) {
    const lib = await safe(() => api.library());
    if (!failed(lib)) for (const s of (lib.systems || []).filter((s) => s.count > 0).sort((a, b) => b.count - a.count))
      if (!systems.includes(s.id)) systems.push(s.id);
  }
  const filtered = !!(p.genre || p.players);
  const fresh = [];
  for (const sys of systems.slice(0, filtered ? 3 : 2)) {
    const all = await safe(() => api.games(sys));
    if (failed(all)) continue;
    for (const x of all.games || []) if (!(x.plays > 0)) fresh.push(Object.assign({ system: sys, sysname: all.name || sys }, x));
  }
  const ok = (x) => (!p.genreRe || p.genreRe.test(fold(x.genre))) && (!p.players || maxPlayers(x.players) >= p.players);
  // i migliori per voto, un po' mescolati: due domande uguali non danno sempre lo stesso
  const pick = (list, n) => list.slice().sort((a, b) => ((b.rating || 0) + rnd() * 25) - ((a.rating || 0) + rnd() * 25)).slice(0, n);
  const resume = filtered ? [] : played.filter((x) => ageDays(x.last, api) >= 7 && (x.secs || 0) >= 1800)
    .sort((a, b) => (b.secs || 0) - (a.secs || 0)).slice(0, 1);
  const newOnes = pick(fresh.filter(ok), 3 - resume.length);
  const oldOnes = filtered && newOnes.length < 3 ? pick(played.filter(ok), 3 - newOnes.length) : [];
  const out = resume.concat(newOnes, oldOnes);
  const what = [p.genre ? L('g_' + p.genre) : '', p.players ? L('for_two') : ''].filter(Boolean).join(' ');
  if (!out.length) return mk(filtered ? L('sg_none_what', { w: what }) : L('sg_empty'), { intent: 'suggest', confidence: 60 });
  const line = (x) => {
    const tag = resume.includes(x) ? L('sg_resume', { d: fmtDur(x.secs, lang), i: fmtIdle(ageDays(x.last, api), lang) })
              : !(x.plays > 0) ? L('sg_new') : L('sg_old');
    const bits = [x.year, x.genre].filter(Boolean).join(', ');
    return '- ' + named(x) + (bits ? ' — ' + bits : '') + ' · _' + tag + '_';
  };
  return mk((what ? L('sg_from_what', { w: what }) : L('sg_from')) + '\n' + out.map(line).join('\n'),
            { intent: 'suggest', action: 'game', arg: out[0].system + '/' + out[0].file, cards: out.map(gameCard) });
}

// ---- trucchi --------------------------------------------------------------------------------------------
// I trucchi li applica RetroArch sulla console, dal prossimo avvio del gioco (nucleos-cheats scrive il .cht e
// accende apply_cheats_after_load nella configurazione di QUEL gioco). Il player del browser non li usa.
// Le descrizioni del database sono in inglese ("Infinite Lives"): chi chiede "vite infinite", "vidas
// infinitas" o "unendliche Leben" va tradotto in quelle parole, non cercato alla lettera.
const SYN = [
  [/^(vite|vita|vidas?|vies?|leben)$/, /\b(lives?|life)\b/],
  [/^(infinit\w*|infinies?|unendlich\w*)$/, /\b(infinite|unlimited|inf|max\w*)\b/],
  [/^(illimitat\w*|ilimitad\w*|illimite\w*|unbegrenzt\w*|unlimited)$/, /\b(unlimited|infinite|inf)\b/],
  [/^(energia|salute|salud|sante|energie|gesundheit|hp|pv|health)$/, /\b(health|energy|hp|vitality|life)\b/],
  [/^(soldi|denaro|monete|dinero|monedas|argent|pieces|geld|munzen|money)$/, /\b(money|gold|cash|coins?|rupees?|gil|zenny|bells)\b/],
  [/^(munizioni|municion\w*|munitions?|ammo)$/, /\b(ammo|ammunition|bullets?|shots?)\b/],
  [/^(tempo|tiempo|temps|zeit|time)$/, /\b(time|timer)\b/],
  [/^(invincibil\w*|invencib\w*|invincib\w*|unverwundbar\w*|unbesiegbar\w*)$/, /\binvinc\w*\b/],
  [/^(salto|salti|saltar|sauts?|springen|sprung|jump)$/, /\bjump\w*\b/],
  [/^(armi|arma|armas|armes?|waffen?|weapons?)$/, /\bweapons?\b/],
  [/^(livello|livelli|nivel|niveles|niveaux?|level)$/, /\b(level|stage|lv)\b/],
  [/^(bombe|bomba|bombas|bombes|bomben|bombs?)$/, /\bbombs?\b/],
  [/^(chiavi|chiave|llaves?|cles?|schlussel|keys?)$/, /\bkeys?\b/],
  [/^(crediti|creditos|credits?)$/, /\b(credits?|continues?)\b/],
];
function decodeEnt(s) { return String(s || '').replace(/&quot;/g, '"').replace(/&#0?39;|&apos;/g, '\'').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&'); }
// i trucchi che corrispondono a "vite infinite" / "3" (il numero dell'elenco, da 1)
export function matchCheats(list, what) {
  const w = words(fold(what));
  if (w.length === 1 && /^\d+$/.test(w[0])) { const c = list[+w[0] - 1]; return c ? [c] : []; }
  const res = w.map((t) => { for (const [k, re] of SYN) if (k.test(t)) return re; return new RegExp('\\b' + t.replace(/[^a-z0-9]/g, '')); });
  if (!res.length) return [];
  return list.filter((c) => { const d = fold(decodeEnt(c.desc)); return res.every((re) => re.test(d)); });
}
const cheatName = (c) => '«' + decodeEnt(c.desc) + '»';
function cheatLines(list, max) {
  return list.slice(0, max).map((c, k) => (k + 1) + '. ' + (c.on ? '✅ ' : '') + decodeEnt(c.desc)).join('\n');
}
// il gioco di cui si parla: quello nominato, altrimenti quello aperto adesso sulla console
async function gameFor(L, api, want) {
  if (want) {
    const r = await findGame(api, want, 40);
    if (r.err) return { reply: mk(unreachable(L, r.err), { intent: 'cheats' }) };
    if (!r.top) return { reply: mk(L('not_in_lib', { q: want }), { intent: 'cheats', confidence: 60 }) };
    return { g: r.top };
  }
  const s = api.gamemode ? await safe(() => api.gamemode()) : null;
  if (!failed(s) && s.gaming && s.game && s.game.file) return { g: s.game, running: true };
  return null;
}
async function cheatList(L, api, g) {
  const c = await safe(() => api.cheatsList(g.system, g.file));
  if (failed(c)) return { reply: mk(unreachable(L, c), { intent: 'cheats' }) };
  if (c.error && c.code !== 6) return { reply: mk(L('ch_unavail', { e: c.error }), { intent: 'cheats' }) };
  if (c.database === false || (c.code === 6 && !c.cheats))
    return { reply: mk(L('ch_nodb'), { intent: 'cheats', cards: [{ kind: 'action', arg: 'cheats-fetch', title: L('ch_dl_title'), sub: L('ch_dl_sub'), btn: L('ch_dl_btn') }] }) };
  return { c };
}
async function cheats(L, lang, api, want) {
  const t = await gameFor(L, api, want);
  if (!t) return kbReply('cheats_help', lang);
  // "trucchi per la pelle": un titolo che la libreria non conosce non è una domanda di gioco -> al modello
  if (t.reply) return t.reply.intent === 'cheats' && t.reply.confidence === 60 ? null : t.reply;
  const g = t.g, x = { intent: 'cheats', action: 'game', arg: g.system + '/' + g.file, cards: [Object.assign(gameCard(g), { open: L('ch_btn') })] };
  const r = await cheatList(L, api, g);
  if (r.reply) return r.reply;
  const list = r.c.cheats || [];
  if (!list.length) return mk(L('ch_none', { g: named(g) }), x);
  const st = api.cheatsStatus ? await safe(() => api.cheatsStatus()) : null;
  return mk(L('ch_head', { g: named(g), n: list.length, on: list.filter((c) => c.on).length }) + '\n' + cheatLines(list, 15) +
            (list.length > 15 ? '\n' + L('ch_more', { n: list.length - 15 }) : '') + '\n\n' + L('ch_how') +
            (!failed(st) && st.hardcore ? '\n\n' + L('ch_hardcore') : ''), Object.assign(x, { value: list.length }));
}
async function cheatSet(L, lang, api, p) {
  const t = await gameFor(L, api, p.game);
  if (!t) return mk(L('ch_which_game'), { intent: 'cheats', confidence: 60 });
  if (t.reply) return t.reply;
  const g = t.g, x = { intent: 'cheats', action: 'game', arg: g.system + '/' + g.file, cards: [Object.assign(gameCard(g), { open: L('ch_btn') })] };
  const r = await cheatList(L, api, g);
  if (r.reply) return r.reply;
  const list = r.c.cheats || [];
  if (!list.length) return mk(L('ch_none', { g: named(g) }), x);
  const cur = new Set(list.filter((c) => c.on).map((c) => c.i));
  let chosen = [];
  if (p.what && !(p.off && p.all)) {
    chosen = matchCheats(list, p.what);
    if (!chosen.length) return mk(L('ch_nomatch', { g: named(g), w: p.what }) + '\n' + cheatLines(list, 10), x);
    // "vite infinite" può voler dire P1 e P2 insieme; più di tre è una domanda troppo vaga
    if (chosen.length > 3) return mk(L('ch_pick', { g: named(g) }) + '\n' + chosen.slice(0, 10).map((c) => '- ' + decodeEnt(c.desc)).join('\n'), x);
  } else if (p.on) return mk(L('ch_pick', { g: named(g) }) + '\n' + cheatLines(list, 15) + '\n\n' + L('ch_how'), x);
  if (p.off && (!p.what || p.all)) cur.clear();
  else for (const c of chosen) p.off ? cur.delete(c.i) : cur.add(c.i);
  const res = await safe(() => api.cheatsSet(g.system, g.file, [...cur].sort((a, b) => a - b), r.c.db));
  if (failed(res)) return mk(unreachable(L, res), x);
  if (res.error) return mk(L('ch_set_fail', { e: res.error }), x);
  const names = chosen.map(cheatName).join(', ');
  const head = p.off && (!p.what || p.all) ? L('ch_off_all', { g: named(g) }) : L(p.off ? 'ch_off' : 'ch_on', { g: named(g), c: names });
  const n = Array.isArray(res.on) ? res.on.length : cur.size;
  return mk(head + ' ' + L('ch_now_on', { n }) + ' ' + L(t.running ? 'ch_restart' : 'ch_next'), Object.assign(x, { value: n }));
}

// ---- ports e emulatori ------------------------------------------------------------------------------
// I port non sono un sistema della libreria: sono gli script .sh della cartella ports/ (Core li legge così,
// sys/ports_data.c). Si elencano dal filesystem; i nomi sono quelli dei file.
async function portsList(L, lang, api) {
  const d = api.listDir ? await safe(() => api.listDir('roms/ports')) : null;
  if (failed(d)) return d && d.__err === 401 ? mk(unreachable(L, d), { intent: 'ports' }) : kbReply('ports', lang);
  const names = (d.entries || []).filter((e) => e.type === 'file' && /\.sh$/i.test(e.name)).map((e) => e.name.replace(/\.sh$/i, ''))
    .sort((a, b) => a.localeCompare(b));
  if (!names.length) return mk(L('ports_none'), { intent: 'ports' });
  return mk(L('ports_head', { n: names.length }) + '\n' + names.slice(0, 30).join(' · ') +
            (names.length > 30 ? '\n' + L('ports_more', { n: names.length - 30 }) : '') + '\n\n' + L('ports_how'), { intent: 'ports', value: names.length });
}
// Il core in uso per sistema: lo stesso che /api/library/bios usa per dire quali BIOS servono.
async function coreOf(L, api, f) {
  const res = await safe(() => api.bios());
  if (failed(res)) return mk(unreachable(L, res), { intent: 'core' });
  const sys = (res.systems || []).filter((x) => x.core);
  const hit = sys.filter((x) => {
    const n = fold(x.name), id = fold(x.system);
    return (' ' + f + ' ').includes(' ' + id + ' ') || (n && f.includes(n)) || n.split(' ').some((w) => w.length >= 4 && (' ' + f + ' ').includes(' ' + w + ' '));
  });
  const pick = (hit.length ? hit : sys).slice(0, 25);
  if (!pick.length) return mk(unreachable(L, res), { intent: 'core' });
  return mk((hit.length ? '' : L('core_head') + '\n') + pick.map((x) => '- **' + x.name + '**: `' + x.core + '`').join('\n') + '\n\n' + L('core_change'), { intent: 'core' });
}

// ---- la console come CONTESTO per i modelli (cloud, WebLLM, Agenti) -------------------------------------
// I modelli non sanno niente di questa console: senza questo blocco "consigliami qualcosa tra i miei giochi"
// o "come vado avanti in quello che sto giocando?" ricevono risposte inventate. Pochi fatti veri, compatti
// (il modello locale ha una finestra minuscola), che il prompt tratta come DATI. Puro: `api` come sopra.
export async function consoleFacts(lang, api, opts) {
  const L = (k, v) => say(lang, k, v);
  const small = opts && opts.small;
  const [gm, rec, st] = await Promise.all([api.gamemode ? safe(() => api.gamemode()) : null,
    api.recent ? safe(() => api.recent(small ? 3 : 8)) : null, api.status && !small ? safe(() => api.status()) : null]);
  const out = [];
  if (!failed(gm)) {
    if (gm.gaming && gm.game && gm.game.name) {
      const secs = gm.since > 0 ? nowOf(api).getTime() / 1000 - gm.since : 0;
      out.push(L('f_open', { g: gm.game.name + ' (' + (gm.game.sysname || gm.game.system) + ')', since: secs > 60 ? L('since', { d: fmtDur(secs, lang) }) : '' }));
    } else if (!gm.gaming) out.push(L('f_none'));
  }
  if (!failed(rec)) {
    const g = (rec.games || []).filter((x) => x.plays > 0);
    if (g.length) out.push(L('f_recent', { l: g.map((x) => x.name + ' [' + (x.sysname || x.system) + (x.genre && !small ? ', ' + x.genre : '') + ', ' +
      fmtDur(x.secs, lang) + ', ' + fmtDay(x.last, lang, api, true) + ']').join('; ') }));
  }
  if (!failed(st)) {
    const b = st.battery || {};
    if (b.present && b.pct >= 0) out.push(L('f_battery', { p: b.pct, c: b.charging ? L('f_charging') : '' }));
    if (st.device && (st.device.name || st.device.model)) out.push(L('f_device', { d: st.device.name || st.device.model }));
  }
  if (!out.length) return '';
  out.push(L('f_hint'));
  return out.join('\n');
}

// ---- risposte di conoscenza (testo, nessun dato) -------------------------------------------------------
// Le spiegazioni (come si caricano le ROM, tasti, trucchi, Ports, RetroAchievements...) stanno in game-kb.js.
function kbReply(id, lang, extra) {
  const e = KB.entry(id, lang);
  return e ? mk(e.text, Object.assign({ intent: 'howto:' + id, trace: 'NucleoOS · guida', cards: e.cards }, extra || {})) : null;
}

export async function consoleSkill(q, lang, api) {
  lang = lang || 'it';
  const L = (k, v) => say(lang, k, v);
  if (!api) return null;
  const p = parseIntent(q);
  if (!p) return KB.answer(q, lang);
  // le risposte generiche (salvataggi, ROM) cedono a una voce più precisa della guida:
  // "si scarica la batteria mentre gioco, perdo i salvataggi?" è la batteria scarica, non dove stanno
  if ((p.kind === 'saves' && !p.q) || p.kind === 'addroms') { const k = KB.answer(q, lang); if (k) return k; }
  switch (p.kind) {
    case 'kb': return kbReply(p.id, lang);
    case 'cheats': return cheats(L, lang, api, p.game);
    case 'cheat_set': return cheatSet(L, lang, api, p);
    case 'ports': return portsList(L, lang, api);
    case 'core': return coreOf(L, api, p.q);
    case 'battery': return battery(L, api);
    case 'storage': return storage(L, api);
    case 'network': return network(L, api);
    case 'time': case 'date': return timeOf(L, api, lang, p.kind);
    case 'version': return version(L, api);
    case 'play': return play(L, api, p.q);
    case 'recent': return recent(L, api);
    case 'bios': return bios(L, api, p.q);
    case 'addroms': return kbReply('addroms', lang);
    case 'saves': return p.q ? savesOf(L, lang, api, p.q, () => kbReply('saves', lang)) : kbReply('saves', lang);
    case 'doctor': return doctor(L, api);
    case 'open': return openApp(L, api, p.q);
    case 'nowplaying': return nowPlaying(L, lang, api);
    case 'playtime': return playtime(L, lang, api, p.q);
    case 'info': return info(L, lang, api, p.q, p.game);
    case 'suggest': return suggest(L, lang, api, p);
  }
  return null;
}

// ---- correzioni al cervello offline ----------------------------------------------------------------
// Il motore WASM risponde "Batteria: {value}." (il valore lo metteva il firmware dell'altro dispositivo).
// Se una risposta di sistema arriva così, la si riscrive con i dati veri; se non si può, via il segnaposto.
export async function fillSystemReply(r, lang, api) {
  if (!r || !r.reply || !/\{value\}/.test(r.reply)) return r;
  const kind = { battery: 'battery', storage: 'storage', time: 'time', date: 'date' }[r.arg || r.intent];
  if (kind && api) {
    const lg = lang || 'it', L = (k, v) => say(lg, k, v);
    const fixed = kind === 'battery' ? await battery(L, api) : kind === 'storage' ? await storage(L, api) : timeOf(L, api, lg, kind);
    if (fixed && fixed.reply) return Object.assign({}, r, { reply: fixed.reply });
  }
  return Object.assign({}, r, { reply: r.reply.replace(/\s*\{value\}\s*/g, ' ').replace(/\s+\./g, '.').trim() });
}

// Il motore offline ha una sua tabella di app compilata dentro il WASM, che il pacchetto comandi non
// cambia: per "apri la console" sceglie il Terminale, per "apri giochi" DOSBox, e "apri il terminale"
// lo prende per "chiudi". Misurato in Node con il motore vero (tools/tests/anima/test.mjs). Un suo
// "apri" vale quindi solo se il registro delle app lo conferma: se il registro dice un'altra app si
// apre quella, se non la riconosce non si apre niente (e la domanda prosegue verso il prossimo livello).
export function vetLaunch(r, q, aliases) {
  if (!r || !(r.action === 'launch' || r.intent === 'open_app' || r.intent === 'close_app')) return r;
  const f = fold(q);
  const om = f.match(OPEN_VERB);
  const m = matchApp(om ? strip(f.slice(om[0].length)) : strip(f), aliases);
  if (!m) return null;
  if (m.id === r.arg && r.action === 'launch') return r;
  return Object.assign({}, r, { action: 'launch', intent: 'open_app', arg: m.id,
    reply: (r.reply || '').replace(/^(Apro|Chiudo|Opening|Closing)\s+\S+/, (x, v) => (/^(Apro|Chiudo)$/.test(v) ? 'Apro ' : 'Opening ') + m.id) || m.id });
}

// "traduci cane in inglese" -> il dizionario risponde in ENTRAMBE le direzioni ("IT->EN: dog… EN->IT: canna…"),
// e la seconda è rumore (cane in inglese è il bastone). Se la domanda dice la lingua d'arrivo, si tiene
// solo quella direzione.
export function fixTranslate(r, q, lang) {
  if (!r || !r.reply || r.intent !== 'translate') return r;
  const f = fold(q);
  const toEn = /\b(in|to|into) (inglese|english)\b/.test(f);
  const toIt = /\b(in|to|into) (italiano|italian)\b/.test(f);
  if (toEn === toIt) return r;
  const keep = toEn ? 'IT->EN' : 'EN->IT';
  const m = r.reply.match(new RegExp(keep + ':\\s*([^]*?)(?:\\s*(?:IT->EN|EN->IT):|$)'));
  if (!m) return r;
  const word = (r.reply.match(/^"([^"]+)"/) || [])[1];
  const list = m[1].trim().replace(/\.$/, '');
  const target = lang === 'it' ? (toEn ? 'in inglese' : 'in italiano') : (toEn ? 'in English' : 'in Italian');
  const reply = word ? '"' + word + '" ' + target + ': ' + list + '.' : list;
  return Object.assign({}, r, { reply });
}
