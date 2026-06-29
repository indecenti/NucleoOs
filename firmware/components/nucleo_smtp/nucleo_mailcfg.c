// nucleo_mailcfg — NVS-backed SMTP account store. See nucleo_mailcfg.h.
#include "nucleo_mailcfg.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "mailcfg";
#define NS "mail"

// Provider presets — user only supplies email + app password.
static const smtp_preset_t s_presets[] = {
    { "Gmail",          "smtp.gmail.com",            465, SMTP_TLS_IMPLICIT, "App Password (2FA required)" },
    { "Outlook/M365",   "smtp.office365.com",        465, SMTP_TLS_IMPLICIT, "App Password if 2FA on" },
    { "Yahoo",          "smtp.mail.yahoo.com",       465, SMTP_TLS_IMPLICIT, "App Password required" },
    { "iCloud",         "smtp.mail.me.com",          465, SMTP_TLS_IMPLICIT, "App-specific password" },
    { "Brevo",          "smtp-relay.brevo.com",      465, SMTP_TLS_IMPLICIT, "Free signup -> SMTP key" },
    { "SMTP2GO",        "mail.smtp2go.com",          465, SMTP_TLS_IMPLICIT, "Free signup -> SMTP user/pass" },
    { "Mailjet",        "in-v3.mailjet.com",         465, SMTP_TLS_IMPLICIT, "Free signup -> API key/secret" },
    { "Custom",         "",                          465, SMTP_TLS_IMPLICIT, "Enter host manually" },
};
#define NPRESET ((int)(sizeof(s_presets)/sizeof(s_presets[0])))

int nucleo_mailcfg_preset_count(void) { return NPRESET; }
const smtp_preset_t *nucleo_mailcfg_preset(int i) {
    return (i >= 0 && i < NPRESET) ? &s_presets[i] : NULL;
}

static void acc_key(int idx, char *out) { snprintf(out, 8, "acc%d", idx); }

int nucleo_mailcfg_count(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    nvs_get_u8(h, "count", &n);
    nvs_close(h);
    if (n > MAIL_MAX_ACCOUNTS) n = MAIL_MAX_ACCOUNTS;
    return n;
}

bool nucleo_mailcfg_get(int idx, smtp_account_t *out) {
    if (!out || idx < 0 || idx >= nucleo_mailcfg_count()) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[8]; acc_key(idx, key);
    size_t sz = sizeof(*out);
    esp_err_t e = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    return e == ESP_OK && sz == sizeof(*out);
}

bool nucleo_mailcfg_get_redacted(int idx, smtp_account_t *out) {
    if (!nucleo_mailcfg_get(idx, out)) return false;
    memset(out->pass, 0, sizeof(out->pass));
    return true;
}

int nucleo_mailcfg_set(int idx, const smtp_account_t *acc) {
    if (!acc) return -1;
    int count = nucleo_mailcfg_count();
    if (idx < 0) {                       // append
        if (count >= MAIL_MAX_ACCOUNTS) return -1;
        idx = count;
    } else if (idx >= MAIL_MAX_ACCOUNTS) {
        return -1;
    }

    smtp_account_t rec = *acc;
    // Write-only password: empty pass on an existing slot keeps the stored one.
    if (rec.pass[0] == 0 && idx < count) {
        smtp_account_t old;
        if (nucleo_mailcfg_get(idx, &old)) memcpy(rec.pass, old.pass, sizeof(rec.pass));
    }

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    char key[8]; acc_key(idx, key);
    esp_err_t e = nvs_set_blob(h, key, &rec, sizeof(rec));
    if (e == ESP_OK && idx >= count) e = nvs_set_u8(h, "count", (uint8_t)(idx + 1));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "set %d failed: %s", idx, esp_err_to_name(e)); return -1; }
    return idx;
}

esp_err_t nucleo_mailcfg_delete(int idx) {
    int count = nucleo_mailcfg_count();
    if (idx < 0 || idx >= count) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    // Shift later accounts down to keep the list dense.
    for (int i = idx; i < count - 1; ++i) {
        smtp_account_t tmp;
        char k0[8], k1[8]; acc_key(i, k0); acc_key(i + 1, k1);
        size_t sz = sizeof(tmp);
        if (nvs_get_blob(h, k1, &tmp, &sz) == ESP_OK)
            nvs_set_blob(h, k0, &tmp, sizeof(tmp));
    }
    char klast[8]; acc_key(count - 1, klast);
    nvs_erase_key(h, klast);
    nvs_set_u8(h, "count", (uint8_t)(count - 1));
    // Fix up default index.
    uint8_t def = 0; nvs_get_u8(h, "default", &def);
    if (def == idx) nvs_set_u8(h, "default", 0);
    else if (def > idx) nvs_set_u8(h, "default", def - 1);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

int nucleo_mailcfg_default(void) {
    int count = nucleo_mailcfg_count();
    if (count == 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t def = 0;
    nvs_get_u8(h, "default", &def);
    nvs_close(h);
    return (def < count) ? def : 0;
}

void nucleo_mailcfg_log_sent(const char *to, const char *subject, bool ok) {
    mkdir("/sd/system", 0775); mkdir("/sd/system/mail", 0775);
    FILE *f = fopen("/sd/system/mail/sent.log", "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char ts[20]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tmv);
    fprintf(f, "%s | %s | %s | %s\n", ts, ok ? "OK" : "ERR",
            to ? to : "", (subject && subject[0]) ? subject : "(no subj)");
    fclose(f);
}

void nucleo_mailcfg_set_default(int idx) {
    if (idx < 0 || idx >= nucleo_mailcfg_count()) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "default", (uint8_t)idx);
    nvs_commit(h);
    nvs_close(h);
}
