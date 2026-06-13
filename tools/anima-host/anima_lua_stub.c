#include "nucleo_anima_lua.h"
#include <stdio.h>

void anima_lua_init(void) {
    printf("[Host Stub] Lua init called.\n");
}

bool anima_lua_run_script(const char* filepath, const char* arg) {
    printf("[Host Stub] Mock Lua execution: %s (arg: %s)\n", filepath, arg ? arg : "null");
    return true; // pretend it executed successfully
}
