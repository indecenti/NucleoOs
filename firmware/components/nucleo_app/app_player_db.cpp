#include "app_player_db.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "music_db";
#define DB_FILE "/cfg/music_db.jsonl"
#define MUSIC_DIR "/sd/data/Music"

// Simple ID3v2 parser
static void extract_id3(const char *path, char *title, char *artist, char *genre) {
    title[0] = 0; artist[0] = 0; genre[0] = 0;
    FILE *f = fopen(path, "rb"); if (!f) return;
    uint8_t h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) { fclose(f); return; }
    uint32_t size = ((h[6]&0x7f)<<21) | ((h[7]&0x7f)<<14) | ((h[8]&0x7f)<<7) | (h[9]&0x7f);
    uint32_t pos = 10;
    
    while (pos < size + 10) {
        uint8_t fh[10];
        if (fread(fh, 1, 10, f) != 10) break;
        if (fh[0] == 0) break; // padding
        uint32_t fsz = (fh[4]<<24) | (fh[5]<<16) | (fh[6]<<8) | fh[7];
        // v2.4 uses synchsafe for frame sizes, v2.3 does not. We approximate v2.3 mostly.
        if (h[3] >= 4) fsz = ((fh[4]&0x7f)<<21) | ((fh[5]&0x7f)<<14) | ((fh[6]&0x7f)<<7) | (fh[7]&0x7f);
        
        pos += 10 + fsz;
        if (fsz == 0 || fsz > 1024) { fseek(f, pos, SEEK_SET); continue; }
        
        char id[5] = {fh[0], fh[1], fh[2], fh[3], 0};
        bool is_tit2 = !strcmp(id, "TIT2");
        bool is_tpe1 = !strcmp(id, "TPE1");
        bool is_tcon = !strcmp(id, "TCON");
        
        if (is_tit2 || is_tpe1 || is_tcon) {
            uint8_t *data = (uint8_t*)malloc(fsz);
            if (data && fread(data, 1, fsz, f) == fsz) {
                // skip encoding byte (usually 0=ISO-8859-1, 1=UTF-16, 3=UTF-8)
                int offset = 1;
                if (fsz > 3 && data[0] == 1 && data[1] == 0xFF && data[2] == 0xFE) offset = 3; // UTF-16 BOM
                int copy_len = fsz - offset;
                if (copy_len > MAX_META_TEXT - 1) copy_len = MAX_META_TEXT - 1;
                char *target = is_tit2 ? title : (is_tpe1 ? artist : genre);
                
                // Extremely rudimentary UTF-16 to ASCII fallback
                int t = 0;
                for (int i = 0; i < copy_len && t < MAX_META_TEXT - 1; i++) {
                    if (data[offset+i] != 0) target[t++] = data[offset+i];
                }
                target[t] = 0;
            }
            if (data) free(data);
            fseek(f, pos, SEEK_SET); // ensure aligned to next frame
        } else {
            fseek(f, pos, SEEK_SET);
        }
    }
    fclose(f);
}

static bool endswith(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    size_t len = strlen(str), slen = strlen(suffix);
    if (len < slen) return false;
    return strcasecmp(str + len - slen, suffix) == 0;
}

