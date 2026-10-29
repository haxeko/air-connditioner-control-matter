#include "ir_capture.h"

#include <string.h>

void ir_capture_reset(ir_capture_t *c, uint16_t *storage, size_t capacity, uint16_t carrier_hz, uint32_t idle_us,
                      uint8_t max_segments)
{
    memset(c, 0, sizeof(*c));
    ir_code_init(&c->code, storage, capacity, carrier_hz);
    c->idle_us = idle_us;
    c->max_segments = (max_segments == 0 || max_segments > IR_CODE_MAX_SEGMENTS) ? IR_CODE_MAX_SEGMENTS : max_segments;
    c->error = IR_OK;
}

ir_result_t ir_capture_add(ir_capture_t *c, const ir_symbol_t *symbols, size_t count, int64_t irq_time_us,
                           bool overflow)
{
    if (c->error != IR_OK) {
        return c->error;
    }
    if (overflow) {
        c->error = IR_ERR_LEVELS_FULL;
        return c->error;
    }
    if (c->code.segment_count >= c->max_segments) {
        c->error = IR_ERR_SEGMENTS_FULL;
        return c->error;
    }
    ir_result_t r = ir_code_append_symbols(&c->code, symbols, count, 0);
    if (r == IR_ERR_EMPTY) {
        return r; /* nothing but silence/zero entries */
    }
    if (r != IR_OK) {
        c->error = r;
        return r;
    }
    uint8_t idx = (uint8_t)(c->code.segment_count - 1);
    uint32_t dur = ir_code_segment_duration_us(&c->code, idx);
    int64_t end = irq_time_us - (int64_t)c->idle_us;
    int64_t start = end - (int64_t)dur;
    if (c->has_prev && idx > 0) {
        int64_t gap = start - c->prev_end_us;
        if (gap < 0) {
            gap = 0; /* interrupt jitter; segments cannot overlap */
        }
        if (gap > (int64_t)UINT32_MAX) {
            gap = (int64_t)UINT32_MAX;
        }
        c->code.segments[idx].gap_before_us = (uint32_t)gap;
    }
    c->prev_end_us = end;
    c->has_prev = true;
    return IR_OK;
}

bool ir_capture_is_noise(const ir_capture_t *c, unsigned min_levels)
{
    return c->code.total_levels < min_levels;
}
