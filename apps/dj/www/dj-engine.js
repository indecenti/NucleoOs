// dj-engine.js — Web Audio executor for the mix plans (the "two bodies, one
// brain" browser corpus). The browser, unlike the Cardputer's single I2S TX,
// can mix 2+ streams live, so here we do REAL-TIME crossfades. Web Audio nodes
// are our VST rack — they run in the browser's native audio thread:
//   BiquadFilter  -> EQ-kill / resonant sweeps (the "Pioneer mixer" sweep)
//   GainNode      -> crossfade + sidechain pumping
//   ConvolverNode -> reverb tail
//   DelayNode     -> echo tail
//   DynamicsCompressor -> glue comp + master limiter
//
// Per-deck graph: source -> eqLow -> eqMid -> eqHigh -> filter -> gain -> bus
// Master bus:     (deckA+deckB) -> glue -> limiter -> destination
//
// It consumes the plan objects from dj-planner.js. fx tags are realised below.

import { loadNpx } from './npx.js';

export class DjEngine {
  constructor(ctx) {
    this.ctx = ctx || new (window.AudioContext || window.webkitAudioContext)();
    const c = this.ctx;
    // master glue + brick-wall safety limiter (so A+B never clips the output)
    this.glue = c.createDynamicsCompressor();
    this.glue.threshold.value = -18; this.glue.ratio.value = 1.6;
    this.glue.attack.value = 0.02; this.glue.release.value = 0.25;
    this.limiter = c.createDynamicsCompressor();
    this.limiter.threshold.value = -1.5; this.limiter.ratio.value = 20;
    this.limiter.attack.value = 0.002; this.limiter.release.value = 0.1;
    // safety soft-clip: the compressor lets transients overshoot >0 dBFS; this
    // WaveShaper is identity below 0.7 and saturates softly above, so the bus
    // output can never clip even when two in-phase kicks sum during a beatmatch.
    this.safety = c.createWaveShaper();
    this.safety.curve = this._softClipCurve();
    this.glue.connect(this.limiter).connect(this.safety).connect(c.destination);
    this.decks = { a: this._mkDeck(), b: this._mkDeck() };
    this._reverbIR = null;
  }

  _softClipCurve() {
    const N = 1024, t = 0.7, curve = new Float32Array(N);
    for (let i = 0; i < N; i++) {
      const x = (i / (N - 1)) * 2 - 1, ax = Math.abs(x), s = Math.sign(x);
      curve[i] = ax <= t ? x : s * (t + (1 - t) * Math.tanh((ax - t) / (1 - t)));
    }
    return curve;
  }

  _mkDeck() {
    const c = this.ctx;
    const eqLow = c.createBiquadFilter(); eqLow.type = 'lowshelf'; eqLow.frequency.value = 150;
    const eqMid = c.createBiquadFilter(); eqMid.type = 'peaking'; eqMid.frequency.value = 1200; eqMid.Q.value = 0.8;
    const eqHigh = c.createBiquadFilter(); eqHigh.type = 'highshelf'; eqHigh.frequency.value = 4000;
    const filter = c.createBiquadFilter(); filter.type = 'allpass'; // bypass until a sweep arms it
    const gain = c.createGain(); gain.gain.value = 0;   // crossfade gain
    const scGain = c.createGain(); scGain.gain.value = 1; // sidechain / kick-roll gain (separate param!)
    eqLow.connect(eqMid).connect(eqHigh).connect(filter).connect(gain).connect(scGain).connect(this.glue);
    return { buffer: null, npx: null, source: null, startCtxTime: 0, startOffset: 0,
             rate: 1, eqLow, eqMid, eqHigh, filter, gain, scGain, playing: false };
  }

  // --- load a track into a deck: decode audio + parse its .npx ---
  async loadDeck(which, audioPath, fetchImpl = fetch) {
    const dk = this.decks[which];
    const r = await fetchImpl('/api/fs/read?path=' + encodeURIComponent(audioPath));
    if (!r.ok) throw new Error('audio fetch ' + r.status);
    const ab = await r.arrayBuffer();
    dk.buffer = await this.ctx.decodeAudioData(ab);
    dk.npx = await loadNpx(audioPath, fetchImpl);
    dk.path = audioPath;
    return dk;
  }

  // --- start a deck at offset seconds, optional pitch (vinyl beat-lock) ---
  play(which, offset = 0, rate = 1, gain = 1) {
    const dk = this.decks[which], c = this.ctx;
    this.stop(which);
    const src = c.createBufferSource();
    src.buffer = dk.buffer;
    src.playbackRate.value = rate;
    src.connect(dk.eqLow);
    src.start(0, Math.max(0, offset));
    dk.source = src; dk.rate = rate;
    dk.startCtxTime = c.currentTime; dk.startOffset = offset; dk.playing = true;
    dk.gain.gain.setValueAtTime(gain, c.currentTime);
    src.onended = () => { dk.playing = false; };
    return dk;
  }

