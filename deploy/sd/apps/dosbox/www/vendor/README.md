# js-dos vendor payload (not in git)

DOS Box needs the js-dos engine. Loaded from the CDN it works only while the
console has internet — which is exactly the case an offline handheld does not
have. This folder is the local mirror; the app probes `vendor/js-dos.js` and
falls back to the CDN only when the mirror is missing.

**The files themselves are deliberately NOT in the repo** (~11MB of third-party
minified JS/WASM). Same rule the Godot/FRT runtime already follows in
`nucleos-arena/vendor/frt`: provenance and checksums are versioned, the binary
is fetched.

```sh
sh nucleos-webos/tools/fetch-jsdos.sh            # download + write SHA256SUMS
sh nucleos-webos/tools/fetch-jsdos.sh --verify   # re-check an existing copy
```

## Provenance

| | |
|---|---|
| Upstream | js-dos v8 — DOSBox / DOSBox-X compiled to WebAssembly |
| Source | `https://v8.js-dos.com/latest/` |
| Fetched | 2026-07-21 |
| Licence | js-dos and the DOSBox cores it embeds are **GPL**. They stay a separate, unmodified payload precisely so their licence is not entangled with NucleoOS code — do not edit these files, re-fetch them. |

`latest` is a moving tag: a re-fetch can legitimately return different bytes.
That is what `SHA256SUMS` is for — a mismatch means upstream moved and the new
build should be tried before it ships, not that the download is corrupt.

## Layout — do not flatten

js-dos derives the location of its wasm/worker assets from the `<script>` that
loaded `js-dos.js`, so `emulators/` must stay a subfolder beside it:

```
vendor/js-dos.js  js-dos.css  SHA256SUMS
vendor/emulators/emulators.js
                 wdosbox.js      wdosbox.wasm        <- the backend the app uses
                 wdosbox-x.js    wdosbox-x.wasm
                 wlibzip.js      wlibzip.wasm        <- .zip / .jsdos bundles
                 file-explorer.js  file-explorer.css <- lazy chunks: they live under
                 webrtcnet.wasm                         emulators/, not at the root
```

The three lazy chunks are the trap: `js-dos.js` names them as bare
`"file-explorer.js"` / `"webrtcnet.wasm"`, which reads like a root-level path,
but the CDN serves them under `emulators/` (the root URLs are 404). Fetch them to
the wrong place and the mirror looks complete right up until a user opens the
file explorer.
