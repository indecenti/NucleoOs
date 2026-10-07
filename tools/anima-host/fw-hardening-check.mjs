#!/usr/bin/env node
// FIRMWARE HARDENING guard — locks the 2026-10 review fixes in code the host harness does NOT compile
// (native app .cpp, ESP-NOW glue). Pure source-invariant check (no exe/device), mirroring
// online-stability-check.mjs. Each assertion fails on the pre-fix code and passes on the fixed code.
//
//   node tools/anima-host/fw-hardening-check.mjs            check the working tree
//   node tools/anima-host/fw-hardening-check.mjs --rev REF  check a git revision (read-only `git show`),
//                                                           e.g. --rev HEAD to prove the checks bite
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const ri = process.argv.indexOf('--rev');
const rev = ri > 0 ? process.argv[ri + 1] : null;
const C = 'firmware/components/';
const read = (p) => rev
  ? execFileSync('git', ['show', `${rev}:${C}${p}`], { cwd: repo, encoding: 'utf8', maxBuffer: 16 << 20 })
  : readFileSync(join(repo, C, p), 'utf8');

const fails = [];
const ok = (cond, msg) => { if (!cond) fails.push(msg); };
// body of `name(...) { ... }` up to the first line that is exactly "}" (file style: top-level closers)
const fnBody = (src, sig) => { const i = src.indexOf(sig); if (i < 0) return ''; const j = src.indexOf('\n}', i); return src.slice(i, j < 0 ? undefined : j); };

// --- 1) Tanks: the peer `starter`/`next` byte indexes s_tk[2]; session packets only from the bound peer
{
  const s = read('nucleo_app/app_tanks.cpp');
  ok(/s_active\s*=\s*starter\s*&\s*1\s*;/.test(s), 'app_tanks build_match: `s_active = starter & 1` (wire byte indexes s_tk[2])');
  ok(/s_active\s*=\s*r->next\s*&\s*1\s*;/.test(s), 'app_tanks apply_result: `s_active = r->next & 1` (wire byte indexes s_tk[2])');
  const nh = fnBody(s, 'static void net_handle(const pnet_pkt_t *p)');
  const iPeer = nh.search(/memcmp\(p->mac,\s*s_peer,\s*6\)/), iAck = nh.indexOf('TK_ACK'), iBye = nh.indexOf('TK_BYE');
  ok(iPeer > 0 && iPeer < iAck && iPeer < iBye, 'app_tanks net_handle: the peer-MAC check must come BEFORE the ACK/BYE/rematch-START branches');
  const br = nh.slice(nh.indexOf('ST_BROWSE'));
  const st = br.slice(br.indexOf('type == TK_START'), br.indexOf('build_match'));
  ok(/memcmp\(p->mac,\s*s_peer,\s*6\)/.test(st) || /from_peer/.test(st),
     'app_tanks browse: TK_START accepted only from the host we joined (the MAC that sends TK_WELCOME)');
  ok(!/memcpy\(s_peer,\s*p->mac,\s*6\);\s*s_haspeer\s*=\s*true;\s*s_seat\s*=\s*1/.test(s),
     'app_tanks browse: TK_START must not adopt ANY sender as the peer');
}

// --- 2) TankDuel: in-match INPUT/BUY/STATE only from the peer; peer dir is a 4-way index
{
  const s = read('nucleo_app/app_tankduel.cpp');
  ok(/GS_OVER\)&&s_haspeer\)\{\s*\r?\n\s*if\(memcmp\(p->mac,s_peer,6\)\) return;/.test(s),
     'app_tankduel: the in-match branch must start with `if(memcmp(p->mac,s_peer,6)) return;` (Pong/Snake/Brawler pattern)');
  ok(/s_tanks\[0\]\.dir=st->p1dir&3;/.test(s) && /s_tanks\[1\]\.dir=st->p2dir&3;/.test(s),
     'app_tankduel TD_STATE: mask the peer tank dir with &3 (it indexes BDX/BDY[4])');
}

