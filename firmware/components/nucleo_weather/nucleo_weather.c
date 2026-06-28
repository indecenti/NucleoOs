// nucleo_weather — device fetch: ip-api geolocation (plain HTTP) + Open-Meteo current/daily (HTTPS),
// parsed with cJSON into the model. Mirrors the ANIMA online http_get: lazy body growth + the heavy-work
// arbiter token to serialize the TLS window so a weather fetch can't OOM the PSRAM-less heap.
#include "nucleo_weather.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "nucleo_arb.h"

static const char *TAG = "weather";
#define WX_CACHE     "/sd/data/weather_loc.json"
#define HTTP_CAP     (16 * 1024)
#define HTTP_TIMEOUT 5000   // <8s task WDT; the app feeds the WDT between the (sequential) geo+forecast GETs

typedef struct { char *buf; int cap, len, max; } acc_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    acc_t *a = (acc_t *)e->user_data;
    if (!a) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) a->len = 0;   // drop any redirect body
    else if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        int want = a->len + e->data_len + 1;
        if (want > a->cap) {
            int nc = a->cap ? a->cap : 1024; while (nc < want) nc <<= 1; if (nc > a->max) nc = a->max;
            char *nb = realloc(a->buf, nc); if (!nb) return ESP_OK;
            a->buf = nb; a->cap = nc;
        }
        int n = e->data_len; if (n > a->cap - 1 - a->len) n = a->cap - 1 - a->len;
        if (n > 0) { memcpy(a->buf + a->len, e->data, n); a->len += n; }
    }
    return ESP_OK;
}

// GET url into a heap buffer (caller frees). Returns bytes or -1. HTTPS uses the bundled CA roots and
// is gated by a heap floor + the arbiter (try-only) so it never OOMs or self-deadlocks on this chip.
static int http_get(const char *url, char **out)
{
    *out = NULL;
    bool https = (strncmp(url, "https", 5) == 0);
    // TLS handshake needs a contiguous block. The app frees it (nucleo_exclusive NX_NET_APP) before
    // calling, so this is just a floor against a guaranteed OOM, not the 12KB it used to (wrongly) demand.
    if (https && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 9 * 1024) {
        ESP_LOGW(TAG, "heap too low for TLS (largest=%u)", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return -1;
    }
    acc_t acc = { NULL, 0, 0, HTTP_CAP };
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = HTTP_TIMEOUT, .buffer_size = 2048, .buffer_size_tx = 1024,
        .max_redirection_count = 4, .event_handler = http_evt, .user_data = &acc,
    };
    if (https) cfg.crt_bundle_attach = esp_crt_bundle_attach;

    uint32_t tk = nucleo_arb_acquire(ARB_FG, "weather-get", 0);   // try-only: busy -> bail honestly
    if (!tk) { ESP_LOGW(TAG, "arbiter busy"); return -1; }
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { nucleo_arb_release(tk); free(acc.buf); return -1; }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    nucleo_arb_release(tk);

    if (err == ESP_OK && status == 200 && acc.buf) { acc.buf[acc.len] = 0; *out = acc.buf; return acc.len; }
    free(acc.buf);
    ESP_LOGW(TAG, "GET %d (%s) %s", status, esp_err_to_name(err), url);
    return -1;
}

static double darr(cJSON *a, int i)
{
    cJSON *e = cJSON_IsArray(a) ? cJSON_GetArrayItem(a, i) : NULL;
    return (e && cJSON_IsNumber(e)) ? e->valuedouble : 0;
}
static double dnum(cJSON *o, const char *k)
{
    cJSON *e = cJSON_GetObjectItem(o, k);
    return (e && cJSON_IsNumber(e)) ? e->valuedouble : 0;
}

bool nucleo_weather_locate_ip(nucleo_weather_t *w)
{
    char *body = NULL;
    if (http_get("http://ip-api.com/json/?fields=status,city,lat,lon", &body) < 0) return false;
    bool ok = false; cJSON *j = cJSON_Parse(body); free(body);
    if (j) {
        cJSON *st = cJSON_GetObjectItem(j, "status");
        if (cJSON_IsString(st) && !strcmp(st->valuestring, "success")) {
            cJSON *city = cJSON_GetObjectItem(j, "city");
            if (cJSON_IsString(city)) snprintf(w->place, sizeof w->place, "%s", city->valuestring);
            w->lat = dnum(j, "lat"); w->lon = dnum(j, "lon");
            ok = (w->place[0] != 0);
        }
        cJSON_Delete(j);
    }
    return ok;
}

