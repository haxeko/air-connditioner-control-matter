#pragma once

/*
 * Assembles a learned command from consecutive receiver segments (pure C, host tested).
 *
 * The RMT receiver ends a segment after `idle_us` of silence and reports it with the time of the
 * end-of-segment interrupt. The real silence between two segments is reconstructed as
 *     gap = (t_irq(k) - idle - duration(k)) - (t_irq(k-1) - idle)
 * so frames separated by more than the hardware idle limit (~32 ms at 1 MHz) are still kept
 * with their original spacing.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ir_code.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ir_code_t code;
    int64_t prev_end_us;    /* end (last edge) of the previous segment */
    bool has_prev;
    uint32_t idle_us;
    uint8_t max_segments;
    ir_result_t error;      /* sticky */
} ir_capture_t;

void ir_capture_reset(ir_capture_t *c, uint16_t *storage, size_t capacity, uint16_t carrier_hz, uint32_t idle_us,
                      uint8_t max_segments);

/**
 * Add one segment. `overflow` = the ISR could not store all symbols of this segment.
 * Returns IR_OK, IR_ERR_EMPTY (segment had no mark, ignored) or a sticky error.
 */
ir_result_t ir_capture_add(ir_capture_t *c, const ir_symbol_t *symbols, size_t count, int64_t irq_time_us,
                           bool overflow);

/** A capture with fewer than min_levels levels in total is treated as noise. */
bool ir_capture_is_noise(const ir_capture_t *c, unsigned min_levels);

#ifdef __cplusplus
}
#endif
