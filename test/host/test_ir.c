#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ir_capture.h"
#include "ir_code.h"
#include "test.h"

static void test_normalise(void)
{
    uint16_t store[64];
    ir_code_t c;
    ir_code_init(&c, store, 64, 38000);
    /* leading space, zero entry, two adjacent marks, trailing space */
    const uint32_t d[] = { 500, 9000, 4500, 0, 560, 560, 1690, 560, 20000 };
    const uint8_t l[] = { 0, 1, 0, 0, 1, 1, 0, 1, 0 };
    CHECK_EQ(ir_code_append_segment(&c, d, l, 9, 1234), IR_OK);
    CHECK_EQ(c.segment_count, 1);
    CHECK_EQ(c.total_levels, 5);
    const uint16_t expect[] = { 9000, 4500, 1120, 1690, 560 };
    CHECK(memcmp(store, expect, sizeof(expect)) == 0);
    CHECK_EQ(c.segments[0].gap_before_us, 0); /* the first segment never has a gap */
    CHECK_EQ(ir_code_segment_duration_us(&c, 0), 9000 + 4500 + 1120 + 1690 + 560);

    /* only spaces: empty */
    const uint32_t d2[] = { 100, 200 };
    const uint8_t l2[] = { 0, 0 };
    CHECK_EQ(ir_code_append_segment(&c, d2, l2, 2, 0), IR_ERR_EMPTY);
    /* a level longer than 65535 us cannot be stored */
    const uint32_t d3[] = { 70000 };
    const uint8_t l3[] = { 1 };
    CHECK_EQ(ir_code_append_segment(&c, d3, l3, 1, 0), IR_ERR_FORMAT);
    CHECK_EQ(c.segment_count, 1);
}

static void test_capacity(void)
{
    uint16_t store[4];
    ir_code_t c;
    ir_code_init(&c, store, 4, 38000);
    const uint32_t d[] = { 9000, 4500, 560, 1690, 560 };
    const uint8_t l[] = { 1, 0, 1, 0, 1 };
    /* 5 levels do not fit into 4: rejected, never truncated, code unchanged */
    CHECK_EQ(ir_code_append_segment(&c, d, l, 5, 0), IR_ERR_LEVELS_FULL);
    CHECK_EQ(c.segment_count, 0);
    CHECK_EQ(c.total_levels, 0);

    uint16_t big[IR_CODE_MAX_SEGMENTS + 1];
    ir_code_init(&c, big, IR_CODE_MAX_SEGMENTS + 1, 38000);
    const uint32_t one = 560;
    const uint8_t mark = 1;
    for (int i = 0; i < IR_CODE_MAX_SEGMENTS; i++) {
        CHECK_EQ(ir_code_append_segment(&c, &one, &mark, 1, 40000), IR_OK);
    }
    CHECK_EQ(ir_code_append_segment(&c, &one, &mark, 1, 40000), IR_ERR_SEGMENTS_FULL);
    CHECK_EQ(c.segment_count, IR_CODE_MAX_SEGMENTS);
    ir_code_drop_last_segment(&c);
    CHECK_EQ(c.segment_count, IR_CODE_MAX_SEGMENTS - 1);
    CHECK_EQ(c.total_levels, IR_CODE_MAX_SEGMENTS - 1);
}

static void build_two_segments(ir_code_t *c, uint16_t *store, size_t cap)
{
    ir_code_init(c, store, cap, 38000);
    const uint32_t d1[] = { 3500, 1750, 450, 1300, 450, 420, 450 };
    const uint8_t l1[] = { 1, 0, 1, 0, 1, 0, 1 };
    const uint32_t d2[] = { 3500, 1750, 450 };
    const uint8_t l2[] = { 1, 0, 1 };
    ir_code_append_segment(c, d1, l1, 7, 0);
    ir_code_append_segment(c, d2, l2, 3, 34800);
}

