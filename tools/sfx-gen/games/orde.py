# Orde (mini vampire-survivors) — rendered to tools/sd-sim/data/Orde/pack, played by app_vs.cpp (sfx_name()
# table). Kills and gems fire constantly, so those are tiny and soft (and are dropped while a bigger cue
# plays); level-up, waves, bosses and the bomb are the loud, interrupting moments.
DIR = 'Orde'

def pack(sx):
    t, e, m, sq, ar, cr = sx.tone, sx.env, sx.mix, sx.seq, sx.arp, sx.crush
    s = {}
    s['select'] = e(t('square', 900, 900, 0.03, duty=0.4), atk=0.002, decay=30.0)
    # a run begins: a short heroic rise
    s['start'] = ar('square', [sx.C5, sx.G5, sx.C6], 0.08, duty=0.4, sparkle=True)
    # pyromancer nova: a whoompf of fire
    s['shot'] = cr(e(m(t('noise', 1, 1, 0.22, amp=0.6), t('saw', 160, 60, 0.22, amp=0.6)), atk=0.005, decay=9.0), step=2)
    # enemy popped: a tiny crunchy blip
    s['die'] = m(e(t('square', 520, 180, 0.05, duty=0.3, amp=0.7), atk=0.001, decay=34.0),
                 e(t('noise', 1, 1, 0.02, amp=0.35), atk=0.001, decay=80.0))
    # xp gem: a bright, very short sparkle
    s['pickup'] = e(m(t('square', sx.E6, sx.E6 * 1.5, 0.04, duty=0.5, amp=0.6), t('sine', sx.E6 * 2, sx.E6 * 2, 0.04, amp=0.25)), atk=0.001, decay=40.0)
    # level up: the classic rising arpeggio with shimmer
    s['levelup'] = ar('square', [sx.C5, sx.E5, sx.G5, sx.C6, sx.E6, sx.G6], 0.06, duty=0.5, sparkle=True)
    # hurt: a low thud + grit
    s['hurt'] = cr(e(m(t('square', 180, 90, 0.1, duty=0.5, amp=0.8), t('noise', 1, 1, 0.06, amp=0.45)), atk=0.001, decay=18.0), step=2)
    # ambush wave: a war horn — two low pulses with vibrato
    s['wave'] = sq([e(t('saw', 110, 110, 0.18, vib_hz=6, vib=0.03), atk=0.02, decay=4.0, hold=0.08),
                    e(t('saw', 147, 147, 0.26, vib_hz=6, vib=0.03), atk=0.02, decay=4.0, hold=0.12)], gap=0.03)
    # game over: a slow descending toll
    s['over'] = sq([e(t('square', f, f, 0.18, duty=0.5), atk=0.004, decay=5.0) for f in [330.0, 262.0, 220.0, 165.0]], gap=0.02)
    # boss arrives: an ominous low swell + dissonant top
    s['boss'] = e(m(t('saw', 55, 70, 0.7, amp=0.8), t('square', 233, 220, 0.7, duty=0.5, amp=0.35, vib_hz=5, vib=0.04)), atk=0.08, decay=2.5, hold=0.25)
    # bomb: a big crushed boom
    s['bomb'] = cr(e(m(t('sine', 80, 40, 0.5), t('noise', 1, 1, 0.45, amp=0.7)), atk=0.002, decay=4.5, hold=0.05), step=3)
    # heal: a soft rising chime
    s['heal'] = ar('tri', [sx.G5, sx.C6, sx.E6], 0.06, sparkle=True)
    return s
