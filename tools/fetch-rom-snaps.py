#!/usr/bin/env python3
r"""Game screenshots for the emulators' gallery, fetched on the PC and put on the device's card.

    python tools/fetch-rom-snaps.py --host <device ip> --pin-file <file holding the pairing PIN> [--sheet out.png] [--force]
    python tools/fetch-rom-snaps.py --card E:\                  (the card in the PC's reader instead)

The device never downloads anything (the emulators run with the radio off). This reads the cartridges the Games
gallery found anywhere on the card (its index, /data/GameShots/.cache/roms.idx; without one, the folders
/data/ROMs/gb, gbc, gg, sms) over the paired file API, finds a screenshot for every cartridge and writes it next to
it, in <its folder>/.snaps/, as <rom file>.snp (the emulators' shelf) and <rom file>.pic (the gallery: ready to
stream, nothing to unpack on the device). A cartridge that already has a .snp gets its .pic from it.

Where a screenshot comes from, in order:
  1. the folder's LICENSES.txt (written with the free homebrew library) names each game's page:
     Homebrew Hub (hh.gbdev.io/game/<slug>)   -> the entry's first screenshot in github.com/gbdev/database
     a GitHub repository                       -> its best-named screenshot / title image
  2. libretro-thumbnails (Named_Snaps), by the cartridge's name, for the commercial No-Intro sets.
Nothing is committed anywhere: the pictures stay on that card. A cartridge with none keeps the console photo.

.snp: "SNP1", u16 w, u16 h, u32 deflated size, then raw-deflated RGB565 (native byte order) - fit into 112 x 101
(a Game Boy / Game Gear screen at 0.7, a Master System screen at 112 x 84). app_gbemu.cpp reads it.
.pic: "PIC1", u16 w, u16 h, u32 0, u32 0, then RGB565 in the panel's byte order (high byte first). gamefront.cpp.
"""
import argparse
import io
import json
import os
import re
import struct
import sys
import urllib.parse
import urllib.request
import zlib

from PIL import Image

UA = {"User-Agent": "NucleoOS-rom-snaps/1.0 (+https://github.com/indecenti/NucleoOs)"}
BOX = (112, 101)
FOLDERS = {"gb": ("Nintendo_-_Game_Boy", (160, 144)), "gbc": ("Nintendo_-_Game_Boy_Color", (160, 144)),
           "gg": ("Sega_-_Game_Gear", (160, 144)), "sms": ("Sega_-_Master_System_-_Mark_III", (256, 192)),
           "nes": ("Nintendo_-_Nintendo_Entertainment_System", (256, 240))}
ROM_EXT = (".gb", ".gbc", ".gg", ".sms", ".zip", ".nes")
GOOD = ("screenshot", "screen", "shot", "gameplay", "title")
BAD = ("logo", "icon", "font", "tile", "sprite", "palette", "map", "button", "banner", "cover", "box", "label")


