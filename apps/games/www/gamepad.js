// gamepad.js — NucleoOS shared gamepad/joystick layer (single source of truth).
//
// Browsers expose the W3C Gamepad API, so ANY USB/Bluetooth controller paired to the device viewing
// the web-OS (phone/tablet/PC) is usable — no firmware needed. This manager polls the pads, applies
// a deadzone, does edge-detection + typematic repeat, and emits THREE clean streams the hub routes
// to whatever game is running. It is game-agnostic: every game gets controller support for free.
//
//   onAxis(x, y)         continuous left-stick (with D-pad folded in), every frame — for real-time
//   onDir(dx, dy)        discrete step (press + auto-repeat) from D-pad/stick — for menu/cursor nav
//   onButton(name, down) rising/falling edge for A B X Y LB RB LT RT Back Start L3 R3 + D-pad dirs
//   onConnect / onDisconnect(info)
//
// Standard mapping (Gamepad "standard" layout): buttons 0=A 1=B 2=X 3=Y 4=LB 5=RB 6=LT 7=RT
// 8=Back 9=Start 10=L3 11=R3 12=Up 13=Down 14=Left 15=Right; axes 0/1 left stick, 2/3 right stick.

const BTN = { 0: 'A', 1: 'B', 2: 'X', 3: 'Y', 4: 'LB', 5: 'RB', 6: 'LT', 7: 'RT', 8: 'Back', 9: 'Start', 10: 'L3', 11: 'R3', 12: 'Up', 13: 'Down', 14: 'Left', 15: 'Right' };
const DEAD = 0.35;
const REPEAT_DELAY = 240, REPEAT_RATE = 130;   // ms: first repeat, then steady

export class GamepadManager {
  constructor(cb = {}) {
    this.cb = cb;
    this._raf = 0;
    this._running = false;
    this._prevBtn = {};                 // name -> bool (last frame)
    this._dir = { dx: 0, dy: 0, t: 0, next: 0 };  // typematic state
    this._onConn = (e) => { this._announce(e.gamepad, true); };
    this._onDisc = (e) => { this._announce(e.gamepad, false); };
  }

  start() {
    if (this._running) return;
    this._running = true;
    window.addEventListener('gamepadconnected', this._onConn);
    window.addEventListener('gamepaddisconnected', this._onDisc);
    // Surface an already-connected pad on start.
    const gp = this._active();
    if (gp && this.cb.onConnect) this.cb.onConnect({ id: gp.id, index: gp.index });
    this._loop();
  }

  stop() {
    this._running = false;
    if (this._raf) cancelAnimationFrame(this._raf);
    window.removeEventListener('gamepadconnected', this._onConn);
    window.removeEventListener('gamepaddisconnected', this._onDisc);
    this._prevBtn = {};
  }

  // Best-effort haptics (score/win feedback). Silently no-ops where unsupported.
  rumble(ms = 160, strong = 0.6, weak = 0.3) {
    const gp = this._active(); if (!gp) return;
    try {
      if (gp.vibrationActuator && gp.vibrationActuator.playEffect) {
        gp.vibrationActuator.playEffect('dual-rumble', { duration: ms, strongMagnitude: strong, weakMagnitude: weak });
      } else if (gp.hapticActuators && gp.hapticActuators[0] && gp.hapticActuators[0].pulse) {
        gp.hapticActuators[0].pulse(strong, ms);
      }
    } catch {}
  }

  connected() { return !!this._active(); }

  _active() {
    const pads = (navigator.getGamepads && navigator.getGamepads()) || [];
    for (const p of pads) if (p && p.connected) return p;
    return null;
  }
  _announce(gp, on) {
    const info = { id: gp ? gp.id : '', index: gp ? gp.index : -1 };
    if (on) this.cb.onConnect && this.cb.onConnect(info);
    else this.cb.onDisconnect && this.cb.onDisconnect(info);
  }

  _loop() {
    const tick = () => {
      if (!this._running) return;
      const gp = this._active();
      if (gp) this._sample(gp, performance.now());
      this._raf = requestAnimationFrame(tick);
    };
    this._raf = requestAnimationFrame(tick);
  }

  _sample(gp, now) {
    const b = gp.buttons || [], a = gp.axes || [];
    const pressed = (i) => !!(b[i] && (b[i].pressed || b[i].value > 0.5));
    // axes with D-pad folded in
    let ax = a[0] || 0, ay = a[1] || 0;
    if (Math.abs(ax) < DEAD) ax = 0; if (Math.abs(ay) < DEAD) ay = 0;
    const dpx = (pressed(15) ? 1 : 0) - (pressed(14) ? 1 : 0);
    const dpy = (pressed(13) ? 1 : 0) - (pressed(12) ? 1 : 0);
    const fx = dpx || ax, fy = dpy || ay;
    if (this.cb.onAxis) this.cb.onAxis(fx, fy);

    // discrete typematic direction (for cursor/menu nav)
    const dx = dpx || (ax > 0 ? 1 : ax < 0 ? -1 : 0);
    const dy = dpy || (ay > 0 ? 1 : ay < 0 ? -1 : 0);
    if (this.cb.onDir) this._typematic(dx, dy, now);

    // buttons (edge-detected)
    if (this.cb.onButton) {
      for (const i in BTN) {
        const name = BTN[i], down = pressed(Number(i)), was = !!this._prevBtn[name];
        if (down !== was) this.cb.onButton(name, down);
        this._prevBtn[name] = down;
      }
    }
  }

  _typematic(dx, dy, now) {
    const d = this._dir;
    if (dx === 0 && dy === 0) { d.dx = 0; d.dy = 0; d.next = 0; return; }
    if (dx !== d.dx || dy !== d.dy) { d.dx = dx; d.dy = dy; d.next = now + REPEAT_DELAY; this.cb.onDir(dx, dy); return; }
    if (now >= d.next) { d.next = now + REPEAT_RATE; this.cb.onDir(dx, dy); }
  }
}
