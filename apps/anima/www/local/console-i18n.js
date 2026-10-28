/* SPDX-License-Identifier: AGPL-3.0-or-later */
// ANIMA — le frasi delle abilità di console, nelle 5 lingue di NucleoOS (it, en, es, fr, de).
//
// Perché un catalogo e non TR(it, en). ANIMA traduce es/fr/de con una tabella indicizzata dalla frase
// inglese ESATTA: funziona per le etichette fisse, non per una risposta composta ("Batteria al 76%",
// "**Zelda** (GBA): giocato 2 volte…"), che in spagnolo, francese e tedesco usciva in inglese. Qui ogni
// frase è un modello con i segnaposto {x}; la lingua sceglie il modello, i dati lo riempiono.
//
// Regole: ogni chiave ha TUTTE e 5 le lingue (tools/tests/anima/test.mjs lo verifica, come
// tools/i18n-check.py fa per Core). Un segnaposto usato in una lingua c'è in tutte. Niente HTML: il testo
// è Markdown e lo rende la chat. Puro: niente DOM, si prova in Node.

export const LANGS = ['it', 'en', 'es', 'fr', 'de'];
export const LOCALE = { it: 'it-IT', en: 'en-GB', es: 'es-ES', fr: 'fr-FR', de: 'de-DE' };

