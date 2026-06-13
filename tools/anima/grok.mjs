// Minimal Groq (teacher) client for offline data enrichment. Reads the key READ-ONLY from the device
// SD teacher.json (never logged). Used by build_refdata / enrich_born to fetch CANDIDATE reference facts
// that are then VERIFIED against the source-anchored corpus before anything is written — the model
// (llama-3.1-8b-instant) is small and WILL get some precise facts wrong, so nothing it says is trusted
// unmeasured. Exposes ask() returning parsed JSON (response_format json_object) with bounded retries.
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const TCFG = ['H:/data/anima/teacher.json', join(here, '..', 'sd-sim', 'data', 'anima', 'teacher.json')];
const path = TCFG.find(existsSync);
if (!path) { console.error('teacher.json not found (SD at H: or tools/sd-sim)'); process.exit(2); }
const cfg = JSON.parse(readFileSync(path, 'utf8'));
const KEY = cfg.key, BASE = cfg.base || 'https://api.groq.com/openai/v1', MODEL = cfg.model || 'llama-3.1-8b-instant';
if (!KEY) { console.error('no key in teacher.json'); process.exit(2); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Ask the model for a STRICT JSON object. system+user prompts; returns the parsed object (or null on
// repeated failure). Low temperature for factual determinism. Retries on rate-limit / transient errors.
export async function askJSON(system, user, { retries = 4, temperature = 0 } = {}) {
  for (let attempt = 0; attempt <= retries; attempt++) {
    try {
      const r = await fetch(`${BASE}/chat/completions`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${KEY}` },
        body: JSON.stringify({
          model: MODEL, temperature,
          response_format: { type: 'json_object' },
          messages: [{ role: 'system', content: system }, { role: 'user', content: user }],
        }),
      });
      if (r.status === 429 || r.status >= 500) { await sleep(1500 * (attempt + 1)); continue; }
      if (!r.ok) { console.error(`grok ${r.status}: ${(await r.text()).slice(0, 200)}`); return null; }
      const j = await r.json();
      const txt = j.choices?.[0]?.message?.content || '';
      try { return JSON.parse(txt); } catch { return null; }
    } catch (e) {
      await sleep(1500 * (attempt + 1));
    }
  }
  return null;
}

export const MODEL_NAME = MODEL;
