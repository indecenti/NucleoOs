// Live-state answers read EXACTLY from /api/status (apps/anima/www/local/cascade.js liveKind/liveFromStatus).
// Regression: with the Cardputer in web mode (its brain off), "Quanto spazio libero ho sulla SD?" went to the
// PC's model, which answered "1,4 GB" on a card with 18.7 GB free. These values must never pass through a model.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { liveKind, liveFromStatus, commandHint, guessLang, liveKinds, deviceLine } from '../apps/anima/www/local/cascade.js';

test('a question asking several live values at once is split and every part answered', () => {
  // the real miss (2026-10-01): it reached a tool-less chat, which said it could not read the SD
  assert.deepEqual(liveKinds("Quanto spazio libero c'è sulla SD e da quanto è acceso il Cardputer?"), ['space', 'uptime']);
  assert.deepEqual(liveKinds('che ore sono e quanta batteria ho'), ['time', 'battery']);
  assert.deepEqual(liveKinds('what time is it and how much battery do I have'), ['time', 'battery']);
  assert.deepEqual(liveKinds('quanto spazio libero ho'), ['space']);
  assert.equal(liveKinds('che ore sono e chi è Einstein'), null, 'one clause is not a live question: the whole turn goes on as usual');
  assert.equal(liveKinds('chi è Einstein'), null);
});

test('the Cardputer named as the subject is still a live question (went to a cloud model on the ADV)', () => {
  assert.deepEqual(liveKinds('Quanta RAM libera ha il Cardputer e a che rete Wi-Fi è collegato?'), ['ram', 'network']);
  assert.deepEqual(liveKinds('che ip ha il cardputer e quanta batteria?'), ['network', 'battery']);
  assert.deepEqual(liveKinds('che rete wifi usa il cardputer e quanta batteria ha?'), ['network', 'battery']);
  assert.deepEqual(liveKinds('how much free memory does the Cardputer have?'), ['ram']);
  assert.deepEqual(liveKinds('che versione ha il cardputer'), ['version']);
  assert.deepEqual(liveKinds('cuanta bateria tiene el cardputer'), ['battery']);
  assert.equal(liveKinds("che cos'è il cardputer"), null, 'a question ABOUT the device is not a status read');
  assert.equal(liveKinds('il cardputer'), null);
});

test('the ADV web-OS battery (2026-10-10): device subject, IP / firmware wording, ", and …" clauses', () => {
  // commandHint said null for these (it did not drop the device subject like liveKind) → an LLM answered
  for (const [q, k] of [['Quanta batteria ha il Cardputer?', 'battery'], ["Quelle est l'adresse IP du Cardputer ?", 'network'],
    ['Wie lautet die IP-Adresse?', 'network'], ['¿Cuál es la dirección IP del Cardputer?', 'network'], ['What is the IP address of the device?', 'network'],
    ['Welche Firmware-Version läuft?', 'version'], ['¿Qué versión del firmware tiene el Cardputer?', 'version'], ['Quelle version du firmware tourne ?', 'version'],
    ['How much free space is left on the SD card?', 'space'], ['Combien d\'espace libre reste sur la carte SD ?', 'space'],
    ['How long has the device been on?', 'uptime'], ['¿Cuánto tiempo lleva encendido el Cardputer?', 'uptime'],
    ['Depuis combien de temps le Cardputer est allumé ?', 'uptime'], ['Wie lange läuft der Cardputer schon?', 'uptime']]) {
    assert.equal(liveKind(q), k, q);
    assert.equal(commandHint(q), 'live', q);
  }
  assert.deepEqual(liveKinds('How much free space is left on the SD card, and how long has the device been on?'), ['space', 'uptime']);
  assert.deepEqual(liveKinds('Wie viel RAM ist frei und welche Firmware-Version läuft?'), ['ram', 'version']);
  // more device wordings (2026-10-10 exact-layer run: these reached the cloud)
  assert.equal(liveKind('Wie viel Speicherplatz ist auf der SD frei?'), 'space');
  assert.equal(liveKind('Combien de batterie reste-t-il ?'), 'battery');
  assert.equal(liveKind('¿A qué red wifi estoy conectado?'), 'network');
  // still not status reads
  for (const q of ['what is an IP address', 'che versione di python devo usare', 'how long has the Eiffel tower been there']) assert.equal(liveKind(q), null, q);
  // a setting order is answered in ITS language ("Mets la luminosité à 40 %" got "Brillo al 40 %")
  for (const [q, lg] of [['Mets la luminosité à 40 %', 'fr'], ['Pon el brillo al 30', 'es'], ['Stelle die Helligkeit auf 50', 'de'],
    ['Imposta la luminosità dello schermo al 50%', 'it'], ['Turn the brightness down', 'en']]) assert.equal(guessLang(q), lg, q);
});

