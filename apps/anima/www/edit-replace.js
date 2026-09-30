// edit-replace.js — tolerant old→new replacement for the agent's edit_file tool.
//
// Ported from OpenCode (packages/opencode/src/tool/edit.ts, github.com/anomalyco/opencode, MIT License,
// Copyright (c) 2025 opencode), whose approach in turn comes from Cline's diff-apply evals and gemini-cli's
// editCorrector. Local models (qwen3.5 9B on the user's PC) often send an oldString that differs from the
// file only in indentation, trailing spaces or escaping; an exact-only match made those edits fail and the
// agent loop. A chain of replacers is tried in order, each yielding candidate spans of the ORIGINAL text:
// the first candidate that occurs exactly once wins (never a guess between several), and a candidate much
// larger than what was asked for is refused (the model must re-read and send the exact text).
//
// Pure, DOM-free, dependency-free. tools/edit-replace.test.mjs.

const SIMILARITY = 0.65;                                   // block-anchor middle-line similarity threshold

function levenshtein(a, b) {
  if (a === '' || b === '') return Math.max(a.length, b.length);
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j);
  for (let i = 1; i <= a.length; i++) {
    const cur = [i];
    for (let j = 1; j <= b.length; j++) cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    prev = cur;
  }
  return prev[b.length];
}
const spanOf = (lines, start, end) => lines.slice(start, end + 1).join('\n');   // lines[start..end] as original text

function* simple(_content, find) { yield find; }

function* lineTrimmed(content, find) {
  const lines = content.split('\n'), search = find.split('\n');
  if (search[search.length - 1] === '') search.pop();
  for (let i = 0; i <= lines.length - search.length; i++) {
    let ok = true;
    for (let j = 0; j < search.length && ok; j++) ok = lines[i + j].trim() === search[j].trim();
    if (ok) yield spanOf(lines, i, i + search.length - 1);
  }
}

function* blockAnchor(content, find) {
  const lines = content.split('\n'), search = find.split('\n');
  if (search.length < 3) return;
  if (search[search.length - 1] === '') search.pop();
  const first = search[0].trim(), last = search[search.length - 1].trim(), size = search.length;
  const maxDelta = Math.max(1, Math.floor(size * 0.25));
  const cands = [];
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].trim() !== first) continue;
    for (let j = i + 2; j < lines.length; j++) {
      if (lines[j].trim() === last) { if (Math.abs(j - i + 1 - size) <= maxDelta) cands.push([i, j]); break; }
    }
  }
  const score = ([s, e]) => {
    const n = Math.min(size - 2, e - s + 1 - 2);
    if (n <= 0) return 1;
    let sim = 0;
    for (let j = 1; j < size - 1 && j < e - s; j++) {
      const a = lines[s + j].trim(), b = search[j].trim(), m = Math.max(a.length, b.length);
      if (m) sim += 1 - levenshtein(a, b) / m;
    }
    return sim / n;
  };
  let best = null, bestSim = -1;
  for (const c of cands) { const s = score(c); if (s > bestSim) { bestSim = s; best = c; } }
  if (best && bestSim >= SIMILARITY) yield spanOf(lines, best[0], best[1]);
}

function* whitespaceNormalized(content, find) {
  const norm = (t) => t.replace(/\s+/g, ' ').trim();
  const nf = norm(find), lines = content.split('\n');
  for (const line of lines) {
    if (norm(line) === nf) { yield line; continue; }
    if (norm(line).includes(nf)) {
      const words = find.trim().split(/\s+/);
      if (words.length) {
        try { const m = line.match(new RegExp(words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('\\s+'))); if (m) yield m[0]; } catch {}
      }
    }
  }
  const fl = find.split('\n');
  if (fl.length > 1) for (let i = 0; i <= lines.length - fl.length; i++) { const b = lines.slice(i, i + fl.length).join('\n'); if (norm(b) === nf) yield b; }
}

function* indentationFlexible(content, find) {
  const dedent = (text) => {
    const ls = text.split('\n'), ne = ls.filter((l) => l.trim().length);
    if (!ne.length) return text;
    const min = Math.min(...ne.map((l) => l.match(/^(\s*)/)[1].length));
    return ls.map((l) => (l.trim().length ? l.slice(min) : l)).join('\n');
  };
  const nf = dedent(find), lines = content.split('\n'), fl = find.split('\n');
  for (let i = 0; i <= lines.length - fl.length; i++) { const b = lines.slice(i, i + fl.length).join('\n'); if (dedent(b) === nf) yield b; }
}

