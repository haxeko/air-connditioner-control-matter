#pragma once

/*
 * Matter server side: node with one Thermostat endpoint (device type 0x0301).
 *
 * - Off and Cool are always supported; Heat (and the HEAT feature + CoolingAndHeating control
 *   sequence) only when the active AC profile supports it. Dry / FanOnly SystemMode values are
 *   accepted only if the profile can transmit them.
 * - LocalTemperature is the external sensor value (null while no valid, fresh value exists).
 * - No ThermostatRunningState / PI demand attributes: an IR remote is one-way, the compressor state is
 *   not measured.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values of the Matter Thermostat SystemModeEnum used by this device. */
typedef enum {
    TSTAT_MODE_OFF = 0,
    TSTAT_MODE_COOL = 3,
    TSTAT_MODE_HEAT = 4,
    TSTAT_MODE_FAN_ONLY = 7,
    TSTAT_MODE_DRY = 8,
} tstat_mode_t;

typedef struct {
    bool heat;
    bool dry;
    bool fan_only;
    int16_t cool_min, cool_max; /* 0.01 degC */
    int16_t heat_min, heat_max; /* 0.01 degC, ignored when !heat */
} thermostat_caps_t;

typedef struct {
    uint8_t system_mode;      /* tstat_mode_t */
    int16_t cool_setpoint;    /* 0.01 degC */
    int16_t heat_setpoint;    /* 0.01 degC */
} thermostat_user_state_t;

#define TSTAT_CHANGED_MODE (1u << 0)
#define TSTAT_CHANGED_COOL_SP (1u << 1)
#define TSTAT_CHANGED_HEAT_SP (1u << 2)
#define TSTAT_CHANGED_BOOT (1u << 7) /* initial state restored from storage */

/** Called on the CHIP thread for every user change (hub, web). Must not block. */
typedef void (*thermostat_user_change_cb_t)(const thermostat_user_state_t *state, uint32_t changed_mask);

typedef struct {
    bool started;
    bool commissioner_ready;
    uint8_t home_fabric_count;     /* fabrics other than this device's own Fabric B */
    bool window_open;
    uint16_t endpoint_id;
    bool heat_feature;
    uint16_t vendor_id, product_id;
    uint16_t discriminator;
} matter_status_t;

typedef struct {
    uint8_t fabric_index;
    uint16_t vendor_id;
    char label[33];
} matter_fabric_info_t;

/** Create node + thermostat endpoint. Call once before matter_thermostat_start(). */
esp_err_t matter_thermostat_create(const thermostat_caps_t *caps, thermostat_user_change_cb_t cb);

/**
 * Start Matter (esp_matter::start) and, on the CHIP thread right after the server init,
 * the Fabric B commissioner. Opens the commissioning window if no home fabric exists.
 */
esp_err_t matter_thermostat_start(void);

void matter_thermostat_get_status(matter_status_t *out);
void matter_thermostat_get_user_state(thermostat_user_state_t *out);
void matter_thermostat_get_caps(thermostat_caps_t *out);

/** Publish the ambient temperature (valid=false -> LocalTemperature = null). Any task. */
esp_err_t matter_thermostat_set_local_temperature(bool valid, int16_t centi_c);

/** Change mode/setpoints locally (web UI); goes through the same validation as hub writes. */
esp_err_t matter_thermostat_set_user_state(const thermostat_user_state_t *state, uint32_t mask);

esp_err_t matter_thermostat_open_commissioning_window(uint16_t seconds);
esp_err_t matter_thermostat_close_commissioning_window(void);

/** QR payload ("MT:...") and 11 digit manual code. Secret material: only for the web portal/console. */
esp_err_t matter_thermostat_get_onboarding(char *qr, size_t qr_len, char *manual, size_t manual_len);

/** Up to max home fabrics (excluding Fabric B). Returns the count copied. */
size_t matter_thermostat_list_home_fabrics(matter_fabric_info_t *out, size_t max);

#ifdef __cplusplus
}
#endif
