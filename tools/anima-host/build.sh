#!/usr/bin/env bash
# Builds the ANIMA host harness on POSIX hosts (Linux/macOS): compiles the REAL
# firmware cascade against the host shims with the system GCC. Mirror of build.ps1
# (the Windows/MinGW build) — keep the two in sync.
# No ESP-IDF, no hardware. Output: build/anima.exe (same name on every platform,
# because 39 tool scripts reference it; the extension is harmless on POSIX).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
anima="$repo/firmware/components/nucleo_anima"
build="$here/build"

mkdir -p "$build" "$here/sd/data/anima"

# GLOB every firmware .c (don't hand-list) so a NEW nucleo_anima file is compiled
# automatically — matching build.ps1 and the staleness check in anima.mjs. Two files
# are excluded ON PURPOSE: nucleo_anima_online.c (the network tier — replaced on the
# host by anima_online_stub.c, so it must NOT be linked too) and
# nucleo_anima_bench.c (a standalone benchmark).
srcs=()
for f in "$anima"/*.c; do
  case "$(basename "$f")" in
    nucleo_anima_online.c|nucleo_anima_bench.c) continue ;;
  esac
  srcs+=("$f")
done
srcs+=("$here/esp_timer_host.c" "$here/anima_online_stub.c" "$here/host_main.c")

exe="$build/anima.exe"
log="$build/compile.log"

# -std=gnu11 (not c11): matches ESP-IDF and exposes strcasecmp / M_PI (hidden behind
# __STRICT_ANSI__ under -std=c11). No -static here: that was for MinGW DLL hygiene;
# on POSIX a normal dynamic link is the idiomatic build.
echo "Compiling ANIMA host with $(command -v gcc) ..."
if gcc -std=gnu11 -O0 -g -DANIMA_HOST \
    -I"$here/shim" -I"$anima/include" -I"$anima" \
    -include "$here/shim/host_compat.h" \
    "${srcs[@]}" -o "$exe" -lm >"$log" 2>&1; then
  echo "OK -> $exe"
else
  code=$?
  cat "$log"
  echo "Build failed (exit $code). See $log" >&2
  exit "$code"
fi