static void test_serialize(void)
{
    uint16_t store[32];
    ir_code_t c;
    build_two_segments(&c, store, 32);
    CHECK_EQ(c.segment_count, 2);
    CHECK_EQ(c.segments[1].gap_before_us, 34800);
    CHECK_EQ(ir_code_duration_us(&c), (3500 + 1750 + 450 + 1300 + 450 + 420 + 450) + 34800 + (3500 + 1750 + 450));

    uint8_t blob[128];
    size_t n = ir_code_serialize(&c, blob, sizeof(blob));
    CHECK_EQ(n, ir_code_serialized_size(&c));
    CHECK_EQ(n, 8 + 2 * 6 + 10 * 2);
    uint16_t total = 0;
    CHECK_EQ(ir_code_validate_blob(blob, n, &total), IR_OK);
    CHECK_EQ(total, 10);
    CHECK_EQ(ir_code_serialize(&c, blob, n - 1), 0); /* buffer too small */

    uint16_t store2[32];
    ir_code_t d;
    ir_code_init(&d, store2, 32, 0);
    CHECK_EQ(ir_code_deserialize(&d, blob, n), IR_OK);
    CHECK_EQ(d.carrier_hz, 38000);
    CHECK(ir_code_similar(&c, &d, 20, 100));
    uint16_t small[8];
    ir_code_t e;
    ir_code_init(&e, small, 8, 0);
    CHECK_EQ(ir_code_deserialize(&e, blob, n), IR_ERR_LEVELS_FULL);

    /* similarity: one level 30 % off is rejected at 20 % tolerance */
    store2[3] = (uint16_t)(store2[3] * 13 / 10);
    CHECK(!ir_code_similar(&c, &d, 20, 100));

    /* corrupted blobs */
    uint8_t bad[128];
    memcpy(bad, blob, n);
    bad[0] = 'X';
    CHECK_EQ(ir_code_validate_blob(bad, n, NULL), IR_ERR_FORMAT);
    CHECK_EQ(ir_code_validate_blob(blob, n - 2, NULL), IR_ERR_FORMAT); /* truncated */
    memcpy(bad, blob, n);
    bad[8] = 6; /* even level count in segment 0 */
    CHECK_EQ(ir_code_validate_blob(bad, n, NULL), IR_ERR_FORMAT);
    memcpy(bad, blob, n);
    bad[10] = 1; /* first segment with a gap */
    CHECK_EQ(ir_code_validate_blob(bad, n, NULL), IR_ERR_FORMAT);
    memcpy(bad, blob, n);
    bad[20] = 0;
    bad[21] = 0; /* zero duration level */
    CHECK_EQ(ir_code_validate_blob(bad, n, NULL), IR_ERR_FORMAT);
    CHECK_EQ(ir_code_validate_blob(NULL, 0, NULL), IR_ERR_FORMAT);
}

static void test_symbols(void)
{
    uint16_t store[16];
    ir_code_t c;
    ir_code_init(&c, store, 16, 38000);
    const uint32_t d1[] = { 40000, 1000, 500 };
    const uint8_t l1[] = { 1, 0, 1 };
    const uint32_t d2[] = { 600 };
    const uint8_t l2[] = { 1 };
    CHECK_EQ(ir_code_append_segment(&c, d1, l1, 3, 0), IR_OK);
    CHECK_EQ(ir_code_append_segment(&c, d2, l2, 1, 100000), IR_OK);
    /* halves: 32767+7233 mark, 1000 space, 500 mark, 3x32767+1699 gap, 600 mark -> 9 -> 5 symbols */
    CHECK_EQ(ir_code_symbol_count(&c), 5);
    ir_symbol_t s[5];
    CHECK_EQ(ir_code_to_symbols(&c, s, 5), 5);
    CHECK_EQ(ir_code_to_symbols(&c, s, 4), 0); /* does not fit: nothing partial */
    CHECK(s[0].level0 == 1 && s[0].duration0 == 32767 && s[0].level1 == 1 && s[0].duration1 == 7233);
    CHECK(s[1].level0 == 0 && s[1].duration0 == 1000 && s[1].level1 == 1 && s[1].duration1 == 500);
    CHECK(s[2].level0 == 0 && s[2].duration0 == 32767 && s[2].level1 == 0 && s[2].duration1 == 32767);
    CHECK(s[3].level0 == 0 && s[3].duration0 == 32767 && s[3].level1 == 0 && s[3].duration1 == 1699);
    CHECK(s[4].level0 == 1 && s[4].duration0 == 600 && s[4].level1 == 0 && s[4].duration1 == 1);
    uint64_t sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += s[i].duration0 + s[i].duration1;
    }
    CHECK_EQ(sum - 1, ir_code_duration_us(&c)); /* only the closing 1 us is added */

    /* receiver symbols (RMT RX: level 1 = mark, final entry with duration 0 = end marker) */
    ir_code_t r;
    uint16_t rs[16];
    ir_code_init(&r, rs, 16, 38000);
    const ir_symbol_t rx[] = { { 9000, 1, 4500, 0 }, { 560, 1, 1690, 0 }, { 560, 1, 0, 0 } };
    CHECK_EQ(ir_code_append_symbols(&r, rx, 3, 0), IR_OK);
    CHECK_EQ(r.total_levels, 5);
    CHECK(rs[0] == 9000 && rs[1] == 4500 && rs[2] == 560 && rs[3] == 1690 && rs[4] == 560);
}

