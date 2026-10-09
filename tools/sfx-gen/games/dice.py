# Dadi sound pack (firmware: app_dice.cpp, sfx("shake" | "throw" | "land")). Rendered on the PC:
#   python tools/sfx-gen/gen_arcade_sfx.py --game dice  ->  tools/sd-sim/data/dice/pack/*.wav
# Dice in a cup are short noisy knocks with a woody body; the landing is a few clacks that settle.
import random

DIR = 'dice'


def pack(sx):
    rnd = random.Random(11)

    def rest(sec):
        return [0.0] * int(sec * sx.RATE)

    def knock(f, amp=1.0, dur=0.012):
        return sx.mix(sx.env(sx.tone('noise', 1, 1, dur, amp=0.7 * amp), atk=0.0005, decay=95.0),
                      sx.env(sx.tone('tri', f, f * 0.7, dur * 2.4, amp=0.8 * amp), atk=0.0005, decay=50.0))

    s = {}
    # one shake of the cup: 5 quick knocks, a rattle that loops while mixing
    s['shake'] = sx.seq(sum([[rest(rnd.uniform(0.012, 0.03)), knock(rnd.uniform(1500, 2600), 0.75)] for _ in range(5)], []))
    # the throw: a burst of knocks thinning out over a soft whoosh
    parts = []
    for i in range(10):
        parts += [rest(rnd.uniform(0.01, 0.03) * (1 + i * 0.12)), knock(rnd.uniform(1300, 2500), 1.0 - i * 0.06)]
    s['throw'] = sx.mix(sx.seq(parts), sx.env(sx.tone('noise', 1, 1, 0.3, amp=0.18), atk=0.03, decay=9.0))
    # landing: a firm thud, then a few settling clacks
    land = [sx.mix(sx.env(sx.tone('tri', 200, 110, 0.09), atk=0.001, decay=26.0), knock(2100, 1.0, 0.014))]
    for i, (g, f) in enumerate([(0.05, 1800), (0.07, 2300), (0.06, 1600), (0.09, 2000)]):
        land += [rest(g), knock(f, 0.7 - i * 0.13)]
    s['land'] = sx.seq(land)
    return s
