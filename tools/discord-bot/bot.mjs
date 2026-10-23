#!/usr/bin/env node
// NucleoOS community Discord bot — answers /ask and /guide from kb.json. Zero cost when idle:
//  * Discord POSTs each slash command to an HTTPS endpoint (nginx -> 127.0.0.1). systemd socket activation
//    starts this process on the first request; it exits after IDLE_MS without requests. Idle = 0 RAM, 0 CPU.
//  * zero npm dependencies (Node >= 22), no gateway, reads no messages, holds NO bot token: it only needs
//    the application's PUBLIC key to verify Discord's Ed25519 signature on every request.
//  * replies are built only from kb.json — user text is never echoed, mentions are never pinged;
//  * answers only in our guild and in DMs with the bot; per-user cooldown + a global cap.
// Env: DISCORD_PUBLIC_KEY, DISCORD_GUILD_ID. Commands are registered from a PC by register.mjs.
import { createServer } from 'node:http';
import { createPublicKey, verify } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { search, byGuide } from './search.mjs';

const GUILD = process.env.DISCORD_GUILD_ID;
const PUBKEY_HEX = process.env.DISCORD_PUBLIC_KEY;
if (!GUILD || !/^[0-9a-f]{64}$/i.test(PUBKEY_HEX || '')) { console.error('DISCORD_GUILD_ID and DISCORD_PUBLIC_KEY are required'); process.exit(1); }
const PUBKEY = createPublicKey({ key: Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(PUBKEY_HEX, 'hex')]), format: 'der', type: 'spki' });

const KB = JSON.parse(readFileSync(new URL('./kb.json', import.meta.url), 'utf8'));
const IDLE_MS = Number(process.env.IDLE_MS || 300_000);
const USER_COOLDOWN_MS = 15_000, GLOBAL_PER_MIN = 30, MAX_LEN = 1900, MAX_BODY = 16_384;
const log = (...a) => console.log(...a);

const clip = s => s.length > MAX_LEN ? s.slice(0, MAX_LEN - 1) + '…' : s;
const fmt = e => `**${e.title}**\n${e.answer}\n-# Source: <${KB.repo}/blob/main/${e.src}>`;

function answerAsk(q) {
    const hits = search(KB.entries, q);
    if (!hits.length) return "I couldn't find that in the NucleoOS docs. Open a post in the **help** forum with the template pinned there, and someone from the Team will answer. You can also try `/guide`.";
    const [best, ...rest] = hits;
    let out = fmt(best);
    if (rest.length) out += `\n\n-# Related: ${rest.slice(0, 2).map(e => e.title).join(' · ')} (try \`/ask\` with those words)`;
    return out;
}
// A guide can exceed one message: split at entry boundaries; the first chunk is the reply, the rest follow-ups.
function answerGuide(topic) {
    const chunks = [];
    for (const part of byGuide(KB.entries, topic).map(fmt)) {
        if (chunks.length && chunks[chunks.length - 1].length + part.length + 2 <= MAX_LEN) chunks[chunks.length - 1] += '\n\n' + part;
        else chunks.push(part);
    }
    return chunks.length ? chunks.slice(0, 4) : ['Unknown topic.'];
}

const lastByUser = new Map();
let windowStart = Date.now(), windowCount = 0;
function limited(userId) {
    const now = Date.now();
    if (now - windowStart > 60_000) { windowStart = now; windowCount = 0; }
    if (windowCount >= GLOBAL_PER_MIN) return 'The bot is busy, try again in a minute.';
    const last = lastByUser.get(userId) || 0;
    if (now - last < USER_COOLDOWN_MS) return `Please wait ${Math.ceil((USER_COOLDOWN_MS - (now - last)) / 1000)} s before asking again.`;
    lastByUser.set(userId, now); windowCount++;
    if (lastByUser.size > 5000) lastByUser.clear();
    return null;
}

const msg = (content, ephemeral) => ({ content: clip(content), flags: (ephemeral ? 64 : 0) | 4, allowed_mentions: { parse: [] } });

// Returns [responseBody, afterResponse?].
export function handle(i) {
    if (i.type === 1) return [{ type: 1 }];                                              // PING
    if (i.type !== 2) return [{ type: 4, data: msg('Unsupported.', true) }];
    const inGuild = i.guild_id === GUILD, inDm = !i.guild_id && i.context === 1;
    if (!inGuild && !inDm) return [{ type: 4, data: msg('I only work on the NucleoOS Cardputer server.', true) }];
    const wait = limited(i.member?.user?.id ?? i.user?.id);
    if (wait) return [{ type: 4, data: msg(wait, true) }];
    const opt = String(i.data.options?.[0]?.value ?? '').slice(0, 200);
    if (i.data.name === 'ask') return [{ type: 4, data: msg(answerAsk(opt), false) }];
    if (i.data.name === 'guide') {
        const [first, ...more] = answerGuide(opt);
        return [{ type: 4, data: msg(first, false) }, async () => {
            for (const c of more) await fetch(`https://discord.com/api/v10/webhooks/${i.application_id}/${i.token}`,
                { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(msg(c, false)) });
        }];
    }
    return [{ type: 4, data: msg('Unknown command.', true) }];
}

let lastRequest = Date.now();
const server = createServer((req, res) => {
    lastRequest = Date.now();
    if (req.method !== 'POST') { res.writeHead(405).end(); return; }
    const chunks = []; let size = 0;
    req.on('data', c => { size += c.length; if (size > MAX_BODY) req.destroy(); else chunks.push(c); });
    req.on('end', async () => {
        const body = Buffer.concat(chunks);
        const sig = req.headers['x-signature-ed25519'], ts = req.headers['x-signature-timestamp'];
        let ok = false;
        try {
            ok = typeof sig === 'string' && typeof ts === 'string' && Math.abs(Date.now() / 1000 - Number(ts)) < 300
                && verify(null, Buffer.concat([Buffer.from(ts), body]), PUBKEY, Buffer.from(sig, 'hex'));
        } catch { ok = false; }
        if (!ok) { res.writeHead(401).end('invalid request signature'); return; }
        let out, after;
        try { [out, after] = handle(JSON.parse(body)); }
        catch (e) { log('handle:', e.message); res.writeHead(400).end(); return; }
        res.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(out));
        if (after) after().catch(e => log('follow-up:', e.message));
    });
});

// systemd socket activation passes the listening socket as fd 3; without it (local test) listen on a port.
if (process.env.LISTEN_FDS === '1') server.listen({ fd: 3 });
else server.listen(Number(process.env.PORT || 8009), '127.0.0.1', () => log('listening on', server.address().port));

setInterval(() => {
    if (Date.now() - lastRequest > IDLE_MS) server.close(() => process.exit(0));    // back to zero; systemd wakes us
}, 30_000).unref();
process.on('SIGTERM', () => server.close(() => process.exit(0)));
