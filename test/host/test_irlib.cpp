/*
 * Host tests of components/ac_profiles/ac_irlib.cpp with the vendored IRremoteESP8266:
 *  - every command generated for a protocol is recognised again as that protocol and state,
 *  - also after passing through ir_capture like a real receiver capture (segments split at 12 ms
 *    of silence, demodulator stretching marks and shortening spaces),
 *  - unsupported / unknown signals are reported as such.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "IRremoteESP8266.h"
#include "ac_irlib.h"
#include "ir_capture.h"
#include "ir_code.h"
#include "test.h"

int g_checks;
int g_failures;

namespace {

constexpr size_t kLevels = 4096;
uint16_t gStore[kLevels];
uint16_t gStore2[kLevels];

ac_irlib_config_t Config(decode_type_t p)
{
    ac_irlib_config_t c;
    c.protocol = static_cast<int16_t>(p);
    c.model = -1;
    c.fan_levels = 3;
    c.extra_flags = AC_IRLIB_X_LIGHT;
    c.swing_h = -1;
    return c;
}

/* Re-capture `in` the way the RMT receiver would: one segment per frame, marks +60 us and spaces
 * -60 us (demodulator delay), end-of-segment interrupt `idle` after the last edge. */
bool Recapture(const ir_code_t * in, ir_code_t * out_code, ir_capture_t * cap)
{
    const uint32_t idle = 12000;
    ir_capture_reset(cap, gStore2, kLevels, in->carrier_hz, idle, 32);
    int64_t t = 1000000;
    size_t lvl = 0;
    static ir_symbol_t syms[kLevels];
    for (uint8_t s = 0; s < in->segment_count; s++) {
        t += in->segments[s].gap_before_us;
        size_t n = 0;
        uint32_t seg_us = 0;
        for (uint16_t i = 0; i < in->segments[s].level_count; i += 2) {
            uint32_t mark = in->levels[lvl + i] + 60u;
            uint32_t space = (i + 1u < in->segments[s].level_count) ? in->levels[lvl + i + 1] - 60u : 0;
            syms[n].level0 = 1;
            syms[n].duration0 = static_cast<uint16_t>(mark);
            syms[n].level1 = 0;
            syms[n].duration1 = static_cast<uint16_t>(space);
            seg_us += mark + space;
            n++;
        }
        lvl += in->segments[s].level_count;
        t += seg_us;
        if (ir_capture_add(cap, syms, n, t + idle, false) != IR_OK) {
            return false;
        }
    }
    *out_code = cap->code;
    return true;
}

/* Protocols whose frames toggle power instead of stating it: on/off cannot be read back. */
bool TogglesPower(decode_type_t p)
{
    return p == KELON;
}

void CheckRoundTrip(decode_type_t p, const ac_state_t & st, bool check_fan, bool check_swing)
{
    char name[AC_IRLIB_NAME_LEN];
    ac_irlib_protocol_name(static_cast<int16_t>(p), name, sizeof(name));
    ac_irlib_config_t cfg = Config(p);
    ir_code_t code;
    ir_code_init(&code, gStore, kLevels, 0);
    ac_irlib_result_t r = ac_irlib_build(&cfg, &st, nullptr, &code);
    CHECK_EQ(r, AC_IRLIB_OK);
    if (r != AC_IRLIB_OK) {
        fprintf(stderr, "  protocol %s: build failed (%d)\n", name, r);
        return;
    }
    CHECK(code.carrier_hz >= 30000 && code.carrier_hz <= 60000);
    CHECK(code.segment_count >= 1);

    for (int pass = 0; pass < 2; pass++) {
        ir_code_t in = code;
        ir_code_t captured;
        ir_capture_t cap;
        if (pass == 1) {
            bool ok = Recapture(&code, &captured, &cap);
            CHECK(ok);
            if (!ok) {
                fprintf(stderr, "  protocol %s: recapture failed\n", name);
                return;
            }
            in = captured;
        }
        ac_irlib_detect_t d;
        CHECK_EQ(ac_irlib_decode(&in, &d), AC_IRLIB_OK);
        bool same = d.recognized && d.protocol == static_cast<int16_t>(p);
        CHECK(same);
        if (!same) {
            fprintf(stderr, "  protocol %s (pass %d): decoded as %s\n", name, pass, d.protocol_name);
            continue;
        }
        CHECK(d.supported);
        CHECK(d.has_state);
        if (!TogglesPower(p)) {
            CHECK_EQ(d.state.power, st.power);
        }
        if (st.power) {
            CHECK_EQ(d.state.mode, st.mode);
            CHECK_EQ(d.state.temp_half, st.temp_half);
            if (check_fan) {
                CHECK_EQ(d.state.fan, st.fan);
            }
            if (check_swing) {
                CHECK_EQ(d.state.swing, st.swing);
            }
            if (d.state.mode != st.mode || d.state.temp_half != st.temp_half) {
                fprintf(stderr, "  protocol %s (pass %d): mode %u temp %u\n", name, pass, d.state.mode,
                        d.state.temp_half);
            }
        }
    }
}

