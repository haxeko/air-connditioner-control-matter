#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "press_detector.h"
#include "test.h"

typedef struct {
    bool pressed;
    int ms;
} seg_t;

static const press_detector_cfg_t k_cfg = { .debounce_ms = 40, .window_ms = 1500, .max_press_ms = 700, .count = 3 };

/* Feed raw levels sampled every 5 ms, like the button task. Returns the number of sequences. */
static int run(const seg_t *segs, size_t n)
{
    press_detector_t d;
    press_detector_init(&d, &k_cfg);
    int64_t t = 1000;
    int fires = 0;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < segs[i].ms; k += 5) {
            fires += press_detector_feed(&d, segs[i].pressed, t) ? 1 : 0;
            t += 5;
        }
    }
    for (int k = 0; k < 3000; k += 5) { /* idle tail */
        fires += press_detector_feed(&d, false, t) ? 1 : 0;
        t += 5;
    }
    CHECK(!press_detector_busy(&d, t));
    return fires;
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
    /* three short presses within 1.5 s: opens the portal once */
    CHECK_EQ(RUN(R(100), P(100), R(150), P(100), R(150), P(100), R(10)), 1);
    /* one or two presses never do anything */
    CHECK_EQ(RUN(R(100), P(100), R(10)), 0);
    CHECK_EQ(RUN(R(100), P(100), R(150), P(100), R(10)), 0);
    /* four presses: the fourth starts a new sequence */
    CHECK_EQ(RUN(R(100), P(80), R(120), P(80), R(120), P(80), R(120), P(80), R(10)), 1);
    /* six quick presses: two sequences (the second one only extends the portal) */
    CHECK_EQ(RUN(R(100), P(60), R(100), P(60), R(100), P(60), R(100), P(60), R(100), P(60), R(100), P(60), R(10)), 2);
    /* too slow: three presses spread over more than 1.5 s */
    CHECK_EQ(RUN(R(100), P(100), R(700), P(100), R(700), P(100), R(10)), 0);
    /* a long hold is not a short press and cancels the sequence */
    CHECK_EQ(RUN(R(100), P(100), R(150), P(1000), R(150), P(100), R(150), P(100), R(10)), 0);
    /* contact bounce shorter than the debounce time is ignored */
    CHECK_EQ(RUN(R(100), P(5), R(5), P(5), R(5), P(100), R(5), P(5), R(150), P(5), R(5), P(100), R(5), P(5), R(150),
                 P(100), R(10)),
             1);
    /* glitches shorter than the debounce time are not presses */
    CHECK_EQ(RUN(R(100), P(20), R(150), P(20), R(150), P(20), R(10)), 0);
    /* a press just above the debounce time counts */
    CHECK_EQ(RUN(R(100), P(50), R(150), P(50), R(150), P(50), R(10)), 1);

    press_detector_t d;
    press_detector_init(&d, &k_cfg);
    CHECK(!press_detector_busy(&d, 0));
    press_detector_feed(&d, true, 0);
    CHECK(press_detector_busy(&d, 0)); /* debounce in progress */
}
