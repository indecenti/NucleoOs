/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-shortcuts.js — the shell's messages to this window, turned into the file manager's actions.
// The shell owns Ctrl+C/X/V/A/Z, F2, Del and F5 (shell.js osKeydown → {type:'os-shortcut', action}) and streams
// device events ({t:'fs.changed', d:{path}}). Keeping the mapping here, without DOM, lets the tests post the very
// messages the shell posts (tests/suite/file-commander-shortcuts.test.mjs).
//
// ctrl: { local(): bool, writable(): bool, copy, cut, paste(clip), selectAll, del, rename, undo, refresh,
//         newWindow, fsChanged(path) }

export function handleShellMessage(d, ctrl) {
  if (!d || typeof d !== 'object') return false;
  if (d.t === 'fs.changed') { ctrl.fsChanged(d.d && typeof d.d.path === 'string' ? d.d.path : null); return true; }
  if (d.type !== 'os-shortcut') return false;
  // On a folder of this computer, writing actions apply only where it is writable; undo is the device's.
  if (ctrl.local()) {
    const w = ctrl.writable();
    switch (d.action) {
      case 'selectAll': ctrl.selectAll(); break;
      case 'copy': ctrl.copy(); break;
      case 'cut': if (w) ctrl.cut(); break;
      case 'paste': if (w) ctrl.paste(null); break;
      case 'delete': if (w) ctrl.del(); break;
      case 'rename': if (w) ctrl.rename(); break;
      case 'refresh': ctrl.refresh(); break;
      default: return false;
    }
    return true;
  }
  switch (d.action) {
    case 'copy': ctrl.copy(); break;
    case 'cut': ctrl.cut(); break;
    case 'paste': ctrl.paste(null); break;     // what to paste is read from the OS clipboard, never taken from the message
    case 'selectAll': ctrl.selectAll(); break;
    case 'delete': ctrl.del(); break;
    case 'rename': ctrl.rename(); break;
    case 'undo': ctrl.undo(); break;
    case 'refresh': ctrl.refresh(); break;
    case 'new': ctrl.newWindow(); break;       // Ctrl+N: another window, like every desktop file manager
    default: return false;
  }
  return true;
}

// Paths the OS rewrites all the time (window layout, session, the trash ledger): never a folder being browsed.
export const FS_NOISE = new Set(['/system/config/ui-state.json', '/system/config/session.json', '/system/config/trash.json']);
