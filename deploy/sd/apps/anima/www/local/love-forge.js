/* SPDX-License-Identifier: AGPL-3.0-or-later */
// ANIMA — la fucina dei giochi: scrive un gioco LÖVE per la console, lo installa, lo prova, lo corregge.
//
// La console ha LÖVE 11.4 (/opt/love2d, lanciato da love.sh per il sistema love2d, file .love). Un .love è
// uno zip: dentro ci mettiamo il gioco (game.lua, scritto dal modello), il kit (nucleo.lua: comandi,
// schermo, suoni...), i suoni del catalogo che il gioco usa davvero, e un'IMBRACATURA (main.lua) che:
//   - carica il gioco dentro un pcall: un errore di sintassi diventa un messaggio, non uno schermo nero;
//   - scrive un registro accanto al gioco, in <cartella del .love>/.anima/<identità>/ (sulla scheda: il
//     browser lo legge con /api/fs/read). NON nella cartella di salvataggio di LÖVE: su queste console
//     love.filesystem.write lì fallisce senza errore (torna false; misurato il 21/10/2026);
//   - in modalità PROVA fa girare il gioco qualche secondo, scatta uno screenshot, misura gli FPS ed esce
//     da sola, tornando alla console;
//   - se il gioco va in errore mostra l'errore per pochi secondi ed esce (niente schermata blu bloccata).
// Il ciclo: modello -> pacchetto -> /api/fs/write -> /api/library/launch -> registro -> se errore, il modello
// lo corregge con il messaggio e la riga -> di nuovo, fino a `tries` volte. Poi si installa la versione da
// gioco (senza prova) e la si offre nella libreria.
//
// Puro: niente DOM, niente fetch. `llm` e `dev` sono iniettati, così si prova in Node con una console finta.

export const SYS = 'love2d';
export const LOVE_VERSION = '11.4';

// "Snake al neon!" -> "snake-al-neon"
export function slug(s) {
  const x = String(s || '').normalize('NFKD').replace(/[̀-ͯ]/g, '').toLowerCase()
    .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32).replace(/-+$/, '');
  return x || 'gioco';
}

// ---- il prompt: cosa sa il modello della console ----------------------------------------------------------
const KIT_DOC = [
  'local N = require("nucleo")  -- the NucleoOS game kit, ALWAYS use it:',
  'Screen: N.W, N.H (real pixels: 640x480 OR 720x720, never assume), N.U (1/100 of the short side: size things in N.U), N.cx, N.cy (centre).',
  '  Fixed-size world instead: N.fit(320,240) in love.load, then in love.draw N.push() ... draw in 0..320 x 0..240 ... N.pop().',
  'Input (handheld buttons): N.down(b) held, N.pressed(b) just pressed this frame, N.released(b); b = "a","b","x","y","start","select","l1","r1","l2","r2","up","down","left","right".',
  '  N.axis() -> dx,dy in -1..1 (d-pad or stick). A = confirm/jump/fire, B = back/secondary, START = pause. NEVER use love.keyboard or love.joystick directly. NEVER quit the game yourself: SELECT+START exits (system).',
  'Sound: N.sfx(id [,vol,pitch]) plays a catalog effect; N.music(id [,vol]) loops a catalog track, N.music() stops; N.beep(freq,dur,"square"|"sine"|"saw"|"noise",vol,slide) synth.',
  'Shapes: N.rect(x,y,w,h[,color]), N.circle(x,y,r[,color]), N.ellipse, N.triangle(x1,y1,x2,y2,x3,y3[,color]), N.polygon(...), N.line(...[,color]) — "fill" by default, or pass "line" first; the optional last argument is a colour (palette name or {r,g,b}). N.translate, N.rotate, N.scale inside love.graphics.push/pop. Plain love.graphics.* works too.',
  'Text/colour: N.text(str,x,y,sizeInU,"left"|"center"|"right",color); N.color(name or {r,g,b}); palette names: black navy purple green brown gray silver white red orange yellow lime blue lavender pink peach.',
  'FX: N.burst(x,y,color,n) particles, N.shake(amount) screen shake (both drawn automatically), N.rumble(0.5,0.2).',
  'Time: N.after(sec,fn), N.every(sec,fn), N.cancel(t), N.tween(obj,sec,{x=..},"out"|"inout"|"back"|"bounce"|"linear",onDone).',
  'Math: N.time() (seconds, for durations), N.clamp, N.lerp, N.dist(x1,y1,x2,y2), N.rand(a,b), N.choose(list), N.overlap(rectA,rectB) with {x,y,w,h}, N.circles(a,b) with {x,y,r}.',
  'Save: N.save(key,value), N.load(key,default) (high scores survive power-off).',
].join('\n');

