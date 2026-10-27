// Host stub for app_keydeck.cpp: fake a CONNECTED NucleoV2 peer streaming telemetry, so the large
// Monitor layout renders a real online session instead of "offline". The shim only DECLARES the BSD
// socket / NVS calls (they would otherwise auto-stub to 0); here we define just enough of them to walk
// the app's own state machine to CS_ONLINE and hand it one WELCOME + STAT line. Fictional data only.
#include "nvs.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

extern "C" {

// --- NVS: pretend a server IP was saved earlier (so on_enter connects instead of browsing mDNS) ---
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *h) { if (h) *h = 1; return 0; }
void      nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return 0; }
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *) { return 0; }
esp_err_t nvs_get_str(nvs_handle_t, const char *k, char *v, size_t *n)
{
    if (!v || !n) return 0;
    const char *val = (k && !strcmp(k, "ip")) ? "192.168.1.42" : "";   // saved IP; no PIN (open mode)
    snprintf(v, *n, "%s", val);
    return 0;
}

// --- BSD sockets: a single always-ready connection to the fake peer ---
int socket(int, int, int) { return 3; }
int fcntl(int, int, ...) { return 0; }
int inet_aton(const char *, struct in_addr *inp) { if (inp) inp->s_addr = 0x2A01A8C0; return 1; }
int connect(int, const struct sockaddr *, socklen_t) { return -1; }   // EINPROGRESS path
int setsockopt(int, int, int, const void *, socklen_t) { return 0; }
int getsockopt(int, int, int o, void *v, socklen_t *)
{
    if (o == SO_ERROR && v) *(int *)v = 0;   // the non-blocking connect "succeeded"
    return 0;
}
int select(int, fd_set *, fd_set *w, fd_set *, struct timeval *)
{
    (void)w;   // the write set already has our fd; leave it set and report it ready
    return 1;
}
int send(int, const void *, size_t n, int) { return (int)n; }
int recv(int, void *b, size_t n, int)
{
    // One WELCOME + STAT, streamed in <=n-byte chunks (the app's recv buffer is 96 B and the STAT line is
    // longer, so a single truncated delivery would lose the trailing '\n' and never complete the line).
    static const char msg[] =
        "WELCOME v1 NucleoOS nucleo-v2\n"
        "STAT ps_free=19922944 ps_total=33554432 sram_free=204800 cpu0=23 cpu1=41 up=4521\n";
    static size_t off = 0;
    const size_t total = sizeof(msg) - 1;
    if (b && off < total) {
        size_t len = total - off;
        if (len > n) len = n;
        memcpy(b, msg + off, len);
        off += len;
        return (int)len;
    }
    errno = EWOULDBLOCK;   // nothing more to read; not a dropped link
    return -1;
}

}  // extern "C"
