/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-langs.js — the OS languages, once (docs/languages.md).
//
// Pure: no DOM, no storage, no side effects, so the shell, the i18n engine, the AI layer and host tests all import the
// same table instead of keeping their own five- or six-entry copies. Who shows words uses `pick()`; who talks to the
// device uses `deviceLang()`; who tells a model which language to answer in uses `languageName()`.
//
//   import { LANGS, pick, deviceLang, languageName, localeOf } from '/nucleo-langs.js';
//   pick('zh', ['it', 'en'])   → 'en'    (the best language a table has for the OS language)
//   deviceLang('zh')           → 'en'    (what to send /api/lang, settings.json, the device's ANIMA)
//   languageName('zh')         → 'Simplified Chinese'

export const BASE = 'en';   // the default and the fallback floor of every word in the OS

// code · native label · flag · direction · locale (formats, Intl) · English name (prompts) · stored by the device
export const LANGS = Object.freeze([
  { code: 'it', label: 'Italiano', flag: '🇮🇹', dir: 'ltr', locale: 'it-IT', name: 'Italian', device: true },
  { code: 'en', label: 'English',  flag: '🇬🇧', dir: 'ltr', locale: 'en-US', name: 'English', device: true },
  { code: 'es', label: 'Español',  flag: '🇪🇸', dir: 'ltr', locale: 'es-ES', name: 'Spanish', device: true },
  { code: 'fr', label: 'Français', flag: '🇫🇷', dir: 'ltr', locale: 'fr-FR', name: 'French',  device: true },
  { code: 'de', label: 'Deutsch',  flag: '🇩🇪', dir: 'ltr', locale: 'de-DE', name: 'German',  device: true },
  // Browser-only: every firmware released so far stores only the five above and turns any other code into 'en'.
  { code: 'zh', label: '简体中文', flag: '🇨🇳', dir: 'ltr', locale: 'zh-CN', name: 'Simplified Chinese', device: false },
].map(Object.freeze));

export const CODES = Object.freeze(LANGS.map((l) => l.code));
// What a device stores (POST /api/lang, settings.json ui.language, the native UI). Grow it only behind a capability
// that says the device stores the new code.
export const DEVICE_LANGS = Object.freeze(LANGS.filter((l) => l.device).map((l) => l.code));
const META = Object.freeze(Object.fromEntries(LANGS.map((l) => [l.code, l])));

// 'zh-CN', 'ZH', ' it ' → the OS code, or null when it is not an OS language.
export function normalize(code) {
  const c = String(code == null ? '' : code).trim().toLowerCase().slice(0, 2);
  return META[c] ? c : null;
}
export const meta = (code) => META[normalize(code)] || null;

// The language to use for `code` among the ones a table has (`available`, default: every OS language): itself, or
// English — never a code the table lacks, never Italian by accident.
export function pick(code, available = CODES) {
  const c = normalize(code);
  if (c && available.includes(c)) return c;
  return available.includes(BASE) ? BASE : available[0] || BASE;
}

// What to tell the device for display language `code`: itself when the device stores it, else English.
export const deviceLang = (code) => pick(code, DEVICE_LANGS);

// The English name a model understands ("Always reply in Simplified Chinese."); English for an unknown code.
export const languageName = (code) => (meta(code) || META[BASE]).name;

// The BCP-47 locale for Intl (dates, numbers); English's for an unknown code.
export const localeOf = (code) => (meta(code) || META[BASE]).locale;
