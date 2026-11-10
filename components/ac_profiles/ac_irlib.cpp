/*
 * IRremoteESP8266 glue: decode a captured command, generate full-state commands.
 * See include/ac_irlib.h. The library is built in its no-Arduino mode (UNIT_TEST=1); the pulses it
 * "sends" are collected by ir_recorder (components/irremote_esp8266/port).
 */
#include "ac_irlib.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>

#include "IRac.h"
#include "IRrecv.h"
#include "IRremoteESP8266.h"
#include "IRtext.h"
#include "IRutils.h"
#include "ir_recorder.h"

namespace {

std::mutex sMutex;

/* Silence that separates two segments, like the RMT receiver (CONFIG_APP_IR_RX_IDLE_US default). */
constexpr uint32_t kSegmentGapUs = 12000;
/* Longest generated command: AC protocols use a few hundred pulses (several frames, repeats). */
constexpr size_t kMaxPulses = 4096;
/* Matching tolerances tried in turn (percent). 25 is the library default. */
constexpr uint8_t kTolerances[] = { 25, 35 };

using stdAc::fanspeed_t;

/* Library fan speeds for a remote with 2..5 speeds, slowest first. */
const fanspeed_t kFans2[] = { fanspeed_t::kLow, fanspeed_t::kHigh };
const fanspeed_t kFans3[] = { fanspeed_t::kLow, fanspeed_t::kMedium, fanspeed_t::kHigh };
const fanspeed_t kFans4[] = { fanspeed_t::kLow, fanspeed_t::kMedium, fanspeed_t::kHigh, fanspeed_t::kMax };
const fanspeed_t kFans5[] = { fanspeed_t::kMin, fanspeed_t::kLow, fanspeed_t::kMedium, fanspeed_t::kHigh,
                              fanspeed_t::kMax };

const fanspeed_t * FanTable(uint8_t levels, uint8_t * count)
{
    switch (levels) {
    case 2:
        *count = 2;
        return kFans2;
    case 4:
        *count = 4;
        return kFans4;
    case 5:
        *count = 5;
        return kFans5;
    default:
        *count = 3;
        return kFans3;
    }
}

/* Position on a common scale (kMediumHigh sits between medium and high). */
int FanRank(fanspeed_t f)
{
    switch (f) {
    case fanspeed_t::kMin:
        return 2;
    case fanspeed_t::kLow:
        return 4;
    case fanspeed_t::kMedium:
        return 6;
    case fanspeed_t::kMediumHigh:
        return 7;
    case fanspeed_t::kHigh:
        return 8;
    case fanspeed_t::kMax:
        return 10;
    default:
        return 0;
    }
}

fanspeed_t ToLibFan(uint8_t fan, uint8_t levels, bool * quiet, bool * turbo)
{
    *quiet = false;
    *turbo = false;
    if (fan == AC_FAN_QUIET) {
        *quiet = true;
        return fanspeed_t::kMin;
    }
    if (fan == AC_FAN_TURBO) {
        *turbo = true;
        return fanspeed_t::kMax;
    }
    if (fan < AC_FAN_1 || fan > AC_FAN_5) {
        return fanspeed_t::kAuto;
    }
    uint8_t n;
    const fanspeed_t * table = FanTable(levels, &n);
    unsigned idx = static_cast<unsigned>(fan - AC_FAN_1);
    return table[idx < n ? idx : n - 1u];
}

uint8_t FromLibFan(fanspeed_t f, uint8_t levels)
{
    if (f == fanspeed_t::kAuto || FanRank(f) == 0) {
        return AC_FAN_AUTO;
    }
    uint8_t n;
    const fanspeed_t * table = FanTable(levels, &n);
    uint8_t best = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (abs(FanRank(table[i]) - FanRank(f)) < abs(FanRank(table[best]) - FanRank(f))) {
            best = i;
        }
    }
    return static_cast<uint8_t>(AC_FAN_1 + best);
}

