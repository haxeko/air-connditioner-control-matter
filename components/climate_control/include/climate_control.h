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
} climate_status_t;

esp_err_t climate_control_start(const app_config_t *cfg);

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

void climate_control_get_status(climate_status_t *out);

#ifdef __cplusplus
}
#endif
