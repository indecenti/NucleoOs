// constellations-gen.js — DETERMINISTIC, SEEDED procedural generator for the infinite universe.
//
// This is the canonical reference for the cross-play world: the firmware (app_constellations.cpp)
// ports these exact functions in C, so the SAME (seed, sector) yields a byte-identical universe on
// Cardputer and web — exactly the parity guarantee already proven for the economy hash.
//
// PORTING RULE (identical to constellations-econ.js): all arithmetic is uint32. Every `*` is
// Math.imul wrapped in u32(); every `+`/`^` is wrapped in u32(); every shift is `>>>`; `%` operands
// are coerced `>>>0` first; never `/` (use Math.floor/Math.trunc on non-negative integers).
//
// The constant tables below (HASH consts, faction thresholds, name syllables, mission tuning) are
// duplicated VERBATIM in the firmware; a parity harness diffs C-emitted vs JS output to catch drift.

const u32 = (x) => x >>> 0;

// ---- 1.1 hash primitive (Murmur/xxHash finalizer family; uint32 only) --------------------------
export function hash32(x) {
  x = u32(x);
  x = u32(x ^ (x >>> 16));
  x = u32(Math.imul(x, 2246822519));   // 0x85EBCA6B7 low32 -> mixing constant (odd, invertible)
  x = u32(x ^ (x >>> 13));
  x = u32(Math.imul(x, 3266489917));
  x = u32(x ^ (x >>> 16));
  return x;
}
// 1.1 hash3(a,b,c): combine three keyed words then finalize
export function hash3(a, b, c) {
  let h = u32(0x9E3779B1);
  h = u32(h ^ hash32(u32(a + 0x85EBCA6B)));
  h = u32(Math.imul(h, 2654435761));
  h = u32(h ^ hash32(u32(b + 0xC2B2AE35)));
  h = u32(Math.imul(h, 2654435761));
  h = u32(h ^ hash32(u32(c + 0x27D4EB2F)));
  return hash32(h);
}

// ---- 1.2 seeded streams (counter-based; no mutable state crosses calls) -------------------------
export const DOM = { COORD: 1, ECON: 2, FAC: 3, BEACON: 4, NAME: 5, MISSION: 6, MCOUNT: 7, FLAVOR: 8 };
// system field draw: key = (sector ; idx<<8 | salt)
const rngSys = (seed, sector, idx, dom, salt) =>
  hash3(u32(seed ^ dom), u32(sector), u32(((idx & 0xff) << 8) | (salt & 0xff)));
// mission field draw: key = (sector<<8 | sysIdx ; slot<<8 | salt)
const rngMis = (seed, sector, sysIdx, slot, dom, salt) =>
  hash3(u32(seed ^ dom), u32(((sector & 0xffffff) << 8) | (sysIdx & 0xff)), u32(((slot & 0xff) << 8) | (salt & 0xff)));

// ---- world shape constants (mirror firmware) ---------------------------------------------------
export const NSYS = 10, NECON = 5, NFAC = 4, NMISS_PER_SYS = 4;
export const F_GILDA = 0, F_CUSTODI = 1, F_RELITTI = 2, F_ECO = 3;
// faction weight ladder (cumulative %), and each faction's rival for mission foes
const FAC_LADDER = [40, 65, 88, 100];                 // <40 GILDA, <65 CUSTODI, <88 RELITTI, else ECO
const FAC_RIVAL = [F_RELITTI, F_RELITTI, F_GILDA, F_RELITTI];
// coined-name syllables (language-neutral ASCII)
const PRE = ['Ve', 'Ach', 'El', 'Ty', 'Cu', 'Ro', 'For', 'Qui', 'Ze', 'Ab', 'Xan', 'Or', 'Ka', 'Ny', 'Vor', 'Lu'];
const MID = ['per', 'ron', 'iso', 'cho', 'sta', 'rax', 'mir', ''];
const SUF = ['', 'Primo', 'Nova', 'Reach', 'IX', 'Gate', 'Hub', 'Cluster'];

