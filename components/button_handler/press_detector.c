#include "press_detector.h"

#include <string.h>

void press_detector_init(press_detector_t *d, const press_detector_cfg_t *cfg)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    if (d->cfg.count == 0) {
        d->cfg.count = 3;
    }
}

static void reset_sequence(press_detector_t *d)
{
    d->presses = 0;
    d->first_press = 0;
}

static bool on_stable_change(press_detector_t *d, bool pressed, int64_t t)
{
    if (pressed) {
        d->press_start = t;
        d->hold_cancelled = false;
        if (d->presses == 0 || (t - d->first_press) > (int64_t)d->cfg.window_ms) {
            /* new sequence (or the previous one ran out of time) */
            d->presses = 0;
            d->first_press = t;
        }
        return false;
    }
    /* released */
    int64_t held = t - d->press_start;
    if (d->hold_cancelled || held > (int64_t)d->cfg.max_press_ms) {
        reset_sequence(d);
        return false;
    }
    if (t - d->first_press > (int64_t)d->cfg.window_ms) {
        reset_sequence(d);
        return false;
    }
    d->presses++;
    if (d->presses >= d->cfg.count) {
        reset_sequence(d);
        return true;
    }
    return false;
}

bool press_detector_feed(press_detector_t *d, bool raw_pressed, int64_t now_ms)
{
    bool fired = false;
    if (raw_pressed != d->candidate_pressed) {
        d->candidate_pressed = raw_pressed;
        d->candidate_since = now_ms;
    }
    if (d->candidate_pressed != d->stable_pressed && (now_ms - d->candidate_since) >= (int64_t)d->cfg.debounce_ms) {
        d->stable_pressed = d->candidate_pressed;
        /* the change really happened when the raw level settled */
        fired = on_stable_change(d, d->stable_pressed, d->candidate_since);
    }
    if (d->stable_pressed && !d->hold_cancelled && (now_ms - d->press_start) > (int64_t)d->cfg.max_press_ms) {
        d->hold_cancelled = true; /* long hold: not part of a short-press sequence */
        reset_sequence(d);
    }
    if (d->presses > 0 && !d->stable_pressed && (now_ms - d->first_press) > (int64_t)d->cfg.window_ms) {
        reset_sequence(d);
    }
    return fired;
}

bool press_detector_busy(const press_detector_t *d, int64_t now_ms)
{
    (void)now_ms;
    return d->stable_pressed || d->candidate_pressed != d->stable_pressed || d->presses > 0;
}
