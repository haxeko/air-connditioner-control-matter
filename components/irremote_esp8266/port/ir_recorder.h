#pragma once

/*
 * Pulse recorder for IRremoteESP8266 built without Arduino.
 *
 * The library is compiled in its host mode (UNIT_TEST=1, no Arduino, no GPIO, no timers). In that
 * mode every protocol class sends through an IRsendTest object (port/IRsend_test.h), whose mark()
 * and space() land here. ac_irlib.cpp starts a recording, lets IRac generate a command and reads
 * back the pulse train, which the ESP32 RMT then transmits.
 *
 * Not thread safe: the caller serialises recordings.
 */

#include <stddef.h>
#include <stdint.h>

namespace ir_recorder {

struct Capture
{
    uint32_t * durations; /* mark, space, mark, ... in microseconds (always starts with a mark) */
    size_t capacity;
    size_t count;
    bool overflow;        /* more pulses than `capacity`: the command must not be used */
    uint32_t freq_hz;     /* carrier of the first mark (0 if none) */
    uint8_t duty;         /* duty cycle of the first mark, percent */
};

/** Record into `cap` until end(). */
void begin(Capture * cap);
void end();

void mark(uint32_t usec, uint32_t freq_hz, uint8_t duty);
void space(uint32_t usec);

} // namespace ir_recorder
