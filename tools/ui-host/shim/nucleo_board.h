// ui-host: mount points are RELATIVE — run.mjs starts every scene in its own fresh sandbox folder
// (build/ui-host/box/wN), so files one scene writes (pins, recents, theme, settings) never leak.
#pragma once
#define NUCLEO_SD_MOUNT   "sd"
#define NUCLEO_CFG_MOUNT  "cfg"
#define NUCLEO_CFG_LABEL  "cfg"
#ifndef HLOG
#define HLOG(tag, ...) ((void)0)
#endif
