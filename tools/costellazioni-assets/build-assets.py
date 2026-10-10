#!/usr/bin/env python3
"""Turn the generated Costellazioni art and music into the shipped, compressed set.

  raw/<id>.png    (gen-images.mjs, git-ignored)  ->  apps/games/www/games/stelle/assets/img/<id>.<hash>.avif
  music/<id>.wav  (ACE-Step renders, git-ignored) ->  apps/games/www/games/stelle/assets/music/<id>.<hash>.ogg

Every shipped file carries a content hash in its name, so the browser-side store (games/stelle/assets.js)
downloads a file once and keeps it in IndexedDB until the hash changes. assets/manifest.json lists them with
size, kind, priority and a two-colour placeholder gradient for the instant first paint.

  python -I tools/costellazioni-assets/build-assets.py [--check]
"""
import hashlib, json, os, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(REPO, 'apps', 'games', 'www', 'games', 'stelle', 'assets')
RAW = os.path.join(HERE, 'raw')
MUSIC = os.path.join(HERE, 'music')
BUDGET = 40 * 1024 * 1024                      # the user's total asset budget (images + music)

# AVIF quality per kind: painterly art hides artefacts well; faces and emblems need cleaner edges.
QUALITY = {'art': 54, 'cast': 62, 'emblem': 60}
PRIORITY = {'title': 0, 'emblem': 1, 'cast': 2}   # lower loads first; everything else streams after
MUSIC_KBPS = 64                                   # Opus, stereo: transparent enough for synth-orchestral
MUSIC_LUFS = -16.0                                # one level for the whole score


def short_hash(data):
    return hashlib.sha1(data).hexdigest()[:10]


def cover(im, w, h):
    """Scale to cover w x h, centre-crop: keeps the composition, no letterboxing."""
    s = max(w / im.width, h / im.height)
    im = im.resize((max(w, round(im.width * s)), max(h, round(im.height * s))), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def placeholder(im):
    """Top and bottom average colours: a gradient the page paints before the image arrives."""
    t = im.resize((1, 2), Image.BOX)
    return ['#%02x%02x%02x' % t.getpixel((0, 0)), '#%02x%02x%02x' % t.getpixel((0, 1))]


def write_unique(sub, base, ext, data):
    """assets/<sub>/<base>.<hash>.<ext>; older hashes of the same asset are removed."""
    d = os.path.join(OUT, sub)
    os.makedirs(d, exist_ok=True)
    name = f'{base}.{short_hash(data)}.{ext}'
    for f in os.listdir(d):
        if f.startswith(base + '.') and f.endswith('.' + ext) and f != name and f.count('.') == 2:
            os.remove(os.path.join(d, f))
    p = os.path.join(d, name)
    if not os.path.exists(p):
        with open(p, 'wb') as fh:
            fh.write(data)
    return f'{sub}/{name}'


def build_images(spec, man):
    import io
    for img in spec['images']:
        src = os.path.join(RAW, img['id'] + '.png')
        if not os.path.exists(src):
            print(f'  missing raw/{img["id"]}.png (not generated yet)')
            continue
        kind = img.get('kind', 'art')
        w, h = img['out']
        im = cover(Image.open(src).convert('RGB'), w, h)
        buf = io.BytesIO()
        im.save(buf, 'AVIF', quality=QUALITY.get(kind, 54), speed=2, subsampling='4:2:0')
        data = buf.getvalue()
        man['images'][img['id']] = {
            'f': write_unique('img', img['id'], 'avif', data), 'w': w, 'h': h, 'b': len(data), 'k': kind,
            'p': PRIORITY.get(img['id'], PRIORITY.get(kind, 3)), 'ph': placeholder(im),
        }


def loudness(path):
    """Integrated loudness (LUFS) of a track, EBU R128."""
    r = subprocess.run(['ffmpeg', '-hide_banner', '-nostats', '-i', path, '-af', 'ebur128=framelog=quiet',
                        '-f', 'null', '-'], capture_output=True, text=True, check=True)
    lines = [l for l in r.stderr.splitlines() if l.strip().startswith('I:')]
    return float(lines[-1].split()[1])


def build_music(man):
    if not os.path.isdir(MUSIC):
        return
    for f in sorted(os.listdir(MUSIC)):
        base, ext = os.path.splitext(f)
        if ext.lower() not in ('.wav', '.flac', '.mp3'):
            continue
        src = os.path.join(MUSIC, f)
        # every track at the same level, under the game's effects; the trailing silence trimmed so the
        # player's loop crossfade starts on music, with a short fade so the cut never clicks
        gain = MUSIC_LUFS - loudness(src)
        af = (f'volume={gain:.2f}dB,alimiter=limit=0.89:level=false,'
              'areverse,silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.3,'
              'afade=t=in:d=0.6,areverse')
        r = subprocess.run(['ffmpeg', '-v', 'error', '-i', src, '-af', af, '-fflags', '+bitexact', '-flags:a', '+bitexact', '-c:a', 'libopus',
                            '-b:a', f'{MUSIC_KBPS}k', '-vbr', 'on', '-compression_level', '10',
                            '-application', 'audio', '-map_metadata', '-1', '-f', 'ogg', '-'],
                           capture_output=True, check=True)
        man['music'][base] = {'f': write_unique('music', base, 'ogg', r.stdout), 'b': len(r.stdout),
                              'p': 0 if base == 'theme' else 4}


def main():
    spec = json.load(open(os.path.join(HERE, 'prompts.json'), encoding='utf-8'))
    man = {'v': 1, 'images': {}, 'music': {}}
    build_images(spec, man)
    build_music(man)
    # drop files that no manifest entry points to any more (renamed or removed assets)
    live = {e['f'] for g in ('images', 'music') for e in man[g].values()}
    for sub in ('img', 'music'):
        d = os.path.join(OUT, sub)
        for f in (os.listdir(d) if os.path.isdir(d) else []):
            if f'{sub}/{f}' not in live:
                os.remove(os.path.join(d, f))
    total = sum(e['b'] for g in ('images', 'music') for e in man[g].values())
    man['bytes'] = total
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'manifest.json'), 'w', encoding='utf-8', newline='\n') as fh:
        json.dump(man, fh, separators=(',', ':'), sort_keys=True)
        fh.write('\n')
    by = {}
    for e in man['images'].values():
        by[e['k']] = by.get(e['k'], 0) + e['b']
    for k, b in sorted(by.items()):
        print(f'  {k:7s} {b / 1024:8.0f} KB')
    print(f'  music   {sum(e["b"] for e in man["music"].values()) / 1024:8.0f} KB')
    print(f'{len(man["images"])} images, {len(man["music"])} tracks, {total / 1048576:.1f} MB (budget {BUDGET / 1048576:.0f} MB)')
    if total > BUDGET:
        sys.exit('over budget')


if __name__ == '__main__':
    main()
