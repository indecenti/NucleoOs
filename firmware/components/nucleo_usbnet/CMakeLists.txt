idf_component_register(
    SRCS "nucleo_usbnet.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb esp_netif lwip esp_event esp_hw_support
)
