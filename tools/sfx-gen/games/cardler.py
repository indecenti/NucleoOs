# Cardler (top-down mini-RPG) — rendered to tools/sd-sim/data/Cardler/pack, played by app_cardler.cpp
# (sfx_name() table). Soft, warm, "fantasy handheld": triangle/pulse leads, little noise, nothing harsh.
DIR = 'Cardler'

def pack(sx):
    t, e, m, sq, ar = sx.tone, sx.env, sx.mix, sx.seq, sx.arp
    s = {}
    # menu cursor: a soft wooden tick
    s['select'] = m(e(t('tri', 760, 700, 0.04), atk=0.002, decay=30.0),
                    e(t('square', 1520, 1520, 0.012, duty=0.25, amp=0.25), atk=0.001, decay=60.0))
    # start an adventure: a rising pulse fanfare
    s['start'] = ar('square', [sx.C5, sx.E5, sx.G5, sx.C6], 0.07, duty=0.35, sparkle=True)
    # footstep on grass/dirt: a short muffled thump + a breath of noise
    s['step'] = m(e(t('tri', 120, 80, 0.035, amp=0.8), atk=0.001, decay=55.0),
                  e(t('noise', 1, 1, 0.02, amp=0.18), atk=0.001, decay=90.0))
    # talk: two soft "blip blip" syllables
    s['talk'] = sq([e(t('square', 620, 640, 0.035, duty=0.3, amp=0.7), atk=0.002, decay=24.0),
                    e(t('square', 540, 520, 0.04, duty=0.3, amp=0.7), atk=0.002, decay=22.0)], gap=0.015)
    # chest: wooden creak up + a coin cascade
    s['chest'] = sq([e(t('saw', 180, 320, 0.08, amp=0.45), atk=0.004, decay=10.0),
                     ar('square', [sx.E6, sx.G6, sx.C7], 0.05, duty=0.5, sparkle=True)])
    # heal at a villager: a gentle rising triangle arpeggio with shimmer
    s['heal'] = ar('tri', [sx.C5, sx.E5, sx.G5, sx.C6, sx.E6], 0.06, sparkle=True)
    # hurt: a dull hit + a short down-bend
    s['hurt'] = m(e(t('square', 300, 120, 0.12, duty=0.4, amp=0.8), atk=0.001, decay=16.0),
                  e(t('noise', 1, 1, 0.04, amp=0.4), atk=0.001, decay=50.0))
    # faint: a slow descending lament
    s['faint'] = sq([e(t('tri', f, f * 0.97, 0.2), atk=0.006, decay=5.0) for f in [392.0, 349.2, 311.1, 261.6]], gap=0.02)
    # victory: every chest found — a little royal fanfare
    s['win'] = sq([ar('square', [sx.G5, sx.C6, sx.E6], 0.08, duty=0.4, sparkle=True),
                   e(m(t('square', sx.G6, sx.G6, 0.35, duty=0.4), t('tri', sx.C6, sx.C6, 0.35, amp=0.6)), atk=0.004, decay=4.0, hold=0.1)])
    # pause: a soft two-note chime
    s['pause'] = ar('tri', [sx.G5, sx.D5], 0.06)
    return s
