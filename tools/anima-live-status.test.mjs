// Live-state answers read EXACTLY from /api/status (apps/anima/www/local/cascade.js liveKind/liveFromStatus).
// Regression: with the Cardputer in web mode (its brain off), "Quanto spazio libero ho sulla SD?" went to the
// PC's model, which answered "1,4 GB" on a card with 18.7 GB free. These values must never pass through a model.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { liveKind, liveFromStatus, commandHint, guessLang } from '../apps/anima/www/local/cascade.js';

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
  };
  for (const [q, l] of Object.entries(cases)) assert.equal(guessLang(q), l, q);
  assert.equal(guessLang('uptime'), null, 'no cue: the OS language is kept');
  assert.equal(liveFromStatus('space', ST, guessLang('Wie viel Speicherplatz habe ich?')), 'Deine SD-Karte hat 18,7 GB frei von 32,0 GB.');
});

test('all five languages answer', () => {
  for (const l of ['it', 'en', 'es', 'fr', 'de']) assert.ok(/18[.,]7/.test(liveFromStatus('space', ST, l)), l);
});
