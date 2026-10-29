idf_component_register(SRCS "app_main.cpp" "app_console.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_state config_store matter_thermostat matter_commissioner remote_temperature
                                     wifi_config web_config button_handler ir_transceiver ac_profiles climate_control
                                     console qrcode esp_netif esp_event nvs_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
