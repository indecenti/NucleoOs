# Reattore (reactor) — the reactor-control game's SFX pack.
# Rendered on the PC (python tools/sfx-gen/gen_arcade_sfx.py --game reactor) to tools/sd-sim/data/reattore/pack,
# played off the SD by app_reactor.cpp (a WAV in /sd/data/reattore/custom/ overrides a cue). Names MUST match
# sfx_name(). Design: control-room hardware — servo whirrs for the rods, a hydraulic slam for SCRAM, a
# heartbeat thump and a two-tone klaxon for the heat, a layered detonation for the meltdown.

DIR = 'reattore'


def pack(sx):
    tone, env, mix, seq, crush, arp = sx.tone, sx.env, sx.mix, sx.seq, sx.crush, sx.arp
    C5, E5, G5, A5, C6, E6 = sx.C5, sx.E5, sx.G5, sx.A5, sx.C6, sx.E6
    s = {}
    # --- UI ---
    s['move'] = env(tone('square', 760, 760, 0.03, duty=0.5, amp=0.7), atk=0.002, decay=24.0)
    s['ok'] = arp('square', [E5, 987.77], 0.05, duty=0.45)
    s['back'] = arp('square', [E5, 440.0], 0.05, duty=0.45)
    # --- rods: a short servo whirr up (pull out = more power) / down (push in = cooling) ---
    s['out'] = env(mix(tone('saw', 420, 900, 0.07, amp=0.5), tone('square', 840, 1200, 0.05, duty=0.3, amp=0.3)),
                   atk=0.003, decay=18.0)
    s['in'] = env(mix(tone('saw', 700, 330, 0.07, amp=0.5), tone('square', 560, 300, 0.05, duty=0.3, amp=0.3)),
                  atk=0.003, decay=18.0)
    # --- SCRAM: a hydraulic slam (noise burst + falling square) and a cooling hiss ---
    slam = crush(env(mix(tone('noise', 1, 1, 0.08, amp=0.8), tone('square', 300, 120, 0.12, duty=0.4, amp=0.6)),
                     atk=0.001, decay=16.0), step=2)
    hiss = env(tone('noise', 1, 1, 0.35, amp=0.35), atk=0.02, decay=6.0, hold=0.08)
    s['scram'] = seq([slam, hiss])
    # --- heat ---
    s['alarm'] = seq([env(tone('square', 988, 988, 0.09, duty=0.5), atk=0.002, decay=10.0)] * 2, gap=0.05)
    s['heart'] = seq([env(tone('sine', 70, 55, 0.08, amp=1.0), atk=0.002, decay=22.0),
                      env(tone('sine', 62, 50, 0.07, amp=0.7), atk=0.002, decay=24.0)], gap=0.07)   # lub-dub
    s['klaxon'] = seq([env(tone('saw', 988, 960, 0.09, amp=0.8), atk=0.003, decay=6.0, hold=0.04),
                       env(tone('saw', 740, 720, 0.12, amp=0.8), atk=0.003, decay=6.0, hold=0.05)])   # nee-naw
    s['rise'] = env(mix(tone('square', 698, 1046, 0.08, duty=0.5, amp=0.6)), atk=0.002, decay=14.0)
    # --- the meltdown: a blinding crack, a deep shockwave, a long rolling roar ---
    s['boom'] = crush(mix(
        env(mix(tone('noise', 1, 1, 0.09, amp=0.9), tone('square', 1400, 420, 0.05, duty=0.5, amp=0.5)), atk=0.0005, decay=24.0),
        env(mix(tone('sine', 80, 24, 0.8, amp=1.0), tone('sine', 52, 18, 0.8, amp=0.7)), atk=0.001, decay=2.6, hold=0.25),
        env(tone('noise', 1, 1, 0.75, amp=0.55), atk=0.02, decay=3.2, hold=0.1)), step=2)
    # --- start: the reactor spinning up ---
    s['start'] = seq([arp('square', [C5, E5, G5], 0.09, duty=0.5),
                      env(tone('square', C6, C6, 0.25, duty=0.5, vib_hz=6, vib=0.03), atk=0.01, decay=5.0, hold=0.08)])
    return s