void TestFanMapping()
{
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_AUTO, 3), 0);
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_1, 3), 2); /* kLow */
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_2, 3), 3); /* kMedium */
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_3, 3), 4); /* kHigh */
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_5, 3), 4); /* beyond the remote's speeds: fastest */
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_1, 5), 1); /* kMin */
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_5, 5), 5); /* kMax */
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_2, 2), 4);
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_QUIET, 3), 1);
    CHECK_EQ(ac_irlib_fan_to_lib(AC_FAN_TURBO, 3), 5);
}

void TestProtocols()
{
    /* Protocols of AC brands common in Vietnam (and their OEM rebrands). */
    const decode_type_t kProtocols[] = {
        DAIKIN,      DAIKIN2,     DAIKIN216, PANASONIC_AC, MITSUBISHI_AC, MITSUBISHI_HEAVY_152, GREE,
        COOLIX,      MIDEA,       LG,        SAMSUNG_AC,   TOSHIBA_AC,    FUJITSU_AC,           HITACHI_AC,
        SHARP_AC,    HAIER_AC,    KELVINATOR, TCL112AC,    ELECTRA_AC,    CARRIER_AC64,         WHIRLPOOL_AC,
        AIRWELL,     NEOCLIMA,    MIRAGE,     KELON,       TECO,          VESTEL_AC,            TRANSCOLD,
    };
    const ac_state_t cool25 = { 1, AC_MODE_COOL, 50, AC_FAN_AUTO, AC_SWING_OFF };
    const ac_state_t cool18 = { 1, AC_MODE_COOL, 36, AC_FAN_AUTO, AC_SWING_OFF };
    for (decode_type_t p : kProtocols) {
        CHECK(ac_irlib_protocol_supported(static_cast<int16_t>(p)));
        CheckRoundTrip(p, cool25, false, false);
        CheckRoundTrip(p, cool18, false, false);
    }
    /* Fan speed and swing on protocols that carry them in every frame (Toshiba sends swing as a
     * separate command, so its state frame has none). */
    const decode_type_t kFull[] = { DAIKIN, PANASONIC_AC, GREE, MITSUBISHI_AC, SAMSUNG_AC, HAIER_AC, FUJITSU_AC };
    const ac_state_t cool26fan2 = { 1, AC_MODE_COOL, 52, AC_FAN_2, AC_SWING_ON };
    const ac_state_t dry24 = { 1, AC_MODE_DRY, 48, AC_FAN_AUTO, AC_SWING_OFF };
    const ac_state_t heat22 = { 1, AC_MODE_HEAT, 44, AC_FAN_3, AC_SWING_OFF };
    for (decode_type_t p : kFull) {
        CheckRoundTrip(p, cool26fan2, true, true);
        CheckRoundTrip(p, dry24, false, false);
        CheckRoundTrip(p, heat22, true, false);
    }
    CheckRoundTrip(TOSHIBA_AC, cool26fan2, true, false);
}

void TestPowerOff()
{
    const decode_type_t kProtocols[] = { DAIKIN, PANASONIC_AC, GREE, MITSUBISHI_AC, MIDEA, SAMSUNG_AC, TOSHIBA_AC };
    const ac_state_t off = { 0, 0, 0, 0, 0 };
    const ac_state_t prev = { 1, AC_MODE_COOL, 50, AC_FAN_AUTO, AC_SWING_OFF };
    for (decode_type_t p : kProtocols) {
        char name[AC_IRLIB_NAME_LEN];
        ac_irlib_protocol_name(static_cast<int16_t>(p), name, sizeof(name));
        ac_irlib_config_t cfg = Config(p);
        ir_code_t code;
        ir_code_init(&code, gStore, kLevels, 0);
        CHECK_EQ(ac_irlib_build(&cfg, &off, &prev, &code), AC_IRLIB_OK);
        ac_irlib_detect_t d;
        CHECK_EQ(ac_irlib_decode(&code, &d), AC_IRLIB_OK);
        CHECK(d.recognized && d.protocol == static_cast<int16_t>(p));
        CHECK(d.has_state);
        CHECK_EQ(d.state.power, 0);
        if (d.state.power != 0) {
            fprintf(stderr, "  protocol %s: power off decoded as on\n", name);
        }
    }
}

