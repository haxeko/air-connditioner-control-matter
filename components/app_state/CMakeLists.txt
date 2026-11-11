idf_component_register(SRCS "app_state.c" "app_diag.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer log freertos
                       PRIV_REQUIRES espcoredump esp_app_format)
