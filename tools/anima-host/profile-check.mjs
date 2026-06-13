#!/usr/bin/env node
// PROFILE-CHECK — the typed personal-profile tier (nucleo_anima_profile.c) end-to-end on the REAL cascade.
// 100+ bilingual IT+EN cases: SET self-facts in natural language, RECALL them across "sessions", the
// "what do you know about me" summary, honest "I don't know yet" on unset fields, and — the hard property —
// ZERO hijack: third-person / non-profile / teach / reminder inputs must NOT route to profile.
//
// Stateful (one exe process; SD profile.tsv persists between turns, /reset only clears the chat session).
// Wipes profile.tsv + the learn store before AND after so nothing leaks into the other gates.
//
//   node tools/anima-host/profile-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const sd = join(here, 'sd', 'data', 'anima');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const wipe = () => { for (const f of ['profile.tsv', 'profile.tsv.tmp', 'user.tsv', 'user.vec', 'user.tsv.tmp', 'user.vec.tmp']) { try { rmSync(join(sd, f)); } catch { /* ok */ } } };

const steps = [];
const S = (q, lang, want) => steps.push({ q, lang, want });          // want: {intent, has, unset, notProfile}
const set = (q, lang, has) => S(q, lang, { intent: 'profile', has });
const recall = (q, lang, has) => S(q, lang, { intent: 'profile', has });
const unset = (q, lang) => S(q, lang, { intent: 'profile', unset: true });
const noProf = (q, lang) => S(q, lang, { notProfile: true });

// --- 1) UNSET first (clean store): honest "I don't know yet", never fabricated ---
unset('come mi chiamo', 'it'); unset('quanti anni ho', 'it'); unset('dove abito', 'it');
unset('what is my name', 'en'); unset('how old am i', 'en'); unset("what's my email", 'en');

// --- 2) NAME: many SET phrasings (each sets a distinct name), then a sentinel + many RECALL phrasings ---
set('mi chiamo Marco', 'it', 'Marco');
set('mi    chiamo   Anna', 'it', 'Anna');               // whitespace robustness: runs of spaces collapse
set('il mio nome è Giulia', 'it', 'Giulia');
set('chiamami Capo', 'it', 'Capo');
set('puoi chiamarmi Dottore', 'it', 'Dottore');
set('my name is Alex', 'en', 'Alex');
set('call me Chief', 'en', 'Chief');
set('you can call me Sam', 'en', 'Sam');
set("i'm called Robin", 'en', 'Robin');
set('mi chiamo Capitano', 'it', 'Capitano');            // sentinel
recall('come mi chiamo', 'it', 'Capitano');
recall('qual è il mio nome', 'it', 'Capitano');
recall('quale è il mio nome', 'it', 'Capitano');
recall('ti ricordi il mio nome', 'it', 'Capitano');
recall('what is my name', 'en', 'Capitano');
recall("what's my name", 'en', 'Capitano');
recall('do you know my name', 'en', 'Capitano');
recall('do you remember my name', 'en', 'Capitano');
recall('what am i called', 'en', 'Capitano');

// --- 3) AGE ---
set('ho 34 anni', 'it', '34');
set('ho 28 anni', 'it', '28');
set('i am 41 years old', 'en', '41');
set("i'm 19 years old", 'en', '19');
set('my age is 50', 'en', '50');
set('ho 37 anni', 'it', '37');                          // sentinel
recall('quanti anni ho', 'it', '37');
recall('che età ho', 'it', '37');
recall('how old am i', 'en', '37');
recall('what is my age', 'en', '37');
recall("what's my age", 'en', '37');

// --- 4) CITY ---
set('abito a Brescia', 'it', 'Brescia');
set('vivo a Roma', 'it', 'Roma');
set('abito in Sicilia', 'it', 'Sicilia');
set('i live in London', 'en', 'London');
set('i live at Berlin', 'en', 'Berlin');
set('abito a Bologna', 'it', 'Bologna');                // sentinel
recall('dove abito', 'it', 'Bologna');
recall('dove vivo', 'it', 'Bologna');
recall('in che città vivo', 'it', 'Bologna');
recall('where do i live', 'en', 'Bologna');
recall('what city do i live in', 'en', 'Bologna');

// --- 5) JOB ---
set('di lavoro faccio il medico', 'it', 'medico');
set('il mio lavoro è insegnante', 'it', 'insegnante');
set('lavoro come designer', 'it', 'designer');
set('my job is developer', 'en', 'developer');
set('i work as a nurse', 'en', 'nurse');
set('i work as an architect', 'en', 'architect');
set('il mio lavoro è pizzaiolo', 'it', 'pizzaiolo');     // sentinel
recall('che lavoro faccio', 'it', 'pizzaiolo');
recall('qual è il mio lavoro', 'it', 'pizzaiolo');
recall('what is my job', 'en', 'pizzaiolo');
recall('what do i do for work', 'en', 'pizzaiolo');

// --- 6) EMAIL ---
set('la mia email è mario@test.it', 'it', 'mario@test.it');
set('la mia mail è anna@x.com', 'it', 'anna@x.com');
set('my email is bob@mail.com', 'en', 'bob@mail.com');
set('my email address is sue@web.org', 'en', 'sue@web.org');
set('la mia email è me@host.it', 'it', 'me@host.it');   // sentinel
recall('qual è la mia email', 'it', 'me@host.it');
recall('qual è la mia mail', 'it', 'me@host.it');
recall('what is my email', 'en', 'me@host.it');
recall("what's my email", 'en', 'me@host.it');

