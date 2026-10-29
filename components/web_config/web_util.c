#include "web_util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------
 * JSON writer
 * ------------------------------------------------------------------------------------------- */

void wj_init(wj_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (cap > 0) {
        buf[0] = 0;
    } else {
        w->overflow = true;
    }
}

static void put(wj_t *w, const char *s, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (w->len + n + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void put_char(wj_t *w, char c)
{
    put(w, &c, 1);
}

static void put_escaped(wj_t *w, const char *s)
{
    put_char(w, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':
            put(w, "\\\"", 2);
            break;
        case '\\':
            put(w, "\\\\", 2);
            break;
        case '\n':
            put(w, "\\n", 2);
            break;
        case '\r':
            put(w, "\\r", 2);
            break;
        case '\t':
            put(w, "\\t", 2);
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                put(w, tmp, 6);
            } else {
                put_char(w, (char)c);
            }
            break;
        }
    }
    put_char(w, '"');
}

static void prefix(wj_t *w, const char *key)
{
    if (w->depth > 0) {
        if (!w->first[w->depth - 1]) {
            put_char(w, ',');
        }
        w->first[w->depth - 1] = false;
    }
    if (key != NULL) {
        put_escaped(w, key);
        put_char(w, ':');
    }
}

static void open_container(wj_t *w, const char *key, char c)
{
    prefix(w, key);
    put_char(w, c);
    if (w->depth >= WJ_MAX_DEPTH) {
        w->overflow = true;
        return;
    }
    w->first[w->depth++] = true;
}

static void close_container(wj_t *w, char c)
{
    if (w->depth == 0) {
        w->overflow = true;
        return;
    }
    w->depth--;
    put_char(w, c);
}

void wj_obj_begin(wj_t *w, const char *key)
{
    open_container(w, key, '{');
}

void wj_obj_end(wj_t *w)
{
    close_container(w, '}');
}

void wj_arr_begin(wj_t *w, const char *key)
{
    open_container(w, key, '[');
}

void wj_arr_end(wj_t *w)
{
    close_container(w, ']');
}

void wj_str(wj_t *w, const char *key, const char *val)
{
    prefix(w, key);
    if (val == NULL) {
        put(w, "null", 4);
    } else {
        put_escaped(w, val);
    }
}

void wj_int(wj_t *w, const char *key, int64_t v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%" PRId64, v);
    prefix(w, key);
    put(w, tmp, (size_t)n);
}

void wj_uint(wj_t *w, const char *key, uint64_t v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    prefix(w, key);
    put(w, tmp, (size_t)n);
}

void wj_bool(wj_t *w, const char *key, bool v)
{
    prefix(w, key);
    if (v) {
        put(w, "true", 4);
    } else {
        put(w, "false", 5);
    }
}

void wj_null(wj_t *w, const char *key)
{
    prefix(w, key);
    put(w, "null", 4);
}

void wj_hex64(wj_t *w, const char *key, uint64_t v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "0x%016" PRIX64, v);
    wj_str(w, key, tmp);
}

const char *wj_finish(wj_t *w)
{
    if (w->overflow || w->depth != 0) {
        return NULL;
    }
    return w->buf;
}

/* ---------------------------------------------------------------------------------------------
 * Form decoding
 * ------------------------------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (body == NULL || key == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    const size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));
        size_t name_len = eq ? (size_t)(eq - p) : (size_t)(end - p);
        if (name_len == klen && memcmp(p, key, klen) == 0) {
            const char *v = eq ? eq + 1 : end;
            size_t o = 0;
            while (v < end) {
                char c = *v++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (end - v < 2) {
                        return -2;
                    }
                    int hi = hexval(v[0]);
                    int lo = hexval(v[1]);
                    if (hi < 0 || lo < 0) {
                        return -2;
                    }
                    c = (char)((hi << 4) | lo);
                    v += 2;
                    if (c == 0) {
                        return -2;
                    }
                }
                if (o + 1 >= out_len) {
                    return -2;
                }
                out[o++] = c;
            }
            out[o] = 0;
            return (int)o;
        }
        p = amp ? amp + 1 : end;
    }
    return -1;
}

bool form_get_int(const char *body, const char *key, long min, long max, long *out)
{
    char tmp[24];
    if (form_get(body, key, tmp, sizeof(tmp)) <= 0) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(tmp, &end, 10);
    if (errno != 0 || end == tmp || *end != 0 || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

bool form_get_u64(const char *body, const char *key, uint64_t *out)
{
    char tmp[24];
    if (form_get(body, key, tmp, sizeof(tmp)) <= 0) {
        return false;
    }
    const char *digits = tmp;
    int base = 10;
    if (tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) {
        digits = tmp + 2;
        base = 16;
    }
    if (*digits == 0 || *digits == '-' || *digits == '+' || *digits == ' ') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(digits, &end, base);
    if (errno != 0 || *end != 0) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

bool form_get_bool(const char *body, const char *key, bool def)
{
    char tmp[8];
    if (form_get(body, key, tmp, sizeof(tmp)) < 0) {
        return def;
    }
    if (strcmp(tmp, "1") == 0 || strcmp(tmp, "true") == 0 || strcmp(tmp, "on") == 0) {
        return true;
    }
    if (strcmp(tmp, "0") == 0 || strcmp(tmp, "false") == 0 || strcmp(tmp, "off") == 0) {
        return false;
    }
    return def;
}

bool secure_str_equal(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) {
        return false; /* the token length is not secret */
    }
    unsigned char d = 0;
    for (size_t i = 0; i < la; i++) {
        d |= (unsigned char)(a[i] ^ b[i]);
    }
    return d == 0;
}

void utf8_trim_tail(char *s)
{
    size_t len = strlen(s);
    if (len == 0) {
        return;
    }
    /* find the start of the last character */
    size_t i = len;
    size_t cont = 0;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && cont < 4) {
        i--;
        cont++;
    }
    if (i == 0) {
        s[0] = 0; /* only continuation bytes */
        return;
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need;
    if (lead < 0x80) {
        need = 0;
    } else if ((lead & 0xE0) == 0xC0) {
        need = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 2;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 3;
    } else {
        s[i - 1] = 0; /* invalid lead byte */
        return;
    }
    if (cont < need) {
        s[i - 1] = 0; /* last character incomplete */
    } else if (cont > need) {
        s[i + need] = 0; /* stray continuation bytes after a complete character */
    }
}
