#include <stdbool.h>
#include <string.h>

#include "ac_types.h"
#include "test.h"

static ac_state_t S(uint8_t power, uint8_t mode, uint8_t temp_half, uint8_t fan, uint8_t swing)
{
    ac_state_t s = { power, mode, temp_half, fan, swing };
    return s;
}

static void test_keys(void)
{
    char key[16];
    ac_state_t back;
    int n = 0;
    for (uint8_t m = 0; m < AC_MODE_COUNT; m++) {
        for (uint8_t t = AC_TEMP_HALF_MIN; t <= AC_TEMP_HALF_MAX; t++) {
            for (uint8_t f = 0; f < AC_FAN_COUNT; f++) {
                for (uint8_t w = 0; w < AC_SWING_COUNT; w++) {
                    ac_state_t s = S(1, m, m == AC_MODE_FAN ? AC_TEMP_NONE : t, f, w);
                    CHECK(ac_state_valid(&s));
                    CHECK(ac_state_key(&s, key, sizeof(key)));
                    CHECK(strlen(key) < 15); /* NVS key limit */
                    CHECK(ac_state_from_key(key, &back));
                    CHECK(ac_state_equal(&s, &back));
                    char desc[64];
                    ac_state_describe(&s, desc, sizeof(desc));
                    CHECK(strlen(desc) <= 60); /* fits the 64 byte label buffers untruncated */
                    n++;
                }
            }
        }
    }
    CHECK(n > 1000);
    ac_state_t off = S(0, 3, 99, 7, 1); /* fields ignored when off */
    CHECK(ac_state_valid(&off));
    CHECK(ac_state_key(&off, key, sizeof(key)));
    CHECK_STR(key, "off");
    CHECK(ac_state_from_key("off", &back) && back.power == 0);

    const char *bad[] = { "", "OFF", "m5t50f0s0", "m0t19f0s0", "m0t71f0s0", "m3t50f0s0", "m0t50f8s0",
                          "m0t50f0s2", "m0t50f0s0x", "m0t50f0", "x0t50f0s0", "m0t300f0s0" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!ac_state_from_key(bad[i], &back));
    }
    ac_state_t fan_with_temp = S(1, AC_MODE_FAN, 50, 0, 0);
    CHECK(!ac_state_valid(&fan_with_temp));
    ac_state_t cool_without_temp = S(1, AC_MODE_COOL, AC_TEMP_NONE, 0, 0);
    CHECK(!ac_state_valid(&cool_without_temp));
    CHECK(!ac_state_key(&cool_without_temp, key, sizeof(key)));

    char desc[64];
    ac_state_t c245 = S(1, AC_MODE_COOL, 49, AC_FAN_AUTO, AC_SWING_OFF);
    ac_state_describe(&c245, desc, sizeof(desc));
    CHECK_STR(desc, "Lạnh 24,5°C, quạt tự động, đảo gió tắt");
    ac_state_describe(&off, desc, sizeof(desc));
    CHECK_STR(desc, "Tắt");
}

static void test_caps(void)
{
    const ac_state_t list[] = { S(0, 0, 0, 0, 0), S(1, AC_MODE_COOL, 48, 0, 0), S(1, AC_MODE_COOL, 56, 2, 1),
                                S(1, AC_MODE_HEAT, 40, 0, 0), S(1, AC_MODE_FAN, AC_TEMP_NONE, 3, 0),
                                S(1, AC_MODE_COOL, 19, 0, 0) /* invalid: ignored */ };
    ac_caps_t caps;
    ac_caps_from_list(list, 6, 1, &caps);
    CHECK(caps.has_off);
    CHECK_EQ(caps.code_count, 5);
    CHECK_EQ(caps.modes_mask, (1 << AC_MODE_COOL) | (1 << AC_MODE_HEAT) | (1 << AC_MODE_FAN));
    CHECK_EQ(caps.temp_min_half[AC_MODE_COOL], 48);
    CHECK_EQ(caps.temp_max_half[AC_MODE_COOL], 56);
    CHECK_EQ(caps.temp_min_half[AC_MODE_HEAT], 40);
    CHECK_EQ(caps.temp_max_half[AC_MODE_FAN], 0);
    CHECK_EQ(caps.fans_mask, (1 << 0) | (1 << 2) | (1 << 3));
    CHECK_EQ(caps.swing_mask, 3);
    CHECK_EQ(caps.temp_step_half, 1);
}

