#pragma once

/*
 * BOOT button (default GPIO0, active low). Sequences of short presses are counted (see
 * press_detector.h) and reported to the application, which maps them to actions (default: 2 =
 * Matter pairing mode, 3 = configuration portal). Long holds are reported in two stages (default
 * 10 s: leave all home ecosystems when released, 20 s: factory reset). The ISR only wakes the task;
 * debouncing, counting and the callbacks run in task context.
 */

#include <stdint.h>

#include "esp_err.h"
#include "press_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called in the button task with the number of short presses of a finished sequence (>= 1). */
typedef void (*button_sequence_cb_t)(uint8_t presses);

/** Called in the button task for the long-hold stages (never HOLD_NONE), see press_detector.h. */
typedef void (*button_hold_cb_t)(hold_event_t ev);

esp_err_t button_handler_start(button_sequence_cb_t on_sequence, button_hold_cb_t on_hold);

#ifdef __cplusplus
}
#endif