function* escapeNormalized(content, find) {
  const un = (s) => s.replace(/\\(n|t|r|'|"|`|\\|\n|\$)/g, (m, c) => ({ n: '\n', t: '\t', r: '\r', "'": "'", '"': '"', '`': '`', '\\': '\\', '\n': '\n', $: '$' }[c] ?? m));
  const uf = un(find);
  if (content.includes(uf)) yield uf;
  const lines = content.split('\n'), fl = uf.split('\n');
  for (let i = 0; i <= lines.length - fl.length; i++) { const b = lines.slice(i, i + fl.length).join('\n'); if (un(b) === uf) yield b; }
}

function* trimmedBoundary(content, find) {
  const tf = find.trim();
  if (tf === find) return;
  if (content.includes(tf)) yield tf;
  const lines = content.split('\n'), fl = find.split('\n');
  for (let i = 0; i <= lines.length - fl.length; i++) { const b = lines.slice(i, i + fl.length).join('\n'); if (b.trim() === tf) yield b; }
}

function* contextAware(content, find) {
  const fl = find.split('\n');
  if (fl.length < 3) return;
  if (fl[fl.length - 1] === '') fl.pop();
  const lines = content.split('\n'), first = fl[0].trim(), last = fl[fl.length - 1].trim();
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].trim() !== first) continue;
    for (let j = i + 2; j < lines.length; j++) {
      if (lines[j].trim() !== last) continue;
      const block = lines.slice(i, j + 1);
      if (block.length === fl.length) {
        let match = 0, total = 0;
        for (let k = 1; k < block.length - 1; k++) {
          const a = block[k].trim(), b = fl[k].trim();
          if (a.length || b.length) { total++; if (a === b) match++; }
        }
        if (!total || match / total >= 0.5) { yield block.join('\n'); return; }
      }
      break;
    }
  }
}

function* multiOccurrence(content, find) {
  for (let i = content.indexOf(find); i !== -1; i = content.indexOf(find, i + find.length)) yield find;
}

export const REPLACERS = [simple, lineTrimmed, blockAnchor, whitespaceNormalized, indentationFlexible, escapeNormalized, trimmedBoundary, contextAware, multiOccurrence];

function disproportionate(search, oldString) {
  const oldLines = oldString.split('\n').length, searchLines = search.split('\n').length;
  if (searchLines >= Math.max(oldLines + 3, oldLines * 2)) return true;
  if (oldLines === 1) return false;
  return search.trim().length > Math.max(oldString.trim().length + 500, oldString.trim().length * 4);
}

// → { ok:true, text, count, strategy } | { ok:false, error:'identical'|'empty-old'|'not-found'|'not-unique'|'too-large', message }
// Line endings: matching runs on LF; the file's own CRLF is restored on the result.
export function tolerantReplace(content, oldString, newString, { all = false } = {}) {
  const crlf = /\r\n/.test(content);
  const c = String(content).replace(/\r\n/g, '\n'), o = String(oldString ?? '').replace(/\r\n/g, '\n'), n = String(newString ?? '').replace(/\r\n/g, '\n');
  const fail = (error, message) => ({ ok: false, error, message });
  if (o === n) return fail('identical', 'No changes to apply: old and new text are identical.');
  if (o === '') return fail('empty-old', 'old cannot be empty when editing an existing file: provide the exact text to replace, or use write_file for a full rewrite.');
  const back = (t) => (crlf ? t.replace(/\n/g, '\r\n') : t);
  let found = false;
  for (const replacer of REPLACERS) {
    for (const search of replacer(c, o)) {
      const idx = c.indexOf(search);
      if (idx === -1) continue;
      found = true;
      if (disproportionate(search, o)) return fail('too-large', 'Refusing the replacement: the matched text is much larger than old. Re-read the file and send the exact old text.');
      if (all) { const count = c.split(search).length - 1; return { ok: true, text: back(c.split(search).join(n)), count, strategy: replacer.name }; }
      if (idx !== c.lastIndexOf(search)) continue;                           // several matches: never guess, try a stricter candidate
      return { ok: true, text: back(c.slice(0, idx) + n + c.slice(idx + search.length)), count: 1, strategy: replacer.name };
    }
  }
  return found
    ? fail('not-unique', 'old matches more than one place: include more surrounding lines to make it unique.')
    : fail('not-found', 'old was not found in the file. Re-read it (read_file) and copy the exact lines, without the "N→" line-number prefix.');
}
