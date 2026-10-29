// nucleo_gbc — the Game Boy Color host: nucleo_gb.c compiled a second time around Walnut-CGB.
//
// Everything about a cartridge (page cache, battery RAM banks, saves, states, sound, clock) is the Game
// Boy's code, so a fix there reaches both consoles. What differs is inside nucleo_gb.c under NUCLEO_GB_CGB:
// the core, its two RAM blocks (allocated first: the 32 KB work RAM needs the heap's largest block), the
// 16-bit ROM reads Walnut asks for, and the colour scanline. Every global name of this copy — ours and the
// core's — is renamed here, so both cores link into one module.
#include "nucleo_gbc.h"

#define NUCLEO_GB_CGB 1

// Our API, as nucleo_gbc.h declares it.
#define nucleo_gb_autosave       nucleo_gbc_autosave
#define nucleo_gb_cart_name      nucleo_gbc_cart_name
#define nucleo_gb_close          nucleo_gbc_close
#define nucleo_gb_get_regs       nucleo_gbc_get_regs
#define nucleo_gb_get_stats      nucleo_gbc_get_stats
#define nucleo_gb_is_open        nucleo_gbc_is_open
#define nucleo_gb_open           nucleo_gbc_open
#define nucleo_gb_probe          nucleo_gbc_probe
#define nucleo_gb_reset          nucleo_gbc_reset
#define nucleo_gb_reset_counters nucleo_gbc_reset_counters
#define nucleo_gb_run_frame      nucleo_gbc_run_frame
#define nucleo_gb_save           nucleo_gbc_save
#define nucleo_gb_set_buttons    nucleo_gbc_set_buttons
#define nucleo_gb_set_frameskip  nucleo_gbc_set_frameskip
#define nucleo_gb_set_interlace  nucleo_gbc_set_interlace
#define nucleo_gb_state_exists   nucleo_gbc_state_exists
#define nucleo_gb_state_load     nucleo_gbc_state_load
#define nucleo_gb_state_save     nucleo_gbc_state_save
#define nucleo_gb_title          nucleo_gbc_title

// The core's globals, which Peanut-GB defines under the same names.
#define __gb_draw_line           wgb_draw_line
#define __gb_execute_cb          wgb_execute_cb
#define __gb_read                wgb_read
#define __gb_read16              wgb_read16
#define __gb_read32              wgb_read32
#define __gb_step_cpu_x          wgb_step_cpu_x
#define __gb_write               wgb_write
#define __gb_write16             wgb_write16
#define __gb_write32             wgb_write32
#define gb_colour_hash           wgb_colour_hash
#define gb_get_rom_name          wgb_get_rom_name
#define gb_get_save_size         wgb_get_save_size
#define gb_get_save_size_s       wgb_get_save_size_s
#define gb_init                  wgb_init
#define gb_init_lcd              wgb_init_lcd
#define gb_init_serial           wgb_init_serial
#define gb_reset                 wgb_reset
#define gb_run_frame             wgb_run_frame
#define gb_run_frame_dualfetch   wgb_run_frame_dualfetch
#define gb_set_bootrom           wgb_set_bootrom
#define gb_set_rtc               wgb_set_rtc
#define gb_tick_rtc              wgb_tick_rtc

#include "nucleo_gb.c"
