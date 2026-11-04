// nucleo_tcp.c / nucleo_secret — the network and the secrets an app may have (nucleo_tcp.h, nucleo_secret.h).
// Every wait is cut in 100 ms slices that feed the task watchdog, so an app's UI task can call them.
#include "nucleo_tcp.h"
#include "nucleo_secret.h"
#include "nucleo_app.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "tcp";

// store_apps.cpp: may the foreground app use the network (built-in, or a store app whose record grants "net")?
extern bool nucleo_app_net_allowed(void);

static int s_fd[NUCLEO_TCP_MAX] = { -1, -1 };

bool nucleo_tcp_network_up(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

static int fd_of(int h) { return (h >= 1 && h <= NUCLEO_TCP_MAX) ? s_fd[h - 1] : -1; }

// Only ordinary unicast addresses that are not this device: no loopback, no 0.x, no multicast or broadcast.
static bool address_allowed(uint32_t a /* network order */)
{
    const uint32_t h = ntohl(a);
    if ((h >> 24) == 127 || (h >> 24) == 0 || h >= 0xE0000000u) return false;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) {
        if (a == ip.ip.addr) return false;
        if (ip.netmask.addr && (a | ip.netmask.addr) == 0xFFFFFFFFu) return false;   // the subnet's broadcast
    }
    return true;
}

// Every call feeds the caller's task watchdog (the app loop is subscribed, 8 s): a session that only ever waits in
// short slices, or that receives without waiting, would otherwise starve it. Unsubscribed callers are left alone.
static void wdt(void) { if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); }

int nucleo_tcp_connect(const char *host, int port, int timeout_ms)
{
    if (!nucleo_app_net_allowed()) { ESP_LOGW(TAG, "%s: no network permission", nucleo_app_active_id() ? nucleo_app_active_id() : "?"); return NUCLEO_TCP_DENIED; }
    if (!host || !host[0] || port <= 0 || port > 65535) return NUCLEO_TCP_DENIED;
    if (!nucleo_tcp_network_up()) return NUCLEO_TCP_NONET;
    int slot = -1;
    for (int i = 0; i < NUCLEO_TCP_MAX; i++) if (s_fd[i] < 0) { slot = i; break; }
    if (slot < 0) return NUCLEO_TCP_FULL;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return NUCLEO_TCP_NOHOST;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)res->ai_addr;
    if (!address_allowed(sin->sin_addr.s_addr)) { freeaddrinfo(res); return NUCLEO_TCP_DENIED; }
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { freeaddrinfo(res); return NUCLEO_TCP_ERROR; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno == EINPROGRESS) {
        rc = -1;
        for (int waited = 0; waited < (timeout_ms > 0 ? timeout_ms : 1); waited += 100) {
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            struct timeval tv = { 0, 100 * 1000 };
            wdt();
            if (select(s + 1, NULL, &w, NULL, &tv) > 0) {
                int err = 0; socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = err == 0 ? 0 : -1;
                break;
            }
        }
    }
    if (rc != 0) { close(s); return NUCLEO_TCP_ERROR; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    s_fd[slot] = s;
    return slot + 1;
}

bool nucleo_tcp_readable(int h, int timeout_ms)
{
    const int s = fd_of(h);
    if (s < 0) return false;
    int left = timeout_ms < 0 ? 0 : timeout_ms;
    do {
        const int slice = left > 100 ? 100 : left;
        fd_set r; FD_ZERO(&r); FD_SET(s, &r);
        struct timeval tv = { 0, slice * 1000 };
        wdt();
        if (select(s + 1, &r, NULL, NULL, &tv) > 0) return true;
        left -= slice;
    } while (left > 0);
    return false;
}

int nucleo_tcp_recv(int h, void *buf, int cap, int timeout_ms)
{
    const int s = fd_of(h);
    if (s < 0 || !buf || cap <= 0) return NUCLEO_TCP_ERROR;
    if (!nucleo_tcp_readable(h, timeout_ms)) return NUCLEO_TCP_TIMEOUT;
    const int n = recv(s, buf, cap, 0);
    if (n > 0) return n;
    if (n == 0) return NUCLEO_TCP_CLOSED;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? NUCLEO_TCP_TIMEOUT : NUCLEO_TCP_ERROR;
}

int nucleo_tcp_send(int h, const void *buf, int len, int timeout_ms)
{
    const int s = fd_of(h);
    if (s < 0 || (!buf && len > 0) || len < 0) return NUCLEO_TCP_ERROR;
    const char *p = (const char *)buf;
    int left = len;
    wdt();
    const int64_t until = esp_timer_get_time() + (int64_t)(timeout_ms > 0 ? timeout_ms : 1) * 1000;
    while (left > 0) {
        const int w = send(s, p, left, 0);
        if (w > 0) { p += w; left -= w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return NUCLEO_TCP_ERROR;
        if (esp_timer_get_time() > until) return NUCLEO_TCP_TIMEOUT;
        fd_set wr; FD_ZERO(&wr); FD_SET(s, &wr);
        struct timeval tv = { 0, 100 * 1000 };
        wdt();
        select(s + 1, NULL, &wr, NULL, &tv);
    }
    return len;
}

void nucleo_tcp_close(int h)
{
    const int s = fd_of(h);
    if (s < 0) return;
    shutdown(s, SHUT_RDWR);
    close(s);
    s_fd[h - 1] = -1;
}

void nucleo_tcp_close_all(void)
{
    for (int i = 1; i <= NUCLEO_TCP_MAX; i++) nucleo_tcp_close(i);
}

// ── secrets: NVS namespace "s.<app id>" (NVS names are 15 characters at most: a long id is folded) ──
static bool secret_ns(char ns[16])
{
    const char *id = nucleo_app_active_id();
    if (!id || !id[0]) return false;
    if (strlen(id) <= 13) { snprintf(ns, 16, "s.%s", id); return true; }
    uint32_t h = 2166136261u;                         // FNV-1a of the whole id
    for (const char *c = id; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    snprintf(ns, 16, "s.%.4s%08lx", id, (unsigned long)h);
    return true;
}

bool nucleo_secret_set(const char *name, const char *value)
{
    char ns[16];
    if (!name || !name[0] || strlen(name) > 15 || !secret_ns(ns)) return false;
    if (value && strlen(value) >= NUCLEO_SECRET_MAX) return false;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = (value && value[0]) ? nvs_set_str(h, name, value) : nvs_erase_key(h, name);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool nucleo_secret_get(const char *name, char *out, int cap)
{
    if (!out || cap <= 0) return false;
    out[0] = 0;
    char ns[16];
    if (!name || !name[0] || strlen(name) > 15 || !secret_ns(ns)) return false;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = (size_t)cap;
    const esp_err_t e = nvs_get_str(h, name, out, &len);
    nvs_close(h);
    if (e != ESP_OK) { out[0] = 0; return false; }
    return true;
}
