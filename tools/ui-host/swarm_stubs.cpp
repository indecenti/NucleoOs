// ui-host: the ESP-NOW swarm backend (nucleo_swarm_espnow.c) as a tiny simulated device for app_swarm.cpp.
// The scene seeds what the radio "sees" through files on the sandbox SD: swarm/peers.txt (one "id free busy"
// per line), swarm/open.txt (membership is OPEN when present), swarm/last.txt (a live-activity line).
#include <stdio.h>
#include <string.h>

extern "C" {

static char s_ids[8][24];
static int  s_free[8], s_busy[8], s_count = -1;

static void load(void)
{
    if (s_count >= 0) return;
    s_count = 0;
    FILE *f = fopen("sd/swarm/peers.txt", "rb");
    if (!f) return;
    char line[64];
    while (s_count < 8 && fgets(line, sizeof line, f)) {
        char id[24]; int fr = 0, bs = 0;
        if (sscanf(line, "%23s %d %d", id, &fr, &bs) >= 1) {
            snprintf(s_ids[s_count], sizeof s_ids[0], "%s", id);
            s_free[s_count] = fr; s_busy[s_count] = bs; s_count++;
        }
    }
    fclose(f);
}

bool        swarm_svc_start(void) { return true; }
void        swarm_svc_stop(void) {}
const char *swarm_svc_name(void) { return "MyCard-07"; }
int         swarm_svc_channel(void) { return 6; }
bool        swarm_svc_is_open(void) { FILE *f = fopen("sd/swarm/open.txt", "rb"); if (f) { fclose(f); return true; } return false; }
int         swarm_svc_set_passphrase(const char *, const char *) { return 0; }
int         swarm_svc_peer_count(void) { load(); return s_count; }
const char *swarm_svc_peer_id(int i) { load(); return (i >= 0 && i < s_count) ? s_ids[i] : ""; }
int         swarm_svc_peer_free(int i) { load(); return (i >= 0 && i < s_count) ? s_free[i] : 0; }
int         swarm_svc_peer_busy(int i) { load(); return (i >= 0 && i < s_count) ? s_busy[i] : 0; }
void        swarm_svc_ping(void) {}
void        swarm_svc_last(char *out, int cap)
{
    out[0] = 0;
    FILE *f = fopen("sd/swarm/last.txt", "rb");
    if (!f) return;
    if (fgets(out, cap, f)) { char *nl = strpbrk(out, "\r\n"); if (nl) *nl = 0; }
    fclose(f);
}

}
