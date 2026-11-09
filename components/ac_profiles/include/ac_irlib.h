#pragma once

/*
 * AC protocol library: IRremoteESP8266 (components/irremote_esp8266, LGPL-2.1) behind a C API.
 *
 *  - ac_irlib_decode(): recognises the protocol of a command captured from the original remote and
 *    reads the state it carries (power, mode, temperature, fan, swing);
 *  - ac_irlib_build(): generates the full-state command of that protocol for any state, so a
 *    "library" profile needs one remote press instead of one learned command per state.
 *
 * Pure C++ without ESP-IDF dependencies (unit tested on the host, test/host). Calls are serialised
 * internally; they use a few KB of heap and are meant for task context (not ISRs).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ac_types.h"
#include "ir_code.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AC_IRLIB_NAME_LEN 24
#define AC_IRLIB_MAX_MODELS 16
#define AC_IRLIB_FAN_LEVELS_MIN 2
#define AC_IRLIB_FAN_LEVELS_MAX 5

/* Remote settings that are not part of ac_state_t, kept from the reference capture so every
 * generated command carries them (e.g. display light on, beep). */
#define AC_IRLIB_X_LIGHT 0x01u
#define AC_IRLIB_X_BEEP 0x02u
#define AC_IRLIB_X_ECONO 0x04u
#define AC_IRLIB_X_FILTER 0x08u
#define AC_IRLIB_X_CLEAN 0x10u

typedef struct {
    int16_t protocol;     /* decode_type_t of the library */
    int16_t model;        /* library model, -1 = protocol default */
    uint8_t fan_levels;   /* fan speeds of the remote besides Auto (2..5): AC_FAN_1..AC_FAN_<n> */
    uint8_t extra_flags;  /* AC_IRLIB_X_* */
    int8_t swing_h;       /* horizontal swing of the library (-1 = off) */
} ac_irlib_config_t;

typedef struct {
    bool recognized;      /* a protocol of the library matched the signal */
    bool supported;       /* full-state commands can be generated for it (an AC protocol) */
    int16_t protocol;
    char protocol_name[AC_IRLIB_NAME_LEN];
    uint16_t bits;
    int16_t model;        /* model read from the signal, -1 if the signal does not tell */
    bool has_state;       /* `state` is valid */
    ac_state_t state;     /* decoded state; fan mapped for 3 fan levels */
    uint8_t fan_raw;      /* library fan speed of the capture (see ac_irlib_fan_label) */
    uint8_t extra_flags;  /* AC_IRLIB_X_* read from the capture */
    int8_t swing_h;
} ac_irlib_detect_t;

typedef struct {
    int16_t model;
    char name[AC_IRLIB_NAME_LEN];
} ac_irlib_model_t;

typedef enum {
    AC_IRLIB_OK = 0,
    AC_IRLIB_ERR_ARG,
    AC_IRLIB_ERR_NO_MEM,
    AC_IRLIB_ERR_UNSUPPORTED, /* protocol without full-state support in the library */
    AC_IRLIB_ERR_EMPTY,       /* the library produced no pulses for this state */
    AC_IRLIB_ERR_TOO_LONG,    /* does not fit the ir_code storage */
} ac_irlib_result_t;

/** Recognise a captured command (levels and gaps as produced by ir_capture). */
ac_irlib_result_t ac_irlib_decode(const ir_code_t *code, ac_irlib_detect_t *out);

/** Generate the command for `state` (power off included). `prev` = state sent before (protocols that
 *  toggle settings), or NULL. Fills `code` (initialised by the caller with ir_code_init), including
 *  code->carrier_hz of the protocol. */
ac_irlib_result_t ac_irlib_build(const ac_irlib_config_t *cfg, const ac_state_t *state, const ac_state_t *prev,
                                 ir_code_t *code);

/** True if full-state commands can be generated for `protocol`. */
bool ac_irlib_protocol_supported(int16_t protocol);

/** Protocol name, e.g. "DAIKIN" ("UNKNOWN" if invalid). */
void ac_irlib_protocol_name(int16_t protocol, char *buf, size_t len);

/** Models the library distinguishes for `protocol` (0 if the protocol has a single model). */
size_t ac_irlib_models(int16_t protocol, ac_irlib_model_t *out, size_t max);

/** Model name ("" when model < 0 or unknown). */
void ac_irlib_model_name(int16_t protocol, int16_t model, char *buf, size_t len);

/** Vietnamese label of a library fan speed (ac_irlib_detect_t.fan_raw). */
const char *ac_irlib_fan_label(uint8_t fan_raw);

/** Library fan speed used for `fan` with a remote that has `fan_levels` speeds (exposed for tests). */
uint8_t ac_irlib_fan_to_lib(uint8_t fan, uint8_t fan_levels);

#ifdef __cplusplus
}
#endif
