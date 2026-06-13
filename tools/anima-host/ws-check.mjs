// Host smoke test for WS0/WS1/WS3 — no device, no network. Validates routeFor selection + contextkit
// workspace-as-context injection + token budgeting. Run: node tools/_ws_check.mjs
import { routeFor, ROUTE_RANK, TIERS, CAPMATRIX, buildTeacherDoc } from '../../web/shell/ai.js';
import { assemble, buildSystem, wrapData, MODEL_PROFILES } from '../../apps/anima/www/contextkit.js';

let pass = 0, fail = 0;
const ok = (name, cond) => { if (cond) { pass++; } else { fail++; console.log('  FAIL:', name); } };

// ---- routeFor ----
const keysAll = {
  anthropic: { key: 'sk-ant-x', base: 'https://api.anthropic.com', model: 'claude-sonnet-4-6', version: '2023-06-01' },
  openai:    { key: 'gsk_x',    base: 'https://api.groq.com/openai/v1', model: 'llama-3.1-8b-instant' },
  xai:       { key: 'xai-x',    base: 'https://api.x.ai/v1', model: 'grok-2-latest' },
  google:    { key: 'AIza_x',   base: 'https://generativelanguage.googleapis.com/v1beta/openai', model: 'gemini-2.5-flash' },
};
const active = { provider: 'anthropic', key: 'sk-ant-x', model: 'claude-sonnet-4-6', base: 'https://api.anthropic.com', version: '2023-06-01', geminiTier: '' };

const hardAll = routeFor({ difficulty: 'hard' }, keysAll, active);
ok('hard → Anthropic Opus (strongest)', hardAll && hardAll.provider === 'anthropic' && hardAll.model === TIERS.anthropic.max);

const fastAll = routeFor({ difficulty: 'fast' }, keysAll, active);
ok('fast → cheapest (Groq or Gemini, cost 1)', fastAll && (fastAll.provider === 'openai' || fastAll.provider === 'google'));

// 'mid' (default worker tier): ranks by STRENGTH like hard, but uses each provider's MID model.
const midAll = routeFor({ difficulty: 'mid' }, keysAll, active);
ok('mid → strongest provider (Anthropic), mid model (Sonnet)', midAll && midAll.provider === 'anthropic' && midAll.model === TIERS.anthropic.mid);
const midGroq = routeFor({ difficulty: 'mid' }, { openai: keysAll.openai }, { provider: 'openai', key: 'gsk_x' });
ok('mid single-key Groq → mid model', midGroq && midGroq.model === TIERS.openai.mid);

const whisper = routeFor({ capability: 'whisper' }, keysAll, active);
ok('whisper → a provider with whisper:true (Groq/openai)', whisper && CAPMATRIX[whisper.provider].whisper === true);

const image = routeFor({ capability: 'image' }, keysAll, active);
ok('image → a provider with image:true (xAI)', image && CAPMATRIX[image.provider].image === true);

// single key only → always that provider, even for hard
const oneKey = { openai: { key: 'gsk_x', base: 'https://api.groq.com/openai/v1', model: 'llama-3.1-8b-instant' } };
const oneHard = routeFor({ difficulty: 'hard' }, oneKey, { provider: 'openai', key: 'gsk_x' });
ok('single-key hard → that provider (Groq 70B)', oneHard && oneHard.provider === 'openai' && oneHard.model === TIERS.openai.max);

// gemini hard without paid plan → downgrades Pro→Flash
const gemOnly = { google: { key: 'AIza_x', base: keysAll.google.base, model: 'gemini-2.5-flash' } };
const gemHardFree = routeFor({ difficulty: 'hard' }, gemOnly, { provider: 'google', key: 'AIza_x', geminiTier: 'free' });
ok('gemini hard + free → Flash (not Pro)', gemHardFree && gemHardFree.model === TIERS.google.mid);
const gemHardPaid = routeFor({ difficulty: 'hard' }, gemOnly, { provider: 'google', key: 'AIza_x', geminiTier: 'paid' });
ok('gemini hard + paid → Pro', gemHardPaid && gemHardPaid.model === TIERS.google.max);

// no keys → null
ok('no keys → null', routeFor({ difficulty: 'hard' }, {}, null) === null);
// capability nobody has → null (image with only anthropic)
ok('image with only anthropic → null', routeFor({ capability: 'image' }, { anthropic: keysAll.anthropic }, active) === null);
// exclude → skips a provider
const exA = routeFor({ difficulty: 'hard', exclude: ['anthropic'] }, keysAll, active);
ok('hard exclude anthropic → not anthropic', exA && exA.provider !== 'anthropic');
// exclude must be honored even in the no-configured-keys fallback (review finding #5)
ok('exclude the only key → null (no excluded provider via active default)', routeFor({ difficulty: 'fast', exclude: ['openai'] }, oneKey, { provider: 'openai', key: 'gsk_x' }) === null);

