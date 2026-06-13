// context.js — history/context management for ANIMA, à la Claude Code's auto-compaction.
// When a conversation outgrows its byte budget, older turns are folded into a compact,
// structured DIGEST that keeps the useful signal (file operations performed, what was asked,
// facts learned, pinned turns) and drops the noise (greetings, confirmations, chit-chat).
// Recent turns are kept verbatim. Pure & DOM-free, so it is host-testable.

export function estimateBytes(x) { try { return JSON.stringify(x).length; } catch { return 0; } }

const stripMd = (s) => String(s || '').replace(/\*\*|`|^#+\s*/gm, '').replace(/\s+/g, ' ').trim();
const clampStr = (s, n) => { s = String(s || ''); return s.length > n ? s.slice(0, n - 1) + '…' : s; };
const firstSentence = (s) => { const t = stripMd(s); const m = t.match(/^(.{0,140}?[.!?])(\s|$)/); return clampStr(m ? m[1] : t, 140); };
const uniq = (a) => [...new Set(a)];

// One-line label for a file op, used in the digest. Prefer the stored op+path summary.
function fileOpLabel(h) {
  if (h.fileop && h.fileop.sum) return h.fileop.sum;
  if (h.fileop && h.fileop.view && h.fileop.view.reply) return stripMd(h.fileop.view.reply);
  return '';
}

// Build an extractive digest (markdown) from a list of older turns.
export function summarizeTurns(turns, lang) {
  const en = lang === 'en';
  const files = [], asks = [], facts = [];
  for (const h of turns) {
    if (h.role === 'user') {
      const t = stripMd(h.text);
      if (t && !/^\/(help|clear|new|theme|export|it|en|offline|online|ibrida|hybrid|impostazioni|settings)\b/i.test(t)) asks.push(t);
      continue;
    }
    const lbl = fileOpLabel(h);
    if (lbl) { files.push(lbl); continue; }
    if (h.engine && h.meta && ['knowledge', 'meteo', 'teacher', 'teacher·wiki', 'teacher·learned'].includes(h.meta.domain)) {
      const s = firstSentence(h.text); if (s) facts.push(s);
    }
  }
  const parts = [];
  if (files.length) parts.push((en ? 'Files: ' : 'File: ') + uniq(files).slice(0, 14).join('; '));
  if (asks.length) parts.push((en ? 'Asked: ' : 'Chiesto: ') + uniq(asks).slice(0, 8).map((a) => '"' + clampStr(a, 56) + '"').join(', '));
  if (facts.length) parts.push((en ? 'Learned: ' : 'Appreso: ') + uniq(facts).slice(0, 6).join(' '));
  return parts.map((p) => '- ' + p).join('\n');
}

function mergeDigest(prevText, summary, covers, lang) {
  const en = lang === 'en';
  const head = en ? `**Summary of ${covers} earlier turns**` : `**Riepilogo di ${covers} turni precedenti**`;
  const prevBody = String(prevText || '').replace(/^\*\*[^\n]*\*\*\n?/, '').trim();
  let body = [prevBody, summary].filter(Boolean).join('\n');
  if (body.length > 1800) body = body.slice(0, 1800) + '…';
  return head + '\n' + body;
}

// Fold a history array down to the byte budget. Returns {history, compacted} where
// `compacted` is the number of turns rolled into the digest (0 if nothing changed).
// Recent turns are kept by byte weight (~60% of budget), at least `minRecent` of them.
export function compact(history, { budget = 18000, lang = 'it', minRecent = 4, force = false } = {}) {
  if (!Array.isArray(history) || history.length === 0) return { history, compacted: 0 };
  if (!force && estimateBytes(history) <= budget) return { history, compacted: 0 };

  let lead = null, rest = history;
  if (history[0] && history[0].kind === 'digest') { lead = history[0]; rest = history.slice(1); }

  const keepBytes = force ? 0 : Math.floor(budget * 0.6);
  let acc = 0, idx = rest.length;
  for (let i = rest.length - 1; i >= 0; i--) {
    acc += estimateBytes([rest[i]]);
    idx = i;
    if (acc > keepBytes && (rest.length - i) >= minRecent) { idx = i + 1; break; }
  }
  const recent = rest.slice(idx);
  const older = rest.slice(0, idx);
  const pinned = older.filter((h) => h && h.pinned);
  const toDigest = older.filter((h) => h && !h.pinned);
  if (!toDigest.length) return { history, compacted: 0 };

  const covers = (lead ? lead.covers || 0 : 0) + toDigest.length;
  const digest = { role: 'system', kind: 'digest', covers, ts: 0,
    text: mergeDigest(lead ? lead.text : '', summarizeTurns(toDigest, lang), covers, lang) };
  return { history: [digest, ...pinned, ...recent], compacted: toDigest.length };
}

// Budget usage as a 0..1 ratio, for the context meter.
export function usage(history, budget = 18000) { return Math.min(1, estimateBytes(history) / budget); }
