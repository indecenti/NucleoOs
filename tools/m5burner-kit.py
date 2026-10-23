#!/usr/bin/env python3
"""Build the M5Burner publication kit for a released NucleoOS version.

M5Burner has no upload API: the upload is an account-bound step on WebBurner (https://burner.m5stack.com,
Developer > Upload firmware, signed in with the project's M5Stack account; the desktop app is discontinued). This tool prepares everything that
step needs, reproducibly, so nothing hand-made can be lost (the old kit lived in dist/, which
tools/package-release.mjs wipes):

  build/m5burner/nucleoos-<full>.bin   the release's MERGED image (0x0), downloaded from the GitHub Release,
                                       SHA-256 checked against its SHA256SUMS, layout + identity verified
  build/m5burner/cover.png             1600x900 (16:9, the catalog's card shape) with the REAL native UI
                                       renders of `npm run ui:shots` (build/ui-host/shots, English)
  build/m5burner/listing.md            paste-ready form fields + the steps, and how to verify the listing

Usage:  python tools/m5burner-kit.py [--version 0.7.2] [--bin <local merged image>] [--out build/m5burner]
See docs/m5launcher.md "Publishing so it shows up in the Launcher's OTA list".
"""
import argparse, hashlib, json, re, struct, sys, urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GH_REPO = 'indecenti/NucleoOs'
GH_URL = f'https://github.com/{GH_REPO}'
SHOTS = REPO / 'build' / 'ui-host' / 'shots'
# The cover's four screens (English, classic theme) — real renders of the shipped native UI.
COVER_SHOTS = ['launcher.home', 'launcher.cat-games', 'launcher.cc', 'wizard.install-done']

# The website checkout (sibling of this repo) holds the brand assets: the device cut-out (M5Stack product
# photo, see its photos/SOURCE.md), the web desktop capture and the OFL fonts.
SITE = Path(__import__('os').environ.get('NUCLEOOS_SITE', REPO.parent / 'nucleos_sites'))
SITE_DEVICE = SITE / 'img' / 'device' / 'adv.webp'
SITE_SCREEN_PCT = (25.170, 6.715, 29.006, 25.318)  # screen box in adv.webp, % (site make-assets.py SCREEN_PCT)
SITE_DESKTOP = SITE / 'img' / 'shots' / 'en' / 'desktop.webp'
SITE_FONTS = {k: SITE / 'fonts' / f for k, f in {'display': 'big-shoulders-700.woff', 'sans-bold': 'work-sans-700.woff',
                                                 'sans': 'work-sans-400.woff', 'pixel': 'silkscreen-400.woff'}.items()}

NAME = 'NucleoOS: desktop + offline AI for Cardputer & ADV'   # the hook shows in the catalog list


def fail(msg):
    print(f'm5burner-kit: {msg}', file=sys.stderr)
    sys.exit(1)


def http_get(url, accept=None):
    req = urllib.request.Request(url, headers={'User-Agent': 'nucleoos-m5burner-kit', **({'Accept': accept} if accept else {})})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def fetch_release_image(version, out):
    rel = json.loads(http_get(f'https://api.github.com/repos/{GH_REPO}/releases/tags/v{version}', 'application/vnd.github+json'))
    assets = {a['name']: a['browser_download_url'] for a in rel.get('assets', [])}
    pat = re.compile(rf'^nucleoos-{re.escape(version)}\.\d+\.bin$')          # merged image, not -ota.bin
    names = [n for n in assets if pat.match(n)]
    if len(names) != 1: fail(f'release v{version}: expected one merged image, found {names or "none"}')
    if 'SHA256SUMS' not in assets: fail(f'release v{version} has no SHA256SUMS')
    sums = {}
    for line in http_get(assets['SHA256SUMS']).decode().splitlines():
        parts = line.split()
        if len(parts) == 2: sums[parts[1].lstrip('*')] = parts[0].lower()
    name = names[0]
    data = http_get(assets[name])
    got = hashlib.sha256(data).hexdigest()
    if sums.get(name) != got: fail(f'{name}: SHA-256 {got} does not match SHA256SUMS ({sums.get(name)})')
    dst = out / name
    dst.write_bytes(data)
    return dst, got


