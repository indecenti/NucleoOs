// clip-tokenizer.js — CLIP ViT-L/14 BPE tokenizer (the text front-end SDXS shares with Stable Diffusion 1.5).
// Pure, DOM-free, dependency-free: the vocab + merge table are INJECTED (loaded from the model's
// tokenizer.json on SD), so the algorithm is host-testable on a tiny synthetic vocab and the real
// CLIP parity is confirmed on-device against the shipped vocab. Implements the standard pipeline:
//   lowercase + whitespace-clean → byte→unicode map → CLIP word regex → BPE merges (with the </w> word-end
//   marker) → vocab ids → [BOS, …, EOS] padded/truncated to the 77-token context window.
//
// makeClipTokenizer({ vocab, merges, bos?, eos?, pad?, maxLen? }) → { encode(text) -> Int32Array(maxLen) }
//   vocab : { "<token></w>": id, … }   (token strings already include the </w> marker where applicable)
//   merges: [["a","b"], …]             (BPE merge ranks, best first) OR ["a b", …]

// GPT-2/CLIP byte→unicode table: maps every byte to a printable unicode codepoint so BPE runs over text.
function byteToUnicode() {
  const bs = [];
  for (let i = 33; i <= 126; i++) bs.push(i);
  for (let i = 161; i <= 172; i++) bs.push(i);
  for (let i = 174; i <= 255; i++) bs.push(i);
  const cs = bs.slice(); let n = 0;
  for (let b = 0; b < 256; b++) if (!bs.includes(b)) { bs.push(b); cs.push(256 + n); n++; }
  const map = {};
  for (let i = 0; i < bs.length; i++) map[bs[i]] = String.fromCharCode(cs[i]);
  return map;
}
const B2U = byteToUnicode();

// CLIP's pre-tokenization regex (lowercased input): contractions, letters, digits, and other runs.
const PAT = /<\|startoftext\|>|<\|endoftext\|>|'s|'t|'re|'ve|'m|'ll|'d|[\p{L}]+|[\p{N}]|[^\s\p{L}\p{N}]+/gu;

function getPairs(word) {
  const pairs = new Set();
  for (let i = 0; i < word.length - 1; i++) pairs.add(word[i] + '' + word[i + 1]);
  return pairs;
}

export function makeClipTokenizer({ vocab, merges, bos = 49406, eos = 49407, pad, maxLen = 77 } = {}) {
  if (!vocab || typeof vocab !== 'object') throw new Error('vocab required');
  const ranks = new Map();
  (merges || []).forEach((m, i) => { const [a, b] = Array.isArray(m) ? m : String(m).split(/\s+/); ranks.set(a + '' + b, i); });
  const padId = pad == null ? eos : pad;
  const unk = vocab['<|endoftext|>'] != null ? vocab['<|endoftext|>'] : eos;

  function bpe(token) {
    // token: a pre-token string already byte→unicode mapped. Append </w> to the last char.
    let word = token.split('');
    word[word.length - 1] = word[word.length - 1] + '</w>';
    if (word.length === 1) return word;
    let pairs = getPairs(word);
    while (true) {
      let best = null, bestRank = Infinity;
      for (const p of pairs) { const r = ranks.get(p); if (r !== undefined && r < bestRank) { bestRank = r; best = p; } }
      if (best === null) break;
      const [a, b] = best.split('');
      const next = []; let i = 0;
      while (i < word.length) {
        const j = word.indexOf(a, i);
        if (j === -1) { next.push(...word.slice(i)); break; }
        next.push(...word.slice(i, j)); i = j;
        if (word[i] === a && i < word.length - 1 && word[i + 1] === b) { next.push(a + b); i += 2; }
        else { next.push(word[i]); i += 1; }
      }
      word = next;
      if (word.length === 1) break;
      pairs = getPairs(word);
    }
    return word;
  }

  function encode(text) {
    const clean = String(text == null ? '' : text).toLowerCase().replace(/\s+/g, ' ').trim();
    const ids = [bos];
    const toks = clean.match(PAT) || [];
    for (const t of toks) {
      // byte→unicode the UTF-8 bytes of this pre-token
      const bytes = new TextEncoder().encode(t);
      let mapped = ''; for (const byte of bytes) mapped += B2U[byte];
      for (const piece of bpe(mapped)) {
        const id = vocab[piece];
        ids.push(id === undefined ? unk : id);
      }
    }
    ids.push(eos);
    // pad / truncate to maxLen (keep BOS…EOS; if truncating, force EOS in the last slot)
    const out = new Int32Array(maxLen).fill(padId);
    const n = Math.min(ids.length, maxLen);
    for (let i = 0; i < n; i++) out[i] = ids[i];
    if (ids.length > maxLen) out[maxLen - 1] = eos;
    return out;
  }

  return { encode, maxLen, bos, eos };
}
