// nucleo-play.js — NucleoOS shared multiplayer transport (single source of truth).
//
// PRINCIPLE: the Cardputer never carries gameplay traffic. Browsers talk peer-to-peer over a
// WebRTC DataChannel; the device only helps them find each other. A "room" is a directory under
// /data/play/rooms/ and the WebRTC handshake (offer/answer/ICE) is exchanged through small files
// via the EXISTING /api/fs/* API — no new firmware, no flash. Once peers are connected the device
// is out of the loop entirely. If WebRTC can't connect (AP client isolation), we fall back to a
// poll-based relay over the same fs mailbox: slower, but keeps turn-based games working.
//
// Topology: STAR. One seat is the authoritative host; guests each open ONE DataChannel to the
// host, and the host relays between them. For <=4 players that's <=3 channels on the host — cheap,
// and it matches the host-authoritative netcode in nucleo-game.js.
//
//   import { NucleoPlay } from '/apps/games/nucleo-play.js';
//   const play = new NucleoPlay({ name:'Ada' });
//   const rooms = await play.listRooms();                 // lobby
//   await play.host({ gameId:'tris', name:"Ada's room", maxSeats:2 });   // create + become host
//   await play.join(roomId);                              // or join an existing room
//   play.on('peer',   ({seat, name}) => …);               // someone took a seat
//   play.on('left',   ({seat}) => …);
//   play.on('message',({from, data}) => …);               // game payloads
//   play.on('chat',   ({from, name, text}) => …);
//   play.send(data);            // broadcast a game message to the room (host relays)
//   play.chat('hi');            // small built-in chat, rides the same transport
//   play.leave();
//
// The message envelope on the wire is { t, ... }. Reserved types: hello, welcome, roster, chat,
// bye, and 'm' (opaque game payload). Everything else is the game's business, carried inside 'm'.

const FS = {
  // Thin wrappers over the device filesystem API. The OS shell is already paired, so the session
  // cookie rides along automatically (credentials:'same-origin').
  async write(path, str) {
    const r = await fetch('/api/fs/write?path=' + encodeURIComponent(path), {
      method: 'POST', credentials: 'same-origin',
      headers: { 'Content-Type': 'application/octet-stream' }, body: str,
    });
    return r.ok;
  },
  async read(path) {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(path), { credentials: 'same-origin' });
    if (!r.ok) return null;
    return await r.text();
  },
  async list(path) {
    const r = await fetch('/api/fs/list?path=' + encodeURIComponent(path), { credentials: 'same-origin' });
    if (!r.ok) return [];
    const j = await r.json().catch(() => null);
    // The fs API returns either an array or {entries:[…]} depending on endpoint version.
    if (!j) return [];
    return Array.isArray(j) ? j : (j.entries || j.items || []);
  },
  async readJSON(path) { const t = await FS.read(path); if (t == null) return null; try { return JSON.parse(t); } catch { return null; } },
  async writeJSON(path, obj) { return FS.write(path, JSON.stringify(obj)); },
  async remove(path) {
    // The firmware route is /api/fs/delete (it tries remove() then rmdir(), so it clears a file OR an
    // EMPTY dir). It is NOT recursive — callers must delete a directory's contents before the dir.
    try { await fetch('/api/fs/delete?path=' + encodeURIComponent(path), { method: 'POST', credentials: 'same-origin' }); } catch {}
  },
  async mkdir(path) {
    try { await fetch('/api/fs/mkdir?path=' + encodeURIComponent(path), { method: 'POST', credentials: 'same-origin' }); } catch {}
  },
};

const ROOMS_DIR = '/data/play/rooms';
const SIG_POLL_MS = 700;     // signaling/lobby poll cadence — low, because handshake is rare
const HEARTBEAT_MS = 4000;   // room liveness; stale rooms are hidden after STALE_MS
const STALE_MS = 15000;
const GC_STALE_MS = 60000;   // dead this long → reclaim the room's files from the SD. 4× STALE_MS so
                             // a slow heartbeat or modest client-clock skew can never delete a LIVE room.

// A short, human-friendly id. No Date.now()/Math.random reliance for correctness — these ids only
// need to be locally unique enough; collisions just mean "join a different room".
function rid(prefix) {
  const a = 'abcdefghjkmnpqrstuvwxyz23456789';
  let s = '';
  for (let i = 0; i < 6; i++) s += a[(Math.random() * a.length) | 0];
  return (prefix || '') + s;
}

