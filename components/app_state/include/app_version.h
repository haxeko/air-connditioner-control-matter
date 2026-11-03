#pragma once

/*
 * Firmware version and the names derived from it. APP_FW_VERSION is a string literal defined for
 * every component by the top-level CMakeLists.txt from PROJECT_VER (the same string as
 * esp_app_get_description()->version). Change the version only there.
 *
 * Every name the device publishes carries the version suffix, so a phone, a Wi-Fi list or a home
 * app always shows which firmware runs on the device:
 *   APP_DEVICE_FULL_NAME   "May lanh IR v1.2.0"  Matter ProductName, NodeLabel, DNS-SD DN, web page
 *   SoftAP SSID            "AC-Matter-1A2B-v1.2.0"   (wifi_config.c)
 *   BLE name               "AC-1A2B-v1.2.0"          (matter_thermostat.cpp, 15 characters max)
 * The Matter names are also defined in main/matter_project_config.h for CHIP (same expression).
 */

#include "sdkconfig.h"

#ifndef APP_FW_VERSION
#error "APP_FW_VERSION must be defined by the top-level CMakeLists.txt"
#endif

#define APP_DEVICE_FULL_NAME CONFIG_APP_DEVICE_NAME " v" APP_FW_VERSION
#define APP_VERSION_SUFFIX "-v" APP_FW_VERSION