// --- 7) BIRTHDAY ---
set('il mio compleanno è il 5 maggio', 'it', '5 maggio');
set('sono nato il 12 marzo', 'it', '12 marzo');
set('my birthday is june 3', 'en', 'june 3');
set('i was born on may 1', 'en', 'may 1');
set('il mio compleanno è il 9 aprile', 'it', '9 aprile'); // sentinel
recall('quando è il mio compleanno', 'it', '9 aprile');
recall('quando sono nato', 'it', '9 aprile');
recall('when is my birthday', 'en', '9 aprile');
recall("when's my birthday", 'en', '9 aprile');

// --- 8) SUMMARY (many fields now set) ---
S('cosa sai di me', 'it', { intent: 'profile', has: 'Capitano' });
S('che cosa sai di me', 'it', { intent: 'profile', has: 'Bologna' });
S('what do you know about me', 'en', { intent: 'profile', has: 'pizzaiolo' });
S('my profile', 'en', { intent: 'profile', has: 'Capitano' });

// --- 9) THIRD-PERSON / NON-PROFILE: must NOT be profile (no hijack, no misattribution) ---
noProf('come si chiama il presidente', 'it');
noProf('dove abita marco', 'it');
noProf('quanti anni ha luca', 'it');
noProf('che lavoro fa giulia', 'it');
noProf('dove si trova la torre di pisa', 'it');
noProf('what is the capital of France', 'en');
noProf('where is the nearest station', 'en');
noProf('how old is the universe', 'en');
noProf("what's the weather today", 'en');
noProf('quando è nato dante', 'it');
noProf('come si chiama tua sorella', 'it');
noProf('what does my colleague do', 'en');
noProf('how old is my brother', 'en');
noProf('in un foglio di calcolo come faccio la somma di A1:A10', 'it');  // "faccio la X" is NOT a job
noProf('come faccio la somma in excel', 'it');
noProf('faccio la doccia ogni mattina', 'it');
noProf('how do i do a sum', 'en');
noProf('chiamami quando arrivi', 'it');                 // call me WHEN — not a name
noProf('chiamami appena puoi', 'it');
noProf('richiamami più tardi', 'it');

// --- 10) SAFETY vs teach / reminder / settings (other tools must still win) ---
S('ricorda che la sala riunioni è al terzo piano', 'it', { intent: 'teach' });
S('remember that the key is under the mat', 'en', { intent: 'teach' });
S('ricordami di comprare il latte', 'it', { notProfile: true });
S('alza il volume', 'it', { notProfile: true });
S('che ore sono', 'it', { notProfile: true });

// --- 11) CROSS-FEATURE: profile + learn coexist without interference ---
set('mi chiamo Ada', 'it', 'Ada');
S('ricorda che la palestra è in via Verdi', 'it', { intent: 'teach' });
S('dove si trova la palestra', 'it', { has: 'via Verdi' });   // learn recall (intent=recall), not profile
recall('come mi chiamo', 'it', 'Ada');                        // profile still intact

// --- run it all in one process (state persists across /reset) ---
wipe();
let lang = 'it';
const lines = [];
for (const s of steps) { lines.push('/reset'); const w = s.lang === 'en' ? 'en' : 'it'; if (w !== lang) { lines.push('/' + w); lang = w; } lines.push(s.q); }
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
const parse = (b) => ({
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
});
const UNSET = /dimmel|non so ancora|non conosco ancora|don't know|tell me/i;

const show = process.argv.includes('--show');
const fails = [];
for (let i = 0; i < steps.length; i++) {
  const s = steps[i], p = parse(blocks[i] || ''), w = s.want; let ok = true, why = '';
  if (w.notProfile && p.intent === 'profile') { ok = false; why = `routed to profile ("${p.reply.slice(0, 30)}")`; }
  if (w.intent && p.intent !== w.intent) { ok = false; why = `intent ${p.intent || '(none)'} != ${w.intent}`; }
  if (w.unset && !UNSET.test(p.reply)) { ok = false; why = `expected "don't know yet", got "${p.reply.slice(0, 40)}"`; }
  if (w.has && !p.reply.toLowerCase().includes(w.has.toLowerCase())) { ok = false; why = `reply "${p.reply.slice(0, 44)}" missing "${w.has}"`; }
  if (!ok) fails.push([s.q, s.lang, why]);
  if (show) console.log(`${ok ? '  ok  ' : 'FAIL  '} [${s.lang}] "${s.q}" -> ${p.intent || '(none)'} ${p.reply ? `"${p.reply.slice(0, 40)}"` : ''}`);
}
wipe();

const nIt = steps.filter(s => s.lang === 'it').length, nEn = steps.length - nIt;
const pass = steps.length - fails.length;
console.log(`[profile-check] personal profile — ${pass}/${steps.length} pass (${(100 * pass / steps.length).toFixed(1)}%)  [IT ${nIt} / EN ${nEn}]`);
if (fails.length) { console.log('FAILURES:'); for (const f of fails) console.log(`  ✗ [${f[1]}] "${f[0]}" — ${f[2]}`); process.exit(1); }
console.log('✓ set/recall/summary, honest-unset, zero third-person hijack, teach/reminder safety, cross-feature — all hold');