export class NucleoPlay {
  constructor(opts = {}) {
    // Profile = the "gamercard": name + emoji avatar + accent colour, carried in the handshake so
    // every peer's roster/chat shows a consistent identity. opts.name kept for back-compat.
    this.profile = opts.profile || { name: opts.name || 'Player', avatar: '👾', color: '#22d3ee' };
    this.name = this.profile.name || 'Player';
    this.peerId = rid('p_');
    this.iceServers = opts.iceServers || [];   // empty = LAN host candidates only (offline-friendly)
    this.roomId = null;
    this.isHost = false;
    this.seat = -1;                  // our seat index (-1 = spectator / not yet seated)
    this.spectator = false;
    this.roster = [];                // [{seat, peerId, name, avatar, color, ai, spectator}]
    this.maxSeats = 2;
    this.gameId = null;
    this._wantSpectator = false;
    this._peers = new Map();         // peerId -> { pc, ch, seat, name, avatar, color, spectator, sigOut, seenIn }
    this._handlers = {};             // event -> [fn]
    this._timers = [];
    this._sigTimer = 0;              // signaling poll interval id (0 = not polling)
    this._gcd = new Set();           // room ids we've already fired a stale-GC for (don't re-spam)
    this._closed = false;
  }

  // ---- event bus -----------------------------------------------------------------------------
  on(ev, fn) { (this._handlers[ev] ||= []).push(fn); return this; }
  _emit(ev, payload) { (this._handlers[ev] || []).forEach(fn => { try { fn(payload); } catch (e) { console.error(e); } }); }

  // ---- lobby ---------------------------------------------------------------------------------
  async listRooms() {
    // mkdir ROOMS_DIR ONCE per session, not on every 4 s lobby tick: the dir is created on first
    // use and persists, so re-mkdir'ing it each poll was a wasted round-trip (and used to spam
    // fs.changed → a /data re-crawl on every client). FS.list creates nothing, so this is safe.
    if (!this._roomsDirReady) { try { await FS.mkdir(ROOMS_DIR); } catch (e) {} this._roomsDirReady = true; }
    const entries = await FS.list(ROOMS_DIR);
    const now = this._now();
    const out = [];
    for (const e of entries) {
      const id = e.name || e;
      const room = await FS.readJSON(`${ROOMS_DIR}/${id}/room.json`);
      if (!room) continue;
      const age = now - (room.seen || 0);
      if (age >= STALE_MS) {
        // Dead room: hide it now. Once it's dead well past any heartbeat/skew window, also reclaim its
        // files so /data/play/rooms doesn't accumulate orphans (the common case: host closed the tab).
        if (age >= GC_STALE_MS && !this._gcd.has(id)) { this._gcd.add(id); this._destroyRoom(id).catch(() => {}); }
        continue;
      }
      out.push(room);
    }
    return out;
  }

  // Fully reclaim a room directory. Because /api/fs/delete is non-recursive, we clear contents
  // bottom-up: the sig/ mailbox files, then sig/, then room.json (+ any stray), then the room dir.
  async _destroyRoom(roomId) {
    const dir = `${ROOMS_DIR}/${roomId}`;
    try {
      for (const f of await FS.list(`${dir}/sig`)) { const nm = f.name || f; if (nm) await FS.remove(`${dir}/sig/${nm}`); }
      await FS.remove(`${dir}/sig`);
      for (const f of await FS.list(dir)) { const nm = f.name || f; if (nm && nm !== 'sig') await FS.remove(`${dir}/${nm}`); }
      await FS.remove(dir);
    } catch {}
  }

  // Wall-clock for liveness only. We read it once per call; determinism is the game's concern, not ours.
  _now() { return new Date().getTime(); }

  // ---- host ----------------------------------------------------------------------------------
  async host({ gameId, name, maxSeats = 2 }) {
    this.gameId = gameId;
    this.maxSeats = Math.max(1, Math.min(4, maxSeats));
    this.roomId = rid('r_');
    this.isHost = true;
    this.seat = 0;
    this.roster = [{ seat: 0, peerId: this.peerId, name: this.profile.name, avatar: this.profile.avatar, color: this.profile.color, ai: false, spectator: false, ready: true }];

    await FS.mkdir(ROOMS_DIR);
    await FS.mkdir(`${ROOMS_DIR}/${this.roomId}`);
    await FS.mkdir(`${ROOMS_DIR}/${this.roomId}/sig`);
    await this._writeRoom(name || `${this.name}'s room`);

    this._startHeartbeat();
    this._startSignalPoll();     // host watches the sig mailbox for incoming guests
    this._emit('ready', { isHost: true, roomId: this.roomId, seat: 0 });
    this._emitRoster();
    return this.roomId;
  }

