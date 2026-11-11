idf_component_register(SRCS "matter_thermostat.cpp" "device_identity.cpp" "matter_map.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_matter
                       PRIV_REQUIRES ac_profiles app_state config_store matter_commissioner mbedtls)
