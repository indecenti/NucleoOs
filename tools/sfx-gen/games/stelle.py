# Costellazioni (space trader + cockpit dogfights) — rendered to tools/sd-sim/data/costellazioni/pack, played by
# app_constellations.cpp (sfx_name() table). Station UI is soft and glassy (sine/triangle), space is wide and
# airy (saw sweeps, vibrato), combat is punchy but short so rapid lasers never smear.
DIR = 'costellazioni'

def pack(sx):
    t, e, m, sq, ar, cr = sx.tone, sx.env, sx.mix, sx.seq, sx.arp, sx.crush
    s = {}
    # station UI
    s['move'] = e(m(t('sine', 1200, 1200, 0.03), t('tri', 2400, 2400, 0.02, amp=0.3)), atk=0.002, decay=40.0)
    s['ok'] = ar('tri', [sx.E5, sx.B5], 0.05, sparkle=True)
    s['back'] = ar('tri', [sx.B5, sx.E5], 0.05)
    s['buy'] = sq([e(t('square', 1318, 1318, 0.04, duty=0.5, amp=0.7), atk=0.002, decay=24.0),
                   e(t('square', 1976, 1976, 0.09, duty=0.5, amp=0.7), atk=0.002, decay=14.0)])   # credits chime
    s['deny'] = e(m(t('square', 196, 190, 0.14, duty=0.5, amp=0.6), t('square', 185, 180, 0.14, duty=0.5, amp=0.6)), atk=0.003, decay=12.0)
    # hyperspace jump: a rising warp whoosh + shimmer
    s['jump'] = e(m(t('saw', 120, 1400, 0.7, amp=0.6), t('noise', 1, 1, 0.7, amp=0.25), t('sine', 2400, 4800, 0.7, amp=0.15)), atk=0.05, decay=3.0, hold=0.35)
    s['event'] = ar('sine', [sx.E5, sx.A5, sx.E6], 0.07)
    # a beacon relit: a bright bell chord that rings out
    s['beacon'] = e(m(t('sine', sx.C6, sx.C6, 1.0), t('sine', sx.E6, sx.E6, 1.0, amp=0.7), t('sine', sx.G6, sx.G6, 1.0, amp=0.6),
                      t('tri', sx.C5, sx.C5, 1.0, amp=0.5)), atk=0.01, decay=2.6)
    # title: a slow space fanfare
    s['title'] = sq([e(t('tri', f, f, 0.2, vib_hz=5, vib=0.01), atk=0.01, decay=5.0) for f in [sx.C5, sx.G5, sx.C6]] +
                    [e(m(t('tri', sx.E6, sx.E6, 0.55, vib_hz=5, vib=0.012), t('sine', sx.C6, sx.C6, 0.55, amp=0.5)), atk=0.01, decay=3.0)])
    s['win'] = ar('square', [sx.C5, sx.E5, sx.G5, sx.C6, sx.E6, sx.G6], 0.08, duty=0.4, sparkle=True)
    s['lose'] = sq([e(t('saw', f, f * 0.94, 0.3, amp=0.7), atk=0.01, decay=4.0) for f in [392.0, 330.0, 262.0]], gap=0.03)
    # combat
    s['laser'] = cr(e(t('square', 2200, 700, 0.07, duty=0.25, amp=0.8), atk=0.001, decay=30.0), step=2)
    s['hit'] = m(e(t('square', 1760, 1500, 0.04, duty=0.5, amp=0.5), atk=0.001, decay=50.0),
                 e(t('noise', 1, 1, 0.03, amp=0.4), atk=0.001, decay=70.0))
    s['boom'] = cr(e(m(t('sine', 90, 40, 0.45), t('noise', 1, 1, 0.4, amp=0.7), t('square', 300, 120, 0.12, amp=0.4)), atk=0.002, decay=6.0, hold=0.03), step=2)
    s['launch'] = e(m(t('saw', 200, 900, 0.35, amp=0.6), t('noise', 1, 1, 0.35, amp=0.3)), atk=0.02, decay=5.0, hold=0.12)
    s['lock'] = ar('square', [1175.0, 1568.0, 3136.0], 0.04, duty=0.5)
    s['hull'] = cr(e(m(t('square', 110, 70, 0.14, duty=0.5), t('noise', 1, 1, 0.08, amp=0.5)), atk=0.001, decay=14.0), step=3)
    s['shielddown'] = e(t('saw', 1245, 300, 0.4, amp=0.7, vib_hz=18, vib=0.05), atk=0.004, decay=5.0, hold=0.1)
    s['alarm'] = sq([e(t('square', f, f, 0.1, duty=0.5, amp=0.7), atk=0.003, decay=6.0) for f in [880.0, 622.0, 880.0, 622.0]], gap=0.02)
    s['pass'] = e(m(t('noise', 1, 1, 0.22, amp=0.6), t('saw', 900, 200, 0.22, amp=0.4)), atk=0.01, decay=8.0)
    return s