static void indexer_task(void *arg) {
    ESP_LOGI(TAG, "Starting background indexer");
    // Just a basic scanner for now to build JSONL if missing or add new files
    // In a full implementation, we'd compare FATFS contents with DB_FILE.
    // For now, let's just make sure it creates the file if missing.
    FILE *db = fopen(DB_FILE, "a");
    if (!db) { vTaskDelete(NULL); return; }
    
    // Simplistic: if file is 0 bytes, we scan. 
    // If not, we assume it's mostly built. (Evolution: hash/diff)
    fseek(db, 0, SEEK_END);
    if (ftell(db) == 0) {
        DIR *dir = opendir(MUSIC_DIR);
        if (dir) {
            struct dirent *e;
            while ((e = readdir(dir)) != NULL) {
                if (e->d_name[0] == '.') continue;

                char full[256];
                snprintf(full, sizeof full, "%s/%s", MUSIC_DIR, e->d_name);

                // FATFS: d_type è DT_UNKNOWN, usiamo stat() per distinguere dir/file
                struct stat fst;
                if (stat(full, &fst) != 0) continue;
                if (S_ISDIR(fst.st_mode)) continue;

                if (!endswith(e->d_name, ".mp3") && !endswith(e->d_name, ".wav")) continue;

                char t[MAX_META_TEXT]="", a[MAX_META_TEXT]="Unknown", g[MAX_META_TEXT]="Unknown";
                if (endswith(e->d_name, ".mp3")) extract_id3(full, t, a, g);
                if (!t[0]) snprintf(t, sizeof t, "%s", e->d_name);

                cJSON *j = cJSON_CreateObject();
                cJSON_AddStringToObject(j, "path", e->d_name);
                cJSON_AddStringToObject(j, "title", t);
                cJSON_AddStringToObject(j, "artist", a);
                cJSON_AddStringToObject(j, "genre", g);
                cJSON_AddNumberToObject(j, "plays", 0);
                cJSON_AddBoolToObject(j, "fav", false);

                char *line = cJSON_PrintUnformatted(j);
                fprintf(db, "%s\n", line);
                free(line);
                cJSON_Delete(j);

                vTaskDelay(pdMS_TO_TICKS(10)); // Yield CPU
            }
            closedir(dir);
        }
    }
    fclose(db);
    ESP_LOGI(TAG, "Indexer complete");
    vTaskDelete(NULL);
}

void music_db_init(void) {
    xTaskCreatePinnedToCore(indexer_task, "music_idx", 4096, NULL, 3, NULL, 0);
}

// is_play=true bumps the play count; otherwise sets "fav" to the absolute value fav_val.
static void update_jsonl_record(const char *path, bool is_play, bool fav_val) {
    FILE *f = fopen(DB_FILE, "r"); if (!f) return;
    FILE *tmp = fopen(DB_FILE ".tmp", "w");
    if (!tmp) { fclose(f); return; }

    char line[2048];   // JSONL records can exceed 512B; a truncated line corrupts the rewrite
    while(fgets(line, sizeof(line), f)) {
        cJSON *j = cJSON_Parse(line);
        if (j) {
            cJSON *p = cJSON_GetObjectItem(j, "path");
            if (p && p->valuestring && !strcmp(p->valuestring, path)) {
                if (is_play) {
                    cJSON *pl = cJSON_GetObjectItem(j, "plays");
                    int v = pl ? pl->valueint + 1 : 1;
                    if (pl) cJSON_SetNumberValue(pl, v); else cJSON_AddNumberToObject(j, "plays", v);
                } else {
                    cJSON *fv = cJSON_GetObjectItem(j, "fav");
                    if (fv) cJSON_ReplaceItemInObject(j, "fav", cJSON_CreateBool(fav_val));
                    else cJSON_AddBoolToObject(j, "fav", fav_val);
                }
            }
            char *out = cJSON_PrintUnformatted(j);
            fprintf(tmp, "%s\n", out);
            free(out);
            cJSON_Delete(j);
        } else {
            fprintf(tmp, "%s", line); // write raw if parse fails
        }
    }
    fclose(f); fclose(tmp);
    remove(DB_FILE);
    rename(DB_FILE ".tmp", DB_FILE);
}

void music_db_add_play(const char *path) { update_jsonl_record(path, true, false); }
void music_db_set_fav(const char *path, bool fav) { update_jsonl_record(path, false, fav); }

bool music_db_is_fav(const char *path) {
    FILE *f = fopen(DB_FILE, "r"); if (!f) return false;
    char line[2048]; bool ret = false;
    while(fgets(line, sizeof(line), f)) {
        if (!strstr(line, path)) continue; // fast pre-check
        cJSON *j = cJSON_Parse(line);
        if (j) {
            cJSON *p = cJSON_GetObjectItem(j, "path");
            if (p && p->valuestring && !strcmp(p->valuestring, path)) {
                cJSON *fv = cJSON_GetObjectItem(j, "fav");
                ret = cJSON_IsTrue(fv);
                cJSON_Delete(j);
                break;
            }
            cJSON_Delete(j);
        }
    }
    fclose(f);
    return ret;
}