  stop(which) {
    const dk = this.decks[which];
    if (dk.source) { try { dk.source.stop(); } catch {} dk.source = null; }
    dk.playing = false;
  }

  // position (track seconds) currently playing on a deck
  position(which) {
    const dk = this.decks[which];
    if (!dk.playing) return 0;
    return dk.startOffset + (this.ctx.currentTime - dk.startCtxTime) * dk.rate;
  }

  setMasterGain(_) { /* reserved */ }

  // --- execute a plan: A is playing, B is loaded; crossfade A->B per plan ---
  // Returns when the crossfade window has been *scheduled* (not awaited).
  mix(plan, fromDeck = 'a', toDeck = 'b') {
    const c = this.ctx, t0 = c.currentTime + 0.05;
    const A = this.decks[fromDeck], B = this.decks[toDeck];
    const xf = Math.max(0.05, plan.xfade || 4);
    const rate = plan.pitch && plan.pitch > 0 ? plan.pitch : 1;
    const offB = plan.mix_in_off || B.npx?.intro_sec || 0;

    // start B at its entry point, beat-locked via playbackRate
    this.play(toDeck, offB, rate, 0);

    // crossfade gains with the chosen curve
    this._curve(A.gain.gain, B.gain.gain, t0, xf, plan.curve || 'epow');

    // realise fx tags (our VST rack)
    const tags = (plan.fx || '').split('+').filter(Boolean);
    for (const tag of tags) this._fx(tag, A, B, t0, xf, plan);

    // stop A after the overlap
    const stopAt = t0 + xf + 0.1;
    try { A.source && A.source.stop(stopAt); } catch {}
    setTimeout(() => this.stop(fromDeck), (stopAt - c.currentTime) * 1000 + 60);
    return { startedAt: t0, endsAt: t0 + xf };
  }

  // crossfade gain shapes
  _curve(gA, gB, t0, xf, curve) {
    const c = this.ctx, N = 48;
    const a = new Float32Array(N), b = new Float32Array(N);
    for (let i = 0; i < N; i++) {
      const x = i / (N - 1);
      let fa, fb;
      if (curve === 'punch') { fb = Math.min(1, x * 1.8); fa = Math.max(0, 1 - x * 1.2); }
      else if (curve === 'fast') { fb = x; fa = 1 - x; }
      else { fa = Math.cos(x * Math.PI / 2); fb = Math.sin(x * Math.PI / 2); } // equal-power
      a[i] = fa; b[i] = fb;
    }
    gA.cancelScheduledValues(t0); gB.cancelScheduledValues(t0);
    gA.setValueCurveAtTime(a, t0, xf);
    gB.setValueCurveAtTime(b, t0, xf);
  }

  // map one fx tag to a Web Audio automation
  _fx(tag, A, B, t0, xf, plan) {
    const c = this.ctx;
    switch (tag) {
      case 'eq_swap': case 'sc_eq': {
        // 2-band kick swap: A's low shelf pulls the sub out as B's comes in.
        A.eqLow.gain.setValueAtTime(0, t0);
        A.eqLow.gain.linearRampToValueAtTime(-30, t0 + xf * 0.6);
        B.eqLow.gain.setValueAtTime(-24, t0);
        B.eqLow.gain.linearRampToValueAtTime(0, t0 + xf * 0.5);
        if (tag === 'sc_eq') this._sidechain(A, B, t0, xf, plan);
        break;
      }
      case 'lp_in': case 'lp_in_res': {
        // B enters behind a rising low-pass (resonant = the club sweep)
        B.filter.type = 'lowpass';
        B.filter.Q.value = tag === 'lp_in_res' ? 8 : 0.7;
        B.filter.frequency.setValueAtTime(250, t0);
        B.filter.frequency.exponentialRampToValueAtTime(18000, t0 + xf);
        break;
      }
      case 'hp_out': case 'hp_out_res': {
        // A leaves behind a rising high-pass (sweep it away)
        A.filter.type = 'highpass';
        A.filter.Q.value = tag === 'hp_out_res' ? 8 : 0.7;
        A.filter.frequency.setValueAtTime(30, t0);
        A.filter.frequency.exponentialRampToValueAtTime(6000, t0 + xf);
        break;
      }
      case 'sweep_up': {
        B.filter.type = 'lowpass'; B.filter.Q.value = 4;
        B.filter.frequency.setValueAtTime(400, t0);
        B.filter.frequency.exponentialRampToValueAtTime(16000, t0 + xf * 0.8);
        break;
      }
      case 'sweep_down': {
        A.filter.type = 'lowpass'; A.filter.Q.value = 2;
        A.filter.frequency.setValueAtTime(16000, t0);
        A.filter.frequency.exponentialRampToValueAtTime(300, t0 + xf);
        break;
      }
      case 'echo_tail': this._echo(A, t0, xf, plan); break;
      case 'reverb_tail': this._reverb(A, t0, xf); break;
      case 'comp_glue':
        this.glue.ratio.setValueAtTime(2.2, t0);
        this.glue.ratio.setValueAtTime(1.6, t0 + xf + 0.5);
        break;
      case 'impact': this._impact(t0); break;
      case 'kick_roll': this._kickRoll(B, t0, xf, plan); break;
      case 'tape_stop': this._tapeStop(A, t0, Math.min(1.2, xf)); break;
      case 'backspin': this._tapeStop(A, t0, Math.min(0.6, xf)); break; // approx
      default: break;
    }
  }