def verify_image(data, version):
    """Merged image at 0x0: bootloader, our partition table at 0x8000, a NucleoOS app of this version."""
    if len(data) < 0x20000 or data[0] != 0xE9: fail('not a merged image (no bootloader image at 0x0)')
    apps = []
    for off in range(0x8000, 0x8C00, 32):
        e = data[off:off + 32]
        if e[:2] != b'\xaa\x50': continue
        typ, sub = e[2], e[3]
        poff, size = struct.unpack('<II', e[4:12])
        label = e[12:28].split(b'\0')[0].decode(errors='replace')
        if typ == 0: apps.append((label, sub, poff, size))
    labels = sorted(a[0] for a in apps)
    if labels != ['ota_0', 'ota_1']: fail(f'partition table app slots {labels}: not NucleoOS\'s own A/B layout')
    app = min(a[2] for a in apps)                                            # the image is written to the first bank
    if len(data) <= app + 0x100 or data[app] != 0xE9: fail(f'no app image at 0x{app:x}')
    desc = app + 0x20                                                        # image header (24) + segment header (8)
    if struct.unpack('<I', data[desc:desc + 4])[0] != 0xABCD5432: fail('app descriptor magic missing')
    ver = data[desc + 16:desc + 48].split(b'\0')[0].decode()
    proj = data[desc + 48:desc + 80].split(b'\0')[0].decode()
    if proj != 'nucleoos': fail(f'project name "{proj}" is not nucleoos')
    if not ver.startswith(version + '+') and ver != version: fail(f'app version {ver} is not {version}')
    if ver.endswith('*'): fail(f'app version {ver} was built from a dirty tree')
    return ver, app


