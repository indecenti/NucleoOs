// ui-host: the backend of the native Updates app (app_updates.cpp) as a small simulated device. The REAL
// update_policy.c is compiled in (version compare, should-show); only the network/NVS engine of
// nucleo_update.c is stubbed here. A scene sets what the device "knows" before it opens the app.
#include "host_state.h"
#include "nucleo_update.h"
#include <string.h>
#include <stdio.h>

extern "C" {
const char *g_host_upd_latest = "v0.5.1";        // the newest release this device learned ("" = never checked)
upd_phase_t g_host_upd_phase  = UPD_IDLE;        // what the engine is doing
const char *g_host_upd_err    = "";              // the engine's last (localized) failure

bool        nucleo_update_dialog_pending(void) { return g_host_upd_latest[0] != 0; }
const char *nucleo_update_latest_tag(void) { return g_host_upd_latest; }
void        nucleo_update_dismiss_latest(void) {}
bool        nucleo_update_kick_check(bool) { return true; }
bool        nucleo_update_start(void) { return false; }
void        nucleo_update_arm_boot_install(void) {}
bool        nucleo_update_boot_install_armed(void) { return false; }
void        nucleo_update_disarm_boot_install(void) {}
bool        nucleo_update_busy(void) { return false; }
void        nucleo_update_get_state(nucleo_update_state_t *out)
{
    memset(out, 0, sizeof *out);
    out->phase = g_host_upd_phase;
    out->pct = -1;
    snprintf(out->latest, sizeof out->latest, "%s", g_host_upd_latest);
    snprintf(out->err, sizeof out->err, "%s", g_host_upd_err);
}

// The rest of what the app calls on the framework (no-ops on the host: the scene draws on the canvas).
bool nucleo_anima_solo_active(void) { return false; }
void nucleo_app_release_buffers(void) {}
void nucleo_app_set_direct_draw(bool) {}
void nucleo_app_solo_request_id(const char *) {}
void nucleo_exclusive_exit(void) {}
}
