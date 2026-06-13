// Targeted tests for the cross-device multiplayer transport (apps/games/www/nucleo-play.js).
// No real WebRTC and no two browsers: peers are simulated with fake open DataChannels and the device
// filesystem API is an in-memory fetch stub that mirrors the firmware (delete is NON-recursive). This
// exercises the host-authoritative STAR relay, seating/roster, the fs signaling mailbox, room GC and
// teardown deterministically — the parts that actually decide whether two devices stay in sync.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
mkdirSync(join(here, '_t'), { recursive: true });

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };
const tick = () => new Promise(r => setTimeout(r, 25));   // let fire-and-forget fs chains settle

// ---- in-memory device fs behind /api/fs/* (delete = remove file OR empty dir, never recursive) ----
function installFs() {
  const files = new Map(), dirs = new Set(['/data', '/data/play', '/data/play/rooms']);
  const log = [];
  const norm = p => p.replace(/\/+$/, '');
  const ensure = p => { const a = norm(p).split('/'); let c = ''; for (let i = 1; i < a.length - 1; i++) { c += '/' + a[i]; dirs.add(c); } };
  const kids = dir => { dir = norm(dir); const pre = dir + '/', o = new Set(); for (const f of files.keys()) if (f.startsWith(pre)) o.add(f.slice(pre.length).split('/')[0]); for (const d of dirs) if (d.startsWith(pre)) { const r = d.slice(pre.length); if (r) o.add(r.split('/')[0]); } return [...o]; };
  globalThis.fetch = async (url, opts = {}) => {
    const u = new URL(url, 'http://d'), path = norm(decodeURIComponent(u.searchParams.get('path') || '')), route = u.pathname;
    log.push({ route, path });
    if (route === '/api/fs/write') { ensure(path); files.set(path, String(opts.body)); return { ok: true, async text() { return 'ok'; } }; }
    if (route === '/api/fs/read') return files.has(path) ? { ok: true, async text() { return files.get(path); } } : { ok: false, async text() { return ''; } };
    if (route === '/api/fs/list') { const n = kids(path); return { ok: true, async json() { return n.map(x => ({ name: x })); } }; }
    if (route === '/api/fs/mkdir') { dirs.add(path); return { ok: true, async text() { return 'ok'; } }; }
    if (route === '/api/fs/delete') { if (files.has(path)) files.delete(path); else if (dirs.has(path) && kids(path).length === 0) dirs.delete(path); return { ok: true, async text() { return 'ok'; } }; }
    return { ok: false, async text() { return ''; }, async json() { return null; } };
  };
  return { files, dirs, log };
}

// ---- WebRTC stub: enough for join()/_mkPeer to create + publish an offer without a browser ----
function installRTC() {
  globalThis.RTCPeerConnection = class {
    constructor() { this.localDescription = null; this.onicecandidate = null; this.onconnectionstatechange = null; this.ondatachannel = null; this.connectionState = 'new'; }
    createDataChannel() { return { readyState: 'connecting', send() {}, close() {}, set onopen(_) {}, set onclose(_) {}, set onmessage(_) {} }; }
    async createOffer() { return { type: 'offer', sdp: 'OFFER' }; }
    async createAnswer() { return { type: 'answer', sdp: 'ANSWER' }; }
    async setLocalDescription(d) { this.localDescription = { type: d.type, sdp: d.sdp, toJSON() { return { type: d.type, sdp: d.sdp }; } }; }
    async setRemoteDescription() {}
    async addIceCandidate() {}
    close() {}
  };
}

// a fake, already-open DataChannel that records the (parsed) frames it was asked to send
const mkCh = () => ({ readyState: 'open', sent: [], send(s) { this.sent.push(JSON.parse(s)); }, close() { this.readyState = 'closed'; } });

const fsState = installFs(); installRTC();
const src = readFileSync(join(root, 'apps/games/www/nucleo-play.js'), 'utf8');
writeFileSync(join(here, '_t', 'play.mjs'), src);
const { NucleoPlay, _fs: FS } = await import(pathToFileURL(join(here, '_t', 'play.mjs')).href);

