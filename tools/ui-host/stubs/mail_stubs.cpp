// Host stub for app_mail.cpp: two FICTIONAL SMTP accounts and the provider presets, so the large layouts render
// with real data. The real store is NVS (nucleo_mailcfg.c); a return-0 stub would only show "+ New account".
// Nothing here touches hardware, files or the network.
#include "nucleo_mailcfg.h"
#include <string.h>
#include <stdio.h>

extern "C" {

static const smtp_preset_t s_presets[] = {
    { "Gmail",        "smtp.gmail.com",       465, SMTP_TLS_IMPLICIT, "App Password (2FA required)" },
    { "Outlook/M365", "smtp.office365.com",   465, SMTP_TLS_IMPLICIT, "App Password if 2FA on" },
    { "Yahoo",        "smtp.mail.yahoo.com",  465, SMTP_TLS_IMPLICIT, "App Password required" },
    { "iCloud",       "smtp.mail.me.com",     465, SMTP_TLS_IMPLICIT, "App-specific password" },
    { "Brevo",        "smtp-relay.brevo.com", 465, SMTP_TLS_IMPLICIT, "Free signup -> SMTP key" },
    { "Custom",       "",                     465, SMTP_TLS_IMPLICIT, "Enter host manually" },
};
int nucleo_mailcfg_preset_count(void) { return (int)(sizeof s_presets / sizeof s_presets[0]); }
const smtp_preset_t *nucleo_mailcfg_preset(int i) { return i >= 0 && i < nucleo_mailcfg_preset_count() ? &s_presets[i] : nullptr; }

static void fill(int idx, smtp_account_t *o)
{
    memset(o, 0, sizeof *o);
    if (idx == 0) {
        snprintf(o->name, sizeof o->name, "Personale"); snprintf(o->host, sizeof o->host, "smtp.example.com");
        snprintf(o->user, sizeof o->user, "luca.ferri@example.com"); snprintf(o->from_name, sizeof o->from_name, "Luca Ferri");
    } else {
        snprintf(o->name, sizeof o->name, "Lavoro"); snprintf(o->host, sizeof o->host, "mail.example.org");
        snprintf(o->user, sizeof o->user, "l.ferri@studio-example.org");
    }
    o->port = 465; o->tls = SMTP_TLS_IMPLICIT;
    snprintf(o->from, sizeof o->from, "%s", o->user);
}
int  nucleo_mailcfg_count(void) { return 2; }
bool nucleo_mailcfg_get(int idx, smtp_account_t *out) { if (idx < 0 || idx > 1) return false; fill(idx, out); snprintf(out->pass, sizeof out->pass, "x"); return true; }
bool nucleo_mailcfg_get_redacted(int idx, smtp_account_t *out) { if (idx < 0 || idx > 1) return false; fill(idx, out); return true; }
int  nucleo_mailcfg_default(void) { return 0; }

}
