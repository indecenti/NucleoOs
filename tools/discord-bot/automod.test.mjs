// node --test tools/discord-bot/automod.test.mjs — the "Secrets and personal data" regexes must catch real
// secrets and must not fire on everyday NucleoOS talk. JS RegExp approximates Discord's Rust engine, matched
// case-insensitively (the stricter assumption for false positives).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const src = readFileSync(new URL('./automod.mjs', import.meta.url), 'utf8');
const block = src.slice(src.indexOf('regex_patterns: ['), src.indexOf('], keyword_filter: [], allow_list'));
const patterns = [...block.matchAll(/^\s*'(.+?)',/gm)].map(m => {
    const p = m[1].replace(/\\\\/g, '\\').replace(/^\(\?i\)/, '');
    return new RegExp(p, 'i');
});
const hits = t => patterns.filter(r => r.test(t)).length;

test('found all secret patterns', () => assert.equal(patterns.length, 8));

for (const t of [
    'token: MTAwMDAwMDAwMDAwMDAwMDAwMA.Gabcde.abcdefghijklmnopqrstuvwxyz0123',
    'use gsk_abcdefghijklmnopqrstuvwxyz', 'sk-ant-api03-abcdefghijklmnopqrstuv', 'AIzaSyA1234567890abcdefghijklmnopqrstuv',
    'ghp_abcdefghijklmnopqrstuvwxyz0123', '-----BEGIN RSA PRIVATE KEY-----', 'wifi password: hunter2222',
    'IT60 X054 2811 1010 0000 0123 456', 'card 4111 1111 1111 1111',
]) test(`blocks: ${t.slice(0, 30)}`, () => assert.ok(hits(t) > 0));

for (const t of [
    'I flashed the ESP32-S3 with esptool v4 today', 'the PIN is 123456', 'Settings > Device > Web sessions',
    'mp34 with this that okay fine good', 'firmware 0.7.3+0.gb6b64da works', 'the password is on the screen',
    'open http://192.168.4.1/ and pair', 'nucleoos-0.7.3.0-sd.zip is 52 MB', 'COM3 at 115200 baud, 921600 to flash',
]) test(`allows: ${t}`, () => assert.equal(hits(t), 0));
