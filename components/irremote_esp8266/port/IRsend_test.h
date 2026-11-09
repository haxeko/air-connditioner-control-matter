#ifndef TEST_IRSEND_TEST_H_
#define TEST_IRSEND_TEST_H_

/*
 * Replacement for the library's test/IRsend_test.h (not part of the library sources we ship).
 *
 * Built with UNIT_TEST=1 (the library's mode without Arduino), each protocol class holds an
 * `IRsendTest _irsend` instead of an `IRsend`. The original test class keeps ~110 KB of buffers
 * per instance, far too much for an ESP32 task stack. This one only forwards the generated pulses
 * to ir_recorder (port/ir_recorder.h) and keeps the members the library refers to.
 */

#include <stdint.h>

#include "IRrecv.h"
#include "IRsend.h"
#include "IRtimer.h"
#include "ir_recorder.h"

class IRsendTest : public IRsend
{
public:
    decode_results capture; /* only used by IRac when a unit-test receiver is attached (never here) */

    explicit IRsendTest(uint16_t pin, bool inverted = false, bool use_modulation = true) :
        IRsend(pin, inverted, use_modulation), capture()
    {}

    void makeDecodeResult(uint16_t offset = 0)
    {
        (void)offset;
        capture.decode_type = UNKNOWN;
        capture.rawbuf = nullptr;
        capture.rawlen = 0;
    }

    uint16_t mark(uint16_t usec) override
    {
        IRtimer::add(usec); /* send functions use IRtimer to pad messages to their minimum length */
        ir_recorder::mark(usec, _freq_unittest, _dutycycle);
        return 0;
    }

    void space(uint32_t usec) override
    {
        IRtimer::add(usec);
        ir_recorder::space(usec);
    }
};

#endif // TEST_IRSEND_TEST_H_
