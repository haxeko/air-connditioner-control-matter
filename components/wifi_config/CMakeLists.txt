idf_component_register(SRCS "wifi_config.c" "wifi_chip_bridge.cpp" "ip4_guard.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_matter app_state config_store esp_wifi esp_event esp_timer esp_driver_gpio lwip)

# app_ip4_input_guard() is only referenced from inside lwIP (LWIP_HOOK_IP4_INPUT): keep it linked.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u app_ip4_input_guard")
