// App store client — device glue. See nucleo_store.h.
#include "nucleo_store.h"
#include "store_verify.h"
#include "nucleo_sdcontent.h"
#include "nucleo_board.h"
#include "nucleo_storage.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const char *nucleo_i18n_lang(void) __attribute__((weak));   // nucleo_storage (resolved at link)
// nucleo_app/native_apps.cpp: the native ABI level this firmware runs (weak: a build without it has none).
extern uint32_t nucleo_native_abi_level(void) __attribute__((weak));

static const char *TAG = "store";

#define WINDOW        6144u     // == the SD-content installer: under the 8 KB TLS record cap (sdc_engine.h)
#define TLS_MIN_BLOCK 22000u    // contiguous heap the TLS handshake needs (nucleo_sdcontent.c)
#define TASK_STACK    6144

static volatile nst_status_t s_status = NST_IDLE;
static nst_op_t s_op;
static char s_id[ST_ID_MAX + 1];
static char s_channel[8] = ST_CH_NATIVE;
static st_state_t s_st;
static char s_err[72];

nst_status_t nucleo_store_status(void) { return s_status; }
nst_op_t nucleo_store_op(void) { return s_op; }
const st_state_t *nucleo_store_state(void) { return &s_st; }
const char *nucleo_store_error(void) { return s_status == NST_FAILED ? s_err : ""; }
const char *nucleo_store_channel(void) { return s_channel; }
uint32_t nucleo_store_native_abi(void) { return nucleo_native_abi_level ? nucleo_native_abi_level() : 0; }

const char *nucleo_store_os_version(void)
{
    static char v[16];
    if (!v[0]) {
        const char *full = esp_app_get_description()->version;
        size_t n = strcspn(full, "+- ");
        snprintf(v, sizeof v, "%.*s", (int)(n < sizeof v ? n : sizeof v - 1), full);
    }
    return v;
}

static const char *L5(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang ? nucleo_i18n_lang() : 0;
    if (!l) return en;
    if (l[0] == 'i' && l[1] == 't') return it;
    if (l[0] == 'e' && l[1] == 's') return es;
    if (l[0] == 'f' && l[1] == 'r') return fr;
    if (l[0] == 'd' && l[1] == 'e') return de;
    return en;
}

static void localize(st_code_t c, int http)
{
    const char *m;
    switch (c) {
    case ST_E_NO_SD:       m = L5("Nessuna scheda SD", "No SD card", "Sin tarjeta SD", "Pas de carte SD", "Keine SD-Karte"); break;
    case ST_E_NET:         m = L5("Store non raggiungibile", "Store unreachable", "Tienda inaccesible", "Boutique injoignable", "Store nicht erreichbar"); break;
    case ST_E_SIG:         m = L5("Firma non valida: rifiutato", "Invalid signature: refused", "Firma invalida: rechazado", "Signature invalide : refuse", "Signatur ungueltig: abgelehnt"); break;
    case ST_E_ROLLBACK:    m = L5("Catalogo vecchio: rifiutato", "Old catalog: refused", "Catalogo antiguo: rechazado", "Catalogue ancien : refuse", "Alter Katalog: abgelehnt"); break;
    case ST_E_EXPIRED:     m = L5("Catalogo scaduto: riprova piu' tardi", "Catalog expired: retry later", "Catalogo caducado: reintenta", "Catalogue expire : reessayez", "Katalog abgelaufen: spaeter"); break;
    case ST_E_BAD:         m = L5("Dati dello store non validi", "Invalid store data", "Datos de tienda invalidos", "Donnees invalides", "Ungueltige Store-Daten"); break;
    case ST_E_VERIFY:      m = L5("File corrotto: rifiutato", "Corrupted file: refused", "Archivo corrupto: rechazado", "Fichier corrompu : refuse", "Datei beschaedigt: abgelehnt"); break;
    case ST_E_WRITE:       m = L5("Scrittura SD fallita (piena?)", "SD write failed (full?)", "Error de SD (llena?)", "Ecriture SD impossible (pleine ?)", "SD-Schreibfehler (voll?)"); break;
    case ST_E_NOMEM:       m = L5("RAM insufficiente: riprova", "Not enough RAM: retry", "Sin RAM: reintenta", "RAM insuffisante : reessayez", "Zu wenig RAM: erneut"); break;
    case ST_E_NOTFOUND:    m = L5("App non trovata", "App not found", "App no encontrada", "App introuvable", "App nicht gefunden"); break;
    case ST_E_CONFLICT:    m = L5("App di sistema: non la tocco", "System app: left untouched", "App del sistema: intacta", "App systeme : intacte", "System-App: unveraendert"); break;
    case ST_E_SPACE:       m = L5("Spazio SD insufficiente", "Not enough space on the SD", "Sin espacio en la SD", "Pas assez de place sur la SD", "Zu wenig Platz auf der SD"); break;
    case ST_E_ABI:         m = L5("Serve un NucleoOS piu' nuovo", "Needs a newer NucleoOS", "Requiere NucleoOS nuevo", "NucleoOS plus recent requis", "Neueres NucleoOS noetig"); break;
    case ST_E_UNSUPPORTED: m = L5("Richiede un firmware piu' nuovo", "Needs a newer firmware", "Requiere firmware nuevo", "Firmware plus recent requis", "Neuere Firmware noetig"); break;
    default:               m = L5("Operazione fallita", "Operation failed", "Operacion fallida", "Echec", "Fehlgeschlagen"); break;
    }
    if (c == ST_E_NET && http >= 400) snprintf(s_err, sizeof s_err, "%s (HTTP %d)", m, http);   // a real server answer
    else snprintf(s_err, sizeof s_err, "%s", m);
}

