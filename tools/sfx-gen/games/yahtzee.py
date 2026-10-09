# Yahtzee sound pack (firmware: app_yahtzee.cpp, sfx_name()). Rendered on the PC:
#   python tools/sfx-gen/gen_arcade_sfx.py --game yahtzee  ->  tools/sd-sim/data/yahtzee/pack/*.wav
# Every cue is peak-normalized by the writer, so loudness is shaped by timbre: UI ticks are soft
# triangle/sine blips, dice are noisy wooden clicks, scores are bright square chimes with a sparkle.
import random

DIR = 'yahtzee'


def pack(sx):
    s = {}
    C5, E5, G5, A5, B5 = sx.C5, sx.E5, sx.G5, sx.A5, sx.B5
    C6, D6, E6, G6, A6, C7 = sx.C6, sx.D6, sx.E6, sx.G6, sx.A6, sx.C7

    def click(f, dur=0.014, amp=1.0):                       # one die knocking the tray: noise burst + woody body
        return sx.mix(sx.env(sx.tone('noise', 1, 1, dur, amp=0.7 * amp), atk=0.0005, decay=90.0),
                      sx.env(sx.tone('tri', f, f * 0.7, dur * 2.2, amp=0.8 * amp), atk=0.0005, decay=55.0))

    def rest(sec):
        return [0.0] * int(sec * sx.RATE)

    # UI
    s['nav'] = sx.env(sx.tone('tri', 700, 760, 0.035), atk=0.002, decay=30.0)
    s['holdon'] = sx.seq([sx.env(sx.tone('tri', G5, G5, 0.03), atk=0.002, decay=25.0),
                          sx.env(sx.tone('tri', 1175, 1175, 0.05), atk=0.002, decay=22.0)])
    s['holdf'] = sx.seq([sx.env(sx.tone('tri', 660, 660, 0.03), atk=0.002, decay=25.0),
                         sx.env(sx.tone('tri', 440, 440, 0.05), atk=0.002, decay=22.0)])
    s['holdall'] = sx.arp('tri', [C5, E5, G5, C6, E6], 0.03)
    s['arm'] = sx.env(sx.tone('sine', 1280, 1500, 0.02, amp=0.6), atk=0.001, decay=60.0)
    s['suggest'] = sx.seq([sx.env(sx.tone('sine', 1480, 1480, 0.06), atk=0.003, decay=30.0),
                           sx.env(sx.tone('sine', 2093, 2093, 0.09), atk=0.003, decay=24.0)])
    s['turn'] = sx.mix(sx.env(sx.tone('tri', D6 / 2, D6 / 2, 0.09), atk=0.004, decay=14.0),
                       sx.seq([rest(0.06), sx.env(sx.tone('tri', A5, A5, 0.16), atk=0.004, decay=10.0)]))
    s['deny'] = sx.seq([sx.env(sx.tone('square', 196, 185, 0.08, duty=0.3, amp=0.55), atk=0.003, decay=14.0),
                        sx.env(sx.tone('square', 165, 150, 0.12, duty=0.3, amp=0.55), atk=0.003, decay=10.0)], gap=0.015)

    # dice: a cup rattle that thins out (random knocks, shrinking gaps), then the landing clack
    rnd = random.Random(7)
    parts, t = [], 0.0
    for i in range(14):
        parts.append(rest(rnd.uniform(0.012, 0.034) * (1.0 + i * 0.05)))
        parts.append(click(rnd.uniform(1400, 2600), amp=1.0 - i * 0.04))
    s['roll'] = sx.seq(parts)
    s['settle'] = sx.mix(sx.env(sx.tone('tri', 190, 110, 0.09), atk=0.001, decay=28.0),
                         click(2200, 0.012), sx.seq([rest(0.045), click(1700, 0.01, 0.55)]))

    # scoring stings
    s['score'] = sx.arp('square', [B5, E6], 0.07, duty=0.5, sparkle=True)
    s['scoreB'] = sx.arp('square', [E5, B5, E6], 0.075, duty=0.5, sparkle=True)
    s['bonus'] = sx.mix(sx.arp('square', [G5, C6, E6, A6], 0.065, duty=0.5, sparkle=True),
                        sx.seq([rest(0.26), sx.env(sx.tone('sine', C7, C7, 0.25, vib_hz=7, vib=0.01), atk=0.004, decay=6.0)]))
    run = sx.arp('square', [C5, E5, G5, C6, E6, G6], 0.075, duty=0.5, sparkle=True)
    tail = sx.mix(sx.env(sx.tone('square', C6, C6, 0.6, duty=0.5, vib_hz=7, vib=0.012), atk=0.01, decay=2.6, hold=0.18),
                  sx.env(sx.tone('square', E6, E6, 0.6, duty=0.5, vib_hz=7, vib=0.012, amp=0.6), atk=0.01, decay=2.6, hold=0.18),
                  sx.env(sx.tone('square', G6, G6, 0.6, duty=0.5, vib_hz=7, vib=0.012, amp=0.5), atk=0.01, decay=2.6, hold=0.18))
    s['yahtzee'] = sx.seq([run, tail])
    # results: a short cadence that resolves up (it is a finish, not a loss)
    cad = sx.seq([sx.env(sx.tone('square', f, f, d, duty=0.5), atk=0.004, decay=7.0) for f, d in
                  [(G5, 0.12), (E5, 0.12), (G5, 0.12), (C6, 0.16)]])
    s['over'] = sx.seq([cad, sx.mix(sx.env(sx.tone('tri', C5, C5, 0.5), atk=0.01, decay=3.0, hold=0.1),
                                    sx.env(sx.tone('square', E6, E6, 0.5, duty=0.5, amp=0.45), atk=0.01, decay=3.5, hold=0.1))])
    return s
