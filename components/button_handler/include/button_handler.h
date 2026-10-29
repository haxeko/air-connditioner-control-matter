#pragma once

/*
 * BOOT button (default GPIO0, active low): N short presses within the window open (or extend)
 * the configuration portal. The ISR only wakes the task; debouncing, counting and the callback
 * run in task context. No other gesture is implemented (no factory reset from the button).
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*button_sequence_cb_t)(void);

esp_err_t button_handler_start(button_sequence_cb_t on_sequence);

#ifdef __cplusplus
}
#endif
