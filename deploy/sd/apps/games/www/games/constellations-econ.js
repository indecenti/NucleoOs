// constellations-econ.js — EXACT port of the firmware economy math so prices/credits match the
// Cardputer to the credit. Mirrors app_constellations.cpp: unit_buy / unit_sell / refuel_price /
// jump_cost / sys_dist. Uses CONTENT from constellations-content.js (generated from the .h).
//
// IMPORTANT: the native code does uint32 hash arithmetic with wraparound and integer division.
// JS must replicate the 32-bit unsigned multiply/shift (Math.imul + >>>0) and Math.floor on
// positive integer divisions, or web prices will drift from the device.

const u32 = (x) => x >>> 0;

// unit_buy(sys, good): base * econmod / 100, deterministic ±22% per-(sys,good,epoch) hash, then a
// reputation discount (rep/8, capped 15%). content = CONTENT; rep = save.rep array.
export function unitBuy(content, sys, good, epoch, rep) {
  const g = content.goods[good];
  const econ = content.systems[sys].econ;
  let p = Math.floor((g.base * content.econmod[econ][good]) / 100);
  // h = sys*2654435761 ^ good*40499 ^ epoch*2246822519 ; h ^= h>>13 ; var = (h%45)-22
  let h = u32(u32(Math.imul(sys, 2654435761)) ^ u32(Math.imul(good, 40499)) ^ u32(Math.imul(epoch, 2246822519)));
  h = u32(h ^ (h >>> 13));
  const variance = (h % 45) - 22;                       // -22..+22 %
  p = Math.floor((p * (100 + variance)) / 100);
  const fac = content.systems[sys].faction;
  if (fac >= 0 && rep[fac] > 0) {
    let disc = Math.floor(rep[fac] / 8); if (disc > 15) disc = 15;
    p = Math.floor((p * (100 - disc)) / 100);
  }
  return p < 1 ? 1 : p;
}
export function unitSell(content, sys, good, epoch, rep) {
  const p = Math.floor((unitBuy(content, sys, good, epoch, rep) * 88) / 100);
  return p < 1 ? 1 : p;
}
// refuel_price(sys): per-economy cell price (EC_REFU=4 cheapest ... EC_TECH dearest).
export function refuelPrice(content, sys) {
  const econ = content.systems[sys].econ;
  const E = content.enums.econ;     // { EC_AGRI, EC_MINE, EC_INDU, EC_TECH, EC_REFU }
  if (econ === E.EC_REFU) return 6;
  if (econ === E.EC_AGRI) return 11;
  if (econ === E.EC_MINE) return 9;
  if (econ === E.EC_INDU) return 9;
  return 13;                        // EC_TECH
}
export function sysDist(content, a, b) {
  const dx = content.systems[a].x - content.systems[b].x;
  const dy = content.systems[a].y - content.systems[b].y;
  return Math.sqrt(dx * dx + dy * dy);
}
export function jumpCost(dist) {
  const c = Math.trunc(dist / 10 + 0.5);
  return c < 1 ? 1 : c;
}

// ---- tiny shared helpers used by the trade/jump UI ---------------------------------------------
export const cargoUsed = (save) => save.cargo.reduce((a, n) => a + n, 0);
export function beaconsTotal(content) { return content.systems.filter((s) => s.beacon).length; }
export function beaconsLit(content, save) {
  let n = 0;
  for (let i = 0; i < content.systems.length; i++)
    if (content.systems[i].beacon && ((save.beacon_lit >>> 0) & (1 << i))) n++;
  return n;
}
