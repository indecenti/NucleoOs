// fido_bootmode — reboot into (and out of) the dedicated FIDO USB personality.
//
// A FIDO authenticator must be the sole HID interface for the OS to enumerate it
// as a security key, and the USB driver is installed once per boot. So entering
// the security key means rebooting with a flag set. RTC_NOINIT (not RTC_DATA)
// survives a software reset; the magic value filters cold-boot garbage.
#include "fido_port.h"
#include "esp_attr.h"
#include "esp_system.h"

static RTC_NOINIT_ATTR uint32_t s_fido_mode;
#define FIDO_KEY_MAGIC 0x4649444Fu   // 'FIDO'

bool fido_boot_key_mode(void) { return s_fido_mode == FIDO_KEY_MAGIC; }

void fido_request_key_mode(bool on) {
    s_fido_mode = on ? FIDO_KEY_MAGIC : 0;
    esp_restart();
}