void TestModelsAndNames()
{
    ac_irlib_model_t models[AC_IRLIB_MAX_MODELS];
    size_t n = ac_irlib_models(FUJITSU_AC, models, AC_IRLIB_MAX_MODELS);
    CHECK(n >= 5);
    CHECK_EQ(models[0].model, 1);
    CHECK_STR(models[0].name, "ARRAH2E");
    CHECK(ac_irlib_models(GREE, models, AC_IRLIB_MAX_MODELS) >= 3);
    CHECK_EQ(ac_irlib_models(COOLIX, models, AC_IRLIB_MAX_MODELS), 0);
    char buf[AC_IRLIB_NAME_LEN];
    ac_irlib_protocol_name(DAIKIN, buf, sizeof(buf));
    CHECK_STR(buf, "DAIKIN");
    ac_irlib_model_name(FUJITSU_AC, 2, buf, sizeof(buf));
    CHECK_STR(buf, "ARDB1");
    ac_irlib_model_name(COOLIX, -1, buf, sizeof(buf));
    CHECK_STR(buf, "");
    CHECK_STR(ac_irlib_fan_label(0), "tự động");
}

/* A TV remote (NEC) is recognised but cannot drive an AC; noise is not recognised. */
void TestUnsupportedAndNoise()
{
    ac_irlib_config_t cfg = Config(NEC);
    ac_state_t cool = { 1, AC_MODE_COOL, 50, AC_FAN_AUTO, AC_SWING_OFF };
    ir_code_t code;
    ir_code_init(&code, gStore, kLevels, 38000);
    CHECK_EQ(ac_irlib_build(&cfg, &cool, nullptr, &code), AC_IRLIB_ERR_UNSUPPORTED);
    CHECK(!ac_irlib_protocol_supported(NEC));

    uint32_t nec[67];
    uint8_t lv[67];
    size_t n = 0;
    nec[n++] = 9000;
    nec[n++] = 4500;
    uint32_t data = 0x20DF10EFu; /* a TV power key */
    for (int i = 31; i >= 0; i--) {
        nec[n++] = 560;
        nec[n++] = ((data >> i) & 1u) ? 1690 : 560;
    }
    nec[n++] = 560;
    for (size_t i = 0; i < n; i++) {
        lv[i] = (i % 2 == 0) ? 1 : 0;
    }
    ir_code_init(&code, gStore, kLevels, 38000);
    CHECK_EQ(ir_code_append_segment(&code, nec, lv, n, 0), IR_OK);
    ac_irlib_detect_t d;
    CHECK_EQ(ac_irlib_decode(&code, &d), AC_IRLIB_OK);
    CHECK(d.recognized);
    CHECK_STR(d.protocol_name, "NEC");
    CHECK(!d.supported);
    CHECK(!d.has_state);

    uint32_t noise[40];
    srand(7);
    for (size_t i = 0; i < 40; i++) {
        noise[i] = 200u + static_cast<uint32_t>(rand() % 3000);
        lv[i] = (i % 2 == 0) ? 1 : 0;
    }
    ir_code_init(&code, gStore, kLevels, 38000);
    CHECK_EQ(ir_code_append_segment(&code, noise, lv, 39, 0), IR_OK);
    CHECK_EQ(ac_irlib_decode(&code, &d), AC_IRLIB_OK);
    CHECK(!d.recognized || !d.supported);
    ir_code_init(&code, gStore, kLevels, 38000);
    CHECK_EQ(ac_irlib_decode(&code, &d), AC_IRLIB_ERR_EMPTY);
}

} // namespace

int main()
{
    struct {
        const char * name;
        void (*fn)();
    } suites[] = {
        { "irlib_fan", TestFanMapping },     { "irlib_protocols", TestProtocols },
        { "irlib_power_off", TestPowerOff }, { "irlib_models", TestModelsAndNames },
        { "irlib_unknown", TestUnsupportedAndNoise },
    };
    for (auto & s : suites) {
        int before = g_failures;
        s.fn();
        printf("%-16s %s\n", s.name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