const RULES = [
  'You write ONE complete LÖVE ' + LOVE_VERSION + ' game (Lua 5.1 / LuaJIT) for a NucleoOS retro handheld. The file is game.lua.',
  'Define love.load, love.update(dt), love.draw (and nothing else of love.*: no love.run, no love.errorhandler, no love.conf, no love.keypressed).',
  'Structure: a title screen ("press A"), the game, a game-over screen with the score and the best score (N.save/N.load), restart with A.',
  'Draw everything with love.graphics shapes (rectangle, circle, polygon, line) and N.text: there are NO image or font files. Use only catalog sound ids.',
  'Performance: weak ARM GPU. No shaders, no canvases created per frame, keep it under ~500 shapes per frame, no love.window.setMode.',
  'Lua 5.1: no // operator, no bitwise operators (use math.floor), no goto, no integer division. Declare locals. Never index a nil table.',
  'Make it FUN and polished: juice (particles, shake, sounds on every action), clear readable text sized in N.U, increasing difficulty.',
  'Reply with ONLY the code, in a single ```lua block. No explanations.',
].join('\n');

export function catalogDoc(cat) {
  if (!cat) return '';
  const s = (cat.sfx || []).map((e) => e.id + ' (' + e.desc.split(':')[0] + ')').join(', ');
  const m = (cat.music || []).map((e) => e.id + ' (' + e.desc.split(':')[0] + ')').join(', ');
  return 'Sound effects for N.sfx: ' + s + '.\nMusic loops for N.music: ' + m + '.';
}
// lang: la lingua di chi chiede. Un gioco chiesto in italiano con "PRESS A TO START" (successo davvero) stona.
const LANG_NAME = { it: 'Italian', en: 'English', es: 'Spanish', fr: 'French', de: 'German' };
export function systemPrompt(cat, lang) {
  const say = LANG_NAME[lang] ? '\nAll on-screen text (title, hints, score, game over) in ' + LANG_NAME[lang] + '.' : '';
  return RULES + say + '\n\n' + KIT_DOC + '\n\n' + catalogDoc(cat);
}

