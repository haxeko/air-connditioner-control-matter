#pragma once

/*
 * BOOT button (default GPIO0, active low). Sequences of short presses are counted (see
 * press_detector.h) and reported to the application, which maps them to actions (default: 2 =
 * Matter pairing mode, 3 = configuration portal). The ISR only wakes the task; debouncing, counting
 * and the callback run in task context. No factory reset from the button.
 */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called in the button task with the number of short presses of a finished sequence (>= 1). */
typedef void (*button_sequence_cb_t)(uint8_t presses);

esp_err_t button_handler_start(button_sequence_cb_t on_sequence);

#ifdef __cplusplus
}
#endif
