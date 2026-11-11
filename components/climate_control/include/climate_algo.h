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
/** The original remote set the AC directly: no correction is applied to it, the next one starts from
 *  zero after one interval. */
void climate_algo_note_remote(climate_algo_t *a, int64_t now_ms);
void climate_algo_step(climate_algo_t *a, const climate_params_t *p, const climate_inputs_t *in, climate_output_t *out);

/* ---- State set with the original remote -> user state of the thermostat ---- */

/* What the thermostat offers (Off and Cool always). */
typedef struct {
    bool heat, dry, fan_only, swing;
    int16_t cool_min, cool_max;
    int16_t heat_min, heat_max;
} climate_offer_t;

typedef struct {
    uint8_t user_mode;          /* CLIMATE_MODE_* */
    int16_t cool_target_centi;
    int16_t heat_target_centi;
    uint8_t fan;                /* ac_fan_t   */
    uint8_t swing;              /* ac_swing_t */
} climate_user_t;

typedef struct {
    bool mode_shown;            /* the AC mode has a thermostat mode (not Auto / not offered) */
    bool cool_sp, heat_sp;      /* setpoint taken from the remote                             */
    bool fan;                   /* fan (and swing when offered) taken from the remote         */
} climate_remote_result_t;

/**
 * Apply a state set with the original remote to `user`. Off -> Off. A mode without thermostat mode
 * keeps the shown mode (Cool if it was Off: the AC is on); only power, fan and swing follow then. In
 * Cool/Heat the setpoint is the remote's temperature clamped to the thermostat limits. Dry keeps the
 * cooling setpoint: many ACs (e.g. Sharp) send a fixed temperature in Dry, which must not become the
 * setpoint of the next Cool.
 */
void climate_user_from_remote(const ac_state_t *remote, const climate_offer_t *offer, climate_user_t *user,
                              climate_remote_result_t *res);

/* ---- What is known about the AC, and whether a command has to go out ----
 *
 * An IR link is one way: the state of the AC is never read back.
 *   ref     the desired state for which nothing more has to be sent: the last command transmitted, the
 *           state set with the original remote or - when nothing is known (first boot, new profile,
 *           thermostat changed while the device was off) - the thermostat state itself;
 *   actual  the state the AC was really put in (transmitted or read from the remote), the "previous
 *           state" of protocols that toggle settings. Absent while it is only assumed.
 * Nothing is ever transmitted because the state is unknown: a command goes out only when the desired
 * state changes afterwards (user, compensation) or when a re-send is forced.
 */
typedef struct {
    bool user_known;   /* the thermostat reported its restored state; nothing is sent before that */
    bool has_ref;
    bool has_actual;
    ac_state_t ref;
    ac_state_t actual;
} climate_link_t;

void climate_link_init(climate_link_t *l);
/** State the AC was last put in, from persistent storage (before the thermostat reports). */
void climate_link_restore(climate_link_t *l, bool has_actual, const ac_state_t *actual);
/** The thermostat reported its state at boot and asks for `desired`. Never causes a transmission:
 *  a missing reference, or one that does not match, is replaced by `desired`. */
void climate_link_boot(climate_link_t *l, const ac_state_t *desired);
/** The state of the AC became unknown (another profile): assume `desired`, forget `actual`. */
void climate_link_assume(climate_link_t *l, const ac_state_t *desired);
/** The AC was put in `actual` for `desired` (command transmitted, or original remote observed). */
void climate_link_confirm(climate_link_t *l, const ac_state_t *desired, const ac_state_t *actual);
/** `desired` cannot be transmitted (no command for it): do not try again until it changes. */
void climate_link_skip(climate_link_t *l, const ac_state_t *desired);
/** True if a command for `desired` has to be transmitted (`force`: explicit re-send). */
bool climate_link_needs_send(const climate_link_t *l, const ac_state_t *desired, bool force);
/** State the AC is really in, for protocols that toggle settings; NULL when it is not known. */
const ac_state_t *climate_link_prev(const climate_link_t *l);

/** Round a temperature to the AC step (ties: up for cooling, down for heating). */
int16_t climate_quantize(int16_t centi, uint8_t step_half, bool heating);

const char *climate_reason_text(climate_reason_t r);

#ifdef __cplusplus
}
#endif
