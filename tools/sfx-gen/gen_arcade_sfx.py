#!/usr/bin/env python3
# Arcade SFX generator for NucleoOS games.
#
# WHY this exists: playing a ready-made WAV off the SD costs the device NOTHING extra
# (the audio path streams PCM straight from disk), while the on-device notify-synth burns
# CPU and is limited to a shared sine+octave "carillon" timbre. So instead of bell tones we
# bake a proper arcade pack here on the PC -- square/saw/triangle/noise, pitch sweeps, vibrato,
# arpeggios and a touch of bit-crush -- the real sfxr/chiptune toolbox. License-clean (we synth
# it ourselves), tailored to each game's events, and a drop-in: the firmware already prefers an
# existing <name>.wav over synthesis.
#
# Output: 16-bit PCM MONO WAV. The player reads the rate from the header, so any rate is fine;
# 22050 Hz balances crispness and size. Names MUST match the game's sfx_name() table.
#
#   python gen_arcade_sfx.py --game pinball --out ../../deploy/sd/data/pinball/pack
#
# Pure standard library (no numpy) so it runs anywhere Python 3 does.

import math, struct, os, argparse, random

RATE = 22050

# ----------------------------------------------------------------------------- core synth
def _wave(kind, ph, duty):
    p = ph - math.floor(ph)
    if kind == 'square': return 1.0 if p < duty else -1.0
    if kind == 'saw':    return 2.0 * p - 1.0
    if kind == 'tri':    return (4.0 * p - 1.0) if p < 0.5 else (3.0 - 4.0 * p)
    if kind == 'sine':   return math.sin(2.0 * math.pi * p)
    if kind == 'noise':  return random.uniform(-1.0, 1.0)
    return 0.0

def tone(kind, f0, f1, dur, duty=0.5, vib_hz=0.0, vib=0.0, amp=1.0):
    """One oscillator. f0->f1 glides exponentially over dur; optional vibrato."""
    n = max(1, int(dur * RATE))
    out = [0.0] * n
    ph = 0.0
    lf0, lf1 = math.log(max(1.0, f0)), math.log(max(1.0, f1))
    for i in range(n):
        t = i / RATE
        frac = i / n
        f = math.exp(lf0 + (lf1 - lf0) * frac)
        if vib_hz > 0.0:
            f *= 1.0 + vib * math.sin(2.0 * math.pi * vib_hz * t)
        ph += f / RATE
        out[i] = amp * _wave(kind, ph, duty)
    return out

def env(buf, atk=0.004, decay=9.0, hold=0.0):
    """Linear attack, exponential decay (hold delays the decay)."""
    n = len(buf); a = max(1, int(atk * RATE)); h = int(hold * RATE)
    for i in range(n):
        if i < a:            e = i / a
        elif i < a + h:      e = 1.0
        else:                e = math.exp(-((i - a - h) / RATE) * decay)
        buf[i] *= e
    return buf

def mix(*bufs):
    n = max(len(b) for b in bufs)
    out = [0.0] * n
    for b in bufs:
        for i in range(len(b)): out[i] += b[i]
    return out

def seq(parts, gap=0.0):
    """Concatenate buffers (arpeggio), optional silence between."""
    g = [0.0] * int(gap * RATE)
    out = []
    for i, p in enumerate(parts):
        out += p
        if gap and i < len(parts) - 1: out += g
    return out

def crush(buf, step=3, bits=8):
    """Sample-hold downsample + amplitude quantize = retro grit."""
    if step > 1:
        for i in range(len(buf)):
            if i % step: buf[i] = buf[i - (i % step)]
    if bits < 16:
        q = 2 ** (bits - 1)
        for i in range(len(buf)):
            buf[i] = round(buf[i] * q) / q
    return buf

def arp(kind, notes, ndur, duty=0.5, sparkle=False):
    parts = []
    for f in notes:
        b = env(tone(kind, f, f, ndur, duty=duty), atk=0.003, decay=11.0)
        if sparkle:
            b = mix(b, env(tone('sine', f * 4, f * 4, ndur, amp=0.18), atk=0.002, decay=16.0))
        parts.append(b)
    return seq(parts)

