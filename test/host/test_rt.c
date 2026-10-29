#include <stdbool.h>
#include <stdint.h>

#include "rt_logic.h"
#include "test.h"

void test_rt_logic(void)
{
    /* stale timeout follows the negotiated reporting interval, never below the configured minimum */
    CHECK_EQ(rt_stale_timeout_s(600, 900), 1260);
    CHECK_EQ(rt_stale_timeout_s(3600, 900), 7260); /* sleepy sensor reporting hourly */
    CHECK_EQ(rt_stale_timeout_s(0, 900), 900);
    CHECK_EQ(rt_stale_timeout_s(400000, 900), 7u * 24u * 3600u);

    /* exponential backoff with +-20 % jitter, capped */
    CHECK_EQ(rt_backoff_ms(0, 5000, 300000, 0), 4000);
    CHECK_EQ(rt_backoff_ms(0, 5000, 300000, 2000), 6000);
    CHECK_EQ(rt_backoff_ms(1, 5000, 300000, 0), 8000);
    uint32_t prev = 0;
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t lo = rt_backoff_ms(i, 5000, 300000, 0);
        uint32_t hi = rt_backoff_ms(i, 5000, 300000, UINT32_MAX);
        CHECK(lo >= prev);
        CHECK(hi <= 360000);
        CHECK(lo <= hi);
        prev = lo;
    }
    CHECK_EQ(rt_backoff_ms(40, 5000, 300000, 0), 240000);
    CHECK(rt_backoff_ms(0, 0, 0, 0) >= 1);

    /* Sensirion CRC-8 example from the SHT3x/SHT4x datasheets: 0xBEEF -> 0x92 */
    const uint8_t beef[] = { 0xBE, 0xEF };
    CHECK_EQ(sht_crc8(beef, 2), 0x92);
    CHECK_EQ(sht_raw_to_centi(0), -4500);
    CHECK_EQ(sht_raw_to_centi(65535), 13000);
    CHECK_EQ(sht_raw_to_centi(26214), 2500);

    /* plausibility: generic room range and the sensor's own Min/MaxMeasuredValue */
    CHECK(rt_value_plausible(2500, false, 0, false, 0));
    CHECK(!rt_value_plausible(-1001, false, 0, false, 0));
    CHECK(!rt_value_plausible(6001, false, 0, false, 0));
    CHECK(!rt_value_plausible(-100, true, 0, false, 0));
    CHECK(!rt_value_plausible(5000, false, 0, true, 4000));
    CHECK(rt_value_plausible(4000, true, 0, true, 4000));
}
