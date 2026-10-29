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
