#include <stdio.h>
#include <string.h>

#include "test.h"

int g_checks;
int g_failures;

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } suites[] = {
        { "press_detector", test_press_detector }, { "ir_code", test_ir_code },     { "ir_capture", test_ir_capture },
        { "ac_types", test_ac_types },             { "climate_algo", test_climate_algo }, { "rt_logic", test_rt_logic },
        { "app_config", test_app_config },         { "web_util", test_web_util },
    };
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        int before = g_failures;
        suites[i].fn();
        printf("%-16s %s\n", suites[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
