#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "climate_algo.h"
#include "test.h"

#define MIN_MS 60000LL

static climate_params_t params(bool external)
{
    climate_params_t p;
    memset(&p, 0, sizeof(p));
    p.external = external ? 1 : 0;
    p.deadband_centi = 50;
    p.max_offset_centi = 300;
    p.comp_interval_ms = 10 * MIN_MS;
    p.filter_tau_ms = 0; /* deterministic: filtered = median */
    p.temp_step_half = 2;
    p.min_half[AC_MODE_COOL] = 32; /* 16 C */
    p.max_half[AC_MODE_COOL] = 60; /* 30 C */
    p.min_half[AC_MODE_HEAT] = 32;
    p.max_half[AC_MODE_HEAT] = 60;
    return p;
}

static climate_inputs_t inputs(uint8_t mode, int16_t target, bool valid, int64_t now_ms)
{
    climate_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.user_mode = mode;
    in.cool_target_centi = target;
    in.heat_target_centi = target;
    in.temp_valid = valid;
    in.now_ms = now_ms;
    return in;
}

static void feed3(climate_algo_t *a, const climate_params_t *p, int16_t v, int64_t now)
{
    for (int i = 0; i < 3; i++) {
        climate_algo_feed(a, p, v, now);
    }
}

static void test_quantize(void)
{
    CHECK_EQ(climate_quantize(2450, 2, false), 2500); /* tie: up when cooling */
    CHECK_EQ(climate_quantize(2450, 2, true), 2400);  /* tie: down when heating */
    CHECK_EQ(climate_quantize(2449, 2, false), 2400);
    CHECK_EQ(climate_quantize(2451, 2, true), 2500);
    CHECK_EQ(climate_quantize(2425, 1, false), 2450);
    CHECK_EQ(climate_quantize(2425, 1, true), 2400);
    CHECK_EQ(climate_quantize(2474, 1, false), 2450);
    CHECK_EQ(climate_quantize(-50, 2, false), 0);
    CHECK_EQ(climate_quantize(-50, 2, true), -100);
}

static void test_off_and_direct(void)
{
    climate_params_t p = params(true);
    climate_algo_t a;
    climate_output_t o;
    climate_algo_init(&a);
    feed3(&a, &p, 3200, 0); /* very hot room */
    /* Off always wins: the algorithm never powers the AC on */
    for (int64_t t = 0; t < 120 * MIN_MS; t += MIN_MS) {
        climate_inputs_t in = inputs(CLIMATE_MODE_OFF, 2400, true, t);
        climate_algo_step(&a, &p, &in, &o);
        CHECK_EQ(o.desired.power, 0);
        CHECK_EQ(o.reason, CLIMATE_REASON_OFF);
    }

    climate_params_t d = params(false);
    climate_algo_init(&a);
    climate_inputs_t in = inputs(CLIMATE_MODE_COOL, 2450, true, 0);
    climate_algo_step(&a, &d, &in, &o);
    CHECK_EQ(o.desired.power, 1);
    CHECK_EQ(o.desired.mode, AC_MODE_COOL);
    CHECK_EQ(o.desired.temp_half, 50);
    CHECK_EQ(o.reason, CLIMATE_REASON_DIRECT);
    CHECK(!o.compensating);

    /* target outside the learned range is clamped to it */
    in = inputs(CLIMATE_MODE_COOL, 1400, true, 0);
    climate_algo_step(&a, &d, &in, &o);
    CHECK_EQ(o.desired.temp_half, 32);

    /* fan only: no temperature, no compensation */
    in = inputs(CLIMATE_MODE_FAN_ONLY, 2500, true, 0);
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.desired.mode, AC_MODE_FAN);
    CHECK_EQ(o.desired.temp_half, AC_TEMP_NONE);
    CHECK_EQ(o.reason, CLIMATE_REASON_NO_COMPENSATION);

    /* dry in external mode: no compensation */
    in = inputs(CLIMATE_MODE_DRY, 2500, true, 0);
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.desired.mode, AC_MODE_DRY);
    CHECK_EQ(o.offset_centi, 0);
    CHECK_EQ(o.reason, CLIMATE_REASON_NO_COMPENSATION);
}

