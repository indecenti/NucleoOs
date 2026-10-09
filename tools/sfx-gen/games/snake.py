# Snake Duel — rendered to tools/sd-sim/data/snake/pack, played by app_snake.cpp through game_sfx
# (sn_sfx_name() table). Bright and quick: eating must feel good a hundred times in a row.
DIR = 'snake'

def pack(sx):
    t, e, m, sq, ar = sx.tone, sx.env, sx.mix, sx.seq, sx.arp
    s = {}
    s['nav']   = e(t('square', 760, 760, 0.03, duty=0.5, amp=0.6), atk=0.001, decay=40.0)
    # 3-2-1 done: a two-step "go!"
    s['start'] = sq([e(t('square', sx.G5, sx.G5, 0.07, duty=0.5), atk=0.002, decay=14.0),
                     e(t('square', sx.C6, sx.C6, 0.14, duty=0.5), atk=0.002, decay=9.0)], gap=0.02)
    # a chomp: short upward pulse + a soft triangle body
    s['eat']   = m(e(t('square', 520, 980, 0.055, duty=0.3, amp=0.8), atk=0.001, decay=26.0),
                   e(t('tri', 260, 390, 0.06, amp=0.6), atk=0.001, decay=24.0))
    s['pu']    = ar('square', [sx.G5, sx.C6, sx.E6, sx.G6], 0.045, duty=0.5, sparkle=True)
    # crash: a falling buzz into a crunch
    s['die']   = sx.crush(m(e(t('saw', 420, 70, 0.38), atk=0.002, decay=5.0, hold=0.05),
                            e(t('noise', 1, 1, 0.18, amp=0.5), atk=0.001, decay=14.0)), step=3)
    s['win']   = sq([ar('square', [sx.C5, sx.E5, sx.G5, sx.C6, sx.E6], 0.07, duty=0.5, sparkle=True),
                     e(t('square', sx.G6, sx.G6, 0.32, duty=0.5, vib_hz=7, vib=0.04), atk=0.006, decay=3.2, hold=0.14)])
    return s
