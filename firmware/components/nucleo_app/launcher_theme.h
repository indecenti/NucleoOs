// Shared theme for the on-device launcher (and any native UI that wants to match it).
// Single source of truth for the RGB565 palette and screen geometry — previously these
// constants were copy-pasted as magic numbers across the launcher code. Pure header: no
// storage, safe to include from every translation unit.
#pragma once

// ---- screen geometry (landscape, rotation 1) -------------------------------
static const int W = 240;          // display width
static const int H = 135;          // display height
#define BAR       16               // status / breadcrumb bar height
#define INSTR     0                // (retired) the focused-item description now lives inside the hero card
#define HINT      14               // bottom hint bar height
#define LIST_TOP    BAR                              // first y of the scrolling list band
// NB: must NOT be named LIST_H — that collides with FreeRTOS list.h's include guard and
// silently breaks every header that pulls in <freertos/list.h> after this one.
#define LIST_BAND_H (H - HINT - INSTR - LIST_TOP)    // height of the animated list band

// ---- base palette ----------------------------------------------------------
#include "nucleo_theme.h"

// Macro per compatibilità con i codici esistenti
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define LINE  THEME_LINE
#define INK   THEME_INK

// ---- accent palette (matches the web simulator's per-category colors) -------
#define C_BLUE   0x4D1F
#define C_PINK   0xFBB6
#define C_GREEN  0x8FF3
#define C_YELLOW 0xFE8C
#define C_PURPLE 0xC5F5
#define C_GREY   0x8C71
#define C_RED    0xF96B
