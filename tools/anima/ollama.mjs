// ollama.mjs — tiny client for a LOCAL ollama server (default http://localhost:11434).
// ANIMA uses it as an OFFLINE teacher: it proposes paraphrase QUESTIONS and (optionally)
// embeddings for collision detection — it never writes answers (those stay sourced). Pure HTTP,
// no deps. Set OLLAMA_HOST to point elsewhere. Every call degrades gracefully (null / throw).
import { setTimeout as sleep } from 'node:timers/promises';

export const OLLAMA = (process.env.OLLAMA_HOST || 'http://localhost:11434').replace(/\/+$/, '');

export async function available() {
  try { const r = await fetch(`${OLLAMA}/api/tags`, { signal: AbortSignal.timeout(1500) }); return r.ok; }
  catch { return false; }
}

export async function listModels() {
  try { const r = await fetch(`${OLLAMA}/api/tags`); if (!r.ok) return []; const j = await r.json(); return (j.models || []).map((m) => m.name); }
  catch { return []; }
}

// Chat completion that returns parsed JSON (ollama `format:"json"` constrains the model to JSON).
export async function genJSON(system, user, { model = 'llama3.1:8b', temperature = 0.2, retries = 3 } = {}) {
  for (let a = 0; a < retries; a++) {
    try {
      const r = await fetch(`${OLLAMA}/api/chat`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model, stream: false, format: 'json', options: { temperature },
          messages: [{ role: 'system', content: system }, { role: 'user', content: user }] }),
      });
      if (r.status >= 500) { await sleep(800 * (a + 1)); continue; }
      if (!r.ok) { console.error('ollama', r.status, (await r.text()).slice(0, 160)); return null; }
      const j = await r.json();
      const c = j?.message?.content;
      if (!c) return null;
      try { return JSON.parse(c); } catch { return null; }
    } catch (e) { await sleep(700 * (a + 1)); }
  }
  return null;
}

// Embedding vector for one string (default a small embed model). Returns number[] or null.
export async function embed(text, { model = 'nomic-embed-text' } = {}) {
  try {
    const r = await fetch(`${OLLAMA}/api/embeddings`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ model, prompt: String(text || '') }),
    });
    if (!r.ok) return null;
    const j = await r.json();
    return Array.isArray(j.embedding) ? j.embedding : null;
  } catch { return null; }
}

// Memoised embedder: caches by string so the same ask is embedded once.
export function makeEmbedder(opts = {}) {
  const cache = new Map();
  return async (text) => {
    if (cache.has(text)) return cache.get(text);
    const v = await embed(text, opts); cache.set(text, v); return v;
  };
}
