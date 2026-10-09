# Pong — rendered to tools/sd-sim/data/pong/pack, played by app_pong.cpp (sfx_name() table).
# The 1972 cabinet's voice (square blips: wall ~226 Hz, paddle ~459 Hz, point ~490 Hz) with a little modern
# juice on top: an octave tick on paddle hits, sweeps for serve and power-ups, short jingles for the match end.
DIR = 'pong'

def pack(sx):
    t, e, m, sq, ar = sx.tone, sx.env, sx.mix, sx.seq, sx.arp
    s = {}
    # --- menus ---
    s['nav']   = e(t('square', 880, 880, 0.03, duty=0.5, amp=0.6), atk=0.001, decay=40.0)
    s['sel']   = ar('square', [sx.E5, sx.A5], 0.045, duty=0.5)
    s['back']  = ar('square', [sx.A5, sx.E5], 0.045, duty=0.5)
    # --- match flow ---
    s['count'] = e(t('square', 660, 660, 0.09, duty=0.5, amp=0.7), atk=0.002, decay=18.0)
    s['serve'] = e(t('tri', 280, 980, 0.10), atk=0.003, decay=14.0)
    # --- the ball ---
    s['wall']  = e(t('square', 226, 226, 0.03, duty=0.5, amp=0.7), atk=0.001, decay=50.0)
    s['hit']   = m(e(t('square', 459, 459, 0.045, duty=0.5), atk=0.001, decay=34.0),
                   e(t('square', 918, 918, 0.02, duty=0.25, amp=0.35), atk=0.001, decay=80.0))
    s['obst']  = sx.crush(m(e(t('square', 330, 250, 0.06, duty=0.4), atk=0.001, decay=30.0),
                            e(t('noise', 1, 1, 0.03, amp=0.35), atk=0.001, decay=70.0)), step=2)
    s['power'] = ar('square', [sx.C6, sx.E6, sx.G6, sx.C7], 0.04, duty=0.5, sparkle=True)
    s['score'] = sq([e(t('square', 490, 490, 0.22, duty=0.5), atk=0.002, decay=6.0),
                     e(t('square', 245, 245, 0.08, duty=0.5, amp=0.7), atk=0.002, decay=14.0)])
    s['level'] = ar('square', [sx.E5, sx.G5, sx.C6, sx.E6], 0.055, duty=0.45, sparkle=True)
    # --- match end ---
    s['win']   = sq([ar('square', [sx.C5, sx.E5, sx.G5, sx.C6], 0.075, duty=0.5, sparkle=True),
                     e(t('square', sx.E6, sx.E6, 0.36, duty=0.5, vib_hz=7, vib=0.04), atk=0.006, decay=3.0, hold=0.16)])
    s['lose']  = sq([e(t('square', f, f, 0.16, duty=0.5), atk=0.003, decay=5.0) for f in [sx.G5, sx.E5, sx.C5, 392.0 / 2]], gap=0.02)
    return s
