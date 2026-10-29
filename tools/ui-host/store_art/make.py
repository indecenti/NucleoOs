#!/usr/bin/env python3
"""The store art packs the gallery's Store scenes find on the card (sd/apps/.store/<ch>/art.bin), in the format
NucleoOS reads (nucleo_store/store_engine.h st_art_*): the fictional seeded apps (store_stubs.cpp) wear real store
icons; a few keep none on purpose, so the fallback tile is in the shots too.

    python tools/ui-host/store_art/make.py <store checkout>    (needs the store's art sources: apps/*/*/store-icon.png)
"""
import os, struct, sys
from PIL import Image

store = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
src = lambda ch, app: os.path.join(store, "apps", ch, app, "store-icon.png")
chan = lambda n: os.path.join(store, "art", "channels", n + ".png")
PACKS = {
    "web": [("neon-snake", src("native", "snake")), ("sky-pilot", src("native", "stelle")), ("metronome", src("web", "metronome")),
            ("unit-convert", src("native", "units")), ("flashcards", src("native", "cardler")), ("notes-md", src("web", "scoreboard")),
            ("@native", chan("native")), ("@web", chan("web"))],
    "native": [("stelle", src("native", "stelle")), ("poker", src("native", "poker")), ("@native", chan("native")), ("@web", chan("web"))],
}
for ch, items in PACKS.items():
    head = b"NART" + bytes([1, 48]) + struct.pack("<H", len(items)) + ch.encode().ljust(8, b"\0")
    table, data = bytearray(), bytearray()
    for app, p in items:
        raw = Image.open(p).convert("RGB").resize((48, 48), Image.LANCZOS).tobytes()
        table += app.encode().ljust(36, b"\0") + struct.pack("<I", len(head) + 40 * len(items) + len(data))
        for i in range(0, len(raw), 3):
            data += struct.pack("<H", (raw[i] >> 3) << 11 | (raw[i + 1] >> 2) << 5 | raw[i + 2] >> 3)
    open(os.path.join(here, ch + ".bin"), "wb").write(head + table + data)
    print(ch, len(items), "icons")
