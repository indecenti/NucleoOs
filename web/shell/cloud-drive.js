/* SPDX-License-Identifier: AGPL-3.0-or-later */
// When the core's own cloud archive (nucleo-archive.js) serves /r2 for nucleo-fs.js. Internal to the core: apps reach
// /r2 through nucleo-fs.js. Its own module so the rule is tested without a browser (tests/cloud-drive.test.mjs).

export function createCloudDrive({ has, status, load }) {
  let drive = null, mode = null;
  return async function cloudDrive(p) {
    if (p !== '/r2' && !p.startsWith('/r2/')) return null;
    try { if (!(await has('cloud.archive'))) return null; } catch { return null; }
    if (mode === null) mode = Promise.resolve().then(status).then((s) => (s && s.mode) || '', () => null);
    const m = await mode;
    if (m === null) mode = null;                    // unknown (unreachable): asked again next time
    else if (m === 'device') return null;
    if (!drive) drive = load();
    return drive;
  };
}