def get(url, data=None, headers=None, jar=None, timeout=60):
    h = dict(UA, **(headers or {}))
    if jar:
        h["Cookie"] = jar
    req = urllib.request.Request(url, data=data, headers=h, method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read(), r.headers


class Device:
    def __init__(self, host, pin):
        self.base = f"http://{host}"
        _, hd = get(self.base + "/api/pair", json.dumps({"pin": pin}).encode(), {"Content-Type": "application/json"})
        self.jar = "; ".join(c.split(";")[0] for c in hd.get_all("Set-Cookie") or [])

    def ls(self, path):
        try:
            body, _ = get(f"{self.base}/api/fs/list?path={urllib.parse.quote(path)}", jar=self.jar)
            return json.loads(body).get("entries", [])
        except Exception:
            return []

    def read(self, path):
        try:
            return get(f"{self.base}/api/fs/read?path={urllib.parse.quote(path)}", jar=self.jar)[0]
        except Exception:
            return b""

    def mkdir(self, path):
        try:
            get(f"{self.base}/api/fs/mkdir?path={urllib.parse.quote(path)}", b"", jar=self.jar)
        except Exception:
            pass

    def write(self, path, data):
        body, _ = get(f"{self.base}/api/fs/write?path={urllib.parse.quote(path)}", data,
                      {"Content-Type": "application/octet-stream"}, self.jar, 120)
        return json.loads(body).get("ok", False)


class Card:
    """The same four calls on a card mounted on the PC (or any folder laid out like one)."""
    def __init__(self, root):
        self.root = root

    def _p(self, path):
        return os.path.join(self.root, path.lstrip("/"))

    def ls(self, path):
        p = self._p(path)
        if not os.path.isdir(p):
            return []
        return [{"name": n, "type": "dir" if os.path.isdir(os.path.join(p, n)) else "file"} for n in os.listdir(p)]

    def read(self, path):
        try:
            return open(self._p(path), "rb").read()
        except OSError:
            return b""

    def mkdir(self, path):
        os.makedirs(self._p(path), exist_ok=True)

    def write(self, path, data):
        open(self._p(path), "wb").write(data)
        return True


def licenses(text):
    """'<file>  -  <title> by <author>  -  <licence>  -  <url>' lines -> {file: url}."""
    out = {}
    for line in text.splitlines():
        parts = [p.strip() for p in line.split("  -  ")]
        if len(parts) >= 4 and parts[0].lower().endswith(ROM_EXT) and parts[-1].startswith("http"):
            out[parts[0]] = parts[-1]
    return out


def gh_tree(repo):
    meta = json.loads(get(f"https://api.github.com/repos/{repo}")[0])
    br = meta.get("default_branch", "master")
    tree = json.loads(get(f"https://api.github.com/repos/{repo}/git/trees/{br}?recursive=1")[0]).get("tree", [])
    return br, [t["path"] for t in tree if t.get("type") == "blob"]


def image_candidates(repo):
    br, paths = gh_tree(repo)
    scored = []
    for p in paths:
        low = p.lower()
        if not low.endswith((".png", ".gif", ".jpg", ".jpeg", ".bmp")):
            continue
        name = os.path.basename(low)
        s = sum(3 if k == "screenshot" else 2 if k in ("screen", "shot", "gameplay") else 1 for k in GOOD if k in low)
        s -= sum(4 for k in BAD if k in name)
        if s > 0:
            scored.append((-s, len(p), p))
    return [f"https://raw.githubusercontent.com/{repo}/{br}/{urllib.parse.quote(p)}" for _, _, p in sorted(scored)]


def hh_candidates(slug):
    e = json.loads(get(f"https://hh3.gbdev.io/api/entry/{slug}.json")[0])
    base = e.get("basepath", "database-gb")
    repo = {"database-gb": "gbdev/database"}.get(base, "gbdev/database")
    return [f"https://raw.githubusercontent.com/{repo}/master/entries/{slug}/{urllib.parse.quote(s)}" for s in e.get("screenshots", [])]


def norm(s):
    s = os.path.splitext(s)[0].lower()
    s = re.sub(r"[\(\[].*?[\)\]]", " ", s).replace("&", " and ")
    return " ".join(re.sub(r"[^a-z0-9]+", " ", s).split())


ROMAN = {"1": "i", "2": "ii", "3": "iii", "4": "iv", "5": "v", "6": "vi"}


def squash(s):
    """norm() without spaces and with 2 = II: 'Megaman 2' and 'Mega Man II' meet, so do 'Duck Tales' / 'DuckTales'."""
    return "".join(ROMAN.get(w, w) for w in norm(s).split())


_libretro = {}


def libretro_candidates(folder, rom):
    repo, _ = FOLDERS[folder]
    if repo not in _libretro:
        try:
            _, paths = gh_tree("libretro-thumbnails/" + repo)
        except Exception:
            paths = []
        _libretro[repo] = [p for p in paths if p.startswith("Named_Snaps/") and p.endswith(".png")]
    want, stem = norm(rom), os.path.splitext(rom)[0].replace("&", "_")
    hits = [p for p in _libretro[repo] if os.path.basename(p)[:-4] == stem]          # the very same No-Intro name
    hits += [p for p in _libretro[repo] if p not in hits and norm(os.path.basename(p)) == want]
    hits += [p for p in _libretro[repo] if p not in hits and squash(os.path.basename(p)) == squash(rom)]
    head = squash(rom.split(" - ")[0]) if " - " in rom else None                  # the title without its subtitle
    hits += [p for p in _libretro[repo] if p not in hits and head and squash(os.path.basename(p)) == head]
    hits += [p for p in _libretro[repo] if p not in hits and norm(os.path.basename(p)).startswith(want + " ")]
    # Among equals, the international release first, then anything but betas and hacks.
    rank = lambda p: (0 if re.search(r"\((USA|World|USA, Europe)\)", p) else 1, 1 if re.search(r"Beta|Hack|Unl|Proto", p) else 0)
    exact = [p for p in hits if os.path.basename(p)[:-4] == stem]
    hits = exact + sorted((p for p in hits if p not in exact), key=rank)
    return [f"https://raw.githubusercontent.com/libretro-thumbnails/{repo}/master/{urllib.parse.quote(p)}" for p in hits[:3]]


def screen_like(img, native):
    """A screenshot, not an asset sheet: roughly the console's aspect and at least a screen big."""
    w, h = img.size
    a, na = w / h, native[0] / native[1]
    return w >= native[0] * 0.8 and h >= native[1] * 0.8 and abs(a - na) / na < 0.25


def to_snap(img, native):
    img = img.convert("RGB")
    w, h = img.size
    k = min(w // native[0], h // native[1])
    if k >= 2 and w % native[0] == 0 and h % native[1] == 0:          # an integer upscale: undo it exactly
        img = img.resize((w // k, h // k), Image.NEAREST)
    img.thumbnail(BOX, Image.LANCZOS)
    raw = bytearray()
    for r, g, b in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()):
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        raw += bytes((v & 0xFF, v >> 8))
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    z = c.compress(bytes(raw)) + c.flush()
    return img, b"SNP1" + struct.pack("<HHI", img.width, img.height, len(z)) + z


SYS_KEYS = list(FOLDERS)                       # the index's system digits, in gamefront.cpp's order


def rgb565(img):
    out = []
    for r, g, b in (img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata()):
        out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return out


def pic_bytes(w, h, px):
    return b"PIC1" + struct.pack("<HHII", w, h, 0, 0) + b"".join(struct.pack(">H", v) for v in px)


def snp_to_pic(blob):
    if blob[:4] != b"SNP1":
        return None
    w, h, n = struct.unpack("<HHI", blob[4:12])
    raw = zlib.decompress(blob[12:12 + n], -15)
    return pic_bytes(w, h, struct.unpack("<%dH" % (w * h), raw))


def cartridges(dev):
    """(system key, folder, file) for every cartridge: from the gallery's index, else the four ROM folders."""
    idx = dev.read("/data/GameShots/.cache/roms.idx").decode("utf-8", "replace").splitlines()
    out, folder = [], None
    if idx and idx[0].startswith("GIX1"):
        for line in idx[1:]:
            if line.startswith("D "):
                folder = line[2:]
                folder = folder[3:] if folder.startswith("/sd/") else folder   # a device path -> a card path
            elif len(line) > 2 and line[0].isdigit() and line[1] == " " and folder is not None:
                out.append((SYS_KEYS[int(line[0])], folder, line[2:]))
        return out
    for key in FOLDERS:
        root = f"/data/ROMs/{key}"
        out += [(key, root, e["name"]) for e in dev.ls(root) if e.get("type") == "file" and e["name"].lower().endswith(ROM_EXT)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="the device, paired over Wi-Fi (with --pin-file)")
    ap.add_argument("--pin-file")
    ap.add_argument("--card", help="a card mounted on the PC instead of --host")
    ap.add_argument("--sheet", help="also write a contact sheet of every snap (PNG) here")
    ap.add_argument("--force", action="store_true", help="replace the snaps already on the card")
    a = ap.parse_args()
    if a.card:
        dev = Card(a.card)
    elif a.host and a.pin_file:
        dev = Device(a.host, open(os.path.expanduser(a.pin_file)).read().strip())
    else:
        ap.error("--host with --pin-file, or --card")
    done, shots = [], []
    have, lic = {}, {}
    for folder, root, rom in sorted(cartridges(dev), key=lambda c: (c[1], c[2].lower())):
        native = FOLDERS[folder][1]
        if root not in have:
            have[root] = {e["name"] for e in dev.ls(root + "/.snaps")}
            lic[root] = licenses(dev.read(root + "/LICENSES.txt").decode("utf-8", "replace"))
            dev.mkdir(root + "/.snaps")
        if rom + ".snp" in have[root] and not a.force:
            if rom + ".pic" not in have[root]:          # an older card: the gallery's picture from the .snp
                pic = snp_to_pic(dev.read(f"{root}/.snaps/{rom}.snp"))
                ok = bool(pic) and dev.write(f"{root}/.snaps/{rom}.pic", pic)
                print(f"  {'pic ' if ok else 'FAIL'}  {root}/{rom}")
            else:
                print(f"  have  {root}/{rom}")
            continue
        url = lic[root].get(rom, "")
        cands = []
        try:
            m = re.search(r"hh\.gbdev\.io/game/([\w.-]+)", url)
            g = re.search(r"github\.com/([\w.-]+/[\w.-]+)", url)
            if m:
                cands = hh_candidates(m.group(1))
            elif g:
                cands = image_candidates(g.group(1))
            cands += libretro_candidates(folder, rom)
        except Exception as e:
            print(f"  ?     {root}/{rom}: {e}")
        picked = None
        for u in cands[:8]:
            try:
                img = Image.open(io.BytesIO(get(u)[0]))
                img.load()
            except Exception:
                continue
            if screen_like(img, native):
                picked = (u, img)
                break
        if not picked:
            print(f"  none  {root}/{rom}  (the console's photo stays)")
            continue
        img, blob = to_snap(picked[1], native)
        ok = dev.write(f"{root}/.snaps/{rom}.snp", blob) and dev.write(f"{root}/.snaps/{rom}.pic", pic_bytes(img.width, img.height, rgb565(img)))
        print(f"  {'put' if ok else 'FAIL'}   {root}/{rom}  {img.width}x{img.height} {len(blob)} B  <- {picked[0]}")
        if ok:
            done.append(rom)
            shots.append((rom, img))
    if a.sheet and shots:
        cols = 5
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (BOX[0] + 8) + 8, rows * (BOX[1] + 8) + 8), (16, 18, 22))
        for i, (_, im) in enumerate(shots):
            sheet.paste(im, (8 + (i % cols) * (BOX[0] + 8), 8 + (i // cols) * (BOX[1] + 8)))
        sheet.save(a.sheet)
    print(f"fetch-rom-snaps: {len(done)} snap(s) written")


if __name__ == "__main__":
    sys.exit(main())
