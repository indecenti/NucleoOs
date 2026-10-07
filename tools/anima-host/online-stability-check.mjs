#!/usr/bin/env node
// ONLINE-STABILITY guard (docs/anima-native.md §stability) — locks the 2026-06-24 anti-reboot fix so it
// can NEVER silently regress. The on-device ANIMA online tier shares nucleo_anima_online.c between the
// native app worker AND the web /api/anima handler; a single esp_http_client_perform() is ONE un-pettable
// blocking call, so a per-attempt timeout >= the Task-WDT is a guaranteed reboot the moment a TLS handshake
// stalls on the fragmented PSRAM-less heap (the .166 crash: timeouts were 8/10/20 s vs an 8 s TWDT).
//
// Pure source-invariant check (no exe/device). Run by `npm run anima:gate`. Asserts:
//   1. per-attempt socket timeout (HTTP_TIMEOUT) stays a safe margin BELOW CONFIG_ESP_TASK_WDT_TIMEOUT_S
//   2. the chat TLS paths bind that symbol, not a raw literal that could dodge the bound
//   3. only the audio-upload (transcribe) path may use a long numeric timeout, and the file pets the WDT
//   4. the wall-clock turn budget + the WDT-pet helper exist and are wired at the retry/perform boundaries
//   5. every online failure logs heap state (the immediate "why" in /api/logs the user asked for)
//   6. nucleo_exclusive_enter returns PER-CALL OWNERSHIP (return acted) so a paired exit can't collapse
//      the session window someone else still owns (the refcount fix)
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const read = (p) => readFileSync(join(repo, p), 'utf8');

const fails = [];
const ok = (cond, msg) => { if (!cond) fails.push(msg); };

// --- 1) the Task-WDT timeout the per-attempt socket timeout must stay under -------------------------
const sdk = read('firmware/sdkconfig.defaults');
const twdM = sdk.match(/CONFIG_ESP_TASK_WDT_TIMEOUT_S=(\d+)/);
ok(!!twdM, 'CONFIG_ESP_TASK_WDT_TIMEOUT_S not found in sdkconfig.defaults');
const twdMs = twdM ? Number(twdM[1]) * 1000 : 8000;

const src = read('firmware/components/nucleo_anima/nucleo_anima_online.c');

