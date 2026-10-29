#include <stdbool.h>
#include <string.h>

#include "config_store.h"
#include "test.h"

/* Apply one change to valid defaults and expect validation to fail/succeed. */
#define EXPECT_CFG(ok, stmt)                                                                         \
    do {                                                                                             \
        app_config_t x;                                                                              \
        app_config_defaults(&x);                                                                     \
        stmt;                                                                                        \
        const char *w = NULL;                                                                        \
        CHECK(app_config_validate(&x, &w) == (ok));                                                  \
        CHECK((ok) || w != NULL);                                                                    \
    } while (0)

void test_app_config(void)
{
    app_config_t c;
    const char *why = NULL;
    app_config_defaults(&c);
    CHECK(app_config_validate(&c, &why));
    CHECK_EQ(c.control_mode, CONTROL_MODE_DIRECT);
    CHECK_EQ(c.resend_on_boot, 0); /* no IR after power-up unless the user asks for it */
    CHECK(app_config_validate(&c, NULL));

    EXPECT_CFG(false, x.version = 2);
    EXPECT_CFG(false, x.control_mode = 2);
    EXPECT_CFG(true, x.control_mode = CONTROL_MODE_EXTERNAL);
    EXPECT_CFG(false, x.allow_local_fallback = 2);
    EXPECT_CFG(false, x.resend_on_boot = 2);
    EXPECT_CFG(false, x.deadband_centi = 9);
    EXPECT_CFG(true, x.deadband_centi = 10);
    EXPECT_CFG(true, x.deadband_centi = 300);
    EXPECT_CFG(false, x.deadband_centi = 301);
    EXPECT_CFG(true, x.max_offset_centi = 0);
    EXPECT_CFG(true, x.max_offset_centi = 500);
    EXPECT_CFG(false, x.max_offset_centi = 501);
    EXPECT_CFG(false, x.comp_interval_s = 119);
    EXPECT_CFG(false, x.comp_interval_s = 7201);
    EXPECT_CFG(true, x.filter_tau_s = 0);
    EXPECT_CFG(false, x.filter_tau_s = 1801);
    EXPECT_CFG(false, x.user_debounce_ms = 199);
    EXPECT_CFG(false, x.user_debounce_ms = 10001);
    EXPECT_CFG(false, x.stale_min_s = 299);
    EXPECT_CFG(false, x.stale_min_s = 86401);
    EXPECT_CFG(true, x.stale_min_s = 300);
}
