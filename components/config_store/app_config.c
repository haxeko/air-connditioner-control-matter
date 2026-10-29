/* Pure functions (no IDF dependency except esp_err.h types) - unit tested on the host. */
#include <string.h>

#include "config_store.h"

void app_config_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = APP_CONFIG_VERSION;
    cfg->control_mode = CONTROL_MODE_DIRECT;
    cfg->allow_local_fallback = 1;
    cfg->deadband_centi = 50;     /* 0.5 C  */
    cfg->max_offset_centi = 300;  /* 3.0 C  */
    cfg->comp_interval_s = 600;   /* 10 min */
    cfg->filter_tau_s = 180;      /* 3 min  */
    cfg->user_debounce_ms = 1500;
    cfg->resend_on_boot = 0;
    cfg->stale_min_s = 900;       /* 15 min */
}

bool app_config_validate(const app_config_t *cfg, const char **why)
{
    const char *dummy;
    if (why == NULL) {
        why = &dummy;
    }
    if (cfg->version != APP_CONFIG_VERSION) {
        *why = "version";
        return false;
    }
    if (cfg->control_mode != CONTROL_MODE_DIRECT && cfg->control_mode != CONTROL_MODE_EXTERNAL) {
        *why = "control_mode";
        return false;
    }
    if (cfg->allow_local_fallback > 1 || cfg->resend_on_boot > 1) {
        *why = "flags";
        return false;
    }
    if (cfg->deadband_centi < 10 || cfg->deadband_centi > 300) {
        *why = "deadband (0.1..3.0 C)";
        return false;
    }
    if (cfg->max_offset_centi > 500) {
        *why = "max_offset (0..5.0 C)";
        return false;
    }
    if (cfg->comp_interval_s < 120 || cfg->comp_interval_s > 7200) {
        *why = "comp_interval (120..7200 s)";
        return false;
    }
    if (cfg->filter_tau_s > 1800) {
        *why = "filter_tau (0..1800 s)";
        return false;
    }
    if (cfg->user_debounce_ms < 200 || cfg->user_debounce_ms > 10000) {
        *why = "user_debounce (200..10000 ms)";
        return false;
    }
    if (cfg->stale_min_s < 300 || cfg->stale_min_s > 86400) {
        *why = "stale_min (300..86400 s)";
        return false;
    }
    *why = NULL;
    return true;
}
