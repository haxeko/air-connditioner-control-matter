#pragma once

/*
 * CHIP project configuration for a node that is both a Matter server (thermostat,
 * Fabric A owned by the home ecosystem) and a commissioner (Fabric B owned by this device).
 *
 * The commissioner is brought up with chip::Controller::DeviceControllerFactory on top of the
 * already running chip::Server, sharing its FabricTable, persistent storage, session keystore,
 * session-resumption storage and group data provider, but listening on its own UDP port.
 * This is the "combined server/commissioner" pattern used by connectedhomeip
 * examples/platform/linux/CommissionerMain.cpp (tv-app, fabric-sync bridge).
 *
 * DeviceControllerFactory::InitSystemState() re-initialises the InteractionModelEngine
 * singleton with the controller's ExchangeManager. Server-side subscriptions must therefore keep
 * the ExchangeManager they were created on: that is what this flag does (connectedhomeip
 * issue #23625, see src/include/platform/CHIPDeviceConfig.h and src/app/ReadHandler.cpp).
 */
#define CHIP_CONFIG_UNSAFE_SUBSCRIPTION_EXCHANGE_MANAGER_USE 1

/* Sessions the controller side may hold at the same time (sensor + commissioning target). */
#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES 4

/*
 * Names published by the device, all carrying the firmware version suffix "vX.Y.Z":
 *   - Basic Information VendorName / ProductName (read by every home app during commissioning),
 *   - DNS-SD TXT "DN" of the commissionable-node advertisement (shown by some commissioners),
 *   - default NodeLabel (matter_thermostat.cpp, from the same CONFIG_APP_DEVICE_NAME).
 * APP_FW_VERSION is a string literal defined for every component by the top-level CMakeLists.txt
 * (= PROJECT_VER = esp_app_get_description()->version). CHIP copies these with strcpy() into
 * 33-byte buffers, so they must be string literals of at most 32 bytes (checked in
 * matter_thermostat.cpp).
 */
#include "sdkconfig.h"

#ifndef APP_FW_VERSION
#error "APP_FW_VERSION must be defined by the top-level CMakeLists.txt"
#endif

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME CONFIG_APP_VENDOR_NAME
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME CONFIG_APP_DEVICE_NAME " v" APP_FW_VERSION
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME 1
#define CHIP_DEVICE_CONFIG_DEVICE_NAME CONFIG_APP_DEVICE_NAME " v" APP_FW_VERSION
