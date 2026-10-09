# Scorribanda (brawler) — the noir belt-scroll beat'em up's SFX pack.
# Rendered on the PC (python tools/sfx-gen/gen_arcade_sfx.py --game brawler) to tools/sd-sim/data/brawler/pack,
# played straight off the SD by brawler_sfx.cpp (zero device CPU). Names MUST match bsfx_name().
# Design: short and physical — a dry swish for a miss, a meaty low thump + crack for a connect (heavier for
# finishers/kicks), a muffled thud for a guard, a falling body for a KO; chiptune stingers for the flow.

DIR = 'brawler'


def pack(sx):
    tone, env, mix, seq, crush, arp = sx.tone, sx.env, sx.mix, sx.seq, sx.crush, sx.arp
    C5, E5, G5, A5, C6, E6, G6 = sx.C5, sx.E5, sx.G5, sx.A5, sx.C6, sx.E6, sx.G6
    s = {}
    # --- UI ---
    s['nav'] = env(tone('square', 880, 880, 0.03, duty=0.5, amp=0.7), atk=0.002, decay=24.0)
    s['sel'] = arp('square', [E5, A5], 0.045, duty=0.45)
    s['back'] = arp('square', [A5, E5], 0.05, duty=0.45)

    # --- moves ---
    # whiff: a dry air swish (noise with a falling saw underneath), quiet so a flurry never tires the ear
    s['whiff'] = env(mix(tone('noise', 1, 1, 0.09, amp=0.5),
                         tone('saw', 1400, 380, 0.09, amp=0.25)), atk=0.006, decay=22.0)
    # hit: knuckles on a body — a low thump, a short crack on top, a little square bite for presence
    s['hit'] = crush(mix(env(tone('sine', 150, 62, 0.12, amp=1.0), atk=0.0008, decay=18.0),
                         env(tone('noise', 1, 1, 0.03, amp=0.55), atk=0.0004, decay=70.0),
                         env(tone('square', 240, 120, 0.05, duty=0.3, amp=0.35), atk=0.001, decay=30.0)), step=2)
    # heavy: finisher / kick — deeper, longer, with a second rumble
    s['heavy'] = crush(mix(env(tone('sine', 120, 42, 0.22, amp=1.0), atk=0.0008, decay=11.0),
                           env(tone('noise', 1, 1, 0.06, amp=0.7), atk=0.0004, decay=40.0),
                           env(tone('square', 180, 80, 0.12, duty=0.35, amp=0.4), atk=0.001, decay=16.0)), step=2)
    # block: a guard soaking the blow — muffled, short, no crack
    s['block'] = env(mix(tone('square', 210, 170, 0.06, duty=0.25, amp=0.6),
                         tone('noise', 1, 1, 0.025, amp=0.25)), atk=0.001, decay=34.0)
    # hurt: the hero takes it — a dissonant grunt
    s['hurt'] = env(mix(tone('square', 300, 240, 0.12, duty=0.4, amp=0.6),
                        tone('square', 318, 250, 0.12, duty=0.4, amp=0.6),
                        tone('noise', 1, 1, 0.04, amp=0.4)), atk=0.002, decay=14.0)
    # ko: a last heavy hit then the body dropping (falling saw + floor thud)
    ko_hit = crush(mix(env(tone('sine', 130, 45, 0.16, amp=1.0), atk=0.0008, decay=14.0),
                       env(tone('noise', 1, 1, 0.05, amp=0.6), atk=0.0004, decay=50.0)), step=2)
    ko_fall = env(tone('saw', 320, 70, 0.32, duty=0.5, amp=0.45), atk=0.005, decay=6.0, hold=0.06)
    ko_floor = env(mix(tone('sine', 80, 40, 0.12, amp=0.9), tone('noise', 1, 1, 0.05, amp=0.35)), atk=0.001, decay=20.0)
    s['ko'] = seq([ko_hit, ko_fall, ko_floor])
    s['jump'] = env(tone('square', 260, 640, 0.1, duty=0.4, amp=0.6), atk=0.002, decay=14.0)

    # --- flow ---
    s['go'] = seq([env(tone('square', A5, A5, 0.07, duty=0.5), atk=0.002, decay=12.0),
                   env(tone('square', A5, A5, 0.07, duty=0.5), atk=0.002, decay=12.0)], gap=0.05)
    s['clear'] = seq([arp('square', [C5, E5, G5, C6], 0.08, duty=0.5, sparkle=True),
                      env(tone('square', E6, E6, 0.3, duty=0.5, vib_hz=7, vib=0.04), atk=0.01, decay=4.0, hold=0.12)])
    s['over'] = seq([env(tone('square', f, f, 0.17, duty=0.5), atk=0.004, decay=5.0)
                     for f in [G5, E5, C5, 392.0 / 2]], gap=0.025)
    return s
