/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-types.js — what a file is, by its name: category, the label the user reads ("PDF document"), the icon.
// Pure (no DOM): the tests import it directly (tests/suite/file-commander-types.test.mjs).

// ext → [category, label key]. The category picks the icon and its colour; the label key the words.
const T = {};
const add = (cat, label, exts) => { for (const e of exts.split(' ')) T[e] = [cat, label]; };
add('image', 'type_image', 'png jpg jpeg gif webp bmp svg ico avif heic heif tif tiff');
add('audio', 'type_audio', 'mp3 wav ogg oga flac m4a aac opus mid midi wma');
add('video', 'type_video', 'mp4 webm mov mkv m4v avi mpg mpeg m1v 3gp ogv wmv');
add('text', 'type_text', 'txt text rtf nfo srt');
add('text', 'type_markdown', 'md markdown');
add('text', 'type_log', 'log');
add('text', 'type_config', 'ini cfg conf env');
add('code', 'type_code', 'js mjs cjs ts tsx jsx json html htm css scss py c h cpp hpp cc rs go java kt sh bat cmd ps1 lua xml yaml yml toml sql php rb');
add('archive', 'type_archive', 'zip rar 7z tar gz tgz bz2 xz zst');
add('pdf', 'type_pdf', 'pdf');
add('document', 'type_document', 'doc docx odt pages epub');
add('spreadsheet', 'type_spreadsheet', 'xls xlsx ods csv tsv numbers');
add('presentation', 'type_presentation', 'ppt pptx odp key');
add('app', 'type_app', 'exe com nap wasm jsdos apk');
add('app', 'type_rom', 'gb gbc gba nes sfc smc gg sms wad');
add('app', 'type_disk', 'img ima iso');
add('font', 'type_font', 'ttf otf woff woff2');
add('shortcut', 'type_shortcut', 'lnk url desktop');
// rtf is a document for most people
T.rtf = ['document', 'type_document'];

export const CATEGORIES = ['folder', 'image', 'audio', 'video', 'text', 'code', 'archive', 'pdf', 'document', 'spreadsheet',
  'presentation', 'app', 'font', 'shortcut', 'unknown'];

// "Photo.JPG" → "jpg"; "archive.tar.gz" → "gz"; ".bashrc" → "" (a hidden name, not an extension); "a." → "".
export function extOf(name) {
  const base = String(name == null ? '' : name).split('/').pop();
  const i = base.lastIndexOf('.');
  if (i <= 0 || i === base.length - 1) return '';
  return base.slice(i + 1).toLowerCase();
}

export function categoryOf(name, isDir = false) {
  if (isDir) return 'folder';
  const r = T[extOf(name)];
  return r ? r[0] : 'unknown';
}

// The words for the Type column and Properties, through the app's t(): "PNG image", "PDF document", "Folder",
// "XYZ file", "File" (no extension).
export function typeLabel(name, isDir, t) {
  if (isDir) return t('type_folder');
  const ext = extOf(name);
  const r = T[ext];
  if (r) return t(r[1], { ext: ext.toUpperCase() });
  return ext ? t('type_file', { ext: ext.toUpperCase() }) : t('type_file_noext');
}
// The sort key of the Type column: folders first, then by the words a user reads, then by extension.
export const typeSortKey = (name, isDir) => (isDir ? '0' : '1' + categoryOf(name) + '.' + extOf(name));

const IMG_THUMB = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'svg', 'ico', 'avif']);
export const isImage = (name) => IMG_THUMB.has(extOf(name));
// Text the preview may show the start of (8 KB at most, never the whole file).
export function isTextLike(name) {
  const c = categoryOf(name);
  return c === 'text' || c === 'code' || ['csv', 'tsv', 'svg'].includes(extOf(name));
}
export const isRunnable = (name) => /\.(m?js|cjs)$/i.test(String(name));
export const isZip = (name) => extOf(name) === 'zip';
export const isShortcut = (name) => extOf(name) === 'lnk';

