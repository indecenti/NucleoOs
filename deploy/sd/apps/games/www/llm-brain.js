// llm-brain.js — the GAME-AGNOSTIC LLM brain for the Game Center. One controller that lets an online
// model (Grok/Claude/Groq) drive ANY game's AI seat, in two modes, with a strict no-silent-fallback rule:
//
//   · mover  (turn-based, e.g. Tris / Forza 4): the model LITERALLY chooses each move. The game supplies
//       def.llm.prompt(state,seat,api) -> {system,user}|null   (null = not its turn / game over)
//       def.llm.parse(text,state,seat)  -> {action,taunt?,persona?}|null   (null = illegal/garbage → retry)
//     The brain installs itself on the harness, so the local heuristic ai() is BYPASSED entirely.
//
//   · coach  (realtime, e.g. Pong): the model can't move a paddle at 60fps, so it sets the STYLE between
//       points via the existing def.coach.brief()/toParams(); a local controller executes that style.
//
// Honesty by construction: every call updates the authoritative state.brain ({status,label,ms}) so the
// HUD shows green ONLY on a confirmed response and red when down. When the model is unreachable the game
// is BLOCKED — coach pauses the match; mover simply doesn't move — never a hidden offline opponent.
//
// To make a NEW game LLM-playable: add `llm:{prompt,parse}` (turn-based) or `coach:{brief,toParams}`
// (realtime) to its defineGame() — nothing here changes. That's the scaling story.
import * as LLM from '/apps/games/llm.js';
import I18N from '/nucleo-i18n.js';   // centralized i18n; the games catalog is loaded by index.html's I18N.init('games')
const t = I18N.scope('games');

// Does this game support an LLM brain at all, and in which mode?
export function brainMode(def) {
  if (!def) return null;
  if (def.realtime && def.coach) return 'coach';
  if (def.llm && typeof def.llm.prompt === 'function') return 'mover';
  return null;
}
export function llmSelected(opts) { return !!(opts && (opts.mente === 'llm' || opts.llm === true)); }

// One real round-trip to verify the model BEFORE a match starts. ui.note/ok/fail report to the app.
// Returns the resolved cfg on success, or null (app then blocks the Grok match — no offline substitute).
export async function preflightBrain(ui) {
  const cfg = await LLM.loadCfg();
  if (!cfg) { ui.fail(t('err_no_key')); return null; }
  const brand = LLM.brandOf(cfg);
  ui.note(t('verifying_conn', { provider: LLM.providerLabel(cfg), host: LLM.endpointHost(cfg) }));
  const t0 = performance.now();
  const txt = await LLM.ask(cfg, 'Connectivity test. Reply with a single word: OK.', 'ping', { maxTokens: 5 });
  const ms = Math.round(performance.now() - t0);
  if (!txt) { ui.fail(t('brand_no_reply', { brand })); return null; }
  ui.ok(cfg, ms, brand);
  return cfg;
}

function extractJson(text) { if (!text) return null; const a = text.indexOf('{'), b = text.lastIndexOf('}'); if (a < 0 || b <= a) return null; try { return JSON.parse(text.slice(a, b + 1)); } catch { return null; } }

export class Brain {
  // ui: { sys(text), taunt(text), note(text), ok(cfg,ms,brand), fail(reason) }
  constructor({ harness, def, play, ui, cfg }) {
    this.harness = harness; this.def = def; this.play = play; this.ui = ui; this.cfg = cfg;
    this.label = cfg ? LLM.providerLabel(cfg) : 'LLM';
    this._alive = false; this._busy = false; this._poll = 0; this._retry = 0; this._lastScore = ''; this._lastRun = 0;
  }

  start(opts) {
    this.stop();
    if (!this.play.isHost) return;
    if (!llmSelected(opts)) { this._setBrain(null); return; }       // classic → no brain badge
    if (!(this.play.roster || []).some(r => r.ai)) return;          // no AI seat to drive
    this.mode = brainMode(this.def);
    if (!this.mode) { this.ui.sys(t('game_no_llm')); return; }
    this.aiSeats = (this.play.roster || []).filter(r => r.ai).map(r => r.seat);
    this._alive = true;
    this._setBrain('connecting');
    if (this.mode === 'coach') this._startCoach(); else this._startMover();
  }

