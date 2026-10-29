#pragma once

/*
 * Room temperature acquisition.
 *
 * Primary source: a Matter Temperature Measurement endpoint (cluster 0x0402, MeasuredValue 0x0000,
 * 0.01 degC, nullable) on a sensor commissioned into Fabric B. The sensor is identified by
 * Fabric B + operational Node ID + endpoint (never by IP: the address is resolved with DNS-SD,
 * so Thread sensors behind a Border Router work as long as IPv6 routing exists).
 *
 * - Subscribe (not polling); re-subscription is driven by the CHIP ReadClient with this module's
 *   exponential backoff, also after reboot, Wi-Fi loss or session errors.
 * - Freshness = last report OR keep-alive (empty report) on the subscription: an unchanged
 *   temperature is not a lost sensor.
 * - Stale timeout = max(stale_min, 2 x negotiated MaxInterval + 60 s): then the value is reported
 *   as unavailable (LocalTemperature null) and compensation stops. No value is ever invented or held
 *   forever.
 * - Optional fallback: a wired local sensor (SHT4x/SHT3x) when allowed by configuration.
 */

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TEMP_SRC_NONE = 0,
    TEMP_SRC_MATTER,
    TEMP_SRC_LOCAL,
    TEMP_SRC_SIMULATED, /* only with CONFIG_APP_TEST_SIMULATED_SENSOR */
} temp_source_t;

typedef enum {
    RT_UNBOUND = 0,
    RT_WAITING_COMMISSIONER, /* Fabric B not ready yet */
    RT_CONNECTING,           /* CASE + subscribe in progress */
    RT_SUBSCRIBED,
    RT_RETRYING,             /* waiting for the next (re)subscription attempt */
    RT_ERROR,                /* binding unusable (e.g. Fabric B was recreated) */
} rt_state_t;

typedef struct {
    uint16_t version;
    uint16_t endpoint;
    uint64_t node_id;
    uint64_t fabric_id;
    uint64_t compressed_fabric_id;
    uint16_t vendor_id, product_id;
    char vendor_name[33];
    char product_name[33];
    char label[33];
    uint32_t bound_at_s;
} sensor_binding_t;

typedef struct {
    bool bound;
    sensor_binding_t binding;
    rt_state_t state;
    char error[96];
    /* Matter value */
    bool has_value;
    bool value_null;            /* sensor reported null (cannot measure) */
    bool value_plausible;
    int16_t value_centi;
    int64_t value_time_us;      /* last report carrying a value */
    int64_t alive_time_us;      /* last report or keep-alive */
    bool has_bounds_min, has_bounds_max;
    int16_t min_centi, max_centi;
    uint16_t min_interval_s, max_interval_s;
    uint32_t stale_timeout_s;
    bool stale;
    uint32_t retries;
    uint32_t next_retry_ms;
    /* local sensor */
    bool local_configured;
    bool local_valid;
    int16_t local_centi;
    int64_t local_time_us;
    /* effective value published to Matter and the climate controller */
    temp_source_t source;
    bool effective_valid;
    int16_t effective_centi;
    bool simulated;
} rt_status_t;

/** Called for every published sample/transition (from the CHIP thread or a sensor task). */
typedef void (*rt_sink_cb_t)(bool valid, int16_t centi, temp_source_t source);

esp_err_t remote_temperature_init(const app_config_t *cfg, rt_sink_cb_t sink);
/** Must be called after the commissioner is ready (CHIP thread not required). */
esp_err_t remote_temperature_start(void);

esp_err_t remote_temperature_bind(uint64_t node_id, uint16_t endpoint, uint16_t vendor_id, uint16_t product_id,
                                  const char *vendor_name, const char *product_name, const char *label);
esp_err_t remote_temperature_unbind(void);
esp_err_t remote_temperature_retry_now(void);
void remote_temperature_config_changed(const app_config_t *cfg);
void remote_temperature_get_status(rt_status_t *out);
bool remote_temperature_get_binding(sensor_binding_t *out);

/** Test mode only (CONFIG_APP_TEST_SIMULATED_SENSOR), otherwise ESP_ERR_NOT_SUPPORTED. */
esp_err_t remote_temperature_set_simulated(bool enable, int16_t centi);

const char *rt_state_name(rt_state_t s);
const char *temp_source_name(temp_source_t s);

#ifdef __cplusplus
}
#endif
