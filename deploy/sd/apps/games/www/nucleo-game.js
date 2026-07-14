// nucleo-game.js — NucleoOS game SDK + host-authoritative netcode harness.
//
// A game is a small module that calls defineGame({...}) and never touches WebRTC. The harness
// (GameHarness) sits on top of NucleoPlay and runs the SAME contract for any game, turn-based or
// real-time. The HOST owns the authoritative state: it applies inputs through the pure reducer,
// ticks real-time games on a fixed step, fills empty/AI seats via ai(), and broadcasts state
// snapshots. GUESTS send inputs to the host and render whatever state arrives. Adding pong, chess,
// or a 4-player strategy game later = one defineGame() module; the netcode below is reused as-is.
//
// THE CONTRACT — defineGame({
//   id, name, minPlayers, maxPlayers,
//   aiCapable: bool,                                   // can ai() fill a seat?
//   realtime: bool,                                    // true -> tick() is called every step
//   tickHz: 20,                                        // step rate for realtime games
//   setup(players, seed) -> state,                     // host builds initial authoritative state
//   reduce(state, action, seat) -> state,              // PURE: apply one player's action
//   tick(state, dtMs) -> state,                        // PURE: advance realtime state (optional)
//   isOver(state) -> { winner } | null,                // null = still playing
//   render(api, state),                                // draw from state on every frame
//   onKey(key, api) -> action | null,                  // map a keypress to an action
//   onPointer(x, y, api) -> action | null,             // map a tap/click to an action (optional)
//   ai(state, seat) -> action | null,                  // choose an action for an AI/empty seat
//   aiThinkMs: 400,                                     // pacing so the AI feels human
// })
//
// 'players' passed to setup() is [{seat, name, ai}]. 'seed' is a deterministic integer so that
// realtime games stay in lockstep without streaming every frame.
//
//   import { GameHarness, defineGame, registerGame, getGame } from '/apps/games/nucleo-game.js';
//   const harness = new GameHarness(play, getGame(play.gameId), canvas);
//   harness.start();

import I18N from '/nucleo-i18n.js';   // centralized i18n; the games catalog is loaded by index.html's I18N.init('games')
const t = I18N.scope('games');

const REGISTRY = new Map();

export function defineGame(def) {
  if (!def || !def.id) throw new Error('game needs an id');
  const game = Object.assign({
    minPlayers: 1, maxPlayers: 2, aiCapable: false, realtime: false, tickHz: 20, aiThinkMs: 400,
    // options: declarative settings rendered generically by the waiting room. Each:
    //   { key, label, type:'select'|'toggle'|'number', choices?:[{value,label}], default, min?, max? }
    // Collected values are passed to setup(players, seed, options); store them in state for reduce/ai.
    options: [],
    setup: () => ({}), reduce: (s) => s, tick: (s) => s, isOver: () => null,
    render: () => {}, onKey: () => null, onPointer: () => null, ai: () => null,
    // Continuous input for real-time games (pointer drag, key release).
    onPointerMove: () => null, onKeyUp: () => null,
    // Gamepad/joystick (transversal). padMode: 'analog' (stick→onAxis) | 'cursor' (D-pad moves a
    // navigable cursor the game owns via api.cursor/api.setCursor; A confirms via onPadButton) | 'none'.
    padMode: 'none', onAxis: () => null, onPadDir: () => null, onPadButton: () => null,
    // Rendering surface. '2d' (default): the harness owns a 2D context (api.ctx) and clears it each
    // frame — every classic game uses this and is untouched. '3d'/'custom': the harness takes NO
    // context, hands the raw canvas to mount(canvas, api) once, and calls render(api, state) every
    // frame (state may be null → draw an idle/attract scene). The game owns WebGL and teardown.
    renderMode: '2d', mount: () => {}, unmount: () => {}, resize: () => {},
    // fillViewport: the app lets this game's canvas fill the whole board area (responsive backing
    // store) instead of the default centred fixed square. Real-time/3D games want the full screen.
    fillViewport: false,
    // capturesPointer: in immersive/fullscreen play the app should lock the mouse to the canvas
    // (relative movement), so the game feels native. Only for games steered by continuous pointer
    // motion (e.g. Pong's paddle); turn-based games that click cells must keep the cursor free.
    capturesPointer: false,
  }, def);
  REGISTRY.set(game.id, game);
  return game;
}
export function registerGame(def) { return defineGame(def); }
export function getGame(id) { return REGISTRY.get(id) || null; }
export function listGames() { return [...REGISTRY.values()].map(g => ({ id: g.id, name: g.name, minPlayers: g.minPlayers, maxPlayers: g.maxPlayers, aiCapable: g.aiCapable, realtime: g.realtime })); }

