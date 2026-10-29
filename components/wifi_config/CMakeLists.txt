idf_component_register(SRCS "wifi_config.c" "wifi_chip_bridge.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_matter app_state config_store esp_wifi esp_event esp_timer esp_driver_gpio lwip)
