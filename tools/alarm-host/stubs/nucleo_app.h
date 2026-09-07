// Host stub of the native app framework: enough of the contract for app_alarm.cpp to compile and run
// on the PC. The registration struct is captured by the harness, which then drives the real callbacks.
#pragma once
#include <stdbool.h>
#include "nucleo_kbd.h"

typedef struct {
    const char *id;
    const char *name;
    const char *category;
    const char *desc;
    char icon;
    unsigned short color;
    void (*on_enter)(void);
    void (*on_key)(int key, char ch);
    void (*on_tick)(void);
    void (*on_draw)(void);
    void (*on_exit)(void);
    unsigned int exclusive_flags;
} nucleo_app_def_t;

#ifdef __cplusplus
extern "C" {
#endif
void nucleo_app_register(const nucleo_app_def_t *app);
void nucleo_app_set_hint(const char *hint);
int  nucleo_app_content_top(void);
int  nucleo_app_content_height(void);
void nucleo_app_request_draw(void);
void nucleo_app_force_repaint(void);
void nucleo_app_set_tab_handler(void (*fn)(void));
void nucleo_app_set_back_handler(bool (*fn)(int key));
void nucleo_app_set_poll_handler(bool (*fn)(void));
void nucleo_app_set_brightness(int pct);
int  nucleo_app_brightness(void);
#ifdef __cplusplus
}
#endif
