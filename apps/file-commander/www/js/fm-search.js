/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-search.js — "Search in subfolders": a bounded walk of the device's folders with fs.list.
// Small devices answer one listing at a time, so the walk is breadth-first, one folder after the other, with
// hard caps on results, depth and folders visited, and it stops the moment it is cancelled.
// Pure apart from the injected `list`: tests/suite/file-commander-search.test.mjs.
import { join, isInside } from './fm-path.js';

export const SEARCH_LIMITS = { maxResults: 300, maxDepth: 8, maxDirs: 1500 };
// Folders never walked into from outside (the recycle bin lives in /system): a search started inside one still
// looks there.
const SKIP = ['/system', '/proc', '/dev'];

// "*.txt", "photo?" → wildcards; anything else → a case-insensitive "contains".
export function matcher(query) {
  const q = String(query == null ? '' : query).trim().toLowerCase();
  if (!q) return () => false;
  if (/[*?]/.test(q)) {
    const re = new RegExp('^' + q.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$');
    return (name) => re.test(String(name).toLowerCase());
  }
  return (name) => String(name).toLowerCase().includes(q);
}

// searchTree({ list, root, query, showHidden, limits, signal, onProgress, onResult })
//   list(dir) → [{ name, type, size, … }]   (fs.list)
//   signal: { aborted } (an AbortSignal works)
//   onResult(entry) as each match is found: { ...entry, path, dir }
// → { results, truncated: false | 'results' | 'dirs', cancelled, dirs (visited) }
export async function searchTree({ list, root = '/data', query, showHidden = false, limits = {}, signal = null, onProgress = null, onResult = null }) {
  const lim = { ...SEARCH_LIMITS, ...limits };
  const match = matcher(query);
  const results = [];
  const queue = [{ dir: root, depth: 0 }];
  let dirs = 0, truncated = false;
  while (queue.length) {
    if (signal && signal.aborted) return { results, truncated, cancelled: true, dirs };
    if (dirs >= lim.maxDirs) { truncated = 'dirs'; break; }
    const { dir, depth } = queue.shift();
    dirs++;
    let entries;
    try { entries = await list(dir); } catch { continue; }          // an unreadable folder is skipped, not fatal
    if (signal && signal.aborted) return { results, truncated, cancelled: true, dirs };
    for (const e of entries || []) {
      if (!e || typeof e.name !== 'string') continue;
      if (!showHidden && e.name.startsWith('.')) continue;
      const path = join(dir, e.name);
      if (match(e.name)) {
        const hit = { ...e, path, dir };
        results.push(hit);
        if (onResult) { try { onResult(hit); } catch {} }
        if (results.length >= lim.maxResults) { truncated = 'results'; break; }
      }
      if (e.type === 'dir' && depth + 1 <= lim.maxDepth && !SKIP.some((s) => isInside(path, s) && !isInside(root, s))) queue.push({ dir: path, depth: depth + 1 });
    }
    if (truncated) break;
    if (onProgress) { try { onProgress({ dirs, found: results.length, dir }); } catch {} }
  }
  return { results, truncated, cancelled: false, dirs };
}
