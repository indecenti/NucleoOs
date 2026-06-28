idf_component_register(
    SRCS "nucleo_imu.c" "nucleo_imu_motion.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
