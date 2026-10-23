// node --test tools/discord-bot/search.test.mjs — the bot must pick the right answer, stay silent on
// off-topic text, and every answer must fit one Discord message with a source that exists.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { search, byGuide, GUIDES } from './search.mjs';

const KB = JSON.parse(readFileSync(new URL('./kb.json', import.meta.url), 'utf8'));
const ROOT = fileURLToPath(new URL('../../', import.meta.url));

const CASES = [
    ['how do I install nucleoos?', 'install'],
    ['come si installa', 'install'],
    ['flasher does not find the serial port', 'flashfail'],
    ['my sd card is not recognized', 'sdproblem'],
    ['what goes on the sd card', 'sd'],
    ['how to open the web desktop', 'webos'],
    ['where do I find the pin to pair?', 'pin'],
    ['how do I use anima', 'anima'],
    ['can it use ollama on my pc', 'models'],
    ['private mode', 'private'],
    ['how to update the firmware', 'update'],
    ['does it run on the cardputer adv', 'adv'],
    ['m5launcher back to launcher', 'launcher'],
    ['change language to italian', 'languages'],
    ['game boy emulator roms', 'games'],
    ['phone cannot join the hotspot', 'ap'],
    ['commercial license', 'license'],
    ['which version do I have', 'version'],
];

for (const [q, id] of CASES) {
    test(`"${q}" -> ${id}`, () => {
        const hits = search(KB.entries, q);
        assert.ok(hits.length, 'no answer');
        assert.equal(hits[0].id, id);
    });
}

for (const q of ['what is the capital of france', 'tell me a joke', 'pizza recipe', '???', 'ciao']) {
    test(`off-topic stays silent: "${q}"`, () => assert.deepEqual(search(KB.entries, q), []));
}

test('every entry fits a message, has a guide and a real source', () => {
    for (const e of KB.entries) {
        assert.ok(e.answer.length + e.title.length < 1500, `${e.id} too long`);
        assert.ok(GUIDES[e.guide], `${e.id} guide ${e.guide}`);
        assert.ok(existsSync(ROOT + e.src.split('#')[0]), `${e.id} src ${e.src}`);
    }
    for (const g of Object.keys(GUIDES)) assert.ok(byGuide(KB.entries, g).length, `empty guide ${g}`);
});
