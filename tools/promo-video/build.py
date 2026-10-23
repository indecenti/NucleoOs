#!/usr/bin/env python3
"""Build the 30-second NucleoOS promo video (Reddit / Discord), cut to the soundtrack's beat.

Every picture is a real capture: the native UI renders of `npm run ui:shots` (build/ui-host/shots,
English, classic theme) composited into the website's device cut-outs, and the website's web-desktop
captures. The brand (fonts, warm gray, the orange `fn` key) comes from the website checkout, like
tools/m5burner-kit.py's cover.

  build/promo/nucleoos-promo-16x9.mp4   1920x1080
  build/promo/nucleoos-promo-1x1.mp4    1080x1080
  build/promo/contact-<fmt>.jpg         one frame from the middle of every shot, for review

Usage:  python tools/promo-video/build.py --audio <mastered.wav> [--bpm 128] [--offset <s>] [--only 16x9|1x1]
The soundtrack is generated with ACE-Step from tools/promo-video/soundtrack.json and mastered to
about -14 LUFS / -1 dBTP (see README.md here). Without --offset the first beat is found with librosa.

Privacy: the web captures' taskbar shows the capture machine's Wi-Fi name, so every web capture is
cropped above it (TASKBAR_CROP). "Casa-Rossi" in the native renders is the harness's fake fixture.
"""
import argparse, os, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

REPO = Path(__file__).resolve().parents[2]
SHOTS = REPO / 'build' / 'ui-host' / 'shots'
SITE = Path(os.environ.get('NUCLEOOS_SITE', REPO.parent / 'nucleos_sites'))
WEB = SITE / 'img' / 'shots' / 'en'
DEVICE = SITE / 'img' / 'device'
SCREEN_PCT = {'original': (25.197, 6.789, 29.291, 25.637), 'adv': (25.170, 6.715, 29.006, 25.318)}  # site make-assets.py
FONTS = {'display': 'big-shoulders-700.woff', 'bold': 'work-sans-700.woff', 'sans': 'work-sans-400.woff',
         'pixel': 'silkscreen-400.woff'}
TASKBAR_CROP = 0.945      # keep the top 94.5% of a web capture: the taskbar (Wi-Fi name, clock) is below

INK, BG, ORANGE, MUTED = (22, 24, 27), (214, 215, 210), (238, 106, 44), (64, 67, 72)
FPS = 30

# The timeline in bars (4 beats). 16 bars at 128 BPM = 30 s; the end card holds for TAIL seconds more.
SHOTS_BARS = [('hook', 2), ('native', 2), ('web', 3), ('anima', 2), ('games', 3), ('install', 2), ('end', 2)]
TAIL = 1.5

_font_cache = {}


def F(kind, size):
    key = (kind, size)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(str(SITE / 'fonts' / FONTS[kind]), size)
    return _font_cache[key]


def ease(x):                       # ease-out cubic, clamped
    x = min(max(x, 0.0), 1.0)
    return 1 - (1 - x) ** 3


def lerp(a, b, k):
    return a + (b - a) * k


def native(name):
    return Image.open(SHOTS / f'{name}.en.classic.png').convert('RGBA')


def device(board, screen, width):
    """The website's device cut-out with a native UI render in its display."""
    dev = Image.open(DEVICE / f'{board}.webp').convert('RGBA')
    dev = dev.resize((width, round(dev.height * width / dev.width)), Image.LANCZOS)
    if screen is not None:
        l, t, w, h = SCREEN_PCT[board]
        sw, sh = round(w / 100 * dev.width), round(h / 100 * dev.height)
        dev.alpha_composite(screen.resize((sw, sh), Image.LANCZOS), (round(l / 100 * dev.width), round(t / 100 * dev.height)))
    return dev


def shadowed(im, blur=22, dy=24, alpha=110):
    """im with a soft contact shadow under it (returned canvas is padded)."""
    pad = blur * 3
    out = Image.new('RGBA', (im.width + 2 * pad, im.height + 2 * pad + dy), (0, 0, 0, 0))
    sh = Image.new('RGBA', out.size, (0, 0, 0, 0))
    sh.paste((20, 22, 26, alpha), (pad + 6, pad + dy), im.getchannel('A'))
    out.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))
    out.alpha_composite(im, (pad, pad))
    return out, pad


def web(name):
    im = Image.open(WEB / f'{name}.webp').convert('RGBA')
    return im.crop((0, 0, im.width, round(im.height * TASKBAR_CROP)))


