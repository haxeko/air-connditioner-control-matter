#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ac_types.h"
#include "climate_algo.h"
#include "config_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool ready;                     /* profile usable for automatic control */
    char not_ready[64];
    /* user side (Matter / web) */
    uint8_t user_mode;
    int16_t cool_target_centi;
    int16_t heat_target_centi;
    uint8_t fan, swing;
    /* sensor side */
    bool temp_valid;
    bool has_filtered;
    int16_t filtered_centi;
    /* algorithm */
    uint8_t control_mode;           /* control_mode_t */
    bool compensating;
    int16_t offset_centi;
    int16_t ir_setpoint_centi;
    climate_reason_t reason;
    ac_state_t desired;
    /* transmission */
    bool has_sent;
    ac_state_t last_sent;           /* state actually encoded (nearest learned) */
    ac_resolve_kind_t last_kind;
    int64_t last_sent_us;
    uint32_t pending_ms;            /* debounce before the next send, 0 = none */
    char last_error[96];
    /* original remote (followed while the profile is locked and remote_sync_off == 0) */
    bool remote_enabled;            /* requested (locked profile, option on)          */
    bool remote_listening;          /* receiver armed right now                       */
    uint32_t remote_count;          /* commands of this AC applied                   */
    uint32_t remote_ignored;        /* received commands that are not of this AC     */
    bool has_remote;
    ac_state_t last_remote;         /* state set with the original remote            */
    int64_t last_remote_us;
    char remote_note[80];           /* why the last received command was ignored     */
} climate_status_t;

esp_err_t climate_control_start(const app_config_t *cfg);

/*
 * The original remote changed the AC: the user state below is already applied here (nothing is
 * transmitted for it); it only has to be shown in the home apps. Called on the climate task, must not
 * block. `mask` = CLIMATE_USER_MODE / _COOL_SP / _HEAT_SP; fan/swing are valid when has_fan.
 */
typedef struct {
    uint8_t user_mode;
    int16_t cool_target_centi;
    int16_t heat_target_centi;
    uint32_t mask;
    bool has_fan;
    uint8_t fan, swing;
} climate_remote_update_t;

typedef void (*climate_remote_cb_t)(const climate_remote_update_t *u);
/** Register before climate_control_start(). */
void climate_control_set_remote_callback(climate_remote_cb_t cb);

/* `mask` bits of climate_control_on_user_change() (same values as TSTAT_CHANGED_*). */
#define CLIMATE_USER_MODE (1u << 0)
#define CLIMATE_USER_COOL_SP (1u << 1)
#define CLIMATE_USER_HEAT_SP (1u << 2)
#define CLIMATE_USER_BOOT (1u << 7)

/** Thermostat attribute callback (CHIP thread). Non blocking. */
void climate_control_on_user_change(uint8_t mode, int16_t cool_centi, int16_t heat_centi, uint32_t mask);

/** New room temperature (valid=false when unknown/stale). Non blocking. */
void climate_control_post_temperature(bool valid, int16_t centi);

esp_err_t climate_control_set_fan_swing(uint8_t fan, uint8_t swing);
void climate_control_config_changed(const app_config_t *cfg);
void climate_control_profile_changed(void);
/** Transmit the current desired state again even if unchanged ("gửi lại"). */
esp_err_t climate_control_force_resend(void);

/**
 * Before a planned restart: transmit a change that is still waiting for its send delay and store the
 * state that is normally written a few seconds later, so that the restarted firmware knows what the AC
 * is doing. Blocks up to 2 x timeout_ms. Not from the CHIP task, not from the climate callbacks.
 */
void climate_control_flush(uint32_t timeout_ms);

void climate_control_get_status(climate_status_t *out);

#ifdef __cplusplus
}
#endif
