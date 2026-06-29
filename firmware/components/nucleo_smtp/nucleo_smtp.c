// nucleo_smtp — SMTP-over-TLS sender. See nucleo_smtp.h.
#include "nucleo_smtp.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nucleo_arb.h"
#include "smtp_proto.h"   // pure, host-tested: reply parsing, base64, MIME/dot-stuffing

static const char *TAG = "smtp";

// Heap floor before attempting a TLS handshake. With MBEDTLS_SSL_VARIABLE_BUFFER
// the handshake still needs a sizeable contiguous block.
#define SMTP_MIN_BLOCK (12 * 1024)
#define SMTP_MIN_FREE  (30 * 1024)
#define SMTP_IO_TIMEOUT_US (10 * 1000 * 1000)

static void fail(char *err, size_t n, const char *msg) {
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = 0; }
}

// Read one full SMTP reply (handles multi-line). Returns the numeric code or -1.
static int smtp_resp(esp_tls_t *tls, char *buf, size_t buflen) {
    size_t off = 0;
    int64_t deadline = esp_timer_get_time() + SMTP_IO_TIMEOUT_US;
    while (off < buflen - 1) {
        int r = esp_tls_conn_read(tls, buf + off, buflen - 1 - off);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE || r == 0) {
            if (esp_timer_get_time() > deadline) return -1;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (r < 0) return -1;
        off += r;
        buf[off] = 0;
        int code = smtp_resp_code(buf, off);   // pure parser (host-tested)
        if (code >= 0) return code;
    }
    return -1;
}

static bool smtp_write(esp_tls_t *tls, const char *data, size_t len) {
    size_t sent = 0;
    int64_t deadline = esp_timer_get_time() + SMTP_IO_TIMEOUT_US;
    while (sent < len) {
        int r = esp_tls_conn_write(tls, data + sent, len - sent);
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE || r == 0) {
            if (esp_timer_get_time() > deadline) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (r < 0) return false;
        sent += r;
    }
    return true;
}

static bool smtp_puts(esp_tls_t *tls, const char *s) {
    return smtp_write(tls, s, strlen(s));
}

// Sink adapter so smtp_write_message() (pure) can stream straight to the TLS socket.
static void tls_sink(void *ctx, const char *d, size_t n) { smtp_write((esp_tls_t *)ctx, d, n); }

esp_err_t nucleo_smtp_send(const smtp_account_t *acc, const smtp_msg_t *msg,
                           char *err, size_t errlen) {
    if (!acc || !msg || !msg->to || !msg->body) { fail(err, errlen, "bad args"); return ESP_ERR_INVALID_ARG; }
    if (acc->tls != SMTP_TLS_IMPLICIT) { fail(err, errlen, "only implicit TLS (465) supported"); return ESP_ERR_NOT_SUPPORTED; }

    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < SMTP_MIN_BLOCK ||
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < SMTP_MIN_FREE) {
        fail(err, errlen, "low memory");
        return ESP_ERR_NO_MEM;
    }

    // Serialize against other outbound TLS work. Foreground (user-initiated).
    uint32_t tk = nucleo_arb_acquire(ARB_FG, "smtp-send", 4000);
    if (tk == 0) { fail(err, errlen, "device busy"); return ESP_ERR_INVALID_STATE; }

    esp_err_t ret = ESP_FAIL;
    char line[640];
    char b64buf[256];
    esp_tls_t *tls = esp_tls_init();
    if (!tls) { fail(err, errlen, "tls init"); nucleo_arb_release(tk); return ESP_ERR_NO_MEM; }

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    if (esp_tls_conn_new_sync(acc->host, strlen(acc->host), acc->port, &cfg, tls) != 1) {
        fail(err, errlen, "connect/TLS failed");
        goto done;
    }

    #define EXPECT(code, what) do { \
        int c = smtp_resp(tls, line, sizeof line); \
        if (c != (code)) { snprintf(line, sizeof line, "%s (got %d)", what, c); \
            fail(err, errlen, line); goto done; } } while (0)

    EXPECT(220, "no greeting");

    smtp_puts(tls, "EHLO nucleo\r\n");
    EXPECT(250, "EHLO refused");

    if (acc->user[0]) {                                   // AUTH LOGIN — skipped for no-auth relays
        smtp_puts(tls, "AUTH LOGIN\r\n");
        EXPECT(334, "AUTH not offered");

        if (!smtp_b64((const unsigned char *)acc->user, strlen(acc->user), b64buf, sizeof b64buf)) { fail(err, errlen, "user too long"); goto done; }
        smtp_puts(tls, b64buf); smtp_puts(tls, "\r\n");
        EXPECT(334, "user rejected");

        if (!smtp_b64((const unsigned char *)acc->pass, strlen(acc->pass), b64buf, sizeof b64buf)) { fail(err, errlen, "pass too long"); goto done; }
        smtp_puts(tls, b64buf); smtp_puts(tls, "\r\n");
        EXPECT(235, "auth failed (check app password)");
    }

    snprintf(line, sizeof line, "MAIL FROM:<%s>\r\n", acc->from[0] ? acc->from : acc->user);
    smtp_puts(tls, line);
    EXPECT(250, "MAIL FROM rejected");

    snprintf(line, sizeof line, "RCPT TO:<%s>\r\n", msg->to);
    smtp_puts(tls, line);
    { int c = smtp_resp(tls, line, sizeof line);
      if (c != 250 && c != 251) { snprintf(line, sizeof line, "recipient rejected (%d)", c); fail(err, errlen, line); goto done; } }

    smtp_puts(tls, "DATA\r\n");
    EXPECT(354, "DATA refused");

    // headers + dot-stuffed body via the pure (host-tested) writer, streamed to the TLS socket.
    {
        time_t now = time(NULL); struct tm tmv; gmtime_r(&now, &tmv);
        char date[48]; strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S +0000", &tmv);
        smtp_write_message(tls_sink, tls, acc->from[0] ? acc->from : acc->user, acc->from_name,
                           msg->to, msg->subject ? msg->subject : "", msg->body, date);
    }
    EXPECT(250, "message rejected");

    smtp_puts(tls, "QUIT\r\n");
    ret = ESP_OK;
    if (err && errlen) err[0] = 0;
    #undef EXPECT

done:
    esp_tls_conn_destroy(tls);
    nucleo_arb_release(tk);
    if (ret == ESP_OK) ESP_LOGI(TAG, "sent to %s via %s", msg->to, acc->host);
    else ESP_LOGW(TAG, "send failed: %s", (err && err[0]) ? err : "?");
    return ret;
}
