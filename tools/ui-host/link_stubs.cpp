// ui-host: the ESP-NOW "Vicino" backend (nucleo_link_espnow.c) as a tiny simulated device for app_link.cpp.
// The scene seeds what the radio "sees" through files on the sandbox SD:
//   link/peers.txt  — one "name proto" per line (proto 0 = Nucleo, 1 = Bruce)
//   link/offer.txt  — "from|filename|sizeKB" makes an incoming file offer pending (RECV tab)
//   link/cmd.txt    — "from|command" makes an incoming command pending (CMD tab)
// The file browser (SEND tab) reads the real sandbox sd/data folder, which the @sd lines populate.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include "nucleo_link_espnow.h"
}

extern "C" {

static char s_pname[8][48]; static int s_pproto[8]; static int s_pn = -1;
static void load_peers(void)
{
    if (s_pn >= 0) return;
    s_pn = 0;
    FILE *f = fopen("sd/link/peers.txt", "rb");
    if (!f) return;
    char line[80];
    while (s_pn < 8 && fgets(line, sizeof line, f)) {
        char nm[48]; int pr = 0;
        if (sscanf(line, "%47s %d", nm, &pr) >= 1) { snprintf(s_pname[s_pn], sizeof s_pname[0], "%s", nm); s_pproto[s_pn] = pr ? 1 : 0; s_pn++; }
    }
    fclose(f);
}

// Split "a|b|c" from a one-line file. Returns true if the file exists and has a first field.
static bool read_pipe(const char *path, char *a, int acap, char *b, int bcap, char *c, int ccap)
{
    FILE *f = fopen(path, "rb"); if (!f) return false;
    char line[200]; bool got = fgets(line, sizeof line, f) != nullptr; fclose(f);
    if (!got) return false;
    char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
    char *p1 = line, *p2 = strchr(line, '|'); if (p2) *p2++ = 0;
    char *p3 = p2 ? strchr(p2, '|') : nullptr; if (p3) *p3++ = 0;
    if (a && acap) snprintf(a, acap, "%s", p1);
    if (b && bcap) snprintf(b, bcap, "%s", p2 ? p2 : "");
    if (c && ccap) snprintf(c, ccap, "%s", p3 ? p3 : "");
    return p1[0] != 0;
}

bool        nlink_svc_start(void) { return true; }
void        nlink_svc_stop(void) {}
void        nlink_svc_pump(void) {}
const char *nlink_svc_name(void) { return "MyCard-07"; }
int         nlink_svc_channel(void) { return 6; }
const char *nlink_svc_inbox(void) { return "sd/data/Vicino"; }
void        nlink_svc_set_inbox(const char *) {}
void        nlink_svc_discover(nlink_proto_t) {}
int         nlink_svc_peer_count(void) { load_peers(); return s_pn; }
const char *nlink_svc_peer_name(int i) { load_peers(); return (i >= 0 && i < s_pn) ? s_pname[i] : ""; }
const uint8_t *nlink_svc_peer_mac(int) { static const uint8_t m[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x07 }; return m; }
int         nlink_svc_peer_proto(int i) { load_peers(); return (i >= 0 && i < s_pn) ? s_pproto[i] : 0; }
void        nlink_svc_peer_clear(void) {}
bool        nlink_svc_send_file(int, const char *, nlink_proto_t) { return true; }
void        nlink_svc_listen(nlink_proto_t) {}

bool nlink_svc_offer_pending(char *name, int ncap, char *from, int fcap, uint32_t *size)
{
    char nm[64], fr[48], sz[16];
    if (!read_pipe("sd/link/offer.txt", fr, sizeof fr, nm, sizeof nm, sz, sizeof sz)) return false;
    if (from && fcap) snprintf(from, fcap, "%s", fr);
    if (name && ncap) snprintf(name, ncap, "%s", nm);
    if (size) *size = (uint32_t)strtoul(sz, nullptr, 10) * 1024u;
    return true;
}
void nlink_svc_offer_answer(bool) {}

bool nlink_svc_send_cmd(int, const char *, nlink_proto_t) { return true; }
bool nlink_svc_cmd_pending(char *cmd, int ccap, char *from, int fcap)
{
    char fr[48], cm[160];
    if (!read_pipe("sd/link/cmd.txt", fr, sizeof fr, cm, sizeof cm, nullptr, 0)) return false;
    if (from && fcap) snprintf(from, fcap, "%s", fr);
    if (cmd && ccap) snprintf(cmd, ccap, "%s", cm);
    return true;
}
void nlink_svc_cmd_confirm(bool) {}

void nlink_svc_status(nlink_status_t *out) { memset(out, 0, sizeof *out); }
void nlink_svc_cancel(void) {}

const char *nucleo_setup_device_name(void) { return "MyCard-07"; }

}