  async _writeRoom(displayName) {
    const players = this.roster.filter(r => r.seat >= 0);
    const room = {
      id: this.roomId, name: displayName, gameId: this.gameId,
      hostPeer: this.peerId, hostName: this.profile.name, hostAvatar: this.profile.avatar,
      maxSeats: this.maxSeats,
      seats: players.map(r => ({ seat: r.seat, name: r.name, avatar: r.avatar, ai: !!r.ai })),
      spectators: this.roster.filter(r => r.spectator).length,
      seen: this._now(),
    };
    this._roomMeta = room;
    await FS.writeJSON(`${ROOMS_DIR}/${this.roomId}/room.json`, room);
  }

  // ---- join ----------------------------------------------------------------------------------
  // opts.spectator: ask the host for a watch-only slot (no seat). The host also auto-spectates a
  // guest if the room's seats are full, so "join a full room" degrades to "watch" instead of failing.
  async join(roomId, opts = {}) {
    const room = await FS.readJSON(`${ROOMS_DIR}/${roomId}/room.json`);
    if (!room) throw new Error('room not found');
    this.roomId = roomId;
    this.gameId = room.gameId;
    this.maxSeats = room.maxSeats;
    this.isHost = false;
    this._wantSpectator = !!opts.spectator;

    // Open a WebRTC connection to the host. As the joiner we create the offer.
    const hostPeerId = room.hostPeer;
    this._mkPeer(hostPeerId, room.hostName, /*createOffer*/ true);
    this._startSignalPoll();   // guest watches its inbox for the host's answer + ICE

    this._emit('ready', { isHost: false, roomId });
    return true;
  }

