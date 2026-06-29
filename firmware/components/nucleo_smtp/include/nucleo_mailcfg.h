// nucleo_mailcfg — account store for nucleo_smtp, persisted in NVS.
// Credentials live in flash (NVS namespace "mail"), never on the SD card.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "nucleo_smtp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAIL_MAX_ACCOUNTS 4

// Provider preset: pre-fills host/port/tls so the user only enters email + app password.
typedef struct {
    const char     *name;  // "Gmail"
    const char     *host;  // "smtp.gmail.com"
    int             port;  // 465
    smtp_tls_mode_t tls;
    const char     *hint;  // short UI note (e.g. "App Password — 2FA required")
} smtp_preset_t;

int                 nucleo_mailcfg_preset_count(void);
const smtp_preset_t *nucleo_mailcfg_preset(int i);

// Account list. Index 0..count-1.
int  nucleo_mailcfg_count(void);
// Full copy incl. password — for the sender only.
bool nucleo_mailcfg_get(int idx, smtp_account_t *out);
// Copy with pass[] blanked — safe to expose to clients/UI.
bool nucleo_mailcfg_get_redacted(int idx, smtp_account_t *out);

// Add (idx<0 appends) or overwrite an account. If acc->pass is empty and the
// slot already exists, the stored password is kept (write-only update).
// Returns the resulting index (>=0) or -1 on error / full.
int  nucleo_mailcfg_set(int idx, const smtp_account_t *acc);
esp_err_t nucleo_mailcfg_delete(int idx);

int  nucleo_mailcfg_default(void);          // -1 if none
void nucleo_mailcfg_set_default(int idx);

// Append a record to the SD sent-mail log, shared by the native app and the web endpoint.
// Format: "YYYY-MM-DD HH:MM | OK|ERR | <to> | <subject>".
void nucleo_mailcfg_log_sent(const char *to, const char *subject, bool ok);

#ifdef __cplusplus
}
#endif
