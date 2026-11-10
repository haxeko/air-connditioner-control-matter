#pragma once

/*
 * Interface for AC protocol drivers.
 *
 * AC_DRIVER_RAW replays full-state commands learned from the original remote and can only produce
 * states that were learned. AC_DRIVER_IRLIB generates the commands of a protocol of the
 * IRremoteESP8266 library (ac_irlib.h), recognised from one press of the original remote and
 * confirmed by the user with test commands before the profile is used.
 */

#include "ac_types.h"
#include "esp_err.h"
#include "ir_code.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AC_DRIVER_RAW = 1,
    AC_DRIVER_IRLIB = 2,
} ac_driver_id_t;

typedef struct ac_protocol_driver {
    uint8_t driver_id;
    const char *name;

    /** Capabilities of `slot` (for RAW: derived from the learned codes). */
    void (*get_caps)(int slot, ac_caps_t *caps);

    /** Closest state the driver can produce. ESP_ERR_NOT_FOUND if none for this mode. */
    esp_err_t (*resolve)(int slot, const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen,
                         ac_resolve_kind_t *kind);

    /** Serialized ir_code blob (see ir_code.h) for a resolved state; caller frees *blob with free().
     *  `prev` = state sent before (protocols that toggle settings), or NULL if unknown. */
    esp_err_t (*encode)(int slot, const ac_state_t *state, const ac_state_t *prev, uint8_t **blob, size_t *len);

    /** State carried by a command received from the original remote; ESP_ERR_NOT_FOUND if it is not a
     *  command of this profile. `why` (len > 0) tells what was received when it does not match. */
    esp_err_t (*decode)(int slot, const ir_code_t *code, ac_state_t *state, char *why, size_t why_len);
} ac_protocol_driver_t;

const ac_protocol_driver_t *ac_protocol_find(uint8_t driver_id);

#ifdef __cplusplus
}
#endif
