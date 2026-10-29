#!/usr/bin/env python3
"""A store emulator's console picture for the Games gallery: console-<key>.gfa, shipped in the app's store package.

    python tools/gen-console-art.py <studio photo> <out .gfa> [--preview out.png]

The built-in consoles' pictures are compiled into the firmware (tools/gen-gallery-art.py -> gamefront_art.h). An
emulator installed from the Store brings its own instead: the store puts every file of the signed package in
/sd/apps/<id>/, and the gallery reads the console it declares (console.txt) and this picture from there. The same cut-out
(soft edges, the studio white unmixed out of them) and the same encoding as the built-in ones:

    "GFA1", u8 w, u8 h, u32 zlen (LE), z    the photo, fitted into 118 x 80
            u8 w, u8 h, u32 zlen (LE), z    the mark beside its name, fitted into 20 x 15

z = RGB565 (native order) then 4-bit alpha (two pixels a byte, high nibble first), raw deflate.
"""
import argparse
import importlib.util
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gallery_art", os.path.join(HERE, "gen-gallery-art.py"))
art = importlib.util.module_from_spec(spec)
spec.loader.exec_module(art)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("photo")
    ap.add_argument("out")
    ap.add_argument("--preview", help="the photo and the mark on the gallery's dark background, to review")
    a = ap.parse_args()
    cut = art.cut_out(Image.open(a.photo))
    blob, parts = b"GFA1", []
    for box, margin in ((art.PHOTO, 0.02), (art.MARK, 0.0)):
        img = art.fit(cut, box, margin)
        z = art.encode(img)
        blob += struct.pack("<BBI", img.width, img.height, len(z)) + z
        parts.append(img)
    with open(a.out, "wb") as f:
        f.write(blob)
    if a.preview:
        sheet = Image.new("RGBA", (parts[0].width + parts[1].width + 12, max(p.height for p in parts) + 8), (16, 18, 22, 255))
        sheet.alpha_composite(parts[0], (4, 4))
        sheet.alpha_composite(parts[1], (parts[0].width + 8, 4))
        sheet.convert("RGB").resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(a.preview)
    print(f"gen-console-art: {a.out} {len(blob)} B (photo {parts[0].width}x{parts[0].height}, mark {parts[1].width}x{parts[1].height})")


if __name__ == "__main__":
    main()