static bool io_verify(void *ctx, const char *key, const uint8_t hash[32], const uint8_t *sig, size_t n)
{
    (void)ctx;
    return st_verify(key, hash, sig, n);
}
static uint32_t io_now(void *ctx) { (void)ctx; return (uint32_t)time(NULL); }
static void io_tick(void *ctx) { (void)ctx; vTaskDelay(1); }
static void io_progress(void *ctx, const st_state_t *st) { (void)ctx; if (st != &s_st) s_st = *st; }

// The new version is downloaded beside the old one before the swap: twice the payload, plus a margin for
// FAT clusters and the card's own files. Unknown free space (no stats) does not block an install.
static bool sd_room(uint32_t payload)
{
    nucleo_storage_refresh();
    const nucleo_storage_info_t *i = nucleo_storage_info();
    if (!i || !i->mounted || !i->total_bytes) return true;
    uint64_t need = (uint64_t)payload * 2 + 2u * 1024 * 1024;
    return i->free_bytes >= need;
}

static void task(void *arg)
{
    (void)arg;
    st_code_t rc = ST_OK;
    uint8_t *buf = NULL;
    if (s_op != NST_OP_REMOVE) {
        // The window first (6 KB), then the handshake's contiguous block must still be there. Right after an
        // app opens, the subsystems NX_NET_APP stops may still be releasing RAM: give them a few seconds.
        size_t largest = 0;
        for (int i = 0; i < 30 && rc == ST_OK; i++) {
            if (!buf) buf = malloc(WINDOW);
            largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (buf && largest >= TLS_MIN_BLOCK) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGW(TAG, "op %d %s: free %u, largest block %u", s_op, s_id,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)largest);
        if (!buf || largest < TLS_MIN_BLOCK) rc = ST_E_NOMEM;
    }
    if (rc == ST_OK) {
        st_cfg_t cfg = { .root = NUCLEO_SD_MOUNT, .base_url = NUCLEO_STORE_URL, .channel = s_channel,
                         .native_abi = nucleo_store_native_abi(), .buf = buf, .window = WINDOW, .retries = 3 };
        st_io_t io = { .ctx = NULL, .get = nucleo_sdcontent_http_get, .verify = io_verify, .now = io_now,
                       .progress = io_progress, .tick = io_tick };
        if (s_op == NST_OP_REFRESH) rc = st_refresh(&cfg, &io, &s_st);
        else if (s_op == NST_OP_REMOVE) rc = st_remove(NUCLEO_SD_MOUNT, s_id);
        else {
            st_app_t *a = malloc(sizeof *a);
            if (!a) rc = ST_E_NOMEM;
            else if (!st_find(NUCLEO_SD_MOUNT, s_channel, s_id, a)) rc = ST_E_NOTFOUND;
            else if (st_ver_cmp(nucleo_store_os_version(), a->min_os) < 0) rc = ST_E_UNSUPPORTED;
            else if (!sd_room(a->size)) rc = ST_E_SPACE;
            else rc = st_install(&cfg, &io, &s_st, a);
            free(a);
        }
        nucleo_sdcontent_http_close();
    }
    free(buf);
    ESP_LOGW(TAG, "op %d %s: rc=%d http=%d detail=%s", s_op, s_id, rc, s_st.http, s_st.detail);
    if (rc != ST_OK) { s_st.code = rc; localize(rc, s_st.http); }
    s_status = rc == ST_OK ? NST_DONE : NST_FAILED;
    vTaskDelete(NULL);
}

bool nucleo_store_start(nst_op_t op, const char *channel, const char *id)
{
    if (s_status == NST_BUSY) return false;
    if (!channel || (strcmp(channel, ST_CH_NATIVE) && strcmp(channel, ST_CH_WEB))) return false;
    if (op != NST_OP_REFRESH && (!id || !st_id_ok(id))) return false;
    snprintf(s_channel, sizeof s_channel, "%s", channel);
    s_op = op;
    snprintf(s_id, sizeof s_id, "%s", id ? id : "");
    memset(&s_st, 0, sizeof s_st);
    s_err[0] = 0;
    s_status = NST_BUSY;
    if (xTaskCreate(task, "store", TASK_STACK, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        s_st.code = ST_E_NOMEM; localize(ST_E_NOMEM, 0);
        s_status = NST_FAILED;
        return false;
    }
    return true;
}

bool nucleo_store_check_installed(const char *id, const char *rel, uint8_t sha[32])
{
    enum { BUF = 2048 };
    uint8_t *buf = malloc(BUF);
    if (!buf) return false;
    st_io_t io = { .ctx = NULL, .verify = io_verify, .tick = io_tick };
    st_code_t rc = st_verify_installed(NUCLEO_SD_MOUNT, id, rel, &io, buf, BUF, sha);
    free(buf);
    if (rc != ST_OK) ESP_LOGE(TAG, "%s/%s does not match its signed package (rc=%d)", id, rel, rc);
    return rc == ST_OK;
}
