#pragma once

/*
 * N-short-press detector (pure C, host tested).
 *
 * Input: raw button samples (pressed / released) with a millisecond timestamp, in time order.
 * - A level change is accepted only after it has been stable for debounce_ms.
 * - A short press is press + release with the pressed time <= max_press_ms; a longer hold cancels
 *   the sequence (it is not a short press).
 * - The sequence fires when `count` short presses complete (last release) within window_ms
 *   measured from the first accepted press. A missed window restarts from the latest press.
 * - The detector fires once per sequence; one or two presses never fire.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t debounce_ms;
    uint32_t window_ms;
    uint32_t max_press_ms;
    uint8_t count;
} press_detector_cfg_t;

typedef struct {
    press_detector_cfg_t cfg;
    bool stable_pressed;      /* debounced state */
    bool candidate_pressed;   /* raw state waiting for debounce */
    int64_t candidate_since;
    int64_t press_start;      /* time of the current debounced press */
    int64_t first_press;      /* time of the first press of the sequence */
    uint8_t presses;          /* completed short presses in the sequence */
    bool hold_cancelled;      /* current press exceeded max_press_ms */
} press_detector_t;

void press_detector_init(press_detector_t *d, const press_detector_cfg_t *cfg);

/** Feed one raw sample. Returns true exactly when the N-press sequence completes. */
bool press_detector_feed(press_detector_t *d, bool raw_pressed, int64_t now_ms);

/** True while a sequence or a debounce is in progress (the task keeps sampling). */
bool press_detector_busy(const press_detector_t *d, int64_t now_ms);

#ifdef __cplusplus
}
#endif
