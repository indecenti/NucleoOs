// node --test tools/discord-bot/bot.test.mjs — starts the real bot.mjs with a throwaway Ed25519 key and
// checks the HTTP contract Discord relies on: signed PING, rejected forgeries, answers, scope, cooldown.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { generateKeyPairSync, sign } from 'node:crypto';

const GUILD = '1563226728762056854', PORT = 18009;
const { publicKey, privateKey } = generateKeyPairSync('ed25519');
const rawPub = publicKey.export({ format: 'der', type: 'spki' }).subarray(12).toString('hex');
let proc;

before(async () => {
    proc = spawn(process.execPath, ['--max-old-space-size=24', new URL('./bot.mjs', import.meta.url).pathname.replace(/^\/(\w:)/, '$1')],
        { env: { ...process.env, DISCORD_PUBLIC_KEY: rawPub, DISCORD_GUILD_ID: GUILD, PORT: String(PORT) }, stdio: ['ignore', 'pipe', 'inherit'] });
    await new Promise(r => proc.stdout.once('data', r));
});
after(() => proc.kill());

async function post(body, { forge = false, ts = Math.floor(Date.now() / 1000) } = {}) {
    const raw = JSON.stringify(body);
    const sig = sign(null, Buffer.from(String(ts) + raw), privateKey).toString('hex');
    const r = await fetch(`http://127.0.0.1:${PORT}/`, { method: 'POST', body: raw,
        headers: { 'x-signature-ed25519': forge ? sig.replace(/^./, c => c === 'a' ? 'b' : 'a') : sig, 'x-signature-timestamp': String(ts) } });
    return { status: r.status, json: r.status === 200 ? await r.json() : null };
}
const cmd = (name, value, extra = {}) => ({ type: 2, guild_id: GUILD, member: { user: { id: extra.user || 'u1' } },
    data: { name, options: [{ name: 'q', value }] }, ...extra });

test('signed PING gets PONG', async () => assert.deepEqual((await post({ type: 1 })).json, { type: 1 }));
test('forged signature is rejected', async () => assert.equal((await post({ type: 1 }, { forge: true })).status, 401));
test('stale timestamp is rejected', async () => assert.equal((await post({ type: 1 }, { ts: 1000 })).status, 401));
test('/ask answers from the KB, never pings', async () => {
    const { json } = await post(cmd('ask', 'how do I install it?', { user: 'a' }));
    assert.match(json.data.content, /web flasher/);
    assert.deepEqual(json.data.allowed_mentions, { parse: [] });
});
test('user text is never echoed', async () => {
    const { json } = await post(cmd('ask', '@everyone buy cheap stuff http://evil.example', { user: 'b' }));
    assert.doesNotMatch(json.data.content, /everyone|evil/);
});
test('cooldown per user', async () => {
    await post(cmd('ask', 'install', { user: 'c' }));
    const { json } = await post(cmd('ask', 'install', { user: 'c' }));
    assert.match(json.data.content, /wait/); assert.equal(json.data.flags & 64, 64);
});
test('other servers are refused', async () => {
    const { json } = await post(cmd('ask', 'install', { user: 'd', guild_id: '42' }));
    assert.match(json.data.content, /only work on/);
});
test('DMs with the bot work', async () => {
    const { json } = await post({ type: 2, context: 1, user: { id: 'e' }, data: { name: 'guide', options: [{ value: 'anima' }] } });
    assert.match(json.data.content, /ANIMA/);
});