// --- 3) Vicino: a peer command longer than the confirm card shows is never run
{
  const s = read('nucleo_app/app_link.cpp');
  ok(/#define\s+CMD_SHOW\s+\d+/.test(s), 'app_link: CMD_SHOW (chars the confirm card shows in full) missing');
  ok(/"%\.\*s",\s*CMD_SHOW,\s*cmd/.test(s) && !/"%\.18s",\s*cmd/.test(s), 'app_link draw_cmd: preview must be bound by CMD_SHOW');
  ok(/strlen\(tmp\)\s*>\s*CMD_SHOW[\s\S]{0,80}nlink_svc_cmd_confirm\(false\)/.test(s),
     'app_link: Y on a command longer than CMD_SHOW must DISCARD it, not run it');
  const e = read('nucleo_link/nucleo_link_espnow.c');
  ok(/nlink_clean_cmd\(/.test(e), 'nucleo_link_espnow finish(): received command must go through nlink_clean_cmd (no control bytes)');
}

// --- 4) Vicino: peer file names sanitised; Bruce receive pinned + capped
{
  const e = read('nucleo_link/nucleo_link_espnow.c');
  ok(/nlink_safe_name\(/.test(fnBody(e, 'static void unique_dest(')), 'unique_dest: the peer name must pass nlink_safe_name before "%s/%s%s"');
  const bf = fnBody(e, 'static void bruce_on_frame(');
  ok(/memcmp\(mac,\s*s_brecv\.mac,\s*6\)/.test(bf), 'bruce_on_frame: an active receive must be pinned to the first sender MAC');
  ok(/#define\s+BRUCE_RX_MAX\b/.test(e) && /BRUCE_RX_MAX/.test(bf), 'bruce_on_frame: total received bytes must be capped (BRUCE_RX_MAX)');
}

// --- 8) Swarm: failed start must not leak the queue/mutex; ESP_ERR_ESPNOW_EXIST is success
{
  const s = read('nucleo_mesh/nucleo_swarm_espnow.c');
  const b = fnBody(s, 'bool swarm_svc_start(void)');
  ok(/ESP_ERR_ESPNOW_EXIST/.test(b), 'swarm_svc_start: treat ESP_ERR_ESPNOW_EXIST as success (esp_now is a shared singleton)');
  ok(!/\{\s*ESP_LOGE\(TAG,\s*"alloc"\);\s*return false;\s*\}/.test(b) && !/esp_now_init\(\)\s*!=\s*ESP_OK\)\s*\{[^}]*return false;/.test(b),
     'swarm_svc_start: failure paths must free the queue + mutex (no bare `return false`)');
}

// --- 10) peer names come from a 22-byte wire field: never "%s" (unterminated -> overread)
{
  const t = read('nucleo_app/app_tanks.cpp'), p = read('nucleo_app/app_pong.cpp'), d = read('nucleo_app/app_tankduel.cpp');
  const raw = (src, re) => (src.match(re) || []).length;
  ok(raw(t, /snprintf\(s_rooms\[[^\]]*\]\.name,\s*22,\s*"%s"/g) === 0, 'app_tanks room_add: copy the wire name with "%.21s"');
  ok(raw(t, /snprintf\(s_peer_name,\s*22,\s*"%s",\s*\(const char \*\)p->buf/g) === 0, 'app_tanks TK_JOIN: copy the wire name with "%.21s"');
  ok(raw(p, /snprintf\(s_hosts\[[^\]]*\]\.name,\s*22,\s*"%s"/g) === 0, 'app_pong host_add: copy the wire name with "%.21s"');
  ok(raw(d, /snprintf\(s_hosts\[[^\]]*\]\.name,22,"%s"/g) === 0, 'app_tankduel host_add: copy the wire name with "%.21s"');
}

// --- 11) pnet: recv_cb reads s_rxq once; stop unhooks + NULLs before deleting the queue
{
  const s = read('nucleo_pnet/nucleo_pnet.c');
  const cb = fnBody(s, 'static void recv_cb(');
  ok((cb.match(/s_rxq/g) || []).length === 1, 'pnet recv_cb: read s_rxq ONCE into a local (stop may NULL it concurrently)');
  const st = fnBody(s, 'void pnet_stop(void)');
  const iNull = st.search(/s_rxq\s*=\s*NULL/), iDeinit = st.indexOf('esp_now_deinit'), iDel = st.indexOf('vQueueDelete');
  ok(iNull >= 0 && iDeinit >= 0 && iDel >= 0 && iNull < iDeinit && iDeinit < iDel,
     'pnet_stop: order must be s_rxq=NULL -> esp_now_deinit -> vQueueDelete (no callback on a freed queue)');
}

// --- 13) Snake: the guest explosion y uses by[0], not bx[0]
{
  const s = read('nucleo_app/app_snake.cpp');
  ok(!/sy_\(dead\.bx\[0\]\)/.test(s), 'app_snake: sy_(dead.bx[0]) -> sy_(dead.by[0])');
}

if (fails.length) {
  console.error(`[fw-hardening] ${fails.length} FAILED${rev ? ` (rev ${rev})` : ''}:`);
  for (const f of fails) console.error('  - ' + f);
  process.exit(1);
}
console.log(`[fw-hardening] ALL GREEN${rev ? ` (rev ${rev})` : ''} — tanks/tankduel peer filter, Vicino cmd+name guards, swarm/pnet lifecycle, snake y`);
