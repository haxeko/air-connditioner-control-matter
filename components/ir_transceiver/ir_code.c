#include "ir_code.h"

#include <string.h>

#define HEADER_SIZE 8u
#define SEGMENT_SIZE 6u

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ir_code_init(ir_code_t *code, uint16_t *levels_storage, size_t capacity, uint16_t carrier_hz)
{
    memset(code, 0, sizeof(*code));
    code->levels = levels_storage;
    code->levels_capacity = capacity;
    code->carrier_hz = carrier_hz;
}

/* Source of (level, duration) entries: either parallel arrays or RMT-like symbols (2 halves each). */
typedef struct {
    const uint32_t *durations;
    const uint8_t *levels;
    const ir_symbol_t *symbols;
    size_t count; /* number of entries (halves for symbols) */
} entry_src_t;

static void src_get(const entry_src_t *src, size_t k, uint8_t *level, uint32_t *duration)
{
    if (src->symbols != NULL) {
        const ir_symbol_t *s = &src->symbols[k / 2];
        if (k & 1u) {
            *level = s->level1 ? 1 : 0;
            *duration = s->duration1;
        } else {
            *level = s->level0 ? 1 : 0;
            *duration = s->duration0;
        }
    } else {
        *level = src->levels[k] ? 1 : 0;
        *duration = src->durations[k];
    }
}

static ir_result_t append_from(ir_code_t *code, const entry_src_t *src, uint32_t gap_before_us)
{
    if (code->segment_count >= IR_CODE_MAX_SEGMENTS) {
        return IR_ERR_SEGMENTS_FULL;
    }
    size_t count = src->count;
    uint8_t lv0;
    uint32_t d0;

    /* skip leading spaces / zero-length entries */
    size_t i = 0;
    while (i < count) {
        src_get(src, i, &lv0, &d0);
        if (lv0 == 1 && d0 > 0) {
            break;
        }
        i++;
    }
    /* drop trailing spaces / zero-length entries (the RMT end marker and the idle tail) */
    size_t end = count;
    while (end > i) {
        src_get(src, end - 1, &lv0, &d0);
        if (lv0 == 1 && d0 > 0) {
            break;
        }
        end--;
    }
    if (i >= end) {
        return IR_ERR_EMPTY;
    }

    /* Two passes: count first so that a capacity failure leaves the code untouched. */
    size_t start = code->total_levels;
    size_t n = 0;
    for (size_t pass = 0; pass < 2; pass++) {
        n = 0;
        int last_level = -1;
        uint32_t acc = 0;
        for (size_t k = i; k < end; k++) {
            uint8_t lvk;
            uint32_t dk;
            src_get(src, k, &lvk, &dk);
            if (dk == 0) {
                continue;
            }
            int lv = lvk;
            if (lv == last_level) {
                acc += dk;
                continue;
            }
            if (last_level >= 0) {
                if (acc > IR_LEVEL_MAX_US) {
                    return IR_ERR_FORMAT;
                }
                if (pass == 1) {
                    code->levels[start + n] = (uint16_t)acc;
                }
                n++;
            }
            last_level = lv;
            acc = dk;
        }
        if (acc > IR_LEVEL_MAX_US) {
            return IR_ERR_FORMAT;
        }
        if (pass == 1) {
            code->levels[start + n] = (uint16_t)acc;
        }
        n++;
        if (pass == 0 && start + n > code->levels_capacity) {
            return IR_ERR_LEVELS_FULL;
        }
        if (pass == 0 && start + n > 0xFFFFu) {
            return IR_ERR_LEVELS_FULL;
        }
    }
    /* starts with a mark (leading spaces trimmed) and ends with a mark -> odd count */
    if ((n & 1u) == 0) {
        return IR_ERR_FORMAT;
    }
    ir_segment_t *seg = &code->segments[code->segment_count++];
    seg->level_count = (uint16_t)n;
    seg->gap_before_us = (code->segment_count == 1) ? 0 : gap_before_us;
    code->total_levels = (uint16_t)(start + n);
    return IR_OK;
}

ir_result_t ir_code_append_segment(ir_code_t *code, const uint32_t *durations_us, const uint8_t *levels,
                                   size_t count, uint32_t gap_before_us)
{
    if (code == NULL || (count > 0 && (durations_us == NULL || levels == NULL))) {
        return IR_ERR_ARG;
    }
    entry_src_t src = { durations_us, levels, NULL, count };
    return append_from(code, &src, gap_before_us);
}

