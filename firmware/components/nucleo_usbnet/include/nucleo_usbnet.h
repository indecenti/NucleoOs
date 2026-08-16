// nucleo_usbnet — web OS over USB as a real network device (NCM), truly plug-and-play.
//
// The Cardputer presents itself to the PC as a USB network card (CDC-NCM). NCM is driverless in-box
// on macOS, Linux and Windows 10 (2004+)/11 — you connect the cable, the device hands the PC an IP
// via its own DHCP server, you open http://192.168.7.1 in ANY browser and the web OS is there. No
// companion, no driver, no install. The device's normal httpd (bound to 0.0.0.0) serves directly on
// the USB interface, so all 72 API handlers + the static shell/app catch-all just work over the cable.
//
// Safety: opt-in via an RTC flag (app_usb key W arms it, then reboots into server-Solo). The flag
// survives the warm reboot but not a cold power-on, so a power-cycle always returns to the normal OS
// (flash + JTAG console never lost). Wi-Fi/LAN is unaffected when the cable isn't used.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Arm USB-net mode (set the RTC flag). The caller reboots into server-Solo right after.
void nucleo_usbnet_web_arm(void);

// Consume the flag once at boot; true if USB-net was armed. Call in main() during server-Solo boot.
bool nucleo_usbnet_web_pending(void);

// Non-consuming peek of the same flag, for an EARLY decision in main() (before Wi-Fi bringup): a
// USB-web boot skips Wi-Fi entirely to free ~48 KB for the web-OS serving heap.
bool nucleo_usbnet_web_armed(void);

// Bring up the USB network card: TinyUSB NCM + an esp_netif with a static IP and a DHCP server.
// Call AFTER nucleo_httpd_start() (the httpd is bound to 0.0.0.0 and will answer on this netif).
void nucleo_usbnet_start(void);

// True once the USB network device is up (for the on-device Remote screen to show the address).
bool nucleo_usbnet_is_active(void);

// True if bring-up was attempted but bailed out safely (e.g. not enough heap) instead of crashing.
bool nucleo_usbnet_failed(void);

#define NUCLEO_USBWEB_IP   "192.168.7.1"
#define NUCLEO_USBWEB_ADDR "http://192.168.7.1"

#ifdef __cplusplus
}
#endif
