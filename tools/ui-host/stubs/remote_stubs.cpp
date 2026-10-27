// Host stubs for the app_remote gallery: a plausible Wi-Fi "Remote Control" listening screen, so the
// two-screen layout renders a real address + PIN instead of the "(no network)" / "----" empty state the
// return-0 auto-stubs would give. (Not the firmware — PC gallery only.)
extern "C" const char *nucleo_setup_ip(void)      { return "192.168.1.37"; }
extern "C" const char *nucleo_auth_pin(void)      { return "7245"; }
extern "C" int         nucleo_ws_shell_count(void){ return 0; }   // 0 = listening/ready (not yet connected)
extern "C" int         nucleo_ws_client_count(void){ return 0; }
extern "C" bool        nucleo_usbnet_is_active(void){ return false; }  // Wi-Fi Remote path (shows the handoff toggle)