stdAc::opmode_t ToLibMode(uint8_t mode)
{
    switch (mode) {
    case AC_MODE_HEAT:
        return stdAc::opmode_t::kHeat;
    case AC_MODE_DRY:
        return stdAc::opmode_t::kDry;
    case AC_MODE_FAN:
        return stdAc::opmode_t::kFan;
    case AC_MODE_AUTO:
        return stdAc::opmode_t::kAuto;
    default:
        return stdAc::opmode_t::kCool;
    }
}

bool FromLibMode(stdAc::opmode_t m, uint8_t * mode)
{
    switch (m) {
    case stdAc::opmode_t::kCool:
        *mode = AC_MODE_COOL;
        return true;
    case stdAc::opmode_t::kHeat:
        *mode = AC_MODE_HEAT;
        return true;
    case stdAc::opmode_t::kDry:
        *mode = AC_MODE_DRY;
        return true;
    case stdAc::opmode_t::kFan:
        *mode = AC_MODE_FAN;
        return true;
    case stdAc::opmode_t::kAuto:
        *mode = AC_MODE_AUTO;
        return true;
    default:
        return false;
    }
}

/* Protocols with several remote models need a valid one: model -1 makes some (e.g. Fujitsu) emit
 * frames their own decoder rejects. Use the first model when none was chosen. */
int16_t EffectiveModel(decode_type_t protocol, int16_t model)
{
    if (model >= 1) {
        return model;
    }
    return irutils::modelToStr(protocol, 1) != kUnknownStr ? 1 : -1;
}

/* `fallback` provides mode/temp/fan/swing for a power-off command. */
stdAc::state_t ToLibState(const ac_irlib_config_t * cfg, const ac_state_t * s, const ac_state_t * fallback)
{
    stdAc::state_t st;
    IRac::initState(&st);
    st.protocol = static_cast<decode_type_t>(cfg->protocol);
    st.model = EffectiveModel(st.protocol, cfg->model);
    st.celsius = true;
    st.light = (cfg->extra_flags & AC_IRLIB_X_LIGHT) != 0;
    st.beep = (cfg->extra_flags & AC_IRLIB_X_BEEP) != 0;
    st.econo = (cfg->extra_flags & AC_IRLIB_X_ECONO) != 0;
    st.filter = (cfg->extra_flags & AC_IRLIB_X_FILTER) != 0;
    st.clean = (cfg->extra_flags & AC_IRLIB_X_CLEAN) != 0;
    st.swingh = (cfg->swing_h >= -1 && cfg->swing_h <= static_cast<int8_t>(stdAc::swingh_t::kLastSwinghEnum))
        ? static_cast<stdAc::swingh_t>(cfg->swing_h)
        : stdAc::swingh_t::kOff;
    st.power = s->power != 0;
    /* A real remote sends its current settings with power off: use the last state if known. */
    const ac_state_t * src = s;
    ac_state_t cool25 = { 1, AC_MODE_COOL, 50, AC_FAN_AUTO, AC_SWING_OFF };
    if (!st.power) {
        src = (fallback != nullptr && fallback->power) ? fallback : &cool25;
    }
    st.mode = ToLibMode(src->mode);
    st.degrees = src->temp_half != AC_TEMP_NONE ? src->temp_half / 2.0f : 25.0f;
    bool quiet;
    bool turbo;
    st.fanspeed = ToLibFan(src->fan, cfg->fan_levels, &quiet, &turbo);
    st.quiet = quiet;
    st.turbo = turbo;
    st.swingv = src->swing == AC_SWING_ON ? stdAc::swingv_t::kAuto : stdAc::swingv_t::kOff;
    return st;
}

void CopyName(const std::string & s, char * buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    size_t n = s.size() < len - 1 ? s.size() : len - 1;
    memcpy(buf, s.data(), n);
    buf[n] = 0;
}

uint16_t Ticks(uint32_t usec)
{
    uint32_t t = (usec + kRawTick / 2) / kRawTick;
    return t > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(t);
}