test('every engine gets one compact line of the live device state', () => {
  const st = { version: '0.4.0', uptime_s: 5025, free_heap: 26820, profile: 'web', storage: { mounted: true, total_bytes: 31998345216, free_bytes: 18714492928 },
    battery: { pct: 100, mv: 4152 }, network: { mode: 'sta', ssid: 'nonnoBob', ip: '192.168.0.104' } };
  assert.equal(deviceLine(st, 'it'), 'SD 18.7 GB liberi su 32.0, batteria 100%, acceso da 1 h 23 min, Wi-Fi «nonnoBob» 192.168.0.104, RAM 26 KB, modalità web (cervello offline in pausa), NucleoOS 0.4.0');
  assert.match(deviceLine(st, 'en'), /^SD 18\.7 GB free of 32\.0, battery 100%, up 1 h 23 min/);
  assert.ok(deviceLine(st, 'it').length < 200, 'small enough for every prompt');
  assert.equal(deviceLine(null), '');
});

// The Cardputer ADV's real /api/status (2026-10-01), trimmed.
const ST = {
  os: 'NucleoOS', version: '0.4.0+0.g6c81727*', uptime_s: 5025, free_heap: 26820, largest_free_block: 8192,
  storage: { mounted: true, fs: 'FAT32', total_bytes: 31998345216, free_bytes: 18714492928 },
  network: { mode: 'sta', ssid: 'nonnoBob', ip: '192.168.0.104', time_synced: true, time: 1790808000 },
  battery: { pct: 100, mv: 4152 },
};

test('liveKind maps each live question to its value', () => {
  const cases = {
    'Quanto spazio libero ho sulla SD?': 'space', 'how much free space do I have': 'space',
    'quanta batteria ho': 'battery', 'battery level': 'battery',
    'che ore sono': 'time', 'what time is it': 'time',
    'che giorno è oggi': 'date', 'in che anno siamo': 'year', 'che stagione è': 'season',
    'quanta ram libera ho': 'ram', 'uptime': 'uptime', 'che versione di nucleoos': 'version',
    'a che wifi sono connesso': 'network', 'che impegni ho oggi': 'agenda',
  };
  for (const [q, k] of Object.entries(cases)) {
    assert.equal(commandHint(q), 'live', q + ' is a live question');
    assert.equal(liveKind(q), k, q);
  }
  assert.equal(liveKind('chi è Einstein'), null);
});

test('the SD answer is the real value, in decimal GB like the desktop', () => {
  assert.equal(liveFromStatus('space', ST, 'it'), 'Sulla SD hai 18,7 GB liberi su 32,0 GB.');
  assert.equal(liveFromStatus('space', ST, 'en'), 'Your SD card has 18.7 GB free of 32.0 GB.');
  assert.equal(liveFromStatus('space', { ...ST, storage: { mounted: false } }, 'it'), 'La scheda SD non è montata.');
});

test('battery, network, RAM, uptime and version come straight from the status', () => {
  assert.equal(liveFromStatus('battery', ST, 'it'), 'Batteria al 100% (4,15 V).');
  assert.equal(liveFromStatus('battery', { ...ST, battery: undefined }, 'en'), 'This Cardputer does not report its battery level.');
  assert.equal(liveFromStatus('network', ST, 'en'), 'Connected to the Wi-Fi network “nonnoBob”, address 192.168.0.104.');
  assert.equal(liveFromStatus('network', { ...ST, network: { mode: 'off' } }, 'it'), 'Il Cardputer non è connesso a nessuna rete.');
  assert.equal(liveFromStatus('ram', ST, 'it'), 'RAM libera: 26 KB (il blocco più grande è 8 KB).');
  assert.equal(liveFromStatus('uptime', ST, 'it'), 'Il Cardputer è acceso da 1 h 23 min.');
  assert.equal(liveFromStatus('version', ST, 'de'), 'NucleoOS 0.4.0+0.g6c81727*.');
});

test('time and date use the device clock, in the reply language', () => {
  const y = new Date(ST.network.time * 1000).getFullYear();
  assert.equal(liveFromStatus('year', ST, 'it'), 'Siamo nel ' + y + '.');
  assert.match(liveFromStatus('time', ST, 'it'), /^Sono le \d{2}:\d{2}\.$/);
  assert.match(liveFromStatus('date', ST, 'fr'), /^Nous sommes le .+2026\.$/);
});

test('values the status does not carry are handed on, never invented', () => {
  assert.equal(liveFromStatus('agenda', ST, 'it'), null);
  assert.equal(liveFromStatus('season', ST, 'it'), null);
  assert.equal(liveFromStatus('space', null, 'it'), null);
});

