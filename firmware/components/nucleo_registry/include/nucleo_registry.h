// Reads the OS registry (installed apps) from SD. See docs/registry.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Capacity. The shipped registry already lists 48 apps and the Agent app adds the user's own on top, so
// the old cap of 48 silently dropped every user app. 80 entries fit in LESS static RAM than the old 48
// (94 B vs 157 B per entry — see the budget assert in nucleo_registry.c): web_route and the standard icon
// path are derived from the id instead of being stored.
#define NUCLEO_MAX_APPS    80
#define NUCLEO_APP_ID_MAX  24   // same as the Agent app's isValidId (apps/agent/www/app-publish.js)

enum { NUCLEO_ROUTE_NONE = 0, NUCLEO_ROUTE_STD };               // STD = "/apps/<id>/"
enum { NUCLEO_ICON_NONE = 0, NUCLEO_ICON_STD, NUCLEO_ICON_RAW }; // STD = "/apps/<id>/icon.svg"

typedef struct {
    char    id[NUCLEO_APP_ID_MAX + 1];
    char    version[12];
    char    name[32];        // from the app manifest (falls back to id)
    char    icon_raw[22];    // manifest "icon" verbatim when not the standard path ("assets/icon.svg", a glyph)
    uint8_t route;           // NUCLEO_ROUTE_*
    uint8_t icon;            // NUCLEO_ICON_*
    bool    enabled;
} nucleo_app_t;

// Load /sd/system/registry/apps.json into memory.
esp_err_t nucleo_registry_load(void);

// Number of installed apps after a successful load.
int nucleo_registry_count(void);

// Installed apps array (length = nucleo_registry_count()).
const nucleo_app_t *nucleo_registry_apps(void);

// The manifest's web_route / icon strings exactly as /api/apps has always reported them ("" when the
// manifest had none), rebuilt into `buf`. Return `buf`.
const char *nucleo_registry_route(const nucleo_app_t *a, char *buf, size_t n);
const char *nucleo_registry_icon(const nucleo_app_t *a, char *buf, size_t n);
