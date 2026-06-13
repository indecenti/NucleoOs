idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nucleo_storage nucleo_registry nucleo_eventbus nucleo_ui nucleo_setup nucleo_httpd nucleo_recorder nucleo_discovery nvs_flash nucleo_app nucleo_anima nucleo_power nucleo_auth nucleo_usbmsc nucleo_evilportal nucleo_arb nucleo_voice nucleo_tts
)