def build_cover(version, dst):
    # The catalog shows covers as small cards next to dozens of look-alike forks: the hook has to read at
    # thumbnail size. Preferred cover = the website's brand (headline, real device photo with the real native
    # UI on its screen, the web desktop it serves); without the site checkout, the plain screenshot grid.
    if all(p.exists() for p in (SITE_DEVICE, SITE_DESKTOP, *SITE_FONTS.values())):
        return build_brand_cover(version, dst)
    from PIL import Image, ImageDraw, ImageFont
    W, H = 1280, 960
    img = Image.new('RGB', (W, H))
    px = img.load()
    top, bot = (10, 14, 30), (22, 18, 48)                                    # navy -> indigo, vertical
    for y in range(H):
        t = y / (H - 1)
        c = tuple(round(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(W): px[x, y] = c
    d = ImageDraw.Draw(img)

    def font(bold, size):
        cands = (['segoeuib.ttf', 'DejaVuSans-Bold.ttf', 'Arial Bold.ttf'] if bold
                 else ['segoeui.ttf', 'DejaVuSans.ttf', 'Arial.ttf'])
        for c in cands:
            for p in (Path('C:/Windows/Fonts') / c, Path('/usr/share/fonts/truetype/dejavu') / c, Path(c)):
                try: return ImageFont.truetype(str(p), size)
                except OSError: pass
        return ImageFont.load_default()

    ACC = (167, 139, 250)                                                    # ANIMA violet
    d.text((92, 46), 'NucleoOS', font=font(True, 112), fill=(255, 255, 255))
    d.text((96, 186), 'for Cardputer & ADV', font=font(False, 48), fill=ACC)
    pill = f'v{version}'
    pf = font(True, 34)
    bw = d.textlength(pill, font=pf) + 40
    d.rounded_rectangle((W - 92 - bw, 74, W - 92, 74 + 58), radius=29, outline=ACC, width=3)
    d.text((W - 92 - bw + 20, 80), pill, font=pf, fill=ACC)

    sw, sh, gap = 516, 290, 30
    x0, y0 = (W - (2 * sw + gap)) // 2, 266
    for i, name in enumerate(COVER_SHOTS):
        p = SHOTS / f'{name}.en.classic.png'
        if not p.exists(): fail(f'{p} missing: run `npm run ui:shots` first')
        shot = Image.open(p).convert('RGB').resize((sw, sh), Image.LANCZOS)
        x, y = x0 + (i % 2) * (sw + gap), y0 + (i // 2) * (sh + gap)
        d.rounded_rectangle((x - 8, y - 8, x + sw + 8, y + sh + 8), radius=18, fill=(4, 6, 14), outline=(58, 68, 110), width=2)
        img.paste(shot, (x, y))
    tag = 'Native apps  \u00b7  Offline assistant  \u00b7  Web console  \u00b7  Safe A/B updates'
    tf = font(False, 30)
    d.text(((W - d.textlength(tag, font=tf)) / 2, y0 + 2 * sh + gap + 22), tag, font=tf, fill=(176, 186, 214))
    img.save(dst, optimize=True)


def build_brand_cover(version, dst):
    # 16:9: the catalog (burner.m5stack.com) shows covers in 16:9 cards with object-fit: cover, so any other
    # shape loses its top and bottom. Text column left, the pictures right, nothing important near the edges.
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
    W, H = 1600, 900
    INK, BG, ORANGE = (22, 24, 27), (214, 215, 210), (238, 106, 44)
    F = lambda k, s: ImageFont.truetype(str(SITE_FONTS[k]), s)
    im = Image.new('RGBA', (W, H), BG)
    d = ImageDraw.Draw(im)

    d.text((72, 64), 'NucleoOS', font=F('sans-bold', 44), fill=INK)
    pill, pf = f'v{version}', F('pixel', 26)
    bw = d.textlength(pill, font=pf) + 36
    x = 72 + d.textlength('NucleoOS', font=F('sans-bold', 44)) + 24
    d.rounded_rectangle((x, 70, x + bw, 116), radius=10, fill=ORANGE)
    d.text((x + 18, 77), pill, font=pf, fill=INK)

    lines = ['A MICROCONTROLLER', "THAT THINKS IT'S", 'A WORKSTATION']
    size, col = 150, 600                             # largest size whose longest line fits the text column
    while d.textlength(max(lines, key=lambda l: d.textlength(l, font=F('display', size))), font=F('display', size)) > col:
        size -= 2
    y = 210
    for line in lines:
        d.text((66, y), line, font=F('display', size), fill=INK)
        y += round(size * 0.9)
    tf = F('sans', 30)
    for row in ('Desktop in your browser.', '50+ apps. Offline AI.'):
        d.text((72, y + 34), row, font=tf, fill=(60, 63, 68))
        y += 42

    # The web desktop the Cardputer serves, in a browser window, the device in front of it.
    bx0, by0, bx1, by1 = 730, 110, W - 64, 640
    shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((bx0 + 6, by0 + 14, bx1 + 6, by1 + 14), 16, fill=(20, 22, 26, 90))
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))
    d.rounded_rectangle((bx0, by0, bx1, by1), 14, fill=(32, 36, 48))
    for i, c in enumerate([(255, 95, 87), (254, 188, 46), (40, 200, 64)]):
        d.ellipse((bx0 + 18 + i * 24, by0 + 13, bx0 + 32 + i * 24, by0 + 27), fill=c)
    d.rounded_rectangle((bx0 + 100, by0 + 9, bx1 - 18, by0 + 31), 11, fill=(52, 58, 74))
    d.text((bx0 + 116, by0 + 10), 'http://nucleo-01.local', font=F('sans', 16), fill=(190, 198, 214))
    vw, vh = bx1 - bx0 - 4, by1 - by0 - 42
    desk = Image.open(SITE_DESKTOP).convert('RGBA')
    cw = min(desk.width, 1180)                       # the icon grid, not the empty right half
    crop = desk.crop((0, 36, cw, 36 + round(cw * vh / vw)))   # below the capture's mouse pointer
    view = crop.resize((vw, vh), Image.LANCZOS)
    mask = Image.new('L', (vw, vh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, -20, vw - 1, vh - 1), 12, fill=255)
    im.paste(view, (bx0 + 2, by0 + 40), mask)

    dev = Image.open(SITE_DEVICE).convert('RGBA')
    dw = 480
    dev = dev.resize((dw, round(dev.height * dw / dev.width)), Image.LANCZOS)
    l, t, w, h = SITE_SCREEN_PCT
    scr = Image.open(SHOTS / 'launcher.home.en.classic.png').convert('RGBA')
    dev.alpha_composite(scr.resize((round(w / 100 * dev.width), round(h / 100 * dev.height)), Image.LANCZOS),
                        (round(l / 100 * dev.width), round(t / 100 * dev.height)))
    dx, dy = 690, H - 50 - dev.height
    sh = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    sh.paste((20, 22, 26, 110), (dx + 10, dy + 26), dev.getchannel('A'))
    im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(22)))
    im.alpha_composite(dev, (dx, dy))

    sf = F('pixel', 24)                              # the site's "fn" key badge
    d.rounded_rectangle((72, 792, 136, 832), 8, fill=ORANGE)
    d.text((86, 799), 'fn', font=sf, fill=INK)
    d.text((152, 799), 'CARDPUTER + ADV  ·  OFFLINE AI', font=sf, fill=INK)
    im.convert('RGB').save(dst, optimize=True)


