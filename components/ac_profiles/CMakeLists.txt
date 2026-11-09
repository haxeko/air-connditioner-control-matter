idf_component_register(SRCS "ac_profiles.c" "ac_types.c" "ac_irlib.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common ir_transceiver
                       PRIV_REQUIRES app_state config_store irremote_esp8266 nvs_flash)
