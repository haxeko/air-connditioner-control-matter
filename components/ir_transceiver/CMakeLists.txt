idf_component_register(SRCS "ir_transceiver.c" "ir_code.c" "ir_capture.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_rmt
                       PRIV_REQUIRES app_state esp_driver_gpio esp_timer esp_rom heap)
