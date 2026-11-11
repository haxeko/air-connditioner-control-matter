#pragma once

/*
 * Short-press sequence detector (pure C, host tested).
 *
 * Input: raw button samples (pressed / released) with a millisecond timestamp, in time order.
 * - A level change is accepted only after it has been stable for debounce_ms.
 * - A short press is press + release with the pressed time <= max_press_ms; a longer hold cancels
 *   the whole sequence (it is not a short press and reports nothing).
 * - A sequence starts with the first press. It ends, and its number of short presses is reported:
 *     * immediately when max_count presses are complete (no need to wait), or
 *     * when window_ms have elapsed since its first press (while the button is released).
 *   A press starting after the window belongs to the next sequence.
 * Example with max_count 3, window 1.5 s: 3 quick presses -> 3 at the third release; 2 presses -> 2
 * after 1.5 s; 1 press -> 1 after 1.5 s (callers ignore counts they do not use).
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
    uint8_t max_count;
} press_detector_cfg_t;

typedef struct {
    press_detector_cfg_t cfg;
    bool stable_pressed;      /* debounced state */
    bool candidate_pressed;   /* raw state waiting for debounce */
    int64_t candidate_since;
    int64_t press_start;      /* time of the current debounced press */
    int64_t first_press;      /* time of the first press of the sequence */
    uint8_t presses;          /* completed short presses in the sequence */
    bool in_sequence;         /* a sequence was started (first press seen) */
    bool hold_cancelled;      /* current press exceeded max_press_ms */
} press_detector_t;

void press_detector_init(press_detector_t *d, const press_detector_cfg_t *cfg);

/** Feed one raw sample. Returns 0, or the number of short presses of a sequence that just ended. */
uint8_t press_detector_feed(press_detector_t *d, bool raw_pressed, int64_t now_ms);

/** True while a sequence or a debounce is in progress (the task keeps sampling). */
bool press_detector_busy(const press_detector_t *d, int64_t now_ms);

/* ---------------------------------------------------------------------------------------------
 * Long-hold detector (pure C, host tested): the two destructive gestures of the button.
 *
 *   held for stage1_ms                 -> HOLD_STAGE1_REACHED  (feedback only: nothing happens yet)
 *   released between the two stages    -> HOLD_STAGE1_RELEASED (run the stage 1 action)
 *   held for stage2_ms                 -> HOLD_STAGE2_REACHED  (run the stage 2 action now; the
 *                                         release that follows reports nothing)
 * Level changes are debounced like above. A hold only counts after the button has been seen released
 * once since init: a button that is stuck, or held while the device powers up, never triggers.
 * ------------------------------------------------------------------------------------------- */

typedef enum {
    HOLD_NONE = 0,
    HOLD_STAGE1_REACHED,
    HOLD_STAGE1_RELEASED,
    HOLD_STAGE2_REACHED,
} hold_event_t;

typedef struct {
    uint32_t debounce_ms;
    uint32_t stage1_ms;
    uint32_t stage2_ms;
} hold_detector_cfg_t;

typedef struct {
    hold_detector_cfg_t cfg;
    bool started;            /* first sample seen */
    bool armed;              /* a debounced "released" was seen */
    bool stable_pressed;
    bool candidate_pressed;
    bool counting;           /* the current press started after `armed` */
    int64_t candidate_since;
    int64_t press_start;
    uint8_t stage;           /* stages reached by the current press: 0, 1 or 2 */
} hold_detector_t;

void hold_detector_init(hold_detector_t *d, const hold_detector_cfg_t *cfg);

/** Feed one raw sample (same samples as press_detector_feed). */
hold_event_t hold_detector_feed(hold_detector_t *d, bool raw_pressed, int64_t now_ms);

#ifdef __cplusplus
}
#endif
