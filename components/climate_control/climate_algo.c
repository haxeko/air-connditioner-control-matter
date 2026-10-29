#include "climate_algo.h"

#include <math.h>
#include <string.h>

void climate_algo_init(climate_algo_t *a)
{
    memset(a, 0, sizeof(*a));
    a->last_ac_mode = 0xFF;
}

void climate_algo_reset_filter(climate_algo_t *a)
{
    a->has_filtered = false;
    a->window_n = 0;
    a->window_pos = 0;
    a->last_sample_ms = 0;
}

static int16_t median3(const int16_t *w, uint8_t n)
{
    if (n == 1) {
        return w[0];
    }
    if (n == 2) {
        return (int16_t)(((int32_t)w[0] + w[1]) / 2);
    }
    int16_t a = w[0], b = w[1], c = w[2];
    if ((a <= b && b <= c) || (c <= b && b <= a)) {
        return b;
    }
    if ((b <= a && a <= c) || (c <= a && a <= b)) {
        return a;
    }
    return c;
}

void climate_algo_feed(climate_algo_t *a, const climate_params_t *p, int16_t value_centi, int64_t now_ms)
{
    a->window[a->window_pos] = value_centi;
    a->window_pos = (uint8_t)((a->window_pos + 1) % 3);
    if (a->window_n < 3) {
        a->window_n++;
    }
    int16_t m = median3(a->window, a->window_n);
    if (!a->has_filtered || p->filter_tau_ms == 0) {
        a->filtered_centi = (float)m;
        a->has_filtered = true;
    } else {
        int64_t dt = now_ms - a->last_sample_ms;
        if (dt < 0) {
            dt = 0;
        }
        float alpha = 1.0f - expf(-(float)dt / (float)p->filter_tau_ms);
        a->filtered_centi += alpha * ((float)m - a->filtered_centi);
    }
    a->last_sample_ms = now_ms;
}

void climate_algo_note_user_change(climate_algo_t *a, int64_t now_ms)
{
    a->last_comp_ms = now_ms;
}

int16_t climate_quantize(int16_t centi, uint8_t step_half, bool heating)
{
    int32_t step = (step_half == 1) ? 50 : 100;
    int32_t v = centi;
    int32_t q = v / step;
    int32_t r = v - q * step;
    if (r < 0) { /* floor for negatives */
        q -= 1;
        r += step;
    }
    int32_t lo = q * step;
    int32_t hi = lo + step;
    if (r * 2 > step) {
        return (int16_t)hi;
    }
    if (r * 2 < step) {
        return (int16_t)lo;
    }
    return (int16_t)(heating ? lo : hi);
}

static uint8_t ac_mode_of(uint8_t user_mode, bool *ok)
{
    *ok = true;
    switch (user_mode) {
    case CLIMATE_MODE_COOL:
        return AC_MODE_COOL;
    case CLIMATE_MODE_HEAT:
        return AC_MODE_HEAT;
    case CLIMATE_MODE_DRY:
        return AC_MODE_DRY;
    case CLIMATE_MODE_FAN_ONLY:
        return AC_MODE_FAN;
    default:
        *ok = false;
        return AC_MODE_COOL;
    }
}

static int16_t clamp16(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return (int16_t)lo;
    }
    if (v > hi) {
        return (int16_t)hi;
    }
    return (int16_t)v;
}

