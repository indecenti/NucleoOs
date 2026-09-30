// guest_policy.c — see include/guest_policy.h. Pure C, host-gated (npm run guest:test).
#include "guest_policy.h"
#include <string.h>

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int guest_parse_table(const uint8_t *buf, size_t len, guest_part_t *out, int cap)
{
    if (!buf || !out || cap <= 0) return -1;
    if (len > GUEST_PT_MAX_BYTES) len = GUEST_PT_MAX_BYTES;
    if (len < GUEST_PT_ENTRY_SIZE || buf[0] != 0xAA || buf[1] != 0x50) return -1;
    int n = 0;
    for (size_t off = 0; off + GUEST_PT_ENTRY_SIZE <= len && n < cap; off += GUEST_PT_ENTRY_SIZE) {
        const uint8_t *e = buf + off;
        if (e[0] == 0xEB && e[1] == 0xEB) break;    // MD5 row closes the table
        if (e[0] == 0xFF && e[1] == 0xFF) break;    // erased flash
        if (e[0] != 0xAA || e[1] != 0x50) break;    // anything else: stop, never guess
        guest_part_t *p = &out[n++];
        p->type = e[2];
        p->subtype = e[3];
        p->offset = rd_le32(e + 4);
        p->size = rd_le32(e + 8);
        memcpy(p->label, e + 12, 16);
        p->label[16] = '\0';
    }
    return n;
}

bool guest_is_hosted(const guest_part_t *parts, int n, uint32_t running_offset)
{
    if (!parts) return false;
    for (int i = 0; i < n; i++)
        if (parts[i].type == GUEST_PT_TYPE_APP && parts[i].subtype == GUEST_PT_SUB_TEST &&
            parts[i].offset != running_offset)
            return true;
    return false;
}

guest_return_t guest_return_mode(bool has_ddlb, uint8_t ddlb, bool has_on_key, int32_t on_key_gpio)
{
    // Same order as the Launcher bootloader: a configured key overrides every reset reason.
    if (has_on_key && on_key_gpio > 0) return GUEST_RET_HOLD_KEY;
    if (has_ddlb && ddlb != 0) return GUEST_RET_POWER_CYCLE;
    return GUEST_RET_DEEP_SLEEP;
}

bool guest_self_ota_allowed(bool hosted) { return !hosted; }

void guest_launcher_plan(const guest_part_t *parts, int n, guest_install_plan_t *plan)
{
    if (!plan) return;
    memset(plan, 0, sizeof *plan);
    if (!parts) return;
    for (int i = 0; i < n; i++) {
        const guest_part_t *p = &parts[i];
        if (p->type == GUEST_PT_TYPE_APP && !plan->app_found &&
            (p->subtype == GUEST_PT_SUB_FACTORY || p->subtype == GUEST_PT_SUB_OTA_0 || p->subtype == GUEST_PT_SUB_TEST)) {
            plan->app_found = true;
            plan->app_offset = p->offset;
            plan->app_declared_size = p->size;
        }
        if (p->type == GUEST_PT_TYPE_DATA &&
            (p->subtype == GUEST_PT_SUB_SPIFFS || p->subtype == GUEST_PT_SUB_LITTLEFS || p->subtype == GUEST_PT_SUB_FAT) &&
            plan->n_data < (int)(sizeof plan->data_label / sizeof plan->data_label[0])) {
            // Launcher names an unlabelled SPIFFS/LittleFS partition "spiffs"; FAT keeps whatever it has.
            const char *lbl = (p->label[0] || p->subtype == GUEST_PT_SUB_FAT) ? p->label : "spiffs";
            memcpy(plan->data_label[plan->n_data], lbl, strlen(lbl) + 1);
            plan->data_subtype[plan->n_data] = p->subtype;
            plan->n_data++;
        }
    }
}