# The first line is all most people read in the catalog: lead with what no other Cardputer firmware does.
DESCRIPTION = """**Turn your Cardputer into a real computer:** a full desktop in your browser, 50+ native apps and games, and an AI assistant that works offline.

Type `nucleo-01.local` in any browser on your Wi-Fi and the Cardputer itself serves you a desktop: files, spreadsheet, paint, terminal, SSH, music and video, 40+ apps. In your hands, a fast launcher with big readable fonts and **ANIMA**, an assistant that answers, calculates, sets timers and opens apps with no cloud at all.

### Highlights
- **ANIMA offline assistant** in English and Italian (it can also use a model on your PC or in the cloud)
- **Web desktop** served by the device: 40+ apps in any browser on your network
- **50+ native apps and games**: Game Boy and Game Gear emulator, Tanks, Pinball, music and video player, voice recorder, IR remote, Wi-Fi and BLE tools
- **One binary for Cardputer and Cardputer ADV**: the board is detected at boot
- **5 languages**: EN, IT, ES, FR, DE
- **Safe A/B updates** with automatic rollback; also runs as a guest under M5Launcher

### Install
1. Burn this image (same file for Cardputer and Cardputer ADV).
2. On first boot the setup wizard joins your Wi-Fi and downloads the SD card content (web desktop, apps) by itself, or copy `nucleoos-{full}-sd.zip` from the GitHub release to a FAT32 microSD.
3. Open the address shown on the screen in your browser.

Free for noncommercial use (PolyForm Noncommercial 1.0.0); commercial use needs a license. Includes security-testing tools: authorized testing, research and education only.

Source, docs and release notes: {gh}"""


def write_listing(version, full, binp, sha, appver, dst):
    desc = DESCRIPTION.format(full=full, gh=GH_URL)
    dst.write_text(f"""# M5Burner listing - NucleoOS v{version}

Upload on **WebBurner** (https://burner.m5stack.com), signed in with the project's M5Stack account:
Developer > **Upload firmware**, fill the form, visibility **Public** (M5Stack reviews it; *Pending* until
then; LauncherHub / the M5Launcher OTA list mirror the public catalog).
Next release: open the same entry and add a version instead of creating a new one.

| Field | Value |
|---|---|
| Name | `{NAME}` |
| Version | `v{version}` |
| Category | `AI & Assistants`, `UI & Launcher` |
| Supported devices | `Cardputer`, `Cardputer ADV` |
| Project Link | `{GH_URL}` |
| Firmware package | `{binp.name}` (merged image, flashed at 0x0) |
| Cover | `cover.png` (1600x900, 16:9) |

Firmware check: SHA-256 `{sha}` (matches the release's SHA256SUMS); app `{appver}`, project `nucleoos`,
own A/B partition table.

## Firmware description (Markdown, paste as is)

```
{desc}
```

## After publishing

1. From the catalog, burn it once to a Cardputer from the catalog: it must boot NucleoOS (`/api/status` `ota.self_update:true`).
2. Within a day LauncherHub mirrors the public catalog:
   `curl "https://api.launcherhub.net/firmwares?category=cardputer&q=nucleoos"` must list it.
3. From an M5Launcher: OTA > search "NucleoOS" > install. It must boot as a guest (`ota.host:"m5launcher"`).
""", encoding='utf-8')
    (dst.parent / 'description.txt').write_text(desc + '\n', encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--version', default=(REPO / 'firmware' / 'version' / 'VERSION').read_text().strip())
    ap.add_argument('--bin', help='use this local merged image instead of downloading the release asset')
    ap.add_argument('--out', default=str(REPO / 'build' / 'm5burner'))
    a = ap.parse_args()
    version = a.version.lstrip('v')
    if not re.fullmatch(r'\d+\.\d+\.\d+', version): fail(f'bad version {a.version}')
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    if a.bin:
        src = Path(a.bin); data = src.read_bytes(); sha = hashlib.sha256(data).hexdigest()
        binp = out / src.name
        if src.resolve() != binp.resolve(): binp.write_bytes(data)
    else:
        binp, sha = fetch_release_image(version, out)
        data = binp.read_bytes()
    appver, app = verify_image(data, version)
    full = re.match(r'^nucleoos-(.+)\.bin$', binp.name).group(1) if binp.name.startswith('nucleoos-') else version
    build_cover(version, out / 'cover.png')
    write_listing(version, full, binp, sha, appver, out / 'listing.md')
    print(f'm5burner-kit: {out}')
    print(f'  {binp.name}  {len(data)} B  sha256 {sha[:16]}...  app {appver} @0x{app:x}')
    print('  cover.png, listing.md, description.txt')


if __name__ == '__main__':
    main()
