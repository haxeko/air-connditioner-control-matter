#pragma once

/* Optional wired room sensor (Sensirion SHT4x / SHT3x on I2C) used as fallback source. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*local_sensor_cb_t)(bool valid, int16_t centi);

/** Starts a sampling task when a local sensor is configured; otherwise returns ESP_ERR_NOT_SUPPORTED. */
esp_err_t local_sensor_start(local_sensor_cb_t cb);
bool local_sensor_configured(void);
const char *local_sensor_name(void);

#ifdef __cplusplus
}
#endif
