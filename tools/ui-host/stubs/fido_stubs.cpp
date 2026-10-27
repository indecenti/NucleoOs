// Host stubs for the app_fido gallery: put the app in FIDO key mode with a connected host, a PIN set, a
// hardware-backed key and a few resident passkeys, so the two-screen status + manager render their real
// layout instead of the return-0 empty state. (PC gallery only — not the firmware.)
#include "nucleo_fido.h"
#include <string.h>
#include <stdio.h>
extern "C" {
bool nucleo_fido_boot_mode(void)      { return true; }   // render the active key-mode screens
bool nucleo_fido_ready(void)          { return true; }   // a host is connected
bool nucleo_fido_pin_is_set(void)     { return true; }
bool nucleo_fido_key_is_hardware(void){ return true; }
int  nucleo_fido_cred_count(void)     { return 3; }
bool nucleo_fido_cred_get(int i, fido_cred_view_t *o) {
    static const char *rp[3] = { "github.com", "accounts.google.com", "cloudflare.com" };
    static const char *us[3] = { "nicola", "niki", "admin" };
    if (i < 0 || i > 2 || !o) return false;
    snprintf(o->rp, sizeof o->rp, "%s", rp[i]);
    snprintf(o->user, sizeof o->user, "%s", us[i]);
    o->sign_count = (uint32_t)(i * 7 + 3);
    return true;
}
}
