idf_component_register(SRCS "matter_thermostat.cpp" "device_identity.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_matter
                       PRIV_REQUIRES app_state config_store matter_commissioner mbedtls)
