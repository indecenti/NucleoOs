// Pure navigation state machine for the NucleoOS on-device launcher (Wear OS-style).
//
// This is the single source of truth for menu/submenu navigation. It has NO DOM or
// rendering dependency, so it is shared by:
//   - the browser device simulator (web/device/device.js), and
//   - the unit tests (tools/device-ui.test.mjs).
// The firmware (firmware/components/nucleo_app) mirrors this exact logic in C.
//
// Menu tree node shapes:
//   menu:  { id, label, icon, color, type:'menu', desc, items:[node,...] }
//   app:   { id, label, icon, color, type:'app',  desc, actions?:[action,...] }
//   action:{ id, label, icon, desc }   (an app's context-menu entry)

const norm = (s) => (s || '').toLowerCase();

// One level on the navigation stack: which menu we're in, the focused index, and the
// active type-to-filter string.
function frame(node) {
  return { node, sel: 0, filter: '' };
}

export class Launcher {
  constructor(root) {
    if (!root || root.type !== 'menu') throw new Error('root must be a menu node');
    this.root = root;
    this.stack = [frame(root)];
  }

  get top() { return this.stack[this.stack.length - 1]; }
  get depth() { return this.stack.length - 1; }

  // Items in the current menu, narrowed by the active filter (case-insensitive substring).
  visibleItems() {
    const { node, filter } = this.top;
    const all = node.items || [];
    if (!filter) return all;
    const f = norm(filter);
    return all.filter((it) => norm(it.label).includes(f));
  }

  // The focused item (or null when the filter excludes everything).
  focused() {
    const items = this.visibleItems();
    return items.length ? items[Math.min(this.top.sel, items.length - 1)] : null;
  }

  // Snapshot for the renderer: everything needed to draw one screen.
  screen() {
    const items = this.visibleItems();
    const sel = items.length ? Math.min(this.top.sel, items.length - 1) : 0;
    const cur = items[sel] || null;
    return {
      title: this.top.node.label,
      icon: this.top.node.icon,
      color: this.top.node.color,
      breadcrumb: this.stack.map((f) => f.node.label),
      depth: this.depth,
      items,
      sel,
      filter: this.top.filter,
      focused: cur,
      instruction: this.instruction(cur),
      hint: this.hint(cur),
    };
  }

  // A clear, contextual one-line instruction for the focused item.
  instruction(cur) {
    if (!this.visibleItems().length) return `No match for "${this.top.filter}"`;
    if (!cur) return '';
    if (cur.desc) return cur.desc;
    if (cur.type === 'menu') return `Open the ${cur.label} menu`;
    if (cur.type === 'app') return `Launch ${cur.label}`;
    return cur.label;
  }

  // Key hints for the bottom bar, adapted to context. Honest: `/` opens the focused app's
  // options (it is not a movement key), and there is no numeric quick-select — digits type
  // into the filter like any other character.
  hint(cur) {
    if (this.top.filter) return 'type to filter · ⏎ open · esc clear';
    const back = this.depth > 0 ? ' · esc back' : '';
    const more = cur && cur.type === 'app' && cur.actions ? ' · / options' : '';
    return `scroll · ⏎ open${more}${back}`;
  }

  // ---- input ----------------------------------------------------------------
  up() {
    const n = this.visibleItems().length; if (!n) return;
    this.top.sel = (this.top.sel + n - 1) % n;
  }
  down() {
    const n = this.visibleItems().length; if (!n) return;
    this.top.sel = (this.top.sel + 1) % n;
  }

  // Enter / right-on-menu: descend, launch, or run an action. Returns an event:
  //   { type:'launch', node }            an app was opened
  //   { type:'action', action, node }    an app action was chosen
  //   null                               pure navigation (a submenu was pushed)
  enter() {
    const cur = this.focused();
    if (!cur) return null;
    if (cur.type === 'menu') { this.push(cur); return null; }
    if (cur.type === 'action') return { type: 'action', action: cur, node: this.contextOwner };
    // app: launch directly (its context menu is reached with `/`).
    return { type: 'launch', node: cur };
  }

  // `/` (right) on a focused app opens its context submenu of actions, if any.
  openContext() {
    const cur = this.focused();
    if (!cur || cur.type !== 'app' || !cur.actions || !cur.actions.length) return false;
    this.contextOwner = cur;
    const menu = {
      id: cur.id + ':menu', type: 'menu', label: cur.label,
      icon: cur.icon, color: cur.color,
      items: cur.actions.map((a) => ({ ...a, type: 'action' })),
    };
    this.push(menu);
    return true;
  }

  push(node) { this.stack.push(frame(node)); }

  // Esc / backtick / left: clear the filter first, else pop one menu level.
  back() {
    if (this.top.filter) { this.top.filter = ''; this.top.sel = 0; return true; }
    if (this.stack.length > 1) { this.stack.pop(); return true; }
    return false; // already at root
  }

  // Type-to-filter: append a printable character and reset focus to the top match.
  typeChar(ch) {
    if (!ch || ch.length !== 1) return;
    this.top.filter += ch;
    this.top.sel = 0;
  }
  backspace() {
    if (this.top.filter) { this.top.filter = this.top.filter.slice(0, -1); this.top.sel = 0; }
  }

  // Programmatic jump-and-activate of the n-th visible item (1-based). No longer bound to a
  // key — kept as a navigation primitive for tests/automation.
  quick(n) {
    const items = this.visibleItems();
    if (n < 1 || n > items.length) return null;
    this.top.sel = n - 1;
    return this.enter();
  }

  reset() { this.stack = [frame(this.root)]; this.contextOwner = null; }
}

// A small helper to dispatch a raw key name to the launcher. Keeps the simulator and the
// firmware key handling aligned. Any printable character (digits included) types into the
// filter — there is no numeric quick-select. Returns the enter() event, if any.
export function handleKey(launcher, key) {
  switch (key) {
    case 'up': launcher.up(); return null;
    case 'down': launcher.down(); return null;
    case 'enter': return launcher.enter();
    case 'context': launcher.openContext(); return null;
    case 'back': launcher.back(); return null;
    case 'backspace': launcher.backspace(); return null;
    default:
      if (key.length === 1) { launcher.typeChar(key); return null; }
      return null;
  }
}