def fit_browser(view, vw, vh):
    """The widest browser window around view that fits a vw x vh box."""
    win = browser(view, vw)
    if win.height > vh:
        win = browser(view, round(vw * vh / win.height))
    return win


def browser(view, width, url='http://nucleo-01.local'):
    """A browser window around a web capture (the capture is fitted to the window width)."""
    bar = max(28, round(width * 0.034))
    vh = round(view.height * (width - 4) / view.width)
    win = Image.new('RGBA', (width, vh + bar + 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(win)
    r = round(bar * 0.45)
    d.rounded_rectangle((0, 0, width - 1, win.height - 1), r, fill=(32, 36, 48))
    dot = bar * 0.32
    for i, c in enumerate([(255, 95, 87), (254, 188, 46), (40, 200, 64)]):
        cx = bar * 0.62 + i * bar * 0.72
        d.ellipse((cx - dot / 2, bar / 2 - dot / 2, cx + dot / 2, bar / 2 + dot / 2), fill=c)
    ux = bar * 2.9
    d.rounded_rectangle((ux, bar * 0.2, width - bar * 0.5, bar * 0.8), bar * 0.3, fill=(52, 58, 74))
    uf = F('sans', round(bar * 0.42))
    d.text((ux + bar * 0.5, bar / 2), url, font=uf, fill=(196, 204, 220), anchor='lm')
    v = view.resize((width - 4, vh), Image.LANCZOS)
    mask = Image.new('L', v.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, -r, v.width - 1, v.height - 1), r, fill=255)
    win.paste(v, (2, bar), mask)
    return win


class Text:
    """A pre-rendered caption block: title lines (display font) + optional subtitle (sans)."""

    def __init__(self, title, sub, width, size, sub_size, color=INK):
        self.lines = title
        f = F('display', size)
        probe = ImageDraw.Draw(Image.new('L', (1, 1)))
        while max(probe.textlength(l, font=f) for l in title) > width and size > 20:
            size -= 2
            f = F('display', size)
        self.size = size
        lh = round(size * 0.92)
        sf = F('sans', sub_size)
        subs = []
        for para in (sub or []):                         # wrap the subtitle to the block width
            words, line = para.split(), ''
            for w in words:
                t = (line + ' ' + w).strip()
                if probe.textlength(t, font=sf) > width and line:
                    subs.append(line)
                    line = w
                else:
                    line = t
            subs.append(line)
        sh = round(sub_size * 1.38)
        H = lh * len(title) + (round(sub_size * 0.9) + sh * len(subs) if subs else 0) + round(size * 0.2)
        self.im = Image.new('RGBA', (width, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(self.im)
        y = 0
        self.line_y = []
        for l in title:
            self.line_y.append(y)
            d.text((0, y), l, font=f, fill=color)
            y += lh
        y += round(sub_size * 0.9)
        for s in subs:
            d.text((2, y), s, font=sf, fill=MUTED)
            y += sh
        self.lh = lh


def paste_alpha(canvas, im, xy, alpha=1.0):
    if alpha <= 0:
        return
    if alpha < 1:
        im = im.copy()
        im.putalpha(im.getchannel('A').point(lambda v: round(v * alpha)))
    canvas.alpha_composite(im, (round(xy[0]), round(xy[1])))


def fn_badge(d, x, y, h, label):
    pf = F('pixel', round(h * 0.6))
    d.rounded_rectangle((x, y, x + h * 1.6, y + h), round(h * 0.2), fill=ORANGE)
    d.text((x + h * 0.8, y + h / 2), 'fn', font=pf, fill=INK, anchor='mm')
    d.text((x + h * 1.95, y + h / 2), label, font=pf, fill=INK, anchor='lm')


class Scene:
    """Base: a caption block that slides in, a visual area. wide = 16:9, else square."""

    def __init__(self, W, H, beat):
        self.W, self.H, self.beat = W, H, beat
        self.wide = W / H > 1.3
        self.m = round(min(W, H) * 0.065)
        if self.wide:
            self.cap_w = round(W * 0.34)
            self.vis = (self.m + self.cap_w + round(self.m * 0.7), round(self.m * 0.7), W - round(self.m * 0.6),
                        H - round(self.m * 0.7))   # visual box
        else:
            self.cap_w = W - 2 * self.m
            self.vis = (self.m, round(H * 0.36), W - self.m, H - self.m)

    def caption(self, title, sub, size=None, sub_size=None):
        size = size or (round(self.H * 0.115) if self.wide else round(self.W * 0.1))
        sub_size = sub_size or round(min(self.W, self.H) * 0.03)
        txt = Text(title, sub, self.cap_w, size, sub_size)
        if not self.wide:                               # square: the visual goes below the tallest caption
            top = round(self.m * 0.9 + txt.im.height + self.m * 0.5)
            self.vis = (self.m, max(self.vis[1], top) if hasattr(self, '_capped') else top, self.W - self.m, self.H - self.m)
            self._capped = True
        return txt

    def draw_caption(self, c, txt, t, stagger=0.4):
        """Title lines slide up one after another (stagger in beats), then the subtitle."""
        if self.wide:
            x0, y0 = self.m, (self.H - txt.im.height) / 2
        else:
            x0, y0 = self.m, self.m * 0.9
        rise = txt.lh * 0.35
        n = len(txt.lines)
        for i in range(n + 1):                          # band i: title line i, or the subtitle (i == n)
            top = txt.line_y[i] if i < n else (txt.line_y[-1] + txt.lh)
            bot = txt.line_y[i + 1] if i + 1 < n else (txt.line_y[-1] + txt.lh if i < n else txt.im.height)
            if bot <= top:
                continue
            k = ease((t - i * stagger * self.beat) / (self.beat * 0.6))
            band = txt.im.crop((0, top, txt.im.width, bot))
            paste_alpha(c, band, (x0, y0 + top + (1 - k) * rise), k)

    def fit(self, im, box, scale=1.0):
        bw, bh = box[2] - box[0], box[3] - box[1]
        s = min(bw / im.width, bh / im.height) * scale
        return s


def base_canvas(W, H):
    return Image.new('RGBA', (W, H), BG)


# ---- the shots ---------------------------------------------------------------------------------------

class Hook(Scene):
    def __init__(self, *a):
        super().__init__(*a)
        size = round(self.H * 0.15) if self.wide else round(self.W * 0.13)
        self.txt = Text(['A MICROCONTROLLER', "THAT THINKS IT'S", 'A WORKSTATION'], None,
                        self.cap_w + (round(self.W * 0.04) if self.wide else 0), size, 10)
        vw = self.vis[2] - self.vis[0]
        dw = round(vw * (0.95 if self.wide else 0.92))
        self.dev, self.pad = shadowed(device('adv', native('launcher.home'), dw))

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        k = ease(t / (self.beat * 1.5))
        if self.wide:
            x = lerp(self.W + 40, self.vis[0] + (self.vis[2] - self.vis[0] - self.dev.width) / 2 + 40, k)
            y = (self.H - self.dev.height) / 2 + self.H * 0.04
        else:
            x = lerp(self.W + 40, (self.W - self.dev.width) / 2, k)
            y = self.H - self.dev.height + self.pad * 0.6
        c.alpha_composite(self.dev, (round(x), round(y)))
        self.draw_caption(c, self.txt, t, stagger=1.0)
        return c


class Native(Scene):
    SCREENS = ['launcher.home', 'launcher.cat-games', 'launcher.cc', 'settings.anima']

    def __init__(self, *a):
        super().__init__(*a)
        self.txt = self.caption(['50+ NATIVE', 'APPS & GAMES'], ['Big fonts and quick keys, made for a 240x135 screen.'])
        vw, vh = self.vis[2] - self.vis[0], self.vis[3] - self.vis[1]
        dw = round(min(vw * 1.0, vh / 0.645 * 0.98))
        self.devs = [shadowed(device('adv', native(s), dw)) for s in self.SCREENS]

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        i = min(int(t / (self.beat * 2)), len(self.devs) - 1)      # a new screen every two beats
        dev, pad = self.devs[i]
        bx = self.vis[0] + (self.vis[2] - self.vis[0] - dev.width) / 2
        by = self.vis[1] + (self.vis[3] - self.vis[1] - dev.height) / 2 + pad * 0.3
        k = ease(t / (self.beat * 0.8))
        c.alpha_composite(dev, (round(bx + (1 - k) * 80), round(by)))
        self.draw_caption(c, self.txt, t)
        return c


class Web(Scene):
    CAPS = ['desktop', 'start', 'terminal']

    def __init__(self, *a):
        super().__init__(*a)
        self.txt = self.caption(['A DESKTOP', 'IN YOUR', 'BROWSER'],
                                ['Open nucleo-01.local: the Cardputer itself serves 40+ web apps.', 'About 6x faster in 0.7.'])
        vw = self.vis[2] - self.vis[0]
        vh = self.vis[3] - self.vis[1]
        self.wins = [shadowed(fit_browser(web(n), vw, vh), blur=18, dy=16, alpha=90) for n in self.CAPS]

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        per = self.dur / len(self.wins)
        i = min(int(t / per), len(self.wins) - 1)
        win, pad = self.wins[i]
        tl = t - i * per
        bx = self.vis[0] + (self.vis[2] - self.vis[0] - win.width) / 2
        by = self.vis[1] + (self.vis[3] - self.vis[1] - win.height) / 2 + pad * 0.2
        k = ease(tl / (self.beat * 0.6))
        c.alpha_composite(win, (round(bx), round(by + (1 - k) * 40)))
        self.draw_caption(c, self.txt, t)
        return c


class Anima(Scene):
    def __init__(self, *a):
        super().__init__(*a)
        self.txt = self.caption(['ANIMA:', 'AI THAT WORKS', 'OFFLINE'],
                                ["Answers, calculates, opens apps with no cloud. Or let it use your PC's models: Ollama, LM Studio."])
        vw, vh = self.vis[2] - self.vis[0], self.vis[3] - self.vis[1]
        src = web('anima')
        # the conversation column, not the empty sidebar: crop to the chat pane
        src = src.crop((round(src.width * 0.18), 0, src.width, src.height))
        self.win, self.pad = shadowed(fit_browser(src, round(vw * 0.96), round(vh * 0.96)), blur=18, dy=16, alpha=90)

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        z = 1.0 + 0.04 * (t / self.dur)                       # a slow push-in
        win = self.win.resize((round(self.win.width * z), round(self.win.height * z)), Image.BILINEAR)
        bx = self.vis[0] + (self.vis[2] - self.vis[0] - win.width) / 2
        by = self.vis[1] + (self.vis[3] - self.vis[1] - win.height) / 2
        k = ease(t / (self.beat * 0.6))
        paste_alpha(c, win, (bx, by + (1 - k) * 40), k)
        self.draw_caption(c, self.txt, t)
        return c


class Games(Scene):
    def __init__(self, *a):
        super().__init__(*a)
        self.txt_a = self.caption(['COSTELLAZIONI'], ['A 6DOF space game: fly, fight, explore a galaxy.'])
        self.txt_b = self.caption(['GAME BOY +', 'GAME GEAR'], ['Native emulators. The Game Gear core is written from scratch.'])
        vw, vh = self.vis[2] - self.vis[0], self.vis[3] - self.vis[1]
        self.pics = []
        for n in ('costellazioni-combat', 'costellazioni-world'):
            im = Image.open(WEB / f'{n}.webp').convert('RGBA')
            s = min(vw / im.width, vh / im.height)
            im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
            mask = Image.new('L', im.size, 0)
            ImageDraw.Draw(mask).rounded_rectangle((0, 0, im.width - 1, im.height - 1), round(min(im.size) * 0.03), fill=255)
            im.putalpha(mask)
            self.pics.append(shadowed(im, blur=18, dy=16, alpha=90))
        dw = round(min(vw, vh / 0.645 * 0.98))
        self.dev = shadowed(device('original', native('launcher.cat-games'), dw))

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        bar = self.beat * 4
        if t < 2 * bar:                                        # two bars of Costellazioni
            i = 0 if t < bar else 1
            pic, pad = self.pics[i]
            tl = t - i * bar
            z = 1.0 + 0.035 * tl / bar
            p = pic.resize((round(pic.width * z), round(pic.height * z)), Image.BILINEAR)
            bx = self.vis[0] + (self.vis[2] - self.vis[0] - p.width) / 2
            by = self.vis[1] + (self.vis[3] - self.vis[1] - p.height) / 2
            k = ease(tl / (self.beat * 0.6))
            paste_alpha(c, p, (bx, by + (1 - k) * 40), k)
            self.draw_caption(c, self.txt_a, t)
        else:                                                  # one bar: the emulators
            tl = t - 2 * bar
            dev, pad = self.dev
            bx = self.vis[0] + (self.vis[2] - self.vis[0] - dev.width) / 2
            by = self.vis[1] + (self.vis[3] - self.vis[1] - dev.height) / 2 + pad * 0.3
            k = ease(tl / (self.beat * 0.8))
            c.alpha_composite(dev, (round(bx + (1 - k) * 80), round(by)))
            self.draw_caption(c, self.txt_b, tl, stagger=0.2)
        return c


class Install(Scene):
    def __init__(self, *a):
        super().__init__(*a)
        self.txt = self.caption(['INSTALLS', 'ITSELF'],
                                ['Downloads and verifies its own SD content. Runs as a guest under M5Launcher.',
                                 'One image for Cardputer + ADV.'])
        vw, vh = self.vis[2] - self.vis[0], self.vis[3] - self.vis[1]
        dw = round(min(vw * 0.62, vh * 0.98 / 0.645 / 1.45))
        self.states = []
        for scr in ('wizard.install-run', 'wizard.install-done'):
            s = native(scr)
            self.states.append((shadowed(device('original', s, dw)), shadowed(device('adv', s, dw))))

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        st = self.states[0 if t < self.beat * 4 else 1]
        (a, pa), (b, pb) = st
        vw, vh = self.vis[2] - self.vis[0], self.vis[3] - self.vis[1]
        k1 = ease(t / (self.beat * 0.8))
        k2 = ease((t - self.beat * 0.5) / (self.beat * 0.8))
        # staggered pair: original back-left, ADV front-right
        ax = self.vis[0] - pa * 0.6
        ay = self.vis[1] + vh * 0.02 - pa * 0.5
        bxx = self.vis[2] - b.width + pb * 0.6
        byy = self.vis[3] - b.height + pb * 0.9
        paste_alpha(c, a, (ax, ay + (1 - k1) * 60), k1)
        paste_alpha(c, b, (bxx, byy + (1 - k2) * 60), k2)
        self.draw_caption(c, self.txt, t)
        return c


class End(Scene):
    def __init__(self, *a):
        super().__init__(*a)
        S = round(min(self.W, self.H) * (0.26 if self.wide else 0.22))
        self.icon = Image.open(SITE / 'img' / 'icon-512.png').convert('RGBA').resize((S, S), Image.LANCZOS)
        self.icon, self.ipad = shadowed(self.icon, blur=16, dy=14, alpha=80)

    def frame(self, t):
        c = base_canvas(self.W, self.H)
        d = ImageDraw.Draw(c)
        W, H = self.W, self.H
        k = ease(t / (self.beat * 1.0))
        ts = round(H * 0.17) if self.wide else round(W * 0.15)
        lf, lk = F('bold', round(min(W, H) * 0.042)), F('sans', round(min(W, H) * 0.042))
        links = [('GitHub', 'github.com/indecenti/NucleoOs'), ('Discord', 'discord.gg/84tyTPtWus')]
        if self.wide:
            ix = round(W * 0.12)
            iy = (H - self.icon.height) / 2
            paste_alpha(c, self.icon, (ix, iy + (1 - k) * 40), k)
            x = ix + self.icon.width - self.ipad + round(W * 0.03)
            y = H * 0.25
        else:
            ix = (W - self.icon.width) / 2
            iy = H * 0.06
            paste_alpha(c, self.icon, (ix, iy + (1 - k) * 40), k)
            x = self.m * 1.4
            y = iy + self.icon.height - self.ipad * 0.4
        tl = Image.new('RGBA', (W, round(ts * 1.2)), (0, 0, 0, 0))
        ImageDraw.Draw(tl).text((0, 0), 'NucleoOS', font=F('display', ts), fill=INK)
        k2 = ease((t - self.beat * 0.5) / (self.beat * 0.9))
        paste_alpha(c, tl, (x, y + (1 - k2) * 30), k2)
        y += ts * 1.15
        for i, (label, url) in enumerate(links):
            k3 = ease((t - self.beat * (1.2 + 0.6 * i)) / (self.beat * 0.8))
            row = Image.new('RGBA', (W, round(lf.size * 1.5)), (0, 0, 0, 0))
            rd = ImageDraw.Draw(row)
            rd.text((0, 0), label, font=lf, fill=ORANGE)
            rd.text((lf.size * 4.2, 0), url, font=lk, fill=INK)
            paste_alpha(c, row, (x + 4, y + (1 - k3) * 24), k3)
            y += lf.size * 1.55
        k4 = ease((t - self.beat * 2.6) / (self.beat * 0.8))
        bl = Image.new('RGBA', (W, round(min(W, H) * 0.05)), (0, 0, 0, 0))
        fn_badge(ImageDraw.Draw(bl), 0, 0, bl.height - 2, 'CARDPUTER + ADV  ·  OFFLINE AI')
        paste_alpha(c, bl, (x + 4, y + min(W, H) * 0.03), k4)
        return c


SCENES = {'hook': Hook, 'native': Native, 'web': Web, 'anima': Anima, 'games': Games, 'install': Install, 'end': End}


def first_beat(audio):
    import librosa
    y, sr = librosa.load(str(audio), sr=22050, mono=True)
    tempo, beats = librosa.beat.beat_track(y=y, sr=sr, units='time')
    return float(beats[0]), float(tempo if not hasattr(tempo, '__len__') else tempo[0])


def build(fmt, W, H, audio, bpm, offset, out, stills=False):
    beat = 60.0 / bpm
    bar = beat * 4
    scenes = []
    t0 = 0.0
    for name, bars in SHOTS_BARS:
        dur = bars * bar + (TAIL if name == 'end' else 0)
        s = SCENES[name](W, H, beat)
        s.dur = dur
        scenes.append((name, t0, dur, s))
        t0 += dur
    total = t0
    nframes = round(total * FPS)
    dst = out / f'nucleoos-promo-{fmt}.mp4'
    fade = 1.2
    cmd = ['ffmpeg', '-y', '-loglevel', 'error',
           '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', str(FPS), '-i', '-',
           '-ss', f'{offset:.3f}', '-i', str(audio),
           '-map', '0:v', '-map', '1:a', '-t', f'{total:.3f}',
           '-af', f'afade=t=out:st={total - fade:.3f}:d={fade}',
           '-vf', f'fade=t=out:st={total - 0.4:.3f}:d=0.4',
           '-c:v', 'libx264', '-preset', 'slow', '-crf', '20', '-maxrate', '5M', '-bufsize', '10M',
           '-pix_fmt', 'yuv420p', '-profile:v', 'high', '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', str(dst)]
    p = None if stills else subprocess.Popen(cmd, stdin=subprocess.PIPE)
    mids = []
    want = {round((st + dur * 0.62) * FPS) for _, st, dur, _ in scenes}
    for f in range(nframes):
        if stills and f not in want:
            continue
        t = f / FPS
        for name, st, dur, s in scenes:
            if st <= t < st + dur or (s is scenes[-1][3]):
                im = s.frame(t - st)
                break
        if p:
            p.stdin.write(im.convert('RGB').tobytes())
        for name, st, dur, s in scenes:                    # remember a frame from the middle of each shot
            if f == round((st + dur * 0.62) * FPS):
                mids.append((name, im.convert('RGB')))
        if f % 150 == 0:
            print(f'  {fmt}: {f}/{nframes}', flush=True)
    if p:
        p.stdin.close()
        if p.wait():
            sys.exit(f'ffmpeg failed for {fmt}')
    # contact sheet: every shot's middle frame
    tw = 640
    th = round(tw * H / W)
    cols = 4
    rows = (len(mids) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * tw, rows * (th + 30)), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(mids):
        x, y = (i % cols) * tw, (i // cols) * (th + 30)
        sheet.paste(im.resize((tw, th), Image.LANCZOS), (x, y + 30))
        d.text((x + 8, y + 6), name, font=F('bold', 18), fill=(230, 230, 230))
    sheet.save(out / f'contact-{fmt}.jpg', quality=88)
    for name, im in mids:
        im.save(out / f'frame-{fmt}-{name}.png')
    if p:
        print(f'{dst.relative_to(REPO)}  {total:.2f}s  {dst.stat().st_size / 1e6:.1f} MB')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--audio', required=True)
    ap.add_argument('--bpm', type=float, default=128)
    ap.add_argument('--offset', type=float, help='seconds into the audio where the first downbeat is')
    ap.add_argument('--only', choices=['16x9', '1x1'])
    ap.add_argument('--stills', action='store_true', help='only the contact sheet + shot frames, no video')
    ap.add_argument('--out', default=str(REPO / 'build' / 'promo'))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    offset = a.offset
    if offset is None and a.stills:
        offset = 0.0
    if offset is None:
        offset, tempo = first_beat(a.audio)
        print(f'first beat {offset:.3f}s, librosa tempo {tempo:.1f} (using --bpm {a.bpm})')
    for fmt, W, H in (('16x9', 1920, 1080), ('1x1', 1080, 1080)):
        if a.only in (None, fmt):
            build(fmt, W, H, a.audio, a.bpm, offset, out, a.stills)


if __name__ == '__main__':
    main()
