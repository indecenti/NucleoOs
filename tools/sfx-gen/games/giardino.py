# Giardino (app_sandgarden.cpp): a calm garden — soft triangle/sine voices on a pentatonic scale, a rising
# pitch for bigger merges, a little wind-chime sparkle on the special moments. Nothing harsh: this is the
# relaxing game of the set. Names match sfx_name() in the firmware.
DIR = 'giardino'

def pack(sx):
    # C major pentatonic, two octaves
    P = [523.25, 587.33, 659.25, 783.99, 880.0, 1046.5, 1174.66, 1318.5, 1567.98, 1760.0]
    def pluck(f, dur=0.12, amp=1.0):
        return sx.env(sx.mix(sx.tone('tri', f, f, dur, amp=amp), sx.tone('sine', f * 2, f * 2, dur, amp=0.25 * amp)),
                      atk=0.004, decay=14.0)
    def chime(notes, nd=0.09):
        return sx.seq([sx.env(sx.mix(sx.tone('sine', f, f, nd * 2.2, amp=0.8), sx.tone('sine', f * 3, f * 3, nd * 2.2, amp=0.12)),
                              atk=0.003, decay=7.0) for f in notes])
    s = {}
    s['nav']   = pluck(P[4], 0.05, 0.7)
    s['sel']   = sx.seq([pluck(P[2], 0.06), pluck(P[5], 0.09)])
    s['back']  = sx.seq([pluck(P[4], 0.06), pluck(P[1], 0.09)])
    # a soft whoosh of leaves: filtered noise swell
    s['slide'] = sx.env(sx.mix(sx.tone('noise', 1, 1, 0.12, amp=0.22), sx.tone('sine', 300, 200, 0.12, amp=0.25)), atk=0.03, decay=16.0)
    s['m1']    = pluck(P[2], 0.14)
    s['m2']    = sx.seq([pluck(P[3], 0.07), pluck(P[5], 0.14)])
    s['m3']    = sx.seq([pluck(P[4], 0.06), pluck(P[6], 0.06), pluck(P[7], 0.16)])
    s['m4']    = sx.mix(chime([P[5], P[7], P[8]], 0.07), sx.seq([[0.0] * int(0.05 * sx.RATE), pluck(P[9], 0.22, 0.6)]))
    s['undo']  = sx.env(sx.tone('tri', 900, 500, 0.12), atk=0.004, decay=12.0)
    s['best']  = chime([P[5], P[6], P[7], P[9]], 0.07)
    s['win']   = sx.mix(chime([P[0], P[2], P[4], P[5], P[7], P[9]], 0.11),
                        sx.env(sx.tone('sine', 261.63, 261.63, 1.1, amp=0.35), atk=0.05, decay=2.2))
    s['over']  = sx.seq([pluck(P[4], 0.14), pluck(P[2], 0.14), pluck(392.0, 0.14), pluck(261.63, 0.3)])
    s['nope']  = sx.env(sx.tone('tri', 180, 160, 0.08, amp=0.7), atk=0.003, decay=20.0)
    return s