// ── icons: one 24 px line set, coloured per category by CSS (.ti-<category> { color }) ──
const DOC_BODY = '<path d="M6.5 2.75h7.25l4.75 4.75v13a.75.75 0 0 1-.75.75H6.5a.75.75 0 0 1-.75-.75v-17a.75.75 0 0 1 .75-.75z" fill="currentColor" fill-opacity=".13" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/><path d="M13.5 2.9V7.75h4.85" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/>';
const S = 'fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"';
const svg = (inner, cls) => `<svg class="ti ti-${cls}" viewBox="0 0 24 24" width="24" height="24" aria-hidden="true" focusable="false">${inner}</svg>`;
const doc = (inner, cls) => svg(DOC_BODY + inner, cls);
const GLYPH = {
  image: '<circle cx="10" cy="11.5" r="1.4" fill="currentColor"/><path d="M8 18.5l3-3.6 2 2.1 2.2-2.8 2.3 4.3z" fill="currentColor"/>',
  audio: `<path d="M11 17.5v-6.2l4.5-1.1v5.6" ${S}/><circle cx="9.9" cy="17.5" r="1.3" fill="currentColor"/><circle cx="14.4" cy="15.8" r="1.3" fill="currentColor"/>`,
  video: '<path d="M10.2 11.2l5 3.1-5 3.1z" fill="currentColor"/>',
  text: `<path d="M9 11.5h6.5M9 14.5h6.5M9 17.5h4" ${S}/>`,
  code: `<path d="M10.5 11.5l-2.3 2.7 2.3 2.7M14 11.5l2.3 2.7L14 16.9" ${S}/>`,
  archive: `<path d="M11.25 3v1.6h1.5v1.6h-1.5v1.6h1.5v1.6h-1.5" ${S}/><rect x="10.25" y="11.2" width="3.5" height="4" rx=".7" ${S}/>`,
  pdf: '<rect x="7.6" y="13" width="9.2" height="5" rx="1" fill="currentColor"/><path d="M9.3 11h5.8" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/>',
  document: `<path d="M9 11h6.5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/><path d="M9 14.2h6.5M9 17.2h6.5" ${S}/>`,
  spreadsheet: `<rect x="8.5" y="10.8" width="7.5" height="7" rx=".6" ${S}/><path d="M8.5 14.3h7.5M12.25 10.8v7" ${S}/>`,
  presentation: `<rect x="8.3" y="10.8" width="8" height="5.2" rx=".6" ${S}/><path d="M12.3 16v2.2" ${S}/>`,
  font: `<path d="M9.3 18l2.9-7.5 2.9 7.5M10.4 15.4h3.6" ${S}/>`,
  shortcut: `<path d="M9 18c0-3.4 2.2-5 6.2-5" ${S}/><path d="M13.4 10.9l2.2 2.1-2.2 2.1" ${S}/>`,
  unknown: '',
};
const FOLDER = '<path d="M2.75 6.5a1 1 0 0 1 1-1h5.1l2 2h9.4a1 1 0 0 1 1 1V18.5a1 1 0 0 1-1 1H3.75a1 1 0 0 1-1-1z" fill="currentColor" fill-opacity=".9"/><path d="M2.75 9.25h18.5" stroke="var(--folder-crease, rgba(0,0,0,.18))" stroke-width="1.2"/>';
const APP = `<rect x="3.25" y="4.25" width="17.5" height="15.5" rx="2" fill="currentColor" fill-opacity=".13" stroke="currentColor" stroke-width="1.3"/><path d="M3.25 8.25h17.5" ${S}/><path d="M8.5 12.5l2.5 2-2.5 2M12.5 16.5h3" ${S}/>`;

export function iconFor(name, isDir = false) {
  const c = categoryOf(name, isDir);
  if (c === 'folder') return svg(FOLDER, 'folder');
  if (c === 'app') return svg(APP, 'app');
  return doc(GLYPH[c] || '', c);
}
export const folderIcon = () => svg(FOLDER, 'folder');

// Places and locations in the sidebar (same line style, coloured by CSS).
const PLACE = {
  device: `<rect x="3.5" y="6" width="17" height="12" rx="2" ${S}/><path d="M7 14.5h.1M10 14.5h.1" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>`,
  computer: `<rect x="3" y="4.5" width="18" height="12" rx="1.5" ${S}/><path d="M9 20h6M12 16.5V20" ${S}/>`,
  drive: `<rect x="3" y="7" width="18" height="10" rx="2" ${S}/><path d="M16.5 12h.1" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"/>`,
  desktop: `<rect x="3" y="4.5" width="18" height="12" rx="1.5" ${S}/><path d="M9 20h6M12 16.5V20" ${S}/>`,
  documents: `<path d="M6.5 3h7l4.5 4.5V21h-11.5z" ${S}/><path d="M13.5 3v4.5H18M9 12h6M9 15h6M9 18h4" ${S}/>`,
  downloads: `<path d="M12 3.5v11M7.5 10.5L12 15l4.5-4.5M4.5 19.5h15" ${S}/>`,
  pictures: `<rect x="3.5" y="5" width="17" height="14" rx="1.5" ${S}/><circle cx="9" cy="10" r="1.5" ${S}/><path d="M4 17.5l5-4.5 3.5 3 3-2.5 4.5 4" ${S}/>`,
  music: `<path d="M9.5 17.5V6.5l9-2v11" ${S}/><circle cx="7.5" cy="17.5" r="2" ${S}/><circle cx="16.5" cy="15.5" r="2" ${S}/>`,
  videos: `<rect x="3.5" y="5.5" width="17" height="13" rx="1.5" ${S}/><path d="M10 9.5l5 2.5-5 2.5z" ${S}/>`,
  star: `<path d="M12 3.6l2.6 5.3 5.8.8-4.2 4.1 1 5.8L12 16.9l-5.2 2.7 1-5.8-4.2-4.1 5.8-.8z" ${S}/>`,
  folder: `<path d="M3 6.5h6l2 2h10v10H3z" ${S}/>`,
  cloud: `<path d="M7 18.5h10a4 4 0 0 0 .6-7.95A5.5 5.5 0 0 0 7.05 9.6 4.5 4.5 0 0 0 7 18.5z" ${S}/>`,
};
export const placeIcon = (k) => `<svg class="pi pi-${k}" viewBox="0 0 24 24" width="18" height="18" aria-hidden="true" focusable="false">${PLACE[k] || PLACE.folder}</svg>`;
