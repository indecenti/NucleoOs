/* SPDX-License-Identifier: AGPL-3.0-or-later */
// ANIMA — i modelli che girano sul PC dell'utente (Ollama, LM Studio, llama.cpp, Jan, KoboldCpp).
//
// Perché. La console non ha la RAM per un modello vero e WebLLM scarica centinaia di MB nel browser; ma
// il PC da cui si guarda la console spesso ne ha già uno, con una GPU vera: un 9B risponde a ~20 token/s
// (Ollama + qwen3.5:9b su questo PC, misurato il 21/10/2026). Si usa da dove gira la pagina: il browser
// sul PC chiama localhost. Niente passa dalla console, niente va in rete, nessuna chiave.
//
// L'ostacolo è uno solo, ed è una regola del server del modello, non nostra: Ollama risponde soltanto
// alle pagine di origini che conosce. Di serie conosce localhost/127.0.0.1 (qualsiasi porta), file://,
// app://... e NON l'indirizzo della console (http://192.168.42.1:8080 -> 403, misurato). La soluzione
// NON tocca la configurazione di Ollama: si porta la console sul PC come http://localhost:18080, con
// l'inoltro di porte che Windows ha già (netsh portproxy). Un file da aprire una volta (helperCmd), un
// avviso di Windows, e da lì la pagina ha un'origine che Ollama accetta così com'è — e in più localhost è
// un "contesto sicuro" per il browser: microfono, app installabile e WebGPU senza certificati.
//
// Puro: fetch è iniettata (`f`), così si prova in Node contro server finti.

export const PC_PORT = 18080;
export const USB_IP = '192.168.42.1';

// I server locali più diffusi, ognuno sulla sua porta di serie. `native`: Ollama ha un'API sua, che
// permette think:false (niente "ragionamento" lento) e num_ctx (la finestra: quella di serie taglierebbe
// il prompt con i fatti della console). Gli altri parlano il protocollo OpenAI.
export const SERVERS = [
  { id: 'ollama', label: 'Ollama', base: 'http://localhost:11434', list: '/api/tags', native: true },
  { id: 'lmstudio', label: 'LM Studio', base: 'http://localhost:1234/v1', list: '/models' },
  { id: 'llamacpp', label: 'llama.cpp', base: 'http://localhost:8080/v1', list: '/models' },
  { id: 'jan', label: 'Jan', base: 'http://localhost:1337/v1', list: '/models' },
  { id: 'kobold', label: 'KoboldCpp', base: 'http://localhost:5001/v1', list: '/models' },
];

// ---- modelli -----------------------------------------------------------------------------------------
// "9.7B", "494.03M", "qwen2.5:14b", "llama-3.1-8b-instruct" -> miliardi di parametri (null se non si sa)
export function paramsB(s) {
  const t = String(s || '').toLowerCase();
  let m = t.match(/(\d+(?:\.\d+)?)\s*b\b/) || t.match(/[:\-_](\d+(?:\.\d+)?)b(?![a-z])/);
  if (m) return +m[1];
  m = t.match(/(\d+(?:\.\d+)?)\s*m\b/) || t.match(/[:\-_](\d+(?:\.\d+)?)m(?![a-z])/);
  return m ? +m[1] / 1000 : null;
}
// Il modello per parlare: abbastanza grande da ragionare (sotto i 3B le risposte diventano casuali),
// abbastanza piccolo da rispondere in fretta. Quelli che non chattano (embedding, estrazione, voce)
// non si scelgono mai; quelli da codice valgono meno per una conversazione.
const NOT_CHAT = /embed|rerank|extract|whisper|tts|bge|nomic|minilm|e5-|clip|guard|ocr/;
export function modelScore(m) {
  const name = String(m.id || '').toLowerCase();
  if (NOT_CHAT.test(name)) return -1;
  const p = m.params != null ? m.params : paramsB(name);
  let s = p == null ? 50 : p < 3 ? 25 + p * 12 : p <= 16 ? 100 - Math.abs(p - 9) * 2 : Math.max(20, 80 - (p - 16) * 1.5);
  if (/coder|code/.test(name)) s -= 30;
  return s;
}
export function rankModels(models) {
  return (models || []).map((m) => Object.assign({}, m, { score: modelScore(m) })).filter((m) => m.score > 0).sort((a, b) => b.score - a.score);
}
function parseModels(srv, j) {
  if (srv.native) return ((j && j.models) || []).map((m) => ({ id: m.name || m.model, params: paramsB(m.details && m.details.parameter_size), family: m.details && m.details.family, bytes: m.size }));
  return ((j && j.data) || []).map((m) => ({ id: m.id, params: paramsB(m.id) }));
}

