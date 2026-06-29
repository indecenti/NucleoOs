// smtp_proto — pure, host-testable SMTP helpers (NO TLS, no ESP deps).
// Used by nucleo_smtp.c on the device and by tools/anima-host/smtp-ctest.c on the PC, so the
// tricky bits (multi-line reply parsing, base64, dot-stuffing, MIME, address shape) are covered
// by a host gate before they ever reach a Cardputer.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sink for the message writer — the device passes a TLS-write wrapper, the test a buffer appender.
typedef void (*smtp_sink_fn)(void *ctx, const char *data, size_t len);

// Final numeric code of a (possibly multi-line) SMTP reply in buf[0..len), or -1 if the reply is
// not yet complete (last line not terminated, or still a "250-" continuation) or malformed.
int smtp_resp_code(const char *buf, size_t len);

// 1 if `a` has a plausible user@host.tld shape (no spaces, '@' present, a '.' after '@' not last).
int smtp_addr_valid(const char *a);

// Standard base64 of src[0..n) into out (NUL-terminated). Returns output length, or 0 on overflow.
size_t smtp_b64(const unsigned char *src, size_t n, char *out, size_t outcap);

// 1 if the string contains any non-ASCII byte (so the Subject must be MIME word-encoded).
int smtp_needs_mime(const char *s);

// Write a complete DATA payload (headers + dot-stuffed, CRLF-normalized body + terminating
// "\r\n.\r\n") through `sink`. `date` is an RFC822 date string (caller supplies; tests pass a fixed one).
void smtp_write_message(smtp_sink_fn sink, void *ctx,
                        const char *from, const char *from_name, const char *to,
                        const char *subject, const char *body, const char *date);

#ifdef __cplusplus
}
#endif
