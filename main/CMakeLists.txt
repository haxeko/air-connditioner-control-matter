idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_state config_store matter_thermostat matter_commissioner
                                     esp_netif esp_event nvs_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