  // ---- WebRTC peer setup ---------------------------------------------------------------------
  _mkPeer(remoteId, remoteName, createOffer) {
    const pc = new RTCPeerConnection({ iceServers: this.iceServers });
    const rec = { pc, ch: null, seat: -1, name: remoteName, sigOut: [], seenIn: 0, connected: false, iceQ: [], haveRemote: false };
    this._peers.set(remoteId, rec);

    pc.onicecandidate = (e) => {
      if (e.candidate) { rec.sigOut.push({ k: 'ice', c: e.candidate.toJSON() }); this._flushSig(remoteId); }
    };
    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'failed') this._emit('error', { kind: 'rtc', peer: remoteId });
    };

    const wireChannel = (ch) => {
      rec.ch = ch;
      ch.onopen = () => { rec.connected = true; this._onChannelOpen(remoteId); };
      ch.onclose = () => { this._onPeerGone(remoteId); };
      ch.onmessage = (e) => { this._onWire(remoteId, e.data); };
    };

    if (createOffer) {
      const ch = pc.createDataChannel('play', { ordered: true });
      wireChannel(ch);
      pc.createOffer().then(o => pc.setLocalDescription(o)).then(() => {
        rec.sigOut.push({ k: 'sdp', d: pc.localDescription.toJSON() });
        this._flushSig(remoteId);
      });
    } else {
      pc.ondatachannel = (e) => wireChannel(e.channel);
    }
    return rec;
  }

  // Signaling files are single-writer: each peer writes ONLY its own outbox, so no races.
  //   sig/<from>__<to>.json  = JSON array of {k,...} messages, append-only (we rewrite the array).
  _sigPath(from, to) { return `${ROOMS_DIR}/${this.roomId}/sig/${from}__${to}.json`; }

  async _flushSig(remoteId) {
    const rec = this._peers.get(remoteId);
    if (!rec) return;
    await FS.writeJSON(this._sigPath(this.peerId, remoteId), rec.sigOut);
  }

  _startSignalPoll() {
    if (this._sigTimer) return;
    this._sigTimer = setInterval(() => this._pollSignals().catch(() => {}), SIG_POLL_MS);
    this._timers.push(this._sigTimer);
    this._pollSignals().catch(() => {});
  }

  _stopSignalPoll() {
    if (!this._sigTimer) return;
    clearInterval(this._sigTimer);
    this._timers = this._timers.filter(t => t !== this._sigTimer);
    this._sigTimer = 0;
  }

  async _pollSignals() {
    if (this._closed || !this.roomId) return;
    if (this.isHost) {
      // Host discovers new joiners by scanning the sig dir for inbox files addressed to it.
      const files = await FS.list(`${ROOMS_DIR}/${this.roomId}/sig`);
      for (const f of files) {
        const nm = f.name || f;
        const m = /^([^_]+(?:_[^_]+)*)__(.+)\.json$/.exec(nm);
        if (!m) continue;
        const [, from, to] = m;
        if (to !== this.peerId) continue;
        if (this._isOurOwnId(from)) continue;
        if (!this._peers.has(from)) {
          // A new guest is knocking; accept as answerer. A full room still accepts watchers, so we
          // cap on TOTAL connections (players + spectators), not seats.
          if (this._peers.size >= this.maxSeats + 6) continue;
          this._mkPeer(from, 'Guest', /*createOffer*/ false);
        }
        await this._drainInbox(from);
      }
    } else {
      // Guest only talks to the host.
      const [host] = [...this._peers.keys()];
      if (host) await this._drainInbox(host);
    }
  }

  _isOurOwnId(id) { return id === this.peerId; }

  async _drainInbox(remoteId) {
    const rec = this._peers.get(remoteId);
    if (!rec) return;
    const arr = await FS.readJSON(this._sigPath(remoteId, this.peerId));
    if (!Array.isArray(arr)) return;
    for (let i = rec.seenIn; i < arr.length; i++) {
      await this._handleSig(remoteId, arr[i]);
    }
    rec.seenIn = arr.length;
  }

  async _handleSig(remoteId, msg) {
    const rec = this._peers.get(remoteId);
    if (!rec) return;
    const pc = rec.pc;
    if (msg.k === 'sdp') {
      const desc = msg.d;
      await pc.setRemoteDescription(desc);
      rec.haveRemote = true;
      // Flush any ICE candidates that arrived before the remote description was set.
      for (const c of rec.iceQ) { try { await pc.addIceCandidate(c); } catch {} }
      rec.iceQ = [];
      if (desc.type === 'offer') {
        const ans = await pc.createAnswer();
        await pc.setLocalDescription(ans);
        rec.sigOut.push({ k: 'sdp', d: pc.localDescription.toJSON() });
        await this._flushSig(remoteId);
      }
    } else if (msg.k === 'ice') {
      if (!rec.haveRemote) { rec.iceQ.push(msg.c); return; }   // buffer until remote desc is set
      try { await pc.addIceCandidate(msg.c); } catch {}
    }
  }

  // ---- channel lifecycle ---------------------------------------------------------------------
  // The HOST waits for the guest's 'hello' before seating it (so it knows the gamercard + whether
  // the guest wants to spectate). The GUEST announces itself the moment its channel opens.
  _onChannelOpen(remoteId) {
    if (this.isHost) return;
    this._wire(remoteId, {
      t: 'hello', name: this.profile.name, avatar: this.profile.avatar,
      color: this.profile.color, spectator: !!this._wantSpectator,
    });
    // Handshake complete: the guest now talks to the host over the DataChannel, so it stops polling
    // the fs signaling mailbox — no more /api/fs/read every SIG_POLL_MS for the rest of the match.
    // (The host keeps polling: it must still see new guests/spectators knocking.)
    this._stopSignalPoll();
  }

  _onHello(remoteId, msg) {
    const rec = this._peers.get(remoteId);
    if (!rec) return;
    rec.name = (msg.name || 'Guest').slice(0, 16);
    rec.avatar = msg.avatar || '👾';
    rec.color = msg.color || '#94a3b8';
    let seat = -1, spectator = !!msg.spectator;
    if (!spectator) { seat = this._nextFreeSeat(); if (seat < 0) spectator = true; }   // full → watch
    rec.seat = seat; rec.spectator = spectator;
    this.roster.push({ seat, peerId: remoteId, name: rec.name, avatar: rec.avatar, color: rec.color, ai: false, spectator, ready: false });
    this._wire(remoteId, {
      t: 'welcome', seat, spectator, gameId: this.gameId, maxSeats: this.maxSeats,
      host: { name: this.profile.name, avatar: this.profile.avatar }, roster: this._rosterWire(),
    });
    this._broadcastRoster();
    this._writeRoom(this._roomMeta?.name);
    this._emit('peer', { seat, peerId: remoteId, name: rec.name, avatar: rec.avatar, spectator });
  }

  _onPeerGone(remoteId) {
    const rec = this._peers.get(remoteId);
    if (!rec) return;
    const seat = rec.seat;
    this._peers.delete(remoteId);
    this.roster = this.roster.filter(r => r.peerId !== remoteId);
    if (this.isHost) { this._broadcastRoster(); this._writeRoom(this._roomMeta?.name); }
    this._emit('left', { seat, peerId: remoteId });
  }

  // ---- wire protocol -------------------------------------------------------------------------
  _wire(remoteId, obj) {
    const rec = this._peers.get(remoteId);
    if (rec && rec.ch && rec.ch.readyState === 'open') { try { rec.ch.send(JSON.stringify(obj)); } catch {} }
  }

  _onWire(remoteId, raw) {
    let obj; try { obj = JSON.parse(raw); } catch { return; }
    switch (obj.t) {
      case 'hello':                       // host only: a guest is announcing itself
        if (this.isHost) this._onHello(remoteId, obj);
        break;
      case 'welcome':
        this.seat = obj.seat; this.gameId = obj.gameId; this.spectator = !!obj.spectator;
        if (obj.maxSeats) this.maxSeats = obj.maxSeats;
        if (obj.roster) { this.roster = obj.roster; this._emit('roster', this.roster); }
        this._emit('ready', { isHost: false, roomId: this.roomId, seat: this.seat, spectator: this.spectator });
        break;
      case 'roster':
        this.roster = obj.roster; this._emit('roster', this.roster);
        break;
      case 'chat':
        // Host relays chat to everyone else; guests just surface it.
        if (this.isHost) this._relay(remoteId, obj);
        this._emit('chat', { from: obj.from, name: obj.name, avatar: obj.avatar, text: obj.text });
        break;
      case 'react':
        if (this.isHost) this._relay(remoteId, obj);
        this._emit('react', { from: obj.from, name: obj.name, avatar: obj.avatar, emoji: obj.emoji });
        break;
      case 'typing':
        if (this.isHost) this._relay(remoteId, obj);
        this._emit('typing', { from: obj.from, name: obj.name, on: !!obj.on });
        break;
      case 'phase':                       // host -> guests: drive the shared waiting/match screens
        this._phase = obj.phase; this._emit('phase', { phase: obj.phase });
        break;
      case 'ready':                       // host only: a guest toggled its ready state
        if (this.isHost) { const r = this.roster.find(x => x.peerId === remoteId); if (r) { r.ready = !!obj.on; this._broadcastRoster(); } }
        break;
      case 'm':
        if (this.isHost) this._relay(remoteId, obj);
        this._emit('message', { from: obj.from ?? this._seatOf(remoteId), data: obj.d });
        break;
      case 'bye':
        this._onPeerGone(remoteId);
        break;
    }
  }

  // Host fan-out: re-send an incoming message to every other connected peer.
  _relay(fromPeerId, obj) {
    for (const [pid, rec] of this._peers) {
      if (pid === fromPeerId) continue;
      if (rec.ch && rec.ch.readyState === 'open') { try { rec.ch.send(JSON.stringify(obj)); } catch {} }
    }
  }

  _rosterWire() { return this.roster.map(r => ({ seat: r.seat, name: r.name, avatar: r.avatar, color: r.color, ai: !!r.ai, spectator: !!r.spectator, ready: !!r.ready })); }
  _broadcastRoster() {
    const roster = this._rosterWire();
    this._emit('roster', roster);
    for (const pid of this._peers.keys()) this._wire(pid, { t: 'roster', roster });
  }
  _emitRoster() { this._emit('roster', this._rosterWire()); }

  // ---- public send / chat --------------------------------------------------------------------
  // Broadcast a game payload to the whole room. From a guest it goes to the host, who relays.
  send(data) {
    const env = { t: 'm', from: this.seat, d: data };
    if (this.isHost) { this._relay(null, env); this._emit('message', { from: this.seat, data }); }
    else { const [host] = [...this._peers.keys()]; if (host) this._wire(host, env); }
  }

  chat(text) {
    const env = { t: 'chat', from: this.seat, name: this.profile.name, avatar: this.profile.avatar, color: this.profile.color, text };
    if (this.isHost) { this._relay(null, env); }
    else { const [host] = [...this._peers.keys()]; if (host) this._wire(host, env); }
    this._emit('chat', { from: this.seat, name: this.profile.name, avatar: this.profile.avatar, text });
  }

  // Lightweight floating reaction (emoji over the board) — rides the same transport as chat.
  react(emoji) {
    const env = { t: 'react', from: this.seat, name: this.profile.name, avatar: this.profile.avatar, emoji };
    if (this.isHost) { this._relay(null, env); }
    else { const [host] = [...this._peers.keys()]; if (host) this._wire(host, env); }
    this._emit('react', { from: this.seat, name: this.profile.name, avatar: this.profile.avatar, emoji });
  }

  // Presence ping: "X is typing…". Sent on/off; the app debounces it.
  typing(on) {
    const env = { t: 'typing', from: this.seat, name: this.profile.name, on: !!on };
    if (this.isHost) { this._relay(null, env); }
    else { const [host] = [...this._peers.keys()]; if (host) this._wire(host, env); }
  }

  // Host-only: announce a fresh match so guests reset their view (used by rematch).
  announce(obj) { if (this.isHost) this._relay(null, obj); }

  // ---- session lifecycle (transversal: every game rides the same phases) ----------------------
  // Phase is host-authoritative and pushed to all peers, so the waiting-room / match / result
  // screens are driven once by the hub for ALL games. phase ∈ 'waiting' | 'playing'.
  setPhase(phase) {
    this._phase = phase;
    if (this.isHost) this._relay(null, { t: 'phase', phase });
    this._emit('phase', { phase });
  }
  get phase() { return this._phase || 'waiting'; }

  // Ready-up in the waiting room. Host start stays its own decision; ready is shown to everyone.
  setReady(on) {
    if (this.isHost) { const me = this.roster.find(r => r.peerId === this.peerId); if (me) { me.ready = !!on; this._broadcastRoster(); this._emit('roster', this._rosterWire()); } }
    else { const [h] = [...this._peers.keys()]; if (h) this._wire(h, { t: 'ready', on: !!on }); }
  }

  // Host can seat an AI in a free slot (filled by nucleo-game's ai() — typically ANIMA offline).
  addAI(name = 'ANIMA') {
    if (!this.isHost) return -1;
    const seat = this._nextFreeSeat();
    if (seat < 0) return -1;
    this.roster.push({ seat, peerId: 'ai_' + seat, name, avatar: '🤖', color: '#94a3b8', ai: true, spectator: false, ready: true });
    this._broadcastRoster();
    this._writeRoom(this._roomMeta?.name);
    this._emit('peer', { seat, peerId: 'ai_' + seat, name, avatar: '🤖', ai: true });
    return seat;
  }

  // ---- helpers -------------------------------------------------------------------------------
  _seatsTaken() { return this.roster.filter(r => r.seat >= 0).length; }   // players only (excludes spectators)
  _nextFreeSeat() {
    const used = new Set(this.roster.map(r => r.seat));
    for (let s = 0; s < this.maxSeats; s++) if (!used.has(s)) return s;
    return -1;
  }
  _seatOf(remoteId) { const r = this._peers.get(remoteId); return r ? r.seat : -1; }

  _startHeartbeat() {
    const t = setInterval(() => { if (this.isHost) this._writeRoom(this._roomMeta?.name).catch(() => {}); }, HEARTBEAT_MS);
    this._timers.push(t);
  }

  // ---- teardown ------------------------------------------------------------------------------
  async leave() {
    this._closed = true;
    // Tell peers we're going so they drop us from the roster immediately, instead of waiting for the
    // DataChannel teardown to surface as 'onclose'. Best-effort: only reaches already-open channels.
    for (const pid of this._peers.keys()) this._wire(pid, { t: 'bye' });
    const roomId = this.roomId, wasHost = this.isHost;
    const hostId = wasHost ? null : [...this._peers.keys()][0];
    for (const [, rec] of this._peers) { try { rec.ch && rec.ch.close(); rec.pc.close(); } catch {} }
    this._timers.forEach(t => clearInterval(t));
    this._timers = []; this._sigTimer = 0;
    if (roomId) {
      // The host owns the room and reclaims it fully; a guest only removes its own signaling outbox
      // (the host's inbox file to us is the host's to clean — it goes when the room is destroyed).
      if (wasHost) await this._destroyRoom(roomId);
      else if (hostId) await FS.remove(this._sigPath(this.peerId, hostId));
    }
    this._peers.clear();
    this._emit('closed', {});
  }
}

export { FS as _fs };
