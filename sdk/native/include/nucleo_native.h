// NucleoOS native app SDK - the contract between a .nap module and the firmware that runs it.
// Spec: docs/native-apps.md. One header, included by the firmware (nucleo_native) and by every module.
//
// A native app is ordinary NucleoOS app code (the same nucleo_app_def_t, the same drawing and input API),
// compiled position-independent and loaded from the SD card at launch. It imports firmware functions by name
// from the ABI export table (sdk/native/abi/v<N>.txt) and exports exactly two symbols:
//
//   nucleo_native_info   a nap_info_t: magic, ABI level, layout fingerprint, app id - read before anything runs
//   nucleo_native_main   called once after relocation; registers the app (nucleo_app_register) and returns
//
// Porting an existing app is one line in one extra file (sdk/native/nap_main.cpp does it for you):
//   NUCLEO_NATIVE_APP("constellations", nucleo_register_constellations)
#pragma once
#include <stdint.h>

#define NUCLEO_NATIVE_ABI    2u            // the level this SDK builds against (abi/v2.txt)
#define NAP_MAGIC            0x3150414eu   // "NAP1" little-endian
#define NAP_ID_MAX           32

#define NAP_FLAG_NET         0x0001u       // uses the radio (pnet multiplayer)
#define NAP_FLAG_NO_HUD      0x0002u       // draws its own Back on the second screen: no system bar over it

#ifdef __cplusplus
extern "C" {
#endif

// Plain data, no pointers: the loader reads it straight from the file, before any relocation.
typedef struct {
    uint32_t magic;                 // NAP_MAGIC
    uint16_t abi;                   // ABI level the module was linked against (1..firmware level)
    uint16_t flags;                 // NAP_FLAG_*
    uint32_t layout;                // nucleo_native_layout() of the headers it was compiled with
    uint32_t ram;                   // bytes of heap it needs free at launch, beyond its own .data/.bss (0 = default)
    char     id[NAP_ID_MAX + 1];    // the store id; must equal the folder it is installed in
    uint8_t  reserved[3];
} nap_info_t;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// ── Layout fingerprint ───────────────────────────────────────────────────────
// Inline code in the module (LovyanGFX methods, struct field access) bakes in the size and shape of firmware
// types. A firmware built with different shapes would run the module into garbage, silently. Both sides
// compute this hash from the SAME expressions; the loader refuses a module whose value differs, and the
// store rebuilds every native app for the new layout (docs/native-apps.md, "When the layout changes").
#include <stddef.h>
#include <M5GFX.h>
#include "nucleo_app.h"
#include "game_ui.h"

namespace nucleo_native_detail {
constexpr uint32_t fnv(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xffu)) * 16777619u;
    return h;
}
}  // namespace nucleo_native_detail

constexpr uint32_t nucleo_native_layout()
{
    using namespace nucleo_native_detail;
    uint32_t h = 2166136261u;
    // Not the ABI level: levels only add names (old modules keep working); this hashes only type shapes.
    h = fnv(h, LGFX_VERSION_MAJOR * 10000u + LGFX_VERSION_MINOR * 100u + LGFX_VERSION_PATCH);
    h = fnv(h, sizeof(lgfx::LGFXBase));     h = fnv(h, alignof(lgfx::LGFXBase));
    h = fnv(h, sizeof(lgfx::LGFX_Sprite));
    h = fnv(h, sizeof(lgfx::pixelcopy_t));
    h = fnv(h, sizeof(lgfx::TextStyle));
    h = fnv(h, sizeof(nucleo_app_def_t));   h = fnv(h, offsetof(nucleo_app_def_t, on_draw_large));
    h = fnv(h, sizeof(nucleo_app_ram_t));
    h = fnv(h, sizeof(gui::Menu));
    h = fnv(h, sizeof(nap_info_t));
    return h;
}

#define NAP_EXPORT extern "C" __attribute__((visibility("default"), used))

// Defines the two exported symbols of a module. `id` is the store id, `reg` the app's usual registration
// function (it calls nucleo_app_register, which the firmware captures). Optional flags / ram in the _EX form.
#define NUCLEO_NATIVE_APP_EX(id_, reg_, flags_, ram_)                                                       \
    extern "C" void reg_(void);                                                                             \
    NAP_EXPORT const nap_info_t nucleo_native_info = { NAP_MAGIC, NUCLEO_NATIVE_ABI, (flags_),               \
                                                       nucleo_native_layout(), (ram_), id_, {0, 0, 0} };     \
    NAP_EXPORT void nucleo_native_main(void) { reg_(); }
#define NUCLEO_NATIVE_APP(id_, reg_) NUCLEO_NATIVE_APP_EX(id_, reg_, 0, 0)
#endif
