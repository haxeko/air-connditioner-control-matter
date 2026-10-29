#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test.h"
#include "web_util.h"

/* Strict UTF-8 validation (no overlongs / surrogates checks needed for these tests). */
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) {
            return false;
        }
        p++;
        for (int i = 0; i < n; i++, p++) {
            if ((*p & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

static void test_json(void)
{
    char buf[256];
    wj_t w;
    wj_init(&w, buf, sizeof(buf));
    wj_obj_begin(&w, NULL);
    wj_bool(&w, "ok", true);
    wj_str(&w, "ssid", "a\"b\\c\n\x01<b>");
    wj_int(&w, "n", -42);
    wj_uint(&w, "u", 18446744073709551615ull);
    wj_null(&w, "z");
    wj_hex64(&w, "node", 0x1A2B3C4D5E6F7081ull);
    wj_arr_begin(&w, "a");
    wj_int(&w, NULL, 1);
    wj_str(&w, NULL, "Lạnh");
    wj_obj_begin(&w, NULL);
    wj_obj_end(&w);
    wj_arr_end(&w);
    wj_str(&w, "nul", NULL);
    wj_obj_end(&w);
    CHECK_STR(wj_finish(&w), "{\"ok\":true,\"ssid\":\"a\\\"b\\\\c\\n\\u0001<b>\",\"n\":-42,"
                             "\"u\":18446744073709551615,\"z\":null,\"node\":\"0x1A2B3C4D5E6F7081\","
                             "\"a\":[1,\"Lạnh\",{}],\"nul\":null}");

    /* overflow is reported, never a truncated document */
    char small[16];
    wj_init(&w, small, sizeof(small));
    wj_obj_begin(&w, NULL);
    wj_str(&w, "key", "a long value that does not fit");
    wj_obj_end(&w);
    CHECK(wj_finish(&w) == NULL);

    /* unbalanced nesting */
    wj_init(&w, buf, sizeof(buf));
    wj_obj_begin(&w, NULL);
    CHECK(wj_finish(&w) == NULL);
    wj_init(&w, buf, sizeof(buf));
    wj_obj_end(&w);
    CHECK(wj_finish(&w) == NULL);

    /* nesting limit */
    wj_init(&w, buf, sizeof(buf));
    for (int i = 0; i < WJ_MAX_DEPTH + 1; i++) {
        wj_arr_begin(&w, NULL);
    }
    CHECK(w.overflow);
}

static void test_form(void)
{
    char v[32];
    const char *body = "a=1&b=x%20y+z&c=&d&ssid2=wrong&ssid=Nh%C3%A0%20t%C3%B4i&pct=%25%26%3D";
    CHECK_EQ(form_get(body, "a", v, sizeof(v)), 1);
    CHECK_STR(v, "1");
    CHECK_EQ(form_get(body, "b", v, sizeof(v)), 5);
    CHECK_STR(v, "x y z");
    CHECK_EQ(form_get(body, "c", v, sizeof(v)), 0);
    CHECK_STR(v, "");
    CHECK_EQ(form_get(body, "d", v, sizeof(v)), 0);
    CHECK_EQ(form_get(body, "e", v, sizeof(v)), -1);
    CHECK_EQ(form_get(body, "ssid", v, sizeof(v)), 9);
    CHECK_STR(v, "Nhà tôi");
    CHECK_EQ(form_get(body, "pct", v, sizeof(v)), 3);
    CHECK_STR(v, "%&=");
    CHECK_EQ(form_get(body, "ss", v, sizeof(v)), -1); /* no prefix match */

    CHECK_EQ(form_get("k=%00x", "k", v, sizeof(v)), -2); /* embedded NUL */
    CHECK_EQ(form_get("k=%2", "k", v, sizeof(v)), -2);
    CHECK_EQ(form_get("k=%zz", "k", v, sizeof(v)), -2);
    CHECK_EQ(form_get("k=12345", "k", v, 5), -2); /* does not fit: error, not truncation */
    CHECK_EQ(form_get("k=1234", "k", v, 5), 4);
    CHECK_EQ(form_get("k=1&k=2", "k", v, sizeof(v)), 1);
    CHECK_STR(v, "1");
    CHECK_EQ(form_get("", "k", v, sizeof(v)), -1);

    long n = 0;
    CHECK(form_get_int("n=12", "n", 0, 100, &n) && n == 12);
    CHECK(form_get_int("n=-5", "n", -10, 0, &n) && n == -5);
    CHECK(!form_get_int("n=12x", "n", 0, 100, &n));
    CHECK(!form_get_int("n=101", "n", 0, 100, &n));
    CHECK(!form_get_int("n=-1", "n", 0, 100, &n));
    CHECK(!form_get_int("n=", "n", 0, 100, &n));
    CHECK(!form_get_int("n=99999999999999999999", "n", 0, 100, &n));
    CHECK(!form_get_int("m=1", "n", 0, 100, &n));

    uint64_t u = 0;
    CHECK(form_get_u64("node=0x1A2B", "node", &u) && u == 0x1A2B);
    CHECK(form_get_u64("node=123", "node", &u) && u == 123);
    CHECK(form_get_u64("node=0xFFFFFFFFFFFFFFFF", "node", &u) && u == UINT64_MAX);
    CHECK(!form_get_u64("node=0x10000000000000000", "node", &u));
    CHECK(!form_get_u64("node=0x", "node", &u));
    CHECK(!form_get_u64("node=-1", "node", &u));
    CHECK(!form_get_u64("node=0x-1", "node", &u));
    CHECK(!form_get_u64("node=12ab", "node", &u));

    CHECK(form_get_bool("x=1", "x", false));
    CHECK(form_get_bool("x=on", "x", false));
    CHECK(!form_get_bool("x=0", "x", true));
    CHECK(form_get_bool("y=0", "x", true)); /* absent: default */
    CHECK(!form_get_bool("x=maybe", "x", false));
}

static void test_utf8(void)
{
    char s[128];
    strcpy(s, "abc");
    utf8_trim_tail(s);
    CHECK_STR(s, "abc");
    strcpy(s, "a\xC3");
    utf8_trim_tail(s);
    CHECK_STR(s, "a");
    strcpy(s, "a\xE1\xBA");
    utf8_trim_tail(s);
    CHECK_STR(s, "a");
    strcpy(s, "a\xC3\xA9");
    utf8_trim_tail(s);
    CHECK_STR(s, "a\xC3\xA9");
    strcpy(s, "a\xA9");
    utf8_trim_tail(s);
    CHECK_STR(s, "a");

    /* every byte-wise truncation of a Vietnamese label becomes valid UTF-8 again */
    const char *label = "Thử: Tự động 24,5°C, quạt tự động, đảo gió tắt";
    size_t len = strlen(label);
    for (size_t cut = 0; cut <= len; cut++) {
        memcpy(s, label, cut);
        s[cut] = 0;
        utf8_trim_tail(s);
        CHECK(utf8_valid(s));
        CHECK(strncmp(s, label, strlen(s)) == 0);
        CHECK(cut - strlen(s) <= 3);
    }

    CHECK(secure_str_equal("abc", "abc"));
    CHECK(!secure_str_equal("abc", "abd"));
    CHECK(!secure_str_equal("abc", "abcd"));
    CHECK(!secure_str_equal("", "a"));
}

void test_web_util(void)
{
    test_json();
    test_form();
    test_utf8();
}
