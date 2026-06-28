#include "nucleo_fsapi.h"
#include "nucleo_auth.h"
#include "nucleo_board.h"
#include "nucleo_fsprotect.h"   // nucleo_fs_is_protected() — the one place the policy lives
#include "nucleo_fsfactory.h"   // nucleo_fs_is_factory() — bundled DOS/ROMs games are pinned too
#include "nucleo_eventbus.h"
#include "nucleo_storage.h"
#include "nucleo_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>

static const char *TAG = "fsapi";

// Upload ceiling. The drag-and-drop uploader (file-commander) and any client write
// is bounded to this; bigger bodies are rejected up-front so a runaway transfer can
// never fill the card or wedge the socket.
#define NUCLEO_MAX_UPLOAD_BYTES (640ULL * 1024 * 1024)   // 640 MB

// Streaming-write buffer. A larger block means far fewer FATFS/SD transactions per
// megabyte, which is what makes a 640 MB upload finish in a sane time. Heap-allocated
// (not on the 8 KB httpd task stack).
#define NUCLEO_WRITE_CHUNK 16384

static int hexval(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c | 32) - 'a' + 10; }

static const char *content_type(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "text/plain";
    if (!strcmp(e, ".mp3")) return "audio/mpeg";
    if (!strcmp(e, ".wav")) return "audio/wav";
    if (!strcmp(e, ".mp4") || !strcmp(e, ".mov")) return "video/mp4";
    if (!strcmp(e, ".webm")) return "video/webm";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".jpg") || !strcmp(e, ".jpeg")) return "image/jpeg";
    if (!strcmp(e, ".gif")) return "image/gif";
    if (!strcmp(e, ".svg")) return "image/svg+xml";
    if (!strcmp(e, ".json")) return "application/json";
    return "text/plain";
}

// Resolve a named query key (e.g. "path", "from", "to") to an absolute SD path.
// Rejects path traversal. The query buffer is generous so multi-arg calls
// (move: ?from=...&to=...) fit. URL-decode is minimal (%xx + '+').
static bool resolve_key(httpd_req_t *req, const char *key, char *abs, size_t n)
{
    char q[512], raw[200] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, raw, sizeof(raw)) != ESP_OK) return false;

    char dec[200]; size_t j = 0;                 // minimal URL-decode
    for (size_t i = 0; raw[i] && j < sizeof(dec) - 1; i++) {
        if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) { dec[j++] = (char)(hexval(raw[i + 1]) * 16 + hexval(raw[i + 2])); i += 2; }
        else if (raw[i] == '+') dec[j++] = ' ';
        else dec[j++] = raw[i];
    }
    dec[j] = '\0';
    if (strstr(dec, "..")) return false;
    snprintf(abs, n, NUCLEO_SD_MOUNT "%s%s", dec[0] == '/' ? "" : "/", dec);
    return true;
}
// Back-compat helper for the common single-path case (?path=...).
static bool resolve_path(httpd_req_t *req, char *abs, size_t n)
{
    return resolve_key(req, "path", abs, n);
}

static void publish_change(const char *op, const char *path)
{
    // Bound the embedded path so the JSON is ALWAYS valid and fits ONE event payload slot. A path can
    // be up to ~203 chars (resolve_key: /sd + dec[200]); wrapped, that exceeds NUCLEO_EVENT_PAYLOAD_MAX
    // and the ring's strncpy would truncate it MID-STRING into corrupt/unterminated JSON, which breaks
    // the client's parse of the WHOLE delta. Instead clip only the path (%.*s) so the object stays
    // valid: short system paths (settings.json/ui-state.json/trash.json — the only ones consumers
    // suffix-match) are unaffected, and a pathological deep path still yields a parseable event the
    // path-agnostic consumers (FsIndex.invalidate / tree rebuild) resync on. Skeleton is 19 chars.
    char d[NUCLEO_EVENT_PAYLOAD_MAX];
    int avail = (int)sizeof(d) - 1 - 19 - (int)strlen(op);
    if (avail < 0) avail = 0;
    snprintf(d, sizeof(d), "{\"op\":\"%s\",\"path\":\"%.*s\"}", op, avail, path);
    nucleo_event_publish("fs.changed", d);
}

