// gen-handoff — copy the shell's boot splash into the device's flash-embedded handoff page.
// firmware/components/nucleo_webfs/handoff.html is served while the Cardputer reboots into its web profile
// (see nucleo_webfs_set_handoff_cb): it must look like the shell's own boot (the animated atom), but it is
// served from FLASH (the device reboots right after sending it, so no SD asset can follow), so the splash
// markup + script are copied in between the BOOT-SPLASH markers. Single source: web/shell/index.html.
//   node tools/gen-handoff.mjs           write the page (only when it changes)
//   node tools/gen-handoff.mjs --check   exit 1 if the page is out of sync (npm run handoff:check, the gate)
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const SHELL = ROOT + 'web/shell/index.html';
const PAGE = ROOT + 'firmware/components/nucleo_webfs/handoff.html';
const BEGIN = '<!-- BOOT-SPLASH:BEGIN';
const END = '<!-- BOOT-SPLASH:END -->';

export function splashOf(shellHtml) {
  const s = shellHtml.replace(/\r\n/g, '\n');
  const a = s.indexOf('<div id="boot-screen"');
  if (a < 0) throw new Error('boot splash not found in web/shell/index.html (<div id="boot-screen">)');
  const b = s.indexOf('</script>', a);
  if (b < 0) throw new Error('boot splash script not closed');
  return s.slice(a, b + '</script>'.length);
}

export function render(pageHtml, splash) {
  const p = pageHtml.replace(/\r\n/g, '\n');
  const a = p.indexOf(BEGIN), b = p.indexOf(END);
  if (a < 0 || b < a) throw new Error('BOOT-SPLASH markers missing in handoff.html');
  const head = p.slice(0, p.indexOf('-->', a) + 3);
  return head + '\n' + splash + '\n' + p.slice(b);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const cur = readFileSync(PAGE, 'utf8');
  const next = render(cur, splashOf(readFileSync(SHELL, 'utf8')));
  const same = cur.replace(/\r\n/g, '\n') === next;
  if (process.argv.includes('--check')) {
    if (!same) { console.log('handoff:check: handoff.html is OUT OF SYNC with the shell boot splash — run node tools/gen-handoff.mjs'); process.exit(1); }
    console.log('handoff:check: handoff.html carries the current shell boot splash');
  } else if (!same) { writeFileSync(PAGE, next); console.log('gen-handoff: handoff.html updated'); }
  else console.log('gen-handoff: handoff.html already current');
}
