// USB HID keyboard: turn the Cardputer into a keyboard for a host PC/phone over USB-C, via
// TinyUSB (the HID class of the same esp_tinyusb stack the USB Drive uses — no new dependency).
//
// Unlike USB Drive (Mass Storage, a dedicated reboot mode), HID is light and touches nothing the
// OS needs, so it runs LIVE inside the USB Keyboard app: open it, type, ESC to leave. Installing
// TinyUSB takes the USB-OTG PHY, so the USB serial console is gone until the next reboot (OTA
// updates over Wi-Fi still work). The driver is installed once and stays resident for the session.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// HID modifier byte bits (USB HID spec) — pass to nucleo_usbhid_key().
#define NK_HIDMOD_CTRL   0x01
#define NK_HIDMOD_SHIFT  0x02
#define NK_HIDMOD_ALT    0x04
#define NK_HIDMOD_GUI    0x08
#define NK_HIDMOD_ALTGR  0x40   // Right Alt (IT layout @ # [ ] { } ...)

// Host LED bitmap bits (what the PC pushes back — the only PC->keyboard feedback over HID).
#define NK_HIDLED_NUM    0x01
#define NK_HIDLED_CAPS   0x02
#define NK_HIDLED_SCROLL 0x04

// A few HID usage IDs the app sends directly (keys with no printable char).
#define NK_HID_ENTER 0x28
#define NK_HID_ESC   0x29
#define NK_HID_BKSP  0x2A
#define NK_HID_TAB   0x2B
#define NK_HID_SPACE 0x2C
#define NK_HID_RIGHT 0x4F
#define NK_HID_LEFT  0x50
#define NK_HID_DOWN  0x51
#define NK_HID_UP    0x52

// Install the USB HID keyboard (idempotent). Safe with no host attached.
esp_err_t nucleo_usbhid_start(void);

// True when a host is connected and ready to accept a report.
bool nucleo_usbhid_ready(void);

// Send one keystroke as press-then-release (HID modifier bitmask + HID usage id). Guaranteed release.
void nucleo_usbhid_key(unsigned char modifier, unsigned char keycode);

// Send a chord: modifier bitmap + up to 6 simultaneous keycodes, then release.
void nucleo_usbhid_report(unsigned char modifier, const unsigned char *keys, int n);

// The host's current LED state (NK_HIDLED_* bits) — Caps/Num/Scroll lock as the PC sees them.
unsigned char nucleo_usbhid_leds(void);

// Translate a printable ASCII char to {modifier, keycode}. Returns false if unmapped.
bool nucleo_usbhid_ascii(char c, unsigned char *modifier, unsigned char *keycode);

// ---- FIDO (CTAPHID) personality --------------------------------------------------------------------
// A SECOND USB personality: instead of a keyboard, present ONE HID interface with the FIDO usage page
// (0xF1D0) and 64-byte IN/OUT reports, which is what an OS security-key stack (Windows WebAuthn,
// Chromium) enumerates. The TinyUSB HID class callbacks live in ONE place (this component), so FIDO is
// wired here rather than as a second, conflicting HID owner. Use start_fido() INSTEAD OF start() —
// only one may install the USB driver per boot (the FIDO app reboots into this personality).
esp_err_t nucleo_usbhid_start_fido(void);
bool      nucleo_usbhid_fido_ready(void);              // host mounted + IN endpoint free
bool      nucleo_usbhid_fido_recv(unsigned char pkt[64]);   // dequeue one 64-byte OUT report (false if none)
bool      nucleo_usbhid_fido_send(const unsigned char pkt[64]); // queue one 64-byte IN report

#ifdef __cplusplus
}
#endif