// ---- rilevamento ---------------------------------------------------------------------------------------
async function withTimeout(f, url, opts, ms) {
  const ac = typeof AbortController !== 'undefined' ? new AbortController() : null;
  const t = ac ? setTimeout(() => ac.abort(), ms) : null;
  try { return await f(url, Object.assign({}, opts, ac ? { signal: ac.signal } : {})); } finally { if (t) clearTimeout(t); }
}
// Un server: 'ok' (risponde e si legge), 'blocked' (c'è, ma non risponde a QUESTA pagina: lo si capisce
// perché una richiesta no-cors, illeggibile ma che non chiede permessi, va a buon fine), 'absent'.
export async function probe(f, srv, ms = 1500) {
  const url = srv.base + srv.list;
  try {
    const r = await withTimeout(f, url, { cache: 'no-store' }, ms);
    if (r.ok) { const j = await r.json().catch(() => null); return { srv, state: 'ok', models: rankModels(parseModels(srv, j)) }; }
    if (r.status === 403) return { srv, state: 'blocked', models: [] };
    return { srv, state: 'absent', models: [] };
  } catch {
    try { await withTimeout(f, url, { mode: 'no-cors', cache: 'no-store' }, ms); return { srv, state: 'blocked', models: [] }; }
    catch { return { srv, state: 'absent', models: [] }; }
  }
}
// Tutti i server insieme (in parallelo: ~1,5 s nel caso peggiore). best = il primo modello del primo
// server che risponde; prefer = un modello scelto dall'utente, se c'è ancora.
// hidden: la pagina NON vede il PC. Chrome (Local Network Access, provato su Chrome 152 il 21/10/2026) non
// lascia partire nessuna richiesta verso localhost da una pagina http:// aperta a un indirizzo di rete, nemmeno
// no-cors: "assente" e "bloccato" diventano indistinguibili. opts.localOrigin = la pagina è già su localhost.
export async function detect(f, opts = {}) {
  const list = await Promise.all(SERVERS.map((s) => probe(f, s, opts.timeout || 1500)));
  const ok = list.filter((x) => x.state === 'ok' && x.models.length);
  let best = null;
  if (opts.prefer) for (const x of ok) { const m = x.models.find((m) => m.id === opts.prefer); if (m) { best = { srv: x.srv, model: m }; break; } }
  if (!best && ok.length) best = { srv: ok[0].srv, model: ok[0].models[0] };
  const hidden = !opts.localOrigin && list.every((x) => x.state === 'absent');
  return { servers: list, best, blocked: list.filter((x) => x.state === 'blocked').map((x) => x.srv), hidden };
}