// A tiny deterministic PRNG (mulberry32) so games can be random yet reproducible across peers.
function mulberry32(seed) {
  let a = seed >>> 0;
  return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export class GameHarness {
  constructor(play, game, canvas, opts = {}) {
    if (!game) throw new Error('unknown game');
    this.play = play;
    this.game = game;
    this.canvas = canvas;
    // 3D/custom games manage their own (WebGL) context — never acquire a 2D one for them, since the
    // first getContext() call locks the canvas to that mode and the game's mount() needs WebGL.
    this.custom = game.renderMode === '3d' || game.renderMode === 'custom';
    this.ctx = this.custom ? null : canvas.getContext('2d');
    this._mounted = false;
    this._paused = false;
    this._brainDecide = null;   // optional async (state,seat,api)->action that REPLACES game.ai() for AI
    this.state = null;          // seats — used so an LLM can actually play turn-based games (no fallback)
    this.seq = 0;
    this._raf = 0;
    this._tickTimer = 0;
    this._aiTimers = [];
    this._running = false;
    this._lastTick = 0;
    this._onOver = opts.onOver || (() => {});
    this._onStatus = opts.onStatus || (() => {});
    this._onAiActed = opts.onAiActed || null;   // (seat, action, state) -> let the app make ANIMA banter
    this._onStart = opts.onStart || (() => {});

    // The drawing/query surface handed to game code. Keeps games ignorant of transport & DOM.
    this.api = {
      get width() { return canvas.width; },
      get height() { return canvas.height; },
      ctx: this.ctx,
      mySeat: () => this.play.seat,
      roster: () => this.play.roster,
      isHost: () => this.play.isHost,
      rng: () => (this._rng ? this._rng() : Math.random()),
      now: () => this._clock,
      // Per-client navigable cursor for gamepad/keyboard nav (opaque; the game defines its meaning).
      cursor: () => this._cursor,
      setCursor: (c) => { this._cursor = c; },
    };
    this._cursor = null;

    this._bind();
  }

  _bind() {
    // Guests receive authoritative snapshots; the host receives guests' inputs.
    this.play.on('message', ({ from, data }) => {
      if (!data || typeof data !== 'object') return;
      if (data.k === 'state') {
        // Guests follow authoritative snapshots. Mirror the host's match lifecycle locally so a guest also
        // gets the start reset and the single game-over callback (banner, stats, rematch UI): seq===1 marks
        // a fresh match; isOver fires exactly once thanks to the _over guard in _checkOver.
        if (!this.play.isHost) {
          const fresh = data.seq === 1;
          this.state = data.s; this.seq = data.seq;
          if (fresh) { this._over = false; this._onStart(this.play.roster.filter(r => r.seat >= 0)); }
          this._checkOver();
        }
      } else if (data.k === 'input') {
        if (this.play.isHost) this._applyInput(data.a, from);
      }
    });
    this.play.on('left', () => { /* host keeps state; a future version can pause */ });
  }

  // start() only runs the render loop. The MATCH is started explicitly by the host via begin(),
  // after the transversal waiting room. This keeps "gather players" and "play" cleanly separated
  // so every game shares the same lobby/waiting/result flow.
  start() {
    this._running = true;
    this._clock = 0;
    if (this.custom && !this._mounted) { try { this.game.mount(this.canvas, this.api); this._mounted = true; } catch (e) { console.error('mount failed', e); } }
    this._loop();
    return this;
  }

  // The app calls this when the canvas backing store changes size (e.g. fillViewport on window
  // resize). Forwarded to custom renderers so they can resize their viewport/camera.
  resize(w, h) { if (this.custom && this._mounted) { try { this.game.resize(w, h, this.api); } catch {} } }

  // Host begins the match with the options the waiting room collected.
  begin(options) { if (!this.play.isHost) return; this._options = options || this._options || {}; this._startMatch(); }
  rematch() { this._startMatch(); }
  // Freeze/unfreeze the realtime tick (host) without ending the match — used to BLOCK a Grok-mind game
  // when the LLM is unreachable, so the opponent never silently falls back to the local controller.
  pause() { this._paused = true; }
  resume() { this._paused = false; }
  isPaused() { return this._paused; }
  // LLM-as-player hook (turn-based): when installed, AI seats are decided by `fn` INSTEAD of game.ai().
  // fn returns the chosen action, or null to act later (e.g. not its turn, or the LLM is down → the
  // opponent simply doesn't move; there is NO fall back to the local heuristic). kickAI() re-triggers
  // a decision (used by the brain to retry after the model reconnects).
  installBrain(fn) { this._brainDecide = fn || null; }
  clearBrain() { this._brainDecide = null; }
  kickAI() { this._scheduleAI(); }
  _startMatch() {
    if (!this.play.isHost) return;
    this.state = null; this.seq = 0; this._cursor = null; this._paused = false; this._over = false; this._warned = {}; this._matchNo = (this._matchNo || 0) + 1;
    if (this._tickTimer) { clearInterval(this._tickTimer); this._tickTimer = 0; }
    this._maybeStart();
  }

  // Host boots the match once enough seats are filled (humans + AI). Guests wait for the first snapshot.
  _maybeStart() {
    if (!this.play.isHost || this.state) return;
    // Players are seated peers only — spectators (seat < 0) never count toward the start threshold
    // and are not handed to setup().
    const players = this.play.roster.filter(r => r.seat >= 0).sort((a, b) => a.seat - b.seat)
      .map(r => ({ seat: r.seat, name: r.name, ai: !!r.ai }));
    if (players.length < this.game.minPlayers) { this._onStatus(t('waiting_players', { have: players.length, need: this.game.minPlayers })); return; }
    const seed = (this._hashRoom() ^ ((this._matchNo || 1) * 2654435761)) >>> 0;
    this._rng = mulberry32(seed);
    this.state = this.game.setup(players, seed, this._options || {});
    this.seq = 1;
    this._broadcastState();
    this._onStatus('');
    this._onStart(players);
    if (this.game.realtime) this._startTick();
    this._scheduleAI();
  }

  _hashRoom() { const s = this.play.roomId || 'r'; let h = 2166136261; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }

  // ---- input -------------------------------------------------------------------------------
  // Called from the app for local keyboard/pointer. Host applies directly; guest ships to host.
  feedKey(key) {
    const action = this.game.onKey(key, this.api);
    if (action == null) return;
    this._submit(action);
  }
  feedPointer(x, y) {
    const action = this.game.onPointer(x, y, this.api);
    if (action == null) return;
    this._submit(action);
  }
  feedPointerMove(x, y) {
    const action = this.game.onPointerMove(x, y, this.api);
    if (action == null) return;
    this._submit(action);
  }
  feedKeyUp(key) {
    const action = this.game.onKeyUp(key, this.api);
    if (action == null) return;
    this._submit(action);
  }
  // Gamepad input. Analog axis (real-time), discrete cursor step, and confirm button.
  feedAxis(x, y) { const a = this.game.onAxis(x, y, this.api); if (a != null) this._submit(a); }
  feedPadDir(dx, dy) { const a = this.game.onPadDir(dx, dy, this.api); if (a != null) this._submit(a); }
  feedPadButton(name) { const a = this.game.onPadButton(name, this.api); if (a != null) this._submit(a); }
  // Host-originated action (not tied to a seat's turn) — used to inject AI policy / coaching.
  hostAct(action) { if (this.play.isHost) this._applyInput(action, -1); }
  _submit(action) {
    if (this.play.isHost) this._applyInput(action, this.play.seat);
    else this.play.send({ k: 'input', a: action });
  }

  _applyInput(action, seat) {
    // Once the match is decided, late inputs (a slow guest, a queued AI move, stopCoach's brain-clear)
    // are ignored — no post-match churn, and a second independent guard against the over->act->over loop.
    if (!this.play.isHost || !this.state || this._over) return;
    let next;
    try { next = this.game.reduce(this.state, action, seat); }
    catch (e) { this._warn('reduce', '[game] reduce threw — action dropped', e); return; }  // one bad action never freezes the host
    if (next && next !== this.state) { this.state = next; this.seq++; this._broadcastState(); this._checkOver(); this._scheduleAI(); }
  }

  // ---- realtime tick (host only) -----------------------------------------------------------
  _startTick() {
    const stepMs = Math.max(16, Math.round(1000 / (this.game.tickHz || 20)));
    this._lastTick = this._clock;
    this._tickTimer = setInterval(() => {
      if (!this.state || this._paused || this._over) return;   // paused = blocked (e.g. Grok mind offline); over = match done
      let next;
      try { next = this.game.tick(this.state, stepMs); }
      catch (e) { this._warn('tick', '[game] tick threw — step skipped', e); return; }  // skip the bad step; never spam, never hard-freeze
      if (next) { this.state = next; this.seq++; this._broadcastState(); this._checkOver(); }
    }, stepMs);
  }

  // ---- AI seats (host only) ----------------------------------------------------------------
  // Empty/AI seats are driven by the game's ai(). For ANIMA-backed opponents a game's ai() can be
  // async and call os.anima(); here we just pace it so it doesn't feel instant.
  _scheduleAI() {
    if (!this.play.isHost || !this.game.aiCapable || !this.state || this._over) return;
    this._aiTimers.forEach(t => clearTimeout(t)); this._aiTimers = [];
    for (const r of this.play.roster) {
      if (!r.ai) continue;
      const seat = r.seat;
      const t = setTimeout(async () => {
        if (!this.state) return;
        let action = null;
        // A brain (LLM) overrides the local heuristic for AI seats — turn-based games let it truly play.
        try { action = await (this._brainDecide ? this._brainDecide(this.state, seat, this.api) : this.game.ai(this.state, seat, this.api)); } catch {}
        if (action != null) { this._applyInput(action, seat); if (this._onAiActed) { try { this._onAiActed(seat, action, this.state); } catch {} } }
      }, this.game.aiThinkMs || 400);
      this._aiTimers.push(t);
    }
  }

  _broadcastState() { this.play.send({ k: 'state', s: this.state, seq: this.seq }); }
  // Re-send the current state (host) — used when a late peer joins a match in progress.
  resync() { if (this.play.isHost && this.state) this._broadcastState(); }

  // Rate-limited error log: a buggy game callback must never spam the console (which itself drags the
  // page). Surface the first few occurrences per match, then stay quiet; reset each match in _startMatch.
  _warn(key, ...a) { if (!this._warned) this._warned = {}; this._warned[key] = (this._warned[key] || 0) + 1; if (this._warned[key] <= 3) console.error(...a); }

  _checkOver() {
    // Over fires exactly once per match. The over-handler can inject host actions (e.g. stopCoach ->
    // hostAct{brain:null}) that loop back through _applyInput -> _checkOver; flip the flag and stop the
    // tick BEFORE calling _onOver so any reentrant check returns here instead of re-firing (stack overflow).
    if (this._over || !this.state) return;
    let over = null;
    try { over = this.game.isOver(this.state); }
    catch (e) { this._warn('isOver', '[game] isOver threw — treated as not over', e); return; }
    if (!over) return;
    this._over = true;
    if (this._tickTimer) { clearInterval(this._tickTimer); this._tickTimer = 0; }
    this._onOver(over);
  }

  // ---- render loop -------------------------------------------------------------------------
  _loop() {
    const frame = (ts) => {
      if (!this._running) return;
      this._clock = ts || 0;
      if (this.custom) {
        // Custom/3D renderer: draw every frame regardless of state (idle attract scene before the
        // first snapshot, smooth interpolation/effects between snapshots). The game owns the surface.
        try { this.game.render(this.api, this.state); } catch (e) { this._warn('render', '[game] render threw', e); }
      } else {
        const ctx = this.ctx;
        ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
        if (this.state) { try { this.game.render(this.api, this.state); } catch (e) { console.error(e); } }
      }
      this._raf = requestAnimationFrame(frame);
    };
    this._raf = requestAnimationFrame(frame);
  }

  stop() {
    this._running = false;
    if (this._raf) cancelAnimationFrame(this._raf);
    this._raf = 0;
    if (this._tickTimer) clearInterval(this._tickTimer);
    this._tickTimer = 0;
    this._aiTimers.forEach(t => clearTimeout(t));
    this._aiTimers = [];
    this._brainDecide = null;
    if (this.custom && this._mounted) { try { this.game.unmount(this.api); } catch {} this._mounted = false; }
  }
}
