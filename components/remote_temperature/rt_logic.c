#include "rt_logic.h"

bool rt_value_plausible(int16_t centi, bool has_min, int16_t min_centi, bool has_max, int16_t max_centi)
{
    if (centi < RT_PLAUSIBLE_MIN_CENTI || centi > RT_PLAUSIBLE_MAX_CENTI) {
        return false;
    }
    if (has_min && centi < min_centi) {
        return false;
    }
    if (has_max && centi > max_centi) {
        return false;
    }
    return true;
}

uint32_t rt_stale_timeout_s(uint32_t negotiated_max_interval_s, uint32_t stale_min_s)
{
    uint64_t t = 2ull * negotiated_max_interval_s + 60u;
    if (t < stale_min_s) {
        t = stale_min_s;
    }
    if (t > 7u * 24u * 3600u) {
        t = 7u * 24u * 3600u;
    }
    return (uint32_t)t;
}

uint32_t rt_backoff_ms(uint32_t attempt, uint32_t min_ms, uint32_t max_ms, uint32_t rnd)
{
    if (min_ms == 0) {
        min_ms = 1;
    }
    if (max_ms < min_ms) {
        max_ms = min_ms;
    }
    uint64_t base = min_ms;
    for (uint32_t i = 0; i < attempt && base < max_ms; i++) {
        base *= 2u;
    }
    if (base > max_ms) {
        base = max_ms;
    }
    /* jitter in [-20 %, +20 %] */
    uint64_t span = base * 40u / 100u;
    uint64_t jitter = span ? (rnd % (span + 1u)) : 0;
    uint64_t v = base - base * 20u / 100u + jitter;
    if (v < 1) {
        v = 1;
    }
    return (uint32_t)v;
}

uint8_t sht_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

int16_t sht_raw_to_centi(uint16_t raw)
{
    /* -4500 + 17500 * raw / 65535, rounded */
    int32_t v = -4500 + (int32_t)((17500LL * raw + 32767) / 65535);
    return (int16_t)v;
}
