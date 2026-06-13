#!/usr/bin/env python3
"""
Augment an AKB3 ANIMA index in-place into "AKB4" by APPENDING a sign-signature trailer.

Backward-compatible by construction: the file magic stays "AKB3" and the original body
(header, centroids, directory, vectors, answer records) is byte-for-byte unchanged, so the
current firmware reads it exactly as before. We only append, at EOF:

    [ sig section : N * (D/8) bytes ]   one little-endian sign-bitmask per vector, in vector
                                        index order 0..N-1 (so cluster (off,count) vectors map
                                        to a contiguous sig block at sig_off + off*(D/8))
    [ footer : 16 bytes ]               "ASIG" | u32 sig_off | u32 sig_bits(=D) | u32 version

New firmware seeks to EOF-16, reads the footer; if magic=="ASIG" and sig_bits==D it enables
the popcount prefilter. Old firmware never reads past the answer records, so the trailer is inert.

Sign convention MUST match the device query path: bit = (value >= 0). Ties (0) -> 1, both sides.

Usage:  python augment_akb4.py <path-to-anima-it-index.bin>   (writes <path>.akb3.bak first)
"""
import os, struct, sys
import numpy as np

def augment(path):
    with open(path, "rb") as f:
        buf = bytearray(f.read())
    if bytes(buf[:4]) != b"AKB3":
        sys.exit(f"not an AKB3 index: magic={bytes(buf[:4])!r}")
    D, K, N = struct.unpack_from("<III", buf, 4)
    if D % 64 != 0:
        sys.exit(f"D={D} not a multiple of 64 (needed for clean uint64 popcount)")
    vec_base = 16 + K * D + K * 2 * 4
    rec = D + 4
    sig_bytes = D // 8

    # already augmented? (idempotent: detect an existing ASIG footer at EOF)
    if len(buf) >= 16 and bytes(buf[-16:-12]) == b"ASIG":
        print(f"[augment] {os.path.basename(path)} already has an ASIG trailer — rewriting it fresh")
        # strip the old trailer: read its sig_off and truncate there
        old_off = struct.unpack_from("<I", buf, len(buf) - 12)[0]
        buf = buf[:old_off]

    # pull the N stored vectors out of the (unchanged) vectors section
    raw = np.frombuffer(bytes(buf), np.uint8, N * rec, vec_base).reshape(N, rec)
    vecs = raw[:, :D].view(np.int8)                       # (N, D) int8
    bits = (vecs.astype(np.int16) >= 0)                   # (N, D) bool ; tie 0 -> 1
    sigs = np.packbits(bits, axis=1, bitorder="little")   # (N, D/8) uint8
    assert sigs.shape[1] == sig_bytes

    sig_off = len(buf)
    out = bytearray(buf)
    out += sigs.tobytes()
    out += b"ASIG" + struct.pack("<III", sig_off, D, 1)

    bak = path + ".akb3.bak"
    if not os.path.exists(bak):
        with open(bak, "wb") as f: f.write(buf if isinstance(buf, (bytes, bytearray)) else bytes(buf))
        print(f"[augment] backup -> {bak}")
    with open(path, "wb") as f: f.write(out)
    print(f"[augment] {os.path.basename(path)}: AKB3 body {sig_off} B + sigs {N*sig_bytes} B + footer 16 B "
          f"= {len(out)} B  (sig_off={sig_off}, D={D}, N={N})")
    print(f"[augment] magic still 'AKB3' -> old firmware unaffected; footer 'ASIG' -> new firmware enables prefilter")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    augment(sys.argv[1])
