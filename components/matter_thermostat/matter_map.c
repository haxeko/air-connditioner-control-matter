#include "matter_map.h"

#include <stddef.h>

#include "ac_types.h"

uint16_t matter_map_running_state(uint8_t system_mode)
{
    switch (system_mode) {
    case MATTER_MAP_MODE_COOL:
    case MATTER_MAP_MODE_DRY:
        return MATTER_MAP_RUNNING_COOL | MATTER_MAP_RUNNING_FAN;
    case MATTER_MAP_MODE_HEAT:
        return MATTER_MAP_RUNNING_HEAT | MATTER_MAP_RUNNING_FAN;
    case MATTER_MAP_MODE_FAN_ONLY:
        return MATTER_MAP_RUNNING_FAN;
    default:
        return 0;
    }
}

uint8_t matter_map_percent_of_speed(uint8_t speed, uint8_t speed_count)
{
    if (speed_count == 0) {
        return 0;
    }
    if (speed > speed_count) {
        speed = speed_count;
    }
    return (uint8_t)((speed * 100u) / speed_count);
}

uint8_t matter_map_auto_shown_speed(uint8_t speed_count)
{
    return (uint8_t)((speed_count + 1u) / 2u);
}

uint8_t matter_map_fan_from_drive(uint8_t fan_mode, int speed_setting, int percent_setting, const uint8_t *speeds,
                                  uint8_t speed_count)
{
    if (fan_mode == MATTER_MAP_FAN_MODE_AUTO || fan_mode == MATTER_MAP_FAN_MODE_OFF || speeds == NULL ||
        speed_count == 0) {
        return AC_FAN_AUTO;
    }
    int speed = 0;
    if (speed_setting >= 0) {
        speed = speed_setting;
    } else if (percent_setting >= 0) {
        speed = (speed_count * percent_setting + 99) / 100;
    }
    if (speed <= 0) {
        return AC_FAN_AUTO;
    }
    if (speed > speed_count) {
        speed = speed_count;
    }
    return speeds[speed - 1];
}
