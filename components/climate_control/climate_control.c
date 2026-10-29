#include "climate_control.h"

#include <stdlib.h>
#include <string.h>

#include "ac_profiles.h"
#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ir_transceiver.h"
#include "sdkconfig.h"

static const char *TAG = "climate";

#define TICK_MS 30000
#define BOOT_RESEND_DELAY_MS 5000
#define PERSIST_DELAY_MS 10000

typedef enum {
    EV_USER,
    EV_TEMP,
    EV_FAN_SWING,
    EV_CONFIG,
    EV_PROFILE,
    EV_FORCE,
} ev_type_t;

typedef struct {
    ev_type_t type;
    union {
        struct {
            uint8_t mode;
            int16_t cool, heat;
            uint32_t mask;
        } user;
        struct {
            bool valid;
            int16_t centi;
        } temp;
        struct {
            uint8_t fan, swing;
        } fs;
        app_config_t cfg;
    };
} event_t;

typedef struct {
    uint8_t version;
    uint8_t fan;
    uint8_t swing;
    uint8_t has_sent;
    ac_state_t last_desired;
} runtime_blob_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static climate_status_t s_status;
static app_config_t s_cfg;
static climate_algo_t s_algo;
static climate_params_t s_params;
static ac_state_t s_last_desired_sent;
static bool s_has_desired_sent;
static int64_t s_send_at_ms;      /* 0 = nothing scheduled */
static bool s_force_next;
static int64_t s_persist_at_ms;

static int64_t now_ms(void)
{
    return app_time_us() / 1000;
}

static void refresh_params(void)
{
    s_params.external = (s_cfg.control_mode == CONTROL_MODE_EXTERNAL);
    s_params.deadband_centi = (int16_t)s_cfg.deadband_centi;
    s_params.max_offset_centi = (int16_t)s_cfg.max_offset_centi;
    s_params.comp_interval_ms = (uint32_t)s_cfg.comp_interval_s * 1000u;
    s_params.filter_tau_ms = (uint32_t)s_cfg.filter_tau_s * 1000u;
    ac_caps_t caps;
    ac_profiles_get_caps(ac_profiles_active_slot(), &caps);
    s_params.temp_step_half = caps.temp_step_half ? caps.temp_step_half : 2;
    ac_profile_meta_t meta;
    uint16_t carrier = CONFIG_APP_IR_CARRIER_HZ;
    if (ac_profiles_get_meta(ac_profiles_active_slot(), &meta) == ESP_OK && meta.carrier_hz != 0) {
        carrier = meta.carrier_hz;
    }
    ir_set_carrier(carrier);
    for (int m = 0; m < AC_MODE_COUNT; m++) {
        s_params.min_half[m] = caps.temp_min_half[m];
        s_params.max_half[m] = caps.temp_max_half[m];
    }
}

static void persist_runtime(void)
{
    runtime_blob_t b = {
        .version = 1,
        .fan = s_status.fan,
        .swing = s_status.swing,
        .has_sent = s_has_desired_sent ? 1 : 0,
        .last_desired = s_last_desired_sent,
    };
    cfg_set_blob(CFG_PART_MAIN, CFG_NS_RUNTIME, "climate", &b, sizeof(b));
}

static void load_runtime(void)
{
    runtime_blob_t b;
    size_t len = sizeof(b);
    if (cfg_get_blob(CFG_PART_MAIN, CFG_NS_RUNTIME, "climate", &b, &len) == ESP_OK && len == sizeof(b) && b.version == 1) {
        s_status.fan = b.fan < AC_FAN_COUNT ? b.fan : AC_FAN_AUTO;
        s_status.swing = b.swing < AC_SWING_COUNT ? b.swing : AC_SWING_OFF;
        s_has_desired_sent = b.has_sent && ac_state_valid(&b.last_desired);
        s_last_desired_sent = b.last_desired;
    } else {
        ac_profile_meta_t meta;
        if (ac_profiles_get_meta(ac_profiles_active_slot(), &meta) == ESP_OK) {
            s_status.fan = meta.default_fan;
            s_status.swing = meta.default_swing;
        }
    }
}

