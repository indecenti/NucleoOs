// constellations-codex.js — the Codex of Costellazioni (web): lore fragments, factions, places, worlds, ship dossiers
// and the contacts, each with its illustration (stelle/assets) or, for ships without a painting, the live 3D model
// on the hub's hangar backdrop. Entries unlock through play (constellations.js decides when, the web save keeps them);
// every text is in the games catalog as cz_cx_<id>_t / _b / _h (title / body / unlock hint), five languages.
// Pure data (no DOM, no i18n): the game core imports it in Node tests; the screen is built in constellations-ui.js.

export const CATS = ['lore', 'factions', 'places', 'worlds', 'ships', 'contacts'];
// id, category, illustration id (assets manifest), 3D model [class, faction] for the hangar view, start = unlocked from the beginning
const E = (id, cat, img, o = {}) => ({ id, cat, img, ...o });
export const ENTRIES = [
  E('costellatori', 'lore', 'lore_costellatori', { start: true }), E('dimming', 'lore', 'lore_dimming', { start: true }),
  E('beacon_dead', 'lore', 'lore_beacon_dead'), E('beacon_lit', 'lore', 'lore_beacon_lit'), E('relight', 'lore', 'lore_beacon_relight'),
  E('voice', 'lore', 'lore_echo_voice'), E('echo_fleet', 'lore', 'lore_echo_fleet'), E('graveyard', 'lore', 'lore_graveyard'),
  E('deep', 'lore', 'lore_deep'), E('warden', 'lore', 'lore_warden'), E('beyond', 'lore', 'lore_beyond'), E('truth', 'lore', 'lore_truth'),
  E('gilda', 'factions', 'hall_gilda', { start: true, emblem: 'emblem_gilda' }), E('custodi', 'factions', 'hall_custodi', { emblem: 'emblem_custodi' }),
  E('relitti', 'factions', 'hall_relitti', { emblem: 'emblem_relitti' }), E('eco', 'factions', 'hall_eco', { emblem: 'emblem_eco' }),
  E('ardali', 'places', 'station_ardali'), E('lamp', 'places', 'station_lamp'), E('gutterdeep', 'places', 'station_gutterdeep'),
  E('gate', 'places', 'ruin_gate'), E('archive', 'places', 'ruin_archive'), E('observatory', 'places', 'ruin_observatory'),
  ...['rocky', 'desert', 'ocean', 'ice', 'jungle', 'volcanic', 'gas', 'crystal'].map((w) => E('w_' + w, 'worlds', 'world_' + w)),
  E('s_courier', 'ships', 'ship_courier', { start: true, model: ['lucciola', 4] }), E('s_lancer', 'ships', 'ship_lancer', { model: ['lancer', 0] }),
  E('s_bastion', 'ships', 'ship_bastion', { model: ['bastion', 0] }), E('s_warden', 'ships', 'battle_capital', { model: ['warden', 0] }),
  E('s_hauler', 'ships', 'brief_escort', { model: ['hauler', 0] }), E('s_scrapwing', 'ships', 'ship_scrapwing', { model: ['scrapwing', 2] }),
  E('s_harpoon', 'ships', null, { model: ['harpoon', 2] }), E('s_gutter', 'ships', null, { model: ['gutter', 2] }), E('s_hulk', 'ships', 'ship_hulk', { model: ['hulk', 2] }),
  E('s_votive', 'ships', null, { model: ['votive', 1] }), E('s_censer', 'ships', 'ship_censer', { model: ['censer', 1] }), E('s_reliquary', 'ships', null, { model: ['reliquary', 1] }),
  E('s_shard', 'ships', 'battle_lattice', { model: ['shard', 3] }), E('s_lattice', 'ships', 'ship_lattice', { model: ['lattice', 3] }), E('s_choir', 'ships', null, { model: ['choir', 3] }),
  E('c_pilot', 'contacts', 'pilot', { start: true }), E('c_broker', 'contacts', 'broker', { start: true }), E('c_admiral', 'contacts', 'admiral'),
  E('c_abbess', 'contacts', 'abbess'), E('c_novice', 'contacts', 'novice'), E('c_matriarch', 'contacts', 'matriarch'), E('c_mechanic', 'contacts', 'mechanic'),
  E('c_echo', 'contacts', 'echo'), E('c_ace_gutter', 'contacts', 'ace_gutter'), E('c_ace_lancer', 'contacts', 'ace_lancer'), E('c_ace_bram', 'contacts', 'ace_bram'), E('c_ace_vigil', 'contacts', 'ace_vigil'),
];
export const BY_ID = Object.fromEntries(ENTRIES.map((e) => [e.id, e]));
export const entriesOf = (cat) => ENTRIES.filter((e) => e.cat === CATS[cat]);
// where the eye should land when an illustration is cropped (fractions of the image), per image id
export const FOCUS = {
  hall_gilda: [0.33, 0.3], hall_custodi: [0.37, 0.44], hall_relitti: [0.56, 0.3], hall_eco: [0.5, 0.52],
  station_ardali: [0.62, 0.36], station_lamp: [0.56, 0.42], station_gutterdeep: [0.5, 0.44], title: [0.5, 0.42],
  lore_echo_voice: [0.5, 0.4], lore_costellatori: [0.5, 0.42], lore_warden: [0.5, 0.45], lore_truth: [0.5, 0.42],
  pilot: [0.5, 0.36], broker: [0.5, 0.36], admiral: [0.5, 0.36], abbess: [0.5, 0.36], novice: [0.5, 0.38], matriarch: [0.5, 0.34], mechanic: [0.5, 0.38],
  echo: [0.5, 0.42], ace_gutter: [0.5, 0.34], ace_lancer: [0.5, 0.34], ace_bram: [0.5, 0.34], ace_vigil: [0.5, 0.34],
};

