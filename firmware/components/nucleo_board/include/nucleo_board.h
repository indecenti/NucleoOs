// Board pin map for the M5Stack Cardputer (ESP32-S3 / StampS3).
#pragma once

// microSD over SPI. IMPORTANT: M5GFX drives the Cardputer display on SPI3_HOST
// (LovyanGFX default on ESP32-S3), and nucleo_ui_init() runs d.init() BEFORE the SD
// mount — so the SD must use the OTHER host (SPI2) or spi_bus_initialize() fails with
// ESP_ERR_INVALID_STATE and the card never mounts. Pins are routed via the GPIO matrix,
// so any pin works on either host. (Earlier note had display/SD hosts swapped.)
#define NUCLEO_SD_SPI_HOST   SPI2_HOST
#define NUCLEO_SD_PIN_SCLK   40
#define NUCLEO_SD_PIN_MISO   39
#define NUCLEO_SD_PIN_MOSI   14
#define NUCLEO_SD_PIN_CS     12

// SD mount point (FATFS VFS root for OS content)
#define NUCLEO_SD_MOUNT      "/sd"

// Power-loss-safe config store: a LittleFS partition on the ESP32 internal flash
// (label "cfg" in partitions.csv), mounted here. Brick-class OS config lives here
// instead of on the SD's FAT, which can corrupt on a power cut mid-write.
#define NUCLEO_CFG_MOUNT     "/cfg"
#define NUCLEO_CFG_LABEL     "cfg"

// Reference pins for other peripherals (handled by M5GFX/M5Unified in nucleo_ui).
// Display ST7789 (SPI2): MOSI 35, SCLK 36, CS 37, DC 34, RST 33, BL 38, 240x135.
// Speaker I2S: BCLK 41, WS 43, DOUT 42, DIN 46 (mic).   I2C: SDA 2, SCL 1.

// Built-in PDM microphone (SPM1423) for the voice recorder. The Cardputer wires
// the PDM clock to GPIO43 and the PDM data line to GPIO46. Used by nucleo_recorder.
#define NUCLEO_MIC_PIN_CLK   43
#define NUCLEO_MIC_PIN_DATA  46

// Built-in speaker amplifier (I2S standard mode), used by nucleo_audio for playback.
// IMPORTANT: the speaker WS line is GPIO43 — the SAME pin as the PDM mic clock above.
// Recording and playback therefore CANNOT run at the same time; nucleo_audio refuses to
// start while a recording is active, and claims the I2S TX pins only while playing.
#define NUCLEO_SPK_PIN_BCLK  41
#define NUCLEO_SPK_PIN_WS    43
#define NUCLEO_SPK_PIN_DOUT  42

// ---- Diagnostic heap/RAM logging gate ----------------------------------------------------------
// The verbose heap traces (boot stages, per-query IN/OUT, L1 load/unload, TLS peak) are priceless
// while chasing a self-reboot but they are NOT free at runtime: each call formats text into the
// logger's per-call stack scratch buffer and churns the fixed 4 KB RAM ring (see nucleo_log.c).
// The format STRINGS themselves live in flash .rodata, not SRAM — so the cost is purely the
// transient stack buffer + ring churn, paid only when a line actually fires.
//
// Define NUCLEO_HEAPLOG=0 in a production build (e.g. -DNUCLEO_HEAPLOG=0) and every HLOG() below
// compiles to nothing: zero flash for the string, zero stack scratch, zero ring churn. Essential
// status one-liners (reset reason, "STA got IP", "HTTP server up", "L1 ready") stay ESP_LOGI and
// are unaffected — only the high-frequency diagnostic tracing is gated. Default 1 (on) for now.
#ifndef NUCLEO_HEAPLOG
#define NUCLEO_HEAPLOG 1
#endif
#if NUCLEO_HEAPLOG
#define HLOG(tag, ...) ESP_LOGI(tag, __VA_ARGS__)   // expands at the call site, which already has esp_log.h
#else
#define HLOG(tag, ...) ((void)0)
#endif