def write_wav(path, buf, rate=RATE, peak=0.86):
    pk = max(1e-6, max(abs(s) for s in buf))
    g = peak / pk
    frames = bytearray()
    for s in buf:
        v = int(max(-1.0, min(1.0, s * g)) * 32767)
        frames += struct.pack('<h', v)
    dl = len(frames)
    with open(path, 'wb') as f:
        f.write(b'RIFF'); f.write(struct.pack('<I', 36 + dl)); f.write(b'WAVE')
        f.write(b'fmt '); f.write(struct.pack('<I', 16)); f.write(struct.pack('<H', 1))
        f.write(struct.pack('<H', 1)); f.write(struct.pack('<I', rate))
        f.write(struct.pack('<I', rate * 2)); f.write(struct.pack('<H', 2)); f.write(struct.pack('<H', 16))
        f.write(b'data'); f.write(struct.pack('<I', dl)); f.write(frames)

# ----------------------------------------------------------------------------- note table
C5, D5, E5, G5, A5 = 523.25, 587.33, 659.25, 783.99, 880.0
B5, C6, D6, E6, Fs6, G6, A6 = 987.77, 1046.5, 1174.66, 1318.5, 1479.98, 1568.0, 1760.0
C7 = 2093.0

# ----------------------------------------------------------------------------- pinball pack
def pinball():
    s = {}
    s['nav']   = env(tone('square', 1000, 1000, 0.035, duty=0.5), atk=0.002, decay=20.0)
    s['sel']   = arp('square', [E5, A5], 0.045, duty=0.45)
    s['back']  = arp('square', [D5, 392.0], 0.05, duty=0.45)
    # launch: rising saw whoosh + air noise
    s['launch'] = env(mix(tone('saw', 180, 760, 0.28, duty=0.5),
                          tone('noise', 1, 1, 0.28, amp=0.25)), atk=0.01, decay=4.0, hold=0.12)
    # flip: mechanical clack = contact transient + plastic thunk + metallic click (layered = richer)
    s['flip']  = mix(env(tone('noise', 1, 1, 0.016, amp=0.7), atk=0.0004, decay=45.0),
                     env(tone('square', 190, 140, 0.05, duty=0.3), atk=0.001, decay=20.0),
                     env(tone('square', 520, 520, 0.02, duty=0.5, amp=0.3), atk=0.001, decay=42.0))
    # bumper pop: bright square + click + a low body pop for weight
    s['bump']  = mix(env(tone('square', 560, 360, 0.09, duty=0.32), atk=0.001, decay=16.0),
                     env(tone('noise', 1, 1, 0.012, amp=0.4), atk=0.0004, decay=60.0),
                     env(tone('sine', 180, 150, 0.06, amp=0.35), atk=0.001, decay=18.0))
    # slingshot: sharp rubber snap = crack + high square + a little body
    s['sling'] = mix(env(tone('noise', 1, 1, 0.013, amp=0.65), atk=0.0003, decay=80.0),
                     env(tone('square', 1300, 850, 0.028, duty=0.4), atk=0.001, decay=34.0),
                     env(tone('square', 420, 420, 0.02, duty=0.5, amp=0.3), atk=0.001, decay=40.0))
    # target ding: two-tone + an octave shimmer on top for a brighter, more polished bell
    s['tgt']   = env(mix(tone('square', C6, C6, 0.08, duty=0.5),
                         tone('square', E6, E6, 0.08, duty=0.5, amp=0.7),
                         tone('sine', C6 * 2, C6 * 2, 0.06, amp=0.25)), atk=0.002, decay=14.0)
    # drain: sad descending warble
    s['drain'] = env(tone('saw', 440, 150, 0.45, duty=0.5, vib_hz=11, vib=0.04), atk=0.01, decay=4.5, hold=0.05)
    s['jack']  = arp('square', [C5, E5, G5, C6, E6], 0.072, duty=0.5, sparkle=True)
    s['bonus'] = arp('tri', [A5, 1174.7, G6], 0.06, duty=0.5)
    # tilt: harsh dissonant buzz
    s['tilt']  = env(mix(tone('square', 110, 96, 0.18, duty=0.5),
                         tone('noise', 1, 1, 0.18, amp=0.35)), atk=0.002, decay=3.0, hold=0.06)
    s['extra'] = arp('square', [E5, 988.0, E6], 0.055, duty=0.5)
    # game over: slow descending toll
    s['over']  = seq([env(tone('square', f, f, 0.16, duty=0.5), atk=0.004, decay=5.5)
                      for f in [392.0, 330.0, 262.0, 196.0]], gap=0.02)
    # pentatonic bumper variants: tone + octave click
    for nm, f in [('bC', C5), ('bD', D5), ('bE', E5), ('bG', G5), ('bA', A5)]:
        s[nm] = mix(env(tone('square', f, f * 0.92, 0.06, duty=0.34), atk=0.001, decay=18.0),
                    env(tone('square', f * 2, f * 2, 0.02, duty=0.5, amp=0.4), atk=0.0005, decay=40.0))
    # power launch: stronger sweep + noise + crush
    s['plg']   = crush(env(mix(tone('saw', 160, 980, 0.32, duty=0.5),
                               tone('noise', 1, 1, 0.32, amp=0.3)), atk=0.008, decay=3.5, hold=0.16), step=2)
    s['lvl']   = arp('square', [E5, A5, C6, E6, A6], 0.09, duty=0.5, sparkle=True)
    s['tw']    = seq([env(tone('square', 140, 130, 0.07, duty=0.5), atk=0.002, decay=8.0)] * 2, gap=0.03)
    # coin: classic two-tone arcade insert
    s['coin']  = seq([env(tone('square', 988.0, 988.0, 0.05, duty=0.5), atk=0.002, decay=14.0),
                      env(tone('square', 1319.0, 1319.0, 0.11, duty=0.5), atk=0.002, decay=8.0)])
    s['x2']    = crush(arp('square', [G5, 988.0, 1319.0, G6], 0.05, duty=0.5), step=2)
    s['slow']  = env(tone('square', 880, 420, 0.3, duty=0.5, vib_hz=14, vib=0.06), atk=0.005, decay=4.0, hold=0.05)
    # blast: low boom + bright crack + crushed noise
    s['blast'] = crush(env(mix(tone('sine', 90, 60, 0.22, amp=1.0),
                               tone('noise', 1, 1, 0.2, amp=0.55),
                               tone('square', 1200, 700, 0.06, duty=0.5, amp=0.5)),
                           atk=0.001, decay=7.0, hold=0.02), step=2)
    # spinner ratchet: a crisp bright metallic tick (plays per-turn while the spinner whirls)
    s['spin']  = mix(env(tone('square', 660, 540, 0.05, duty=0.4), atk=0.0005, decay=32.0),
                     env(tone('square', 980, 980, 0.025, duty=0.5, amp=0.5), atk=0.0004, decay=44.0),
                     env(tone('noise', 1, 1, 0.007, amp=0.3), atk=0.0003, decay=95.0))
    # rail/deflector: short bright metallic ping on a firm rail hit
    s['rail']  = mix(env(tone('square', 1500, 1100, 0.025, duty=0.5, amp=0.6), atk=0.0004, decay=50.0),
                     env(tone('noise', 1, 1, 0.006, amp=0.3), atk=0.0003, decay=110.0))
    return s

