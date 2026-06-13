# ANIMA — memory hierarchy: a brain that uses every bit of the device

How ANIMA's hyperdimensional brain ([`anima-hdc.md`](anima-hdc.md)) is sized across the three storage
tiers of the Cardputer, so it holds **tens of thousands of concepts** offline and reasons over them in
**single-digit milliseconds**, without ever leaving the device. Measured by `tools/anima/mem-hier.mjs`.

The reasoning op is XOR + popcount over 1 KB binary hypervectors — *trivially cheap* (measured ~1800
HV/ms in JS; the S3's 128-bit SIMD is in the same ballpark). So the brain's size is bounded by **storage
and I/O bandwidth**, not by compute. That reframes the whole design: put the bulk where it can be
*scanned in place*, and make the scan cheap.

## The three tiers

| Tier | Size | Role | Per-HV access |
|---|---|---|---|
| **SRAM** arena (~64 KB) | ~64 HV | hot working set + LRU of the hottest vectors | ~instant |
| **FLASH** (mmap/XIP, MBs) | 4–8 K HV | the **bulk codebook**, memory-mapped, scanned in place | near-bus (cached ~SRAM) |
| **SD** (GBs, ~1 MB/s) | millions | cold overflow + the append-only write **journal** | slow (~1 ms/HV) |

### The key idea: memory-mapped flash brain (XIP)
ESP-IDF's `esp_partition_mmap` maps a flash partition into the CPU address space (execute/read-in-place).
Put the binary hypervector codebook there and a popcount scan runs over flash **with zero copy to SRAM**,
at flash-cache speed. A 4 MB brain partition = **4096** hypervectors resident; 8 MB = **8192**. Compared
to SRAM-only (~64), that is **~64–128× more knowledge** — and at ~60 facts per hypervector
([`anima-hdc.md`](anima-hdc.md) capacity bench) an 8 MB brain holds on the order of **~490 000 facts**.

### The coarse-prefix prefilter (makes the full tier scannable)
Reading 8192 × 1 KB from flash to score them all is ~200 ms — too slow. So each HV gets an 8-byte
**coarse prefix** (the first 64 bits, a valid truncated-Matryoshka signature). The cleanup runs in two
passes: read **8 B/HV** to shortlist (popcount over 64 KB), then read the full 1 KB only for the ~32
survivors. Measured: scanning the **entire 8 MB brain drops from ~205 ms to ~2.4 ms**. With the prefix,
the brain is bounded by **capacity, not latency** — you can use the whole partition and still answer in
single-digit ms.

## Safe, intelligent swap (the 8 MB flash, used right)

Flash is not RAM — it has limited erase endurance (~100 000 cycles/sector), so we never page it like a
swap file. Instead:

- **Write-rarely brain, write-heavy journal.** New learned cards append to the SD journal (huge
  endurance, FAT). The flash brain is **rebuilt in batches** (a few times/day) from the journal, the
  vectors packed and wear-leveled across the partition's sectors. Endurance math: 1024 sectors ×
  100 000 cycles ÷ 4 rebuilds/day ≈ **~70 000 years** before wear — i.e. never, in practice.
- **Hot promotion.** A small SRAM LRU caches the hottest vectors; the flash mmap serves the warm bulk;
  SD holds the cold tail. A query scans SRAM-hot + flash-warm (both fast); SD only on a deep miss.
- **Integrity & safety.** Each flash HV page carries a CRC32; the partition can use the S3's hardware
  flash encryption (AES). A corrupted or tampered page fails its checksum and is **refused** — the brain
  cannot be silently poisoned. "Intelligent and safe swap": the device decides what to promote/evict and
  verifies every page it trusts.

## Build plan (firmware, gated)

1. A dedicated `nvs`-adjacent **`anima_brain` partition** in the partition table (size from free flash
   after firmware + OTA slots — verify the real layout; many S3 modules ship 8–16 MB).
2. `esp_partition_mmap` the brain read-only; `nucleo_anima_hdc.c` runs popcount over the mapped region
   (the coarse prefix first, then full-HV rerank for survivors).
3. The batch rebuilder reads the SD journal, packs `{prefix(8 B) | crc(4 B) | hv(1 KB)}` records, and
   writes the partition wear-leveled; the LRU lives in the ANIMA SRAM arena.
4. Optional flash encryption for the brain partition.

This is the storage substrate for the reasoning core in [`anima-hdc.md`](anima-hdc.md) and the
deductive KGE; together they make a genuinely large, fast, **offline** brain on a $15 MCU.
