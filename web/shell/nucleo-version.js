/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-version.js — what this web OS is, in numbers anyone can compare. The rules: docs/versions.md.
//
//   import { WEBOS, CORE } from '/nucleo-version.js';
//   WEBOS     the web OS release (semver), tagged v<WEBOS> in nucleo-webos, named in CHANGELOG.md and in every
//             edition's webos.lock and build.json
//   CORE      the core API level: one more each time a core module gains something apps can use, never less
//             (contract/core-api.json lists what each level added; tests/core-api.test.mjs holds the line).
//             A store app that needs a level lists "core.<n>" in `requires`: nucleo-caps.js answers core.1 … core.CORE,
//             so an older web OS simply does not offer the app.
//   CONTRACT  the /api contract's version (contract/api.yaml info.version)
export const WEBOS = '1.0.0';
export const CORE = 3;
export const CONTRACT = '1';

export default { webos: WEBOS, core: CORE, contract: CONTRACT };