// ---- 1.5 coined system name --------------------------------------------------------------------
export function genName(seed, sector, idx) {
  const h = rngSys(seed, sector, idx, DOM.NAME, 0);
  let s = PRE[h % 16] + MID[(h >>> 4) % 8];
  const suf = SUF[(h >>> 7) % 8];
  if (suf) s += ' ' + suf;
  return s.slice(0, 15);
}

// ---- 1.3 a procedural system within a sector ---------------------------------------------------
export function genSystem(seed, sector, idx) {
  const col = idx % 4, row = Math.floor(idx / 4);     // 4-col grid, rows 0..2
  const jx = rngSys(seed, sector, idx, DOM.COORD, 0) % 18;   // cellW-6
  const jy = rngSys(seed, sector, idx, DOM.COORD, 1) % 26;   // cellH-6
  const x = 2 + col * 24 + jx;
  const y = 2 + row * 32 + jy;
  const econ = rngSys(seed, sector, idx, DOM.ECON, 0) % NECON;
  const r = rngSys(seed, sector, idx, DOM.FAC, 0) % 100;
  let faction = NFAC - 1;
  for (let f = 0; f < NFAC; f++) if (r < FAC_LADDER[f]) { faction = f; break; }
  // beacon: exactly BPS lowest-hash slots carry a beacon (order-independent rank, no sort)
  const bps = 3 + (sector % 3);
  const hb = (j) => rngSys(seed, sector, j, DOM.BEACON, 0);
  const mine = hb(idx); let rank = 0;
  for (let j = 0; j < NSYS; j++) { const hj = hb(j); if (hj < mine || (hj === mine && j < idx)) rank++; }
  const beacon = rank < bps ? 1 : 0;
  const name = genName(seed, sector, idx);
  return { it: name, en: name, x, y, econ, faction, beacon, descIt: '', descEn: '' };
}

// Build the whole current sector (what the web game and econ module read as content.systems).
export function genSector(seed, sector) {
  return Array.from({ length: NSYS }, (_, i) => genSystem(seed, sector, i));
}
export const beaconsPerSector = (sector) => 3 + (sector % 3);

// ---- 1.4 procedural missions at a system -------------------------------------------------------
export function genMissionCount(seed, sector, sysIdx, systemFaction) {
  if (systemFaction === F_ECO) return 0;              // the Abyss-type has no mission broker
  return 3 + (rngMis(seed, sector, sysIdx, 0, DOM.MCOUNT, 0) % 3);   // 3..5 — fuller mission boards
}
export function genMission(seed, sector, sysIdx, slot, systemFaction) {
  let tier = 1 + sector; if (tier > 12) tier = 12;
  const base = rngMis(seed, sector, sysIdx, slot, DOM.MISSION, 0);
  const type = base % 4;                              // MT_PATROL..MT_DEFEND
  const offer_fac = systemFaction;
  const foe_fac = FAC_RIVAL[systemFaction];
  const waves = 6 + ((base >>> 4) % 4) + (tier >= 4 ? 1 : 0) + (tier >= 8 ? 2 : 0);   // 6..13, longer sorties
  const per_wave = 3 + ((base >>> 8) % 2);                                             // 3..4 (+ ace stays <= NFOE 6)
  const foe_hp = 36 + tier * 7 + ((base >>> 12) % 10);                                 // tougher: foes take more hits
  const foe_dmg = 8 + tier + ((base >>> 16) % 3);                                      // hits harder
  const foe_speed_pml = 820 + tier * 45;
  const ace = ((base >>> 20) % 100) < (15 + tier * 3) ? 1 : 0;
  const kill_cr = 18 + tier * 4;
  const reward_cr = (60 + tier * 40) * waves;
  const rep_gain = 3 + tier;
  const enemy_rep_loss = 2 + Math.floor(tier / 2);
  return { type, offer_fac, foe_fac, waves, per_wave, foe_hp, foe_dmg, foe_speed_pml, ace,
    reward_cr, kill_cr, rep_gain, enemy_rep_loss, tier, sysIdx, slot };
}
export function genMissions(seed, sector, sysIdx, systemFaction) {
  const n = genMissionCount(seed, sector, sysIdx, systemFaction);
  return Array.from({ length: n }, (_, slot) => {
    const m = genMission(seed, sector, sysIdx, slot, systemFaction);
    m.flavor = genMissionFlavor(seed, sector, m, systemFaction);   // web-only evocative layer (no numeric impact)
    return m;
  });
}

