#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool  nucleo_imu_present(void);
void  nucleo_imu_sample(void);
void  nucleo_imu_level(float *lx, float *ly, float *deg);
float nucleo_imu_energy(void);
#ifdef __cplusplus
}
#endif
