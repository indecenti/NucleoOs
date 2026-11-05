# Vendored code

## mquickjs/ — MicroQuickJS (MIT, Fabrice Bellard and Charlie Gordon)

- Upstream: https://github.com/bellard/mquickjs, commit `6d4d7eb6b99011d5aab6e88a9b99e8fd4d86c6e9` (2026-09-26).
- Files: the engine (`mquickjs.c/h`, `mquickjs_priv.h`, `mquickjs_opcode.h`), `dtoa`, `libm`, `cutils`, `list.h`,
  `softfp_*`, and the table generator (`mquickjs_build.c/h`, `mqjs_stdlib.c`) as upstream, plus `LICENSE`.
- One NUCLEO PATCH (`mquickjs.nucleo.patch`): `JS_NucleoInterruptSoon` (Esc and the time limit seen at the next
  instruction, for scripts that live in C: drawing, sleeping) and `JS_NucleoLiveMax` / `JS_NucleoUsedNow`, the memory figures Nucleo Code
  reports (the most memory live after a compacting GC, and what is in use now).
- The ROM tables are generated from `../code_stdlib.c` by `node tools/code/gen-tables.mjs` into `../gen/` (32-bit).
- To update: replace the files from a newer upstream commit, re-apply the patch, regenerate, `npm run code:test`.