test('routing understands all five UI languages (a missed command reached a tool-less chat)', () => {
  const live = {
    '¿Cuánto espacio libre tengo?': 'space', '¿Qué hora es?': 'time', '¿Cuánta batería me queda?': 'battery', '¿A qué wifi estoy conectado?': 'network',
    'Combien d\'espace libre il me reste ?': 'space', 'Quelle heure est-il ?': 'time', 'Quel jour sommes-nous ?': 'date', 'Quelle version de NucleoOS ?': 'version',
    'Wie viel Speicherplatz habe ich?': 'space', 'Wie spät ist es?': 'time', 'Welches Jahr haben wir?': 'year', 'Welche Jahreszeit haben wir?': 'season', 'Mit welchem WLAN bin ich verbunden?': 'network',
  };
  for (const [q, k] of Object.entries(live)) { assert.equal(commandHint(q), 'live', q); assert.equal(liveKind(q), k, q); }
  const act = ['sube el volumen', 'baja el brillo al 30', 'recuérdame llamar a Marta mañana', 'pon un temporizador de 5 minutos',
    'monte le volume', 'rappelle-moi le dentiste demain à 9h', 'mets un minuteur de 10 minutes',
    'mach die Lautstärke lauter', 'erinnere mich morgen an den Zahnarzt', 'stelle einen Wecker um 7'];
  for (const q of act) assert.equal(commandHint(q), 'act', q);
  for (const q of ['abre la calculadora', 'ouvre la calculatrice', 'öffne den Rechner']) assert.equal(commandHint(q), 'launch', q);
  // how-to questions and statements are NOT commands
  for (const q of ['¿cómo subo el volumen de mi PC?', 'comment augmenter le volume sur Windows ?', 'wie mache ich den Bildschirm heller?',
    'son las cinco', 'el volumen del libro es grande y pesado', 'chi è Einstein']) assert.notEqual(commandHint(q), 'act', q);
});

test('the answer follows the language the question was written in', () => {
  const cases = {
    'Quanto spazio libero ho sulla SD?': 'it', 'che ore sono': 'it', 'quanta batteria ho': 'it',
    'how much free space do I have': 'en', 'what time is it': 'en', 'battery level': 'en',
    '¿Cuánto espacio libre tengo?': 'es', 'qué hora es': 'es', 'cuanta bateria me queda': 'es',
    'Combien d\'espace libre il me reste ?': 'fr', 'Quelle heure est-il ?': 'fr',
    'Wie viel Speicherplatz habe ich?': 'de', 'Wie spät ist es?': 'de', 'Welches Jahr haben wir?': 'de',
    'Imposta la luminosità dello schermo del Cardputer al 50%': 'it',   // answered "Brillo al 50 %" on the ADV
    'alza il volume': 'it', 'baja el volumen': 'es', 'pon el brillo al 50': 'es',
  };
  for (const [q, l] of Object.entries(cases)) assert.equal(guessLang(q), l, q);
  assert.equal(guessLang('uptime'), null, 'no cue: the OS language is kept');
  assert.equal(liveFromStatus('space', ST, guessLang('Wie viel Speicherplatz habe ich?')), 'Deine SD-Karte hat 18,7 GB frei von 32,0 GB.');
});

test('all five languages answer', () => {
  for (const l of ['it', 'en', 'es', 'fr', 'de']) assert.ok(/18[.,]7/.test(liveFromStatus('space', ST, l)), l);
});

test('a volume / brightness order is read without an engine (web mode said "not carried out")', async () => {
  const { settingAct, settingReply } = await import('../apps/anima/www/local/cascade.js');
  const arg = (q) => { const a = settingAct(q); return a && a.tool + ' ' + a.arg; };
  assert.equal(arg('Imposta la luminosità dello schermo del Cardputer al 50%'), 'set_brightness 50');
  assert.equal(arg('alza il volume'), 'set_volume +10');
  assert.equal(arg('abbassa la luminosità'), 'set_brightness -10');
  assert.equal(arg('volume al massimo'), 'set_volume 100');
  assert.equal(arg('silenzia il volume'), 'set_volume 0');
  assert.equal(arg('set the brightness to 30 percent'), 'set_brightness 30');
  assert.equal(arg('baja el volumen'), 'set_volume -10');
  assert.equal(arg('stelle die Helligkeit auf 80'), 'set_brightness 80');
  assert.equal(settingAct('come alzo il volume del mio PC?'), null, 'a how-to is not an order');
  assert.equal(settingAct('cambia il volume'), null, 'no amount, no direction: not ours to guess');
  assert.equal(settingAct("l'audio del film era basso ieri sera"), null);
  // a CODE request naming the screen is not a setting (it set the backlight to 96% on the ADV)
  assert.equal(settingAct('Ora modifica contatore/www/index.html: il numero deve essere grande (almeno 96px) e centrato orizzontalmente e verticalmente nello schermo, con i pulsanti − e + centrati sotto. Poi ripubblica l\'app.'), null);
  assert.equal(commandHint('metti il testo al centro dello schermo in style.css'), null);
  assert.equal(commandHint('rendi lo schermo più grande con font 20px'), null);
  assert.equal(settingReply('set_brightness', 'Luminosita al 50%.', 'it'), 'Luminosità al 50%.');
  assert.equal(settingReply('set_volume', 'Volume 100%.', 'de'), 'Lautstärke 100 %.');
});