  // sidechain: duck A's level on every beat of B (the EDM pump). Acts on the
  // deck's SEPARATE scGain so it never collides with the crossfade curve on gain.
  _sidechain(A, B, t0, xf, plan) {
    const bpm = plan.eff_bpm_b || plan.bpm_b || (B.npx && B.npx.bpm) || 0;
    if (!bpm) return;
    const beat = 60 / bpm;
    const g = A.scGain.gain;
    g.setValueAtTime(1, t0);
    for (let t = t0; t < t0 + xf; t += beat) {
      g.setValueAtTime(0.35, t);                       // duck on the kick
      g.linearRampToValueAtTime(1.0, t + beat * 0.5);  // recover before next
    }
    g.setValueAtTime(1, t0 + xf);
  }

  _echo(A, t0, xf, plan) {
    const c = this.ctx;
    const delay = c.createDelay(1.0);
    const bpm = plan.bpm_a || (A.npx && A.npx.bpm) || 120;
    delay.delayTime.value = (60 / bpm) * 0.75; // dotted-eighth dub
    const fb = c.createGain(); fb.gain.value = 0.45;
    const wet = c.createGain(); wet.gain.value = 0;
    A.gain.connect(delay); delay.connect(fb).connect(delay); delay.connect(wet); wet.connect(this.glue);
    wet.gain.setValueAtTime(0, t0);
    wet.gain.linearRampToValueAtTime(0.5, t0 + xf * 0.5);
    wet.gain.linearRampToValueAtTime(0, t0 + xf + 1.5);
    setTimeout(() => { try { wet.disconnect(); delay.disconnect(); fb.disconnect(); } catch {} },
              (t0 + xf + 2 - c.currentTime) * 1000);
  }

  _reverb(A, t0, xf) {
    const c = this.ctx;
    if (!this._reverbIR) this._reverbIR = this._makeIR(2.0, 2.5);
    const conv = c.createConvolver(); conv.buffer = this._reverbIR;
    const wet = c.createGain(); wet.gain.value = 0;
    A.gain.connect(conv); conv.connect(wet); wet.connect(this.glue);
    wet.gain.setValueAtTime(0, t0);
    wet.gain.linearRampToValueAtTime(0.4, t0 + xf * 0.6);
    wet.gain.linearRampToValueAtTime(0, t0 + xf + 2.0);
    setTimeout(() => { try { wet.disconnect(); conv.disconnect(); } catch {} },
              (t0 + xf + 2.5 - c.currentTime) * 1000);
  }

  _makeIR(seconds, decay) {
    const c = this.ctx, rate = c.sampleRate, len = rate * seconds;
    const ir = c.createBuffer(2, len, rate);
    for (let ch = 0; ch < 2; ch++) {
      const d = ir.getChannelData(ch);
      for (let i = 0; i < len; i++) d[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / len, decay);
    }
    return ir;
  }

  // a short filtered noise "impact" hit (no sample needed)
  _impact(t0) {
    const c = this.ctx, len = c.sampleRate * 0.5;
    const buf = c.createBuffer(1, len, c.sampleRate);
    const d = buf.getChannelData(0);
    for (let i = 0; i < len; i++) d[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / len, 3);
    const src = c.createBufferSource(); src.buffer = buf;
    const lp = c.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 200;
    const g = c.createGain(); g.gain.value = 0.7;
    src.connect(lp).connect(g).connect(this.glue);
    src.start(t0);
    setTimeout(() => { try { g.disconnect(); lp.disconnect(); } catch {} }, 800);
  }

  // build a kick roll on B's entry (retriggered gain pulses speeding up). Uses
  // scGain so it doesn't collide with B's crossfade-in curve on gain.
  _kickRoll(B, t0, xf, plan) {
    const bpm = plan.eff_bpm_b || (B.npx && B.npx.bpm) || 150;
    const beat = 60 / bpm;
    const g = B.scGain.gain;
    g.setValueAtTime(1, t0);
    let t = t0, step = beat / 2, n = 0;
    while (t < t0 + Math.min(xf, beat * 4) && n < 16) {
      g.setValueAtTime(0.2, t);
      g.linearRampToValueAtTime(1.0, t + step * 0.4);
      t += step; step *= 0.85; n++; // accelerate
    }
    g.setValueAtTime(1, t + 0.01);
  }

  _tapeStop(A, t0, dur) {
    if (!A.source) return;
    const r = A.source.playbackRate;
    r.cancelScheduledValues(t0);
    r.setValueAtTime(A.rate || 1, t0);
    r.linearRampToValueAtTime(0.001, t0 + dur);
  }
}
