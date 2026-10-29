#pragma once

/*
 * Raw infrared command representation (pure C, no ESP-IDF dependency; unit tested on the host).
 *
 * A command is a list of segments (frames separated by silence). Each segment is an alternating
 * list of level durations in microseconds that starts and ends with a MARK (carrier on):
 *     mark, space, mark, space, ..., mark
 * gap_before_us is the silence between the previous segment's last mark and this segment's first
 * mark (0 for the first segment). Repeats sent by the original remote are simply further segments.
 *
 * Serialized blob (little endian):
 *     u8  magic 'R', u8 version (1), u16 carrier_hz, u8 segment_count, u8 flags, u16 total_levels,
 *     segment_count x { u16 level_count, u32 gap_before_us },
 *     total_levels x u16 duration_us
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IR_CODE_MAGIC 'R'
#define IR_CODE_VERSION 1
#define IR_CODE_MAX_SEGMENTS 32
#define IR_LEVEL_MAX_US 65535u

typedef struct {
    uint16_t level_count;
    uint32_t gap_before_us;
} ir_segment_t;

typedef struct {
    uint16_t carrier_hz;
    uint8_t segment_count;
    uint16_t total_levels;
    ir_segment_t segments[IR_CODE_MAX_SEGMENTS];
    uint16_t *levels;          /* points into caller storage */
    size_t levels_capacity;
} ir_code_t;

typedef enum {
    IR_OK = 0,
    IR_ERR_ARG,
    IR_ERR_LEVELS_FULL,    /* more levels than the buffer holds: capture rejected, never truncated */
    IR_ERR_SEGMENTS_FULL,
    IR_ERR_FORMAT,
    IR_ERR_EMPTY,
} ir_result_t;

void ir_code_init(ir_code_t *code, uint16_t *levels_storage, size_t capacity, uint16_t carrier_hz);

/**
 * Append one segment given as alternating durations (first = mark). Leading spaces and a trailing
 * space are dropped, adjacent zero-length levels are merged. Fails without modifying the code
 * when capacity would be exceeded.
 */
ir_result_t ir_code_append_segment(ir_code_t *code, const uint32_t *durations_us, const uint8_t *levels,
                                   size_t count, uint32_t gap_before_us);

/** Total on-air duration of the command (marks, spaces and gaps), microseconds. */
uint64_t ir_code_duration_us(const ir_code_t *code);

/** Remove the last appended segment (used to discard noise). */
void ir_code_drop_last_segment(ir_code_t *code);

/** Sum of the level durations of one segment (without its gap), microseconds. */
uint32_t ir_code_segment_duration_us(const ir_code_t *code, uint8_t segment);

size_t ir_code_serialized_size(const ir_code_t *code);
/** Returns bytes written or 0 on error. */
size_t ir_code_serialize(const ir_code_t *code, uint8_t *out, size_t out_len);
/** Parses into code (levels copied into code->levels storage). */
ir_result_t ir_code_deserialize(ir_code_t *code, const uint8_t *in, size_t in_len);

/** Structural validation of a serialized blob without needing storage. */
ir_result_t ir_code_validate_blob(const uint8_t *in, size_t in_len, uint16_t *out_total_levels);

/**
 * Compare two commands level by level. Each pair must match within max(tolerance_pct of the larger,
 * abs_tolerance_us); gaps are compared with a 25 % / 2 ms tolerance. Used to confirm a second capture.
 */
bool ir_code_similar(const ir_code_t *a, const ir_code_t *b, unsigned tolerance_pct, unsigned abs_tolerance_us);

/* ---------------------------------------------------------------------------------------------
 * Conversion to transmitter symbols. A symbol holds two (level, duration) halves like the ESP32
 * RMT rmt_symbol_word_t; durations are in ticks of 1 us and limited to 15 bits (32767).
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    uint16_t duration0;
    uint8_t level0;
    uint16_t duration1;
    uint8_t level1;
} ir_symbol_t;

#define IR_SYMBOL_MAX_DURATION 32767u

/** Append one received segment given as receiver symbols (level 1 = mark). Same rules as above. */
ir_result_t ir_code_append_symbols(ir_code_t *code, const ir_symbol_t *symbols, size_t symbol_count,
                                   uint32_t gap_before_us);

/** Number of symbols ir_code_to_symbols() will produce. */
size_t ir_code_symbol_count(const ir_code_t *code);

/** Fill out[] (capacity max_symbols). Returns the number written or 0 if it does not fit. */
size_t ir_code_to_symbols(const ir_code_t *code, ir_symbol_t *out, size_t max_symbols);

#ifdef __cplusplus
}
#endif
