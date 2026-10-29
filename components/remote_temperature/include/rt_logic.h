#pragma once

/* Pure helpers of the temperature path (host tested). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Room temperatures outside this range are treated as sensor faults for control purposes. */
#define RT_PLAUSIBLE_MIN_CENTI (-1000) /* -10 C */
#define RT_PLAUSIBLE_MAX_CENTI 6000    /*  60 C */

/** MeasuredValue (0.01 C) is usable: inside the sensor's own Min/MaxMeasuredValue and plausible. */
bool rt_value_plausible(int16_t centi, bool has_min, int16_t min_centi, bool has_max, int16_t max_centi);

/**
 * Stale timeout derived from the negotiated subscription MaxInterval: a publisher must report at
 * least once per MaxInterval, so silence for two intervals (+ margin) means the data is too old.
 */
uint32_t rt_stale_timeout_s(uint32_t negotiated_max_interval_s, uint32_t stale_min_s);

/** Exponential backoff with +-20 % jitter: min * 2^attempt capped at max. `rnd` is any random value. */
uint32_t rt_backoff_ms(uint32_t attempt, uint32_t min_ms, uint32_t max_ms, uint32_t rnd);

/** Sensirion CRC-8 (poly 0x31, init 0xFF). */
uint8_t sht_crc8(const uint8_t *data, size_t len);
/** SHT3x/SHT4x raw temperature to 0.01 C: T = -45 + 175 * raw / 65535. */
int16_t sht_raw_to_centi(uint16_t raw);

#ifdef __cplusplus
}
#endif
