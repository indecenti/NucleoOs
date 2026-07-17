// fido_service — assemble the FIDO stack on device and pump the CTAPHID transport.
//
// Ties together: mbedTLS crypto, the eFuse-derived wrapping key, the NVS resident
// store + persisted counter, the U2F and CTAP2 handlers, and the TinyUSB FIDO HID
// transport (via nucleo_usbhid). OUT reports are drained on the app loop, not the
// USB callback, because the CTAPHID dispatch blocks on the on-device approval.
#include "nucleo_fido.h"
#include "fido_port.h"
#include "fido_ctap2.h"
#include "fido_u2f.h"
#include "fido_ctaphid.h"
#include "fido_pin.h"
#include "fido_attestation.h"
#include "nucleo_usbhid.h"
#include <string.h>
#include <stdlib.h>

// Stable model id for this authenticator (16 bytes).
static const uint8_t AAGUID[16] = { 'N','U','C','L','E','O','O','S','-','F','I','D','O','-','0','1' };

#define FIDO_RBUF_LEN 2048

static uint8_t  s_devkey[32];
static bool     s_hw_key = false;
static uint32_t s_counter, s_persisted;
static fido_u2f_cfg    s_u2f;
static fido_ctap2_cfg  s_c2;
// The CTAPHID context (~2.1 KB, incl. the reassembly buffer) and the response
// scratch (2 KB) are HEAP-allocated only while the security key runs (key mode) —
// never static .bss. On this no-PSRAM chip every resident KB is heap the OS boot
// can't use; keeping ~4 KB out of .bss is what lets the device boot (see the
// project boot-RAM discipline: buffers heap-on-enter, not .bss).
static fido_ctaphid_ctx *s_hid;
static uint8_t          *s_rbuf;
static nucleo_fido_ui_t s_ui;
static fido_pin_state s_pin;
static bool     s_started = false;

// authenticatorReset side-effect: forget the device PIN too.
static void svc_on_reset(void *ui) {
    (void)ui;
    fido_pin_init(&s_pin);
    fido_pin_erase();
    s_c2.uv_configured = false;
}

// Bridge the core callbacks to the app's UI (the core passes cfg->ui = NULL; we
// hold the real UI struct at module scope).
static int c2_present(void *ui, const char *rp) { (void)ui; return s_ui.present ? s_ui.present(rp, s_ui.ui) : 0; }
static int c2_verify(void *ui, const char *rp)  { (void)ui; return s_ui.verify  ? s_ui.verify(rp, s_ui.ui)  : 0; }
static int u2f_present(void *ui)                { (void)ui; return s_ui.present ? s_ui.present("U2F request", s_ui.ui) : 0; }

static void sink(const uint8_t pkt[FIDO_HID_PKT], void *ctx) { (void)ctx; nucleo_usbhid_fido_send(pkt); }
static uint16_t msg_thunk(const uint8_t *r, uint16_t rl, uint8_t *o, uint16_t cap, void *ctx) {
    return fido_u2f_handle((const fido_u2f_cfg *)ctx, r, rl, o, cap);
}
static uint16_t cbor_thunk(const uint8_t *r, uint16_t rl, uint8_t *o, uint16_t cap, void *ctx) {
    return fido_ctap2_handle((const fido_ctap2_cfg *)ctx, r, rl, o, cap);
}

bool nucleo_fido_boot_mode(void) { return fido_boot_key_mode(); }
void nucleo_fido_request_mode(bool on) { fido_request_key_mode(on); }

static bool s_loaded = false;
// Load the wrapping key, PIN and resident store WITHOUT bringing USB up, so the
// web console can read/manage passkeys in normal mode (not only in key mode).
static void ensure_loaded(void) {
    if (s_loaded) return;
    fido_derive_devkey(s_devkey, &s_hw_key);
    s_counter = fido_counter_load();
    s_persisted = s_counter;
    if (!fido_pin_load(&s_pin)) fido_pin_init(&s_pin);
    memset(&s_c2, 0, sizeof s_c2);
    s_c2.cy = fido_mbedtls_crypto();
    s_c2.devkey = s_devkey;
    s_c2.aaguid = AAGUID;
    s_c2.counter = &s_counter;
    s_c2.store = fido_cred_store_nvs();
    s_c2.uv_configured = fido_pin_is_set(&s_pin);
    s_loaded = true;
}
void nucleo_fido_admin_open(void) { ensure_loaded(); }