// ---- 1. STAR relay / fan-out (the heart of host-authoritative cross-device play) -------------
{
  const host = new NucleoPlay({ profile: { name: 'Ada', avatar: '🦊', color: '#22d3ee' } });
  host.isHost = true; host.seat = 0; host.roomId = 'r_relay'; host.gameId = 'tris'; host.maxSeats = 3;
  const a = mkCh(), b = mkCh();
  host._peers.set('p_a', { ch: a, seat: 1, name: 'A' });
  host._peers.set('p_b', { ch: b, seat: 2, name: 'B' });
  let got = []; host.on('message', m => got.push(m));

  host.send({ mv: 7 });
  ok(a.sent.length === 1 && b.sent.length === 1 && a.sent[0].t === 'm' && a.sent[0].d.mv === 7, 'host.send fans out the move to every peer');
  ok(got.length === 1 && got[0].from === 0 && got[0].data.mv === 7, 'host.send surfaces the move locally (host seat)');

  a.sent.length = 0; b.sent.length = 0; got = [];
  host._onWire('p_a', JSON.stringify({ t: 'm', from: 1, d: { mv: 9 } }));
  ok(b.sent.length === 1 && b.sent[0].d.mv === 9 && a.sent.length === 0, 'host relays a guest move to OTHERS, never back to the sender');
  ok(got.length === 1 && got[0].from === 1 && got[0].data.mv === 9, 'host also surfaces the relayed move locally');

  b.readyState = 'closed'; a.sent.length = 0; b.sent.length = 0;
  host._relay(null, { t: 'm', from: 0, d: { x: 1 } });
  ok(a.sent.length === 1 && b.sent.length === 0, 'relay skips a closed channel (dropped peer never blocks the room)');
}

// ---- 2. guest send goes ONLY to the host (no echo, waits for the authoritative relay) --------
{
  const g = new NucleoPlay({ profile: { name: 'Bob' } });
  g.isHost = false; g.seat = 1; g.roomId = 'r_relay';
  const ch = mkCh(); g._peers.set('p_host', { ch, seat: 0 });
  let msgs = []; g.on('message', m => msgs.push(m));
  g.send({ mv: 3 });
  ok(ch.sent.length === 1 && ch.sent[0].t === 'm' && ch.sent[0].d.mv === 3, 'guest.send wires the move to the host only');
  ok(msgs.length === 0, 'guest.send does NOT echo locally (it waits for the host snapshot/relay)');
}

// ---- 3. chat/react relay vs local echo -------------------------------------------------------
{
  const h = new NucleoPlay({ profile: { name: 'Ada' } });
  h.isHost = true; h.seat = 0; h.roomId = 'r_chat';
  const a = mkCh(), b = mkCh(); h._peers.set('p_a', { ch: a, seat: 1 }); h._peers.set('p_b', { ch: b, seat: 2 });
  let chats = []; h.on('chat', c => chats.push(c));
  h._onWire('p_a', JSON.stringify({ t: 'chat', from: 1, name: 'A', avatar: '😀', text: 'ciao' }));
  ok(b.sent.length === 1 && b.sent[0].text === 'ciao' && a.sent.length === 0, 'host relays incoming chat to others, not the sender');
  ok(chats.length === 1 && chats[0].text === 'ciao', 'host surfaces the relayed chat');

  const g = new NucleoPlay({ profile: { name: 'Bob' } });
  g.isHost = false; g.seat = 1; const ch = mkCh(); g._peers.set('p_h', { ch, seat: 0 });
  let gchats = []; g.on('chat', c => gchats.push(c));
  g.chat('yo');
  ok(ch.sent.length === 1 && ch.sent[0].t === 'chat' && ch.sent[0].text === 'yo', 'guest.chat sends to the host');
  ok(gchats.length === 1 && gchats[0].text === 'yo', 'guest.chat echoes locally immediately (snappy UI)');
  const r = mkCh(); g._peers.set('p_h', { ch: r, seat: 0 }); let reacts = []; g.on('react', e => reacts.push(e));
  g.react('🔥');
  ok(r.sent[0].t === 'react' && r.sent[0].emoji === '🔥' && reacts.length === 1, 'react rides the same transport + echoes');
}

