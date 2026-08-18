// truthlamp-yield.mjs — MEASUREMENT SPIKE for the "truth lamp" idea (per-claim badges on
// generative replies, judged by the device's deterministic brain via nucleo_anima_verify_claim).
//
// The idea only earns UI if the verifier actually reaches decisive verdicts on the material the
// copilot really produces: if ~90% of extracted claims come back UNKNOWN, badges are a wall of
// grey that trains the user to ignore the one red that matters. So: measure first, build second.
// This script runs forge/extract.js (the real browser-side extractor) over a corpus of realistic
// generative-style replies, routes every checkable claim to the REAL firmware verifier (host
// build), and prints the yield. Decision rule (from the design review): build the badges only if
// decisive (confirmed+contradicted) claims exceed ~20% of extracted claims.
//
//   node tools/anima-host/truthlamp-yield.mjs        (builds anima.exe first if needed)

import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { existsSync } from 'node:fs';
import { extractProseClaims, routeClaim } from '../../apps/anima/www/forge/extract.js';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) {
  const b = spawnSync('node', [join(here, 'anima.mjs'), '--ensure'], { encoding: 'utf8', stdio: 'inherit' });
  if (b.status !== 0) { console.error('host build failed'); process.exit(1); }
}

// The prose-claim → verifier-spec router lives in forge/extract.js (routeClaim) so the copilot
// badges and this harness measure the SAME code path.

function verify(spec, lang) {
  const en = spec.lang ? spec.lang === 'en' : lang === 'en';   // the routed pattern knows its language
  const args = (en ? ['--en'] : []).concat(['--verify', `${spec.kind}|${spec.key}|${spec.asserted}`]);
  const r = spawnSync(exe, args, { encoding: 'utf8' });
  const m = /VERDICT=(\w+)/.exec(r.stdout || '');
  return m ? m[1] : 'error';
}

