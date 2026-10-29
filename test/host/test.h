#pragma once

#include <stdio.h>

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        g_checks++;                                                                                  \
        if (!(cond)) {                                                                               \
            g_failures++;                                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        long long _a = (long long)(a);                                                               \
        long long _b = (long long)(b);                                                               \
        g_checks++;                                                                                  \
        if (_a != _b) {                                                                              \
            g_failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b);   \
        }                                                                                            \
    } while (0)

#define CHECK_STR(a, b)                                                                              \
    do {                                                                                             \
        const char *_a = (a);                                                                        \
        const char *_b = (b);                                                                        \
        g_checks++;                                                                                  \
        if (_a == NULL || strcmp(_a, _b) != 0) {                                                     \
            g_failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a,       \
                    _a ? _a : "(null)", _b);                                                         \
        }                                                                                            \
    } while (0)

void test_press_detector(void);
void test_ir_code(void);
void test_ir_capture(void);
void test_ac_types(void);
void test_climate_algo(void);
void test_rt_logic(void);
void test_app_config(void);
void test_web_util(void);
