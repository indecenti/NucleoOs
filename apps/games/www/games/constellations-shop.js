// constellations-shop.js — EXACT port of the Cardputer shipyard upgrade curves (app_constellations.cpp
// up_level/up_cost/buy_upgrade), so web upgrades cost and apply identically to the device. Integer math.
// jump_range base is 64 (the soft-lock fix), matching the firmware's `(g.jump_range - 64) / 12`.

// Each track: how to read its level from the save, its base cost step, and how a purchase applies.
export const SHOP = [
  { key: 'hull',    label: ['Scafo +25', 'Hull +25'],       base: 220, level: (r) => (r.hull_max - 100) / 25,  apply: (r) => { r.hull_max += 25; r.hull = r.hull_max; } },
  { key: 'shield',  label: ['Scudo +30', 'Shield +30'],     base: 260, level: (r) => (r.shield_max - 40) / 30,  apply: (r) => { r.shield_max += 30; } },
  { key: 'weapon',  label: ['Laser +1', 'Laser +1'],        base: 240, level: (r) => r.weapon,                  apply: (r) => { r.weapon += 1; } },
  { key: 'cargo',   label: ['Stiva +10', 'Hold +10'],       base: 250, level: (r) => (r.cargo_max - 20) / 10,   apply: (r) => { r.cargo_max += 10; } },
  { key: 'jump',    label: ['Iperdrive +12', 'Jump +12'],   base: 300, level: (r) => (r.jump_range - 64) / 12,  apply: (r) => { r.jump_range += 12; } },
  { key: 'sensors', label: ['Sensori +1', 'Sensors +1'],    base: 200, level: (r) => r.sensors,                apply: (r) => { r.sensors += 1; } },
  { key: 'tank',    label: ['Serbatoio +4', 'Tank +4'],     base: 180, level: (r) => (r.fuel_max - 8) / 4,      apply: (r) => { r.fuel_max += 4; } },
];
const byKey = Object.fromEntries(SHOP.map((s) => [s.key, s]));

export function shopLevel(key, run) { return Math.max(0, Math.floor(byKey[key].level(run))); }
export function shopMaxed(key, run) { return shopLevel(key, run) >= 4; }
export function shopCost(key, run) { return byKey[key].base * (shopLevel(key, run) + 1); }
// Repair: heal to full at 3 cr per missing hull (firmware up_cost item 7).
export function repairCost(run) { return Math.max(0, (run.hull_max - run.hull) * 3); }

// Mutate the run with a purchase. Returns true on success (caller persists), false if maxed/too poor.
export function shopBuy(key, run) {
  if (key === 'repair') {
    if (run.hull >= run.hull_max) return false;
    const c = repairCost(run); if (run.credits < c) return false;
    run.credits -= c; run.hull = run.hull_max; return true;
  }
  if (shopMaxed(key, run)) return false;
  const c = shopCost(key, run); if (run.credits < c) return false;
  run.credits -= c; byKey[key].apply(run); return true;
}