static void test_resolve(void)
{
    const ac_state_t list[] = {
        S(0, 0, 0, 0, 0),                               /* off */
        S(1, AC_MODE_COOL, 48, AC_FAN_AUTO, 0),         /* cool 24 */
        S(1, AC_MODE_COOL, 52, AC_FAN_AUTO, 0),         /* cool 26 */
        S(1, AC_MODE_COOL, 50, AC_FAN_3, 0),            /* cool 25, fan 3 only */
        S(1, AC_MODE_HEAT, 44, AC_FAN_AUTO, 0),         /* heat 22 */
        S(1, AC_MODE_FAN, AC_TEMP_NONE, AC_FAN_2, 0),   /* fan only */
    };
    const size_t n = sizeof(list) / sizeof(list[0]);
    ac_state_t chosen;
    ac_state_t d;

    d = S(0, 0, 0, 0, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 0, &chosen), AC_RESOLVE_EXACT);
    CHECK_EQ(chosen.power, 0);
    CHECK_EQ(ac_resolve(list + 1, n - 1, AC_FAN_AUTO, 0, &d, 0, &chosen), AC_RESOLVE_NONE); /* Off not learned */

    d = S(1, AC_MODE_COOL, 52, AC_FAN_AUTO, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 52, &chosen), AC_RESOLVE_EXACT);
    CHECK_EQ(chosen.temp_half, 52);

    /* temperature first: 25 C is only learned with fan 3 -> fan falls back, temperature stays */
    d = S(1, AC_MODE_COOL, 50, AC_FAN_AUTO, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 50, &chosen), AC_RESOLVE_FALLBACK_FAN_SWING);
    CHECK_EQ(chosen.temp_half, 50);
    CHECK_EQ(chosen.fan, AC_FAN_3);

    /* nearest learned temperature with the requested fan */
    d = S(1, AC_MODE_COOL, 56, AC_FAN_AUTO, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 56, &chosen), AC_RESOLVE_NEAREST_TEMP);
    CHECK_EQ(chosen.temp_half, 52);

    /* tie 24/26 for 25: closer to the user's target, then the energy saving side */
    const ac_state_t two[] = { S(1, AC_MODE_COOL, 48, 0, 0), S(1, AC_MODE_COOL, 52, 0, 0) };
    d = S(1, AC_MODE_COOL, 50, 0, 0);
    CHECK_EQ(ac_resolve(two, 2, 0, 0, &d, 50, &chosen), AC_RESOLVE_NEAREST_TEMP);
    CHECK_EQ(chosen.temp_half, 52); /* cooling: warmer */
    CHECK_EQ(ac_resolve(two, 2, 0, 0, &d, 48, &chosen), AC_RESOLVE_NEAREST_TEMP);
    CHECK_EQ(chosen.temp_half, 48); /* user target 24 */
    const ac_state_t heat2[] = { S(1, AC_MODE_HEAT, 40, 0, 0), S(1, AC_MODE_HEAT, 48, 0, 0) };
    d = S(1, AC_MODE_HEAT, 44, 0, 0);
    CHECK_EQ(ac_resolve(heat2, 2, 0, 0, &d, 44, &chosen), AC_RESOLVE_NEAREST_TEMP);
    CHECK_EQ(chosen.temp_half, 40); /* heating: cooler */

    /* never another mode */
    d = S(1, AC_MODE_DRY, 50, 0, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 50, &chosen), AC_RESOLVE_NONE);
    d = S(1, AC_MODE_HEAT, 42, 0, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 42, &chosen), AC_RESOLVE_NEAREST_TEMP);
    CHECK_EQ(chosen.mode, AC_MODE_HEAT);
    CHECK_EQ(chosen.temp_half, 44);

    /* fan only mode has no temperature */
    d = S(1, AC_MODE_FAN, AC_TEMP_NONE, AC_FAN_2, 0);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 0, &chosen), AC_RESOLVE_EXACT);
    d = S(1, AC_MODE_FAN, AC_TEMP_NONE, AC_FAN_5, 1);
    CHECK_EQ(ac_resolve(list, n, AC_FAN_AUTO, 0, &d, 0, &chosen), AC_RESOLVE_FALLBACK_FAN_SWING);
    CHECK_EQ(chosen.fan, AC_FAN_2);

    /* fan/swing preference order: requested > requested fan + default swing > default fan + requested swing */
    const ac_state_t fs[] = { S(1, AC_MODE_COOL, 50, AC_FAN_AUTO, 0), S(1, AC_MODE_COOL, 50, AC_FAN_1, 0),
                              S(1, AC_MODE_COOL, 50, AC_FAN_AUTO, 1) };
    d = S(1, AC_MODE_COOL, 50, AC_FAN_1, 1);
    CHECK_EQ(ac_resolve(fs, 3, AC_FAN_AUTO, 0, &d, 50, &chosen), AC_RESOLVE_FALLBACK_FAN_SWING);
    CHECK(chosen.fan == AC_FAN_1 && chosen.swing == 0);
}

void test_ac_types(void)
{
    test_keys();
    test_caps();
    test_resolve();
}