const htM = src.match(/#define\s+HTTP_TIMEOUT\s+(\d+)/);
ok(!!htM, 'HTTP_TIMEOUT #define not found in nucleo_anima_online.c');
const ht = htM ? Number(htM[1]) : 1e9;
ok(ht <= twdMs - 1000,
   `HTTP_TIMEOUT (${ht} ms) must be <= TWDT-1000 (${twdMs - 1000} ms): one stalled handshake is an un-pettable ` +
   `blocking call and must finish inside the ${twdMs / 1000} s Task-WDT with margin`);

// --- 2) chat TLS paths (get / post_json / post_anthropic) use the symbol, not a raw literal ---------
// A path may bind the symbol directly, or `tmo` — the per-question/per-POST budget CLAMP, accepted only
// while its helpers provably cap it at HTTP_TIMEOUT (so the TWDT bound above still holds).
const symUses = (src.match(/\.timeout_ms\s*=\s*(HTTP_TIMEOUT|tmo)\b/g) || []).length;
ok(symUses >= 3,
   `expected >=3 chat TLS paths binding ".timeout_ms = HTTP_TIMEOUT" (or the clamped "tmo") (get/post/anthropic), ` +
   `found ${symUses} — a raw numeric literal would dodge the TWDT bound`);
if (/\.timeout_ms\s*=\s*tmo\b/.test(src)) {
  ok(/static\s+int\s+net_attempt_timeout[\s\S]*?return[^;]*HTTP_TIMEOUT[^;]*;/.test(src),
     'net_attempt_timeout() must cap the per-attempt timeout at HTTP_TIMEOUT (the TWDT-safe bound)');
  ok(/static\s+int\s+post_attempt_timeout[\s\S]*?net_attempt_timeout\(/.test(src),
     'post_attempt_timeout() must derive from net_attempt_timeout() (never exceed HTTP_TIMEOUT)');
}

// --- 3) at most ONE numeric literal timeout (the audio-upload transcribe path, on an unwatched task) ---
const numLits = [...src.matchAll(/\.timeout_ms\s*=\s*(\d+)/g)].map((m) => Number(m[1]));
ok(numLits.length <= 1,
   `only the transcribe upload may use a numeric .timeout_ms (it pets the WDT per-chunk); found ${numLits.length}` +
   (numLits.length ? `: ${numLits.join(', ')}` : ''));

// --- 4) wall-clock turn budget + the WDT-pet helper exist and are wired in --------------------------
ok(/#define\s+TLS_TURN_BUDGET_MS\s+\d+/.test(src), 'TLS_TURN_BUDGET_MS (per-turn wall-clock budget) #define missing');
ok(/static\s+inline\s+void\s+tls_wdt_pet/.test(src), 'tls_wdt_pet() helper missing');
ok((src.match(/tls_wdt_pet\(\)/g) || []).length >= 3,
   'tls_wdt_pet() must be called at the retry/perform boundaries (>=3 sites: get, the two POST loops, transcribe)');

// --- 5) every online failure logs heap state (immediate "why" in /api/logs) -------------------------
ok(/FAIL[^\n]*free=%u largest=%u/.test(src),
   'online failure logs must carry free=%u largest=%u heap state for on-device diagnosis');

// --- 6) nucleo_exclusive_enter returns per-call ownership, not global active-ness --------------------
const excl = read('firmware/components/nucleo_app/nucleo_exclusive.c');
// Positive-only (the legit nucleo_exclusive_active() also says "return s_active != 0"): enter() is the
// only place that declares `bool acted` and returns it. A revert to "return s_active != 0" drops both.
ok(/\bbool\s+acted\b/.test(excl) && /return\s+acted\s*;/.test(excl),
   'nucleo_exclusive_enter must compute and "return acted" (this-call ownership) so a paired enter/exit ' +
   'caller never tears down a window another owner still holds');

// --- 7) teacher.json: never silently truncated into "no key" (2026-10 review) -----------------------
// A multi-provider teacher.json outgrew the old fixed 1535-byte read -> cJSON failed -> "no key", no log.
// The reader must size-check (too big -> ESP_LOGW + explicit fail), and EVERY read must go through it
// (no stray direct fread of teacher.json into a fixed stack buffer).
{
  const rf = (src.match(/static\s+char\s*\*\s*teacher_read_file\s*\([\s\S]*?\n\}/) || [''])[0];
  ok(!!rf, 'teacher_read_file must return a heap buffer (char *) sized to the file, caller frees');
  ok(/st_size|ftell/.test(rf) && /ESP_LOGW/.test(rf) && /TEACHER_JSON_MAX/.test(rf),
     'teacher_read_file: size-check against TEACHER_JSON_MAX and ESP_LOGW when the file is too big');
  ok((src.match(/fopen\(\s*NUCLEO_SD_MOUNT\s*"\/data\/anima\/teacher\.json"/g) || []).length === 1,
     'teacher.json must be opened in ONE place (teacher_read_file) — direct fread copies truncate silently');
}

// --- 8) summarize_file: the partials buffer is capped, and the merge request is clipped ---------------
{
  const sf = (src.match(/int\s+nucleo_anima_summarize_file\s*\([\s\S]*?\n\}/) || [''])[0];
  ok(/SUM_PARTIALS_MAX/.test(sf) && !/realloc\(/.test(sf),
     'nucleo_anima_summarize_file: partials must be capped at SUM_PARTIALS_MAX (no unbounded realloc +2048 per window)');
  ok(/SUM_MERGE_MAX/.test(sf), 'nucleo_anima_summarize_file: clip partials to SUM_MERGE_MAX before the merge call (like %.3100s)');
}

// --- 9) clip_reply: a truncated multibyte lead must not advance p past the terminating NUL ----------
{
  const cr = (src.match(/static\s+void\s+clip_reply\s*\([\s\S]*?\n\}/) || [''])[0];
  ok(!!cr && !/p\s*\+=\s*len/.test(cr), 'clip_reply: no blind `p += len` — bound the advance by checking p[k] for k < len');
}

if (fails.length) {
  console.error('[online-stability] FAIL:\n  - ' + fails.join('\n  - '));
  process.exit(1);
}
console.log(`[online-stability] OK — per-attempt ${ht}ms < TWDT ${twdMs}ms (margin ${twdMs - ht}ms); ` +
            `${symUses} symbol-bound chat paths; budget+wdt-pet+heap-logging+ownership present`);
process.exit(0);
