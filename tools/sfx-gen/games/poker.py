# Poker sound pack (firmware: app_poker.cpp, sfx_name()). Rendered on the PC:
#   python tools/sfx-gen/gen_arcade_sfx.py --game poker  ->  tools/sd-sim/data/poker/pack/*.wav
# Cards are papery noise slaps with a soft body, chips are short metallic double clinks, wins are tiered:
# coin pings (small), a rising fanfare (straight and up), the Ode to Joy in two voices (four of a kind and up).
DIR = 'poker'


def pack(sx):
    s = {}
    C5, D5, E5, G5, A5, B5 = sx.C5, sx.D5, sx.E5, sx.G5, sx.A5, sx.B5
    C6, E6, G6, C7 = sx.C6, sx.E6, sx.G6, sx.C7
    F5 = 698.46

    def rest(sec):
        return [0.0] * int(sec * sx.RATE)

    def clink(f, amp=1.0):                                   # a chip: two detuned metallic pings
        return sx.mix(sx.env(sx.tone('square', f, f, 0.05, duty=0.25, amp=0.5 * amp), atk=0.0008, decay=45.0),
                      sx.env(sx.tone('sine', f * 2.76, f * 2.76, 0.07, amp=0.45 * amp), atk=0.0008, decay=35.0),
                      sx.env(sx.tone('noise', 1, 1, 0.006, amp=0.4 * amp), atk=0.0003, decay=200.0))

    # UI
    s['nav'] = sx.env(sx.tone('tri', 760, 800, 0.035), atk=0.002, decay=30.0)
    s['sel'] = sx.seq([sx.env(sx.tone('tri', E5, E5, 0.04), atk=0.002, decay=22.0),
                       sx.env(sx.tone('tri', B5, B5, 0.07), atk=0.002, decay=16.0)])
    s['back'] = sx.seq([sx.env(sx.tone('tri', D5, D5, 0.04), atk=0.002, decay=22.0),
                        sx.env(sx.tone('tri', 392, 392, 0.07), atk=0.002, decay=16.0)])
    s['holdon'] = sx.seq([sx.env(sx.tone('tri', G5, G5, 0.025), atk=0.002, decay=30.0),
                          sx.env(sx.tone('tri', 1175, 1175, 0.05), atk=0.002, decay=24.0)])
    s['holdf'] = sx.seq([sx.env(sx.tone('tri', 660, 660, 0.025), atk=0.002, decay=30.0),
                         sx.env(sx.tone('tri', 440, 440, 0.05), atk=0.002, decay=24.0)])
    s['bet'] = sx.seq([clink(2400), rest(0.03), clink(2650, 0.8)])

    # cards: the deal slaps the felt, the draw is a lighter flick
    s['deal'] = sx.mix(sx.env(sx.tone('noise', 1, 1, 0.05, amp=0.9), atk=0.001, decay=55.0),
                       sx.env(sx.tone('tri', 260, 140, 0.05, amp=0.8), atk=0.001, decay=40.0))
    s['draw'] = sx.mix(sx.env(sx.tone('noise', 1, 1, 0.035, amp=0.7), atk=0.001, decay=75.0),
                       sx.env(sx.tone('tri', 420, 300, 0.03, amp=0.5), atk=0.001, decay=60.0))

    # results
    s['nowin'] = sx.seq([sx.env(sx.tone('tri', 233, 220, 0.11), atk=0.004, decay=10.0),
                         sx.env(sx.tone('tri', 196, 185, 0.16), atk=0.004, decay=8.0)], gap=0.02)
    s['winS'] = sx.mix(sx.arp('square', [G5, C6, E6], 0.065, duty=0.5, sparkle=True),
                       sx.seq([rest(0.2), clink(2600, 0.7)]))
    s['winB'] = sx.mix(sx.arp('square', [C5, E5, G5, C6, E6, G6], 0.085, duty=0.5, sparkle=True),
                       sx.seq([rest(0.5), sx.env(sx.tone('square', C7, C7, 0.3, duty=0.5, vib_hz=7, vib=0.01, amp=0.6), atk=0.005, decay=5.0)]))
    # Ode to Joy: melody (square) over a bass (triangle), the jackpot anthem
    mel = [(E5, 1), (E5, 1), (F5, 1), (G5, 1), (G5, 1), (F5, 1), (E5, 1), (D5, 1),
           (C5, 1), (C5, 1), (D5, 1), (E5, 1), (E5, 1.5), (D5, 0.5), (D5, 2)]
    bass = [130.81, 130.81, 130.81, 98.0, 130.81, 130.81, 98.0, 98.0, 87.31, 130.81, 98.0, 130.81, 98.0, 98.0, 98.0]
    q = 0.16
    m_parts, b_parts = [], []
    for (f, beats), bf in zip(mel, bass):
        dur = q * beats
        m_parts.append(sx.env(sx.tone('square', f, f, dur, duty=0.5), atk=0.004, decay=3.5 if beats > 1 else 6.0))
        b_parts.append(sx.env(sx.tone('tri', bf, bf, dur, amp=0.7), atk=0.004, decay=3.0))
    s['ode'] = sx.mix(sx.seq(m_parts), sx.seq(b_parts))
    s['bonus'] = sx.seq([clink(2200), rest(0.02), clink(2500), rest(0.02), clink(2800),
                         sx.arp('square', [E6, G6, C7], 0.06, duty=0.5, sparkle=True)])
    s['bust'] = sx.seq([sx.env(sx.tone('square', f, f * 0.97, 0.16, duty=0.4, amp=0.7), atk=0.004, decay=5.0)
                        for f in [392.0, 330.0, 262.0]] + [sx.env(sx.tone('tri', 196, 185, 0.4), atk=0.004, decay=3.0)])
    return s
