idf_component_register(SRCS "button_handler.c" "press_detector.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_timer)