# ----------------------------------------------------------------------------- slots pack
def slots():
    s = {}
    # UI
    s['move'] = env(tone('square', 760, 760, 0.035, duty=0.5), atk=0.002, decay=20.0)
    s['sel']  = arp('square', [E5, B5], 0.045, duty=0.45)
    s['back'] = arp('square', [D5, 392.0], 0.05, duty=0.45)
    s['bet']  = env(mix(tone('square', 880, 880, 0.04, duty=0.5),
                        tone('square', 1318, 1318, 0.03, duty=0.5, amp=0.5)), atk=0.002, decay=22.0)
    # lever pull + reel spin-up whirr
    s['spin'] = env(mix(tone('saw', 600, 130, 0.18, duty=0.5),                 # lever descending
                        tone('noise', 1, 1, 0.08, amp=0.4),                    # mechanism
                        tone('square', 330, 587, 0.13, duty=0.5, amp=0.4)),    # reel hum rising
                    atk=0.005, decay=4.0, hold=0.07)
    # reel stops: mechanical clunk, rising pitch per reel, bit-crushed for that arcade grit
    for nm, lf, hf in [('stop1', 196, 1100), ('stop2', 220, 1250), ('stop3', 247, 1400)]:
        s[nm] = crush(mix(env(tone('square', lf, lf * 0.8, 0.07, duty=0.4), atk=0.001, decay=20.0),
                          env(tone('noise', 1, 1, 0.012, amp=0.5), atk=0.0005, decay=60.0),
                          env(tone('square', hf, hf, 0.018, duty=0.5, amp=0.3), atk=0.001, decay=40.0)), step=2)
    # WINS — proper little jingles
    s['winS'] = arp('square', [G5, C6, E6], 0.07, duty=0.5, sparkle=True)                       # small win: 3 coin pings
    s['winB'] = arp('square', [C5, E5, G5, C6, E6, G6], 0.1, duty=0.5, sparkle=True)             # big win: rising victory run
    jack_run  = arp('square', [C5, G5, C6, E6, G6, A6], 0.092, duty=0.5, sparkle=True)
    jack_tail = env(tone('square', 880, 1760, 0.5, duty=0.5, vib_hz=8, vib=0.16), atk=0.01, decay=2.0, hold=0.3)
    s['jack'] = seq([jack_run, jack_tail])                                                       # JACKPOT: run + siren tail
    # tension / loss
    s['antic'] = env(tone('saw', 330, 760, 0.45, duty=0.5, vib_hz=10, vib=0.06), atk=0.01, decay=2.0, hold=0.25)
    s['nowin'] = seq([env(tone('square', 196, 196, 0.1, duty=0.5, amp=0.6), atk=0.005, decay=8.0),
                      env(tone('square', 165, 165, 0.12, duty=0.5, amp=0.6), atk=0.005, decay=7.0)], gap=0.02)
    s['near']  = seq([env(tone('saw', f, f * 0.94, 0.16, duty=0.5), atk=0.01, decay=4.0)         # "aww" descending bends
                      for f in [659.0, 523.0, 392.0]], gap=0.01)
    s['bonus'] = mix(arp('square', [E6, G6, C7], 0.06, duty=0.5, sparkle=True),
                     env(tone('sine', 200, 200, 0.05, amp=0.3), atk=0.002, decay=20.0))          # cash register + thunk
    # ascending payline-reveal dyads (one per winning line)
    for i, (a, b) in enumerate([(C5, G5), (E5, B5), (G5, D6), (B5, Fs6), (D6, A6)]):
        s['ln%d' % (i + 1)] = env(mix(tone('square', a, a, 0.11, duty=0.5),
                                      tone('square', b, b, 0.11, duty=0.5, amp=0.7)), atk=0.002, decay=9.0)
    s['casc']  = crush(arp('square', [G6, E6, C6, E6, G6, A6, G6, C7], 0.045, duty=0.5, sparkle=True), step=2)  # coin rollup
    s['chach'] = seq([env(tone('square', 1318, 1318, 0.08, duty=0.5), atk=0.002, decay=12.0),
                      env(tone('square', 1568, 1568, 0.13, duty=0.5), atk=0.002, decay=6.0)])    # cha-CHING
    s['sprk']  = env(mix(tone('square', C7, 2637.0, 0.1, duty=0.5),
                         tone('sine', 3000, 4000, 0.08, amp=0.3)), atk=0.001, decay=14.0)        # 7 / WILD sparkle
    # accelerating drumroll into a hit
    hits = [env(tone('noise', 1, 1, max(0.02, 0.05 - i * 0.004), amp=0.55 + i * 0.04), atk=0.001, decay=30.0) for i in range(8)]
    s['drum'] = seq(hits) + env(tone('square', 196, 196, 0.12, duty=0.5), atk=0.002, decay=6.0)
    return s

