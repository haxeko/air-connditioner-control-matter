#include "ac_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_modes[AC_MODE_COUNT] = { "cool", "heat", "dry", "fan", "auto" };
static const char *const k_fans[AC_FAN_COUNT] = { "auto", "1", "2", "3", "4", "5", "quiet", "turbo" };
static const char *const k_mode_labels[AC_MODE_COUNT] = { "Lạnh", "Sưởi", "Khô", "Chỉ quạt", "Tự động" };
static const char *const k_fan_labels[AC_FAN_COUNT] = { "tự động", "mức 1", "mức 2", "mức 3",
                                                        "mức 4",   "mức 5", "êm",    "mạnh" };

const char *ac_mode_name(uint8_t mode)
{
    return mode < AC_MODE_COUNT ? k_modes[mode] : "?";
}

const char *ac_fan_name(uint8_t fan)
{
    return fan < AC_FAN_COUNT ? k_fans[fan] : "?";
}

const char *ac_mode_label(uint8_t mode)
{
    return mode < AC_MODE_COUNT ? k_mode_labels[mode] : "?";
}

const char *ac_fan_label(uint8_t fan)
{
    return fan < AC_FAN_COUNT ? k_fan_labels[fan] : "?";
}

bool ac_state_valid(const ac_state_t *s)
{
    if (s->power == 0) {
        return true;
    }
    if (s->power != 1 || s->mode >= AC_MODE_COUNT || s->fan >= AC_FAN_COUNT || s->swing >= AC_SWING_COUNT) {
        return false;
    }
    if (s->mode == AC_MODE_FAN) {
        return s->temp_half == AC_TEMP_NONE;
    }
    return s->temp_half >= AC_TEMP_HALF_MIN && s->temp_half <= AC_TEMP_HALF_MAX;
}

bool ac_state_equal(const ac_state_t *a, const ac_state_t *b)
{
    if (a->power != b->power) {
        return false;
    }
    if (a->power == 0) {
        return true;
    }
    return a->mode == b->mode && a->temp_half == b->temp_half && a->fan == b->fan && a->swing == b->swing;
}

bool ac_state_key(const ac_state_t *s, char *buf, size_t len)
{
    if (!ac_state_valid(s) || len < 16) {
        return false;
    }
    if (s->power == 0) {
        snprintf(buf, len, "off");
    } else {
        snprintf(buf, len, "m%ut%uf%us%u", s->mode, s->temp_half, s->fan, s->swing);
    }
    return true;
}

bool ac_state_from_key(const char *key, ac_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (strcmp(key, "off") == 0) {
        return true;
    }
    unsigned m, t, f, w;
    char tail;
    if (sscanf(key, "m%ut%uf%us%u%c", &m, &t, &f, &w, &tail) != 4) {
        return false;
    }
    if (m > 255 || t > 255 || f > 255 || w > 255) {
        return false;
    }
    out->power = 1;
    out->mode = (uint8_t)m;
    out->temp_half = (uint8_t)t;
    out->fan = (uint8_t)f;
    out->swing = (uint8_t)w;
    return ac_state_valid(out);
}

void ac_state_describe(const ac_state_t *s, char *buf, size_t len)
{
    if (s->power == 0) {
        snprintf(buf, len, "Tắt");
        return;
    }
    const char *swing = s->swing ? "đảo gió bật" : "đảo gió tắt";
    if (s->mode == AC_MODE_FAN || s->temp_half == AC_TEMP_NONE) {
        snprintf(buf, len, "%s, quạt %s, %s", ac_mode_label(s->mode), ac_fan_label(s->fan), swing);
        return;
    }
    snprintf(buf, len, "%s %u%s°C, quạt %s, %s", ac_mode_label(s->mode), s->temp_half / 2, (s->temp_half & 1) ? ",5" : "",
             ac_fan_label(s->fan), swing);
}

void ac_caps_from_list(const ac_state_t *list, size_t n, uint8_t temp_step_half, ac_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->temp_step_half = (temp_step_half == 1) ? 1 : 2;
    for (size_t i = 0; i < n; i++) {
        const ac_state_t *s = &list[i];
        if (!ac_state_valid(s)) {
            continue;
        }
        caps->code_count++;
        if (s->power == 0) {
            caps->has_off = true;
            continue;
        }
        caps->modes_mask |= (uint8_t)(1u << s->mode);
        caps->fans_mask |= (uint16_t)(1u << s->fan);
        caps->swing_mask |= (uint8_t)(1u << s->swing);
        if (s->temp_half != AC_TEMP_NONE) {
            if (caps->temp_min_half[s->mode] == 0 || s->temp_half < caps->temp_min_half[s->mode]) {
                caps->temp_min_half[s->mode] = s->temp_half;
            }
            if (s->temp_half > caps->temp_max_half[s->mode]) {
                caps->temp_max_half[s->mode] = s->temp_half;
            }
        }
    }
}

static int tier_of(const ac_state_t *c, const ac_state_t *d, uint8_t def_fan, uint8_t def_swing)
{
    bool fan_req = c->fan == d->fan;
    bool swing_req = c->swing == d->swing;
    bool fan_def = c->fan == def_fan;
    bool swing_def = c->swing == def_swing;
    if (fan_req && swing_req) {
        return 0;
    }
    if (fan_req && swing_def) {
        return 1;
    }
    if (fan_def && swing_req) {
        return 2;
    }
    if (fan_def && swing_def) {
        return 3;
    }
    return 4;
}

ac_resolve_kind_t ac_resolve(const ac_state_t *list, size_t n, uint8_t default_fan, uint8_t default_swing,
                             const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen)
{
    if (desired->power == 0) {
        for (size_t i = 0; i < n; i++) {
            if (list[i].power == 0) {
                memset(chosen, 0, sizeof(*chosen));
                return AC_RESOLVE_EXACT;
            }
        }
        return AC_RESOLVE_NONE;
    }

    int best = -1;
    int best_tier = 99;
    int best_dt = 1 << 30;
    int best_du = 1 << 30;
    for (size_t i = 0; i < n; i++) {
        const ac_state_t *c = &list[i];
        if (c->power != 1 || c->mode != desired->mode || !ac_state_valid(c)) {
            continue;
        }
        int tier = tier_of(c, desired, default_fan, default_swing);
        int dt = 0;
        int du = 0;
        if (desired->mode != AC_MODE_FAN) {
            dt = abs((int)c->temp_half - (int)desired->temp_half);
            du = abs((int)c->temp_half - (int)user_target_half);
        }
        /* The temperature is the controlled quantity: it is matched first, fan/swing second. */
        bool better = false;
        if (dt < best_dt) {
            better = true;
        } else if (dt == best_dt) {
            if (tier < best_tier) {
                better = true;
            } else if (tier == best_tier) {
                if (du < best_du) {
                    better = true;
                } else if (du == best_du && best >= 0) {
                    /* energy saving side: warmer when cooling/drying, cooler when heating */
                    if (desired->mode == AC_MODE_HEAT) {
                        better = c->temp_half < list[best].temp_half;
                    } else {
                        better = c->temp_half > list[best].temp_half;
                    }
                }
            }
        }
        if (better) {
            best = (int)i;
            best_tier = tier;
            best_dt = dt;
            best_du = du;
        }
    }
    if (best < 0) {
        return AC_RESOLVE_NONE;
    }
    *chosen = list[best];
    if (best_tier != 0) {
        return AC_RESOLVE_FALLBACK_FAN_SWING;
    }
    return best_dt == 0 ? AC_RESOLVE_EXACT : AC_RESOLVE_NEAREST_TEMP;
}
