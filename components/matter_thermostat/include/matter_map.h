#pragma once

/*
 * How the AC state is shown in, and read back from, the Matter clusters of the Room Air Conditioner
 * endpoint. Pure functions (no ESP-IDF), unit tested on the host (test/host/test_matter_map.c).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thermostat SystemModeEnum values used here (same as TSTAT_MODE_* in matter_thermostat.h). */
#define MATTER_MAP_MODE_OFF 0
#define MATTER_MAP_MODE_COOL 3
#define MATTER_MAP_MODE_HEAT 4
#define MATTER_MAP_MODE_FAN_ONLY 7
#define MATTER_MAP_MODE_DRY 8

/* Thermostat ThermostatRunningState (RelayStateBitmap) */
#define MATTER_MAP_RUNNING_HEAT 0x0001u
#define MATTER_MAP_RUNNING_COOL 0x0002u
#define MATTER_MAP_RUNNING_FAN 0x0004u

/* Fan Control FanModeEnum */
#define MATTER_MAP_FAN_MODE_OFF 0
#define MATTER_MAP_FAN_MODE_LOW 1
#define MATTER_MAP_FAN_MODE_MEDIUM 2
#define MATTER_MAP_FAN_MODE_HIGH 3
#define MATTER_MAP_FAN_MODE_ON 4
#define MATTER_MAP_FAN_MODE_AUTO 5
#define MATTER_MAP_FAN_MODE_SMART 6

/**
 * What the AC is doing, for ThermostatRunningState. An IR link has no feedback, so it follows the
 * mode: a running AC in Cool or Dry counts as cooling (and its fan as running), Heat as heating, Fan
 * only as fan. Apple Home shows a Room Air Conditioner as a heater/cooler whose current state comes
 * from this attribute: without it the tile reads Off or Idle while the AC runs.
 */
uint16_t matter_map_running_state(uint8_t system_mode);

/** PercentSetting the Fan Control cluster derives from SpeedSetting `speed` (same integer formula). */
uint8_t matter_map_percent_of_speed(uint8_t speed, uint8_t speed_count);

/**
 * Current speed shown while the AC chooses the fan speed itself (fan Auto): the middle speed. Never 0
 * while the AC runs: a home app reads a running fan at 0 % as "off" and then shows the AC as off.
 */
uint8_t matter_map_auto_shown_speed(uint8_t speed_count);

/**
 * AC fan (ac_fan_t) for a FanMode / SpeedSetting / PercentSetting written by a home app.
 * speed_setting / percent_setting: -1 when null. speeds[n - 1] is the AC fan of Matter speed n
 * (slowest first), speed_count of them.
 * Off, 0 % and speed 0 select the AC's automatic fan: power belongs to On/Off and SystemMode, a fan
 * slider at its minimum (or a home app writing back a speed it does not know) never turns the AC off.
 */
uint8_t matter_map_fan_from_drive(uint8_t fan_mode, int speed_setting, int percent_setting, const uint8_t *speeds,
                                  uint8_t speed_count);

#ifdef __cplusplus
}
#endif
