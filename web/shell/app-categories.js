/* SPDX-License-Identifier: AGPL-3.0-or-later */
// app-categories.js — ONE category vocabulary for every app of the OS: built-in apps (their manifest / the app
// catalog), apps installed from a store (the signed catalog line, the installed manifest), and the person's own
// choice. Start, the Store and the installer all ask this module, so an app lands in the same place everywhere.
//
// The stores speak their own words (the web apps channel: games tools media office learn network security system;
// the P4 store: category names): `normalize()` maps every known word to one canonical id, anything unknown to
// 'other'. A new word is added HERE, once, with its alias.

// The canonical categories, in the order Start shows them.
export const CATEGORIES = [
  { id: 'productivity' },
  { id: 'media' },
  { id: 'games' },
  { id: 'learn' },
  { id: 'tools' },
  { id: 'connectivity' },
  { id: 'security' },
  { id: 'system' },
  { id: 'other' },
];
export const CATEGORY_IDS = CATEGORIES.map((c) => c.id);

const ALIASES = {
  office: 'productivity', productivity: 'productivity', documents: 'productivity', document: 'productivity',
  work: 'productivity', business: 'productivity', notes: 'productivity',
  media: 'media', multimedia: 'media', music: 'media', audio: 'media', video: 'media', photo: 'media', photos: 'media',
  graphics: 'media', entertainment: 'media',
  games: 'games', game: 'games', gaming: 'games', emulators: 'games', emulator: 'games', arcade: 'games',
  learn: 'learn', learning: 'learn', education: 'learn', edu: 'learn', science: 'learn', reference: 'learn',
  tools: 'tools', tool: 'tools', utilities: 'tools', utility: 'tools', development: 'tools', developer: 'tools',
  dev: 'tools', accessories: 'tools',
  connectivity: 'connectivity', network: 'connectivity', networking: 'connectivity', internet: 'connectivity',
  communication: 'connectivity', social: 'connectivity', web: 'connectivity',
  security: 'security', privacy: 'security',
  system: 'system', settings: 'system', os: 'system',
  other: 'other', misc: 'other',
};

// Any word (any case, spaces or dashes) -> a canonical id; '' / unknown -> '' (the caller decides the fallback).
export function normalize(word) {
  const k = String(word == null ? '' : word).trim().toLowerCase().replace(/[\s_-]+/g, '');
  return ALIASES[k] || '';
}

// The category an app shows under. In order: the person's choice, the app's own (registry / manifest), the store's
// (signed catalog line, kept beside the installed app), the built-in catalog, then 'other'.
export function categoryOf(app, { overrides = {}, catalog = {} } = {}) {
  if (!app) return 'other';
  const own = overrides && overrides[app.id];
  if (CATEGORY_IDS.includes(own)) return own;
  return normalize(app.category) || normalize(app.store_category) || normalize(catalog[app.id]) || 'other';
}

// The i18n key of a category's name (shell namespace: start_cat_<id>).
export const categoryKey = (id) => 'start_cat_' + (CATEGORY_IDS.includes(id) ? id : 'other');

export default { CATEGORIES, CATEGORY_IDS, normalize, categoryOf, categoryKey };