# ----------------------------------------------------------------------------- tanks pack
def tanks():
    # Artillery: punchy launches, weighty booms, sci-fi exotics. Names MUST match app_tanks sfx_name().
    s = {}
    def boom(lo, hi, dur, ns=0.55, crk=0.0, step=2, hold=0.02):
        parts = [tone('sine', lo, hi, dur, amp=1.0), tone('noise', 1, 1, dur * 0.9, amp=ns)]
        if crk: parts.append(tone('square', 1300, 700, 0.05, duty=0.5, amp=crk))
        return crush(env(mix(*parts), atk=0.001, decay=6.0 / dur, hold=hold), step=step)
    def launch(f0, f1, dur, ns=0.3, duty=0.5):
        return env(mix(tone('saw', f0, f1, dur, duty=duty), tone('noise', 1, 1, dur * 0.7, amp=ns)),
                   atk=0.006, decay=4.0, hold=dur * 0.4)

    # --- UI / flow ---
    s['nav']   = env(tone('square', 920, 920, 0.035, duty=0.5), atk=0.002, decay=20.0)
    s['sel']   = arp('square', [E5, A5], 0.05, duty=0.45)
    s['back']  = arp('square', [A5, E5], 0.055, duty=0.45)
    s['turn']  = arp('square', [G5, C6], 0.07, duty=0.5, sparkle=True)                 # "your turn" rising dyad
    s['extra'] = arp('square', [C6, E6, G6, C7], 0.06, duty=0.5, sparkle=True)         # level-up sparkle
    # short menu theme: a confident little marching motif
    s['theme'] = seq([env(tone('square', f, f, 0.13, duty=0.5), atk=0.004, decay=6.0)
                      for f in [C5, E5, G5, E5, A5, G5]], gap=0.015)

    # --- generic combat ---
    s['fire']  = mix(launch(150, 360, 0.16, ns=0.4, duty=0.4), boom(120, 70, 0.10, ns=0.3, step=2))
    s['fly']   = env(tone('saw', 760, 300, 0.5, duty=0.5, vib_hz=9, vib=0.05), atk=0.01, decay=2.2, hold=0.18)
    s['boom']  = boom(95, 55, 0.30, ns=0.6, crk=0.5, hold=0.03)
    s['small'] = boom(150, 90, 0.16, ns=0.45, crk=0.35)
    s['hit']   = mix(boom(110, 70, 0.16, ns=0.4, crk=0.6), env(tone('square', 300, 180, 0.06, duty=0.3), atk=0.001, decay=20.0))
    s['tele']  = env(mix(tone('square', 400, 1900, 0.22, duty=0.5, vib_hz=40, vib=0.2),
                         tone('sine', 1200, 2600, 0.18, amp=0.3)), atk=0.002, decay=6.0)
    # victory fanfare: rising run + bright held tail
    win_run    = arp('square', [C5, E5, G5, C6, E6, G6], 0.085, duty=0.5, sparkle=True)
    win_tail   = env(tone('square', C6, C6, 0.4, duty=0.5, vib_hz=7, vib=0.05), atk=0.01, decay=2.5, hold=0.22)
    s['win']   = seq([win_run, win_tail])
    # defeat: slow descending toll
    s['lose']  = seq([env(tone('square', f, f, 0.18, duty=0.5), atk=0.004, decay=4.5)
                      for f in [G5, E5, C5, 196.0]], gap=0.025)

    # --- per-weapon FIRE (f_*) ---
    s['f_std'] = mix(launch(160, 380, 0.15, ns=0.35, duty=0.4), boom(120, 75, 0.09, ns=0.25))
    s['f_bmb'] = mix(launch(90, 230, 0.22, ns=0.45, duty=0.35), boom(80, 55, 0.12, ns=0.3))   # heavy
    s['f_dig'] = crush(env(mix(tone('square', 220, 130, 0.16, duty=0.3), tone('noise', 1, 1, 0.14, amp=0.5)),
                           atk=0.002, decay=8.0), step=3)                                       # dull thunk
    s['f_clu'] = mix(launch(180, 420, 0.14, ns=0.3), env(tone('noise', 1, 1, 0.06, amp=0.4), atk=0.001, decay=24.0))
    s['f_rai'] = env(mix(tone('saw', 300, 980, 0.26, duty=0.5), tone('noise', 1, 1, 0.24, amp=0.3)),
                     atk=0.01, decay=3.0, hold=0.14)                                            # airy upward salvo
    s['f_swm'] = mix(launch(200, 520, 0.14, ns=0.25), arp('square', [G5, B5, D6], 0.04, duty=0.5))
    s['f_rol'] = crush(env(mix(tone('square', 140, 120, 0.2, duty=0.5, vib_hz=18, vib=0.1),
                               tone('noise', 1, 1, 0.16, amp=0.35)), atk=0.004, decay=6.0), step=3)
    s['f_tel'] = env(tone('square', 300, 1600, 0.18, duty=0.5, vib_hz=36, vib=0.18), atk=0.002, decay=7.0)
    s['f_nuk'] = env(mix(tone('saw', 70, 200, 0.4, duty=0.4), tone('sine', 50, 120, 0.4, amp=0.6),
                         tone('noise', 1, 1, 0.3, amp=0.3)), atk=0.02, decay=2.0, hold=0.22)    # ominous charge

    # --- per-weapon HIT (h_*) ---
    s['h_std'] = boom(120, 75, 0.18, ns=0.5, crk=0.5)
    s['h_bmb'] = boom(80, 48, 0.34, ns=0.7, crk=0.6, hold=0.04)                                 # big
    s['h_dig'] = crush(env(mix(tone('sine', 90, 50, 0.22, amp=0.9), tone('noise', 1, 1, 0.22, amp=0.6)),
                           atk=0.001, decay=5.0, hold=0.03), step=3)                            # earth crumble
    s['h_clu'] = seq([boom(160, 100, 0.07, ns=0.4) for _ in range(4)], gap=0.01)               # pop-pop-pop
    s['h_rai'] = crush(seq([env(tone('noise', 1, 1, 0.03, amp=0.5), atk=0.001, decay=30.0) for _ in range(6)], gap=0.012), step=2)
    s['h_swm'] = seq([boom(180, 110, 0.06, ns=0.35) for _ in range(3)], gap=0.015)
    s['h_rol'] = boom(100, 60, 0.22, ns=0.55, crk=0.4, hold=0.02)
    s['h_tel'] = env(mix(tone('square', 1800, 500, 0.18, duty=0.5, vib_hz=30, vib=0.15),
                         tone('sine', 2400, 900, 0.14, amp=0.3)), atk=0.001, decay=8.0)
    # NUKE hit: three layered stages so it reads as catastrophic, not just another boom.
    s['h_nuk'] = crush(mix(
        env(mix(tone('noise', 1, 1, 0.10, amp=0.9), tone('square', 1500, 480, 0.05, duty=0.5, amp=0.5)),
            atk=0.0004, decay=22.0),                                                  # 1) blinding flash crack
        env(mix(tone('sine', 72, 20, 0.95, amp=1.0), tone('sine', 46, 16, 0.95, amp=0.7)),
            atk=0.001, decay=2.0, hold=0.32),                                          # 2) deep sub-bass shockwave
        env(tone('noise', 1, 1, 0.90, amp=0.6), atk=0.02, decay=3.0, hold=0.12)        # 3) long rolling roar
    ), step=2)

    # --- specials ---
    s['shld']    = env(tone('sine', 300, 760, 0.3, duty=0.5, vib_hz=8, vib=0.06), atk=0.02, decay=3.0, hold=0.12)  # bubble up
    s['shldhit'] = mix(env(tone('sine', 1600, 2200, 0.12, amp=0.7), atk=0.001, decay=12.0),
                       env(tone('square', 900, 1300, 0.06, duty=0.5, amp=0.4), atk=0.001, decay=18.0))             # deflect ting
    s['heal']    = arp('sine', [E5, G5, C6, E6], 0.07, sparkle=True)                                                # gentle chime
    s['jump']    = env(tone('square', 300, 900, 0.14, duty=0.5), atk=0.002, decay=9.0)                              # boing up
    s['beam']    = env(mix(tone('saw', 1400, 600, 0.3, duty=0.5, vib_hz=60, vib=0.1),
                          tone('square', 700, 300, 0.3, duty=0.4, amp=0.4)), atk=0.003, decay=3.5, hold=0.12)       # sci-fi beam
    s['drill']   = crush(env(mix(tone('square', 120, 120, 0.4, duty=0.4, vib_hz=24, vib=0.12),
                                 tone('noise', 1, 1, 0.36, amp=0.4)), atk=0.005, decay=2.0, hold=0.25), step=3)     # burrow motor
    s['build']   = mix(boom(140, 90, 0.1, ns=0.3), env(tone('square', 200, 520, 0.18, duty=0.5), atk=0.004, decay=6.0))  # thud + rise
    s['burn']    = crush(env(mix(tone('noise', 1, 1, 0.5, amp=0.6), tone('saw', 110, 80, 0.5, duty=0.5, amp=0.5)),
                             atk=0.02, decay=2.0, hold=0.3), step=2)                                                 # napalm roar
    return s

GAMES = {'pinball': pinball, 'slots': slots, 'tanks': tanks}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='pinball', choices=list(GAMES))
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=1234)
    a = ap.parse_args()
    random.seed(a.seed)                       # deterministic noise -> reproducible pack
    os.makedirs(a.out, exist_ok=True)
    pack = GAMES[a.game]()
    total = 0
    for nm, buf in pack.items():
        p = os.path.join(a.out, nm + '.wav')
        write_wav(p, buf)
        total += os.path.getsize(p)
    print(f"{a.game}: wrote {len(pack)} sfx, {total // 1024} KB -> {a.out}")

if __name__ == '__main__':
    main()