ac_irlib_result_t FromIrResult(ir_result_t r)
{
    switch (r) {
    case IR_OK:
        return AC_IRLIB_OK;
    case IR_ERR_LEVELS_FULL:
    case IR_ERR_SEGMENTS_FULL:
        return AC_IRLIB_ERR_TOO_LONG;
    case IR_ERR_EMPTY:
        return AC_IRLIB_ERR_EMPTY;
    default:
        return AC_IRLIB_ERR_ARG;
    }
}

void FillDetect(const decode_results & r, ac_irlib_detect_t * out)
{
    out->recognized = true;
    out->protocol = static_cast<int16_t>(r.decode_type);
    CopyName(typeToString(r.decode_type), out->protocol_name, sizeof(out->protocol_name));
    out->bits = r.bits;
    out->supported = IRac::isProtocolSupported(r.decode_type);
    if (!out->supported) {
        return;
    }
    stdAc::state_t st;
    IRac::initState(&st);
    if (!IRAcUtils::decodeToState(&r, &st, nullptr)) {
        return;
    }
    out->model = st.model;
    out->fan_raw = static_cast<uint8_t>(st.fanspeed);
    out->extra_flags = static_cast<uint8_t>((st.light ? AC_IRLIB_X_LIGHT : 0u) | (st.beep ? AC_IRLIB_X_BEEP : 0u) |
                                            (st.econo ? AC_IRLIB_X_ECONO : 0u) | (st.filter ? AC_IRLIB_X_FILTER : 0u) |
                                            (st.clean ? AC_IRLIB_X_CLEAN : 0u));
    out->swing_h = static_cast<int8_t>(st.swingh);

    /* Settings are read even when the frame says "off": protocols that toggle power (e.g. Kelon)
     * cannot tell on/off from one frame, their settings are still meaningful. */
    ac_state_t s;
    memset(&s, 0, sizeof(s));
    uint8_t mode = AC_MODE_COOL;
    if (FromLibMode(st.mode, &mode)) {
        s.power = 1;
        s.mode = mode;
        if (mode == AC_MODE_FAN) {
            s.temp_half = AC_TEMP_NONE;
        } else {
            float c = st.celsius ? st.degrees : fahrenheitToCelsius(st.degrees);
            long half = lroundf(c * 2.0f);
            s.temp_half = static_cast<uint8_t>(half < AC_TEMP_HALF_MIN ? AC_TEMP_HALF_MIN
                                                                       : (half > AC_TEMP_HALF_MAX ? AC_TEMP_HALF_MAX : half));
        }
        s.fan = st.quiet ? static_cast<uint8_t>(AC_FAN_QUIET)
                         : (st.turbo ? static_cast<uint8_t>(AC_FAN_TURBO) : FromLibFan(st.fanspeed, 3));
        s.swing = st.swingv != stdAc::swingv_t::kOff ? AC_SWING_ON : AC_SWING_OFF;
    }
    if (ac_state_valid(&s)) {
        s.power = st.power ? 1 : 0; /* kept separately: an "off" state still shows its settings */
        out->state = s;
        out->has_state = true;
    }
}

} // namespace

