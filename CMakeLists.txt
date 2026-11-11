# AC Matter IR controller for ESP32-S3
#
# Locked SDK versions (see README.md, section "Phiên bản SDK"):
#   ESP-IDF                v6.0.2   (commit 7101770dc6db2667b3c477cc31365dd1acd6db4e)
#   espressif/esp_matter   1.6.1    (component registry, esp-matter commit 067ccbd1a4)
#   connectedhomeip        b1902b39ac6734913ce5f4d2abf153693a50264b (bundled in esp_matter 1.6.1)
cmake_minimum_required(VERSION 3.16)

# Firmware version: the only place to change it. It is the ESP-IDF app version, the Matter
# SoftwareVersionString, and the "vX.Y.Z" suffix of every name the device publishes
# (Matter product/node/commissionable name, BLE name, configuration Wi-Fi SSID).
set(PROJECT_VER "1.5.0")
# Matter SoftwareVersion (uint32, must grow with every release): MAJOR*10000 + MINOR*100 + PATCH.
if(NOT PROJECT_VER MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PROJECT_VER must be MAJOR.MINOR.PATCH, got '${PROJECT_VER}'")
endif()
math(EXPR PROJECT_VER_NUMBER "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "6.0.2")
    message(WARNING "This project is validated with ESP-IDF v6.0.2, current IDF is "
                    "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
endif()

project(ac_matter_ir)

# Same global options as the official esp-matter managed-component example.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
# Version as a C string literal for names that CHIP needs at compile time (main/matter_project_config.h).
# Same mechanism ESP-IDF uses for IDF_VER; PROJECT_VER is read back after a possible
# CONFIG_APP_PROJECT_VER_FROM_CONFIG override so it always equals esp_app_get_description()->version.
idf_build_get_property(app_fw_version PROJECT_VER)
idf_build_set_property(COMPILE_DEFINITIONS "APP_FW_VERSION=\"${app_fw_version}\"" APPEND)

# IPv4 strong host model (configuration portal only reachable from the SoftAP side), injected into
# lwIP with the documented ESP-IDF mechanism for custom hooks (api-guides/lwip.rst "Customized lwIP Hooks").
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_options(${lwip} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}/components/wifi_config/lwip_hooks")
target_compile_definitions(${lwip} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"app_lwip_hooks.h\"")

# The commissioner validates sensor attestation without a trusted real-time clock (no Internet/SNTP
# by design; mbedTLS is built without MBEDTLS_HAVE_TIME_DATE). Like the connectedhomeip ESP32 GN build
# (src/credentials/BUILD.gn) and esp_matter's own controller builds, skip the DAC "valid at current
# time" check; chain signatures, revocation-independent checks and the CD are still verified.
idf_component_get_property(esp_matter_lib espressif__esp_matter COMPONENT_LIB)
target_compile_definitions(${esp_matter_lib} PRIVATE CURRENT_TIME_NOT_IMPLEMENTED=1)
