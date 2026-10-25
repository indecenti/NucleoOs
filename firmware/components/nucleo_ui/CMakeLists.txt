idf_component_register(
    SRCS "nucleo_ui.cpp" "nucleo_ui_modal.cpp" "nucleo_theme.cpp"
         "nucleo_extdisp.cpp" "extdisp_mirror.cpp" "extdisp_probe.c"
    INCLUDE_DIRS "include"
    REQUIRES m5stack__m5gfx nucleo_kbd nucleo_board nucleo_power esp_timer json
             nucleo_storage esp_driver_spi esp_driver_gpio esp_app_format
)
