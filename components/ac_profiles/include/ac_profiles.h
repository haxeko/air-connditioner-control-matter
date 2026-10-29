#pragma once

/*
 * AC profiles stored on the "irstore" NVS partition.
 *
 * A profile slot (0..AC_PROFILE_SLOTS-1) holds metadata and, for raw profiles, one learned IR
 * command per full state (power off, or mode + temperature + fan + swing). Exactly one slot is
 * active. Automatic control only transmits when the active profile is LOCKED (confirmed by the
 * user after testing); learning/deleting codes is only possible while unlocked.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ac_protocol.h"
#include "ac_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AC_PROFILE_SLOTS 4
#define AC_PROFILE_MAX_CODES 200
#define AC_PROFILE_VERSION 1

typedef struct {
    uint16_t version;
    uint8_t driver;          /* ac_driver_id_t */
    uint8_t locked;
    char name[32];
    uint8_t temp_step_half;  /* 1 = 0.5 C, 2 = 1 C */
    uint8_t default_fan;
    uint8_t default_swing;
    uint8_t reserved0;
    uint16_t carrier_hz;     /* 0 = Kconfig default */
    uint16_t reserved1;
    uint32_t created_s;
    uint32_t reserved[4];
} ac_profile_meta_t;

esp_err_t ac_profiles_init(void);

int ac_profiles_active_slot(void);
/** ESP_ERR_NOT_FOUND for an empty slot. */
esp_err_t ac_profiles_get_meta(int slot, ac_profile_meta_t *out);
esp_err_t ac_profiles_create(int slot, const char *name, uint8_t temp_step_half, uint16_t carrier_hz);
esp_err_t ac_profiles_update_settings(int slot, const char *name, uint8_t temp_step_half, uint8_t default_fan,
                                      uint8_t default_swing, uint16_t carrier_hz);
esp_err_t ac_profiles_delete(int slot);
/** Select the active slot. *heat_changed tells whether the Matter Heat feature must change (reboot). */
esp_err_t ac_profiles_select(int slot, bool *caps_changed);
esp_err_t ac_profiles_set_locked(int slot, bool locked);

esp_err_t ac_profiles_store_code(int slot, const ac_state_t *state, const uint8_t *blob, size_t len);
esp_err_t ac_profiles_delete_code(int slot, const ac_state_t *state);
/** *inout_len in = buffer size; ESP_ERR_INVALID_SIZE with the needed size when too small. */
esp_err_t ac_profiles_load_code(int slot, const ac_state_t *state, uint8_t *buf, size_t *inout_len);
size_t ac_profiles_list_codes(int slot, ac_state_t *out, size_t max);

void ac_profiles_get_caps(int slot, ac_caps_t *caps);

/** True if the active profile may be used by automatic control (exists, locked, has Off). */
bool ac_profiles_ready_for_control(const char **why);

/* Setpoint limits exposed through the Matter Thermostat (0.01 degC). */
typedef struct {
    bool from_profile;       /* false: no locked profile yet, defaults (Cool 16..30 C) */
    bool heat, dry, fan_only;
    int16_t cool_min, cool_max;
    int16_t heat_min, heat_max;
} ac_thermostat_limits_t;

#define AC_DEFAULT_COOL_MIN_CENTI 1600
#define AC_DEFAULT_COOL_MAX_CENTI 3000

/**
 * Limits for the Matter Thermostat derived from the LOCKED profile in `slot`: the learned
 * temperature range per mode (a raw profile cannot produce anything else). Modes without learned
 * codes are not offered. Without a locked profile: Off/Cool only with the default range.
 */
void ac_profiles_thermostat_limits(int slot, ac_thermostat_limits_t *out);

/**
 * Resolve + encode for the active profile. `chosen` reports what will actually be sent.
 * Returns ESP_ERR_NOT_FOUND if no learned/encodable command matches.
 */
esp_err_t ac_profiles_build_command(const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen,
                                    ac_resolve_kind_t *kind, uint8_t **blob, size_t *len);

#ifdef __cplusplus
}
#endif