// ── corpus: 50 realistic generative-style replies (the material the copilot/agent shows) ──────
const CORPUS = [
  // math / calculator asks (the cloud loves restating the computation)
  { lang: 'it', text: 'Il risultato di 2+2 è 4.' },
  { lang: 'it', text: '10 * 10 fa 100.' },
  { lang: 'it', text: 'La somma richiesta, 15+27, è uguale a 42.' },
  { lang: 'en', text: 'Sure — 12*12 equals 144.' },
  { lang: 'en', text: 'The division 100/4 is equal to 25.' },
  { lang: 'it', text: '7*8 fa 54.' },                                            // wrong on purpose
  { lang: 'en', text: '9+9 equals 19.' },                                        // wrong on purpose
  { lang: 'it', text: 'Il quadrato di 9, cioè 9*9, è uguale a 81.' },
  { lang: 'en', text: 'Half of 90 is 45, since 90/2 = 45.' },
  { lang: 'it', text: 'Applicando la formula: 3*(4+5) = 27.' },
  // capital-of facts (the known routed pattern), true and false
  { lang: 'it', text: 'La capitale della Francia è Parigi.' },
  { lang: 'it', text: 'La capitale della Francia è Lione.' },                     // wrong on purpose
  { lang: 'it', text: 'La capitale del Giappone è Tokyo.' },
  { lang: 'it', text: 'La capitale della Spagna è Madrid, una città molto viva.' },
  { lang: 'en', text: 'The capital of France is Paris.' },
  { lang: 'en', text: 'The capital of Germany is Munich.' },                      // wrong on purpose
  { lang: 'en', text: 'The capital of Italy is Rome.' },
  { lang: 'en', text: 'As you asked: the capital of Portugal is Lisbon.' },
  { lang: 'it', text: 'La capitale dell\'Australia è Canberra, non Sydney.' },
  { lang: 'en', text: 'The capital city of Canada is Ottawa.' },
  // entity descriptions with embedded measurements (typical "chi è / cos'è" cloud answers)
  { lang: 'it', text: 'Mercurio è il pianeta più interno del sistema solare, con un diametro di circa 4879 km.' },
  { lang: 'it', text: 'Il Monte Bianco è alto 4808 metri ed è la vetta più alta delle Alpi.' },
  { lang: 'it', text: 'Dante Alighieri nacque a Firenze nel 1265 e morì a Ravenna nel 1321.' },
  { lang: 'it', text: 'La Luna dista in media 384400 km dalla Terra.' },
  { lang: 'it', text: 'Il Po è lungo 652 km ed è il fiume più lungo d\'Italia.' },
  { lang: 'en', text: 'The Eiffel Tower is 330 metres tall and was completed in 1889.' },
  { lang: 'en', text: 'Mount Everest rises 8849 metres above sea level.' },
  { lang: 'en', text: 'Light travels at about 299792 km per second.' },
  { lang: 'en', text: 'The Great Wall of China is over 21000 km long.' },
  { lang: 'en', text: 'Water boils at 100 degrees Celsius at sea level.' },
  // bare factual assertions without a routed pattern (assertive, but no known key)
  { lang: 'it', text: 'Giuseppe Verdi è il compositore de La Traviata.' },
  { lang: 'it', text: 'Il gallio è un metallo che fonde a temperatura ambiente.' },
  { lang: 'en', text: 'Ada Lovelace is considered the first computer programmer.' },
  { lang: 'en', text: 'The Sahara is the largest hot desert on Earth.' },
  { lang: 'it', text: 'La fotosintesi è il processo con cui le piante producono energia.' },
  // procedural / chatty copilot turns (no claims at all — should extract nothing)
  { lang: 'it', text: 'Fatto! Ho aperto l\'app Musica.' },
  { lang: 'it', text: 'Ho creato il file appunti.txt nella cartella Documenti.' },
  { lang: 'it', text: 'Ecco, promemoria impostato per domani.' },
  { lang: 'en', text: 'Done — the note has been saved.' },
  { lang: 'en', text: 'Opening the weather app now.' },
  // mixed multi-sentence replies (the realistic worst case)
  { lang: 'it', text: 'Parigi è la capitale della Francia. Ha circa 2100000 abitanti e la Torre Eiffel è alta 330 metri.' },
  { lang: 'en', text: 'Rome is in Italy. The capital of Italy is Rome, and the city has about 2800000 inhabitants.' },
  { lang: 'it', text: 'Il conto è presto fatto: 25*4 = 100. Quindi servono 100 euro in tutto.' },
  { lang: 'en', text: 'Quick check: 6*7 equals 42, so the answer to your question is 42.' },
  { lang: 'it', text: 'Ti riassumo: la capitale della Germania è Berlino, e la città ha più di 3500000 abitanti.' },
  { lang: 'en', text: 'In short, the speed of sound is about 343 metres per second in air.' },
  { lang: 'it', text: 'Napoleone nacque nel 1769 in Corsica. Divenne imperatore nel 1804.' },
  { lang: 'en', text: 'The human body has 206 bones, and an adult heart beats about 100000 times a day.' },
  { lang: 'it', text: 'Certo! Il Colosseo fu inaugurato nell\'80 d.C. e poteva contenere circa 50000 spettatori.' },
  { lang: 'en', text: 'The Pacific is the largest ocean, covering more than 165000000 square km.' },
];

// ── run ───────────────────────────────────────────────────────────────────────────────────────
const isMain = process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1];
if (isMain) {
  let replies = 0, claims = 0, routed = 0;
  const verdicts = { confirmed: 0, contradicted: 0, unknown: 0, error: 0 };
  const unrouted = [];
  for (const r of CORPUS) {
    replies++;
    const cs = extractProseClaims(r.text);
    for (const c of cs) {
      if (!c.checkable && c.kind !== 'fact') continue;        // uncovered span: counted below as unrouted
      claims++;
      const spec = routeClaim(c);
      if (!spec) { unrouted.push(c.text.slice(0, 60)); continue; }
      routed++;
      const v = verify(spec, r.lang);
      verdicts[v] = (verdicts[v] || 0) + 1;
    }
  }
  const decisive = verdicts.confirmed + verdicts.contradicted;
  const pct = (n, d) => (d ? Math.round((n / d) * 100) : 0);
  console.log(`replies: ${replies}`);
  console.log(`claims extracted (checkable): ${claims}`);
  console.log(`claims routed to the verifier: ${routed} (${pct(routed, claims)}%)`);
  console.log(`verdicts: confirmed=${verdicts.confirmed} contradicted=${verdicts.contradicted} unknown=${verdicts.unknown} error=${verdicts.error}`);
  console.log(`DECISIVE yield: ${decisive}/${claims} extracted (${pct(decisive, claims)}%) — build the badges only above ~20%`);
  if (unrouted.length) console.log(`unrouted claim texts (${unrouted.length}):\n  - ` + unrouted.slice(0, 12).join('\n  - '));
}
