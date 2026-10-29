#pragma once

/*
 * Infrared transmit/receive on the ESP-IDF RMT driver (driver/rmt_tx.h, driver/rmt_rx.h).
 *
 * RMT only moves pulses; protocol knowledge lives in ac_profiles. This module provides:
 *   - learning: raw capture of a remote (multi-segment, repeats, long frames) with explicit
 *     "buffer too small" errors instead of silent truncation;
 *   - transmission: a single "pending" slot (latest full-state command wins), duplicate filter,
 *     minimum pause between commands. "Sent" only means the LED emitted the frame: an IR remote
 *     link is one-way, the AC does not acknowledge.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IR_LEARN_IDLE = 0,
    IR_LEARN_WAITING,    /* receiver armed, waiting for the remote */
    IR_LEARN_CAPTURING,  /* segments are arriving                  */
    IR_LEARN_DONE,
    IR_LEARN_FAILED,
    IR_LEARN_CANCELLED,
} ir_learn_state_t;

typedef struct {
    ir_learn_state_t state;
    uint32_t session;          /* increments with every ir_learn_start() */
    char message[96];
    uint8_t segments;
    uint16_t levels;
    uint32_t duration_ms;
    uint16_t noise_discarded;  /* captures rejected as too short */
    uint32_t result_size;      /* serialized size of the result (DONE) */
} ir_learn_status_t;

typedef enum {
    IR_SRC_CLIMATE = 0, /* automatic: user change through Matter or compensation */
    IR_SRC_MANUAL,      /* manual control page                                    */
    IR_SRC_TEST,        /* "phát thử" of a learned code                           */
} ir_source_t;

typedef struct {
    bool initialised;
    bool busy;
    uint32_t sent;
    uint32_t failed;
    uint32_t dropped_duplicate;
    uint32_t superseded;       /* pending command replaced by a newer one before transmission */
    int64_t last_sent_us;      /* 0 = never */
    ir_source_t last_source;
    char last_label[64];
    esp_err_t last_error;
    uint16_t carrier_hz;
} ir_tx_status_t;

esp_err_t ir_transceiver_init(void);

esp_err_t ir_learn_start(void);
void ir_learn_cancel(void);
void ir_learn_get_status(ir_learn_status_t *out);
/** Copy the serialized ir_code blob of the last successful capture. */
esp_err_t ir_learn_get_result(uint8_t *out, size_t *inout_len);

/**
 * Queue a serialized command (copied). `repeats` extra transmissions (0..3) with the minimum gap.
 * force=true bypasses the duplicate filter (explicit "send again").
 */
esp_err_t ir_send(const uint8_t *blob, size_t len, ir_source_t source, bool force, uint8_t repeats, const char *label);

void ir_tx_get_status(ir_tx_status_t *out);

/** Carrier for subsequent transmissions (profile may override the Kconfig default). */
esp_err_t ir_set_carrier(uint16_t carrier_hz);

#ifdef __cplusplus
}
#endif
