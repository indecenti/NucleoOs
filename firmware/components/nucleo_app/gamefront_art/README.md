# Games gallery: console pictures

Cut out by `tools/gen-gallery-art.py` from Evan-Amos's photos on Wikimedia Commons (public domain):
Game-Boy-FL.jpg, Nintendo-Game-Boy-Color-FL.jpg, Game-Gear-Handheld.jpg, Sega-Master-System-Console-FL.jpg,
Nintendo-Entertainment-System-NES-Console-FL.jpg (their 1280 px renditions). `<key>.png` is the photo the gallery
shows, `<key>_s.png` the mark beside a console's name, both previewed on a dark background; the firmware carries
them as `gamefront_art.h` (RGB565 + 4-bit alpha, deflated) and blends them on the theme's background.

`native.png` (the native games' entry) is not a photo: a pocket computer painted on this PC with Qwen-Image 2.1
(ComfyUI, `qwen_assets.py grid`, seed 4242, top-right panel) from: "A 2x2 grid of four studio product photographs, each
panel shows one tiny handheld pocket computer the size of a credit card: a small rectangular color LCD screen in the
top part and a full QWERTY keyboard of many tiny round keys below it, dark charcoal grey plastic body with a few orange
keys, a little antenna stub on the side, photographed from a three-quarter view from above, sharp focus, soft even
studio lighting, isolated on a pure white seamless background, no shadow under it, no text, no logo, no hands".
