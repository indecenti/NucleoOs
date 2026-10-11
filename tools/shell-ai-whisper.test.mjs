// Speech -> text through the shared AI layer (web/shell/ai.js): the ONE Whisper endpoint builder and the vault's
// Whisper key. Dictation and the agent's transcribe tool rewrote Groq's base to https://api.groq.com/v1/…, which
// answers 404 (only /openai/v1 serves Whisper — checked against the live API 2026-10-11); the Recorder read the
// key only from this browser's localStorage, never from the vault Settings writes.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { whisperUrl, whisperKey } from '../web/shell/ai.js';

const GROQ = 'https://api.groq.com/openai/v1/audio/transcriptions';

test('whisperUrl: Groq always lands on /openai/v1, other OpenAI-compatible bases are kept', () => {
  for (const b of ['', null, undefined, 'https://api.groq.com/openai/v1', 'https://api.groq.com/openai/v1/', 'https://api.groq.com/v1', 'https://api.groq.com', 'not a url'])
    assert.equal(whisperUrl(b), GROQ, String(b));
  assert.equal(whisperUrl('https://api.openai.com/v1'), 'https://api.openai.com/v1/audio/transcriptions');
  assert.equal(whisperUrl('http://localhost:8000/v1/'), 'http://localhost:8000/v1/audio/transcriptions');
});

test('no caller builds a Whisper URL by hand any more', () => {
  for (const f of ['apps/dictation/www/asr.js', 'apps/agent/www/runtime.js']) {
    const src = readFileSync(new URL('../' + f, import.meta.url), 'utf8');
    assert.ok(!/api\.groq\.com\/v1/.test(src), f + ' still names the dead route');
    assert.ok(!/replace\(\/\\\/openai\\\/v1\$\//.test(src), f + ' still rewrites /openai/v1 away');
    assert.ok(/whisperUrl\(/.test(src), f + ' uses whisperUrl');
  }
});

function vault(doc) {
  globalThis.fetch = async (url) => {
    if (!String(url).includes('teacher.json')) return { ok: false, status: 404, text: async () => '' };
    return doc == null ? { ok: false, status: 404, text: async () => '' } : { ok: true, status: 200, text: async () => JSON.stringify(doc) };
  };
}

test('whisperKey: the Groq key from the vault, even when the active chat provider is Claude', async () => {
  vault({ provider: 'anthropic', key: 'sk-ant-x', keys: { anthropic: { key: 'sk-ant-x' }, openai: { key: 'gsk_live', base: 'https://api.groq.com/openai/v1' } } });
  assert.deepEqual(await whisperKey({ fresh: true }), { key: 'gsk_live', base: 'https://api.groq.com/openai/v1' });
});

test('whisperKey: a Groq-only vault (provider-level key), the legacy keys.groq slot, and an honest null', async () => {
  vault({ provider: 'openai', key: 'gsk_top', base: 'https://api.groq.com/openai/v1' });
  assert.equal((await whisperKey({ fresh: true })).key, 'gsk_top');
  vault({ provider: 'anthropic', key: 'sk-ant-x', keys: { groq: { key: 'gsk_old' } } });
  assert.deepEqual(await whisperKey({ fresh: true }), { key: 'gsk_old', base: 'https://api.groq.com/openai/v1' });
  vault({ provider: 'anthropic', key: 'sk-ant-x', keys: { anthropic: { key: 'sk-ant-x' } } });
  assert.equal(await whisperKey({ fresh: true }), null, 'Claude alone cannot transcribe');
  vault(null);
  assert.equal(await whisperKey({ fresh: true }), null, 'no vault');
});
