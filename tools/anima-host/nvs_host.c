// In-memory NVS for host gates (declared in setup-store-shim/nvs.h). Bounded tables, no persistence:
// each test starts from nvs_host_reset().
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

#define NS_MAX  8
#define KEY_MAX 32
#define H_MAX   16

typedef struct { bool used; char name[16]; } ns_t;
typedef struct { bool used; int ns; char key[16]; char *val; } kv_t;
typedef struct { bool used; int ns; nvs_open_mode_t mode; } hd_t;

static ns_t s_ns[NS_MAX];
static kv_t s_kv[KEY_MAX];
static hd_t s_hd[H_MAX];
static bool s_init = true;

void nvs_host_reset(void)
{
    for (int i = 0; i < KEY_MAX; i++) { free(s_kv[i].val); }
    memset(s_ns, 0, sizeof s_ns); memset(s_kv, 0, sizeof s_kv); memset(s_hd, 0, sizeof s_hd);
    s_init = true;
}
void nvs_host_set_initialized(bool on) { s_init = on; }

static int ns_find(const char *name)
{
    for (int i = 0; i < NS_MAX; i++) if (s_ns[i].used && !strcmp(s_ns[i].name, name)) return i;
    return -1;
}
static int kv_find(int ns, const char *key)
{
    for (int i = 0; i < KEY_MAX; i++) if (s_kv[i].used && s_kv[i].ns == ns && !strcmp(s_kv[i].key, key)) return i;
    return -1;
}
static hd_t *hd_get(nvs_handle_t h) { return (h >= 1 && h <= H_MAX && s_hd[h - 1].used) ? &s_hd[h - 1] : NULL; }

int nvs_host_key_count(const char *name)
{
    int ns = ns_find(name), n = 0;
    if (ns < 0) return -1;
    for (int i = 0; i < KEY_MAX; i++) if (s_kv[i].used && s_kv[i].ns == ns) n++;
    return n;
}
int nvs_host_open_handles(void)
{
    int n = 0;
    for (int i = 0; i < H_MAX; i++) if (s_hd[i].used) n++;
    return n;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_init) return ESP_ERR_NVS_NOT_INITIALIZED;
    if (!name || strlen(name) > 15 || !out) return ESP_ERR_INVALID_ARG;
    int ns = ns_find(name);
    if (ns < 0) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        for (int i = 0; i < NS_MAX && ns < 0; i++) if (!s_ns[i].used) ns = i;
        if (ns < 0) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        s_ns[ns].used = true; strcpy(s_ns[ns].name, name);
    }
    for (int i = 0; i < H_MAX; i++)
        if (!s_hd[i].used) { s_hd[i].used = true; s_hd[i].ns = ns; s_hd[i].mode = mode; *out = (nvs_handle_t)(i + 1); return ESP_OK; }
    return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t h) { hd_t *d = hd_get(h); if (d) d->used = false; }

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    hd_t *d = hd_get(h);
    if (!d) return ESP_ERR_NVS_INVALID_HANDLE;
    if (d->mode != NVS_READWRITE) return ESP_ERR_NVS_READ_ONLY;
    if (!key || strlen(key) > 15 || !value) return ESP_ERR_INVALID_ARG;
    int i = kv_find(d->ns, key);
    if (i < 0) {
        for (int j = 0; j < KEY_MAX && i < 0; j++) if (!s_kv[j].used) i = j;
        if (i < 0) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        s_kv[i].used = true; s_kv[i].ns = d->ns; strcpy(s_kv[i].key, key); s_kv[i].val = NULL;
    }
    char *v = malloc(strlen(value) + 1);
    if (!v) return ESP_ERR_NO_MEM;
    strcpy(v, value);
    free(s_kv[i].val); s_kv[i].val = v;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *length)
{
    hd_t *d = hd_get(h);
    if (!d) return ESP_ERR_NVS_INVALID_HANDLE;
    if (!length) return ESP_ERR_INVALID_ARG;
    int i = kv_find(d->ns, key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = strlen(s_kv[i].val) + 1;
    if (!out) { *length = need; return ESP_OK; }
    if (*length < need) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, s_kv[i].val, need); *length = need;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    hd_t *d = hd_get(h);
    if (!d) return ESP_ERR_NVS_INVALID_HANDLE;
    if (d->mode != NVS_READWRITE) return ESP_ERR_NVS_READ_ONLY;
    int i = kv_find(d->ns, key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    free(s_kv[i].val); memset(&s_kv[i], 0, sizeof s_kv[i]);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    hd_t *d = hd_get(h);
    if (!d) return ESP_ERR_NVS_INVALID_HANDLE;
    if (d->mode != NVS_READWRITE) return ESP_ERR_NVS_READ_ONLY;
    for (int i = 0; i < KEY_MAX; i++)
        if (s_kv[i].used && s_kv[i].ns == d->ns) { free(s_kv[i].val); memset(&s_kv[i], 0, sizeof s_kv[i]); }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) { return hd_get(h) ? ESP_OK : ESP_ERR_NVS_INVALID_HANDLE; }
