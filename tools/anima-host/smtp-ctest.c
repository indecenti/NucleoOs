// Host gate for the pure SMTP layer (firmware/components/nucleo_smtp/smtp_proto.c).
// Proves reply parsing, base64, address validation, MIME and dot-stuffing on the PC.
#include "smtp_proto.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, tot = 0;
#define CHK(cond, msg) do { tot++; if (!(cond)) { fails++; printf("FAIL: %s\n", msg); } } while (0)

static char g[8192]; static size_t gl;
static void bufsink(void *ctx, const char *d, size_t n) { (void)ctx; if (gl + n < sizeof g) { memcpy(g + gl, d, n); gl += n; g[gl] = 0; } }
static const char *build(const char *fn, const char *to, const char *subj, const char *body) {
    gl = 0; g[0] = 0;
    smtp_write_message(bufsink, NULL, "m@x.com", fn, to, subj, body, "Mon, 01 Jan 2026 00:00:00 +0000");
    return g;
}
static int eqcode(const char *s, int want) { return smtp_resp_code(s, strlen(s)) == want; }
static int b64eq(const char *in, const char *want) {
    char o[64]; size_t n = smtp_b64((const unsigned char *)in, strlen(in), o, sizeof o);
    return n == strlen(want) && strcmp(o, want) == 0;
}

int main(void) {
    // ---- reply parsing (multi-line) ----
    CHK(eqcode("220 ready\r\n", 220), "resp 220");
    CHK(eqcode("235 2.7.0 ok\r\n", 235), "resp 235");
    CHK(eqcode("354 go\r\n", 354), "resp 354");
    CHK(eqcode("250-PIPELINING\r\n250 OK\r\n", 250), "resp multiline final");
    CHK(eqcode("250-PIPELINING\r\n", -1), "resp continuation incomplete");
    CHK(eqcode("250 ok", -1), "resp no CRLF incomplete");
    CHK(eqcode("", -1), "resp empty");
    CHK(eqcode("535-x\r\n535 auth fail\r\n", 535), "resp 535 multiline");

    // ---- address validation ----
    CHK(smtp_addr_valid("niki@gmail.com"), "addr ok");
    CHK(smtp_addr_valid("a.b+c@sub.example.co"), "addr complex ok");
    CHK(!smtp_addr_valid("abc"), "addr no @");
    CHK(!smtp_addr_valid("a@b"), "addr no dot");
    CHK(!smtp_addr_valid("a@b."), "addr trailing dot");
    CHK(!smtp_addr_valid("@gmail.com"), "addr no local");
    CHK(!smtp_addr_valid("a@@b.com"), "addr double @");
    CHK(!smtp_addr_valid("a b@c.com"), "addr space");
    CHK(!smtp_addr_valid(""), "addr empty");

    // ---- base64 (RFC 4648 vectors) ----
    CHK(b64eq("", ""), "b64 empty");
    CHK(b64eq("f", "Zg=="), "b64 f");
    CHK(b64eq("fo", "Zm8="), "b64 fo");
    CHK(b64eq("foo", "Zm9v"), "b64 foo");
    CHK(b64eq("foobar", "Zm9vYmFy"), "b64 foobar");

    // ---- MIME detection ----
    CHK(!smtp_needs_mime("plain ascii"), "mime ascii no");
    CHK(smtp_needs_mime("caff\xC3\xA8"), "mime utf8 yes");   // "caffè"

    // ---- message building ----
    const char *m = build("Mario", "d@y.com", "Ciao", "riga1\nriga2");
    CHK(strstr(m, "From: Mario <m@x.com>\r\n") != NULL, "msg From with name");
    CHK(strstr(m, "To: <d@y.com>\r\n") != NULL, "msg To");
    CHK(strstr(m, "Subject: Ciao\r\n") != NULL, "msg Subject plain");
    CHK(strstr(m, "Content-Type: text/plain; charset=UTF-8") != NULL, "msg content-type");
    CHK(strstr(m, "riga1\r\nriga2") != NULL, "msg body CRLF normalized");
    CHK(strstr(m, "\r\n.\r\n") != NULL, "msg terminator");

    m = build(NULL, "d@y.com", "Ciao", "x");
    CHK(strstr(m, "From: <m@x.com>\r\n") != NULL, "msg From no name");

    // dot-stuffing: a line starting with '.' must be doubled
    m = build("N", "d@y.com", "s", ".secret\nok");
    CHK(strstr(m, "..secret\r\n") != NULL, "msg dot-stuffed");

    // non-ASCII subject -> MIME word-encoded
    m = build("N", "d@y.com", "caff\xC3\xA8", "x");
    CHK(strstr(m, "Subject: =?UTF-8?B?") != NULL, "msg subject MIME");

    printf("%s smtp_proto: %d/%d passed\n", fails ? "FAIL" : "PASS", tot - fails, tot);
    return fails ? 1 : 0;
}
