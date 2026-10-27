// ui-host (apps.mjs only): seed the signed-store catalog on the sandbox SD BEFORE app_store opens, so the app
// gallery shows a populated store, and give it a device address so the "open on phone" QR has a URL. Referenced
// ONLY by apps/store.txt (@host) — never by run.mjs, whose store goldens seed per-scene via scenes.cpp, so this
// file cannot change them. A scene that wants the empty state drops an sd/.noseed marker.
#include <stdio.h>

extern "C" {
void host_store_seed(bool with_catalog);   // in store_stubs.cpp

const char *nucleo_setup_ip(void) { return "192.168.1.42"; }   // fictional: never a real device address

__attribute__((constructor)) static void apps_store_seed(void)
{
    FILE *f = fopen("sd/.noseed", "rb");
    if (f) { fclose(f); return; }     // the "empty" scene: no catalog, show the first-run message
    host_store_seed(true);
}
}
