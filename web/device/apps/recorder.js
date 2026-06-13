// Voice Recorder — device-simulator app. Mirrors firmware app_recorder.cpp.
// R/Space toggles recording (live timer + meter); the takes on the SD card show in a
// smartwatch focused list you can scroll and play back (Enter) — playback reuses the
// same WAV path the device plays through nucleo_audio. Recording is driven by /api/rec/*.
import { makeListState, drawFocusList, title } from './_list.js';
const lst = makeListState();
const player = new Audio();
player.preload = 'none';

const REC_DIR = '/data/Recordings';

let recording = false, seconds = 0, level = 0, list = [], sel = 0, playing = -1, poll = null, busy = false;

async function refreshStatus() {
  try {
    const s = await (await fetch('/api/rec/status')).json();
    const justStopped = recording && !s.recording;
    recording = s.recording; seconds = s.seconds || 0; level = s.level || 0;
    if (justStopped) await refreshList();
  } catch { /* offline: keep last state */ }
}
async function refreshList() {
  try {
    const r = await (await fetch('/api/fs/list?path=' + encodeURIComponent(REC_DIR))).json();
    list = (r.entries || []).filter(e => e.type === 'file' && /\.wav$/i.test(e.name))
      .map(e => ({ name: e.name, kb: Math.round((e.size || 0) / 1024) }))
      .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
    if (sel >= list.length) sel = Math.max(0, list.length - 1);
  } catch { list = []; }
}

async function toggleRec() {
  if (busy) return; busy = true;
  if (!recording) { player.pause(); playing = -1; }        // never record while playing
  try { await fetch('/api/rec/' + (recording ? 'stop' : 'start'), { method: 'POST' }); await refreshStatus(); }
  catch { /* ignore */ }
  busy = false;
}
function playSel() {
  if (recording || !list.length) return;                   // record XOR play
  player.src = '/api/fs/read?path=' + encodeURIComponent(REC_DIR + '/' + list[sel].name);
  player.currentTime = 0; player.play().catch(() => {}); playing = sel;
}

export const recorderApp = {
  id: 'recorder',

  enter() {
    recording = false; seconds = 0; level = 0; list = []; sel = 0; playing = -1;
    refreshStatus(); refreshList();
    poll = setInterval(refreshStatus, 300);
    return { hint: 'R rec/stop   enter play   ;/. pick   esc back' };
  },
  exit() { if (poll) { clearInterval(poll); poll = null; } player.pause(); },

  key(key, ch) {
    if (ch === 'r' || ch === ' ') return toggleRec();
    if (key === 'up') { if (list.length) sel = (sel + list.length - 1) % list.length; }
    else if (key === 'down') { if (list.length) sel = (sel + 1) % list.length; }
    else if (key === 'enter') playSel();
    else if (ch === 's') { player.pause(); playing = -1; }
  },

  draw(g) {
    const top = g.contentTop, h = g.contentH;
    const y0 = title(g, 'Voice Recorder', '#ff6b6b', recording ? null : `${list.length} take${list.length === 1 ? '' : 's'}`);

    if (recording) {
      const t = `${String((seconds / 60) | 0).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
      g.circle(18, y0 + 9, 5, '#ff5a5a');
      g.text('REC ' + t, 30, y0 + 9, '#ffffff', 14, 'bold');
      meter(g, y0 + 22, level);
      return { instruction: 'Recording to a WAV on the SD card', hint: 'R stop   esc back' };
    }

    if (!list.length) {
      g.text('No recordings yet', 12, y0 + 20, g.COL.dim, 9);
      g.text('Press R to record', 12, y0 + 36, '#7CFC9A', 9);
      return { instruction: 'Record the mic to a WAV on the SD card', hint: 'R record   esc back' };
    }
    drawFocusList(g, lst, {
      top: y0, h: top + h - y0, count: list.length, sel, now: g.now,
      label: i => list[i].name,
      right: i => list[i].kb + 'K',
      color: () => '#ff6b6b',
      marked: i => i === playing && !player.paused,
    });
    return { instruction: 'Record, or play back a take', hint: 'R rec   enter play   ;/. pick   esc back' };
  },
};

function meter(g, y, lv) {
  lv = Math.max(0, Math.min(100, lv));
  const col = lv > 80 ? '#ff5a5a' : lv > 40 ? '#ffd166' : '#7CFC9A';
  g.roundRect(10, y, 220, 8, 4, g.COL.line);
  if (lv > 0) g.roundRect(10, y, 220 * lv / 100, 8, 4, col);
}

// Test/inspection hook (preview harness; not part of the app contract).
export function _debugState() { return { recording, seconds, count: list.length, sel, playing, paused: player.paused }; }
