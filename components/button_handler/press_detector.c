#include "press_detector.h"

#include <string.h>

void press_detector_init(press_detector_t *d, const press_detector_cfg_t *cfg)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    if (d->cfg.max_count == 0) {
        d->cfg.max_count = 3;
    }
}

static void reset_sequence(press_detector_t *d)
{
    d->presses = 0;
    d->in_sequence = false;
    d->first_press = 0;
}

static uint8_t end_sequence(press_detector_t *d)
{
    uint8_t n = d->presses;
    reset_sequence(d);
    return n;
}

/* A debounced level change that really happened at time t. Returns a reported count or 0. */
static uint8_t on_stable_change(press_detector_t *d, bool pressed, int64_t t)
{
    uint8_t reported = 0;
    if (pressed) {
        if (d->in_sequence && (t - d->first_press) > (int64_t)d->cfg.window_ms) {
            reported = end_sequence(d); /* this press starts the next sequence */
        }
        if (!d->in_sequence) {
            d->in_sequence = true;
            d->first_press = t;
            d->presses = 0;
        }
        d->press_start = t;
        d->hold_cancelled = false;
        return reported;
    }
    /* released */
    if (d->hold_cancelled || (t - d->press_start) > (int64_t)d->cfg.max_press_ms) {
        reset_sequence(d); /* a long hold is not a short press: the whole sequence is void */
        return 0;
    }
    if (!d->in_sequence) {
        return 0;
    }
    d->presses++;
    if (d->presses >= d->cfg.max_count) {
        return end_sequence(d);
    }
    return 0;
}

uint8_t press_detector_feed(press_detector_t *d, bool raw_pressed, int64_t now_ms)
{
    uint8_t reported = 0;
    if (raw_pressed != d->candidate_pressed) {
        d->candidate_pressed = raw_pressed;
        d->candidate_since = now_ms;
    }
    if (d->candidate_pressed != d->stable_pressed && (now_ms - d->candidate_since) >= (int64_t)d->cfg.debounce_ms) {
        d->stable_pressed = d->candidate_pressed;
        /* the change really happened when the raw level settled */
        reported = on_stable_change(d, d->stable_pressed, d->candidate_since);
    }
    if (d->stable_pressed && !d->hold_cancelled && (now_ms - d->press_start) > (int64_t)d->cfg.max_press_ms) {
        d->hold_cancelled = true; /* long hold: not part of a short-press sequence */
        reset_sequence(d);
    }
    /* Window over while released (and no press waiting for its debounce): the sequence ends. */
    if (reported == 0 && d->in_sequence && !d->stable_pressed && d->candidate_pressed == d->stable_pressed &&
        (now_ms - d->first_press) > (int64_t)d->cfg.window_ms) {
        reported = end_sequence(d);
    }
    return reported;
}

bool press_detector_busy(const press_detector_t *d, int64_t now_ms)
{
    (void)now_ms;
    return d->stable_pressed || d->candidate_pressed != d->stable_pressed || d->in_sequence;
}

/* ------------------------------------------------------------------------------------------ */

void hold_detector_init(hold_detector_t *d, const hold_detector_cfg_t *cfg)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
}

hold_event_t hold_detector_feed(hold_detector_t *d, bool raw_pressed, int64_t now_ms)
{
    if (!d->started) {
        d->started = true;
        d->candidate_pressed = raw_pressed;
        d->candidate_since = now_ms;
    } else if (raw_pressed != d->candidate_pressed) {
        /* The caller may not sample while the button is idle: a release that lasted long enough is
         * recognised when the next press arrives. */
        if (!d->candidate_pressed && (now_ms - d->candidate_since) >= (int64_t)d->cfg.debounce_ms) {
            d->armed = true;
        }
        d->candidate_pressed = raw_pressed;
        d->candidate_since = now_ms;
    }
    const bool settled = (now_ms - d->candidate_since) >= (int64_t)d->cfg.debounce_ms;
    if (settled && !d->candidate_pressed) {
        d->armed = true;
    }
    if (settled && d->candidate_pressed != d->stable_pressed) {
        d->stable_pressed = d->candidate_pressed;
        if (d->stable_pressed) {
            d->press_start = d->candidate_since;
            d->stage = 0;
            d->counting = d->armed;
        } else {
            const bool stage1_only = d->counting && d->stage == 1;
            d->stage = 0;
            d->counting = false;
            return stage1_only ? HOLD_STAGE1_RELEASED : HOLD_NONE;
        }
    }
    if (d->stable_pressed && d->counting) {
        const int64_t held = now_ms - d->press_start;
        if (d->stage == 0 && held >= (int64_t)d->cfg.stage1_ms) {
            d->stage = 1;
            return HOLD_STAGE1_REACHED;
        }
        if (d->stage == 1 && held >= (int64_t)d->cfg.stage2_ms) {
            d->stage = 2;
            return HOLD_STAGE2_REACHED;
        }
    }
    return HOLD_NONE;
}
