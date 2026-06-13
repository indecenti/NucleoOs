// Host check for the ANIMA workspace NL parser (apps/anima/www/nlfs.js) and the pure
// helpers in fsclient.js. Pure modules — no DOM, no network — so they import straight into
// Node. Run: node tools/anima-host/nlfs-check.mjs
import { parseFileIntent } from '../../apps/anima/www/nlfs.js';
import { normPath, globToRegExp, diffStat, isTextName } from '../../apps/anima/www/fsclient.js';

let pass = 0, fail = 0;
const fails = [];
// expected: null OR a partial object; every listed key must deep-equal the parsed intent.
function t(input, expected) {
  const got = parseFileIntent(input);
  let ok;
  if (expected === null) ok = got === null;
  else if (!got) ok = false;
  else ok = Object.keys(expected).every((k) => JSON.stringify(got[k]) === JSON.stringify(expected[k]));
  if (ok) pass++; else { fail++; fails.push({ input, expected, got }); }
}

// ---------- READ ----------
t('leggi note.txt', { op: 'read', path: 'note.txt' });
t('apri config.json', { op: 'read', path: 'config.json' });
t('open src/main.c', { op: 'read', path: 'src/main.c' });
t('mostra README.md', { op: 'read', path: 'README.md' });
t('cat package.json', { op: 'read', path: 'package.json' });
t('apri il file README', { op: 'read', path: 'README' });
t('dammi il contenuto di diario.txt', { op: 'read', path: 'diario.txt' });
// trap: app launches and knowledge — NOT file ops
t('apri la calcolatrice', null);
t('apri le foto', null);
t('apri le impostazioni', null);

// ---------- CREATE / WRITE ----------
t('crea note.txt', { op: 'write', path: 'note.txt', content: '' });
t('crea diario.txt con scritto ciao mondo', { op: 'write', path: 'diario.txt', content: 'ciao mondo', mode: 'literal' });
t('scrivi nel file todo.md: comprare il latte', { op: 'write', path: 'todo.md', content: 'comprare il latte' });
t('create report.txt with the text Hello world', { op: 'write', path: 'report.txt', content: 'Hello world', mode: 'literal' });
t('salva appunti.md con contenuto la riunione di oggi', { op: 'write', path: 'appunti.md', mode: 'auto' });
t('nuovo file diario/2026.md', { op: 'write', path: 'diario/2026.md' });
t('crea "le mie note.txt" con scritto test', { op: 'write', path: 'le mie note.txt', content: 'test' });

// ---------- MKDIR ----------
t('crea cartella progetti', { op: 'mkdir', path: 'progetti' });
t('mkdir src', { op: 'mkdir', path: 'src' });
t('nuova cartella docs', { op: 'mkdir', path: 'docs' });
t('crea la directory build/out', { op: 'mkdir', path: 'build/out' });

// ---------- DELETE ----------
t('elimina vecchio.txt', { op: 'delete', path: 'vecchio.txt' });
t('cancella la cartella tmp', { op: 'delete', path: 'tmp' });
t('rm build.log', { op: 'delete', path: 'build.log' });
t('rimuovi src/old.c', { op: 'delete', path: 'src/old.c' });
// trap: not files
t('cancella la conversazione', null);
t('elimina l’evento di domani', null);

// ---------- RENAME / MOVE ----------
t('rinomina a.txt in b.txt', { op: 'move', from: 'a.txt', to: 'b.txt', rename: true });
t('rename old.md to new.md', { op: 'move', from: 'old.md', to: 'new.md' });
t('sposta foto.jpg in Pictures/foto.jpg', { op: 'move', from: 'foto.jpg', to: 'Pictures/foto.jpg' });
t('move src/a.c to lib/a.c', { op: 'move', from: 'src/a.c', to: 'lib/a.c' });
t('mv x.txt archivio/x.txt', { op: 'move', from: 'x.txt', to: 'archivio/x.txt' });
// rename keeping dir
t('rinomina src/a.txt in b.txt', { op: 'move', from: 'src/a.txt', to: 'src/b.txt', rename: true });