void climate_algo_step(climate_algo_t *a, const climate_params_t *p, const climate_inputs_t *in, climate_output_t *out)
{
    memset(out, 0, sizeof(*out));
    bool ok;
    uint8_t ac_mode = ac_mode_of(in->user_mode, &ok);
    if (in->user_mode == CLIMATE_MODE_OFF || !ok) {
        /* The user's Off wins; nothing here can switch the AC on again. A later On starts
         * without a stale correction. */
        out->desired.power = 0;
        out->reason = CLIMATE_REASON_OFF;
        a->offset_centi = 0;
        a->comp_active = false;
        a->last_ac_mode = 0xFF;
        return;
    }
    if (ac_mode != a->last_ac_mode) {
        a->offset_centi = 0; /* the sign of a correction depends on the mode */
        a->comp_active = false;
        a->last_ac_mode = ac_mode;
    }

    out->desired.power = 1;
    out->desired.mode = ac_mode;
    out->desired.fan = in->fan < AC_FAN_COUNT ? in->fan : AC_FAN_AUTO;
    out->desired.swing = in->swing < AC_SWING_COUNT ? in->swing : AC_SWING_OFF;

    if (ac_mode == AC_MODE_FAN) {
        out->desired.temp_half = AC_TEMP_NONE;
        out->reason = CLIMATE_REASON_NO_COMPENSATION;
        a->comp_active = false;
        return;
    }

    bool heating = (ac_mode == AC_MODE_HEAT);
    int16_t target = heating ? in->heat_target_centi : in->cool_target_centi;
    int16_t step = (p->temp_step_half == 1) ? 50 : 100;
    int16_t lo = (int16_t)(p->min_half[ac_mode] ? p->min_half[ac_mode] * 50 : 1600);
    int16_t hi = (int16_t)(p->max_half[ac_mode] ? p->max_half[ac_mode] * 50 : 3000);
    if (hi < lo) {
        hi = lo;
    }
    int16_t target_q = clamp16(climate_quantize(target, p->temp_step_half, heating), lo, hi);
    out->target_centi = target;
    out->user_target_half = (uint8_t)(target_q / 50);

    bool can_compensate = p->external && (ac_mode == AC_MODE_COOL || ac_mode == AC_MODE_HEAT);
    if (!can_compensate) {
        a->offset_centi = 0;
        a->comp_active = false;
        out->reason = (ac_mode == AC_MODE_DRY && p->external) ? CLIMATE_REASON_NO_COMPENSATION : CLIMATE_REASON_DIRECT;
    } else if (!in->temp_valid || !a->has_filtered) {
        /* sensor unavailable: stop compensating, behave like DIRECT; restart cleanly later */
        a->offset_centi = 0;
        a->comp_active = false;
        out->reason = CLIMATE_REASON_NO_SENSOR;
    } else {
        if (!a->comp_active) {
            a->comp_active = true;
            a->last_comp_ms = in->now_ms; /* let the AC act on the plain setpoint first */
        }
        int32_t err = heating ? ((int32_t)target - (int32_t)lrintf(a->filtered_centi))
                              : ((int32_t)lrintf(a->filtered_centi) - (int32_t)target);
        int32_t delta = 0; /* change of the IR setpoint */
        if (err > p->deadband_centi) {
            delta = heating ? step : -step; /* more conditioning */
        } else if (err < -p->deadband_centi) {
            delta = heating ? -step : step; /* less conditioning */
        }
        if (delta == 0) {
            out->reason = CLIMATE_REASON_IN_DEADBAND;
        } else if (in->now_ms - a->last_comp_ms < (int64_t)p->comp_interval_ms) {
            out->reason = CLIMATE_REASON_WAITING;
        } else {
            int32_t max_off = p->max_offset_centi;
            int32_t proposed = clamp16((int32_t)a->offset_centi + delta, -max_off, max_off);
            int32_t ir = clamp16((int32_t)target_q + proposed, lo, hi);
            int32_t effective = ir - target_q;
            if (effective != a->offset_centi) {
                a->offset_centi = (int16_t)effective;
                a->last_comp_ms = in->now_ms;
                out->reason = CLIMATE_REASON_CORRECTED;
            } else {
                out->reason = CLIMATE_REASON_AT_LIMIT;
            }
        }
        out->compensating = true;
    }

    int16_t ir = clamp16((int32_t)target_q + a->offset_centi, lo, hi);
    a->offset_centi = (int16_t)(ir - target_q); /* anti-windup when the AC limits cut the offset */
    out->offset_centi = a->offset_centi;
    out->ir_setpoint_centi = ir;
    out->desired.temp_half = (uint8_t)(ir / 50);
}

const char *climate_reason_text(climate_reason_t r)
{
    switch (r) {
    case CLIMATE_REASON_OFF:
        return "Tắt (lệnh của người dùng)";
    case CLIMATE_REASON_DIRECT:
        return "Direct: gửi nhiệt độ đặt của người dùng";
    case CLIMATE_REASON_NO_SENSOR:
        return "Không có nhiệt độ hợp lệ: ngừng bù, dùng nhiệt độ đặt";
    case CLIMATE_REASON_IN_DEADBAND:
        return "Trong vùng chết: giữ nguyên";
    case CLIMATE_REASON_WAITING:
        return "Cần bù nhưng chờ đủ chu kỳ";
    case CLIMATE_REASON_CORRECTED:
        return "Đã chỉnh setpoint IR một bước";
    case CLIMATE_REASON_AT_LIMIT:
        return "Đã chạm giới hạn bù/giới hạn máy lạnh";
    case CLIMATE_REASON_NO_COMPENSATION:
        return "Chế độ không bù theo nhiệt độ";
    default:
        return "?";
    }
}
