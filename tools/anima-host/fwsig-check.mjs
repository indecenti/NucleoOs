// Host gate for release signatures (firmware/components/nucleo_fwsig, docs/release-signing.md): compiles the
// REAL fwsig.c against mbedTLS from ESP-IDF with a throw-away key table, signs test images here exactly as
// tools/fw_sign.py does, and checks that only the genuine signature passes — whatever the chunking of the
// stream (the device hashes while it downloads, holding the last 128 bytes back). Also checks that the key
// table compiled into the firmware matches keys/*.pub.pem. Wired as `npm run fwsig:test`.
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash, createPublicKey, generateKeyPairSync, sign } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const C = join(ROOT, 'firmware', 'components', 'nucleo_fwsig');
const MBED = 'C:/esp/esp-idf/components/mbedtls/mbedtls';
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };
const OUT = join(ROOT, 'build', 'fwsig-host');
mkdirSync(OUT, { recursive: true });
let checks = 0, fails = 0;
const ok = (c, m) => { checks++; if (!c) { fails++; console.log(`  FAIL: ${m}`); } };

// mbedTLS for the host (shared with store-host when it is already built)
const shared = join(ROOT, 'build', 'store-host', 'mbed', 'libmbedcrypto-host.a');
let lib = join(OUT, 'libmbedcrypto-host.a');
if (existsSync(shared)) lib = shared;
else if (!existsSync(lib)) {
  console.log('fwsig: building mbedTLS for the host (once)...');
  const objs = [];
  for (const f of readdirSync(join(MBED, 'library')).filter((f) => f.endsWith('.c'))) {
    const o = join(OUT, f.replace(/\.c$/, '.o'));
    execFileSync(GCC, ['-std=gnu11', '-O1', '-w', '-I', join(MBED, 'include'), '-I', join(MBED, 'library'), '-c', join(MBED, 'library', f), '-o', o], { env });
    objs.push(o);
  }
  execFileSync(join(MINGW, 'ar.exe'), ['rcs', lib, ...objs], { env });
}

const point = (pub) => { const j = pub.export({ format: 'jwk' }); return Buffer.concat([Buffer.from([4]), Buffer.from(j.x, 'base64url'), Buffer.from(j.y, 'base64url')]); };
const keyId = (pub) => createHash('sha256').update(point(pub)).digest('hex').slice(0, 16);
const keys = [generateKeyPairSync('ec', { namedCurve: 'prime256v1' }), generateKeyPairSync('ec', { namedCurve: 'prime256v1' })];
const stranger = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
const table = keys.map((k) => `    { "${keyId(k.publicKey)}", { ${[...point(k.publicKey)].map((b) => '0x' + b.toString(16).padStart(2, '0')).join(', ')} } },`).join('\n');
writeFileSync(join(OUT, 'keys_test.c'), `#include "fwsig.h"\nconst fwsig_key_t FWSIG_KEYS[] = {\n${table}\n};\nconst int FWSIG_NKEYS = ${keys.length};\n`);

const exe = join(OUT, 'fwsig-ctest.exe');
const cc = spawnSync(GCC, ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', join(C, 'include'), '-I', join(MBED, 'include'),
  join(ROOT, 'tools', 'anima-host', 'fwsig-ctest.c'), join(C, 'fwsig.c'), join(OUT, 'keys_test.c'), lib, '-lbcrypt', '-o', exe], { env, encoding: 'utf8' });
if (cc.status !== 0) { console.error('fwsig: COMPILE FAILED'); process.stdout.write(cc.stdout || ''); process.stderr.write(cc.stderr || ''); process.exit(1); }