// ---------- APPEND ----------
t('aggiungi una riga finale a log.txt', { op: 'append', path: 'log.txt', content: 'una riga finale' });
t('append Done to status.txt', { op: 'append', path: 'status.txt', content: 'Done' });
// trap: calendar / math
t('aggiungi un evento domani alle 16', null);
t('aggiungi 2 e 3', null);

// ---------- EDIT ----------
t('sostituisci "foo" con "bar" in main.c', { op: 'edit', path: 'main.c', old: 'foo', new: 'bar' });
t('replace cat with dog in story.txt', { op: 'edit', path: 'story.txt', old: 'cat', new: 'dog' });

// ---------- LIST / TREE / CD ----------
t('elenca i file', { op: 'list', path: '.' });
t('ls src', { op: 'list', path: 'src' });
t('mostra i file in Documents', { op: 'list', path: 'Documents' });
t('cosa c’è in /data/anima', { op: 'list', path: '/data/anima' });
t('mostra l’albero del progetto', { op: 'tree', path: '.' });
t('struttura della cartella src', { op: 'tree' });
t('vai in src', { op: 'cd', path: 'src' });
t('cd ..', { op: 'cd', path: '..' });

// ---------- RUN (only a runnable .js/.mjs/.cjs path; else falls through to ANIMA) ----------
t('esegui hello.js', { op: 'run', path: 'hello.js' });
t('run scripts/build.mjs', { op: 'run', path: 'scripts/build.mjs' });
t('node tools/gen.cjs', { op: 'run', path: 'tools/gen.cjs' });
t('esegui report.txt', null);          // not a runnable extension -> not a file op
t('lancia la calcolatrice', null);     // app launch, no script path
t('esegui il backup', null);           // no path at all

// ---------- SEARCH / GLOB ----------
t('cerca TODO nei file', { op: 'search', query: 'TODO' });
t('grep funzione', { op: 'search', query: 'funzione' });
t('trova "import" nel codice', { op: 'search', query: 'import' });
t('trova il file config.json', { op: 'glob', pattern: '**/config.json' });
t('trova tutti i .js', { op: 'glob', pattern: '**/*.js' });
t('all .txt files', { op: 'glob', pattern: '**/*.txt' });
t('*.md', { op: 'glob', pattern: '**/*.md' });
// trap: knowledge questions that start with cerca/trova
t('cerca chi è Einstein', null);
t('trova la capitale della Francia', null);

// ---------- non-file passthrough ----------
t('che ore sono', null);
t('quanto fa 12% di 80', null);
t('meteo brescia', null);
t('chi ha scritto la Divina Commedia', null);

// ---------- pure helpers ----------
function eq(name, got, exp) { const ok = JSON.stringify(got) === JSON.stringify(exp); if (ok) pass++; else { fail++; fails.push({ input: name, expected: exp, got }); } }
eq('normPath a/b/../c', normPath('/data/a/b/../c'), '/data/a/c');
eq('normPath //x//', normPath('/data//x//'), '/data/x');
eq('normPath dotdot root', normPath('/a/../..'), '/');
eq('glob *.js match', globToRegExp('**/*.js').test('src/deep/main.js'), true);
eq('glob *.js nomatch', globToRegExp('**/*.js').test('src/main.ts'), false);
eq('glob seg *', globToRegExp('*.txt').test('a/b.txt'), true);
eq('diffStat', diffStat('a\nb\nc', 'a\nB\nc\nd'), { added: 2, removed: 1 });
eq('isText md', isTextName('readme.md'), true);
eq('isText png', isTextName('photo.png'), false);
eq('isText Makefile', isTextName('Makefile'), true);

// ---------- report ----------
console.log(`\nnlfs-check: ${pass} passed, ${fail} failed (of ${pass + fail})`);
if (fails.length) {
  console.log('\nFAILURES:');
  for (const f of fails) console.log('  input:', JSON.stringify(f.input), '\n    expected:', JSON.stringify(f.expected), '\n    got:     ', JSON.stringify(f.got));
  process.exit(1);
}
console.log('all green ✓');
