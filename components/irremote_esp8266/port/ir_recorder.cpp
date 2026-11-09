#include "ir_recorder.h"

namespace ir_recorder {

namespace {
Capture * sCap = nullptr;

void Append(uint32_t usec)
{
    if (sCap->count >= sCap->capacity) {
        sCap->overflow = true;
        return;
    }
    sCap->durations[sCap->count++] = usec;
}
} // namespace

void begin(Capture * cap)
{
    cap->count = 0;
    cap->overflow = false;
    cap->freq_hz = 0;
    cap->duty = 0;
    sCap = cap;
}

void end()
{
    sCap = nullptr;
}

void mark(uint32_t usec, uint32_t freq_hz, uint8_t duty)
{
    if (sCap == nullptr || usec == 0) {
        return;
    }
    if (sCap->count == 0) {
        sCap->freq_hz = freq_hz;
        sCap->duty = duty;
    }
    if (sCap->count > 0 && (sCap->count % 2) == 1) {
        sCap->durations[sCap->count - 1] += usec; /* two marks in a row: one longer mark */
    } else {
        Append(usec);
    }
}

void space(uint32_t usec)
{
    if (sCap == nullptr || usec == 0 || sCap->count == 0) {
        return; /* leading silence is not part of the command */
    }
    if ((sCap->count % 2) == 0) {
        sCap->durations[sCap->count - 1] += usec; /* two spaces in a row */
    } else {
        Append(usec);
    }
}

} // namespace ir_recorder
