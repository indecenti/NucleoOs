/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoPad - browser client for "use the device's physical joypad here".
//
// This module is the CAPABILITY, not the Console. It knows nothing about
// EmulatorJS, games, or what the buttons mean: it does the handshake, keeps the
// state, and emits raw device events. Whoever imports it decides what they drive.
// Console is consumer #1; a canvas game, a remote for the media player, or a
// presentation app would each map the same events their own way.
//
//   NucleoPad.request('Console')  ask for the pad -> the DEVICE shows a consent
//                                 screen; resolves when the ask was delivered,
//                                 NOT when the user accepted (watch onState)
//   NucleoPad.release()           give it back
//   NucleoPad.onState(cb)         cb({state:'idle'|'pending'|'active', app, client, player})
//   NucleoPad.onButton(cb)        cb({b, v, player})  b = RAW device SDL button, v = 1 down / 0 up
//   NucleoPad.onHat(cb)           cb({m, player})     m = SDL_HAT_* bitmask
//   NucleoPad.onAxis(cb)          cb({a, v, player})  v = -32768..32767
//   NucleoPad.state               last known state string
//
// The device is the source of truth for its own hardware, so button numbers
// arrive RAW (whatever this board's SDL reports). The mapping to a consumer's
// own scheme belongs to the consumer - see player.html's PAD_TO_EJS.
//
// Opens its own WebSocket rather than relying on the shell forwarding events into
// the iframe, so an app works the same embedded in the desktop or opened by URL.
(function (global) {
  const listeners = { state: [], button: [], hat: [], axis: [], reconnecting: [] };
  const emit = (k, d) => { for (const f of listeners[k]) { try { f(d); } catch (e) { console.error('NucleoPad handler', e); } } };

  // This PAGE's identity for the pad session. The ws bus broadcasts every pad
  // event to every open page, and an IP can't tell two browsers on one pc
  // apart - so ownership is a client id: only the page that holds the session
  // consumes the input, everyone else sees the pad as 'busy'.
  let cid = '';
  try {
    cid = localStorage.getItem('nucleopad.cid') || '';
    if (!cid) {
      cid = 'c_' + Math.random().toString(36).slice(2, 10);
      localStorage.setItem('nucleopad.cid', cid);
    }
  } catch (e) { cid = 'c_' + Math.random().toString(36).slice(2, 10); }

  let ws = null, backoff = 1000, closed = false;
  const mineOrLegacy = (d) => !d || !d.cid || d.cid === cid;   // old server: no cid → keep working
  const asLocal = (d) => (d && d.state === 'active' && d.client && d.client !== cid)
    ? Object.assign({}, d, { state: 'busy' }) : d;             // someone ELSE holds it

  function connect() {
    if (closed) return;
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
    try {
      ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    } catch (e) { setTimeout(connect, backoff); return; }

    ws.onopen = () => { backoff = 1000; };
    ws.onmessage = (m) => {
      let msg; try { msg = JSON.parse(m.data); } catch (e) { return; }
      // the bus also carries 'sync' batches (see server.py ws_publish / the shell's contract)
      const events = msg.op === 'sync' ? (msg.events || []) : [msg];
      for (const ev of events) {
        if (!ev || !ev.t || !ev.d) continue;
        if (ev.t === 'pad.state') {
          const d = asLocal(ev.d);                  // another page's session reads as 'busy' here
          NucleoPad.state = d.state; NucleoPad.info = d;
          if (d.state === 'active') { pingOn(); reconnectStop(); }
          else if (d.state === 'idle') maybeReconnect(d.why || '');
          emit('state', d);
        }
        // Input goes ONLY to the owning page: without the cid filter every open
        // app was driven by the same presses at once.
        else if (ev.t === 'pad.btn')  { if (mineOrLegacy(ev.d)) emit('button', ev.d); }
        else if (ev.t === 'pad.hat')  { if (mineOrLegacy(ev.d)) emit('hat', ev.d); }
        else if (ev.t === 'pad.axis') { if (mineOrLegacy(ev.d)) emit('axis', ev.d); }
      }
    };
    ws.onclose = () => {
      ws = null;
      if (closed) return;
      setTimeout(connect, backoff);
      backoff = Math.min(backoff * 2, 15000);   // backed-off retry, no reconnect storm
    };
  }

  async function post(path, body) {
    const r = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {}),
      // release() runs from pagehide: without keepalive the browser cancels the
      // request as the page goes away and the device stays lent to a ghost until
      // its own ping timeout. These bodies are bytes - keepalive is free here.
      keepalive: true
    });
    return r.json();
  }

  // Liveness. release() on pagehide covers a tab CLOSING; it does not cover a
  // crash, a kill, or the wifi dropping - and then the device sits there lent to
  // nobody, screen dark, until someone finds it and holds SELECT+START. A borrowed
  // thing has to come back on its own if the borrower disappears.
  let pingT = null;
  function pingOn() {
    if (pingT) return;
    pingT = setInterval(() => {
      if (NucleoPad.state !== 'active') return;
      fetch('/api/pad/ping', { method: 'POST' }).catch(() => {});
    }, 10000);
  }

  // ---- reconnect ------------------------------------------------------------
  // A session can end for two very different kinds of reason and they must not be
  // treated the same:
  //   deliberate ('user', 'refused', 'self')  -> the human chose it. Reconnecting
  //       would shove the consent prompt back in their face the moment they hand
  //       the pad back. Never.
  //   involuntary ('gone', 'boot', 'timeout') -> the device's shell restarted, or
  //       the link went quiet. Nobody chose it, so ask for the pad back.
  // Bounded and polite: backoff, a hard cap, and only while the page is actually
  // being looked at - a background tab must never make the device prompt.
  const RECONNECT_WHY = new Set(['gone', 'boot', 'timeout']);
  let lastApp = null, lastWhy = '', reTries = 0, reT = null;

  function reconnectStop() { if (reT) { clearTimeout(reT); reT = null; } reTries = 0; }

  function maybeReconnect(why) {
    lastWhy = why;   // remembered so returning to the tab retries only involuntary drops
    if (!lastApp || !RECONNECT_WHY.has(why)) return;
    if (reT || reTries >= 4) return;
    const wait = Math.min(2000 * Math.pow(2, reTries), 20000);   // 2s, 4s, 8s, 16s
    reTries++;
    emit('reconnecting', { why, attempt: reTries, in: wait });
    reT = setTimeout(async () => {
      reT = null;
      if (NucleoPad.state === 'active') return reconnectStop();
      if (document.hidden) return maybeReconnect(why);   // not while nobody's watching
      try {
        const r = await NucleoPad.request(lastApp);
        if (!r || !r.ok) maybeReconnect(why);            // Core still down -> keep trying, backed off
      } catch (e) { maybeReconnect(why); }
    }, wait);
  }

  // Coming back to the tab is the moment to retry at once: the user is here, and
  // a prompt now is expected rather than a surprise. Also the moment to start
  // OVER when the four backed-off tries ran dry with nobody watching - but only
  // for involuntary drops: a refusal must stay refused.
  document.addEventListener('visibilitychange', () => {
    if (document.hidden || NucleoPad.state !== 'idle' || !lastApp) return;
    if (!RECONNECT_WHY.has(lastWhy)) return;
    if (reT) { clearTimeout(reT); reT = null; }
    reTries = 0;
    maybeReconnect(lastWhy);
  });

  const NucleoPad = {
    state: 'idle',
    info: null,

    async request(appName) {
      connect();
      lastApp = appName || 'app';   // remembered so an involuntary drop can ask again
      const r = await post('/api/pad/request', { app: lastApp, cid });
      if (r.state) { NucleoPad.state = r.state; emit('state', r); }
      return r;
    },

    async release() {
      reconnectStop();   // this one IS deliberate: don't chase it back
      lastApp = null;
      try { return await post('/api/pad/release', { cid }); }   // only the owner can hand it back
      finally { NucleoPad.state = 'idle'; }
    },

    onReconnecting(cb) { listeners.reconnecting.push(cb); return NucleoPad; },

    // Current truth from the device, for a page that just loaded into an
    // already-paired session (e.g. the player opening while the pad is live).
    async refresh() {
      try {
        const r = await fetch('/api/pad/state', { cache: 'no-store' });
        const s = asLocal(await r.json());    // active-for-someone-else = busy here
        NucleoPad.state = s.state; NucleoPad.info = s;
        if (s.state === 'active') pingOn();   // page loaded into a live session WE own
        emit('state', s);
        return s;
      } catch (e) { return null; }
    },

    onState (cb) { listeners.state.push(cb);  return NucleoPad; },
    onButton(cb) { listeners.button.push(cb); return NucleoPad; },
    onHat   (cb) { listeners.hat.push(cb);    return NucleoPad; },
    onAxis  (cb) { listeners.axis.push(cb);   return NucleoPad; },

    // Start listening without asking for anything (a page that only consumes an
    // already-active session, like the game player).
    listen() { connect(); return NucleoPad; },

    stop() { closed = true; if (ws) try { ws.close(); } catch (e) {} }
  };

  global.NucleoPad = NucleoPad;
})(window);
