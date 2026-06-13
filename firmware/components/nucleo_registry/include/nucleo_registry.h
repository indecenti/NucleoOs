// Reads the OS registry (installed apps) from SD. See docs/registry.md.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define NUCLEO_MAX_APPS 48   // was 32; registry already lists 33+ apps, so the cap silently dropped the last one

typedef struct {
    char id[24];
    char version[12];
    char name[32];        // from the app manifest (falls back to id)
    char web_route[48];   // UI mount point, e.g. "/apps/notepad/"
    char icon[40];        // icon path or emoji glyph
    bool enabled;
} nucleo_app_t;

// Load /sd/system/registry/apps.json into memory.
esp_err_t nucleo_registry_load(void);

// Number of installed apps after a successful load.
int nucleo_registry_count(void);

// Installed apps array (length = nucleo_registry_count()).
const nucleo_app_t *nucleo_registry_apps(void);