int music_db_search(const char *query, int filter_type, struct TrackMeta **out_results) {
    // 0=All, 1=Genre, 2=Artist, 3=Favorites, 4=Most Played
    FILE *f = fopen(DB_FILE, "r"); if (!f) { *out_results = NULL; return 0; }

    // Grow in chunks (16 -> doubling) instead of one ~64 KB contiguous block, which almost
    // always fails on the fragmented runtime heap. cap = hard ceiling against runaway DBs.
    int cap = 16, count = 0;
    struct TrackMeta *res = (struct TrackMeta *)malloc(sizeof(struct TrackMeta) * cap);
    if (!res) { fclose(f); *out_results = NULL; return 0; }
    const int CAP_MAX = 2000;

    char line[2048];
    while(fgets(line, sizeof(line), f) && count < CAP_MAX) {
        if (query && query[0]) {
            // Very simple case-insensitive substring search across the raw JSON line! Fast!
            char *lower_line = strdup(line);
            for(char *p=lower_line; *p; ++p) *p = tolower(*p);
            char lower_q[64]; snprintf(lower_q, sizeof lower_q, "%s", query);
            for(char *p=lower_q; *p; ++p) *p = tolower(*p);
            if (!strstr(lower_line, lower_q)) { free(lower_line); continue; }
            free(lower_line);
        }
        
        cJSON *j = cJSON_Parse(line);
        if (!j) continue;
        
        cJSON *tit = cJSON_GetObjectItem(j, "title");
        cJSON *art = cJSON_GetObjectItem(j, "artist");
        cJSON *gen = cJSON_GetObjectItem(j, "genre");
        cJSON *pth = cJSON_GetObjectItem(j, "path");
        cJSON *pl  = cJSON_GetObjectItem(j, "plays");
        cJSON *fv  = cJSON_GetObjectItem(j, "fav");
        
        bool match = true;
        if (filter_type == 1 && query) match = (gen && gen->valuestring && !strcasecmp(gen->valuestring, query));
        if (filter_type == 2 && query) match = (art && art->valuestring && !strcasecmp(art->valuestring, query));
        if (filter_type == 3) match = cJSON_IsTrue(fv);
        
        if (match && pth && pth->valuestring) {
            if (count == cap) {
                int ncap = cap * 2; if (ncap > CAP_MAX) ncap = CAP_MAX;
                struct TrackMeta *grown = (struct TrackMeta *)realloc(res, sizeof(struct TrackMeta) * ncap);
                if (!grown) { cJSON_Delete(j); break; } // keep what we have; res still valid
                res = grown; cap = ncap;
            }
            snprintf(res[count].path, sizeof res[count].path, "%s", pth->valuestring);
            snprintf(res[count].title, sizeof res[count].title, "%s", tit ? tit->valuestring : "");
            snprintf(res[count].artist, sizeof res[count].artist, "%s", art ? art->valuestring : "");
            snprintf(res[count].genre, sizeof res[count].genre, "%s", gen ? gen->valuestring : "");
            res[count].plays = pl ? pl->valueint : 0;
            res[count].fav = cJSON_IsTrue(fv);
            count++;
        }
        cJSON_Delete(j);
    }
    fclose(f);
    
    if (filter_type == 4) { // Sort by Most Played
        for (int i=0; i<count-1; i++) {
            for (int j=0; j<count-i-1; j++) {
                if (res[j].plays < res[j+1].plays) {
                    struct TrackMeta tmp = res[j];
                    res[j] = res[j+1];
                    res[j+1] = tmp;
                }
            }
        }
    }
    
    *out_results = res;
    return count;
}

int music_db_get_unique(int type, char ***out_list) {
    // 1=Genre, 2=Artist
    FILE *f = fopen(DB_FILE, "r"); if (!f) { *out_list = NULL; return 0; }
    
    char **res = (char **)malloc(sizeof(char*) * 50);
    if (!res) { fclose(f); *out_list = NULL; return 0; }
    int count = 0;

    char line[2048];
    while(fgets(line, sizeof(line), f) && count < 50) {
        cJSON *j = cJSON_Parse(line);
        if (!j) continue;
        
        cJSON *tgt = cJSON_GetObjectItem(j, type == 1 ? "genre" : "artist");
        if (tgt && tgt->valuestring && strlen(tgt->valuestring) > 0 && strcasecmp(tgt->valuestring, "Unknown") != 0) {
            bool found = false;
            for (int i=0; i<count; i++) {
                if (!strcasecmp(res[i], tgt->valuestring)) { found = true; break; }
            }
            if (!found) res[count++] = strdup(tgt->valuestring);
        }
        cJSON_Delete(j);
    }
    fclose(f);
    *out_list = res;
    return count;
}
