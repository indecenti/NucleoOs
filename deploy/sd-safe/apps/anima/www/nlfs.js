// nlfs.js — bilingual (IT/EN) natural-language -> file-operation parser for ANIMA's
// workspace agent. Deterministic and HIGH-PRECISION by design: it returns an intent only
// when the text clearly asks for a file operation; otherwise null, so the query falls
// through to the ANIMA engine (questions, math, app launches, knowledge). This module is the
// "hands" router — ANIMA stays the brain. No DOM, host-testable.
//
// Intent shapes:
//   {op:'read',   path}
//   {op:'write',  path, content, mode, verb}         mode: 'literal'|'auto'|''
//   {op:'append', path, content, mode}
//   {op:'edit',   path, old, new, all?}
//   {op:'move',   from, to, rename?}
//   {op:'delete', path}
//   {op:'mkdir',  path}
//   {op:'list',   path}
//   {op:'tree',   path}
//   {op:'search', query, glob?}
//   {op:'glob',   pattern}
//   {op:'cd',     path}

const Q = /"([^"]+)"|'([^']+)'|«([^»]+)»|`([^`]+)`/g;
const hasExt = (t) => /\.[a-z0-9]{1,8}$/i.test(t || '');
const PATHTOK = /^\/?(?:\.\.?\/)?(?:[\w.\- ]+\/)*[\w.\-]+\/?$/;
const looksPath = (t) => !!t && !/^https?:/i.test(t) && (t.includes('/') || hasExt(t)) && PATHTOK.test(t);
const norm = (s) => String(s || '').toLowerCase().normalize('NFC').replace(/[‘’ʼ´`]/g, "'").replace(/\s+/g, ' ').trim();

function quoted(s) { const out = []; let m; Q.lastIndex = 0; while ((m = Q.exec(s))) out.push(m[1] ?? m[2] ?? m[3] ?? m[4]); return out; }
function stripQuotes(s) { return String(s || '').trim().replace(/^["'«`]+|["'»`]+$/g, '').trim(); }
const STOP = /^(in|nel|nella|nello|the|a|an|di|del|della|to|su|con|i|le|gli|il|la|un|una)$/i;
function cleanPath(s) {
  s = String(s || '').trim();
  const qm = /^["'«`]([^"'»`]+)["'»`]/.exec(s);
  if (qm) return qm[1].trim();
  const t = s.split(/\s+/)[0] || '';
  if (t === '.' || t === '..') return t;
  return t.replace(/[.,;:!?)\]]+$/, '');
}

// First path-like token: a quoted path, then a bare token, then the word after a file noun.
function pickPath(s) {
  for (const q of quoted(s)) { const c = stripQuotes(q); if (c && (hasExt(c) || c.includes('/'))) return c; }
  for (const t of s.split(/\s+/)) { const c = t.replace(/[.,;:!?)\]]+$/, ''); if (looksPath(c)) return c; }
  const m = /\b(?:file|cartella|directory|folder|dir|documento)\s+(?:chiamat[oa]\s+|denominat[oa]\s+|named\s+|called\s+|nominat[oa]\s+)?["'«`]?([\w.\-/]+)/i.exec(s);
  return m && m[1] && !STOP.test(m[1]) ? m[1] : null;
}

// Content clause for create/write/append. Explicit connectors win; bare "con/with" is auto.
function extractContent(raw) {
  let m;
  if ((m = /\b(?:con\s+(?:scritto|su\s+scritto|il\s+testo|testo|dentro\s+scritto)|che\s+(?:dice|recita))\b\s*:?\s*([\s\S]+)$/i.exec(raw))) return { content: stripQuotes(m[1]), mode: 'literal' };
  if ((m = /\bwith\s+(?:the\s+)?text\b\s*:?\s*([\s\S]+)$/i.exec(raw))) return { content: stripQuotes(m[1]), mode: 'literal' };
  if ((m = /\b(?:that\s+says|saying)\b\s*:?\s*([\s\S]+)$/i.exec(raw))) return { content: stripQuotes(m[1]), mode: 'literal' };
  if ((m = /\b(?:con\s+(?:il\s+)?contenuto|with\s+content|containing|contenente)\b\s*:?\s*([\s\S]+)$/i.exec(raw))) return { content: stripQuotes(m[1]), mode: 'auto' };
  if ((m = /\bcon\s+(?!nome\b|il\s+nome\b|titolo\b|estensione\b)([\s\S]+)$/i.exec(raw))) return { content: stripQuotes(m[1]), mode: 'auto' };
  if ((m = /:\s*([\s\S]+)$/.exec(raw)) && m[1].trim()) return { content: stripQuotes(m[1]), mode: 'literal' };
  return null;
}

// Verb groups (word-boundary alternations).
const V = {
  edit:   /\b(sostituisci|sostituire|rimpiazza|replace)\b/i,
  rename: /\b(rinomina|rinominare|rename)\b/i,
  move:   /\b(sposta|spostare|muovi|sposto|move|mv)\b/i,
  append: /\b(aggiungi|aggiungere|accoda|appendi|append)\b/i,
  delete: /\b(elimina|eliminare|cancella|cancellare|rimuovi|rimuovere|delete|remove|del|rm)\b/i,
  create: /\b(crea|creare|nuov[oa]|scrivi|scrivere|salva|salvare|genera|generare|create|new|write|save|generate|make|touch)\b/i,
  read:   /\b(leggi|leggere|apri|aprire|mostra|mostrami|visualizza|vedi|stampa|cat|read|open|show|view|display|print|dammi)\b/i,
  list:   /\b(elenca|elencare|lista|elenco|ls|dir|ll)\b/i,
  tree:   /\b(albero|struttura|tree)\b/i,
  search: /\b(cerca|cercare|trova|trovare|grep|search|find|locate)\b/i,
  mkdir:  /\b(mkdir)\b/i,
  cd:     /\b(cd|vai|entra|spostati|passa|chdir)\b/i,
};

const DEST = '(?:in|into|nel|nella|nello|dentro|to|al|allo|alla|su)';

export function parseFileIntent(text) {
  const raw = String(text || '').trim();
  if (!raw) return null;
  const n = norm(raw);
  const qs = quoted(raw).map(stripQuotes);
  let m;

  // ---- EDIT: sostituisci X con Y [in F] / replace X with Y [in F] ----
  if (V.edit.test(n)) {
    let oldS = null, newS = null, path = null;
    if (qs.length >= 2) { oldS = qs[0]; newS = qs[1]; }
    else if ((m = /\b(?:sostituisci|sostituire|rimpiazza|replace)\s+([\s\S]+?)\s+(?:con|with)\s+([\s\S]+?)(?:\s+(?:in|nel|nella|nel file|in the file|dentro)\s+(\S+))?\s*$/i.exec(raw))) {
      oldS = m[1].trim(); newS = m[2].trim(); if (m[3]) path = cleanPath(m[3]);
    }
    path = path || pickPath(raw);
    const all = /\b(tutt[ie]|ovunque|all|everywhere|global)\b/i.test(n);
    if (oldS != null && newS != null && path) return { op: 'edit', path, old: stripQuotes(oldS), new: stripQuotes(newS), all };
  }

  // ---- RENAME: rinomina X in Y / rename X to Y (basename change in place) ----
  if (V.rename.test(n)) {
    const two = twoPaths(raw, qs, /\b(?:rinomina|rinominare|rename)\b/i);
    if (two) return { op: 'move', from: two.a, to: resolveRenameTarget(two.a, two.b), rename: true };
  }

  // ---- MOVE: sposta X in Y / move X to Y / mv X Y ----
  if (V.move.test(n)) {
    const two = twoPaths(raw, qs, /\b(?:sposta|spostare|muovi|move|mv)\b/i);
    if (two) return { op: 'move', from: two.a, to: two.b };
  }

  // ---- APPEND: aggiungi <content> a <file> / append <content> to <file> ----
  if (V.append.test(n)) {
    if ((m = /\b(?:aggiungi|aggiungere|accoda|appendi|append)\s+([\s\S]+?)\s+(?:a|al|allo|alla|in|nel|nella|to|into)\s+(\S+)\s*$/i.exec(raw))) {
      const path = cleanPath(m[2]);
      if (looksPath(path)) return { op: 'append', path, content: stripQuotes(m[1]), mode: 'literal' };
    }
    // "aggiungi a F: X" / "append to F: X"
    if ((m = /\b(?:aggiungi|accoda|append)\s+(?:a|al|to|into)\s+(\S+)\s*:?\s*([\s\S]+)$/i.exec(raw))) {
      const path = cleanPath(m[1]);
      if (looksPath(path)) return { op: 'append', path, content: stripQuotes(m[2]), mode: 'literal' };
    }
    // guard: bare "aggiungi un evento/promemoria" is calendar, not a file -> fall through to ANIMA
  }

  // ---- MKDIR: crea cartella X / nuova cartella X / mkdir X ----
  if (V.mkdir.test(n) || /\b(?:crea|creare|nuov[oa]|make|create)\s+(?:una?\s+|la\s+|the\s+)?(?:cartella|directory|folder|dir)\b/i.test(n)) {
    const path = afterNoun(raw, /(?:cartella|directory|folder|dir)/i) || (V.mkdir.test(n) ? afterWord(raw, /mkdir/i) : null) || pickPath(raw);
    if (path) return { op: 'mkdir', path };
  }

  // ---- CREATE / WRITE: crea|scrivi|salva FILE [con/: content] ----
  if (V.create.test(n)) {
    const verb = (n.match(V.create) || [''])[0];
    const path = pickPath(raw);
    if (path) {
      const c = extractContent(raw);
      return { op: 'write', path, content: c ? c.content : '', mode: c ? c.mode : '', verb };
    }
    // create verb + content but no explicit filename -> let ANIMA name it (returns null here)
  }

  // ---- DELETE: elimina/cancella/rm <file or folder> ----
  if (V.delete.test(n)) {
    const named = afterNoun(raw, /(?:file|cartella|directory|folder|dir)/i);   // explicit "cartella tmp" allows extensionless
    const path = pickPath(raw) || named;
    if (path && (looksPath(path) || named)) return { op: 'delete', path };
    // bare "elimina la conversazione / l'evento" -> not a file -> ANIMA
  }

  // ---- GLOB: explicit wildcard, or "tutti i .ext / all .ext" ----
  if (/[*?]/.test(raw)) {
    const gm = /((?:\*\*\/)?[\w.\-]*[*?][\w.\-*?/]*)/.exec(raw);
    if (gm && (V.search.test(n) || V.list.test(n) || /\bfile/i.test(n) || hasExt(gm[1]) || gm[1].includes('/'))) {
      let pat = gm[1]; if (!pat.includes('/')) pat = '**/' + pat;
      return { op: 'glob', pattern: pat };
    }
  }
  if ((m = /\b(?:tutti\s+i|tutte\s+le|all\s+(?:the\s+)?)\s*(?:files?\s+)?\*?\.([a-z0-9]{1,8})\b/i.exec(raw))) {
    return { op: 'glob', pattern: '**/*.' + m[1].toLowerCase() };
  }

  // ---- SEARCH (grep): cerca/trova X nei file / grep X / find X in <dir> ----
  if (V.search.test(n)) {
    const fileCtx = /\b(nei file|nei documenti|nel workspace|nel progetto|in the files|in files|in the workspace|in the project|nei sorgenti|nel codice|in the code)\b/i.test(n) || /\bgrep\b/i.test(n);
    // "trova il file NAME" -> glob by name
    if ((m = /\b(?:trova|cerca|find|locate)\s+(?:il\s+|the\s+)?file\s+["'«`]?([\w.\-]+)/i.exec(raw))) {
      const name = m[1]; return { op: 'glob', pattern: hasExt(name) ? '**/' + name : '**/*' + name + '*' };
    }
    if (fileCtx) {
      // needle: prefer quoted, else the words between the verb and the file-context phrase
      let needle = qs[0];
      if (!needle && (m = /\b(?:cerca|cercare|trova|trovare|grep|search|find|locate)\s+([\s\S]+?)\s+(?:nei file|nei documenti|nel workspace|nel progetto|in the files|in files|in the workspace|in the project|nei sorgenti|nel codice|in the code)\b/i.exec(raw))) needle = m[1].trim();
      if (!needle && (m = /\bgrep\s+([\s\S]+)$/i.exec(raw))) needle = m[1].trim();
      let dir = '';
      if ((m = /\b(?:in|nel|nella|dentro)\s+(\S+)\s*$/i.exec(raw)) && looksPath(cleanPath(m[1]))) dir = cleanPath(m[1]);
      if (needle) return { op: 'search', query: stripQuotes(needle), glob: dir && dir.endsWith('/') ? dir + '**' : (dir ? dir + '/**' : '') };
    }
    // otherwise: ambiguous "cerca/trova X" -> leave to ANIMA (could be a knowledge question)
  }

  // ---- TREE: albero / struttura del progetto / tree [dir] ----
  if (V.tree.test(n) && /\b(progetto|cartella|directory|workspace|file|albero|tree|struttura)\b/i.test(n)) {
    return { op: 'tree', path: pickPath(raw) || '.' };
  }

  // ---- LIST: elenca/ls/dir / mostra i file [in DIR] / cosa c'è in DIR ----
  if (V.list.test(n) || /\b(?:mostra(?:mi)?|show|list)\s+(?:i\s+|the\s+|tutti\s+i\s+)?files?\b/i.test(n) || /\b(?:cosa\s+c'?è|cosa\s+contiene|what'?s\s+in)/i.test(n)) {
    let path = afterNoun(raw, /(?:cartella|directory|folder|dir)/i);
    if (!path) { const aw = afterWord(raw, /(?:ls|dir|ll)/i); if (aw && !STOP.test(aw)) path = aw; }
    if (!path) path = pickPathDir(raw);
    return { op: 'list', path: path || '.' };
  }

  // ---- CD: vai in / entra in / cd DIR ----
  if (V.cd.test(n)) {
    if ((m = /\b(?:cd|vai\s+(?:in|nella?|nello)|entra\s+(?:in|nella?)|spostati\s+(?:in|nella?)|passa\s+a)\s+(\S+)/i.exec(raw))) {
      const path = cleanPath(m[1]); if (path) return { op: 'cd', path };
    }
  }

  // ---- READ: leggi/apri/mostra/cat <file> (only when the object looks like a file) ----
  if (V.read.test(n)) {
    const named = afterNoun(raw, /(?:file|documento)/i);   // "apri il file README" allows extensionless
    const path = pickPath(raw) || named;
    if (path && (looksPath(path) || named)) return { op: 'read', path };
    // "apri X" where X is not a path -> app launch -> ANIMA (return null)
  }

  return null;
}

// Two-path extractor for rename/move: "<verb> A <dest> B".
function twoPaths(raw, qs, verbRx) {
  let a = null, b = null, m;
  if (qs.length >= 2) { a = qs[0]; b = qs[1]; }
  else if ((m = new RegExp(verbRx.source + '\\s+(?:il\\s+file\\s+|the\\s+file\\s+|la\\s+cartella\\s+)?(.+?)\\s+(?:' + DEST + '|->|=>)\\s+(.+?)\\s*$', 'i').exec(raw))) {
    a = cleanPath(m[1]); b = cleanPath(m[2]);
  } else if ((m = new RegExp(verbRx.source + '\\s+(\\S+)\\s+(\\S+)\\s*$', 'i').exec(raw))) {  // mv-style "verb A B"
    a = cleanPath(m[1]); b = cleanPath(m[2]);
  }
  if (a && b) return { a: cleanPath(a), b: cleanPath(b) };
  return null;
}

// "rinomina a.txt in b.txt" where b has no dir -> keep a's directory.
function resolveRenameTarget(from, to) {
  if (to.includes('/')) return to;
  const slash = from.lastIndexOf('/');
  return slash >= 0 ? from.slice(0, slash + 1) + to : to;
}

function afterNoun(raw, nounRx) {
  const m = new RegExp('\\b' + nounRx.source + '\\s+(?:chiamat[oa]\\s+|named\\s+|called\\s+)?["\'«`]?([\\w.\\-/]+)', 'i').exec(raw);
  return m && m[1] ? m[1] : null;
}
function afterWord(raw, wordRx) {
  const m = new RegExp('\\b' + wordRx.source + '\\s+["\'«`]?([\\w.\\-/]+)', 'i').exec(raw);
  return m && m[1] ? m[1] : null;
}
// A directory-ish path token (allows extensionless names for list/cd targets).
function pickPathDir(raw) {
  const p = pickPath(raw);
  if (p) return p;
  const m = /\b(?:in|nella?|nello|dentro|di|della|del|in the)\s+["'«`]?([\w.\-/]+)/i.exec(raw);
  return m && m[1] && !/^(file|files|cartella|directory|folder)$/i.test(m[1]) ? m[1] : null;
}
