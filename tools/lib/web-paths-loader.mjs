// Node ESM resolve hook: lets a host test import browser modules that use the DEVICE's absolute URLs.
// The device serves /apps/<id>/… from apps/<id>/www/… and the shell's root files (/ai.js, /ai-engines.js …)
// from web/shell/. Register it before importing such a module:
//   import { register } from 'node:module';
//   register('./lib/web-paths-loader.mjs', import.meta.url);
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { join } from 'node:path';

const REPO = fileURLToPath(new URL('../..', import.meta.url));

export function webPathToFile(spec) {
  const m = /^\/apps\/([^/]+)\/(.+)$/.exec(spec);
  const file = m ? join(REPO, 'apps', m[1], 'www', m[2]) : join(REPO, 'web', 'shell', spec.slice(1));
  return existsSync(file) ? file : null;
}

export async function resolve(specifier, context, next) {
  if (specifier.startsWith('/') && !specifier.startsWith('//')) {
    const file = webPathToFile(specifier);
    if (file) return { url: pathToFileURL(file).href, shortCircuit: true };
  }
  return next(specifier, context);
}
