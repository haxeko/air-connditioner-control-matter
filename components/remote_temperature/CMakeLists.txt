idf_component_register(SRCS "remote_temperature.cpp" "rt_logic.c" "local_sensor.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES config_store
                       PRIV_REQUIRES esp_matter app_state matter_commissioner esp_driver_i2c)
