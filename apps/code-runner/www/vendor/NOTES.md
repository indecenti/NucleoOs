# code-runner vendored libraries

- **acorn.mjs** — [acorn](https://github.com/acornjs/acorn) 8.18.0, `dist/acorn.mjs` from the npm tarball
  (`npm pack acorn@8.18.0`), unmodified. MIT — `acorn.LICENSE`. sha256
  `953573b8fdab71599749ea5f2b33d3e760c2116178f9423ee7458dbe39d59453`.
  Loaded on demand by `nucleo-run.js loadParser()` so `checkSyntax(code, { bare: true })` parses real ES
  modules (import / export, top-level await) with line + column — the Function-constructor check cannot, so
  agent-written modules and module pages were not checked at all. Never loaded at boot; ~60 KB gzipped.
