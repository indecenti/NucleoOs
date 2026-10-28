#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_VERSION 0x10A
#include <ctype.h>
#include <string.h>
// newlib has it, mingw does not: a case-insensitive strstr (the emulators look for ".zip" in a path).
static inline char *strcasestr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) { size_t i = 0; while (i < l && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++; if (i == l) return (char *)h; }
    return l ? NULL : (char *)h;
}
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "ERR"; }