bool nucleo_fido_start(const nucleo_fido_ui_t *ui) {
    if (s_started) return true;
    s_ui = *ui;
    ensure_loaded();
    const fido_crypto_t *cy = s_c2.cy;

    memset(&s_u2f, 0, sizeof s_u2f);
    s_u2f.cy = cy; s_u2f.devkey = s_devkey;
    s_u2f.att_cert = FIDO_ATT_CERT; s_u2f.att_cert_len = FIDO_ATT_CERT_LEN; s_u2f.att_priv = FIDO_ATT_PRIV;
    s_u2f.counter = &s_counter; s_u2f.user_present = u2f_present; s_u2f.ui = NULL;

    // s_c2 base fields were set by ensure_loaded(); add the interactive callbacks.
    s_c2.user_present = c2_present;
    s_c2.user_verify  = c2_verify;                // UV-capable; gated by uv_configured
    s_c2.on_reset = svc_on_reset;
    s_c2.ui = NULL;

    if (!s_rbuf) s_rbuf = malloc(FIDO_RBUF_LEN);
    if (!s_hid)  s_hid  = malloc(sizeof *s_hid);
    if (!s_rbuf || !s_hid) return false;          // no heap for the transport — bail, don't crash
    fido_ctaphid_init(s_hid, sink, NULL, s_rbuf, FIDO_RBUF_LEN);
    fido_ctaphid_set_msg(s_hid, msg_thunk, &s_u2f);
    fido_ctaphid_set_cbor(s_hid, cbor_thunk, &s_c2);

    if (nucleo_usbhid_start_fido() != ESP_OK) return false;
    s_started = true;
    return true;
}

void nucleo_fido_poll(void) {
    if (!s_started || !s_hid) return;
    uint8_t pkt[64];
    while (nucleo_usbhid_fido_recv(pkt)) fido_ctaphid_rx(s_hid, pkt);
    if (s_counter != s_persisted) { fido_counter_store(s_counter); s_persisted = s_counter; }
}

void nucleo_fido_keepalive(void) { if (s_hid) fido_ctaphid_keepalive(s_hid, FIDO_HID_KA_UPNEEDED); }

void nucleo_fido_stop(void) {
    if (s_counter != s_persisted) { fido_counter_store(s_counter); s_persisted = s_counter; }
    s_started = false;
}

bool nucleo_fido_ready(void)          { return nucleo_usbhid_fido_ready(); }
bool nucleo_fido_uv_configured(void)  { ensure_loaded(); return fido_pin_is_set(&s_pin); }
bool nucleo_fido_key_is_hardware(void){ ensure_loaded(); return s_hw_key; }

// On-device PIN — only ever entered on the Cardputer, checked locally.
bool nucleo_fido_pin_is_set(void)  { ensure_loaded(); return fido_pin_is_set(&s_pin); }
int  nucleo_fido_pin_retries(void) { ensure_loaded(); return fido_pin_retries(&s_pin); }
bool nucleo_fido_pin_set(const char *pin) {
    ensure_loaded();
    if (fido_pin_set(&s_pin, s_c2.cy, s_devkey, pin)) return false;
    fido_pin_store(&s_pin);
    s_c2.uv_configured = true;
    return true;
}
int nucleo_fido_pin_check(const char *pin) {
    ensure_loaded();
    int r = fido_pin_check(&s_pin, s_c2.cy, s_devkey, pin);
    if (r == 0 || r == 1) fido_pin_store(&s_pin);   // persist the retry counter
    return r;
}

// Credential manager (list / delete resident passkeys) — device screen or web.
int nucleo_fido_cred_count(void) {
    ensure_loaded();
    return s_c2.store ? s_c2.store->count(s_c2.store) : 0;
}
bool nucleo_fido_cred_get(int index, fido_cred_view_t *out) {
    ensure_loaded();
    if (!s_c2.store) return false;
    fido_cred_record r;
    if (s_c2.store->get_at(s_c2.store, index, &r)) return false;
    memset(out, 0, sizeof *out);
    strncpy(out->rp,   r.rpId,     sizeof out->rp - 1);
    strncpy(out->user, r.userName, sizeof out->user - 1);
    out->sign_count = r.signCount;
    return true;
}
bool nucleo_fido_cred_delete(int index) {
    ensure_loaded();
    if (!s_c2.store) return false;
    fido_cred_record r;
    if (s_c2.store->get_at(s_c2.store, index, &r)) return false;
    return s_c2.store->remove(s_c2.store, r.id) == 0;
}

// Reset from the web console (wipe all resident passkeys + PIN + counter).
void nucleo_fido_admin_reset(void) {
    ensure_loaded();
    if (s_c2.store) s_c2.store->wipe_all(s_c2.store);
    s_counter = 0; fido_counter_store(0); s_persisted = 0;
    fido_pin_init(&s_pin); fido_pin_erase();
    s_c2.uv_configured = false;
}
