// External display chrome — what the touch layer draws in the letterbox around the mirrored frame: the soft
// keys, the status strip and the calibration screen. Platform-neutral LovyanGFX (the host gate renders it
// into build/extdisp-host/external.png). The chrome never draws inside the frame rectangle (that belongs to the
// mirror); only the calibration screen covers the whole panel, while the mirror is held dark.
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
// Calibration owns the WHOLE panel (the mirror is held dark meanwhile): white, the red target `step` (0..3) in its
// corner, and in the centre the title, "step k of 4", four progress dots (done green, current red, next grey)
// and the instruction — red when `retry` (the last attempt was rejected). step 4 = done: a green check.
void extdisp_chrome_calib(lgfx::LovyanGFX *g, int step, bool retry, const char *title, const char *line_step,
                          const char *instr);
