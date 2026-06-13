import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const htmlPath = path.resolve('apps/terminal/www/index.html');
const html = fs.readFileSync(htmlPath, 'utf8');
const scriptMatch = html.match(/<script type="module">([\s\S]*?)<\/script>/);
if (!scriptMatch) {
  throw new Error('Could not find script block in index.html');
}

let scriptCode = scriptMatch[1];

// Taglia l'auto-avvio e il boot finale per evitare top-level await SyntaxError e chiamate sul DOM finto
const bootIndex = scriptCode.indexOf('// ---- boot ------------------------------------------------------------------');
if (bootIndex !== -1) {
  scriptCode = scriptCode.slice(0, bootIndex);
}

// The app's module script imports the OS i18n engine (`import I18N from '/nucleo-i18n.js'`) and inits it
// with a top-level await. vm.runInContext runs a SCRIPT (not a module): strip the import(s) and de-await
// the init — the sandbox below provides an I18N stub (t() passthrough). Isolates the command logic.
scriptCode = scriptCode
  .replace(/^[ \t]*import\b[^\n]*\n/gm, '')
  .replace(/\bawait\s+(I18N\.init\s*\()/g, '$1');

// Espone let/const all'oggetto globale del sandbox VM per poterli testare
scriptCode = scriptCode
  .replace('const COMMANDS =', 'globalThis.COMMANDS =')
  .replace('let cwd =', 'globalThis.cwd =')
  .replace('const norm =', 'globalThis.norm =')
  .replace('const api =', 'globalThis.api =')
  .replace('const fmtBytes =', 'globalThis.fmtBytes =')
  .replace('const setCwd =', 'globalThis.setCwd =')
  .replace('const hist =', 'globalThis.hist =')
  .replace('const envVars =', 'globalThis.envVars =')
  .replace('async function run(', 'globalThis.run = async function (');

// Preparazione dei mock del DOM e delle chiamate di rete
const emitted = [];
const mockedFetch = {};
const mockedFs = {};
const postedMessages = [];

const domElementMock = {
  appendChild: () => {},
  addEventListener: () => {},
  focus: () => {},
  set textContent(val) {},
  get textContent() { return ''; },
  set value(val) {},
  get value() { return ''; },
  set onclick(fn) {},
  style: {},
  dataset: {}
};

const sandbox = {
  document: {
    documentElement: { dataset: {} },
    createElement: (tag) => {
      return {
        className: '',
        textContent: ''
      };
    },
    getElementById: (id) => {
      if (id === 'out') {
        return {
          appendChild: (el) => {
            emitted.push({ text: el.textContent, cls: el.className });
          },
          addEventListener: () => {},
          set scrollTop(v) {},
          get scrollHeight() { return 100; }
        };
      }
      return domElementMock;
    }
  },
  window: {
    addEventListener: () => {}
  },
  parent: {
    postMessage: (msg) => {
      postedMessages.push(msg);
    }
  },
  performance: {
    now: () => 1000
  },
  Blob: class {
    constructor(parts) {
      this.size = parts.join('').length;
    }
  },
  fetch: async (url) => {
    if (mockedFetch[url]) {
      const val = mockedFetch[url];
      const isObj = typeof val === 'object';
      return {
        ok: true,
        status: 200,
        text: async () => isObj ? JSON.stringify(val) : String(val),
        json: async () => isObj ? val : JSON.parse(val),
        arrayBuffer: async () => new TextEncoder().encode(isObj ? JSON.stringify(val) : String(val)).buffer
      };
    }
    const urlObj = new URL(url, 'http://localhost');
    const pathParam = urlObj.searchParams.get('path');

    if (urlObj.pathname === '/api/status') {
      return {
        ok: true,
        status: 200,
        json: async () => mockedFetch['/api/status'] || {}
      };
    }

    if (urlObj.pathname === '/api/logs') {
      return {
        ok: true,
        status: 200,
        text: async () => mockedFetch['/api/logs'] || 'kernel logs'
      };
    }

    if (urlObj.pathname === '/api/reboot') {
      return {
        ok: true,
        status: 200,
        json: async () => ({ ok: true })
      };
    }

    if (urlObj.pathname === '/api/fs/list') {
      if (mockedFs[pathParam]) {
        return {
          ok: true,
          status: 200,
          json: async () => ({ entries: mockedFs[pathParam] })
        };
      }
      return { ok: false, status: 404, text: async () => 'no dir' };
    }

    if (urlObj.pathname === '/api/fs/read') {
      if (mockedFs[pathParam]) {
        return {
          ok: true,
          status: 200,
          text: async () => mockedFs[pathParam],
          arrayBuffer: async () => new TextEncoder().encode(mockedFs[pathParam]).buffer
        };
      }
      return { ok: false, status: 404, text: async () => 'no file' };
    }

    if (urlObj.pathname === '/api/fs/write') {
      return { ok: true, status: 200 };
    }

    if (urlObj.pathname === '/api/fs/delete') {
      return { ok: true, status: 200 };
    }

    return {
      ok: true,
      status: 200,
      text: async () => '',
      json: async () => ({})
    };
  },
  setTimeout: (fn) => { fn(); return 0; },
  setInterval: () => 0,
  console: console,
  // OS i18n engine stub: init() returns a t() passthrough (returns a string fallback arg if present, else the key).
  I18N: { init: () => ((k, ...rest) => { for (const a of rest) if (typeof a === 'string') return a; return String(k); }), setLang() {}, onChange() {}, getLang: () => 'it' },
};

const context = vm.createContext(sandbox);
vm.runInContext(scriptCode, context);

const { COMMANDS, norm, cwd, setCwd, fmtBytes, hist, envVars, run } = sandbox;

function resetTest() {
  emitted.length = 0;
  postedMessages.length = 0;
  setCwd('/');
  if (hist) hist.length = 0;
  if (envVars) {
    for (const k of Object.keys(envVars)) delete envVars[k];
    Object.assign(envVars, { SHELL: '/bin/sh', PATH: '/apps/bin', USER: 'admin', OS: 'NucleoOS', TERM: 'xterm-color', CWD: '/' });
  }
  // Mock del file manuale .info
  mockedFs['/system/manual/ls.info'] = JSON.stringify({
    id: 'ls',
    category: 'terminal',
    it: {
      title: 'ls - Elenca cartella',
      synopsis: 'ls [percorso]',
      description: 'Mostra i file.',
      details: 'Dettagli in italiano.'
    },
    en: {
      title: 'ls - List directory',
      synopsis: 'ls [path]',
      description: 'List directory contents.',
      details: 'Details in english.'
    }
  });
}

test('norm: normalizza percorsi relativi e assoluti', () => {
  setCwd('/data');
  assert.equal(norm('Music'), '/data/Music');
  assert.equal(norm('../system'), '/system');
  assert.equal(norm('/system/config'), '/system/config');
});

test('ls: elenca i file di una cartella esistente', async () => {
  resetTest();
  mockedFs['/'] = [
    { name: 'data', type: 'dir' },
    { name: 'test.txt', type: 'file', size: 100 }
  ];

  await COMMANDS.ls();
  assert.equal(emitted.length, 3);
  assert.ok(emitted[0].text.includes('data/'));
  assert.ok(emitted[1].text.includes('test.txt'));
  assert.ok(emitted[2].text.includes('2 items'));
});

test('ls: restituisce errore per cartella inesistente', async () => {
  resetTest();
  mockedFs['/ghost'] = null;

  await COMMANDS.ls('/ghost');
  assert.equal(emitted.length, 1);
  assert.ok(emitted[0].text.includes('ls: no such directory'));
});

test('grep: trova le corrispondenze all\'interno di un file', async () => {
  resetTest();
  mockedFs['/data/test.txt'] = 'line one\nSSID=MyWifi\nline three';

  await COMMANDS.grep('SSID /data/test.txt');
  assert.equal(emitted.length, 1);
  assert.ok(emitted[0].text.includes('2: SSID=MyWifi'));
});

test('find: cerca file ricorsivamente', async () => {
  resetTest();
  mockedFs['/'] = [
    { name: 'data', type: 'dir' },
    { name: 'index.html', type: 'file' }
  ];
  mockedFs['/data'] = [
    { name: 'music', type: 'dir' }
  ];
  mockedFs['/data/music'] = [
    { name: 'song.mp3', type: 'file' }
  ];

  await COMMANDS.find('song');
  assert.equal(emitted.length, 3);
  assert.ok(emitted[0].text.includes('searching for "song"'));
  assert.equal(emitted[1].text, '/data/music/song.mp3');
  assert.ok(emitted[2].text.includes('found 1 item'));
});

test('wc: conta linee, parole e byte', async () => {
  resetTest();
  mockedFs['/data/test.txt'] = 'Hello world\nThis is a test';

  await COMMANDS.wc('/data/test.txt');
  assert.equal(emitted.length, 1);
  assert.ok(emitted[0].text.includes('2 lines, 6 words, 26 bytes'));
});

test('head: mostra le prime N righe', async () => {
  resetTest();
  mockedFs['/data/test.txt'] = '1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12';

  await COMMANDS.head('-n 3 /data/test.txt');
  assert.equal(emitted.length, 1);
  assert.equal(emitted[0].text, '1\n2\n3');
});

test('tail: mostra le ultime N righe', async () => {
  resetTest();
  mockedFs['/data/test.txt'] = '1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12';

  await COMMANDS.tail('-n 3 /data/test.txt');
  assert.equal(emitted.length, 1);
  assert.equal(emitted[0].text, '10\n11\n12');
});

test('df: mostra l\'utilizzo del disco', async () => {
  resetTest();
  mockedFetch['/api/status'] = {
    storage: {
      mounted: true,
      fs: 'FAT',
      total_bytes: 4000000000,
      free_bytes: 1000000000
    }
  };

  await COMMANDS.df();
  assert.equal(emitted.length, 2);
  assert.ok(emitted[0].text.includes('Filesystem'));
  assert.ok(emitted[1].text.includes('SDCard'));
  assert.ok(emitted[1].text.includes('75%'));
});

test('uptime: mostra l\'uptime del dispositivo', async () => {
  resetTest();
  mockedFetch['/api/status'] = {
    uptime_s: 7265, // 2h 1m 5s
    free_heap: 120000
  };

  await COMMANDS.uptime();
  assert.equal(emitted.length, 1);
  assert.ok(emitted[0].text.includes('up 2h 1m 5s'));
  assert.ok(emitted[0].text.includes('120.0 KB'));
});

test('env: mostra le variabili d\'ambiente', () => {
  resetTest();
  COMMANDS.env();
  assert.ok(emitted.some(e => e.text.includes('OS=NucleoOS')));
  assert.ok(emitted.some(e => e.text.includes('SHELL=/bin/sh')));
});

test('cp: copia correttamente un file', async () => {
  resetTest();
  mockedFs['/data/orig.txt'] = 'copiame';

  await COMMANDS.cp('/data/orig.txt /data/dest.txt');
  assert.equal(emitted.length, 2);
  assert.ok(emitted[0].text.includes('copying /data/orig.txt'));
  assert.ok(emitted[1].text.includes('copied successfully'));
});

test('mv: sposta correttamente un file', async () => {
  resetTest();
  mockedFs['/data/orig.txt'] = 'spostame';

  await COMMANDS.mv('/data/orig.txt /data/dest.txt');
  assert.equal(emitted.length, 2);
  assert.ok(emitted[0].text.includes('moving /data/orig.txt'));
  assert.ok(emitted[1].text.includes('moved successfully'));
});

test('history: visualizza lo storico dei comandi', () => {
  resetTest();
  hist.push('ls');
  hist.push('pwd');

  COMMANDS.history();
  assert.equal(emitted.length, 2);
  assert.ok(emitted[0].text.includes('1  ls'));
  assert.ok(emitted[1].text.includes('2  pwd'));
});

test('uname: stampa le informazioni di sistema', async () => {
  resetTest();
  mockedFetch['/api/status'] = { os: 'NucleoOS', version: '0.1.0' };

  await COMMANDS.uname();
  assert.equal(emitted.length, 1);
  assert.equal(emitted[0].text, 'NucleoOS');

  resetTest();
  await COMMANDS.uname('-a');
  assert.equal(emitted.length, 1);
  assert.ok(emitted[0].text.includes('esp32s3'));
});

test('curl: scarica e mostra testo da URL', async () => {
  resetTest();
  mockedFetch['https://example.com'] = 'hello example.com';

  await COMMANDS.curl('example.com');
  assert.equal(emitted.length, 2);
  assert.ok(emitted[0].text.includes('fetching https://example.com'));
  assert.equal(emitted[1].text, 'hello example.com');
});

test('wget: scarica un file e lo scrive sulla SD', async () => {
  resetTest();
  mockedFetch['https://example.com/file.txt'] = 'file download';

  await COMMANDS.wget('example.com/file.txt -O /data/downloaded.txt');
  assert.equal(emitted.length, 3);
  assert.ok(emitted[0].text.includes('downloading https://example.com/file.txt'));
  assert.ok(emitted[1].text.includes('writing to /data/downloaded.txt'));
  assert.ok(emitted[2].text.includes('saved successfully'));
});

test('sleep: esegue un ritardo corretto', async () => {
  resetTest();
  const start = Date.now();
  await COMMANDS.sleep('0.05');
  const elapsed = Date.now() - start;
  assert.equal(emitted.length, 1);
  assert.ok(emitted[0].text.includes('sleeping for 0.05s'));
  assert.ok(elapsed >= 0);
});

test('dmesg: visualizza i log di sistema', async () => {
  resetTest();
  mockedFetch['/api/logs'] = 'ESP32 system booted';

  await COMMANDS.dmesg();
  assert.equal(emitted.length, 1);
  assert.equal(emitted[0].text, 'ESP32 system booted');
});

test('reboot: esegue il riavvio del dispositivo', async () => {
  resetTest();
  await COMMANDS.reboot();
  assert.equal(emitted.length, 2);
  assert.ok(emitted[0].text.includes('rebooting Cardputer'));
  assert.ok(emitted[1].text.includes('Reboot command sent'));
});

test('export: esporta e aggiorna variabili d\'ambiente', () => {
  resetTest();
  COMMANDS.export('MY_DIR=/sd/downloads');
  assert.equal(envVars.MY_DIR, '/sd/downloads');

  resetTest();
  COMMANDS.export();
  assert.ok(emitted.some(e => e.text.includes('declare -x USER="admin"')));
});

test('man: mostra la pagina di manuale dei comandi', async () => {
  resetTest();
  // Test in inglese (default)
  await COMMANDS.man('ls');
  assert.ok(emitted.length >= 1);
  assert.ok(emitted[0].text.includes('LS(1) - ls [path]'));
  assert.ok(emitted.some(e => e.text.includes('List directory contents.')));

  // Test in italiano (tramite LANG variabile d'ambiente)
  resetTest();
  envVars.LANG = 'it_IT.UTF-8';
  await COMMANDS.man('ls');
  assert.ok(emitted.length >= 1);
  assert.ok(emitted[0].text.includes('LS(1) - ls [percorso]'));
  assert.ok(emitted.some(e => e.text.includes('Mostra i file.')));
});

test('run: effettua correttamente l\'espansione delle variabili d\'ambiente', async () => {
  resetTest();
  envVars.TARGET_DIR = '/system/config';
  mockedFs['/system/config'] = [];

  await run('cd $TARGET_DIR');
  assert.equal(sandbox.cwd, '/system/config');
});
