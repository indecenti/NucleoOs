// External display chrome — what the touch layer draws in the letterbox around the mirrored frame: the soft
// keys, the status strip and the calibration targets. Platform-neutral LovyanGFX (the host gate renders it
// into build/extdisp-host/external.png). Never draws inside the frame rectangle: that belongs to the mirror.
#pragma once
#define LGFX_USE_V1
#include "lgfx/v1/LGFXBase.hpp"
#include "extdisp_touch.h"

struct extdisp_frame_t { int ox, oy, fw, fh; };

// Everything outside the frame: bands, side strips, keys, status. `l1`/`l2` = status lines, `swatches` =
// the R/G/B/W colour check (tester diagnostics).
void extdisp_chrome_draw(lgfx::LovyanGFX *g, const extdisp_rect_t r[SK_COUNT], const extdisp_frame_t &fr,
                         const char *l1, const char *l2, bool swatches);
// One key (or SK_STATUS) in its normal / pressed look.
void extdisp_chrome_key(lgfx::LovyanGFX *g, const extdisp_rect_t r[SK_COUNT], int key, bool pressed,
                        const char *l1, const char *l2, bool swatches);
// Calibration: bands cleared, crosshair on target `step` (0..3), instructions in the top band.
void extdisp_chrome_calib(lgfx::LovyanGFX *g, const extdisp_frame_t &fr, int step, const char *l1, const char *l2);