// ---- 1.5 procedural mission "flavor" (No Man's Sky style): named targets, archetypes, rarity, briefs.
// Deterministic from a DEDICATED hash domain (DOM.FLAVOR) so it never perturbs the numeric draws above.
const NPC_PRE = ['Vex', 'Krull', 'Mor', 'Zar', 'Drix', 'Nyx', 'Hask', 'Orla', 'Veng', 'Skar', 'Rann', 'Tox', 'Grim', 'Vael', 'Korr', 'Zael'];
const NPC_SUF = ['nor', 'ax', 'is', 'oth', 'ek', 'ul', 'ar', 'ix', 'one', 'ag', 'eth', 'os', 'un', 'ire', 'um', 'or'];
const EPI = ['il Rosso', 'Occhio-Morto', 'la Lama', 'il Corvo', 'Senza-Volto', 'il Flagello', 'Mano-Fredda', "l'Avvoltoio", 'il Cremisi', 'lo Spettro'];
const GANG = ['Corsari Cremisi', 'Lupi del Vuoto', 'Sciacalli della Cenere', 'Predoni di Ferro', 'Flotta Fantasma', 'Branco di Dramir', 'Mietitori Neri', 'Vipere del Vuoto'];
const RAR = [['', '#9fb0bf', 'Comune'], ['★', '#5ee6ff', 'Raro'], ['★★', '#b46be0', 'Epico'], ['★★★', '#e0b13b', 'Leggendario']];
const MODS = ['Nebulosa densa', 'Campo di asteroidi', "Squadriglia d'élite", 'Veterani', 'Branco', 'Taglia maggiorata', 'Tempesta ionica'];
function genMissionFlavor(seed, sector, m, sysFac) {
  const h = rngMis(seed, sector, m.sysIdx, m.slot, DOM.FLAVOR, 1);
  const h2 = rngMis(seed, sector, m.sysIdx, m.slot, DOM.FLAVOR, 2);
  const name = NPC_PRE[h % NPC_PRE.length] + NPC_SUF[(h >>> 5) % NPC_SUF.length] + (m.ace ? ' ' + EPI[(h >>> 10) % EPI.length] : '');
  const gang = GANG[(h >>> 16) % GANG.length];
  let arch, title, brief;
  if (m.type === 1) { arch = (m.ace && (h2 & 1)) ? 'Duello' : 'Caccia'; title = arch + ': ' + name; brief = arch === 'Duello' ? 'Solo tu e ' + name + '. Niente gregari, niente fughe.' : 'Taglia su ' + name + ': arriva con la sua scorta.'; }
  else if (m.type === 2) { arch = 'Scorta'; title = 'Scorta convoglio'; brief = 'Tieni vivo il convoglio: i ' + gang + ' lo vogliono fermo.'; }
  else if (m.type === 3) { arch = (h2 & 2) ? 'Bonifica' : 'Difesa'; title = arch === 'Bonifica' ? 'Bonifica sciame' : 'Difesa faro'; brief = arch === 'Bonifica' ? 'Sciame di droni-saccheggio: tanti, fragili, ovunque.' : 'Proteggi il faro dai ' + gang + ' finché non cedono.'; }
  else { arch = 'Pattuglia'; title = 'Pattuglia'; brief = 'I ' + gang + ' battono la zona. Ricacciali indietro.'; }
  const score = m.tier + (m.ace ? 2 : 0) + (m.waves >= 5 ? 1 : 0) + ((h2 >>> 3) % 3);
  const rarity = score >= 9 ? 3 : score >= 7 ? 2 : score >= 5 ? 1 : 0;
  const mods = [];
  if ((h2 >>> 6) % 3 === 0) mods.push(MODS[(h2 >>> 8) % MODS.length]);
  if (m.tier >= 4 && (h2 >>> 12) % 3 === 0) { const x = MODS[(h2 >>> 14) % MODS.length]; if (!mods.includes(x)) mods.push(x); }
  return { arch, title, brief, name, gang, rarity, star: RAR[rarity][0], color: RAR[rarity][1], rarityName: RAR[rarity][2], mods };
}