// ---- 4. seating: hello -> welcome, roster broadcast, full room degrades to spectator ---------
{
  const h = new NucleoPlay({ profile: { name: 'Ada', avatar: '🦊', color: '#111' } });
  h.isHost = true; h.seat = 0; h.roomId = 'r_seat'; h.gameId = 'tris'; h.maxSeats = 2; h._roomMeta = { name: 'room' };
  h.roster = [{ seat: 0, peerId: h.peerId, name: 'Ada', avatar: '🦊', color: '#111', ai: false, spectator: false, ready: true }];
  const bob = mkCh(); h._peers.set('p_bob', { ch: bob, seat: -1, name: 'Guest' });
  let peers = []; h.on('peer', p => peers.push(p));
  h._onHello('p_bob', { name: 'Bob', avatar: '🐼', color: '#222', spectator: false });
  const w = bob.sent.find(x => x.t === 'welcome');
  ok(w && w.seat === 1 && w.spectator === false, 'host seats the first guest at seat 1 and welcomes it');
  ok(w.roster.some(r => r.name === 'Bob' && r.seat === 1), 'welcome carries the updated roster');
  ok(h.roster.some(r => r.peerId === 'p_bob' && r.seat === 1), 'guest added to the host roster');
  ok(peers.length === 1 && peers[0].seat === 1, 'peer event fired with the assigned seat');

  const cara = mkCh(); h._peers.set('p_cara', { ch: cara, seat: -1, name: 'Guest' });
  h._onHello('p_cara', { name: 'Cara', spectator: false });
  const w2 = cara.sent.find(x => x.t === 'welcome');
  ok(w2 && w2.seat === -1 && w2.spectator === true, 'full room: an extra player gracefully degrades to spectator');
  ok(h._seatsTaken() === 2, 'spectators never consume a seat');

  const h2 = new NucleoPlay({ profile: { name: 'Ada' } });
  h2.isHost = true; h2.seat = 0; h2.roomId = 'r_seat2'; h2.maxSeats = 4; h2._roomMeta = { name: 'r' };
  h2.roster = [{ seat: 0, peerId: h2.peerId, name: 'Ada', spectator: false }];
  const dee = mkCh(); h2._peers.set('p_dee', { ch: dee, seat: -1, name: 'Guest' });
  h2._onHello('p_dee', { name: 'Dee', spectator: true });
  const wd = dee.sent.find(x => x.t === 'welcome');
  ok(wd && wd.seat === -1 && wd.spectator === true, 'explicit spectator gets no seat even when seats are free');
}

// ---- 5. seat allocation, AI seats, and departures --------------------------------------------
{
  const np = new NucleoPlay({}); np.maxSeats = 4;
  np.roster = [{ seat: 0 }, { seat: 2 }];
  ok(np._nextFreeSeat() === 1, 'next free seat fills the gap (1) rather than appending');
  np.roster = [{ seat: 0 }, { seat: 1 }, { seat: 2 }, { seat: 3 }];
  ok(np._nextFreeSeat() === -1, 'no free seat when the room is full');
  np.roster = [{ seat: 0 }, { seat: -1, spectator: true }, { seat: 1 }];
  ok(np._seatsTaken() === 2, 'seatsTaken counts players only');

  const h = new NucleoPlay({ profile: { name: 'Ada' } });
  h.isHost = true; h.seat = 0; h.roomId = 'r_gone'; h.maxSeats = 4; h._roomMeta = { name: 'r' };
  h.roster = [{ seat: 0, peerId: h.peerId }];
  ok(h.addAI('ANIMA') === 1 && h.roster.some(r => r.ai && r.seat === 1), 'addAI seats an AI in the next free slot');
  const x = mkCh(); h._peers.set('p_x', { ch: x, seat: 2 }); h.roster.push({ seat: 2, peerId: 'p_x' });
  const y = mkCh(); h._peers.set('p_y', { ch: y, seat: 3 }); h.roster.push({ seat: 3, peerId: 'p_y' });
  let left = []; h.on('left', e => left.push(e));
  h._onWire('p_x', JSON.stringify({ t: 'bye' }));
  ok(!h._peers.has('p_x') && !h.roster.some(r => r.peerId === 'p_x'), 'bye removes the departing peer from peers + roster');
  ok(left.length === 1 && left[0].seat === 2, 'left event reports the vacated seat');
  ok(y.sent.some(f => f.t === 'roster'), 'surviving peers receive a fresh roster after a departure');
}

