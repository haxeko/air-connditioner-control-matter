idf_component_register(SRCS "matter_commissioner.cpp" "fabric_b_issuer.cpp" "paa_trust_store.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_matter
                       PRIV_REQUIRES app_state config_store esp_system)

# Production PAA roots (DCL mirror, see paa/README.md) -> one embedded binary bundle.
idf_build_get_property(python PYTHON)
file(GLOB PAA_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/paa/*.der")
set(PAA_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/paa_bundle.bin")
set(PAA_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen_paa_bundle.py")
add_custom_command(OUTPUT "${PAA_BUNDLE}"
                   COMMAND "${python}" "${PAA_SCRIPT}" "${PAA_BUNDLE}" "${CMAKE_CURRENT_SOURCE_DIR}/paa"
                   DEPENDS ${PAA_FILES} "${PAA_SCRIPT}"
                   VERBATIM)
add_custom_target(paa_bundle_gen DEPENDS "${PAA_BUNDLE}")
add_dependencies(${COMPONENT_LIB} paa_bundle_gen)
target_add_binary_data(${COMPONENT_LIB} "${PAA_BUNDLE}" BINARY DEPENDS paa_bundle_gen)