ir_result_t ir_code_append_symbols(ir_code_t *code, const ir_symbol_t *symbols, size_t symbol_count,
                                   uint32_t gap_before_us)
{
    if (code == NULL || (symbol_count > 0 && symbols == NULL)) {
        return IR_ERR_ARG;
    }
    entry_src_t src = { NULL, NULL, symbols, symbol_count * 2u };
    return append_from(code, &src, gap_before_us);
}

void ir_code_drop_last_segment(ir_code_t *code)
{
    if (code->segment_count == 0) {
        return;
    }
    code->segment_count--;
    code->total_levels = (uint16_t)(code->total_levels - code->segments[code->segment_count].level_count);
}

uint32_t ir_code_segment_duration_us(const ir_code_t *code, uint8_t segment)
{
    if (segment >= code->segment_count) {
        return 0;
    }
    size_t idx = 0;
    for (uint8_t s = 0; s < segment; s++) {
        idx += code->segments[s].level_count;
    }
    uint32_t total = 0;
    for (uint16_t k = 0; k < code->segments[segment].level_count; k++) {
        total += code->levels[idx + k];
    }
    return total;
}

uint64_t ir_code_duration_us(const ir_code_t *code)
{
    uint64_t total = 0;
    size_t idx = 0;
    for (uint8_t s = 0; s < code->segment_count; s++) {
        total += code->segments[s].gap_before_us;
        for (uint16_t k = 0; k < code->segments[s].level_count; k++) {
            total += code->levels[idx++];
        }
    }
    return total;
}

size_t ir_code_serialized_size(const ir_code_t *code)
{
    return HEADER_SIZE + (size_t)code->segment_count * SEGMENT_SIZE + (size_t)code->total_levels * 2u;
}

size_t ir_code_serialize(const ir_code_t *code, uint8_t *out, size_t out_len)
{
    size_t need = ir_code_serialized_size(code);
    if (code->segment_count == 0 || out == NULL || out_len < need) {
        return 0;
    }
    out[0] = IR_CODE_MAGIC;
    out[1] = IR_CODE_VERSION;
    put16(out + 2, code->carrier_hz);
    out[4] = code->segment_count;
    out[5] = 0;
    put16(out + 6, code->total_levels);
    uint8_t *p = out + HEADER_SIZE;
    for (uint8_t s = 0; s < code->segment_count; s++) {
        put16(p, code->segments[s].level_count);
        put32(p + 2, code->segments[s].gap_before_us);
        p += SEGMENT_SIZE;
    }
    for (uint16_t k = 0; k < code->total_levels; k++) {
        put16(p, code->levels[k]);
        p += 2;
    }
    return need;
}

ir_result_t ir_code_validate_blob(const uint8_t *in, size_t in_len, uint16_t *out_total_levels)
{
    if (in == NULL || in_len < HEADER_SIZE || in[0] != IR_CODE_MAGIC || in[1] != IR_CODE_VERSION) {
        return IR_ERR_FORMAT;
    }
    uint8_t segs = in[4];
    uint16_t total = get16(in + 6);
    if (segs == 0 || segs > IR_CODE_MAX_SEGMENTS || total == 0) {
        return IR_ERR_FORMAT;
    }
    if (in_len != HEADER_SIZE + (size_t)segs * SEGMENT_SIZE + (size_t)total * 2u) {
        return IR_ERR_FORMAT;
    }
    uint32_t sum = 0;
    const uint8_t *p = in + HEADER_SIZE;
    for (uint8_t s = 0; s < segs; s++) {
        uint16_t lc = get16(p);
        if (lc == 0 || (lc & 1u) == 0) {
            return IR_ERR_FORMAT;
        }
        if (s == 0 && get32(p + 2) != 0) {
            return IR_ERR_FORMAT;
        }
        sum += lc;
        p += SEGMENT_SIZE;
    }
    if (sum != total) {
        return IR_ERR_FORMAT;
    }
    for (uint16_t k = 0; k < total; k++) {
        if (get16(p + 2u * k) == 0) {
            return IR_ERR_FORMAT;
        }
    }
    if (out_total_levels) {
        *out_total_levels = total;
    }
    return IR_OK;
}