const OTA = 'nucleoos-cardputer-ota-v1', SD = 'nucleoos-cardputer-sd-v1';
function block(key, domain, version, data, kid = keyId(key.publicKey)) {        // == fw_sign.py make_block
  const msg = `${domain}\n${version}\n${createHash('sha256').update(data).digest('hex')}\n${data.length}\n`;
  const der = sign('sha256', Buffer.from(msg), key.privateKey);
  const b = Buffer.alloc(128);
  b.write('nucleoos-sig-v1\n', 0, 'ascii'); b.write(kid, 16, 'ascii'); b.writeUInt16LE(der.length, 32); der.copy(b, 34);
  return b;
}
function verdict(file, domain, version, chunk) {
  const r = spawnSync(exe, [file, domain, version, String(chunk)], { encoding: 'utf8' });
  const w = (r.stdout || '').trim().split(' ');
  return [w.slice(0, -2).join(' '), ...w.slice(-2)];   // the verdict may hold a space ("bad signature")
}
const img = Buffer.alloc(50021); for (let i = 0; i < img.length; i++) img[i] = (i * 131 + (i >> 7)) & 0xff;
const f = join(OUT, 'img.bin');
const put = (b) => writeFileSync(f, b);

put(Buffer.concat([img, block(keys[0], OTA, '0.9.0', img)]));
for (const ch of [1, 7, 127, 128, 129, 1024, 4096, 70000]) {
  const [v, sha, size] = verdict(f, OTA, '0.9.0', ch);
  ok(v === 'ok', `genuine image verifies (chunk ${ch}): ${v}`);
  ok(sha === createHash('sha256').update(img).digest('hex') && +size === img.length, `chunk ${ch}: the hash covers exactly the image`);
}
put(Buffer.concat([img, block(keys[1], OTA, '0.9.0', img)]));
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'ok', 'the backup key is trusted too');
put(Buffer.concat([img, block(keys[0], OTA, '0.9.0', img)]));
ok(verdict(f, OTA, '0.8.9', 1024)[0] === 'bad signature', 'another version (the one in the image) is refused');
ok(verdict(f, SD, '0.9.0', 1024)[0] === 'bad signature', 'an SD signature cannot pass as a firmware one');
{ const t = Buffer.from(img); t[30000] ^= 1; put(Buffer.concat([t, block(keys[0], OTA, '0.9.0', img)])); }
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'bad signature', 'one flipped bit in the image is refused');
put(Buffer.concat([img, Buffer.from('x'), block(keys[0], OTA, '0.9.0', img)]));
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'bad signature', 'a byte appended before the block is refused');
put(Buffer.concat([img, block(stranger, OTA, '0.9.0', img)]));
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'unknown key', 'a stranger key is refused');
put(Buffer.concat([img, block(stranger, OTA, '0.9.0', img, keyId(keys[0].publicKey))]));
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'bad signature', 'a stranger signature under a trusted key id is refused');
put(img);
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'unsigned', 'an unsigned image is refused');
put(Buffer.alloc(60));
ok(verdict(f, OTA, '0.9.0', 7)[0] === 'short', 'a stream shorter than a block is refused');
{ const b = block(keys[0], OTA, '0.9.0', img); b.writeUInt16LE(200, 32); put(Buffer.concat([img, b])); }
ok(verdict(f, OTA, '0.9.0', 1024)[0] === 'bad signature', 'an oversized DER length is refused');

// The compiled-in table must be the public keys in the repo (fw_sign.py keys regenerates it).
const kc = readFileSync(join(C, 'fwsig_keys.c'), 'utf8');
for (const w of ['primary', 'backup']) {
  const pem = join(C, 'keys', `${w}.pub.pem`);
  ok(existsSync(pem), `keys/${w}.pub.pem exists`);
  if (!existsSync(pem)) continue;
  const pub = createPublicKey(readFileSync(pem));
  const hex = [...point(pub)].map((b) => '0x' + b.toString(16).padStart(2, '0')).join(', ');
  ok(kc.includes(`"${keyId(pub)}"`) && kc.replace(/\s+/g, ' ').includes(hex), `fwsig_keys.c carries the ${w} key`);
}

console.log(`fwsig: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
