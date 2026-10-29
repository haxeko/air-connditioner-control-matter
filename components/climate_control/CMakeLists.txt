idf_component_register(SRCS "climate_control.c" "climate_algo.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ac_profiles config_store
                       PRIV_REQUIRES app_state ir_transceiver)
