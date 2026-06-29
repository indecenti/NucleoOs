// smtp_proto — see smtp_proto.h. Pure C, no platform deps.
#include "smtp_proto.h"
#include <string.h>
#include <stdio.h>

int smtp_resp_code(const char *buf, size_t len) {
    if (len < 5) return -1;
    if (buf[len - 2] != '\r' || buf[len - 1] != '\n') return -1;      // last line not terminated
    // start of the last complete line
    size_t ls = 0;
    for (size_t i = len - 3; i > 0; --i) { if (buf[i - 1] == '\n') { ls = i; break; } }
    if (len - ls < 4) return -1;
    if (buf[ls + 3] == '-') return -1;                                // still a continuation line
    if (buf[ls + 3] != ' ') return -1;
    if (buf[ls] < '0' || buf[ls] > '9' || buf[ls + 1] < '0' || buf[ls + 1] > '9' ||
        buf[ls + 2] < '0' || buf[ls + 2] > '9') return -1;
    return (buf[ls] - '0') * 100 + (buf[ls + 1] - '0') * 10 + (buf[ls + 2] - '0');
}

int smtp_addr_valid(const char *a) {
    if (!a || !a[0]) return 0;
    const char *at = NULL;
    for (const char *p = a; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') return 0;
        if (*p == '@') { if (at) return 0; at = p; }                  // exactly one '@'
    }
    if (!at || at == a || !at[1]) return 0;                           // need local part + domain
    const char *dot = strchr(at + 1, '.');
    if (!dot || dot[1] == 0) return 0;                                // domain needs a dot, not trailing
    return 1;
}

size_t smtp_b64(const unsigned char *src, size_t n, char *out, size_t outcap) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((n + 2) / 3) * 4;
    if (outcap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = src[i] << 16;
        if (i + 1 < n) v |= src[i + 1] << 8;
        if (i + 2 < n) v |= src[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

int smtp_needs_mime(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p)
        if (*p > 0x7f) return 1;
    return 0;
}

static void sink_str(smtp_sink_fn sink, void *ctx, const char *s) { sink(ctx, s, strlen(s)); }

void smtp_write_message(smtp_sink_fn sink, void *ctx,
                        const char *from, const char *from_name, const char *to,
                        const char *subject, const char *body, const char *date) {
    char line[640], b64[256];
    if (from_name && from_name[0]) snprintf(line, sizeof line, "From: %s <%s>\r\n", from_name, from);
    else                            snprintf(line, sizeof line, "From: <%s>\r\n", from);
    sink_str(sink, ctx, line);
    snprintf(line, sizeof line, "To: <%s>\r\n", to ? to : ""); sink_str(sink, ctx, line);
    const char *subj = subject ? subject : "";
    if (smtp_needs_mime(subj) && smtp_b64((const unsigned char *)subj, strlen(subj), b64, sizeof b64))
        snprintf(line, sizeof line, "Subject: =?UTF-8?B?%s?=\r\n", b64);
    else
        snprintf(line, sizeof line, "Subject: %s\r\n", subj);
    sink_str(sink, ctx, line);
    snprintf(line, sizeof line,
             "Date: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
             "Content-Transfer-Encoding: 8bit\r\nX-Mailer: NucleoOS\r\n\r\n",
             date ? date : "");
    sink_str(sink, ctx, line);

    // body: normalize to CRLF and dot-stuff lines starting with '.'
    char chunk[256]; size_t n = 0; int at_line_start = 1;
    for (const char *p = body ? body : ""; *p; ++p) {
        char c = *p;
        if (at_line_start && c == '.') { if (n >= sizeof chunk) { sink(ctx, chunk, n); n = 0; } chunk[n++] = '.'; }
        if (c == '\r') continue;
        if (c == '\n') {
            if (n + 2 > sizeof chunk) { sink(ctx, chunk, n); n = 0; }
            chunk[n++] = '\r'; chunk[n++] = '\n'; at_line_start = 1; continue;
        }
        if (n >= sizeof chunk) { sink(ctx, chunk, n); n = 0; }
        chunk[n++] = c; at_line_start = 0;
    }
    if (n) sink(ctx, chunk, n);
    sink_str(sink, ctx, "\r\n.\r\n");
}
