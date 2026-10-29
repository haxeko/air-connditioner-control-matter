idf_component_register(SRCS "web_server.c" "web_api.c" "web_util.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_http_server esp_netif lwip esp_timer esp_system esp_app_format esp_psram
                                     heap esp_hw_support spi_flash
                                     app_state config_store wifi_config matter_thermostat matter_commissioner
                                     remote_temperature climate_control ac_profiles ir_transceiver qrcode)

# Offline UI: www/* -> gzip -> embedded binary data (served with Content-Encoding: gzip).
idf_build_get_property(python PYTHON)
set(GZ_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gzip_asset.py")
set(WEB_ASSETS index.html app.js app.css)
set(WEB_GZ "")
foreach(asset ${WEB_ASSETS})
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(OUTPUT "${out}"
                       COMMAND "${python}" "${GZ_SCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/www/${asset}" "${out}"
                       DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/www/${asset}" "${GZ_SCRIPT}"
                       VERBATIM)
    list(APPEND WEB_GZ "${out}")
endforeach()
add_custom_target(web_assets_gz DEPENDS ${WEB_GZ})
add_dependencies(${COMPONENT_LIB} web_assets_gz)
foreach(out ${WEB_GZ})
    target_add_binary_data(${COMPONENT_LIB} "${out}" BINARY DEPENDS web_assets_gz)
endforeach()

# Locked esp_matter version (from the managed component manifest) for the diagnostics page.
idf_component_get_property(esp_matter_dir espressif__esp_matter COMPONENT_DIR)
file(STRINGS "${esp_matter_dir}/idf_component.yml" esp_matter_version_line REGEX "^version:")
string(REGEX REPLACE "^version:[ \"]*([^ \"]+).*$" "\\1" esp_matter_version "${esp_matter_version_line}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_ESP_MATTER_VERSION="${esp_matter_version}")