extern "C" ac_irlib_result_t ac_irlib_decode(const ir_code_t * code, ac_irlib_detect_t * out)
{
    if (code == nullptr || out == nullptr) {
        return AC_IRLIB_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->protocol = static_cast<int16_t>(decode_type_t::UNKNOWN);
    out->model = -1;
    out->swing_h = -1;
    strcpy(out->protocol_name, "UNKNOWN");
    if (code->segment_count == 0 || code->total_levels == 0) {
        return AC_IRLIB_ERR_EMPTY;
    }
    /* rawbuf as filled by IRrecv: [0] = silence before the message (not used), then
     * mark, space, ... in kRawTick units; the gaps between segments are spaces. */
    size_t n = 1u + code->total_levels + (code->segment_count - 1u);
    if (n > UINT16_MAX) {
        return AC_IRLIB_ERR_TOO_LONG;
    }
    uint16_t * raw = static_cast<uint16_t *>(malloc(n * sizeof(uint16_t)));
    if (raw == nullptr) {
        return AC_IRLIB_ERR_NO_MEM;
    }
    size_t k = 0;
    raw[k++] = UINT16_MAX;
    size_t lvl = 0;
    for (uint8_t s = 0; s < code->segment_count; s++) {
        if (s > 0) {
            raw[k++] = Ticks(code->segments[s].gap_before_us);
        }
        for (uint16_t i = 0; i < code->segments[s].level_count; i++) {
            raw[k++] = Ticks(code->levels[lvl++]);
        }
    }

    std::lock_guard<std::mutex> guard(sMutex);
    IRrecv recv(0, 16, kTimeoutMs, false); /* own buffer unused: decode() reads `raw` (UNIT_TEST mode) */
    for (uint8_t tol : kTolerances) {
        recv.setTolerance(tol);
        decode_results r;
        r.rawbuf = raw;
        r.rawlen = static_cast<uint16_t>(k);
        r.overflow = false;
        if (recv.decode(&r) && r.decode_type != decode_type_t::UNKNOWN) {
            FillDetect(r, out);
            break;
        }
    }
    free(raw);
    return AC_IRLIB_OK;
}

extern "C" ac_irlib_result_t ac_irlib_build(const ac_irlib_config_t * cfg, const ac_state_t * state,
                                            const ac_state_t * prev, ir_code_t * code)
{
    if (cfg == nullptr || state == nullptr || code == nullptr || code->levels == nullptr || !ac_state_valid(state)) {
        return AC_IRLIB_ERR_ARG;
    }
    decode_type_t protocol = static_cast<decode_type_t>(cfg->protocol);
    if (!IRac::isProtocolSupported(protocol)) {
        return AC_IRLIB_ERR_UNSUPPORTED;
    }
    bool prev_ok = prev != nullptr && ac_state_valid(prev);
    stdAc::state_t want = ToLibState(cfg, state, prev_ok ? prev : nullptr);
    /* Without a known previous state assume only the power changes, so toggle-type settings (light,
     * swing on some protocols) are not flipped by accident. */
    stdAc::state_t before = want;
    before.power = !want.power;
    if (prev_ok) {
        before = ToLibState(cfg, prev, nullptr);
    }

    uint32_t * pulses = static_cast<uint32_t *>(malloc(kMaxPulses * sizeof(uint32_t)));
    uint8_t * levels = static_cast<uint8_t *>(malloc(kMaxPulses));
    if (pulses == nullptr || levels == nullptr) {
        free(pulses);
        free(levels);
        return AC_IRLIB_ERR_NO_MEM;
    }
    ir_recorder::Capture cap = { pulses, kMaxPulses, 0, false, 0, 0 };
    bool sent;
    {
        std::lock_guard<std::mutex> guard(sMutex);
        IRac ac(0); /* no GPIO is touched: every pulse goes to the recorder */
        ir_recorder::begin(&cap);
        sent = ac.sendAc(want, &before);
        ir_recorder::end();
    }

    ac_irlib_result_t res = AC_IRLIB_OK;
    if (!sent) {
        res = AC_IRLIB_ERR_UNSUPPORTED;
    } else if (cap.overflow) {
        res = AC_IRLIB_ERR_TOO_LONG;
    } else if (cap.count == 0) {
        res = AC_IRLIB_ERR_EMPTY;
    }
    if (res == AC_IRLIB_OK) {
        uint32_t carrier = cap.freq_hz != 0 ? cap.freq_hz : 38000u;
        ir_code_init(code, code->levels, code->levels_capacity, static_cast<uint16_t>(carrier > 65535u ? 65535u : carrier));
        for (size_t i = 0; i < cap.count; i++) {
            levels[i] = (i % 2 == 0) ? 1 : 0;
        }
        /* Split at long silences, as a capture of the original remote would be. */
        size_t start = 0;
        uint32_t gap = 0;
        for (size_t i = 0; i <= cap.count && res == AC_IRLIB_OK; i++) {
            bool end = i == cap.count;
            bool split = !end && (i % 2 == 1) && pulses[i] >= kSegmentGapUs;
            if (!end && !split) {
                continue;
            }
            if (i > start) {
                res = FromIrResult(ir_code_append_segment(code, &pulses[start], &levels[start], i - start, gap));
            }
            if (split) {
                gap = pulses[i];
                start = i + 1;
            }
        }
        if (res == AC_IRLIB_OK && code->segment_count == 0) {
            res = AC_IRLIB_ERR_EMPTY;
        }
    }
    free(pulses);
    free(levels);
    return res;
}

extern "C" bool ac_irlib_protocol_supported(int16_t protocol)
{
    return protocol > 0 && IRac::isProtocolSupported(static_cast<decode_type_t>(protocol));
}

extern "C" void ac_irlib_protocol_name(int16_t protocol, char * buf, size_t len)
{
    CopyName(typeToString(static_cast<decode_type_t>(protocol)), buf, len);
}

extern "C" size_t ac_irlib_models(int16_t protocol, ac_irlib_model_t * out, size_t max)
{
    size_t n = 0;
    for (int16_t m = 1; m <= 32 && n < max; m++) {
        std::string name = irutils::modelToStr(static_cast<decode_type_t>(protocol), m);
        if (name == kUnknownStr) {
            continue;
        }
        out[n].model = m;
        CopyName(name, out[n].name, sizeof(out[n].name));
        n++;
    }
    return n;
}

extern "C" void ac_irlib_model_name(int16_t protocol, int16_t model, char * buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    buf[0] = 0;
    if (model < 0) {
        return;
    }
    std::string name = irutils::modelToStr(static_cast<decode_type_t>(protocol), model);
    if (name != kUnknownStr) {
        CopyName(name, buf, len);
    }
}

extern "C" const char * ac_irlib_fan_label(uint8_t fan_raw)
{
    switch (static_cast<fanspeed_t>(fan_raw)) {
    case fanspeed_t::kAuto:
        return "tự động";
    case fanspeed_t::kMin:
        return "rất thấp";
    case fanspeed_t::kLow:
        return "thấp";
    case fanspeed_t::kMedium:
        return "vừa";
    case fanspeed_t::kMediumHigh:
        return "vừa-cao";
    case fanspeed_t::kHigh:
        return "cao";
    case fanspeed_t::kMax:
        return "rất cao";
    default:
        return "?";
    }
}

extern "C" uint8_t ac_irlib_fan_from_lib(uint8_t fan_raw, uint8_t fan_levels)
{
    if (fan_raw > static_cast<uint8_t>(fanspeed_t::kLastFanspeedEnum)) {
        return AC_FAN_AUTO;
    }
    return FromLibFan(static_cast<fanspeed_t>(fan_raw), fan_levels);
}

extern "C" bool ac_irlib_profile_state(const ac_irlib_detect_t * d, uint8_t fan_levels, uint8_t temp_min_half,
                                       uint8_t temp_max_half, ac_state_t * out)
{
    if (d == nullptr || out == nullptr || !d->has_state) {
        return false;
    }
    ac_state_t st = d->state;
    if (st.power == 0) {
        memset(&st, 0, sizeof(st));
    } else {
        if (st.fan != AC_FAN_QUIET && st.fan != AC_FAN_TURBO) {
            st.fan = ac_irlib_fan_from_lib(d->fan_raw, fan_levels);
        }
        if (st.mode != AC_MODE_FAN && temp_min_half <= temp_max_half) {
            st.temp_half = st.temp_half < temp_min_half ? temp_min_half
                                                        : (st.temp_half > temp_max_half ? temp_max_half : st.temp_half);
        }
    }
    *out = st;
    return true;
}

extern "C" uint8_t ac_irlib_fan_to_lib(uint8_t fan, uint8_t fan_levels)
{
    bool quiet;
    bool turbo;
    return static_cast<uint8_t>(ToLibFan(fan, fan_levels, &quiet, &turbo));
}
