// nucleo_smtp — minimal SMTP-over-TLS sender for NucleoOS.
// Implicit TLS (SMTPS, port 465) + AUTH LOGIN. One TLS session at a time
// (gated through nucleo_arb). Text/plain UTF-8 messages only in v1.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SMTP_TLS_IMPLICIT = 0,  // wrap socket in TLS immediately (port 465) — supported in v1
    SMTP_TLS_STARTTLS = 1,  // plaintext then STARTTLS (port 587) — reserved, not yet implemented
} smtp_tls_mode_t;

// A configured outbound account. Sizes kept tight for NVS/.bss budget.
typedef struct {
    char            name[32];      // friendly label shown in UI
    char            host[64];      // smtp server, e.g. "smtp.gmail.com"
    int             port;          // 465
    smtp_tls_mode_t tls;
    char            user[96];      // SMTP login (usually the full email)
    char            pass[96];      // app password — never sent back to clients
    char            from[96];      // From: address
    char            from_name[48]; // optional display name
} smtp_account_t;

typedef struct {
    const char *to;       // single recipient address (v1)
    const char *subject;  // UTF-8; auto MIME-encoded if non-ASCII
    const char *body;     // UTF-8 text/plain
} smtp_msg_t;

// Send one message. Blocks for the duration of the SMTP exchange.
// On failure writes a short human-readable reason into err (if err && errlen).
// Returns ESP_OK on a 250 final accept, else an esp_err_t.
esp_err_t nucleo_smtp_send(const smtp_account_t *acc, const smtp_msg_t *msg,
                           char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
