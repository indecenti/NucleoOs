#!/usr/bin/env bash
# Builds the heavy-work arbiter HOST test on POSIX (Linux/macOS): compiles the REAL device core
# (nucleo_arb.c) against the portable platform shim (arb_plat_host.c) + the concurrency test
# (arb_test.c) with the system GCC and -DARB_HOST. Mirror of arb-build.ps1 — keep the two in sync.
# No ESP-IDF. Output: build/arb_test.exe (same name on every platform; the extension is harmless).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
comp="$repo/firmware/components/nucleo_arb"
build="$here/build"
mkdir -p "$build"

exe="$build/arb_test.exe"
log="$build/compile.log"

echo "Compiling arbiter host test with $(command -v gcc) ..."
if gcc -std=gnu11 -O2 -g -DARB_HOST -Wall -Wextra \
    -I"$comp/include" -I"$comp" \
    "$comp/nucleo_arb.c" "$here/arb_plat_host.c" "$here/arb_test.c" \
    -o "$exe" -lpthread >"$log" 2>&1; then
  echo "OK -> $exe"
else
  code=$?
  cat "$log"
  echo "Build failed (exit $code). See $log" >&2
  exit "$code"
fi
