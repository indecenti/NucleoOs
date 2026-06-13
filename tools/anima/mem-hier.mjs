// Squeeze every bit of the ESP32-S3: a 3-tier memory hierarchy for ANIMA's hyperdimensional brain.
//
//   SRAM  (~64 KB arena)   hot working set + LRU of hottest vectors        ~instant
//   FLASH (mmap/XIP, MB)   the BULK codebook, memory-mapped, scanned in place by popcount  (the key idea)
//   SD    (GB, ~1 MB/s)    cold overflow + the write-heavy append-only journal             slow
//
// The novelty: binary hypervectors live in a memory-MAPPED flash partition (esp_partition_mmap → XIP),
// so a popcount scan runs over them at near-bus speed with ZERO copy to SRAM. Plus a COARSE-PREFIX
// prefilter (first 8 bytes of each HV) makes the scan effectively sublinear: read 8 B/HV to shortlist,
// then read full 1 KB only for the few survivors. This module MEASURES popcount throughput for real and
// applies a datasheet latency model to size the brain and the per-tier query cost.
//   node tools/anima/mem-hier.mjs
import { D, randomHV, hamming } from './hdc.mjs';
import { performance } from 'node:perf_hooks';

const C = { g: '\x1b[32m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };
const BYTES = D / 8;                       // 1024 B per hypervector at D=8192
const KB = 1024, MB = 1024 * 1024;

// --- 1) MEASURE popcount/XOR throughput (the actual reasoning op) on this machine, as a proxy that the
//     op is trivially cheap; on the device the S3 SIMD (128-bit EE.* lanes) is in the same ballpark. ---
function measurePopcount() {
  const N = 20000, a = randomHV('probe');
  const cb = []; for (let i = 0; i < 512; i++) cb.push(randomHV('m' + i));
  // warmup
  for (let i = 0; i < 512; i++) hamming(a, cb[i]);
  const t0 = performance.now(); let acc = 0;
  for (let n = 0; n < N; n++) acc ^= hamming(a, cb[n & 511]);
  const dt = performance.now() - t0;
  return { hvPerMs: N / dt, nsPerHV: dt * 1e6 / N, _: acc };
}

// --- 2) latency model (ESP32-S3 datasheet-ish constants; conservative) -------------------------------
const FLASH_XIP_MBps = 40;     // QIO ~80 MHz, cold/sequential effective; cache hits are ~SRAM speed
const SD_MBps = 1.0;           // SPI microSD per docs/anima.md
const PREFIX = 8;              // coarse-prefix bytes per HV for the prefilter
const RERANK = 32;             // full-HV survivors re-scored after the coarse pass
const readMs = (bytes, MBps) => (bytes / MB) / MBps * 1000;

const pop = measurePopcount();
const FACTS_PER_HV = 60;       // measured in hdc-bench.mjs (≥95% recall, key→value bundling)
console.log(`\n${C.b}=== ANIMA brain memory hierarchy — spremere ESP32-S3 ===${C.x}  ${C.d}HV=${BYTES} B (D=${D})${C.x}\n`);
console.log(`${C.b}popcount misurato${C.x}: ${pop.hvPerMs.toFixed(0)} HV/ms (${pop.nsPerHV.toFixed(0)} ns per confronto da ${BYTES} B) ${C.d}— il ragionamento è gratis; il collo di bottiglia è l'I/O, non la CPU${C.x}\n`);

// Honest per-tier picture: a tier is bound by the SMALLER of its CAPACITY (bytes/HV) and what its
// bandwidth can SCAN within the budget. The coarse prefix is what removes the latency wall.
const TIERS = [
  { name: 'SRAM arena', bytes: 64 * KB, MBps: Infinity },
  { name: 'FLASH mmap 4MB', bytes: 4 * MB, MBps: FLASH_XIP_MBps },
  { name: 'FLASH mmap 8MB', bytes: 8 * MB, MBps: FLASH_XIP_MBps },
  { name: 'SD (cold)', bytes: 4096 * MB, MBps: SD_MBps },
];
const fullScanMs = (cap, MBps) => readMs(cap * BYTES, MBps) + cap * pop.nsPerHV / 1e6;
const prefixScanMs = (cap, MBps) => readMs(cap * PREFIX, MBps) + cap * (PREFIX / BYTES) * pop.nsPerHV / 1e6 + readMs(RERANK * BYTES, MBps) + RERANK * pop.nsPerHV / 1e6;
const fmt = (ms) => ms < 1 ? (ms * 1000).toFixed(0) + ' µs' : ms < 1000 ? ms.toFixed(1) + ' ms' : (ms / 1000).toFixed(1) + ' s';

console.log(`${C.b}Cervello per tier — capienza e tempo di scansione completa${C.x}`);
console.log(`   ${C.d}tier             concetti(HV)   fatti(~60/HV)   scan FULL     scan +PREFISSO${C.x}`);
for (const t of TIERS) {
  const cap = Math.floor(t.bytes / BYTES);
  console.log(`   ${t.name.padEnd(16)} ${C.c}${cap.toLocaleString().padStart(10)}${C.x}   ${(cap * FACTS_PER_HV).toLocaleString().padStart(11)}   ${fmt(fullScanMs(cap, t.MBps)).padEnd(12)} ${C.g}${fmt(prefixScanMs(cap, t.MBps))}${C.x}`);
}
console.log();
const sram = Math.floor(64 * KB / BYTES), flash8 = Math.floor(8 * MB / BYTES);
console.log(`${C.b}Il salto${C.x}`);
console.log(`   sola SRAM:        ${C.c}${sram}${C.x} concetti (~${(sram * FACTS_PER_HV).toLocaleString()} fatti)`);
console.log(`   + FLASH mmap 8MB: ${C.c}${flash8.toLocaleString()}${C.x} concetti (~${(flash8 * FACTS_PER_HV).toLocaleString()} fatti), scansionati col prefisso in ${C.g}${fmt(prefixScanMs(flash8, FLASH_XIP_MBps))}${C.x}`);
console.log(`   ${C.g}→ ~${Math.round(flash8 / sram)}× la conoscenza, ancora in pochi ms: col prefisso coarse il cervello è limitato dalla CAPienza, non dalla latenza.${C.x}`);
console.log(`   ${C.d}(SD resta per overflow freddo + il journal di scrittura; non si scandisce a caldo: 1 MB/s)${C.x}\n`);

// --- 3) SAFE swap: wear + integrity math (the "swap intelligente e sicuro") --------------------------
console.log(`${C.b}Swap sicuro (flash a scrittura rara, SD assorbe le scritture)${C.x}`);
const sectors = (4 * MB) / (4 * KB);                  // 4 KB flash sectors in a 4 MB brain partition
const endurance = 100_000;                            // typical NOR flash erase cycles per sector
const rebuildsPerDay = 4;                             // batch-rebuild the flash brain a few times/day
const yearsToWear = (sectors * endurance) / (rebuildsPerDay * 365);
console.log(`   - la flash si RISCRIVE a lotti (${rebuildsPerDay}/giorno, wear-leveled su ${sectors} settori), NON a ogni fatto`);
console.log(`   - le scritture vere vanno sul JOURNAL append-only su SD (endurance enorme, FAT)`);
console.log(`   - durata stimata flash: ${C.c}~${Math.round(yearsToWear).toLocaleString()} anni${C.x} prima dell'usura ${C.d}(${sectors}×${endurance.toLocaleString()} cicli ÷ ${rebuildsPerDay}/g)${C.x}`);
console.log(`   - integrità: ogni pagina HV con CRC32 + cifratura flash HW (AES) → una pagina corrotta/ostile è rilevata e RIFIUTATA (no cervello avvelenato)\n`);

console.log(`${C.b}=== Conclusione ===${C.x} ${C.g}la flash mmap trasforma un cervello da ~poche centinaia a ~decine di migliaia di concetti, scansionati con popcount, restando offline e sicuro.${C.x}`);
