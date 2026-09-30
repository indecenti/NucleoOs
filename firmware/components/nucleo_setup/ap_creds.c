// Hotspot credential lifecycle. See ap_creds.h.
#include "ap_creds.h"
#include <stdio.h>
#include <string.h>

bool ap_creds_ensure(ap_creds_t *c, const uint8_t mac[6], uint32_t (*rnd)(void))
{
    bool changed = false;
    // Upgrade path: a device still on the old shared defaults gets fresh per-device credentials. (A
    // user who set their own values never matches these exact strings.)
    if (!strcmp(c->pass, AP_CREDS_PASS_LEGACY)) { c->pass[0] = 0; c->open = false; }
    if (!strcmp(c->ssid, AP_CREDS_SSID_LEGACY)) c->ssid[0] = 0;

    if (!c->ssid[0]) {
        snprintf(c->ssid, sizeof c->ssid, "%s-%02X%02X", AP_CREDS_SSID_PREFIX, mac[4], mac[5]);
        changed = true;
    }
    if (!c->pass[0] && !c->open) {                      // never initialised — NOT a deliberate open hotspot
        static const char AB[] = "abcdefghjkmnpqrstuvwxyz23456789";   // 30 chars, no ambiguous 0/O/1/I/l
        for (int i = 0; i < 12; i++) c->pass[i] = AB[rnd() % (sizeof(AB) - 1)];   // ~59 bits of entropy
        c->pass[12] = 0;                                // 12 chars, WPA2-valid
        changed = true;
    }
    return changed;
}

bool ap_creds_set_pass(ap_creds_t *c, const char *p)
{
    if (!p) return false;
    size_t len = strlen(p);
    if ((len > 0 && len < 8) || len >= sizeof c->pass) return false;
    memcpy(c->pass, p, len + 1);
    c->open = (len == 0);
    return true;
}

bool ap_creds_secure(const ap_creds_t *c) { return strlen(c->pass) >= 8; }

void ap_creds_load(ap_creds_t *c, const cJSON *doc)
{
    const cJSON *s = cJSON_GetObjectItem(doc, "ap_ssid");
    const cJSON *p = cJSON_GetObjectItem(doc, "ap_pass");
    const cJSON *o = cJSON_GetObjectItem(doc, "ap_open");
    if (cJSON_IsString(s) && s->valuestring[0]) { strncpy(c->ssid, s->valuestring, sizeof c->ssid - 1); c->ssid[sizeof c->ssid - 1] = 0; }
    if (cJSON_IsString(p)) { strncpy(c->pass, p->valuestring, sizeof c->pass - 1); c->pass[sizeof c->pass - 1] = 0; }
    c->open = cJSON_IsTrue(o) && !c->pass[0];
}

bool ap_creds_save(const ap_creds_t *c, cJSON *doc)
{
    return cJSON_AddStringToObject(doc, "ap_ssid", c->ssid) &&
           cJSON_AddStringToObject(doc, "ap_pass", c->pass) &&   // /cfg + NVS only — never the SD mirror
           cJSON_AddBoolToObject(doc,   "ap_open", c->open);     // not a secret: kept in the SD mirror
}
