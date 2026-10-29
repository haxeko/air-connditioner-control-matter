idf_component_register(SRCS "ac_profiles.c" "ac_types.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES app_state config_store ir_transceiver nvs_flash)