/* Evaluate the algorithm, update the status, return the desired state. */
static void evaluate(climate_output_t *out)
{
    climate_inputs_t in = {
        .user_mode = s_status.user_mode,
        .cool_target_centi = s_status.cool_target_centi,
        .heat_target_centi = s_status.heat_target_centi,
        .fan = s_status.fan,
        .swing = s_status.swing,
        .temp_valid = s_status.temp_valid,
        .now_ms = now_ms(),
    };
    climate_algo_step(&s_algo, &s_params, &in, out);
    const char *why = NULL;
    bool ready = ac_profiles_ready_for_control(&why);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.ready = ready;
    strlcpy(s_status.not_ready, why ? why : "", sizeof(s_status.not_ready));
    s_status.control_mode = s_cfg.control_mode;
    s_status.compensating = out->compensating;
    s_status.offset_centi = out->offset_centi;
    s_status.ir_setpoint_centi = out->ir_setpoint_centi;
    s_status.reason = out->reason;
    s_status.desired = out->desired;
    s_status.has_filtered = s_algo.has_filtered;
    s_status.filtered_centi = (int16_t)s_algo.filtered_centi;
    xSemaphoreGive(s_lock);
}

static void transmit(const climate_output_t *out, bool force)
{
    if (!s_status.ready) {
        ESP_LOGW(TAG, "not transmitting: %s", s_status.not_ready);
        return;
    }
    ac_state_t chosen;
    ac_resolve_kind_t kind = AC_RESOLVE_NONE;
    uint8_t *blob = NULL;
    size_t len = 0;
    esp_err_t err = ac_profiles_build_command(&out->desired, out->user_target_half, &chosen, &kind, &blob, &len);
    if (err != ESP_OK) {
        char desc[64]; /* + 24 byte prefix fits last_error[96] */
        ac_state_describe(&out->desired, desc, sizeof(desc));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_status.last_error, sizeof(s_status.last_error), "Chưa học lệnh cho: %s", desc);
        xSemaphoreGive(s_lock);
        app_log_error("climate", "no learned command for %s (%s)", desc, esp_err_to_name(err));
        /* Do not retry the same impossible state on every tick. */
        s_last_desired_sent = out->desired;
        s_has_desired_sent = true;
        return;
    }
    char label[64];
    ac_state_describe(&chosen, label, sizeof(label));
    err = ir_send(blob, len, IR_SRC_CLIMATE, force, 0, label);
    free(blob);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        s_status.has_sent = true;
        s_status.last_sent = chosen;
        s_status.last_kind = kind;
        s_status.last_sent_us = app_time_us();
        s_status.last_error[0] = 0;
    } else {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "Hàng đợi IR từ chối: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        s_last_desired_sent = out->desired;
        s_has_desired_sent = true;
        s_persist_at_ms = now_ms() + PERSIST_DELAY_MS;
    }
}

/* Decide whether the new desired state must be sent, and when. */
static void schedule(bool user_origin)
{
    climate_output_t out;
    evaluate(&out);
    if (!s_status.ready) {
        s_send_at_ms = 0; /* nothing is transmitted until a locked profile exists */
        return;
    }
    bool changed = !s_has_desired_sent || !ac_state_equal(&out.desired, &s_last_desired_sent);
    if (!changed && !s_force_next) {
        if (user_origin && s_send_at_ms != 0) {
            s_send_at_ms = 0; /* user reverted to the already sent state within the debounce */
        }
        return;
    }
    if (out.desired.power == 0) {
        s_send_at_ms = now_ms(); /* OFF: no debounce, highest priority */
    } else if (user_origin) {
        s_send_at_ms = now_ms() + s_cfg.user_debounce_ms;
    } else if (s_send_at_ms == 0) {
        s_send_at_ms = now_ms();
    }
}