ir_result_t ir_code_deserialize(ir_code_t *code, const uint8_t *in, size_t in_len)
{
    uint16_t total = 0;
    ir_result_t r = ir_code_validate_blob(in, in_len, &total);
    if (r != IR_OK) {
        return r;
    }
    if (total > code->levels_capacity) {
        return IR_ERR_LEVELS_FULL;
    }
    code->carrier_hz = get16(in + 2);
    code->segment_count = in[4];
    code->total_levels = total;
    const uint8_t *p = in + HEADER_SIZE;
    for (uint8_t s = 0; s < code->segment_count; s++) {
        code->segments[s].level_count = get16(p);
        code->segments[s].gap_before_us = get32(p + 2);
        p += SEGMENT_SIZE;
    }
    for (uint16_t k = 0; k < total; k++) {
        code->levels[k] = get16(p);
        p += 2;
    }
    return IR_OK;
}

static bool within(uint32_t a, uint32_t b, unsigned pct, unsigned abs_us)
{
    uint32_t hi = a > b ? a : b;
    uint32_t diff = a > b ? a - b : b - a;
    uint32_t tol = (uint32_t)(((uint64_t)hi * pct) / 100u);
    if (tol < abs_us) {
        tol = abs_us;
    }
    return diff <= tol;
}

bool ir_code_similar(const ir_code_t *a, const ir_code_t *b, unsigned tolerance_pct, unsigned abs_tolerance_us)
{
    if (a->segment_count != b->segment_count || a->total_levels != b->total_levels) {
        return false;
    }
    for (uint8_t s = 0; s < a->segment_count; s++) {
        if (a->segments[s].level_count != b->segments[s].level_count) {
            return false;
        }
        if (!within(a->segments[s].gap_before_us, b->segments[s].gap_before_us, 25, 2000)) {
            return false;
        }
    }
    for (uint16_t k = 0; k < a->total_levels; k++) {
        if (!within(a->levels[k], b->levels[k], tolerance_pct, abs_tolerance_us)) {
            return false;
        }
    }
    return true;
}

/* Iterate (level, duration) halves of the whole command with long runs split to <= 15 bits. */
typedef struct {
    const ir_code_t *code;
    uint8_t seg;
    uint16_t lvl_in_seg;
    size_t lvl_idx;
    uint32_t remaining;   /* remaining duration of the current run */
    uint8_t level;
    bool in_gap;
    bool started;
} half_iter_t;

static bool next_run(half_iter_t *it)
{
    const ir_code_t *c = it->code;
    for (;;) {
        if (it->seg >= c->segment_count) {
            return false;
        }
        const ir_segment_t *s = &c->segments[it->seg];
        if (!it->started) {
            it->started = true;
            it->lvl_in_seg = 0;
            if (s->gap_before_us > 0) {
                it->in_gap = true;
                it->level = 0;
                it->remaining = s->gap_before_us;
                return true;
            }
        }
        it->in_gap = false;
        if (it->lvl_in_seg < s->level_count) {
            it->level = (it->lvl_in_seg & 1u) ? 0 : 1;
            it->remaining = c->levels[it->lvl_idx++];
            it->lvl_in_seg++;
            return true;
        }
        it->seg++;
        it->started = false;
    }
}

static bool next_half(half_iter_t *it, uint8_t *level, uint16_t *dur)
{
    while (it->remaining == 0) {
        if (!next_run(it)) {
            return false;
        }
    }
    uint32_t d = it->remaining > IR_SYMBOL_MAX_DURATION ? IR_SYMBOL_MAX_DURATION : it->remaining;
    it->remaining -= d;
    *level = it->level;
    *dur = (uint16_t)d;
    return true;
}

static size_t convert(const ir_code_t *code, ir_symbol_t *out, size_t max_symbols)
{
    half_iter_t it;
    memset(&it, 0, sizeof(it));
    it.code = code;
    size_t n = 0;
    uint8_t lv;
    uint16_t d;
    while (next_half(&it, &lv, &d)) {
        ir_symbol_t sym;
        sym.level0 = lv;
        sym.duration0 = d;
        if (next_half(&it, &lv, &d)) {
            sym.level1 = lv;
            sym.duration1 = d;
        } else {
            /* odd number of halves: the command ends with a mark, close it with a 1 us space */
            sym.level1 = 0;
            sym.duration1 = 1;
        }
        if (out != NULL) {
            if (n >= max_symbols) {
                return 0;
            }
            out[n] = sym;
        }
        n++;
    }
    return n;
}

size_t ir_code_symbol_count(const ir_code_t *code)
{
    return convert(code, NULL, 0);
}

size_t ir_code_to_symbols(const ir_code_t *code, ir_symbol_t *out, size_t max_symbols)
{
    if (out == NULL || max_symbols == 0) {
        return 0;
    }
    return convert(code, out, max_symbols);
}