// ---- parlare -------------------------------------------------------------------------------------------
// I modelli che ragionano (qwen3, deepseek-r1...) possono lasciare il ragionamento nel testo: via.
export function stripThink(s) { return String(s || '').replace(/<think>[\s\S]*?<\/think>\s*/g, '').replace(/^[\s\S]*?<\/think>\s*/, '').trim(); }
// onDelta(caratteriFinora, testoFinora): la risposta arriva a pezzi (stream) e chi chiama può mostrare che il
// modello sta scrivendo - un gioco intero su un 9B sono minuti. Senza onDelta, una risposta sola.
export async function pcChat(f, srv, model, { system, messages = [], maxTokens = 1024, temperature = 0.4, ctx = 8192, signal, onDelta } = {}) {
  const msgs = [...(system ? [{ role: 'system', content: system }] : []), ...messages];
  const stream = typeof onDelta === 'function';
  const body = srv.native
    ? { model, messages: msgs, stream, think: false, keep_alive: KEEP, options: { num_ctx: ctx, num_predict: maxTokens, temperature } }
    : { model, messages: msgs, stream, max_tokens: maxTokens, temperature };
  const r = await f(srv.base + (srv.native ? '/api/chat' : '/chat/completions'), { method: 'POST', signal, headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
  if (stream && r.ok && r.body && r.body.getReader) {
    const rd = r.body.getReader(), dec = new TextDecoder();
    let buf = '', full = '';
    for (;;) {
      const { value, done } = await rd.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        let line = buf.slice(0, nl).trim(); buf = buf.slice(nl + 1);
        if (!line) continue;
        if (!srv.native) { if (!line.startsWith('data:')) continue; line = line.slice(5).trim(); if (line === '[DONE]') continue; }
        let ev; try { ev = JSON.parse(line); } catch { continue; }
        if (ev.error) { const e = new Error(ev.error.message || ev.error); e.status = r.status; throw e; }
        const piece = srv.native ? (ev.message && ev.message.content) : (ev.choices && ev.choices[0] && ev.choices[0].delta && ev.choices[0].delta.content);
        if (piece) { full += piece; try { onDelta(full.length, full); } catch {} }
      }
    }
    return stripThink(full);
  }
  const j = await r.json().catch(() => null);
  if (!r.ok || !j || j.error) { const e = new Error((j && (j.error && (j.error.message || j.error))) || ('HTTP ' + r.status)); e.status = r.status; throw e; }
  const txt = srv.native ? (j.message && j.message.content) : (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content);
  return stripThink(txt);
}
// Ollama carica un modello in memoria alla prima domanda: con un 9B sono ~25 s di attesa (misurato). Appena
// ANIMA lo trova lo fa caricare (un /api/generate senza prompt carica e basta) e lo tiene caldo mezz'ora:
// la prima domanda vera risponde subito. Gli altri server non hanno un comando così: lì non si fa niente.
// Poi guarda (/api/ps) se il modello è RIMASTO in memoria: con alcune combinazioni modello/scheda video Ollama lo
// scarica subito dopo ogni richiesta, keep_alive o no (qwen3.5:9b su una RTX 5070 Laptop da 8 GB: ~20 s di
// caricamento a OGNI domanda, misurato il 21/10/2026). Non si può aggiustare da qui: si dice, e si consiglia
// un modello più leggero. Ritorna {ok, resident}; resident null = non si sa.
const KEEP = '30m';
export async function warm(f, srv, model) {
  if (!srv.native) return { ok: false, resident: null };
  const post = (p, body) => f(srv.base + p, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
  try {
    const r = await post('/api/generate', { model, keep_alive: KEEP });
    if (!r.ok) return { ok: false, resident: null };
    let resident = null;
    try { const ps = await (await f(srv.base + '/api/ps', { cache: 'no-store' })).json(); resident = ((ps && ps.models) || []).some((m) => m.name === model || m.model === model); } catch {}
    return { ok: true, resident };
  } catch { return { ok: false, resident: null }; }
}
// Lo stesso modello come "provider" OpenAI per gli Agenti (strumenti, cicli): Ollama espone anche /v1.
export function agentCfg(srv, model) {
  return { provider: 'openai', key: 'local', base: srv.native ? srv.base + '/v1' : srv.base, model, local: true, label: srv.label };
}

// ---- l'origine della pagina ------------------------------------------------------------------------------
// localhost / 127.0.0.1 / [::1]: le origini che Ollama accetta di serie e che il browser considera sicure.
export function isLocalOrigin(loc) {
  const h = String((loc && loc.hostname) || '').toLowerCase();
  return h === 'localhost' || h === '127.0.0.1' || h === '[::1]' || h === '::1' || h.endsWith('.localhost');
}
export function localUrl(path) { return 'http://localhost:' + PC_PORT + (path || '/'); }

// Il file "una volta per PC" (Windows): inoltra http://localhost:18080 alla console con netsh portproxy,
// che Windows ha già. Nessuna installazione e nessuna modifica a Ollama: l'unica regola nuova è l'inoltro,
// che resta (anche dopo un riavvio) e che si toglie con `netsh interface portproxy delete v4tov4
// listenaddress=127.0.0.1 listenport=18080`. Stessa forma del file di "Internet dal PC": un .cmd che
// estrae lo script PowerShell da sé e lo lancia; lo script chiede da solo i permessi (UAC).
// host: l'IP della console com'è visto dal PC (192.168.42.1 col cavo, l'IP WiFi altrimenti).
export function helperCmd(host, opts = {}) {
  const ip = /^\d{1,3}(\.\d{1,3}){3}$/.test(String(host)) ? host : USB_IP;
  const port = opts.port || PC_PORT, open = opts.open || '/apps/anima/';
  const sep = '#==' + 'PS1==';
  const head = [
    '@echo off',
    'rem NucleoOS - la console anche come http://localhost:' + port + ' su questo PC (una volta per PC).',
    'rem Serve ad ANIMA per usare i modelli del PC (Ollama accetta le pagine di localhost) senza toccarne la configurazione.',
    'rem Doppio clic; Windows chiede la conferma da amministratore. Nient\'altro da installare.',
    'powershell -NoProfile -ExecutionPolicy Bypass -Command "' +
      '$t=[IO.File]::ReadAllText(\'%~f0\');' +
      '$i=$t.LastIndexOf(\'#==\'+\'PS1==\');' +
      '$f=Join-Path $env:TEMP \'nucleo-localhost.ps1\';' +
      '[IO.File]::WriteAllText($f,$t.Substring($i+8).TrimStart([char]13,[char]10),(New-Object Text.UTF8Encoding $true));' +
      '& powershell -NoProfile -ExecutionPolicy Bypass -File $f"',
    'exit /b',
  ];
  const ps = [
    '$Console = "' + ip + '"; $Port = ' + port + '; $Open = "' + open + '"',
    '$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()',
    'if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {',
    '  try { Start-Process powershell -Verb RunAs -Wait -ArgumentList @("-NoProfile","-ExecutionPolicy","Bypass","-File","`"$PSCommandPath`"") }',
    '  catch { Write-Host "Serve la conferma da amministratore: riapri il file e scegli Si."; Start-Sleep 6; exit 1 }',
    '  Start-Process ("http://localhost:" + $Port + $Open); exit 0',
    '}',
    'Write-Host "NucleoOS: la console $Console diventa http://localhost:$Port su questo PC..."',
    '# l\'inoltro di porte di Windows vive nel servizio "Supporto IP" (iphlpsvc): acceso e automatico',
    'Set-Service iphlpsvc -StartupType Automatic -ErrorAction SilentlyContinue',
    'Start-Service iphlpsvc -ErrorAction SilentlyContinue',
    'netsh interface portproxy delete v4tov4 listenaddress=127.0.0.1 listenport=$Port 2>$null | Out-Null',
    'netsh interface portproxy add v4tov4 listenaddress=127.0.0.1 listenport=$Port connectaddress=$Console connectport=8080 | Out-Null',
    '$ok = $false',
    'try { $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 6 ("http://localhost:" + $Port + "/api/library/rev"); $ok = ($r.StatusCode -eq 200) } catch {}',
    'if ($ok) { Write-Host "Fatto. Da ora: http://localhost:$Port (con la console collegata)." }',
    'else { Write-Host "Inoltro creato, ma la console non risponde adesso: collegala e apri http://localhost:$Port" }',
    'Start-Sleep 3',
  ];
  return head.join('\r\n') + '\r\n' + sep + '\r\n' + ps.join('\r\n') + '\r\n';
}
// Mac e Linux: nessun inoltro di sistema senza installare niente; c'è ssh, già presente. Finché la finestra
// resta aperta, http://localhost:18080 è la console (password della console: quella di SSH).
export function sshHint(host) {
  const ip = /^\d{1,3}(\.\d{1,3}){3}$/.test(String(host)) ? host : USB_IP;
  return 'ssh -N -L ' + PC_PORT + ':127.0.0.1:8080 ark@' + ip;
}
