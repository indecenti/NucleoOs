// Host stub for the IMU apps (level, goniometer, pedometer): a present BMI270 held at a small fixed tilt with
// some steps already counted, so the gallery renders live readings instead of "IMU not detected".
extern "C" {
bool nucleo_imu_present(void) { return true; }
const char *nucleo_imu_debug(void) { return "host"; }
bool nucleo_imu_sample(void) { return true; }
bool nucleo_imu_level(float *lx, float *ly, float *deg)
{
    if (lx) *lx = 0.12f;      // ~7 deg toward the right edge
    if (ly) *ly = 0.30f;      // ~17 deg toward the bottom: roll ~22 deg for the protractor
    if (deg) *deg = 18.8f;
    return true;
}
static unsigned s_steps = 4321;
unsigned nucleo_imu_steps(void) { return s_steps; }
void nucleo_imu_steps_reset(void) { }
}