static void handle_event(const event_t *ev)
{
    switch (ev->type) {
    case EV_USER: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ev->user.mask & CLIMATE_USER_MODE) {
            s_status.user_mode = ev->user.mode;
        }
        if (ev->user.mask & CLIMATE_USER_COOL_SP) {
            s_status.cool_target_centi = ev->user.cool;
        }
        if (ev->user.mask & CLIMATE_USER_HEAT_SP) {
            s_status.heat_target_centi = ev->user.heat;
        }
        xSemaphoreGive(s_lock);
        climate_algo_note_user_change(&s_algo, now_ms());
        if (ev->user.mask & CLIMATE_USER_BOOT) {
            /* boot: restore without transmitting unless the user asked for it */
            if (s_cfg.resend_on_boot) {
                s_force_next = true;
                s_send_at_ms = now_ms() + BOOT_RESEND_DELAY_MS;
            } else {
                climate_output_t out;
                evaluate(&out);
            }
            break;
        }
        schedule(true);
        break;
    }
    case EV_TEMP:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.temp_valid = ev->temp.valid;
        xSemaphoreGive(s_lock);
        if (ev->temp.valid) {
            climate_algo_feed(&s_algo, &s_params, ev->temp.centi, now_ms());
        } else {
            climate_algo_reset_filter(&s_algo);
        }
        schedule(false);
        break;
    case EV_FAN_SWING:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.fan = ev->fs.fan;
        s_status.swing = ev->fs.swing;
        xSemaphoreGive(s_lock);
        s_persist_at_ms = now_ms() + PERSIST_DELAY_MS;
        schedule(true);
        break;
    case EV_CONFIG:
        s_cfg = ev->cfg;
        refresh_params();
        schedule(false);
        break;
    case EV_PROFILE:
        refresh_params();
        s_has_desired_sent = false; /* new profile: the AC state is unknown */
        {
            climate_output_t out;
            evaluate(&out);
        }
        break;
    case EV_FORCE:
        s_force_next = true;
        s_send_at_ms = now_ms();
        break;
    }
}

static void task(void *arg)
{
    (void)arg;
    int64_t next_tick = now_ms() + TICK_MS;
    for (;;) {
        int64_t now = now_ms();
        int64_t wake = next_tick;
        if (s_send_at_ms != 0 && s_send_at_ms < wake) {
            wake = s_send_at_ms;
        }
        if (s_persist_at_ms != 0 && s_persist_at_ms < wake) {
            wake = s_persist_at_ms;
        }
        TickType_t wait = wake > now ? pdMS_TO_TICKS(wake - now) : 0;
        event_t ev;
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            handle_event(&ev);
            continue;
        }
        now = now_ms();
        if (now >= next_tick) {
            next_tick = now + TICK_MS;
            schedule(false); /* time based corrections (comp_interval) */
        }
        if (s_send_at_ms != 0 && now >= s_send_at_ms) {
            s_send_at_ms = 0;
            climate_output_t out;
            evaluate(&out);
            bool force = s_force_next;
            s_force_next = false;
            if (force || !s_has_desired_sent || !ac_state_equal(&out.desired, &s_last_desired_sent)) {
                transmit(&out, force);
            }
        }
        if (s_persist_at_ms != 0 && now >= s_persist_at_ms) {
            s_persist_at_ms = 0;
            persist_runtime();
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.pending_ms = s_send_at_ms ? (uint32_t)(s_send_at_ms > now ? s_send_at_ms - now : 0) : 0;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t climate_control_start(const app_config_t *cfg)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_queue = xQueueCreate(16, sizeof(event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_cfg = *cfg;
    climate_algo_init(&s_algo);
    memset(&s_status, 0, sizeof(s_status));
    s_status.cool_target_centi = 2600;
    s_status.heat_target_centi = 2400;
    load_runtime();
    refresh_params();
    s_status.running = true;
    if (xTaskCreate(task, "climate", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void post(const event_t *ev)
{
    if (s_queue == NULL) {
        return;
    }
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        app_log_error("climate", "event queue full, event %d dropped", ev->type);
    }
}

void climate_control_on_user_change(uint8_t mode, int16_t cool_centi, int16_t heat_centi, uint32_t mask)
{
    event_t ev = { .type = EV_USER };
    ev.user.mode = mode;
    ev.user.cool = cool_centi;
    ev.user.heat = heat_centi;
    ev.user.mask = mask;
    post(&ev);
}

void climate_control_post_temperature(bool valid, int16_t centi)
{
    event_t ev = { .type = EV_TEMP };
    ev.temp.valid = valid;
    ev.temp.centi = centi;
    post(&ev);
}

esp_err_t climate_control_set_fan_swing(uint8_t fan, uint8_t swing)
{
    if (fan >= AC_FAN_COUNT || swing >= AC_SWING_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    event_t ev = { .type = EV_FAN_SWING };
    ev.fs.fan = fan;
    ev.fs.swing = swing;
    post(&ev);
    return ESP_OK;
}

void climate_control_config_changed(const app_config_t *cfg)
{
    event_t ev = { .type = EV_CONFIG };
    ev.cfg = *cfg;
    post(&ev);
}

void climate_control_profile_changed(void)
{
    event_t ev = { .type = EV_PROFILE };
    post(&ev);
}

esp_err_t climate_control_force_resend(void)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    event_t ev = { .type = EV_FORCE };
    post(&ev);
    return ESP_OK;
}

void climate_control_get_status(climate_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
