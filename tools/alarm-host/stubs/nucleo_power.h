#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool nucleo_power_battery_available(void);
int  nucleo_power_battery_pct(void);
#ifdef __cplusplus
}
#endif
