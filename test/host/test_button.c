#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "press_detector.h"
#include "test.h"

typedef struct {
    bool pressed;
    int ms;
} seg_t;

/* Default gestures: 2 presses = Matter pairing mode, 3 presses = configuration portal. */
static const press_detector_cfg_t k_cfg = { .debounce_ms = 40, .window_ms = 1500, .max_press_ms = 700, .max_count = 3 };

static char s_out[64];
static int64_t s_report_time[8];

/* Feed raw levels sampled every 5 ms, like the button task. Returns the reported counts, e.g. "3,1". */
static const char *run(const seg_t *segs, size_t n)
{
    press_detector_t d;
    press_detector_init(&d, &k_cfg);
    int64_t t = 1000;
    size_t reports = 0;
    s_out[0] = 0;
    for (size_t i = 0; i <= n; i++) {
        bool pressed = i < n ? segs[i].pressed : false;
        int ms = i < n ? segs[i].ms : 3000; /* idle tail */
        for (int k = 0; k < ms; k += 5) {
            uint8_t c = press_detector_feed(&d, pressed, t);
            if (c) {
                size_t l = strlen(s_out);
                snprintf(s_out + l, sizeof(s_out) - l, "%s%u", l ? "," : "", c);
                if (reports < 8) {
                    s_report_time[reports] = t;
                }
                reports++;
            }
            t += 5;
        }
    }
    CHECK(!press_detector_busy(&d, t));
    return s_out;
}

#define P(ms) { true, ms }
#define R(ms) { false, ms }
#define RUN(...)                                                                                     \
    ({                                                                                               \
        const seg_t s_[] = { __VA_ARGS__ };                                                          \
        run(s_, sizeof(s_) / sizeof(s_[0]));                                                         \
    })

void test_press_detector(void)
{
    /* three short presses within 1.5 s: portal, reported at the third release (no waiting) */
    CHECK_STR(RUN(R(100), P(100), R(150), P(100), R(150), P(100), R(10)), "3");
    CHECK(s_report_time[0] < 1000 + 100 + 100 + 150 + 100 + 150 + 100 + 60);
    /* two short presses: pairing mode, reported when the 1.5 s window is over */
    CHECK_STR(RUN(R(100), P(100), R(150), P(100), R(10)), "2");
    CHECK(s_report_time[0] >= 1000 + 100 + 1500);
    /* one press: reported as 1 (no action is mapped to it) */
    CHECK_STR(RUN(R(100), P(100), R(10)), "1");
    /* four presses: a sequence of 3, then a new sequence of 1 */
    CHECK_STR(RUN(R(100), P(80), R(120), P(80), R(120), P(80), R(120), P(80), R(10)), "3,1");
    /* six quick presses: two sequences of 3 (the second only extends the open portal) */
    CHECK_STR(RUN(R(100), P(60), R(100), P(60), R(100), P(60), R(100), P(60), R(100), P(60), R(100), P(60), R(10)),
              "3,3");
    /* presses spread over more than the window are separate sequences */
    CHECK_STR(RUN(R(100), P(100), R(700), P(100), R(700), P(100), R(10)), "2,1");
    CHECK_STR(RUN(R(100), P(100), R(1600), P(100), R(10)), "1,1");
    /* a long hold cancels the sequence: nothing at all (no factory reset either) */
    CHECK_STR(RUN(R(100), P(100), R(150), P(1000), R(10)), "");
    CHECK_STR(RUN(R(100), P(3000), R(10)), "");
    /* short presses after a cancelled hold form a new sequence */
    CHECK_STR(RUN(R(100), P(1000), R(150), P(100), R(150), P(100), R(10)), "2");
    /* contact bounce shorter than the debounce time is ignored */
    CHECK_STR(RUN(R(100), P(5), R(5), P(5), R(5), P(100), R(5), P(5), R(150), P(5), R(5), P(100), R(5), P(5), R(150),
                  P(100), R(10)),
              "3");
    CHECK_STR(RUN(R(100), P(5), R(5), P(5), R(5), P(100), R(5), P(5), R(150), P(5), R(5), P(100), R(10)), "2");
    /* glitches shorter than the debounce time are not presses */
    CHECK_STR(RUN(R(100), P(20), R(150), P(20), R(150), P(20), R(10)), "");
    /* presses just above the debounce time count */
    CHECK_STR(RUN(R(100), P(50), R(150), P(50), R(150), P(50), R(10)), "3");

    press_detector_t d;
    press_detector_init(&d, &k_cfg);
    CHECK(!press_detector_busy(&d, 0));
    press_detector_feed(&d, true, 0);
    CHECK(press_detector_busy(&d, 0)); /* debounce in progress */
}