bool nucleo_weather_locate_city(const char *city, nucleo_weather_t *w)
{
    // percent-encode so spaces and accented (UTF-8) city names resolve (e.g. "Forlì", "L'Aquila")
    char enc[120]; int j = 0;
    for (int i = 0; city[i] && j < (int)sizeof(enc) - 4; i++) {
        unsigned char c = (unsigned char)city[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) enc[j++] = c;
        else if (c == ' ') enc[j++] = '+';
        else { snprintf(enc + j, 4, "%%%02X", c); j += 3; }
    }
    enc[j] = 0;
    char url[200];
    snprintf(url, sizeof url, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=it", enc);
    char *body = NULL; if (http_get(url, &body) < 0) return false;
    bool ok = false; cJSON *root = cJSON_Parse(body); free(body);
    if (root) {
        cJSON *res = cJSON_GetObjectItem(root, "results");
        cJSON *r0 = res ? cJSON_GetArrayItem(res, 0) : NULL;
        if (r0) {
            cJSON *nm = cJSON_GetObjectItem(r0, "name");
            if (cJSON_IsString(nm)) snprintf(w->place, sizeof w->place, "%s", nm->valuestring);
            w->lat = dnum(r0, "latitude"); w->lon = dnum(r0, "longitude");
            ok = (w->place[0] != 0);
        }
        cJSON_Delete(root);
    }
    return ok;
}

bool nucleo_weather_fetch(nucleo_weather_t *w)
{
    char url[400];
    snprintf(url, sizeof url,
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,wind_speed_10m,wind_direction_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&timezone=auto&forecast_days=6", w->lat, w->lon);
    char *body = NULL; if (http_get(url, &body) < 0) return false;
    cJSON *root = cJSON_Parse(body); free(body);
    if (!root) return false;

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        w->temp = dnum(cur, "temperature_2m"); w->feels = dnum(cur, "apparent_temperature");
        w->humidity = (int)dnum(cur, "relative_humidity_2m"); w->wind = dnum(cur, "wind_speed_10m");
        w->wind_dir = (int)dnum(cur, "wind_direction_10m"); w->code = (int)dnum(cur, "weather_code");
        w->is_day = (int)dnum(cur, "is_day"); w->precip_mm = dnum(cur, "precipitation");
        cJSON *tm = cJSON_GetObjectItem(cur, "time");
        if (cJSON_IsString(tm)) { const char *p = strchr(tm->valuestring, 'T'); if (p) snprintf(w->updated, sizeof w->updated, "%.5s", p + 1); }
    }
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    w->n_days = 0;
    if (daily) {
        cJSON *tms = cJSON_GetObjectItem(daily, "time"), *cd = cJSON_GetObjectItem(daily, "weather_code"),
              *mx = cJSON_GetObjectItem(daily, "temperature_2m_max"), *mn = cJSON_GetObjectItem(daily, "temperature_2m_min"),
              *pp = cJSON_GetObjectItem(daily, "precipitation_probability_max");
        int n = cJSON_GetArraySize(tms); if (n > WX_MAX_DAYS) n = WX_MAX_DAYS;
        for (int i = 0; i < n; i++) {
            cJSON *ti = cJSON_GetArrayItem(tms, i); int y, m, d;
            w->day[i].dow = (cJSON_IsString(ti) && weather_parse_date(ti->valuestring, &y, &m, &d)) ? weather_dow(y, m, d) : 0;
            w->day[i].code = (int)darr(cd, i);
            w->day[i].tmax = darr(mx, i); w->day[i].tmin = darr(mn, i);
            w->day[i].precip_prob = (int)darr(pp, i);
        }
        w->n_days = n;
    }
    cJSON_Delete(root);
    w->valid = true;
    return true;
}

bool nucleo_weather_cache_load(nucleo_weather_t *w)
{
    FILE *f = fopen(WX_CACHE, "rb"); if (!f) return false;
    char buf[256]; int n = (int)fread(buf, 1, sizeof buf - 1, f); fclose(f);
    if (n <= 0) return false;
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf); if (!j) return false;
    cJSON *pl = cJSON_GetObjectItem(j, "place");
    bool ok = false;
    if (cJSON_IsString(pl)) { snprintf(w->place, sizeof w->place, "%s", pl->valuestring); w->lat = dnum(j, "lat"); w->lon = dnum(j, "lon"); ok = true; }
    cJSON_Delete(j);
    return ok;
}

void nucleo_weather_cache_save(const nucleo_weather_t *w)
{
    FILE *f = fopen(WX_CACHE, "wb"); if (!f) return;
    fprintf(f, "{\"place\":\"%s\",\"lat\":%.4f,\"lon\":%.4f}", w->place, w->lat, w->lon);
    fclose(f);
}
