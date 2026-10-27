// ui-host stub for app_evilportal.cpp — a simulated captive-portal engine: SSID presets + page templates for
// the config lists, a running portal with live client / capture counters, and a few captured credentials for
// the Loot list. FICTIONAL data only (made-up SSIDs + credentials). Authorized-test tool.
#include <string.h>
#include <stdio.h>

extern "C" {

static bool g_running = false;
static char g_ssid[40] = "";

// ---- page templates --------------------------------------------------------
static const char *TPL[] = { "Google", "Facebook", "Microsoft 365", "Captive generico" };
static const int   NTPL = (int)(sizeof(TPL) / sizeof(TPL[0]));
int         nucleo_evilportal_template_count(void) { return NTPL; }
const char *nucleo_evilportal_template_name(int i) { return (i >= 0 && i < NTPL) ? TPL[i] : ""; }

// ---- SSID presets ----------------------------------------------------------
static const char *SSID[] = { "FreeWiFi", "Aeroporto_Free", "Hotel Guest", "Bar_Centrale", "Biblioteca" };
static const int   NSSID = (int)(sizeof(SSID) / sizeof(SSID[0]));
int         nucleo_evilportal_ssid_count(void)  { return NSSID; }
const char *nucleo_evilportal_ssid_preset(int i) { return (i >= 0 && i < NSSID) ? SSID[i] : ""; }

// ---- lifecycle -------------------------------------------------------------
int  nucleo_evilportal_start(const char *ssid, int) { g_running = true; snprintf(g_ssid, sizeof g_ssid, "%s", ssid ? ssid : "FreeWiFi"); return 0; }
int  nucleo_evilportal_start_twin(const char *ssid, int, const unsigned char *, int, int, bool) { g_running = true; snprintf(g_ssid, sizeof g_ssid, "%s", ssid ? ssid : "Twin"); return 0; }
void nucleo_evilportal_stop(void)     { g_running = false; }
bool nucleo_evilportal_running(void)  { return g_running; }
int  nucleo_evilportal_clone_page(const char *) { return 0; }
const char *nucleo_evilportal_clone_name(void)  { return ""; }
const char *nucleo_evilportal_ssid(void)        { return g_ssid[0] ? g_ssid : "FreeWiFi"; }

// ---- live counters ---------------------------------------------------------
int  nucleo_evilportal_clients(void)   { return 3; }
int  nucleo_evilportal_captures(void)  { return 2; }
int  nucleo_evilportal_confirmed(void) { return 1; }
unsigned nucleo_evilportal_uptime_s(void) { return 128; }
const char *nucleo_evilportal_logpath(void) { return "/sd/evilportal/loot.csv"; }

// ---- captured credentials (fictional) --------------------------------------
static const char *LOOT_U[] = { "mario.rossi@mail.it", "guest_01", "a.bianchi@corp.eu" };
static const char *LOOT_P[] = { "Estate2024!", "hunter2", "Password#9" };
static const int   NLOOT = (int)(sizeof(LOOT_U) / sizeof(LOOT_U[0]));
const char *nucleo_evilportal_last_user(void) { return LOOT_U[0]; }
const char *nucleo_evilportal_last_pass(void) { return LOOT_P[0]; }
int         nucleo_evilportal_recent_count(void) { return NLOOT; }
const char *nucleo_evilportal_recent_user(int i) { return (i >= 0 && i < NLOOT) ? LOOT_U[i] : ""; }
const char *nucleo_evilportal_recent_pass(int i) { return (i >= 0 && i < NLOOT) ? LOOT_P[i] : ""; }

// ---- evil-twin fields (Civetta path doesn't use them) ----------------------
bool          nucleo_evilportal_twin(void)           { return false; }
bool          nucleo_evilportal_twin_coherent(void)  { return false; }
int           nucleo_evilportal_twin_channel(void)   { return 0; }
unsigned long nucleo_evilportal_deauth_frames(void)  { return 0; }

// The RUNNING screen frees the shared canvas on the device; host no-ops.
void nucleo_screen_release(void) {}
bool nucleo_screen_acquire(void) { return true; }

}  // extern "C"
