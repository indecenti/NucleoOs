idf_component_register(
    SRCS "nucleo_ir.c" "nucleo_ir_proto.c" "nucleo_ir_pack.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt esp_http_server json nucleo_auth nucleo_board
)