static esp_err_t list_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char abs[256];
    if (!resolve_path(req, abs, sizeof(abs))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path"); return ESP_FAIL; }
    DIR *dir = opendir(abs);
    if (!dir) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no dir"); return ESP_FAIL; }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char full[300]; snprintf(full, sizeof(full), "%s/%s", abs, de->d_name);
        struct stat st = {0}; stat(full, &st);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        bool is_dir = S_ISDIR(st.st_mode);
        cJSON_AddStringToObject(e, "type", is_dir ? "dir" : "file");
        cJSON_AddNumberToObject(e, "size", (double)st.st_size);
        // Tell the client which entries are protected system files so it can show a lock
        // and gray out delete/rename/cut. Authoritative enforcement is server-side (above);
        // this is only UX. Emitted only when true to keep the listing JSON small. Bundled
        // factory games are marked in a second pass below (one streaming read of .factory).
        if (nucleo_fs_is_protected(full)) cJSON_AddBoolToObject(e, "protected", true);
        if (is_dir) {
            bool has_subdirs = false;
            DIR *subdir = opendir(full);
            if (subdir) {
                struct dirent *subde;
                while ((subde = readdir(subdir)) != NULL) {
                    if (strcmp(subde->d_name, ".") != 0 && strcmp(subde->d_name, "..") != 0) {
                        char subfull[360]; snprintf(subfull, sizeof(subfull), "%s/%s", full, subde->d_name);
                        struct stat subst = {0};
                        if (stat(subfull, &subst) == 0 && S_ISDIR(subst.st_mode)) {
                            has_subdirs = true;
                            break;
                        }
                    }
                }
                closedir(subdir);
            }
            cJSON_AddBoolToObject(e, "has_subdirs", has_subdirs);
        }
        cJSON_AddItemToArray(arr, e);
    }
    closedir(dir);

    // Second pass (UX lock-flag only): inside a bundled-game folder, stream its ".factory"
    // manifest ONCE and flag each listed game + the manifest itself as protected. One SD read
    // per listing, no heap — the per-entry alternative would re-open .factory for every ROM in
    // a 200+ file folder. Enforcement is authoritative server-side in delete/move regardless.
    if (nucleo_fs_factory_scope(abs)) {
        char fpath[300]; snprintf(fpath, sizeof(fpath), "%s/.factory", abs);
        FILE *ff = fopen(fpath, "r");
        if (ff) {
            char line[160];
            while (fgets(line, sizeof(line), ff)) {
                cJSON *it;
                cJSON_ArrayForEach(it, arr) {
                    cJSON *nm = cJSON_GetObjectItem(it, "name");
                    if (nm && nm->valuestring && nucleo_fs_factory_line_eq(line, nm->valuestring) &&
                        !cJSON_GetObjectItem(it, "protected")) {
                        cJSON_AddBoolToObject(it, "protected", true);
                        break;
                    }
                }
            }
            fclose(ff);
            cJSON *it;   // pin the ".factory" entry itself (never a manifest line, handle apart)
            cJSON_ArrayForEach(it, arr) {
                cJSON *nm = cJSON_GetObjectItem(it, "name");
                if (nm && nm->valuestring && !strcasecmp(nm->valuestring, ".factory") &&
                    !cJSON_GetObjectItem(it, "protected")) {
                    cJSON_AddBoolToObject(it, "protected", true);
                    break;
                }
            }
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

static esp_err_t read_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char abs[256];
    if (!resolve_path(req, abs, sizeof(abs))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path"); return ESP_FAIL; }
    FILE *f = fopen(abs, "rb");
    if (!f) {
        // Optional system-config files (clipboard, session, ui-state, settings…) do not exist
        // on a freshly prepared SD card. Return an empty JSON object so the browser receives
        // a 200 with parseable content instead of a 404 that pollutes the DevTools console.
        // Scope is intentionally narrow: only /system/config/*.json gets this treatment.
        // All other missing paths (user data, media, app assets) still return 404 as expected.
        const char *cfg_prefix = NUCLEO_SD_MOUNT "/system/config/";
        size_t cfg_len = strlen(cfg_prefix);
        if (strncmp(abs, cfg_prefix, cfg_len) == 0) {
            const char *dot = strrchr(abs, '.');
            if (dot && strcmp(dot, ".json") == 0) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                httpd_resp_set_hdr(req, "Cache-Control", "no-store");
                httpd_resp_sendstr(req, "{}");
                return ESP_OK;
            }
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no file");
        return ESP_OK;   // complete 404 sent (benign optional-asset read) — ESP_OK avoids IDF's WARN + socket teardown
    }

    // Calcoliamo la dimensione del file
    fseek(f, 0, SEEK_END);
    long long total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *ct = content_type(abs);
    httpd_resp_set_type(req, ct);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    
    // Caching agressivo (7 giorni) per i media, evita di scaricare mega di wallpaper/icone ogni volta.
    // I file di testo e JSON (live data, settings) restano rigorosamente no-store.
    if (strncmp(ct, "image/", 6) == 0 || strncmp(ct, "audio/", 6) == 0 || strncmp(ct, "video/", 6) == 0) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }

    if (total_size == 0) {
        // Empty file: browsers' <audio>/<video> always send "Range: bytes=0-", which would hit the
        // start>=total_size guard below and return 416 (a noisy console error). There is nothing to
        // range over, so serve a clean empty 200 instead — friendlier and harmless.
        fclose(f);
        return httpd_resp_send(req, NULL, 0);
    }

    long long start = 0;
    long long end = total_size - 1;
    bool is_range = false;
    char range_hdr[64];
    // FUNCTION scope on purpose: esp_http_server's httpd_resp_set_hdr() stores the value by
    // POINTER (no copy), and the headers are only serialised on the FIRST send_chunk far below —
    // after buf[] has been filled with file bytes. A block-scoped buffer here would have its
    // stack slot reused by buf[], so Content-Range ended up holding the file's first bytes
    // ("ID3…" for mp3, NUL for mp4). Browsers reject a 206 with a malformed Content-Range, so
    // every <audio>/<video> Range request failed and media would not play. Keep this alive.
    char content_range_hdr[64];

    // Parsing dell'header Range (es. bytes=1000-2000 o bytes=1000-)
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        long long r_start = 0, r_end = -1;
        int parsed = sscanf(range_hdr, "bytes=%lld-%lld", &r_start, &r_end);
        if (parsed >= 1) {
            is_range = true;
            start = r_start;
            if (parsed == 2 && r_end != -1) {
                end = r_end;
            }
        }
    }

    if (is_range) {
        if (start < 0 || start >= total_size) {
            // Se start è fuori dai limiti, rispondiamo con 416 Range Not Satisfiable
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            snprintf(content_range_hdr, sizeof(content_range_hdr), "bytes */%lld", total_size);
            httpd_resp_set_hdr(req, "Content-Range", content_range_hdr);
            fclose(f);
            return httpd_resp_send_chunk(req, NULL, 0);
        }
        if (end < start || end >= total_size) {
            end = total_size - 1;
        }
        
        snprintf(content_range_hdr, sizeof(content_range_hdr), "bytes %lld-%lld/%lld", start, end, total_size);
        httpd_resp_set_hdr(req, "Content-Range", content_range_hdr);
        httpd_resp_set_status(req, "206 Partial Content");
        fseek(f, start, SEEK_SET);
    }

    long long remaining = is_range ? (end - start + 1) : total_size;
    // 4 KB read/send blocks: media files (audio/video) stream with 4x fewer fread/send_chunk cycles
    // than the old 1 KB buffer, and SD reads are faster in larger blocks. Safe on the 16 KB httpd
    // task stack — a plain file read uses a shallow call chain (ANIMA's L0/L1 cascade, far heavier,
    // already runs in this same task).
    char buf[4096];
    while (remaining > 0) {
        size_t to_read = (remaining < sizeof(buf)) ? (size_t)remaining : sizeof(buf);
        size_t r = fread(buf, 1, to_read, f);
        if (r <= 0) break;
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
        remaining -= r;
    }
    
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t write_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char abs[256];
    if (!resolve_path(req, abs, sizeof(abs))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path"); return ESP_FAIL; }

    // Reject oversize bodies before opening anything. content_len is the declared
    // Content-Length (browsers always set it for a File upload); a chunked body with
    // no length reports 0 here and is policed by the running-total check in the loop.
    size_t declared = req->content_len;
    if (declared > NUCLEO_MAX_UPLOAD_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file too large (max 640 MB)");
        return ESP_FAIL;
    }
    // Refuse if the card plainly can't hold it (the .tmp lives alongside the original
    // until the swap, so we need room for the incoming bytes regardless of overwrite).
    nucleo_storage_refresh();
    const nucleo_storage_info_t *st = nucleo_storage_info();
    if (declared && st->mounted && declared > st->free_bytes) {
        // No dedicated 507 in esp_http_server; 500 + message is what the client surfaces.
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not enough free space on SD");
        return ESP_FAIL;
    }

    char tmp[272]; snprintf(tmp, sizeof(tmp), "%s.tmp", abs);   // atomic: write temp, then rename
    // The receive buffer wants NUCLEO_WRITE_CHUNK (16 KB), but right after boot the heap is
    // fragmented (launcher canvas + Wi-Fi pools) and a 16 KB contiguous block may not exist
    // even with ~40 KB free — which used to fail big media uploads with a bare "oom". Fall
    // back to progressively smaller chunks so the upload still goes through, just slower.
    size_t chunk = NUCLEO_WRITE_CHUNK;
    char *buf = NULL;
    while (chunk >= 2048 && !(buf = malloc(chunk))) chunk /= 2;
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open"); return ESP_FAIL; }

    // A still-open but quiet socket yields HTTPD_SOCK_ERR_TIMEOUT every recv_wait_timeout
    // seconds (raised to 30 s in nucleo_httpd). We forgive these so a slow link or an SD
    // flush stall never kills a multi-hundred-MB upload, but cap consecutive silence so a
    // vanished client (no TCP FIN) can't pin the handler forever (~30 s * 40 ≈ 20 min).
    const int MAX_IDLE = 40;
    int r, idle = 0; uint64_t total = 0; esp_err_t fail = ESP_OK;
    while ((r = httpd_req_recv(req, buf, chunk)) != 0) {
        if (r < 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++idle <= MAX_IDLE) continue;
            fail = ESP_FAIL; break;
        }
        idle = 0;
        total += (uint64_t)r;
        if (total > NUCLEO_MAX_UPLOAD_BYTES) { fail = ESP_FAIL; break; }   // guards chunked/lying clients
        if (fwrite(buf, 1, r, f) != (size_t)r) { fail = ESP_FAIL; break; } // SD full / write error
        vTaskDelay(pdMS_TO_TICKS(1));   // Yield to lwIP/WiFi to prevent TCP starvation and ERR_CONNECTION_RESET
    }
    free(buf);
    if (fclose(f) != 0) fail = ESP_FAIL;

    // FATFS rename() does NOT overwrite an existing file (fails with EEXIST), which broke
    // every re-save (ui-state/session/settings -> HTTP 500). Drop the old file first, then
    // swap in the freshly-written temp. Write stays atomic-ish: temp is complete before swap.
    if (fail != ESP_OK) { remove(tmp); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); return ESP_FAIL; }
    remove(abs);
    if (rename(tmp, abs) != 0) { remove(tmp); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename"); return ESP_FAIL; }
    publish_change("write", abs);
    // Installing an app over the air rewrites the registry index. Reload it in place so
    // /api/apps reflects the new app immediately (no reboot), and nudge clients to refetch
    // their launcher. Cheap and only fires for this one path.
    if (strstr(abs, "/system/registry/apps.json")) {
        nucleo_registry_load();
        nucleo_event_publish("apps.changed", "{}");
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char out[64]; snprintf(out, sizeof(out), "{\"ok\":true,\"bytes\":%llu}", total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t delete_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char abs[256];
    if (!resolve_path(req, abs, sizeof(abs))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path"); return ESP_FAIL; }
    // System files (OS/shell/apps + ANIMA's offline brain) cannot be deleted by anyone —
    // file manager, ANIMA, JS runtime, any app. See nucleo_fsprotect.h. Bundled factory games
    // (DOS/ROMs listed in their folder's .factory) are pinned the same way; user imports there
    // are absent from the list and stay deletable. See nucleo_fsfactory.h.
    if (nucleo_fs_is_protected(abs) || nucleo_fs_is_factory(abs)) {
        ESP_LOGW(TAG, "blocked delete of protected system path: %s", abs);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "protected system file");
        return ESP_FAIL;
    }
    if (remove(abs) != 0 && rmdir(abs) != 0) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no entry"); return ESP_FAIL; }
    publish_change("delete", abs);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t mkdir_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char abs[256];
    if (!resolve_path(req, abs, sizeof(abs))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path"); return ESP_FAIL; }
    bool created = true;
    if (mkdir(abs, 0775) != 0) {
        if (errno != EEXIST) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir");
            return ESP_FAIL;
        }
        created = false;   // already existed: nothing changed on disk
    }
    // Publish fs.changed ONLY when a directory was actually created. The Game Center lobby
    // mkdir's its room dir on every 4 s tick; firing fs.changed on each EEXIST made every client's
    // shell re-crawl /data and re-poll /api/status for a no-op — a self-inflicted request storm.
    if (created) publish_change("mkdir", abs);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Move/rename within the SD. POST /api/fs/move?from=<path>&to=<path>[&overwrite=1].
// Atomic where the platform allows (rename(2) on the same FATFS volume). Refuses to
// clobber an existing destination unless overwrite=1 (mirrors create_file's never-
// surprise-overwrite stance). The caller (fsclient) is expected to have created the
// destination's parent directory; we add a best-effort single-level mkdir as a net.
static esp_err_t move_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    char from[256], to[256];
    if (!resolve_key(req, "from", from, sizeof(from)) || !resolve_key(req, "to", to, sizeof(to))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "from/to"); return ESP_FAIL;
    }
    // Refuse to move/rename a protected system file AWAY from where the OS expects it, and
    // refuse to clobber a protected destination. (Renaming a system file is as destructive
    // as deleting it.) In-place content updates still go through /api/fs/write, which is
    // intentionally allowed so OTA installs and config re-saves keep working.
    if (nucleo_fs_is_protected(from) || nucleo_fs_is_protected(to) ||
        nucleo_fs_is_factory(from)  || nucleo_fs_is_factory(to)) {
        ESP_LOGW(TAG, "blocked move touching protected system path: %s -> %s", from, to);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "protected system file");
        return ESP_FAIL;
    }
    // Best-effort: ensure the destination's immediate parent exists.
    char dir[256]; snprintf(dir, sizeof(dir), "%s", to);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }

    // Overwrite policy: refuse an existing destination unless explicitly allowed.
    char q[512], ov[4] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "overwrite", ov, sizeof(ov));
    struct stat st = {0};
    if (stat(to, &st) == 0) {
        if (ov[0] != '1') { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "destination exists"); return ESP_FAIL; }
        remove(to);   // FATFS rename won't overwrite; clear the way first
    }
    if (rename(from, to) != 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename"); return ESP_FAIL; }
    publish_change("move", to);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t nucleo_fsapi_register(httpd_handle_t server)
{
    httpd_uri_t routes[] = {
        { .uri = "/api/fs/list",   .method = HTTP_GET,  .handler = list_get },
        { .uri = "/api/fs/read",   .method = HTTP_GET,  .handler = read_get },
        { .uri = "/api/fs/write",  .method = HTTP_POST, .handler = write_post },
        { .uri = "/api/fs/delete", .method = HTTP_POST, .handler = delete_post },
        { .uri = "/api/fs/mkdir",  .method = HTTP_POST, .handler = mkdir_post },
        { .uri = "/api/fs/move",   .method = HTTP_POST, .handler = move_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);
    ESP_LOGI(TAG, "file API ready: /api/fs/{list,read,write,delete,mkdir,move}");
    return ESP_OK;
}