// ---- 6. host() provisions the room on the device fs ------------------------------------------
{
  const h = new NucleoPlay({ profile: { name: 'Ada', avatar: '🦊', color: '#111' } });
  const id = await h.host({ gameId: 'pong', name: 'Arena', maxSeats: 2 });
  ok(typeof id === 'string' && id.startsWith('r_'), 'host() returns an r_ room id');
  const room = await FS.readJSON(`/data/play/rooms/${id}/room.json`);
  ok(room && room.gameId === 'pong' && room.maxSeats === 2 && room.hostName === 'Ada' && room.seats.length === 1, 'room.json carries gameId/maxSeats/host + the host seat');
  ok(fsState.dirs.has(`/data/play/rooms/${id}/sig`), 'host() creates the sig mailbox directory');
  h._timers.forEach(t => clearInterval(t)); h._closed = true;
}

// ---- 7. lobby freshness + room GC (time-controlled; anti clock-skew) -------------------------
{
  const lobby = new NucleoPlay({});
  const now = 1_000_000; lobby._now = () => now;
  await FS.writeJSON('/data/play/rooms/r_fresh/room.json', { id: 'r_fresh', name: 'fresh', seen: now - 1000 });
  await FS.writeJSON('/data/play/rooms/r_stale/room.json', { id: 'r_stale', name: 'stale', seen: now - 20000 });   // >STALE(15s) <GC(60s)
  await FS.mkdir('/data/play/rooms/r_dead/sig');
  await FS.writeJSON('/data/play/rooms/r_dead/sig/p_a__p_b.json', [{ k: 'ice' }]);
  await FS.writeJSON('/data/play/rooms/r_dead/room.json', { id: 'r_dead', name: 'dead', seen: now - 90000 });        // >GC(60s)
  const list = await lobby.listRooms();
  ok(list.some(r => r.id === 'r_fresh'), 'lobby shows a fresh room');
  ok(!list.some(r => r.id === 'r_stale') && !list.some(r => r.id === 'r_dead'), 'lobby hides stale + dead rooms');
  await tick();
  ok(!fsState.dirs.has('/data/play/rooms/r_dead') && !fsState.files.has('/data/play/rooms/r_dead/room.json'), 'a long-dead room is reclaimed from the SD (incl. its sig mailbox)');
  ok(fsState.files.has('/data/play/rooms/r_stale/room.json'), 'a merely-stale room is hidden but NOT deleted (skew margin protects live rooms)');
}

// ---- 8. _destroyRoom reclaims bottom-up via /api/fs/delete (the 405-bug + leak regression) ---
{
  await FS.mkdir('/data/play/rooms/r_d/sig');
  await FS.writeJSON('/data/play/rooms/r_d/room.json', { id: 'r_d' });
  await FS.writeJSON('/data/play/rooms/r_d/sig/p_a__p_b.json', [1]);
  await FS.writeJSON('/data/play/rooms/r_d/sig/p_b__p_a.json', [2]);
  fsState.log.length = 0;
  await new NucleoPlay({})._destroyRoom('r_d');
  const del = fsState.log.filter(c => c.route === '/api/fs/delete').map(c => c.path);
  const iSig = Math.max(del.indexOf('/data/play/rooms/r_d/sig/p_a__p_b.json'), del.indexOf('/data/play/rooms/r_d/sig/p_b__p_a.json'));
  const iSigDir = del.indexOf('/data/play/rooms/r_d/sig');
  const iRoomDir = del.indexOf('/data/play/rooms/r_d');
  ok(iSig >= 0 && iSigDir > iSig, 'destroyRoom deletes the sig FILES before the sig DIR');
  ok(iRoomDir === del.length - 1 && iRoomDir > iSigDir, 'destroyRoom deletes the room DIR last (non-recursive bottom-up)');
  ok(!fsState.dirs.has('/data/play/rooms/r_d'), 'the room directory is fully reclaimed');
  ok(!fsState.log.some(c => c.route === '/api/fs/remove'), 'never calls the non-existent /api/fs/remove route (the old 405 bug)');
}

