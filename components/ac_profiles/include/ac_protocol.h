#pragma once

/*
 * Interface for AC protocol drivers.
 *
 * The only driver shipped is AC_DRIVER_RAW: it replays full-state commands learned from the
 * original remote and can only produce states that were learned. A native C/C++ protocol driver
 * for a known brand/model (generating the frame bits itself) implements the same three functions
 * and is added to ac_protocol_registry() in ac_profiles.c. No multi-brand encoder is included:
 * the brand/model of the target AC is unknown and an unverified encoder would send wrong frames.
 */

#include "ac_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AC_DRIVER_RAW = 1,
} ac_driver_id_t;

typedef struct ac_protocol_driver {
    uint8_t driver_id;
    const char *name;

    /** Capabilities of `slot` (for RAW: derived from the learned codes). */
    void (*get_caps)(int slot, ac_caps_t *caps);

    /** Closest state the driver can produce. ESP_ERR_NOT_FOUND if none for this mode. */
    esp_err_t (*resolve)(int slot, const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen,
                         ac_resolve_kind_t *kind);

    /** Serialized ir_code blob (see ir_code.h) for a resolved state; caller frees *blob with free(). */
    esp_err_t (*encode)(int slot, const ac_state_t *state, uint8_t **blob, size_t *len);
} ac_protocol_driver_t;

const ac_protocol_driver_t *ac_protocol_find(uint8_t driver_id);

#ifdef __cplusplus
}
#endif
