// nucleo_tcp — the only network an app may have: TCP client connections the system opens for it (plain, or
// TLS with a verified certificate from ABI 10).
//
// No listening, no UDP, no raw sockets, no scans: an app connects to one address the user gave it and exchanges
// bytes. Every call is bounded in time. A store app gets it only with the "net" permission in its signed record
// (the Store shows it before installing); built-in apps always may. Loopback, broadcast, multicast and the
// Cardputer's own address are refused, so an app cannot reach the device's own services. At most
// NUCLEO_TCP_MAX connections per app; the framework closes the ones left open when the app closes.
// docs/native-apps.md ("Network").
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_TCP_MAX 2
#define NUCLEO_TCP_TLS_MAX 1   // of which TLS (ABI 10): one session fits the heap of a Solo boot

enum {
    NUCLEO_TCP_TIMEOUT = -1,   // nothing within the time given (recv/send); the connection is still open
    NUCLEO_TCP_CLOSED  = -2,   // the other side closed it
    NUCLEO_TCP_ERROR   = -3,   // the connection broke (reset, unreachable, bad handle)
    NUCLEO_TCP_DENIED  = -4,   // this app has no network permission, or the address is not allowed
    NUCLEO_TCP_NOHOST  = -5,   // the name did not resolve
    NUCLEO_TCP_NONET   = -6,   // no Wi-Fi
    NUCLEO_TCP_FULL    = -7,   // NUCLEO_TCP_MAX connections (or NUCLEO_TCP_TLS_MAX TLS sessions) already open
    NUCLEO_TCP_TLS     = -8,   // the TLS handshake failed: a certificate the bundle does not trust, a wrong name...
    NUCLEO_TCP_NOMEM   = -9,   // no heap for a TLS session
};

bool nucleo_tcp_network_up(void);                                         // Wi-Fi joined with an address
int  nucleo_tcp_connect(const char *host, int port, int timeout_ms);       // a handle (> 0) or an error (< 0)
// ABI 10: the same connection over TLS. The certificate is always verified (CA bundle + host name); send, recv,
// readable and close then work on it unchanged. A handshake takes a second or two.
int  nucleo_tcp_connect_tls(const char *host, int port, int timeout_ms);
int  nucleo_tcp_send(int h, const void *buf, int len, int timeout_ms);     // len when all of it went, or < 0
int  nucleo_tcp_recv(int h, void *buf, int cap, int timeout_ms);           // bytes (> 0), TIMEOUT, CLOSED, ERROR
bool nucleo_tcp_readable(int h, int timeout_ms);                           // data waiting (or the end) within the time
void nucleo_tcp_close(int h);
void nucleo_tcp_close_all(void);                                           // the framework, when an app closes

#ifdef __cplusplus
}
#endif