export const MSG = {
  // ---- quando la console non risponde ----
  unreach_pin: {
    it: 'Per leggerlo serve l\'abbinamento: inserisci il PIN che mostra la console.',
    en: 'Reading this needs pairing: enter the PIN the console shows.',
    es: 'Para leerlo hace falta vincular: introduce el PIN que muestra la consola.',
    fr: 'Pour le lire, il faut appairer : saisis le code PIN affiché par la console.',
    de: 'Dafür muss gekoppelt werden: Gib die PIN ein, die die Konsole anzeigt.' },
  unreach: {
    it: 'Non riesco a leggerlo dalla console in questo momento.',
    en: 'I can\'t read that from the console right now.',
    es: 'Ahora mismo no puedo leerlo de la consola.',
    fr: 'Je n\'arrive pas à le lire sur la console pour le moment.',
    de: 'Ich kann das gerade nicht von der Konsole lesen.' },

  // ---- durate e date ----
  dur_under_min: { it: 'meno di un minuto', en: 'under a minute', es: 'menos de un minuto', fr: 'moins d\'une minute', de: 'unter einer Minute' },
  today: { it: 'oggi', en: 'today', es: 'hoy', fr: 'aujourd\'hui', de: 'heute' },
  yesterday: { it: 'ieri', en: 'yesterday', es: 'ayer', fr: 'hier', de: 'gestern' },
  days_ago: { it: '{n} giorni fa', en: '{n} days ago', es: 'hace {n} días', fr: 'il y a {n} jours', de: 'vor {n} Tagen' },
  on_date: { it: 'il {d}', en: 'on {d}', es: 'el {d}', fr: 'le {d}', de: 'am {d}' },
  just_now: { it: 'adesso', en: 'just now', es: 'ahora mismo', fr: 'à l\'instant', de: 'gerade eben' },
  min_ago: { it: '{n} min fa', en: '{n} min ago', es: 'hace {n} min', fr: 'il y a {n} min', de: 'vor {n} Min.' },
  h_ago: { it: '{n} h fa', en: '{n} h ago', es: 'hace {n} h', fr: 'il y a {n} h', de: 'vor {n} Std.' },
  idle_days: { it: '{n} giorni', en: '{n} days', es: '{n} días', fr: '{n} jours', de: '{n} Tagen' },
  idle_weeks: { it: '{n} settimane', en: '{n} weeks', es: '{n} semanas', fr: '{n} semaines', de: '{n} Wochen' },
  idle_months: { it: '{n} mesi', en: '{n} months', es: '{n} meses', fr: '{n} mois', de: '{n} Monaten' },

  // ---- batteria ----
  bat_na: {
    it: 'Questa console non riporta il livello della batteria.', en: 'This console does not report its battery level.',
    es: 'Esta consola no informa del nivel de batería.', fr: 'Cette console n\'indique pas le niveau de sa batterie.',
    de: 'Diese Konsole meldet ihren Akkustand nicht.' },
  bat: { it: 'Batteria al {p}%.', en: 'Battery at {p}%.', es: 'Batería al {p} %.', fr: 'Batterie à {p} %.', de: 'Akku bei {p} %.' },
  bat_charging: {
    it: 'Batteria al {p}%, in carica.', en: 'Battery at {p}%, charging.', es: 'Batería al {p} %, cargando.',
    fr: 'Batterie à {p} %, en charge.', de: 'Akku bei {p} %, wird geladen.' },
  bat_full: {
    it: 'Batteria al {p}%, collegata: carica completa.', en: 'Battery at {p}%, plugged in: fully charged.',
    es: 'Batería al {p} %, enchufada: carga completa.', fr: 'Batterie à {p} %, branchée : charge complète.',
    de: 'Akku bei {p} %, angeschlossen: voll geladen.' },
  bat_plugged: {
    it: 'Batteria al {p}%, collegata all’alimentatore.', en: 'Battery at {p}%, plugged in.',
    es: 'Batería al {p} %, conectada al cargador.', fr: 'Batterie à {p} %, branchée sur le chargeur.',
    de: 'Akku bei {p} %, am Netzteil.' },
  bat_low: {
    it: 'È bassa: mettila in carica. Sotto il 5% NucleoOS chiude il gioco salvando e spegne la console.',
    en: 'It\'s low: plug it in. Below 5% NucleoOS closes the game with a save and powers off.',
    es: 'Está baja: ponla a cargar. Por debajo del 5 % NucleoOS cierra el juego guardando y apaga la consola.',
    fr: 'Elle est faible : branche-la. Sous 5 %, NucleoOS ferme le jeu en sauvegardant et éteint la console.',
    de: 'Er ist schwach: Lade die Konsole. Unter 5 % beendet NucleoOS das Spiel mit Speichern und schaltet aus.' },

  // ---- spazio ----
  card_games: { it: 'Scheda dei giochi', en: 'Games card', es: 'Tarjeta de juegos', fr: 'Carte des jeux', de: 'Spielekarte' },
  card_media: { it: 'Seconda scheda (media)', en: 'Second card (media)', es: 'Segunda tarjeta (multimedia)', fr: 'Deuxième carte (médias)', de: 'Zweite Karte (Medien)' },
  free_of: { it: '{f} liberi su {t}', en: '{f} free of {t}', es: '{f} libres de {t}', fr: '{f} libres sur {t}', de: '{f} frei von {t}' },
  space_head: { it: 'Spazio sulle schede:', en: 'Card space:', es: 'Espacio en las tarjetas:', fr: 'Espace sur les cartes :', de: 'Speicher auf den Karten:' },
  space_na: {
    it: 'Non riesco a leggere lo spazio delle schede.', en: 'I can\'t read the cards\' space.',
    es: 'No puedo leer el espacio de las tarjetas.', fr: 'Je n\'arrive pas à lire l\'espace des cartes.',
    de: 'Ich kann den Speicher der Karten nicht lesen.' },
  sd2_empty: {
    it: 'Lo slot della seconda scheda è vuoto: inserendone una, NucleoOS la usa per film e musica.',
    en: 'The second card slot is empty: insert one and NucleoOS uses it for films and music.',
    es: 'La ranura de la segunda tarjeta está vacía: si insertas una, NucleoOS la usa para películas y música.',
    fr: 'L\'emplacement de la deuxième carte est vide : insères-en une et NucleoOS l\'utilise pour les films et la musique.',
    de: 'Der Steckplatz für die zweite Karte ist leer: Steck eine ein, dann nutzt NucleoOS sie für Filme und Musik.' },

  // ---- rete, ora, versione ----
  net_wifi: { it: 'Connessa al WiFi «{ssid}».', en: 'Connected to WiFi “{ssid}”.', es: 'Conectada al WiFi «{ssid}».', fr: 'Connectée au WiFi « {ssid} ».', de: 'Mit dem WLAN „{ssid}“ verbunden.' },
  net_wifi_ip: {
    it: 'Connessa al WiFi «{ssid}», indirizzo {ip}.', en: 'Connected to WiFi “{ssid}”, address {ip}.',
    es: 'Conectada al WiFi «{ssid}», dirección {ip}.', fr: 'Connectée au WiFi « {ssid} », adresse {ip}.',
    de: 'Mit dem WLAN „{ssid}“ verbunden, Adresse {ip}.' },
  net_usb: {
    it: 'Collegata al PC con il cavo USB: indirizzo {ip}. Dal PC: http://{ip}:8080',
    en: 'Connected to the PC over the USB cable: address {ip}. From the PC: http://{ip}:8080',
    es: 'Conectada al PC por cable USB: dirección {ip}. Desde el PC: http://{ip}:8080',
    fr: 'Reliée au PC par le câble USB : adresse {ip}. Depuis le PC : http://{ip}:8080',
    de: 'Per USB-Kabel mit dem PC verbunden: Adresse {ip}. Vom PC aus: http://{ip}:8080' },
  net_none: {
    it: 'Nessuna rete attiva. Col cavo USB la console si raggiunge dal PC su 192.168.42.1.',
    en: 'No network up. Over the USB cable the console is reachable from the PC at 192.168.42.1.',
    es: 'No hay red activa. Con el cable USB la consola se alcanza desde el PC en 192.168.42.1.',
    fr: 'Aucun réseau actif. Avec le câble USB, la console est joignable depuis le PC à 192.168.42.1.',
    de: 'Kein Netzwerk aktiv. Über das USB-Kabel ist die Konsole vom PC aus unter 192.168.42.1 erreichbar.' },
  net_unsynced: {
    it: '(L\'ora non è ancora sincronizzata.)', en: '(The clock is not synced yet.)', es: '(La hora aún no está sincronizada.)',
    fr: '(L\'heure n\'est pas encore synchronisée.)', de: '(Die Uhrzeit ist noch nicht synchronisiert.)' },
  time_is: { it: 'Sono le {t}.', en: 'It\'s {t}.', es: 'Son las {t}.', fr: 'Il est {t}.', de: 'Es ist {t} Uhr.' },
  date_is: { it: 'Oggi è {d}.', en: 'Today is {d}.', es: 'Hoy es {d}.', fr: 'Nous sommes le {d}.', de: 'Heute ist {d}.' },
  version: {
    it: 'NucleoOS {v}. Gli aggiornamenti si cercano dall\'app Aggiornamenti.', en: 'NucleoOS {v}. Look for updates in the Updates app.',
    es: 'NucleoOS {v}. Las actualizaciones se buscan desde la app Actualizaciones.', fr: 'NucleoOS {v}. Les mises à jour se cherchent dans l\'app Mises à jour.',
    de: 'NucleoOS {v}. Updates findest du in der App Updates.' },

  // ---- giocare ----
  play_none: {
    it: 'Non trovo «{q}» nella libreria. Prova con un\'altra parola del titolo, oppure chiedimi come si caricano le ROM.',
    en: 'I can\'t find “{q}” in the library. Try another word of the title, or ask me how to add ROMs.',
    es: 'No encuentro «{q}» en la biblioteca. Prueba con otra palabra del título o pregúntame cómo se cargan las ROM.',
    fr: 'Je ne trouve pas « {q} » dans la bibliothèque. Essaie un autre mot du titre, ou demande-moi comment ajouter des ROM.',
    de: 'Ich finde „{q}“ nicht in der Bibliothek. Versuch ein anderes Wort aus dem Titel oder frag mich, wie man ROMs hinzufügt.' },
  not_in_lib: { it: 'Non trovo «{q}» nella libreria.', en: 'I can\'t find “{q}” in the library.', es: 'No encuentro «{q}» en la biblioteca.', fr: 'Je ne trouve pas « {q} » dans la bibliothèque.', de: 'Ich finde „{q}“ nicht in der Bibliothek.' },
  play_found: {
    it: 'Ho trovato {g}. Giocalo qui nel browser o direttamente sulla console.', en: 'Found {g}. Play it here in the browser or right on the console.',
    es: 'He encontrado {g}. Juégalo aquí en el navegador o directamente en la consola.', fr: 'J\'ai trouvé {g}. Joue ici dans le navigateur ou directement sur la console.',
    de: 'Gefunden: {g}. Spiel es hier im Browser oder direkt auf der Konsole.' },
  play_more: { it: 'Sotto anche gli altri risultati.', en: 'Other matches are below.', es: 'Debajo están los demás resultados.', fr: 'Les autres résultats sont en dessous.', de: 'Weitere Treffer stehen darunter.' },
  play_pick: { it: 'Ho trovato {n} giochi: scegli quello giusto.', en: 'I found {n} games: pick the right one.', es: 'He encontrado {n} juegos: elige el correcto.', fr: 'J\'ai trouvé {n} jeux : choisis le bon.', de: 'Ich habe {n} Spiele gefunden: Wähl das richtige.' },
  recent_none: { it: 'Non hai ancora giocato niente su questa console.', en: 'Nothing has been played on this console yet.', es: 'Todavía no has jugado a nada en esta consola.', fr: 'Rien n\'a encore été joué sur cette console.', de: 'Auf dieser Konsole wurde noch nichts gespielt.' },
  recent_last: {
    it: 'L\'ultimo gioco è {g}. Riprendendolo sulla console riparte da dove l\'hai lasciato.',
    en: 'Your last game is {g}. Resumed on the console it picks up where you left it.',
    es: 'Tu último juego es {g}. Si lo retomas en la consola, sigue donde lo dejaste.',
    fr: 'Ton dernier jeu est {g}. Repris sur la console, il reprend là où tu l\'as laissé.',
    de: 'Dein letztes Spiel ist {g}. Auf der Konsole fortgesetzt, geht es dort weiter, wo du aufgehört hast.' },
  opening: { it: 'Apro {a}.', en: 'Opening {a}.', es: 'Abro {a}.', fr: 'J\'ouvre {a}.', de: 'Ich öffne {a}.' },

  // ---- gioco in corso ----
  none_open: { it: 'Sulla console non c\'è nessun gioco aperto in questo momento.', en: 'No game is open on the console right now.', es: 'Ahora mismo no hay ningún juego abierto en la consola.', fr: 'Aucun jeu n\'est ouvert sur la console en ce moment.', de: 'Auf der Konsole ist gerade kein Spiel geöffnet.' },
  playing: { it: 'Sulla console stai giocando a {g}{since}.', en: 'On the console you\'re playing {g}{since}.', es: 'En la consola estás jugando a {g}{since}.', fr: 'Sur la console, tu joues à {g}{since}.', de: 'Auf der Konsole spielst du {g}{since}.' },
  playing_noname: { it: 'Sulla console c\'è un gioco aperto{since}.', en: 'A game is open on the console{since}.', es: 'Hay un juego abierto en la consola{since}.', fr: 'Un jeu est ouvert sur la console{since}.', de: 'Auf der Konsole ist ein Spiel geöffnet{since}.' },
  since: { it: ', da {d}', en: ', for {d}', es: ', desde hace {d}', fr: ', depuis {d}', de: ', seit {d}' },

  // ---- tempo giocato ----
  played_never: { it: 'Non ci hai ancora giocato sulla console.', en: 'You haven\'t played it on the console yet.', es: 'Aún no lo has jugado en la consola.', fr: 'Tu n\'y as pas encore joué sur la console.', de: 'Du hast es auf der Konsole noch nicht gespielt.' },
  played_once: { it: 'una volta', en: 'once', es: 'una vez', fr: 'une fois', de: 'einmal' },
  played_n: { it: '{n} volte', en: '{n} times', es: '{n} veces', fr: '{n} fois', de: '{n}-mal' },
  played: { it: 'Giocato {times}{dur}{last}.', en: 'Played {times}{dur}{last}.', es: 'Jugado {times}{dur}{last}.', fr: 'Joué {times}{dur}{last}.', de: '{times} gespielt{dur}{last}.' },
  played_dur: { it: ', {d} in tutto', en: ', {d} in total', es: ', {d} en total', fr: ', {d} au total', de: ', insgesamt {d}' },
  played_last: { it: '; l\'ultima volta {w}', en: '; last time {w}', es: '; la última vez {w}', fr: ' ; la dernière fois {w}', de: '; zuletzt {w}' },
  pt_nothing: {
    it: 'Sulla console non hai ancora giocato a niente: il tempo si conta da quando un gioco parte.',
    en: 'Nothing has been played on the console yet: time is counted from the moment a game starts.',
    es: 'Todavía no has jugado a nada en la consola: el tiempo se cuenta desde que arranca un juego.',
    fr: 'Rien n\'a encore été joué sur la console : le temps se compte dès qu\'un jeu démarre.',
    de: 'Auf der Konsole wurde noch nichts gespielt: Die Zeit zählt ab dem Start eines Spiels.' },
  pt_total: {
    it: 'Sulla console hai giocato **{d}** in {s} partite, su {g} giochi.', en: 'On the console you have played **{d}** over {s} sessions, across {g} games.',
    es: 'En la consola has jugado **{d}** en {s} partidas, con {g} juegos.', fr: 'Sur la console, tu as joué **{d}** en {s} parties, sur {g} jeux.',
    de: 'Auf der Konsole hast du **{d}** in {s} Sitzungen mit {g} Spielen gespielt.' },
  pt_fav: { it: 'Il sistema che usi di più: {s}.', en: 'Your most played system: {s}.', es: 'El sistema que más usas: {s}.', fr: 'Le système que tu utilises le plus : {s}.', de: 'Dein meistgespieltes System: {s}.' },
  pt_top: { it: 'I più giocati:', en: 'Most played:', es: 'Los más jugados:', fr: 'Les plus joués :', de: 'Am meisten gespielt:' },
  sessions_1: { it: '1 partita', en: '1 session', es: '1 partida', fr: '1 partie', de: '1 Sitzung' },
  sessions_n: { it: '{n} partite', en: '{n} sessions', es: '{n} partidas', fr: '{n} parties', de: '{n} Sitzungen' },

  // ---- scheda ----
  up_to_players: { it: 'fino a {n} giocatori', en: 'up to {n} players', es: 'hasta {n} jugadores', fr: 'jusqu\'à {n} joueurs', de: 'bis zu {n} Spieler' },
  no_desc: {
    it: 'Questo gioco non ha ancora una scheda con la descrizione (la scrive lo scraper della libreria).',
    en: 'This game has no description card yet (the library scraper writes it).',
    es: 'Este juego aún no tiene ficha con descripción (la escribe el scraper de la biblioteca).',
    fr: 'Ce jeu n\'a pas encore de fiche avec description (c\'est le scraper de la bibliothèque qui l\'écrit).',
    de: 'Für dieses Spiel gibt es noch keine Beschreibung (die schreibt der Scraper der Bibliothek).' },

  // ---- salvataggi ----
  saves_btn: { it: 'Salvataggi', en: 'Saves', es: 'Partidas guardadas', fr: 'Sauvegardes', de: 'Spielstände' },
  saves_none: { it: 'Per {g} non c\'è ancora nessun salvataggio.', en: 'There are no saves for {g} yet.', es: 'Todavía no hay partidas guardadas de {g}.', fr: 'Il n\'y a encore aucune sauvegarde pour {g}.', de: 'Für {g} gibt es noch keine Spielstände.' },
  saves_of: { it: 'Salvataggi di {g}:', en: 'Saves of {g}:', es: 'Partidas guardadas de {g}:', fr: 'Sauvegardes de {g} :', de: 'Spielstände von {g}:' },
  kind_srm: { it: 'salvataggio del gioco', en: 'in-game save', es: 'partida del juego', fr: 'sauvegarde du jeu', de: 'Spielspeicher' },
  kind_state: { it: 'stato', en: 'save state', es: 'estado guardado', fr: 'état sauvegardé', de: 'Savestate' },
  saved_when: { it: ', salvato {w}', en: ', saved {w}', es: ', guardado {w}', fr: ', sauvegardé {w}', de: ', gespeichert {w}' },
  deleted: { it: ', cancellato', en: ', deleted', es: ', borrado', fr: ', supprimé', de: ', gelöscht' },
  versions_1: { it: '1 versione precedente', en: '1 earlier version', es: '1 versión anterior', fr: '1 version précédente', de: '1 ältere Version' },
  versions_n: { it: '{n} versioni precedenti', en: '{n} earlier versions', es: '{n} versiones anteriores', fr: '{n} versions précédentes', de: '{n} ältere Versionen' },
  restore_hint: {
    it: 'Per tornare a una versione precedente apri i Salvataggi del gioco qui sotto: quella attuale resta comunque nella cronologia.',
    en: 'To go back to an earlier version open the game\'s Saves below: the current one stays in the history anyway.',
    es: 'Para volver a una versión anterior abre las partidas guardadas del juego aquí abajo: la actual se queda igualmente en el historial.',
    fr: 'Pour revenir à une version précédente, ouvre les sauvegardes du jeu ci-dessous : l\'actuelle reste de toute façon dans l\'historique.',
    de: 'Um zu einer älteren Version zurückzukehren, öffne unten die Spielstände des Spiels: Die aktuelle bleibt trotzdem im Verlauf.' },

  // ---- consigli ----
  sg_from: { it: 'Dalla tua libreria:', en: 'From your library:', es: 'De tu biblioteca:', fr: 'Dans ta bibliothèque :', de: 'Aus deiner Bibliothek:' },
  sg_from_what: { it: 'Dalla tua libreria, giochi {w}:', en: 'From your library, {w} games:', es: 'De tu biblioteca, juegos {w}:', fr: 'Dans ta bibliothèque, des jeux {w} :', de: 'Aus deiner Bibliothek, {w}:' },
  sg_resume: {
    it: 'da riprendere: {d} giocati, fermo da {i}', en: 'to pick up again: {d} played, untouched for {i}',
    es: 'para retomar: {d} jugados, parado desde hace {i}', fr: 'à reprendre : {d} joués, en pause depuis {i}',
    de: 'zum Weiterspielen: {d} gespielt, seit {i} unberührt' },
  sg_new: { it: 'mai provato', en: 'never tried', es: 'nunca probado', fr: 'jamais essayé', de: 'nie ausprobiert' },
  sg_old: { it: 'già giocato', en: 'played before', es: 'ya jugado', fr: 'déjà joué', de: 'schon gespielt' },
  sg_none_what: {
    it: 'Non trovo giochi {w} nella libreria. Il genere e i giocatori li sa solo un gioco con la scheda (lo scraper della libreria la scrive).',
    en: 'I can\'t find {w} games in the library. Genre and players are known only for games with a card (the library scraper writes it).',
    es: 'No encuentro juegos {w} en la biblioteca. El género y los jugadores solo se conocen en juegos con ficha (la escribe el scraper de la biblioteca).',
    fr: 'Je ne trouve pas de jeux {w} dans la bibliothèque. Le genre et les joueurs ne sont connus que pour les jeux avec une fiche (le scraper de la bibliothèque l\'écrit).',
    de: 'Ich finde keine {w} in der Bibliothek. Genre und Spielerzahl kennt man nur bei Spielen mit Steckbrief (den schreibt der Scraper der Bibliothek).' },
  sg_empty: { it: 'La libreria è vuota: chiedimi come si caricano le ROM.', en: 'The library is empty: ask me how to add ROMs.', es: 'La biblioteca está vacía: pregúntame cómo se cargan las ROM.', fr: 'La bibliothèque est vide : demande-moi comment ajouter des ROM.', de: 'Die Bibliothek ist leer: Frag mich, wie man ROMs hinzufügt.' },
  for_two: { it: 'per giocare in due', en: 'for two players', es: 'para jugar a dos', fr: 'pour jouer à deux', de: 'für zwei Spieler' },
  g_racing: { it: 'di corse', en: 'racing', es: 'de carreras', fr: 'de course', de: 'Rennspiele' },
  g_fighting: { it: 'picchiaduro', en: 'fighting', es: 'de lucha', fr: 'de combat', de: 'Kampfspiele' },
  g_platform: { it: 'a piattaforme', en: 'platform', es: 'de plataformas', fr: 'de plateforme', de: 'Jump ’n’ Runs' },
  g_rpg: { it: 'di ruolo', en: 'role-playing', es: 'de rol', fr: 'de rôle', de: 'Rollenspiele' },
  g_shooter: { it: 'sparatutto', en: 'shooter', es: 'de disparos', fr: 'de tir', de: 'Shooter' },
  g_puzzle: { it: 'rompicapo', en: 'puzzle', es: 'de puzles', fr: 'de réflexion', de: 'Puzzlespiele' },
  g_sports: { it: 'sportivi', en: 'sports', es: 'de deportes', fr: 'de sport', de: 'Sportspiele' },
  g_adventure: { it: 'd\'avventura', en: 'adventure', es: 'de aventura', fr: 'd\'aventure', de: 'Abenteuerspiele' },
  g_strategy: { it: 'di strategia', en: 'strategy', es: 'de estrategia', fr: 'de stratégie', de: 'Strategiespiele' },
  g_beatemup: { it: 'beat \'em up', en: 'beat \'em up', es: 'beat \'em up', fr: 'beat \'em up', de: 'Beat ’em ups' },

  // ---- BIOS ----
  bios_where: {
    it: 'I file vanno nella cartella {dir} (copiali con l\'app File o col Disco USB).', en: 'The files go in {dir} (copy them with the File app or the USB Drive).',
    es: 'Los archivos van en la carpeta {dir} (cópialos con la app Archivos o con el Disco USB).', fr: 'Les fichiers vont dans le dossier {dir} (copie-les avec l\'app Fichiers ou le Disque USB).',
    de: 'Die Dateien gehören in den Ordner {dir} (kopier sie mit der App Dateien oder über das USB-Laufwerk).' },
  bios_missing: { it: 'mancano {f}', en: 'missing {f}', es: 'faltan {f}', fr: 'il manque {f}', de: 'es fehlen {f}' },
  bios_unknown: { it: 'non so cosa chieda il core «{c}» (nessuna scheda informativa).', en: 'I don\'t know what core “{c}” needs (no info file).', es: 'no sé qué pide el core «{c}» (sin ficha informativa).', fr: 'je ne sais pas ce que demande le core « {c} » (pas de fichier info).', de: 'ich weiß nicht, was der Core „{c}“ braucht (keine Info-Datei).' },
  bios_none: { it: 'non serve nessun BIOS.', en: 'no BIOS needed.', es: 'no necesita BIOS.', fr: 'aucun BIOS nécessaire.', de: 'kein BIOS nötig.' },
  bios_ok: { it: 'a posto.', en: 'all set.', es: 'todo en orden.', fr: 'tout est en ordre.', de: 'alles da.' },
  bios_opt: { it: '(manca solo qualche BIOS facoltativo.)', en: '(only optional BIOS files missing.)', es: '(solo faltan BIOS opcionales.)', fr: '(il ne manque que des BIOS facultatifs.)', de: '(es fehlen nur optionale BIOS-Dateien.)' },
  bios_all_ok: { it: 'Tutti i sistemi con dei giochi hanno i BIOS che servono.', en: 'Every system with games has the BIOS it needs.', es: 'Todos los sistemas con juegos tienen las BIOS que necesitan.', fr: 'Tous les systèmes avec des jeux ont les BIOS nécessaires.', de: 'Alle Systeme mit Spielen haben die nötigen BIOS-Dateien.' },
  bios_head: { it: 'Mancano dei BIOS:', en: 'Some BIOS files are missing:', es: 'Faltan algunas BIOS:', fr: 'Il manque des BIOS :', de: 'Einige BIOS-Dateien fehlen:' },

  // ---- diagnosi ----
  doc_na: { it: 'Lo strumento di diagnosi non è installato su questa console.', en: 'The diagnosis tool is not installed on this console.', es: 'La herramienta de diagnóstico no está instalada en esta consola.', fr: 'L\'outil de diagnostic n\'est pas installé sur cette console.', de: 'Das Diagnosewerkzeug ist auf dieser Konsole nicht installiert.' },
  doc_ok: { it: 'Ho fatto la visita alla console: nessun problema.', en: 'I gave the console a check-up: no problems.', es: 'He revisado la consola: ningún problema.', fr: 'J\'ai examiné la console : aucun problème.', de: 'Ich habe die Konsole durchgecheckt: keine Probleme.' },
  doc_found_1: { it: 'La visita ha trovato 1 problema:', en: 'The check-up found 1 problem:', es: 'La revisión ha encontrado 1 problema:', fr: 'L\'examen a trouvé 1 problème :', de: 'Der Check hat 1 Problem gefunden:' },
  doc_found_n: { it: 'La visita ha trovato {n} problemi:', en: 'The check-up found {n} problems:', es: 'La revisión ha encontrado {n} problemas:', fr: 'L\'examen a trouvé {n} problèmes :', de: 'Der Check hat {n} Probleme gefunden:' },
  doc_unreadable: { it: 'La visita ha trovato qualcosa da guardare, ma non riesco a leggerne il dettaglio.', en: 'The check-up found something, but I can\'t read the details.', es: 'La revisión ha encontrado algo, pero no puedo leer el detalle.', fr: 'L\'examen a trouvé quelque chose, mais je n\'arrive pas à en lire le détail.', de: 'Der Check hat etwas gefunden, aber ich kann die Details nicht lesen.' },
  doc_fix: { it: 'rimedio', en: 'fix', es: 'solución', fr: 'solution', de: 'Lösung' },

  // ---- trucchi ----
  ch_btn: { it: 'Trucchi', en: 'Cheats', es: 'Trucos', fr: 'Codes de triche', de: 'Cheats' },
  ch_unavail: {
    it: 'I trucchi non sono disponibili per questo gioco: {e}', en: 'Cheats are not available for this game: {e}',
    es: 'Los trucos no están disponibles para este juego: {e}', fr: 'Les codes de triche ne sont pas disponibles pour ce jeu : {e}',
    de: 'Für dieses Spiel gibt es keine Cheats: {e}' },
  ch_nodb: {
    it: 'Il database dei trucchi non è ancora sulla console: è quello ufficiale di libretro, circa 37 MB da scaricare una volta sola (serve internet).',
    en: 'The cheat database is not on the console yet: it\'s the official libretro one, about 37 MB to download once (needs internet).',
    es: 'La base de datos de trucos aún no está en la consola: es la oficial de libretro, unos 37 MB que se descargan una sola vez (hace falta internet).',
    fr: 'La base de codes de triche n\'est pas encore sur la console : c\'est celle de libretro, environ 37 Mo à télécharger une seule fois (internet nécessaire).',
    de: 'Die Cheat-Datenbank ist noch nicht auf der Konsole: Es ist die offizielle von libretro, etwa 37 MB, einmal herunterzuladen (Internet nötig).' },
  ch_dl_title: { it: 'Database dei trucchi', en: 'Cheat database', es: 'Base de datos de trucos', fr: 'Base de codes de triche', de: 'Cheat-Datenbank' },
  ch_dl_sub: { it: 'libretro, circa 37 MB', en: 'libretro, about 37 MB', es: 'libretro, unos 37 MB', fr: 'libretro, environ 37 Mo', de: 'libretro, etwa 37 MB' },
  ch_dl_btn: { it: 'Scarica', en: 'Download', es: 'Descargar', fr: 'Télécharger', de: 'Herunterladen' },
  ch_none: { it: 'Nel database non ci sono trucchi per {g}.', en: 'The database has no cheats for {g}.', es: 'La base de datos no tiene trucos para {g}.', fr: 'La base n\'a aucun code de triche pour {g}.', de: 'Die Datenbank hat keine Cheats für {g}.' },
  ch_head: { it: 'Trucchi per {g}: {n}, attivi {on}.', en: 'Cheats for {g}: {n}, {on} on.', es: 'Trucos para {g}: {n}, {on} activos.', fr: 'Codes de triche pour {g} : {n}, {on} actifs.', de: 'Cheats für {g}: {n}, {on} aktiv.' },
  ch_more: { it: '…e altri {n}: li vedi tutti nella scheda del gioco.', en: '…and {n} more: see them all in the game\'s card.', es: '…y {n} más: los ves todos en la ficha del juego.', fr: '…et {n} autres : tu les vois tous dans la fiche du jeu.', de: '…und {n} weitere: Alle stehen im Steckbrief des Spiels.' },
  ch_how: {
    it: 'Dimmi «attiva il trucco … in …» (anche col numero); valgono dal prossimo avvio del gioco sulla console.',
    en: 'Tell me “enable the … cheat in …” (the number works too); they apply from the game\'s next start on the console.',
    es: 'Dime «activa el truco … en …» (también con el número); valen desde el próximo arranque del juego en la consola.',
    fr: 'Dis-moi « active le code … dans … » (le numéro marche aussi) ; ils valent dès le prochain lancement du jeu sur la console.',
    de: 'Sag mir „aktiviere den Cheat … in …“ (die Nummer geht auch); sie gelten ab dem nächsten Start des Spiels auf der Konsole.' },
  ch_hardcore: {
    it: '⚠ RetroAchievements è in modalità hardcore: finché resta attiva, RetroArch non applica nessun trucco.',
    en: '⚠ RetroAchievements is in hardcore mode: while it\'s on, RetroArch applies no cheats.',
    es: '⚠ RetroAchievements está en modo hardcore: mientras siga activo, RetroArch no aplica ningún truco.',
    fr: '⚠ RetroAchievements est en mode hardcore : tant qu\'il est actif, RetroArch n\'applique aucun code.',
    de: '⚠ RetroAchievements ist im Hardcore-Modus: Solange er an ist, wendet RetroArch keine Cheats an.' },
  ch_which_game: {
    it: 'Per quale gioco? Dimmi per esempio «attiva il trucco vite infinite in Contra».', en: 'For which game? Say for example “enable the infinite lives cheat in Contra”.',
    es: '¿Para qué juego? Dime por ejemplo «activa el truco vidas infinitas en Contra».', fr: 'Pour quel jeu ? Dis par exemple « active le code vies infinies dans Contra ».',
    de: 'Für welches Spiel? Sag zum Beispiel „aktiviere den Cheat unendliche Leben in Contra“.' },
  ch_nomatch: { it: 'Per {g} non trovo un trucco che corrisponda a «{w}». Eccone alcuni:', en: 'For {g} I can\'t find a cheat matching “{w}”. Here are some:', es: 'Para {g} no encuentro un truco que coincida con «{w}». Aquí tienes algunos:', fr: 'Pour {g}, je ne trouve aucun code correspondant à « {w} ». En voici quelques-uns :', de: 'Für {g} finde ich keinen Cheat zu „{w}“. Hier sind einige:' },
  ch_pick: { it: 'Quale? Per {g} ci sono:', en: 'Which one? For {g} there are:', es: '¿Cuál? Para {g} hay:', fr: 'Lequel ? Pour {g} il y a :', de: 'Welcher? Für {g} gibt es:' },
  ch_set_fail: { it: 'Non è stato possibile cambiare i trucchi: {e}', en: 'The cheats could not be changed: {e}', es: 'No se han podido cambiar los trucos: {e}', fr: 'Impossible de modifier les codes : {e}', de: 'Die Cheats konnten nicht geändert werden: {e}' },
  ch_on: { it: 'Attivato per {g}: {c}.', en: 'Enabled for {g}: {c}.', es: 'Activado para {g}: {c}.', fr: 'Activé pour {g} : {c}.', de: 'Aktiviert für {g}: {c}.' },
  ch_off: { it: 'Disattivato per {g}: {c}.', en: 'Disabled for {g}: {c}.', es: 'Desactivado para {g}: {c}.', fr: 'Désactivé pour {g} : {c}.', de: 'Deaktiviert für {g}: {c}.' },
  ch_off_all: { it: 'Tutti i trucchi di {g} sono spenti.', en: 'All cheats for {g} are off.', es: 'Todos los trucos de {g} están desactivados.', fr: 'Tous les codes de {g} sont désactivés.', de: 'Alle Cheats für {g} sind aus.' },
  ch_now_on: { it: 'Attivi adesso: {n}.', en: 'Now on: {n}.', es: 'Activos ahora: {n}.', fr: 'Actifs maintenant : {n}.', de: 'Jetzt aktiv: {n}.' },
  ch_next: { it: 'Vale dal prossimo avvio del gioco sulla console.', en: 'It applies from the game\'s next start on the console.', es: 'Vale desde el próximo arranque del juego en la consola.', fr: 'Cela vaut dès le prochain lancement du jeu sur la console.', de: 'Gilt ab dem nächsten Start des Spiels auf der Konsole.' },
  ch_restart: {
    it: 'Il gioco è aperto adesso: per farlo valere esci (SELECT+START) e riaprilo, riparte dallo stato salvato.',
    en: 'The game is open right now: to apply it, quit (SELECT+START) and open it again; it resumes from the saved state.',
    es: 'El juego está abierto ahora: para aplicarlo, sal (SELECT+START) y vuelve a abrirlo; sigue desde el estado guardado.',
    fr: 'Le jeu est ouvert en ce moment : pour l\'appliquer, quitte (SELECT+START) et rouvre-le ; il reprend depuis l\'état sauvegardé.',
    de: 'Das Spiel ist gerade offen: Damit es gilt, beende es (SELECT+START) und öffne es neu; es macht beim gespeicherten Zustand weiter.' },

  // ---- ports ----
  ports_head: { it: 'Hai {n} port:', en: 'You have {n} ports:', es: 'Tienes {n} ports:', fr: 'Tu as {n} ports :', de: 'Du hast {n} Ports:' },
  ports_more: { it: '…e altri {n}.', en: '…and {n} more.', es: '…y {n} más.', fr: '…et {n} autres.', de: '…und {n} weitere.' },
  ports_none: {
    it: 'Nella cartella `ports/` non c\'è ancora nessun port: si installano con **PortMaster** (scheda Giochi).',
    en: 'There are no ports in the `ports/` folder yet: install them with **PortMaster** (Games tab).',
    es: 'Todavía no hay ningún port en la carpeta `ports/`: se instalan con **PortMaster** (pestaña Juegos).',
    fr: 'Il n\'y a encore aucun port dans le dossier `ports/` : on les installe avec **PortMaster** (onglet Jeux).',
    de: 'Im Ordner `ports/` gibt es noch keine Ports: Installier sie mit **PortMaster** (Reiter Spiele).' },
  ports_how: {
    it: 'Si avviano dalla tessera **Ports** (scheda Giochi); nuovi port e aggiornamenti da **PortMaster**.',
    en: 'Launch them from the **Ports** tile (Games tab); new ports and updates come from **PortMaster**.',
    es: 'Se lanzan desde la casilla **Ports** (pestaña Juegos); ports nuevos y actualizaciones desde **PortMaster**.',
    fr: 'Ils se lancent depuis la tuile **Ports** (onglet Jeux) ; nouveaux ports et mises à jour via **PortMaster**.',
    de: 'Gestartet werden sie über die Kachel **Ports** (Reiter Spiele); neue Ports und Updates kommen von **PortMaster**.' },

  // ---- emulatori ----
  core_head: { it: 'Emulatori in uso:', en: 'Emulators in use:', es: 'Emuladores en uso:', fr: 'Émulateurs utilisés :', de: 'Verwendete Emulatoren:' },
  core_change: {
    it: 'Per cambiarlo: nella lista dei sistemi Y → Emulatore (tutto il sistema), oppure sul gioco Y e poi sinistra/destra (solo quel gioco).',
    en: 'To change it: in the systems list Y → Emulator (whole system), or on a game Y then left/right (that game only).',
    es: 'Para cambiarlo: en la lista de sistemas Y → Emulador (todo el sistema), o sobre el juego Y y luego izquierda/derecha (solo ese juego).',
    fr: 'Pour le changer : dans la liste des systèmes Y → Émulateur (tout le système), ou sur le jeu Y puis gauche/droite (ce jeu seulement).',
    de: 'Zum Ändern: in der Systemliste Y → Emulator (ganzes System) oder auf dem Spiel Y, dann links/rechts (nur dieses Spiel).' },

  // ---- fatti per i modelli ----
  f_open: { it: 'Gioco aperto adesso sulla console: {g}{since}.', en: 'Game open on the console right now: {g}{since}.', es: 'Juego abierto ahora en la consola: {g}{since}.', fr: 'Jeu ouvert en ce moment sur la console : {g}{since}.', de: 'Gerade auf der Konsole geöffnet: {g}{since}.' },
  f_none: { it: 'Sulla console non è aperto nessun gioco.', en: 'No game is open on the console.', es: 'No hay ningún juego abierto en la consola.', fr: 'Aucun jeu n\'est ouvert sur la console.', de: 'Auf der Konsole ist kein Spiel geöffnet.' },
  f_recent: { it: 'Giocati di recente (dal più recente): {l}.', en: 'Recently played (newest first): {l}.', es: 'Jugados recientemente (del más reciente): {l}.', fr: 'Joués récemment (du plus récent) : {l}.', de: 'Zuletzt gespielt (neueste zuerst): {l}.' },
  f_battery: { it: 'Batteria: {p}%{c}.', en: 'Battery: {p}%{c}.', es: 'Batería: {p} %{c}.', fr: 'Batterie : {p} %{c}.', de: 'Akku: {p} %{c}.' },
  f_charging: { it: ' in carica', en: ' charging', es: ' cargando', fr: ' en charge', de: ' lädt' },
  f_device: { it: 'Console: {d}.', en: 'Console: {d}.', es: 'Consola: {d}.', fr: 'Console : {d}.', de: 'Konsole: {d}.' },
  f_hint: {
    it: 'Per giocare, cercare giochi, trucchi, BIOS, salvataggi e tempo di gioco l\'utente può chiederlo direttamente ad ANIMA ("gioca a…", "cosa gioco?", "trucchi di…", "salvataggi di…", "quanto ho giocato?"): sono dati veri della console.',
    en: 'To play, find games, cheats, BIOS, saves and play time the user can ask ANIMA directly ("play…", "what should I play?", "cheats for…", "saves of…", "how long have I played?"): those come from the console\'s real data.',
    es: 'Para jugar, buscar juegos, trucos, BIOS, partidas guardadas y tiempo de juego el usuario puede pedírselo directamente a ANIMA ("juega a…", "¿a qué juego?", "trucos de…", "partidas de…", "¿cuánto he jugado?"): son datos reales de la consola.',
    fr: 'Pour jouer, trouver des jeux, des codes de triche, des BIOS, des sauvegardes et le temps de jeu, l\'utilisateur peut le demander directement à ANIMA (« joue à… », « à quoi je joue ? », « codes de… », « sauvegardes de… », « combien j\'ai joué ? ») : ce sont les vraies données de la console.',
    de: 'Zum Spielen, für Spiele, Cheats, BIOS, Spielstände und Spielzeit kann man ANIMA direkt fragen („spiele…“, „was soll ich spielen?“, „Cheats für…“, „Spielstände von…“, „wie lange habe ich gespielt?“): Das sind echte Daten der Konsole.' },
};

// minuscolo, senza accenti, apostrofi e punteggiatura come spazi: "l'app", "lapp" e "L’App" sono la stessa
// cosa per chi scrive di fretta su una tastiera virtuale. ß diventa ss ("weiß" -> "weiss"), ñ diventa n.
export function fold(s) {
  return String(s || '').replace(/ß/g, 'ss').normalize('NFKD').replace(/[̀-ͯ]/g, '').toLowerCase()
    .replace(/[’'`´]/g, ' ').replace(/[^a-z0-9%]+/g, ' ').trim().replace(/\s+/g, ' ');
}

// Il testo di una chiave nella lingua, con i segnaposto riempiti. Lingua sconosciuta -> inglese;
// chiave sconosciuta -> la chiave stessa (si vede subito nei test, non sparisce in silenzio).
export function say(lang, key, vars) {
  const e = MSG[key];
  if (!e) return key;
  const s = e[lang] != null ? e[lang] : e.en;
  return vars ? s.replace(/\{(\w+)\}/g, (m, k) => (vars[k] != null ? String(vars[k]) : m)) : s;
}
