// Pure keyboard-shortcut resolution shared by the shell (osKeydown) and its unit tests. No DOM and
// no side effects: given the salient bits of a keydown event plus the focus context, decide what the
// OS should do. Keeping this pure makes the routing exhaustively testable (tools/shortcuts.test.mjs).
//
// Returns exactly one of:
//   { type: 'doc',    action }       document/app action (save | saveAs | new | open) — fires even
//                                    while typing in a field; the shell preventDefaults it and routes
//                                    it to the focused app so the browser never hijacks Ctrl+S/N/O.
//   { type: 'edit',   action }       editing action (copy | cut | paste | selectAll | undo | redo |
//                                    rename | delete | refresh) — only OUTSIDE text fields; routed to
//                                    the focused app, or to the desktop when no window is active.
//   { type: 'native', action: null } inside a text field, any non-document combo → leave it to the
//                                    browser (native copy/paste/select-all/undo/typing).
//   { type: 'none',   action: null } nothing for the OS to do.

const DOC = { s: 'save', n: 'new', o: 'open' };
const EDIT = { c: 'copy', x: 'cut', v: 'paste', a: 'selectAll', z: 'undo', y: 'redo' };

export function resolveShortcut(e, ctx = {}) {
  const ctrl = e.ctrlKey || e.metaKey;
  const key = e.key || '';
  const k = key.toLowerCase();

  // Document actions fire even inside a text field, but only when an app window is focused (so the
  // bare desktop never swallows Ctrl+S/N/O). Save-As is Ctrl+Shift+S.
  if (ctrl && ctx.hasActiveWindow) {
    if (e.shiftKey && k === 's') return { type: 'doc', action: 'saveAs' };
    if (!e.shiftKey && DOC[k]) return { type: 'doc', action: DOC[k] };
  }
  // Inside a text field every other combo stays native (the app's own typing/clipboard/undo).
  if (ctx.editable) return { type: 'native', action: null };

  let action = null;
  if (ctrl && !e.shiftKey) action = EDIT[k] || null;
  else if (key === 'F2') action = 'rename';
  else if (key === 'Delete') action = 'delete';
  else if (key === 'F5') action = 'refresh';
  return action ? { type: 'edit', action } : { type: 'none', action: null };
}
