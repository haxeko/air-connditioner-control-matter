#include "matter_map.h"

#include "ac_types.h"
#include "test.h"

static void test_running_state(void)
{
    CHECK_EQ(matter_map_running_state(MATTER_MAP_MODE_OFF), 0);
    CHECK_EQ(matter_map_running_state(MATTER_MAP_MODE_COOL), MATTER_MAP_RUNNING_COOL | MATTER_MAP_RUNNING_FAN);
    CHECK_EQ(matter_map_running_state(MATTER_MAP_MODE_DRY), MATTER_MAP_RUNNING_COOL | MATTER_MAP_RUNNING_FAN);
    CHECK_EQ(matter_map_running_state(MATTER_MAP_MODE_HEAT), MATTER_MAP_RUNNING_HEAT | MATTER_MAP_RUNNING_FAN);
    CHECK_EQ(matter_map_running_state(MATTER_MAP_MODE_FAN_ONLY), MATTER_MAP_RUNNING_FAN);
    CHECK_EQ(matter_map_running_state(1 /* Auto: not offered */), 0);
}

static void test_speeds(void)
{
    /* same as FanControlCluster::ApplySpeedSettingChanged(): speed * 100 / SpeedMax */
    CHECK_EQ(matter_map_percent_of_speed(1, 3), 33);
    CHECK_EQ(matter_map_percent_of_speed(2, 3), 66);
    CHECK_EQ(matter_map_percent_of_speed(3, 3), 100);
    CHECK_EQ(matter_map_percent_of_speed(5, 3), 100);
    CHECK_EQ(matter_map_percent_of_speed(1, 0), 0);
    /* the fan of a running AC in Auto is never shown at 0 */
    for (uint8_t n = 1; n <= 7; n++) {
        uint8_t s = matter_map_auto_shown_speed(n);
        CHECK(s >= 1 && s <= n);
        CHECK(matter_map_percent_of_speed(s, n) > 0);
    }
    CHECK_EQ(matter_map_auto_shown_speed(1), 1);
    CHECK_EQ(matter_map_auto_shown_speed(3), 2);
    CHECK_EQ(matter_map_auto_shown_speed(4), 2);
    CHECK_EQ(matter_map_auto_shown_speed(5), 3);
}

static void test_fan_from_drive(void)
{
    const uint8_t sp3[] = { AC_FAN_1, AC_FAN_2, AC_FAN_3 };
    /* Off, 0 % and speed 0 are the automatic fan, never "AC off" */
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_OFF, 0, 0, sp3, 3), AC_FAN_AUTO);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_LOW, 0, -1, sp3, 3), AC_FAN_AUTO);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, -1, 0, sp3, 3), AC_FAN_AUTO);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_AUTO, -1, -1, sp3, 3), AC_FAN_AUTO);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_LOW, -1, -1, sp3, 3), AC_FAN_AUTO);
    /* speeds as the cluster derives them for FanMode Low / Medium / High */
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_LOW, 1, 33, sp3, 3), AC_FAN_1);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_MEDIUM, 2, 66, sp3, 3), AC_FAN_2);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, 3, 100, sp3, 3), AC_FAN_3);
    /* SpeedSetting wins; percent only when the speed is null */
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_MEDIUM, 1, 90, sp3, 3), AC_FAN_1);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_MEDIUM, -1, 50, sp3, 3), AC_FAN_2);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, -1, 1, sp3, 3), AC_FAN_1);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, -1, 100, sp3, 3), AC_FAN_3);
    /* out of range speed is clamped */
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, 9, -1, sp3, 3), AC_FAN_3);
    /* learned levels that are not contiguous: Matter speed n is the n-th learned level */
    const uint8_t sp2[] = { AC_FAN_QUIET, AC_FAN_TURBO };
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_LOW, 1, 50, sp2, 2), AC_FAN_QUIET);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, 2, 100, sp2, 2), AC_FAN_TURBO);
    /* without learned levels there is only the automatic fan */
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, 3, 100, sp3, 0), AC_FAN_AUTO);
    CHECK_EQ(matter_map_fan_from_drive(MATTER_MAP_FAN_MODE_HIGH, 3, 100, NULL, 3), AC_FAN_AUTO);
}

void test_matter_map(void)
{
    test_running_state();
    test_speeds();
    test_fan_from_drive();
}