  stop() {
    this._alive = false;
    if (this._poll) { clearInterval(this._poll); this._poll = 0; }
    if (this._retry) { clearTimeout(this._retry); this._retry = 0; }
    if (this.harness) this.harness.clearBrain();
    this._setBrain(null);
  }

  _setBrain(status, ms) { if (this.harness && this.play.isHost && this.harness.state) this.harness.hostAct({ type: 'brain', brain: status ? { status, label: this.label, ms: ms || 0, mode: this.mode } : null }); }
  _down(reason, pause) {
    this._setBrain('down');
    this.ui.sys(t('brain_down', { label: this.label, reason }));
  }

  // ── mover (turn-based): the model picks each move ────────────────────────────────────────────
  _startMover() {
    this.harness.installBrain((state, seat, api) => this._decide(state, seat, api));
    this.harness.kickAI();   // in case it's already the AI's turn
  }
  async _decide(state, seat, api) {
    if (!this._alive || this._busy) return null;
    const req = this.def.llm.prompt(state, seat, api);
    if (!req) return null;                              // not this seat's turn / game over
    this._busy = true;
    try {
      this._setBrain('connecting');
      const t0 = performance.now();
      const txt = await LLM.ask(this.cfg, req.system, req.user, { maxTokens: req.maxTokens || 120 });
      const ms = Math.round(performance.now() - t0);
      if (!txt) { this._down(t('reason_no_reply')); this._scheduleRetry(); return null; }
      const out = this.def.llm.parse(txt, state, seat);
      if (!out || out.action == null) { this._down(t('reason_bad_move')); this._scheduleRetry(); return null; }
      this._setBrain('live', ms);
      if (out.taunt) this.ui.taunt(out.taunt);
      if (out.persona != null && this.harness.state) this.harness.hostAct({ type: 'policy', seat, params: {}, persona: out.persona });
      return out.action;                                // ← Grok's actual move
    } catch { this._down(t('reason_net_error')); this._scheduleRetry(); return null; }
    finally { this._busy = false; }
  }
  _scheduleRetry() { clearTimeout(this._retry); this._retry = setTimeout(() => { if (this._alive && this.harness && this.harness.state) this.harness.kickAI(); }, 2200); }

  // ── coach (realtime): the model tunes STYLE between points; a local controller executes it ───
  _startCoach() {
    const run = async () => {
      if (!this._alive || this._busy || !this.harness || !this.harness.state) return; this._busy = true;
      try {
        let okAny = false, lastMs = 0;
        for (const seat of this.aiSeats) {
          const { system, user } = this.def.coach.brief(this.harness.state, seat);
          const t0 = performance.now();
          const txt = await LLM.ask(this.cfg, system, user, { maxTokens: 200 });
          lastMs = Math.round(performance.now() - t0);
          if (txt) { okAny = true; const style = extractJson(txt); if (style) { const { params, taunt, persona } = this.def.coach.toParams(style, seat); this.harness.hostAct({ type: 'policy', seat, params, persona }); if (taunt) this.ui.taunt(taunt); } }
        }
        if (okAny) { this._setBrain('live', lastMs); if (this.harness.isPaused()) { this.harness.resume(); this.ui.sys(t('coach_reconnected', { label: this.label })); } }
        else throw new Error('no-response');
      } catch {
        this._setBrain('down');
        if (!this.harness.isPaused()) { this.harness.pause(); this.ui.sys(t('coach_paused', { label: this.label })); }
      } finally { this._busy = false; }
    };
    (async () => {
      await run();
      this._lastRun = performance.now();
      this._poll = setInterval(() => {
        if (!this._alive || !this.harness || !this.harness.state) return;
        const sc = (this.harness.state.scores || []).join('-'), now = performance.now();
        if (this.harness.isPaused() || sc !== this._lastScore || now - this._lastRun > 14000) { this._lastScore = sc; this._lastRun = now; run(); }
      }, 600);
    })();
  }
}