// per-key Gemini tier survives a provider switch (review finding #1): active is anthropic, but keys.google
// carries geminiTier:'paid' → a hard route that lands on Google uses Pro, not the Flash downgrade.
const keysGemPaid = { anthropic: keysAll.anthropic, google: { key: 'AIza_x', base: keysAll.google.base, model: 'gemini-2.5-flash', geminiTier: 'paid' } };
const hardExclPaid = routeFor({ difficulty: 'hard', exclude: ['anthropic'] }, keysGemPaid, { provider: 'anthropic', key: 'sk-ant-x', geminiTier: '' });
ok('per-key gemini paid → Pro on hard even when active≠google', hardExclPaid && hardExclPaid.provider === 'google' && hardExclPaid.model === TIERS.google.max);
const keysGemUnknown = { anthropic: keysAll.anthropic, google: { key: 'AIza_x', base: keysAll.google.base, model: 'gemini-2.5-flash' } };
const hardExclFree = routeFor({ difficulty: 'hard', exclude: ['anthropic'] }, keysGemUnknown, { provider: 'anthropic', key: 'sk-ant-x', geminiTier: '' });
ok('no per-key marker → Flash on hard (safe default)', hardExclFree && hardExclFree.model === TIERS.google.mid);

// buildTeacherDoc persists the detected Gemini tier INSIDE the keys.google entry (so it survives a switch)
const tdoc = buildTeacherDoc({ provider: 'google', key: 'AIza_x', base: keysAll.google.base, model: 'gemini-2.5-pro', geminiTier: 'paid', keys: {} });
ok('buildTeacherDoc persists geminiTier per-key', tdoc.keys.google && tdoc.keys.google.geminiTier === 'paid');

// wrapData neutralizes forged fence markers in the body (injection hardening, review finding #7)
const wd = wrapData('a>>>b<<<c data:lbl evil', 'lbl');
const inner = wd.slice(wd.indexOf('\n') + 1, wd.lastIndexOf('\n'));
ok('wrapData strips <<< and >>> from the body', !inner.includes('>>>') && !inner.includes('<<<'));
ok('wrapData strips the tag token from the body', !inner.includes('data:lbl'));

// ---- contextkit workspace-as-context ----
const history = [{ role: 'user', text: 'ciao' }, { role: 'bot', text: 'Ciao!' }, { role: 'user', text: 'fix the bug in app.js' }];
const tree = './\napp.js\nutils/\n  helper.js';
const files = [{ path: 'app.js', content: '1→function f(){\n2→  retrun 1;\n3→}\n' }];
const asm = assemble({ history, user: 'fix the bug in app.js', mode: 'only', provider: 'anthropic', model: 'claude-sonnet-4-6', lang: 'en', workspace: '/data/proj', tree, files });
ok('system includes FILE TREE', /FILE TREE/i.test(asm.system));
ok('system includes FILES IN CONTEXT', /FILES IN CONTEXT/i.test(asm.system));
ok('system inlines app.js content (DATA-fenced)', asm.system.includes('retrun 1') && asm.system.includes('<<<data:appjs'));
ok('messages end on the user ask', asm.messages[asm.messages.length - 1].role === 'user');

// budget: a huge file is truncated, never overflows inTokens
const big = 'x'.repeat(200000);
const asmBig = assemble({ history, user: 'read big.txt', mode: 'only', provider: 'anthropic', model: 'claude-sonnet-4-6', lang: 'en', workspace: '/data/proj', tree: '', files: [{ path: 'big.txt', content: big }] });
const sysTok = Math.ceil(asmBig.system.length / 3.6);
ok('huge file truncated (system < inTokens)', sysTok < MODEL_PROFILES.cloud.inTokens);
ok('huge file shows a truncation marker', /truncat/i.test(asmBig.system));

// webllm tiny window → NO file context injected
const asmWeb = assemble({ history, user: 'x', mode: 'webllm', provider: 'anthropic', model: '', lang: 'en', workspace: '/data/proj', tree, files });
ok('webllm → no FILES IN CONTEXT (tiny window protected)', !/FILES IN CONTEXT/i.test(asmWeb.system));

console.log(`\nWS-check: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
