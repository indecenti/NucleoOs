// Host stubs for the app_remote gallery: a plausible Wi-Fi "Remote Control" listening screen, so the
// two-screen layout renders a real address + PIN instead of the "(no network)" / "----" empty state the
// return-0 auto-stubs would give. (Not the firmware — PC gallery only; fictional addresses, never a device's.)
// Scenes (host:<n>): 1 = the longest LAN address (15 characters), 2 = USB-web (the fixed http://192.168.7.1),
// 3 = a browser connected, 4 = no network yet.
static int s_view;
extern "C" void ui_host_scene(int n) { s_view = n; }
extern "C" const char *nucleo_setup_ip(void)      { return s_view == 1 ? "192.168.100.254" : s_view == 4 ? "" : "192.168.1.37"; }
extern "C" const char *nucleo_auth_pin(void)      { return "314159"; }   // six digits, as the device's
extern "C" int         nucleo_ws_shell_count(void){ return s_view == 3 ? 1 : 0; }   // 0 = listening/ready (not yet connected)
extern "C" int         nucleo_ws_client_count(void){ return s_view == 3 ? 1 : 0; }
extern "C" bool        nucleo_usbnet_is_active(void){ return s_view == 2; }  // 2 = USB-web; else the Wi-Fi Remote path