// ---- 9. signaling mailbox: single-writer path, host inbox routing (ids contain underscores) --
{
  const sp = new NucleoPlay({}); sp.roomId = 'r_x';
  ok(sp._sigPath('p_aaa', 'p_bbb') === '/data/play/rooms/r_x/sig/p_aaa__p_bbb.json', 'sig path is <from>__<to>.json under the room sig dir (single-writer)');

  const h = new NucleoPlay({}); h.isHost = true; h.roomId = 'r_poll'; const me = h.peerId;
  await FS.mkdir('/data/play/rooms/r_poll/sig');
  await FS.writeJSON(`/data/play/rooms/r_poll/sig/p_guest__${me}.json`, [{ k: 'sdp' }]);   // addressed to us
  await FS.writeJSON('/data/play/rooms/r_poll/sig/p_guest__p_other.json', [{ k: 'sdp' }]); // addressed to someone else
  await FS.writeJSON(`/data/play/rooms/r_poll/sig/${me}__p_guest.json`, [{ k: 'sdp' }]);    // our OWN outbox
  const drained = []; h._drainInbox = async from => { drained.push(from); };
  h._mkPeer = from => { h._peers.set(from, { seenIn: 0 }); };   // no WebRTC in this unit
  await h._pollSignals();
  ok(drained.includes('p_guest'), 'host parses an inbox file from a peer id that itself contains "_"');
  ok(!drained.includes('p_other') && !drained.includes(me), 'host ignores files not addressed to it and its own outbox');
  ok(h._peers.has('p_guest'), 'host opens a peer for a new knocking guest');
}

// ---- 10. inbox drain is append-only + idempotent (no double-applying SDP/ICE across devices) -
{
  const d = new NucleoPlay({}); d.roomId = 'r_di'; d._peers.set('p_g', { seenIn: 0 });
  const handled = []; d._handleSig = async (_id, msg) => { handled.push(msg.k); };
  await FS.writeJSON(`/data/play/rooms/r_di/sig/p_g__${d.peerId}.json`, [{ k: 'sdp' }, { k: 'ice' }]);
  await d._drainInbox('p_g');
  ok(handled.length === 2 && d._peers.get('p_g').seenIn === 2, 'drainInbox processes all new entries + advances seenIn');
  await d._drainInbox('p_g');
  ok(handled.length === 2, 'drainInbox never reprocesses already-seen entries');
  await FS.writeJSON(`/data/play/rooms/r_di/sig/p_g__${d.peerId}.json`, [{ k: 'sdp' }, { k: 'ice' }, { k: 'ice' }]);
  await d._drainInbox('p_g');
  ok(handled.length === 3 && d._peers.get('p_g').seenIn === 3, 'drainInbox applies only the newly-appended candidate');
}

// ---- 11. join() reads the room contract and publishes its SDP offer to the host inbox --------
{
  await FS.mkdir('/data/play/rooms/r_join/sig');
  await FS.writeJSON('/data/play/rooms/r_join/room.json', { id: 'r_join', gameId: 'tris', maxSeats: 2, hostPeer: 'p_host', hostName: 'Ada' });
  const j = new NucleoPlay({ profile: { name: 'Bob' } });
  await j.join('r_join');
  ok(j.roomId === 'r_join' && j.gameId === 'tris' && j.maxSeats === 2 && !j.isHost, 'join() adopts the room contract (gameId/maxSeats)');
  ok(j._peers.has('p_host'), 'join() opens a peer channel toward the host');
  await tick();
  const outbox = await FS.readJSON(j._sigPath(j.peerId, 'p_host'));
  ok(Array.isArray(outbox) && outbox.some(m => m.k === 'sdp' && m.d.type === 'offer'), 'join() writes its SDP offer to the host inbox over the fs signaling channel');
  j._timers.forEach(t => clearInterval(t)); j._closed = true;
}

console.log(`\nMultiplayer transport: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