static void test_compensation(void)
{
    climate_params_t p = params(true);
    climate_algo_t a;
    climate_output_t o;
    climate_algo_init(&a);

    /* no valid sensor: direct behaviour */
    climate_inputs_t in = inputs(CLIMATE_MODE_COOL, 2500, false, 0);
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_NO_SENSOR);
    CHECK_EQ(o.ir_setpoint_centi, 2500);
    CHECK(!o.compensating);

    /* room 27 C, target 25 C: first a full interval on the plain setpoint */
    feed3(&a, &p, 2700, 0);
    in = inputs(CLIMATE_MODE_COOL, 2500, true, 0);
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_WAITING);
    CHECK_EQ(o.ir_setpoint_centi, 2500);
    in.now_ms = 9 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_WAITING);
    /* one step per interval, bounded by max_offset (3 C) */
    int16_t expect[] = { 2400, 2300, 2200, 2200 };
    climate_reason_t reasons[] = { CLIMATE_REASON_CORRECTED, CLIMATE_REASON_CORRECTED, CLIMATE_REASON_CORRECTED,
                                   CLIMATE_REASON_AT_LIMIT };
    for (int k = 0; k < 4; k++) {
        in.now_ms = (10 + 10 * k) * MIN_MS;
        climate_algo_step(&a, &p, &in, &o);
        CHECK_EQ(o.ir_setpoint_centi, expect[k]);
        CHECK_EQ(o.reason, reasons[k]);
        CHECK(o.compensating);
        in.now_ms += 1; /* immediately after: waits */
        climate_algo_step(&a, &p, &in, &o);
        CHECK(o.reason == CLIMATE_REASON_WAITING || o.reason == CLIMATE_REASON_AT_LIMIT);
        CHECK_EQ(o.ir_setpoint_centi, expect[k]);
    }
    CHECK_EQ(o.offset_centi, -300);

    /* inside the deadband: hold */
    feed3(&a, &p, 2530, 50 * MIN_MS);
    in.now_ms = 60 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_IN_DEADBAND);
    CHECK_EQ(o.ir_setpoint_centi, 2200);

    /* overcooled: back off one step per interval */
    feed3(&a, &p, 2400, 61 * MIN_MS);
    in.now_ms = 70 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_CORRECTED);
    CHECK_EQ(o.ir_setpoint_centi, 2300);

    /* sensor lost: compensation stops at once (no stale value used) */
    in.temp_valid = false;
    in.now_ms = 71 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_NO_SENSOR);
    CHECK_EQ(o.offset_centi, 0);
    CHECK_EQ(o.ir_setpoint_centi, 2500);
    /* back: waits a full interval again before the first correction */
    in.temp_valid = true;
    feed3(&a, &p, 2700, 72 * MIN_MS);
    in.now_ms = 72 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_WAITING);
    in.now_ms = 82 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.ir_setpoint_centi, 2400);

    /* Off then On: no stale correction carried over */
    climate_inputs_t off = inputs(CLIMATE_MODE_OFF, 2500, true, 83 * MIN_MS);
    climate_algo_step(&a, &p, &off, &o);
    in.now_ms = 84 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.offset_centi, 0);
    CHECK_EQ(o.ir_setpoint_centi, 2500);

    /* mode change resets the correction (its sign depends on the mode) */
    in.now_ms = 94 * MIN_MS;
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.ir_setpoint_centi, 2400);
    climate_inputs_t heat = inputs(CLIMATE_MODE_HEAT, 2200, true, 95 * MIN_MS);
    climate_algo_step(&a, &p, &heat, &o);
    CHECK_EQ(o.offset_centi, 0);
    CHECK_EQ(o.desired.mode, AC_MODE_HEAT);
}

static void test_limits(void)
{
    /* anti-windup: the AC limit cuts the correction, the offset does not keep growing */
    climate_params_t p = params(true);
    climate_algo_t a;
    climate_output_t o;
    climate_algo_init(&a);
    feed3(&a, &p, 2000, 0);
    climate_inputs_t in = inputs(CLIMATE_MODE_COOL, 1700, true, 0);
    climate_algo_step(&a, &p, &in, &o);
    for (int k = 1; k <= 5; k++) {
        in.now_ms = 10 * k * MIN_MS;
        climate_algo_step(&a, &p, &in, &o);
        CHECK(o.ir_setpoint_centi >= 1600);
        CHECK(o.offset_centi >= -100);
    }
    CHECK_EQ(o.reason, CLIMATE_REASON_AT_LIMIT);
    /* the user raises the target: the remembered offset was cut to what the AC allowed (-1 C),
     * it did not keep growing while the limit was hit */
    in.cool_target_centi = 2400;
    in.now_ms = 60 * MIN_MS;
    climate_algo_note_user_change(&a, in.now_ms);
    climate_algo_step(&a, &p, &in, &o);
    CHECK_EQ(o.reason, CLIMATE_REASON_WAITING);
    CHECK_EQ(o.ir_setpoint_centi, 2300);
}

static void test_filter(void)
{
    climate_params_t p = params(true);
    climate_algo_t a;
    climate_algo_init(&a);
    climate_algo_feed(&a, &p, 2500, 0);
    climate_algo_feed(&a, &p, 2500, 1000);
    climate_algo_feed(&a, &p, 4000, 2000); /* single spike rejected by the median */
    CHECK(fabsf(a.filtered_centi - 2500.0f) < 0.01f);
    climate_algo_feed(&a, &p, 2510, 3000);
    CHECK(fabsf(a.filtered_centi - 2510.0f) < 0.01f);

    p.filter_tau_ms = 180000;
    climate_algo_init(&a);
    climate_algo_feed(&a, &p, 2500, 0);
    climate_algo_feed(&a, &p, 2600, 180000); /* median of 2 = 2550, alpha = 1 - 1/e */
    float expect = 2500.0f + (1.0f - expf(-1.0f)) * 50.0f;
    CHECK(fabsf(a.filtered_centi - expect) < 0.05f);
    climate_algo_reset_filter(&a);
    CHECK(!a.has_filtered);
}

void test_climate_algo(void)
{
    test_quantize();
    test_off_and_direct();
    test_compensation();
    test_limits();
    test_filter();
}
