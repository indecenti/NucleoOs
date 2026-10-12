#!/usr/bin/env python3
"""Build the M5Burner publication kit for a released NucleoOS version.

M5Burner has no upload API: the upload is a manual, account-bound step in the M5Burner DESKTOP app
(USER CUSTOM > Publish, signed in with the project's M5Stack account). This tool prepares everything that
step needs, reproducibly, so nothing hand-made can be lost (the old kit lived in dist/, which
tools/package-release.mjs wipes):

  build/m5burner/nucleoos-<full>.bin   the release's MERGED image (0x0), downloaded from the GitHub Release,
                                       SHA-256 checked against its SHA256SUMS, layout + identity verified
  build/m5burner/cover.png             1280x960 (4:3, the catalog's cover shape) from the REAL native UI
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

NAME = 'NucleoOS for Cardputer & ADV'
DEVICE_TYPE = 'Cardputer'


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


DESCRIPTION = """NucleoOS - an appliance OS for the M5Stack Cardputer and Cardputer ADV. One binary: the board is detected at boot.

- Native launcher with apps and games, large readable fonts, 5 languages (EN, IT, ES, FR, DE)
- ANIMA: a built-in offline assistant (no cloud needed; can also use your PC's or a cloud model)
- Web console: open the Cardputer's address in any browser on your network for a full desktop (files, editor, media, terminal, games)
- Safe updates: A/B OTA with automatic rollback. Installed from M5Launcher, it updates from the Launcher's OTA list

Install: burn this image. On first boot the setup wizard joins your Wi-Fi and can download the SD card content (web OS, apps) by itself - or copy nucleoos-{full}-sd.zip from the GitHub release onto the SD card.

Free for noncommercial use (PolyForm Noncommercial 1.0.0); commercial use needs a license. Includes security-testing tools: authorized testing, research and education only.
Source, docs and release notes: {gh}"""


def write_listing(version, full, binp, sha, appver, dst):
    desc = DESCRIPTION.format(full=full, gh=GH_URL)
    dst.write_text(f"""# M5Burner listing - NucleoOS v{version}

Upload in the **M5Burner desktop app** (no web upload exists): sign in (top right) with the project's
M5Stack account, then **USER CUSTOM > Publish**, fill the form, **Upload**. The entry starts in your list;
use its **Publish** toggle to make it public (that is what LauncherHub / the M5Launcher OTA list mirror).
Next release: open the same entry and add a version instead of creating a new one.

| Field | Value |
|---|---|
| Name | `{NAME}` |
| Version | `v{version}` |
| Device Type | `{DEVICE_TYPE}` (covers the ADV too) |
| GitHub | `{GH_URL}` |
| FirmWare | `{binp.name}` (merged image, flashed at 0x0) |
| Cover | `cover.png` (1280x960) |

Firmware check: SHA-256 `{sha}` (matches the release's SHA256SUMS); app `{appver}`, project `nucleoos`,
own A/B partition table.

## Description (paste as is)

```
{desc}
```

## After publishing

1. In M5Burner, burn it once to a Cardputer from the catalog: it must boot NucleoOS (`/api/status` `ota.self_update:true`).
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
    print('  cover.png 1280x960, listing.md, description.txt')


if __name__ == '__main__':
    main()