static void test_capture(void)
{
    const uint32_t idle = 12000;
    uint16_t store[64];
    ir_capture_t cap;
    ir_capture_reset(&cap, store, 64, 38000, idle, 4);

    const ir_symbol_t seg1[] = { { 9000, 1, 4500, 0 }, { 560, 1, 0, 0 } }; /* 14060 us */
    const ir_symbol_t seg2[] = { { 9000, 1, 2250, 0 }, { 560, 1, 0, 0 } }; /* 11810 us */
    int64_t start1 = 1000000;
    int64_t irq1 = start1 + 14060 + idle;
    int64_t start2 = start1 + 14060 + 40000; /* 40 ms of silence: longer than the RMT idle limit */
    int64_t irq2 = start2 + 11810 + idle;
    CHECK_EQ(ir_capture_add(&cap, seg1, 2, irq1, false), IR_OK);
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2, false), IR_OK);
    CHECK_EQ(cap.code.segment_count, 2);
    CHECK_EQ(cap.code.segments[0].gap_before_us, 0);
    CHECK_EQ(cap.code.segments[1].gap_before_us, 40000); /* reconstructed exactly */
    CHECK(ir_capture_is_noise(&cap, 16));                  /* only 6 levels */
    CHECK(!ir_capture_is_noise(&cap, 6));

    /* interrupt jitter cannot create a negative gap */
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2 + 100, false), IR_OK);
    CHECK_EQ(cap.code.segments[2].gap_before_us, 0);

    /* empty segment (silence only) is ignored */
    const ir_symbol_t silent[] = { { 3000, 0, 0, 0 } };
    CHECK_EQ(ir_capture_add(&cap, silent, 1, irq2 + 50000, false), IR_ERR_EMPTY);
    CHECK_EQ(cap.error, IR_OK);

    /* segment limit (4) */
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2 + 90000, false), IR_OK);
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2 + 190000, false), IR_ERR_SEGMENTS_FULL);
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2 + 290000, false), IR_ERR_SEGMENTS_FULL); /* sticky */

    /* receiver buffer overflow is an error, never a silently shortened frame */
    ir_capture_reset(&cap, store, 64, 38000, idle, 4);
    CHECK_EQ(ir_capture_add(&cap, seg1, 2, irq1, true), IR_ERR_LEVELS_FULL);
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2, false), IR_ERR_LEVELS_FULL);

    /* storage overflow while assembling */
    uint16_t tiny[4];
    ir_capture_reset(&cap, tiny, 4, 38000, idle, 4);
    CHECK_EQ(ir_capture_add(&cap, seg1, 2, irq1, false), IR_OK); /* 3 levels */
    CHECK_EQ(ir_capture_add(&cap, seg2, 2, irq2, false), IR_ERR_LEVELS_FULL);
    CHECK_EQ(cap.code.total_levels, 3);
}

void test_ir_code(void)
{
    test_normalise();
    test_capacity();
    test_serialize();
    test_symbols();
}

void test_ir_capture(void)
{
    test_capture();
}
