// nucleo_fido — device-facing FIDO2 / U2F security-key service.
//
// The pure protocol core lives in core/ (host-tested). This is the on-device
// wiring: mbedTLS crypto, an eFuse-HMAC-derived wrapping key (clone-resistant —
// never in flash), an NVS resident-credential store, and the TinyUSB FIDO HID
// transport (via nucleo_usbhid). Because a FIDO device must be the sole HID
// personality, the app reboots into a dedicated FIDO USB mode (RTC flag).
//
// Innovation over a keyless authenticator: the relying-party id is shown on the
// Cardputer screen before you approve, and user verification is an on-device PIN
// that never crosses USB.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// UI hooks the app supplies. Both block until the user decides and should call
// nucleo_fido_keepalive() periodically so the host does not time out.
typedef struct {
    // Show "<rp> wants to sign in" and wait. Return 1 to approve, 0 to deny.
    int (*present)(const char *rp, void *ui);
    // Prompt for the on-device PIN and verify it. Return 1 if verified. May be
    // NULL (then the key reports no user verification).
    int (*verify)(const char *rp, void *ui);
    void *ui;
} nucleo_fido_ui_t;

// Boot personality (RTC flag). request_mode() sets the flag and reboots.
bool nucleo_fido_boot_mode(void);
void nucleo_fido_request_mode(bool on);

// Bring up the USB FIDO interface + CTAP2/U2F stack. Returns false on failure.
bool nucleo_fido_start(const nucleo_fido_ui_t *ui);
void nucleo_fido_poll(void);        // drain the transport — call from the app loop
void nucleo_fido_keepalive(void);   // send a CTAPHID KEEPALIVE (call while blocking)
void nucleo_fido_stop(void);

bool nucleo_fido_ready(void);           // a host is connected
bool nucleo_fido_uv_configured(void);   // an on-device PIN is set
bool nucleo_fido_key_is_hardware(void); // wrapping key came from eFuse (not NVS fallback)

// On-device PIN (internal user verification). The PIN is only ever entered on the
// Cardputer and checked locally — it never crosses USB.
bool nucleo_fido_pin_is_set(void);
int  nucleo_fido_pin_retries(void);
bool nucleo_fido_pin_set(const char *pin);   // configure/change; false on bad length
int  nucleo_fido_pin_check(const char *pin); // 1 ok, 0 wrong, -1 locked, -2 unset

// On-device credential manager: list and delete resident passkeys straight from
// the Cardputer screen — no host tool needed (Poseidon cannot delete at all).
typedef struct {
    char     rp[65];         // relying-party id
    char     user[65];       // user name (may be empty)
    uint32_t sign_count;
} fido_cred_view_t;

int  nucleo_fido_cred_count(void);
bool nucleo_fido_cred_get(int index, fido_cred_view_t *out);
bool nucleo_fido_cred_delete(int index);

// Admin (web console) — usable in normal mode, no USB. admin_open lazily loads
// the store/PIN; admin_reset wipes all passkeys + PIN + counter.
void nucleo_fido_admin_open(void);
void nucleo_fido_admin_reset(void);

#ifdef __cplusplus
}
#endif
