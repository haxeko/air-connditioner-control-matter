#pragma once

/* Full-state model of an air conditioner remote (pure C, host tested). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AC_MODE_COOL = 0,
    AC_MODE_HEAT,
    AC_MODE_DRY,
    AC_MODE_FAN,
    AC_MODE_AUTO,
    AC_MODE_COUNT
} ac_mode_t;

typedef enum {
    AC_FAN_AUTO = 0,
    AC_FAN_1, /* lowest */
    AC_FAN_2,
    AC_FAN_3,
    AC_FAN_4,
    AC_FAN_5,
    AC_FAN_QUIET,
    AC_FAN_TURBO,
    AC_FAN_COUNT
} ac_fan_t;

typedef enum {
    AC_SWING_OFF = 0,
    AC_SWING_ON,
    AC_SWING_COUNT
} ac_swing_t;

/* Temperatures are kept in half degrees Celsius (2 * C) because remotes use 1 C or 0.5 C steps. */
#define AC_TEMP_HALF_MIN 20  /* 10 C */
#define AC_TEMP_HALF_MAX 70  /* 35 C */
#define AC_TEMP_NONE 0       /* mode without a temperature (fan only) */

typedef struct {
    uint8_t power;      /* 0 = off (all other fields ignored), 1 = on */
    uint8_t mode;       /* ac_mode_t */
    uint8_t temp_half;  /* 2 * degC, AC_TEMP_NONE for fan mode */
    uint8_t fan;        /* ac_fan_t */
    uint8_t swing;      /* ac_swing_t */
} ac_state_t;

typedef struct {
    bool has_off;
    uint8_t modes_mask;                       /* bit (1 << ac_mode_t) */
    uint8_t temp_min_half[AC_MODE_COUNT];     /* learned range per mode, 0 if none */
    uint8_t temp_max_half[AC_MODE_COUNT];
    uint16_t fans_mask;                       /* bit (1 << ac_fan_t), any mode */
    uint8_t swing_mask;                       /* bit (1 << ac_swing_t) */
    uint8_t temp_step_half;                   /* 1 = 0.5 C, 2 = 1 C */
    uint16_t code_count;
} ac_caps_t;

const char *ac_mode_name(uint8_t mode);
const char *ac_fan_name(uint8_t fan);
/* Vietnamese labels for the UI and the "last IR command" text. */
const char *ac_mode_label(uint8_t mode);
const char *ac_fan_label(uint8_t fan);
bool ac_state_valid(const ac_state_t *s);
bool ac_state_equal(const ac_state_t *a, const ac_state_t *b);

/** NVS key for a learned state ("off" or "m%ut%uf%us%u"), buf >= 16 bytes. Returns false if invalid. */
bool ac_state_key(const ac_state_t *s, char *buf, size_t len);
bool ac_state_from_key(const char *key, ac_state_t *out);

/** Human readable, e.g. "Lạnh 25°C, quạt tự động, đảo gió tắt" (<= 60 bytes, use a 64 byte buffer). */
void ac_state_describe(const ac_state_t *s, char *buf, size_t len);

/** Derive capabilities from the list of learned states. */
void ac_caps_from_list(const ac_state_t *list, size_t n, uint8_t temp_step_half, ac_caps_t *caps);

typedef enum {
    AC_RESOLVE_EXACT = 0,
    AC_RESOLVE_NEAREST_TEMP,
    AC_RESOLVE_FALLBACK_FAN_SWING,
    AC_RESOLVE_NONE,
} ac_resolve_kind_t;

/**
 * Choose the learned state to transmit for `desired` (raw profiles may only send learned states).
 * - power off -> the learned "off" code, otherwise none;
 * - same mode only (never another mode);
 * - temperature first: nearest learned temperature;
 * - then fan/swing: requested, requested fan + default swing, default fan + requested swing,
 *   defaults, anything else;
 * - remaining ties: closer to user_target_half, then the energy-saving side (higher for
 *   cool/dry, lower for heat).
 */
ac_resolve_kind_t ac_resolve(const ac_state_t *list, size_t n, uint8_t default_fan, uint8_t default_swing,
                             const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen);

#ifdef __cplusplus
}
#endif