// Il codice dalla risposta del modello: il blocco ```lua (o il primo blocco, o tutto se sembra Lua)
export function extractLua(text) {
  const t = String(text || '');
  const m = t.match(/```(?:lua)?[ \t]*\r?\n([\s\S]*?)```/i) || t.match(/```(?:lua)?[ \t]*\r?\n([\s\S]*)$/i);
  if (m) return m[1].trim() + '\n';
  if (/\bfunction\s+love\.(load|update|draw)\b/.test(t)) return t.trim() + '\n';
  return '';
}
// ---- Lua senza stringhe né commenti: le parole chiave, con la loro posizione nel testo --------------------
function luaWords(code) {
  const out = [], s = String(code);
  let i = 0;
  while (i < s.length) {
    const c = s[i];
    if (c === '-' && s[i + 1] === '-') {                       // commento (anche --[[ ... ]])
      const m = s.slice(i + 2).match(/^\[(=*)\[/);
      if (m) { const end = s.indexOf(']' + m[1] + ']', i + 2 + m[0].length); i = end < 0 ? s.length : end + m[1].length + 2; }
      else { const nl = s.indexOf('\n', i); i = nl < 0 ? s.length : nl; }
      continue;
    }
    if (c === '"' || c === "'") { let j = i + 1; while (j < s.length && s[j] !== c && s[j] !== '\n') j += s[j] === '\\' ? 2 : 1; i = j + 1; continue; }
    if (c === '[') { const m = s.slice(i).match(/^\[(=*)\[/); if (m) { const end = s.indexOf(']' + m[1] + ']', i + m[0].length); i = end < 0 ? s.length : end + m[1].length + 2; continue; } }
    if (/[A-Za-z_]/.test(c)) { let j = i; while (j < s.length && /\w/.test(s[j])) j++; out.push({ w: s.slice(i, j), at: i }); i = j; continue; }
    if (c === ':' && s[i + 1] === ':') { const m = s.slice(i).match(/^::\s*([A-Za-z_]\w*)\s*::/); if (m) { out.push({ w: '::', label: m[1], at: i }); i += m[0].length; continue; } }
    i++;
  }
  return out;
}
// `goto continue` senza `::continue::`: l'errore più comune dei modelli (Lua non ha `continue`; LuaJIT ha
// goto, ma l'etichetta va messa). Si aggiusta da sé, senza un altro giro del modello: l'etichetta va come
// ultima istruzione del ciclo che contiene il goto, subito prima della sua `end` (in fondo al blocco Lua la
// permette anche dopo le local). Solo cicli for/while; repeat...until resta al modello.
// Conta il BLOCCO, non solo l'esistenza: un'etichetta vale solo nel blocco in cui sta. Il caso vero (21/10/2026):
// il modello scriveva `::continue::` DENTRO l'if della collisione, e il goto del ciclo non la vedeva: "undefined
// label" tre volte di fila. Per ogni goto si guarda se l'etichetta è visibile da lì (nel suo blocco o in uno che
// lo contiene, senza uscire dalla funzione); se no, la si mette in fondo al ciclo che contiene il goto e si
// tolgono le copie fuori posto con lo stesso nome dentro quel ciclo (sarebbero un'etichetta doppia).
export function fixContinue(code) {
  const toks = luaWords(code);
  if (!toks.some((t) => t.w === 'goto')) return code;
  const root = { kind: 'chunk', parent: null };
  const stack = [root], gotos = [], labels = [];
  let pendingDo = 0;
  const push = (kind, at) => { const b = { kind, parent: stack[stack.length - 1], start: at }; stack.push(b); return b; };
  for (let k = 0; k < toks.length; k++) {
    const t = toks[k].w;
    if (t === 'for' || t === 'while') { push('loop', toks[k].at); pendingDo++; }
    else if (t === 'do') { if (pendingDo) pendingDo--; else push('do', toks[k].at); }
    else if (t === 'function' || t === 'if' || t === 'repeat') push(t, toks[k].at);
    else if (t === 'until' || t === 'end') { if (stack.length > 1) stack.pop().end = toks[k].at; }
    else if (t === '::') labels.push({ name: toks[k].label, block: stack[stack.length - 1], at: toks[k].at });
    else if (t === 'goto' && toks[k + 1]) gotos.push({ name: toks[k + 1].w, block: stack[stack.length - 1] });
  }
  const chain = (b) => { const out = []; for (let x = b; x; x = x.parent) { out.push(x); if (x.kind === 'function') break; } return out; };
  const inside = (b, loop) => { for (let x = b; x; x = x.parent) if (x === loop) return true; return false; };
  const add = new Map(), drop = new Set();
  for (const g of gotos) {
    const seen = chain(g.block);
    if (labels.some((l) => l.name === g.name && seen.includes(l.block))) continue;
    const loop = seen.find((b) => b.kind === 'loop');
    if (!loop || loop.end == null) continue;                       // niente ciclo for/while: lo vede il modello
    add.set(loop, (add.get(loop) || new Set()).add(g.name));
    for (const l of labels) if (l.name === g.name && l.block !== loop && inside(l.block, loop)) drop.add(l);
  }
  if (!add.size) return code;
  const edits = [];
  for (const l of drop) edits.push({ at: l.at, del: (code.slice(l.at).match(/^::\s*\w+\s*::/) || [''])[0].length });
  for (const [loop, names] of add) for (const n of names) edits.push({ at: loop.end, label: n });
  let out = code;
  for (const e of edits.sort((a, b) => b.at - a.at)) {
    if (e.del) { out = out.slice(0, e.at) + out.slice(e.at + e.del); continue; }
    const lineStart = out.lastIndexOf('\n', e.at - 1) + 1;
    const indent = (out.slice(lineStart, e.at).match(/^\s*/) || [''])[0];
    out = /^\s*$/.test(out.slice(lineStart, e.at))
      ? out.slice(0, lineStart) + indent + '  ::' + e.label + '::\n' + out.slice(lineStart)
      : out.slice(0, e.at) + '::' + e.label + ':: ' + out.slice(e.at);
  }
  return out;
}
// Le abitudini da C/JavaScript che i modelli portano in Lua, corrette a colpo sicuro (riga per riga, fuori da
// stringhe e commenti). Il caso vero (21/10/2026): qwen3.5:9b scriveva `for i = 0; i < 10; i = i + 1 do` e,
// con l'errore davanti, restituiva il file IDENTICO tre volte. Ciò che non è sicuro resta al modello.
function codeOnly(line) {   // la parte di riga prima di un commento, con le stringhe spente (stesse lunghezze)
  let out = '', q = '';
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (q) { out += c === q ? c : ' '; if (c === '\\') { out += ' '; i++; } else if (c === q) q = ''; continue; }
    if (c === '"' || c === "'") { q = c; out += c; continue; }
    if (c === '-' && line[i + 1] === '-') break;
    out += c;
  }
  return out;
}
export function fixCStyle(code) {
  return String(code).split('\n').map((line) => {
    const c = codeOnly(line);
    let m;
    // for i = a; i < b; i = i + 1 do   /   i <= b   /   i++   /   for (...) do
    if ((m = c.match(/\bfor\s*\(?\s*(?:local\s+)?([A-Za-z_]\w*)\s*=\s*([^;]+?)\s*;\s*\1\s*(<=|<|>=|>)\s*([^;]+?)\s*;\s*\1\s*(?:=\s*\1\s*([+-])\s*(\d+)|(\+\+|--)|([+-])=\s*(\d+))\s*\)?\s*do\b/))) {
      const [all, v, a, op, b0] = m;
      const sign = m[5] || (m[7] ? m[7][0] : m[8]), step = +(m[6] || m[9] || 1);
      const down = sign === '-';
      const b = op === '<' ? '(' + b0 + ') - 1' : op === '>' ? '(' + b0 + ') + 1' : b0;
      const head = 'for ' + v + ' = ' + a + ', ' + b + (down ? ', -' + step : step !== 1 ? ', ' + step : '') + ' do';
      const at = c.indexOf(all);
      return line.slice(0, at) + head + line.slice(at + all.length);
    }
    // math.clamp / math.lerp / math.round... non esistono in Lua (i modelli li inventano: math.clamp provato):
    // sono nel kit con lo stesso significato
    if (/\bmath\.(clamp|lerp|round|sign|dist)\s*\(/.test(c)) {
      let s = '';
      const re = /\bmath\.(clamp|lerp|round|sign|dist)(\s*\()/g;
      let last = 0, mm;
      while ((mm = re.exec(c))) { s += line.slice(last, mm.index) + 'N.' + mm[1] + mm[2]; last = mm.index + mm[0].length; }
      line = s + line.slice(last);
    }
    // != -> ~=
    if (c.includes('!=')) { let s = ''; for (let i = 0; i < line.length; i++) s += (i < c.length && c[i] === '!' && c[i + 1] === '=') ? '~' : line[i]; line = s; }
    // x += y  (riga con quella sola istruzione)
    if ((m = codeOnly(line).match(/^(\s*)([A-Za-z_][\w.]*(?:\[[^\]]+\])?)\s*([+\-*/])=\s*(.+?)\s*;?\s*$/)) && !/^(\s*)(local|if|for|while|return)\b/.test(line)) {
      const rest = line.slice(codeOnly(line).length);
      return m[1] + m[2] + ' = ' + m[2] + ' ' + m[3] + ' (' + m[4] + ')' + (rest ? ' ' + rest.trim() : '');
    }
    return line;
  }).join('\n');
}
export function autofix(code) { return code ? fixContinue(fixCStyle(code)) : code; }

// Le righe intorno a un errore "game.lua:N:", numerate: al modello serve VEDERE dove ha sbagliato
export function around(code, err, pad = 4) {
  const m = String(err || '').match(/game\.lua:(\d+):/);
  if (!m) return '';
  const n = +m[1], lines = String(code).split('\n');
  const a = Math.max(1, n - pad), b = Math.min(lines.length, n + pad);
  const w = String(b).length;
  return lines.slice(a - 1, b).map((l, k) => String(a + k).padStart(w) + (a + k === n ? ' >> ' : '    ') + l).join('\n');
}
// Un consiglio preciso per gli errori che i modelli fanno di più (Lua 5.1 / LuaJIT su LÖVE 11)
const EMPTY = 'EMPTY SCREEN: the game runs but nothing visible is on screen (the test pressed A to start and then played)';
const STUCK = 'STUCK ON THE TITLE: the test pressed A and then played for seconds, but the screen stayed exactly the same — the game cannot be started';
const HINTS = [
  [/STUCK ON THE TITLE/, () => 'In love.update, on the title screen: `if N.pressed("a") then state = "play" ... end` (and the same to restart after game over). Read input ONLY with N.pressed / N.down / N.axis — a home-made input table that nothing fills never changes.'],
  [/EMPTY SCREEN/, () => 'Something covers or hides the drawing. Draw the background FIRST (love.graphics.clear or a full rectangle) and everything else AFTER it — anything drawn later covers what is under it. Use colours different from the background, and keep coordinates inside 0..N.W, 0..N.H (or inside the N.fit world between N.push and N.pop).'],
  [/vertex components must be a multiple of two|bad argument #\d+ to '(rectangle|circle|polygon|line|print|printf)'/, () => 'Wrong arguments to a drawing call: shapes take numbers only (x, y pairs for lines and polygons), with an optional colour LAST: N.rect(x,y,w,h,"red"), N.line(x1,y1,x2,y2,"gray"). Do not pass "line"/"fill" anywhere but first, and never pass nil coordinates.'],
  [/undefined label '(\w+)'/, (m) => 'Lua has no `' + m[1] + '` statement. Put `::' + m[1] + '::` as the LAST line inside that loop (just before its `end`), or restructure with if/else.'],
  [/attempt to index .*\(a nil value\)/, () => 'Something is nil at that line: create every table in love.load (or at the top) before using it, check the spelling, and guard optional values with `if x then`.'],
  [/attempt to call .*\(a nil value\)/, () => 'You call a function that does not exist: define it ABOVE its first use (local function), or check the name against the N.* kit API.'],
  [/attempt to (perform arithmetic|compare|concatenate) .*nil/, () => 'A number or string is nil at that line: initialise it in love.load and make sure every code path sets it.'],
  [/',' expected near ';'|'=' or 'in' expected/, () => 'That is a C-style loop. Lua numeric for loops are `for i = 1, n do ... end` (with an optional step: `for i = n, 1, -1 do`), and `for k, v in ipairs(t) do` for lists.'],
  [/'end' expected|'then' expected|'do' expected|unexpected symbol|'=' expected|near 'end'/, () => 'Syntax: count the if/then, for/do, function and end of that block; every block needs exactly one end. Lua uses ~= for "not equal" and has no += operator.'],
];
export function hint(err) { for (const [re, f] of HINTS) { const m = String(err || '').match(re); if (m) return f(m); } return ''; }

// Le funzioni che il kit offre DAVVERO, lette dal sorgente di nucleo.lua (non da un elenco da tenere a mano)
export function kitApi(nucleo) {
  const s = new Set();
  for (const m of String(nucleo || '').matchAll(/function N\.([A-Za-z]\w*)\s*\(/g)) s.add(m[1]);
  for (const m of String(nucleo || '').matchAll(/^N\.([A-Za-z]\w*)\s*=\s*(?:shape\(|N\.[a-z])/gm)) s.add(m[1]);   // N.circle = shape(...), N.rectangle = N.rect
  return s;
}
// Controlli prima di spendere un avvio sulla console: costa ~10 s, un controllo qui niente.
// api = kitApi(nucleo): un N.qualcosa() che il kit non ha è il secondo errore dei modelli (N.time inventato).
export function lint(code, api) {
  const out = [];
  if (api && api.size) {
    const bad = new Set();
    for (const m of String(code).replace(/"[^"\n]*"|'[^'\n]*'/g, '').replace(/--[^\n]*/g, '').matchAll(/\bN\.([A-Za-z]\w*)\s*\(/g)) if (!api.has(m[1])) bad.add(m[1]);
    if (bad.size) out.push('the kit has no ' + [...bad].map((b) => 'N.' + b).join(', ') + ' — use only: ' + [...api].map((b) => 'N.' + b).join(' ') + ' (or plain love.* / math.*)');
  }
  const toks = luaWords(code), labels = new Set(toks.filter((t) => t.w === '::').map((t) => t.label));
  for (let k = 0; k < toks.length - 1; k++)
    if (toks[k].w === 'goto' && !labels.has(toks[k + 1].w)) { out.push('`goto ' + toks[k + 1].w + '` has no `::' + toks[k + 1].w + '::` label: put it as the last line of the loop, or remove the goto'); break; }
  if (/\+=|-=|\*=|\/=(?!=)/.test(code.replace(/"[^"\n]*"|'[^'\n]*'/g, '').replace(/--[^\n]*/g, '').replace(/~=/g, ''))) out.push('Lua has no += -= *= /= operators: write x = x + 1');
  if (/!=/.test(code.replace(/"[^"\n]*"|'[^'\n]*'/g, '').replace(/--[^\n]*/g, ''))) out.push('Lua writes "not equal" as ~= (not !=)');
  if (!/function\s+love\.draw\b/.test(code) && !/love\.draw\s*=/.test(code)) out.push('love.draw is missing');
  if (!/require\s*\(?\s*["']nucleo["']/.test(code)) out.push('the game must start with local N = require("nucleo")');
  if (/\/\//.test(code.replace(/"[^"\n]*"|'[^'\n]*'/g, '').replace(/--[^\n]*/g, ''))) out.push('the // operator does not exist in Lua 5.1: use math.floor(a / b)');
  if (/love\.(keyboard\.isDown|joystick)/.test(code)) out.push('use N.down / N.pressed / N.axis, not love.keyboard or love.joystick');
  if (/love\.graphics\.newImage|love\.graphics\.newFont\(\s*["']/.test(code)) out.push('there are no image or font files: draw shapes and use N.text');
  if (/function\s+love\.(run|errorhandler|errhand|conf)\b/.test(code)) out.push('do not define love.run / love.errorhandler / love.conf');
  return out;
}
// Gli id del catalogo che il gioco usa: solo quelli entrano nel pacchetto
export function usedAssets(code, cat) {
  const want = new Set();
  for (const m of String(code).matchAll(/N\.(sfx|music)\s*\(\s*["']([a-z0-9_-]+)["']/g)) want.add(m[1] + ':' + m[2]);
  const out = [];
  for (const e of (cat && cat.sfx) || []) if (want.has('sfx:' + e.id)) out.push(e.file);
  for (const e of (cat && cat.music) || []) if (want.has('music:' + e.id)) out.push(e.file);
  return out;
}

// ---- l'imbracatura (main.lua) e conf.lua ----------------------------------------------------------------------
export function confLua(identity, title) {
  return [
    '-- NucleoOS · ANIMA: configurazione del gioco (generata)',
    'function love.conf(t)',
    '  t.identity = ' + JSON.stringify(identity),
    '  t.version = "' + LOVE_VERSION + '"',
    '  t.window.title = ' + JSON.stringify(String(title || identity).slice(0, 60)),
    '  t.window.fullscreen = true',
    '  t.window.fullscreentype = "desktop"',
    '  t.window.vsync = 1',
    '  t.modules.physics = false',
    '  t.modules.video = false',
    'end', ''].join('\n');
}
export function harnessLua({ test = 0, run = '' } = {}) {
  return `-- NucleoOS · ANIMA: l'imbracatura del gioco (generata). Carica game.lua, scrive il registro per ANIMA
-- accanto al gioco (N.DATA, sulla scheda) e, in prova, esce da sola dopo ${test} s con uno screenshot.
local TEST, RUN = ${Number(test) || 0}, ${JSON.stringify(String(run))}
local N = require("nucleo")
N.write("log.txt", "")
local function log(s) pcall(N.write, "log.txt", s .. "\\n", "ab") end
log("run " .. RUN)
log("love " .. table.concat({ love.getVersion() }, ".", 1, 3))
local W, H = love.graphics.getDimensions()
log("screen " .. W .. "x" .. H)
N.refresh()
N.strict = TEST > 0      -- in prova gli argomenti sbagliati al kit sono errori con la riga del gioco

function love.errorhandler(msg)
  msg = tostring(msg)
  local tb = debug.traceback("", 2) or ""
  log("ERROR " .. msg)
  for line in tb:gmatch("[^\\n]+") do
    if line:find("game.lua") or line:find("nucleo.lua") then log("AT " .. line:gsub("^%s+", "")) end
  end
  log("END error")
  if not (love.window and love.graphics and love.event and love.timer) then return end
  if not love.window.isOpen() then pcall(love.window.setMode, 640, 480) end
  pcall(love.graphics.reset)
  local font, t0 = love.graphics.newFont(16), love.timer.getTime()
  return function()
    love.event.pump()
    for e in love.event.poll() do if e == "quit" then return 1 end end
    if love.timer.getTime() - t0 > 6 then return 1 end
    local w = love.graphics.getWidth()
    love.graphics.clear(0.08, 0.06, 0.12)
    love.graphics.setFont(font)
    love.graphics.setColor(1, 0.8, 0.4)
    love.graphics.printf("ANIMA - errore nel gioco (la console torna tra pochi secondi)\\n\\n" .. msg, 24, 24, w - 48)
    love.graphics.present()
    love.timer.sleep(0.1)
  end
end

local ok, err = pcall(require, "game")
if not ok then error(err, 0) end

local gload, gupdate, gdraw = love.load, love.update, love.draw
local t, frames, shot, looked, before, later, looked2 = 0, 0, false, false, false, false, false
function love.load(...)
  if gload then gload(...) end
  log("loaded")
end
local started = false
function love.update(dt)
  -- in prova si GIOCA, senza mani: A per cominciare a un quarto del tempo, poi destra/sinistra e A di
  -- continuo. Così gira anche il codice della partita (non solo il titolo) e lo screenshot la mostra.
  if TEST > 0 then
    if not started and t > TEST * 0.25 then started = true; log("autoplay"); N._inject("a", 3) end
    if started then
      -- asimmetrico (1 parte a sinistra, 3 a destra): il giocatore si SPOSTA. Simmetrico tornava al punto di
      -- partenza e due immagini coincidevano per caso (falso "bloccato" sul K36)
      N._inject((math.floor(t * 2) % 4 < 1) and "left" or "right", 1)
      if math.floor(t * 4) % 3 == 0 then N._inject("a", 1) end
    end
  end
  N._input()
  if gupdate then gupdate(dt) elseif N.current and N.current.update then N.current.update(dt) end
  N._frame(dt)
  t, frames = t + dt, frames + 1
  if TEST > 0 then
    -- l'occhio: quanti colori diversi ci sono (campionati su ~90x90 punti), quanta parte dello schermo non è il
    -- colore dominante, e un'impronta dei pixel. Si guarda due volte: sul titolo PRIMA di premere A e durante
    -- il gioco: impronte uguali = A non fa niente (un gioco che non si può cominciare: successo davvero).
    local function look(img, tag)
      local w, h = img:getDimensions()
      local step = math.max(1, math.floor(math.min(w, h) / 90))
      local counts, n, best, kinds, sig = {}, 0, 0, 0, 0
      for y = 0, h - 1, step do
        for x = 0, w - 1, step do
          local r, g, b = img:getPixel(x, y)
          local k = math.floor(r * 15) * 256 + math.floor(g * 15) * 16 + math.floor(b * 15)
          counts[k] = (counts[k] or 0) + 1
          n = n + 1
          sig = (sig * 31 + k + 1) % 2147483647
        end
      end
      for _, c in pairs(counts) do kinds = kinds + 1; if c > best then best = c end end
      log(string.format("%s %d %.4f %d", tag, kinds, 1 - best / n, sig))
    end
    if not before and t > TEST * 0.2 then
      before = true
      love.graphics.captureScreenshot(function(img) pcall(look, img, "look0") end)
    end
    if not shot and t > TEST * 0.6 then
      shot = true
      love.graphics.captureScreenshot(function(img)
        local done = pcall(function() N.write("shot.png", img:encode("png"):getString()) end)
        log(done and "shot shot.png" or "shot failed")
        pcall(look, img, "look")
        looked = true
      end)
    end
    if not later and t > TEST * 0.85 then
      later = true
      love.graphics.captureScreenshot(function(img) pcall(look, img, "look2"); looked2 = true end)
    end
    -- si esce solo dopo che lo screenshot è stato salvato ed esaminato (la richiesta si compie a fine
    -- fotogramma: un fotogramma lento che salta oltre TEST la perderebbe), o al massimo 3 s dopo
    if t > TEST and ((looked and looked2) or t > TEST + 3) then
      log(string.format("OK %.1fs %d frames %.0f fps mem %.0fKB", t, frames, frames / t, collectgarbage("count")))
      log("END ok")
      love.event.quit()
    end
  end
end
function love.draw()
  love.graphics.push()
  -- un mondo a misura fissa (N.fit) va SEMPRE a schermo intero: lo ingrandisce l'imbracatura, e i N.push/N.pop
  -- del gioco diventano innocui (se il gioco se li dimentica, il mondo non resta più piccolo nell'angolo)
  local fit = N.VW ~= nil
  if fit then love.graphics.clear(0, 0, 0); N.push(); N._autofit = true end
  N._predraw()
  if gdraw then gdraw() elseif N.current and N.current.draw then N.current.draw() end
  N._draw()
  if fit then N._autofit = false; N.pop() end
  love.graphics.pop()
end
`;
}

// ---- lo zip (.love), senza librerie: metodo "stored", CRC32 -----------------------------------------------
const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
export function crc32(b) { let c = 0xFFFFFFFF; for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }
const enc = (s) => (typeof s === 'string' ? new TextEncoder().encode(s) : s);
export function zipStore(files) {
  const parts = [], central = [];
  let off = 0;
  const u16 = (v) => [v & 255, (v >>> 8) & 255], u32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  for (const f of files) {
    const name = enc(f.name), data = enc(f.data), crc = crc32(data);
    const common = [...u16(20), ...u16(0x0800), ...u16(0), ...u16(0), ...u16(0x21), ...u32(crc), ...u32(data.length), ...u32(data.length), ...u16(name.length), ...u16(0)];
    const local = new Uint8Array([...u32(0x04034b50), ...common]);
    parts.push(local, name, data);
    central.push(new Uint8Array([...u32(0x02014b50), ...u16(20), ...common, ...u16(0), ...u16(0), ...u16(0), ...u32(0), ...u32(off)]), name);
    off += local.length + name.length + data.length;
  }
  const csize = central.reduce((a, b) => a + b.length, 0);
  const end = new Uint8Array([...u32(0x06054b50), ...u16(0), ...u16(0), ...u16(files.length), ...u16(files.length), ...u32(csize), ...u32(off), ...u16(0)]);
  const all = [...parts, ...central, end];
  const out = new Uint8Array(all.reduce((a, b) => a + b.length, 0));
  let p = 0; for (const b of all) { out.set(b, p); p += b.length; }
  return out;
}

// ---- il registro ---------------------------------------------------------------------------------------------
// -> { run, done, ok, error, at:[righe], fps, screen, shot, loaded }
export function parseLog(text) {
  const r = { run: '', done: false, ok: false, error: '', at: [], fps: 0, screen: '', shot: '', loaded: false, colors: null, busy: null };
  for (const line of String(text || '').split(/\r?\n/)) {
    let m;
    if ((m = line.match(/^run (.*)$/))) r.run = m[1];
    else if ((m = line.match(/^screen (\d+x\d+)/))) r.screen = m[1];
    else if (line === 'loaded') r.loaded = true;
    else if ((m = line.match(/^shot (.+)$/))) r.shot = m[1] === 'failed' ? '' : m[1];
    else if ((m = line.match(/^look0 (\d+) ([\d.]+)(?: (\d+))?/))) r.sig0 = m[3] || '';
    else if ((m = line.match(/^look2 (\d+) ([\d.]+)(?: (\d+))?/))) r.sig2 = m[3] || '';
    else if ((m = line.match(/^look (\d+) ([\d.]+)(?: (\d+))?/))) { r.colors = +m[1]; r.busy = +m[2]; r.sig = m[3] || ''; }
    else if ((m = line.match(/^OK .* ([\d.]+) fps/))) { r.ok = true; r.fps = +m[1]; }
    else if ((m = line.match(/^ERROR (.*)$/))) r.error = m[1];
    else if ((m = line.match(/^AT (.*)$/))) r.at.push(m[1]);
    else if (/^END /.test(line)) r.done = true;
  }
  return r;
}

// ---- il ciclo ------------------------------------------------------------------------------------------------
// opts: { idea, prev:{code,name}, llm(system, messages) -> text, dev, kit:{nucleo, catalog, asset(path)->bytes},
//         onStep(kind, info), tries=3, testSec=6, now() }
// dev: { write(path, bytes), read(path) -> text|null, launch(sys, file) -> {ok, error}, gaming() -> bool, sleep(ms) }
export async function forge(opts) {
  const { idea, prev, llm, dev, kit, onStep = () => {}, tries = 3, testSec = 6 } = opts;
  const name = prev && prev.name ? prev.name : slug(opts.title || idea);
  const ident = 'anima-' + name, dir = 'roms/love2d/', data = dir + '.anima/' + ident + '/', logPath = data + 'log.txt';
  const system = systemPrompt(kit.catalog, opts.lang);
  const msgs = prev && prev.code
    ? [{ role: 'user', content: 'Here is the current game.lua:\n```lua\n' + prev.code + '\n```\nChange it as asked, keep everything else working, and return the WHOLE file: ' + idea }]
    : [{ role: 'user', content: 'Make this game: ' + idea }];
  onStep('write', { name });
  // ogni risposta: il codice, con le correzioni che non hanno bisogno del modello (etichette dei goto);
  // il terzo argomento riceve i caratteri scritti finora, per mostrare che il modello sta lavorando
  const ask = async (m) => autofix(extractLua(await llm(system, m, (chars) => onStep('writing', { chars }))));
  let code = await ask(msgs);
  const history = [];
  let lastLint = '', lastFailed = '';
  for (let attempt = 1; attempt <= tries + 1; attempt++) {
    if (!code) return { ok: false, name, stage: 'write', error: 'no Lua code in the model reply', history };
    const problems = lint(code, kitApi(kit.nucleo));
    // lo stesso problema dopo una correzione: il modello non lo sa togliere, si prova sulla console (che dirà
    // se è un errore vero) invece di sprecare altri giri a riscrivere lo stesso file
    const sig = problems.join('|'), repeated = sig && sig === lastLint;
    lastLint = sig;
    if (problems.length && attempt <= tries && !repeated) {
      onStep('lint', { attempt, problems });
      history.push({ attempt, lint: problems });
      code = (await ask([...msgs, { role: 'assistant', content: '```lua\n' + code + '```' },
        { role: 'user', content: 'Fix these problems and return the whole file:\n- ' + problems.join('\n- ') }])) || code;
      continue;
    }
    const run = name + '-' + attempt + '-' + (opts.now ? opts.now() : Date.now());
    const pack = await pack_(code, { name, ident, test: testSec, run, kit, title: opts.title || idea });
    onStep('install', { attempt, bytes: pack.length });
    await dev.write(dir + name + '.love', pack);
    await dev.write(dir + 'src/' + name + '/game.lua', enc(code));
    // un gioco ancora aperto (la prova precedente) va prima chiuso: si aspetta. Appena chiuso, Core torna al menu
    // per qualche secondo e un PLAY in quel momento si perde (provato sul K36: il secondo avvio non arrivava mai)
    let waited = false;
    for (let k = 0; k < 20 && (await dev.gaming()); k++) { waited = true; await dev.sleep(1000); }
    if (waited || attempt > 1) await dev.sleep(3000);
    onStep('launch', { attempt });
    const lr = await dev.launch(SYS, name + '.love');
    if (!lr || !lr.ok) return { ok: false, name, stage: 'launch', error: (lr && lr.error) || 'launch failed', code, history };
    // il registro: si aspetta la riga END della corsa giusta. Se dopo 15 s la corsa non ha scritto nemmeno la
    // prima riga e la console non è in gioco, il PLAY si è perso: si rilancia una volta.
    let res = null, relaunched = false;
    for (let k = 0; k < Math.ceil((testSec + 45) / 1.5); k++) {
      await dev.sleep(1500);
      const r = parseLog(await dev.read(logPath));
      if (r.run === run && r.done) { res = r; break; }
      if (!relaunched && k === 10 && r.run !== run && !(await dev.gaming())) {
        relaunched = true;
        onStep('launch', { attempt, retry: true });
        const again = await dev.launch(SYS, name + '.love');
        if (!again || !again.ok) return { ok: false, name, stage: 'launch', error: (again && again.error) || 'launch failed', code, history };
      }
    }
    if (!res) res = { ok: false, error: 'the game did not start or did not finish (no log after ' + (testSec + 45) + ' s)', at: [] };
    // "gira" non è "funziona": un titolo che non si vede è un errore anche se niente è andato in crash
    if (res.ok && res.colors != null && res.colors < 3)
      res = Object.assign({}, res, { ok: false, error: EMPTY + ' (' + res.colors + ' colour' + (res.colors === 1 ? '' : 's') + ' on screen after ' + Math.round(testSec * 0.6) + ' s)' });
    // bloccato = TUTTE le immagini dopo A uguali a quella prima (una sola coincidenza può essere il caso)
    else if (res.ok && res.sig && res.sig0 && res.sig === res.sig0 && (!res.sig2 || res.sig2 === res.sig0))
      res = Object.assign({}, res, { ok: false, error: STUCK });
    history.push({ attempt, result: res });
    onStep('result', { attempt, result: res });
    if (res.ok) {
      const play = await pack_(code, { name, ident, test: 0, run: 'play', kit, title: opts.title || idea });
      await dev.write(dir + name + '.love', play);
      return { ok: true, name, file: name + '.love', code, attempts: attempt, fps: res.fps, screen: res.screen,
               shot: res.shot ? data + res.shot : '', history };
    }
    if (attempt > tries) break;
    onStep('fix', { attempt, error: res.error });
    // la riga del GIOCO: quella dell'errore, o se l'errore è nato nel kit la prima di game.lua nella traccia
    const where = [res.error, ...(res.at || [])].join('\n');
    const near = around(code, where), tip = hint(res.error);
    // il modello che restituisce lo stesso file: glielo si dice, con la riga da cambiare in chiaro
    const same = lastFailed === code;
    lastFailed = code;
    const ln = (where.match(/game\.lua:(\d+):/) || [])[1];
    const bad = ln ? String(code).split('\n')[+ln - 1] : '';
    const fixed = (await ask([...msgs, { role: 'assistant', content: '```lua\n' + code + '```' },
      { role: 'user', content: 'The game crashed on the console with LÖVE ' + LOVE_VERSION + ':\n' + res.error + (res.at.length ? '\n' + res.at.join('\n') : '') +
        (near ? '\n\nThe lines around it (>> marks the failing one):\n' + near : '') + (tip ? '\n\nHow to fix this kind of error: ' + tip : '') +
        (same ? '\n\nIMPORTANT: your previous answer was IDENTICAL to this broken file. You MUST change line ' + (ln || '?') + (bad ? ' (`' + bad.trim() + '`)' : '') + '.' : '') +
        '\n\nFix the cause (check that line and every value it uses) and return the WHOLE file.' }]));
    code = fixed || code;
  }
  const last = history.length && history[history.length - 1].result;
  return { ok: false, name, stage: 'test', error: last ? last.error : 'failed', code, history };
}

async function pack_(code, { name, ident, test, run, kit, title }) {
  const files = [
    { name: 'conf.lua', data: confLua(ident, title) },
    { name: 'main.lua', data: harnessLua({ test, run }) },
    { name: 'nucleo.lua', data: kit.nucleo },
    { name: 'game.lua', data: code },
  ];
  for (const p of usedAssets(code, kit.catalog)) {
    const b = await kit.asset(p);
    if (b) files.push({ name: p, data: b });
  }
  return zipStore(files);
}
