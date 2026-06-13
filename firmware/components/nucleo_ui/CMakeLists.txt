idf_component_register(
    SRCS "nucleo_ui.cpp" "nucleo_theme.cpp"
    INCLUDE_DIRS "include"
    REQUIRES m5stack__m5gfx nucleo_kbd nucleo_board nucleo_power esp_timer json
)
