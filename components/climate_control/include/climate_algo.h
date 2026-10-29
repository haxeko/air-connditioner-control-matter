#pragma once

/*
 * Setpoint compensation algorithm (pure C, host tested).
 *
 * The AC regulates on its own internal sensor; this device cannot override that (no Follow-Me /
 * I-Feel is assumed). In EXTERNAL mode it shifts the IR setpoint so that the room - as measured by
 * the external sensor - approaches the user's target:
 *   - median-of-3 + exponential low-pass filter (time based) on the room temperature,
 *   - no action while |room - target| <= deadband,
 *   - one AC step per correction, at most one correction per comp_interval,
 *   - |offset| <= max_offset, offset re-clamped when the AC limits cut it (anti-windup),
 *   - no compensation (offset = 0, i.e. DIRECT behaviour) while the sensor value is not valid/fresh,
 *   - power is never changed by the algorithm: OFF from the user always wins and nothing turns
 *     the AC back on except the user.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ac_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* tstat_mode_t values (Matter SystemModeEnum) handled by the algorithm. */
#define CLIMATE_MODE_OFF 0
#define CLIMATE_MODE_COOL 3
#define CLIMATE_MODE_HEAT 4
#define CLIMATE_MODE_FAN_ONLY 7
#define CLIMATE_MODE_DRY 8

typedef struct {
    uint8_t external;          /* 0 = direct, 1 = external sensor control */
    int16_t deadband_centi;
    int16_t max_offset_centi;
    uint32_t comp_interval_ms;
    uint32_t filter_tau_ms;
    uint8_t temp_step_half;    /* 1 = 0.5 C, 2 = 1 C */
    uint8_t min_half[AC_MODE_COUNT];
    uint8_t max_half[AC_MODE_COUNT];
} climate_params_t;

typedef struct {
    bool has_filtered;
    float filtered_centi;
    int64_t last_sample_ms;
    int16_t window[3];
    uint8_t window_n;
    uint8_t window_pos;

    int16_t offset_centi;
    int64_t last_comp_ms;
    bool comp_active;
    uint8_t last_ac_mode;      /* offset is reset when the AC mode changes */
} climate_algo_t;

typedef struct {
    uint8_t user_mode;          /* CLIMATE_MODE_* */
    int16_t cool_target_centi;
    int16_t heat_target_centi;
    uint8_t fan;                /* ac_fan_t chosen by the user (web)  */
    uint8_t swing;              /* ac_swing_t                         */
    bool temp_valid;            /* valid and fresh room temperature   */
    int64_t now_ms;
} climate_inputs_t;

typedef enum {
    CLIMATE_REASON_OFF = 0,
    CLIMATE_REASON_DIRECT,
    CLIMATE_REASON_NO_SENSOR,      /* external mode but no valid temperature: direct behaviour */
    CLIMATE_REASON_IN_DEADBAND,
    CLIMATE_REASON_WAITING,        /* correction needed but comp_interval not elapsed            */
    CLIMATE_REASON_CORRECTED,      /* a correction step was applied now                          */
    CLIMATE_REASON_AT_LIMIT,       /* correction needed but offset/AC limit reached              */
    CLIMATE_REASON_NO_COMPENSATION /* mode without setpoint compensation (dry/fan)               */
} climate_reason_t;

typedef struct {
    ac_state_t desired;
    uint8_t user_target_half;
    int16_t target_centi;
    int16_t ir_setpoint_centi;
    int16_t offset_centi;
    bool compensating;
    climate_reason_t reason;
} climate_output_t;

void climate_algo_init(climate_algo_t *a);
/** Feed one valid room temperature sample. */
void climate_algo_feed(climate_algo_t *a, const climate_params_t *p, int16_t value_centi, int64_t now_ms);
/** Forget filter history (sensor changed or was unavailable for a long time). */
void climate_algo_reset_filter(climate_algo_t *a);
/** The user changed the target: give the room one interval before the next correction. */
void climate_algo_note_user_change(climate_algo_t *a, int64_t now_ms);
void climate_algo_step(climate_algo_t *a, const climate_params_t *p, const climate_inputs_t *in, climate_output_t *out);

/** Round a temperature to the AC step (ties: up for cooling, down for heating). */
int16_t climate_quantize(int16_t centi, uint8_t step_half, bool heating);

const char *climate_reason_text(climate_reason_t r);

#ifdef __cplusplus
}
#endif
