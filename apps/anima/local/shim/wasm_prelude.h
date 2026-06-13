// WASM forced-include prelude (emcc -include). Under Emscripten's clang + musl the
// case-insensitive string ops the firmware uses live in <strings.h> (on MinGW they come in
// via <string.h>), and modern clang treats an implicit declaration as a HARD error — so pull
// them in up front. Mirrors the role of tools/anima-host/shim/host_compat.h for the host build.
#pragma once
#include <string.h>
#include <strings.h>   // strcasecmp / strncasecmp
