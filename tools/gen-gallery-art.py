#!/usr/bin/env python3
"""The consoles' pictures for the Games gallery (gamefront.cpp): cut out of their studio photos with soft edges.

    python tools/gen-gallery-art.py <folder with the photos and cardputer-generated.png>

The photos are Evan-Amos's public-domain shots on Wikimedia Commons (the 1280 px renditions are plenty):
Game-Boy-FL.jpg, Nintendo-Game-Boy-Color-FL.jpg, Game-Gear-Handheld.jpg, Sega-Master-System-Console-FL.jpg,
Nintendo-Entertainment-System-NES-Console-FL.jpg. For each console:

  1. the white studio background is found by a flood from the photo's edges (a light part of the console is not
     reached: it is not connected to the edge through background);
  2. alpha is soft only where the background meets the console: how far a pixel is from the studio white;
  3. the console's own colour is recovered under the edge (the white is "unmixed" out of it), so no light fringe
     is left on a dark screen;
  4. it is cropped to the console and scaled down premultiplied: a photo (PHOTO box) and a small mark (MARK box).

Written to firmware/components/nucleo_app/gamefront_art.h as RGB565 (native order) followed by 4-bit alpha (two
pixels a byte), raw deflate: the device inflates a picture once and blends it on the theme's background.
gamefront_art/<key>.png, <key>_s.png: the cut-outs on a dark background, to review.
"""
import os
import sys
import zlib
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

PHOTO = (118, 80)                     # the picture box inside the gallery's left column
MARK = (26, 18)                       # a console's mark in the wheel, beside its name
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "firmware", "components", "nucleo_app")
SRC = {
    "gb": "Game-Boy-FL.jpg",
    "gbc": "Nintendo-Game-Boy-Color-FL.jpg",
    "gg": "Game-Gear-Handheld.jpg",
    "sms": "Sega-Master-System-Console-FL.jpg",
    "nes": "Nintendo-Entertainment-System-NES-Console-FL.jpg",
    # The native games' entry: a pocket computer painted locally with Qwen-Image 2.1 (ComfyUI, tools/qwen_assets.py grid,
    # seed 4242, the top-right panel of the prompt in gamefront_art/README.md) - no photo, no licence.
    "native": "cardputer-generated.png",
}


def cut_out(img, near_d=28, ramp0=12):
    """RGBA with the studio background transparent and the edge colours unmixed from it."""
    a = np.asarray(img.convert("RGB"), dtype=np.float32)
    soft = np.asarray(img.convert("RGB").filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float32)   # the paper's grain out
    h, w, _ = a.shape
    border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
    bg = np.median(border, axis=0)
    dist = np.sqrt(((soft - bg) ** 2).sum(axis=2))
    # The background: reachable from the edges through pixels close to the studio white.
    near = dist < near_d
    seen = np.zeros((h, w), dtype=bool)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            if near[y, x] and not seen[y, x]:
                seen[y, x] = True
                q.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if near[y, x] and not seen[y, x]:
                seen[y, x] = True
                q.append((y, x))
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and not seen[ny, nx] and near[ny, nx]:
                seen[ny, nx] = True
                q.append((ny, nx))
    # Alpha: 1 inside the console; in the background region, a ramp on the distance from the white (its soft edge).
    alpha = np.where(seen, np.clip((dist - ramp0) / 20, 0, 1), 1.0).astype(np.float32)
    # The console's own colour where it was blended with the white: c = (obs - (1 - a) * bg) / a.
    am = np.maximum(alpha, 1e-3)[..., None]
    fg = np.clip((a - (1 - am) * bg) / am, 0, 255)
    fg = np.where(alpha[..., None] > 0.02, fg, 0)
    rgba = np.dstack([fg, alpha * 255]).astype(np.uint8)
    return Image.fromarray(rgba, "RGBA")


def fit(rgba, box, margin):
    """Crop to the console (plus a margin) and scale it into box, premultiplied (no dark or light halo)."""
    bb = rgba.getchannel("A").point(lambda v: 255 if v > 24 else 0).getbbox()
    l, t, r, b = bb
    mx, my = int((r - l) * margin), int((b - t) * margin)
    rgba = rgba.crop((max(0, l - mx), max(0, t - my), min(rgba.width, r + mx), min(rgba.height, b + my)))
    k = min(box[0] / rgba.width, box[1] / rgba.height)
    size = (max(1, round(rgba.width * k)), max(1, round(rgba.height * k)))
    return rgba.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")


def encode(rgba):
    """RGB565 (native little-endian) then 4-bit alpha, two pixels a byte (high nibble first), raw deflate."""
    px = np.asarray(rgba, dtype=np.uint16)
    r, g, b, a = px[..., 0], px[..., 1], px[..., 2], px[..., 3]
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    c = np.where(a < 8, 0, c).astype("<u2")
    a4 = ((a.astype(np.uint32) * 15 + 127) // 255).astype(np.uint8).flatten()
    if len(a4) % 2:
        a4 = np.append(a4, 0)
    packed = (a4[0::2] << 4) | a4[1::2]
    raw = c.tobytes() + packed.astype(np.uint8).tobytes()
    co = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return co.compress(raw) + co.flush()


def preview(rgba, path):
    bg = Image.new("RGBA", rgba.size, (16, 18, 22, 255))
    Image.alpha_composite(bg, rgba).convert("RGB").save(path, optimize=True)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    os.makedirs(os.path.join(OUT, "gamefront_art"), exist_ok=True)
    lines = ["// Generated by tools/gen-gallery-art.py - do not edit. Console photos: Evan-Amos, public domain",
             "// (Wikimedia Commons), cut out with soft edges. Each: RGB565 (native order), then 4-bit alpha (two pixels a",
             "// byte, high nibble first), raw deflate. gamefront.cpp blends them on the theme's background.",
             "#pragma once", "#include <stdint.h>", ""]
    total = 0
    for key, name in SRC.items():
        img = Image.open(os.path.join(folder, name))
        if key == "native":                        # a panel of a 2x2 grid: its edge carries the grid's grey lines
            img = img.crop((10, 10, img.width - 10, img.height - 10))
        # The painted one has a soft grey shadow under it: it is part of the background here (a halo on a dark screen).
        cut = cut_out(img, 60, 45) if key == "native" else cut_out(img)
        for suffix, box, margin in (("", PHOTO, 0.02), ("_S", MARK, 0.0)):
            img = fit(cut, box, margin)
            preview(img, os.path.join(OUT, "gamefront_art", f"{key}{suffix.lower()}.png"))
            z = encode(img)
            total += len(z)
            sym = f"GFA_{key.upper()}{suffix}"
            lines.append(f"#define {sym}_W {img.width}")
            lines.append(f"#define {sym}_H {img.height}")
            lines.append(f"static const uint8_t {sym}_Z[{len(z)}] = {{")
            for i in range(0, len(z), 32):
                lines.append("    " + ",".join(str(v) for v in z[i:i + 32]) + ",")
            lines.append("};")
        lines.append("")
    with open(os.path.join(OUT, "gamefront_art.h"), "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"gen-gallery-art: {len(SRC)} consoles, {total} B deflated -> gamefront_art.h")


if __name__ == "__main__":
    main()
