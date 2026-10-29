#pragma once

/*
 * Helpers of the configuration portal that do not depend on ESP-IDF (host tested):
 *   - a small streaming JSON writer with string escaping,
 *   - application/x-www-form-urlencoded parsing with strict percent-decoding.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WJ_MAX_DEPTH 8

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    uint8_t depth;
    bool first[WJ_MAX_DEPTH]; /* next value at this depth is the first one (no comma) */
} wj_t;

void wj_init(wj_t *w, char *buf, size_t cap);
/* `key` is NULL for array elements and for the root value. */
void wj_obj_begin(wj_t *w, const char *key);
void wj_obj_end(wj_t *w);
void wj_arr_begin(wj_t *w, const char *key);
void wj_arr_end(wj_t *w);
void wj_str(wj_t *w, const char *key, const char *val); /* val NULL -> null */
void wj_int(wj_t *w, const char *key, int64_t v);
void wj_uint(wj_t *w, const char *key, uint64_t v);
void wj_bool(wj_t *w, const char *key, bool v);
void wj_null(wj_t *w, const char *key);
/** 64-bit identifiers as "0x%016llX" strings (JavaScript numbers lose precision above 2^53). */
void wj_hex64(wj_t *w, const char *key, uint64_t v);
/** NULL if the buffer overflowed or the nesting is unbalanced, else the NUL-terminated text. */
const char *wj_finish(wj_t *w);

/**
 * Find `key` in a form-encoded body ("a=1&b=x%20y") and decode its value into `out`.
 * Returns the decoded length, -1 when the key is absent, -2 when the value is malformed, contains
 * a NUL byte or does not fit into `out` (out_len includes the terminator).
 */
int form_get(const char *body, const char *key, char *out, size_t out_len);
/** Decimal integer within [min, max]. False when absent or invalid. */
bool form_get_int(const char *body, const char *key, long min, long max, long *out);
/** Unsigned 64-bit, decimal or 0x-prefixed hexadecimal. */
bool form_get_u64(const char *body, const char *key, uint64_t *out);
/** "1"/"true"/"on" -> true, "0"/"false"/"off" -> false; `def` when absent. */
bool form_get_bool(const char *body, const char *key, bool def);

/** Same as form_get for a URL query string ("slot=1&x=2"). */
static inline int query_get(const char *query, const char *key, char *out, size_t out_len)
{
    return form_get(query, key, out, out_len);
}

/** Drop an incomplete UTF-8 sequence at the end of `s` (left by a byte-wise truncation). */
void utf8_trim_tail(char *s);

/** Constant time comparison of two NUL-terminated strings. */
bool secure_str_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
