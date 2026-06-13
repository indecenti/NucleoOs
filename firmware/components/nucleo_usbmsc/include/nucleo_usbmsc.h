// USB Mass Storage mode: expose the SD card to a host PC as a USB drive, via TinyUSB (the
// standard way on a native-USB MCU — same approach as CircuitPython / Flipper / the ESP-IDF
// tusb_msc example). See docs/usb-msc.md.
//
// Design: a DEDICATED reboot mode, not a live overlay. The ESP32-S3's USB-OTG (TinyUSB) and the
// USB-Serial/JTAG console share the same D+/D- pins, and a host writing the FAT while the firmware
// also has it mounted would corrupt it. So entering "USB Drive" sets a reboot flag and restarts;
// the next boot runs ONLY the USB-MSC loop (no Wi-Fi, no apps, nothing else touching the SD) and
// any key / reset / power-cycle restarts back into the normal OS. Mutual exclusion = no corruption.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Reboot into USB Mass Storage mode (sets the flag, then esp_restart()). Does not return.
void nucleo_usbmsc_request(void);

// True exactly once, when this boot was asked to enter USB mode; consumes the flag so a later
// plain reset can't get stuck in USB mode. Call very early in app_main, before normal services.
bool nucleo_usbmsc_pending(void);

// Run the dedicated USB-drive loop: bring up TinyUSB MSC over the mounted SD card, show a status
// screen, and wait. Never returns — any key (or reset/power) restarts back into the normal OS.
void nucleo_usbmsc_run(void);

#ifdef __cplusplus
}
#endif
