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
    uint16_t fan_levels;        /* learned fan speeds (1 << ac_fan_t, AUTO excluded): Fan Control speeds */
    bool swing;                 /* swing learned: Fan Control Rocking feature */
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
    bool ble_commissioning;        /* built with CHIPoBLE: phones find the device over Bluetooth */
    bool ble_advertising;          /* CHIPoBLE advertising right now (commissioning window open) */
    char node_label[33];           /* Basic Information NodeLabel (may be renamed by a home app) */
    char ble_name[17];             /* BLE GAP device name */
} matter_status_t;

typedef struct {
    uint8_t fabric_index;
    uint16_t vendor_id;
    char label[33];
} matter_fabric_info_t;

/* Commissioning state changes for user feedback (status LED). */
typedef enum {
    MATTER_UI_WINDOW_OPENED = 0, /* pairing mode: the thermostat can be added with its QR/manual code */
    MATTER_UI_WINDOW_CLOSED,
    MATTER_UI_COMMISSIONED,      /* added to a home ecosystem */
    MATTER_UI_IDENTIFY_START,    /* a home app asks "which device is this?" (Identify cluster) */
    MATTER_UI_IDENTIFY_STOP,
    MATTER_UI_IDENTIFY_EFFECT,   /* one-shot identify effect (TriggerEffect) */
} matter_ui_event_t;

/** Called on the CHIP thread; must not block. Register before matter_thermostat_start(). */
typedef void (*matter_ui_event_cb_t)(matter_ui_event_t ev);
void matter_thermostat_set_ui_callback(matter_ui_event_cb_t cb);

/*
 * Fan of the AC (Matter Fan Control cluster on the thermostat endpoint): speed (learned fan levels),
 * Auto and swing (Rocking). Values are ac_fan_t / ac_swing_t (ac_types.h).
 * The callback runs on the CHIP thread when a home app changes the fan; it must not block.
 */
typedef void (*thermostat_fan_change_cb_t)(uint8_t fan, uint8_t swing);
void matter_thermostat_set_fan_callback(thermostat_fan_change_cb_t cb);

/** Fan/swing chosen on the web page or restored at boot: shown in the home apps, then passed to the callback. */
esp_err_t matter_thermostat_set_fan(uint8_t fan, uint8_t swing);

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

/**
 * The original IR remote changed the AC (the climate controller already follows it): show the new
 * mode/setpoints (mask TSTAT_CHANGED_*) and, when has_fan, fan/swing in the home apps WITHOUT calling
 * the user/fan callbacks, so nothing is transmitted back. Modes the thermostat does not offer are
 * skipped, setpoints are clamped to its limits. Any task, non blocking.
 */
esp_err_t matter_thermostat_show_remote_state(const thermostat_user_state_t *state, uint32_t mask, bool has_fan,
                                              uint8_t fan, uint8_t swing);

esp_err_t matter_thermostat_open_commissioning_window(uint16_t seconds);
esp_err_t matter_thermostat_close_commissioning_window(void);

/** QR payload ("MT:...") and 11 digit manual code. Secret material: only for the web portal/console. */
esp_err_t matter_thermostat_get_onboarding(char *qr, size_t qr_len, char *manual, size_t manual_len);

/** Up to max home fabrics (excluding Fabric B). Returns the count copied. */
size_t matter_thermostat_list_home_fabrics(matter_fabric_info_t *out, size_t max);

/**
 * Leave one home ecosystem / all of them on the device side (for a hub that is gone, or to hand the
 * device over): the fabric is deleted here exactly as its RemoveFabric command would do, the hub is
 * told with the Leave event when it is reachable. Fabric B (sensor), Wi-Fi, profiles and settings are
 * kept. When no home fabric is left the commissioning window opens again. Any task, non blocking.
 */
esp_err_t matter_thermostat_remove_home_fabric(uint8_t fabric_index);
esp_err_t matter_thermostat_remove_all_home_fabrics(void);

#ifdef __cplusplus
}
#endif
