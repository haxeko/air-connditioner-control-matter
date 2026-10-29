/*
 * REST-like API of the configuration portal. GET returns JSON; POST takes form fields
 * (application/x-www-form-urlencoded) and answers {"ok":true,...} or {"ok":false,"error":"..."}.
 * Handlers only start asynchronous operations and read state: none waits for pairing, IR
 * learning, Wi-Fi connection or a sensor read.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_profiles.h"
#include "app_state.h"
#include "climate_control.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "ir_transceiver.h"
#include "matter_commissioner.h"
#include "matter_thermostat.h"
#include "qrcode.h"
#include "remote_temperature.h"
#include "sdkconfig.h"
#include "web_api.h"
#include "web_util.h"
#include "wifi_config.h"

#ifndef APP_ESP_MATTER_VERSION
#define APP_ESP_MATTER_VERSION "?"
#endif

#if CONFIG_APP_TEST_SIMULATED_SENSOR
#define TEST_MODE true
#else
#define TEST_MODE false
#endif

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                      */
/* ------------------------------------------------------------------------------------------ */

static void put_age(wj_t *w, const char *key, int64_t t_us)
{
    if (t_us <= 0) {
        wj_null(w, key);
    } else {
        int64_t age = (app_time_us() - t_us) / 1000000LL;
        wj_int(w, key, age < 0 ? 0 : age);
    }
}

static void put_state(wj_t *w, const char *key, const ac_state_t *s)
{
    char k[16] = "";
    char desc[64];
    ac_state_key(s, k, sizeof(k));
    ac_state_describe(s, desc, sizeof(desc));
    wj_obj_begin(w, key);
    wj_uint(w, "power", s->power);
    wj_uint(w, "mode", s->mode);
    wj_uint(w, "temp_half", s->temp_half);
    wj_uint(w, "fan", s->fan);
    wj_uint(w, "swing", s->swing);
    wj_str(w, "key", k);
    wj_str(w, "desc", desc);
    wj_obj_end(w);
}

static bool get_slot(api_ctx_t *c, const char *src, long *slot)
{
    if (!form_get_int(src, "slot", 0, AC_PROFILE_SLOTS - 1, slot)) {
        api_fail(c, 400, "Ô profile không hợp lệ");
        return false;
    }
    return true;
}

static const char *sensor_name(const sensor_binding_t *b)
{
    if (b->label[0]) {
        return b->label;
    }
    if (b->product_name[0]) {
        return b->product_name;
    }
    return b->vendor_name;
}

static void put_wifi(wj_t *w, const wifi_status_t *s)
{
    wj_obj_begin(w, "wifi");
    wj_bool(w, "configured", s->sta_configured);
    wj_str(w, "ssid", s->sta_ssid);
    wj_bool(w, "connected", s->sta_connected);
    wj_int(w, "rssi", s->sta_rssi);
    wj_uint(w, "channel", s->sta_channel);
    wj_str(w, "ip4", s->sta_ip4);
    wj_arr_begin(w, "ip6");
    for (uint8_t i = 0; i < s->sta_ip6_count && i < 3; i++) {
        wj_str(w, NULL, s->sta_ip6[i]);
    }
    wj_arr_end(w);
    wj_bool(w, "conflict", s->subnet_conflict);
    wj_bool(w, "portal_open", s->portal_open);
    wj_str(w, "ap_ssid", s->ap_ssid);
    wj_uint(w, "ap_clients", s->ap_clients);
    wj_uint(w, "portal_remaining_s", s->portal_remaining_s);
    wj_int(w, "job", s->job);
    wj_str(w, "job_message", s->job_message);
    wj_bool(w, "scanning", s->scanning);
    wj_obj_end(w);
}

static void put_matter(wj_t *w)
{
    matter_status_t m;
    matter_thermostat_get_status(&m);
    wj_obj_begin(w, "matter");
    wj_bool(w, "started", m.started);
    wj_bool(w, "commissioner_ready", m.commissioner_ready);
    wj_uint(w, "home_fabrics", m.home_fabric_count);
    wj_bool(w, "window_open", m.window_open);
    wj_uint(w, "endpoint", m.endpoint_id);
    wj_bool(w, "heat", m.heat_feature);
    wj_uint(w, "vendor_id", m.vendor_id);
    wj_uint(w, "product_id", m.product_id);
    wj_uint(w, "discriminator", m.discriminator);
    wj_obj_end(w);
}

static void put_temperature(wj_t *w, const rt_status_t *rs)
{
    wj_obj_begin(w, "temp");
    wj_bool(w, "valid", rs->effective_valid);
    if (rs->effective_valid) {
        wj_int(w, "centi", rs->effective_centi);
    } else {
        wj_null(w, "centi");
    }
    wj_int(w, "source", rs->source);
    wj_str(w, "source_name", temp_source_name(rs->source));
    if (rs->source == TEMP_SRC_LOCAL) {
        put_age(w, "value_age_s", rs->local_time_us);
        put_age(w, "alive_age_s", rs->local_time_us);
    } else {
        put_age(w, "value_age_s", rs->value_time_us);
        put_age(w, "alive_age_s", rs->alive_time_us);
    }
    wj_bool(w, "simulated", rs->simulated);
    wj_obj_end(w);
}

static void put_sensor(wj_t *w, const rt_status_t *rs, bool full)
{
    wj_obj_begin(w, "sensor");
    wj_bool(w, "bound", rs->bound);
    wj_str(w, "name", rs->bound ? sensor_name(&rs->binding) : "");
    wj_int(w, "state", rs->state);
    wj_str(w, "state_name", rt_state_name(rs->state));
    wj_bool(w, "stale", rs->stale);
    wj_str(w, "error", rs->error);
    if (full) {
        if (rs->bound) {
            const sensor_binding_t *b = &rs->binding;
            wj_hex64(w, "node_id", b->node_id);
            wj_uint(w, "endpoint", b->endpoint);
            wj_uint(w, "vendor_id", b->vendor_id);
            wj_uint(w, "product_id", b->product_id);
            wj_str(w, "vendor_name", b->vendor_name);
            wj_str(w, "product_name", b->product_name);
            wj_str(w, "label", b->label);
        }
        wj_bool(w, "has_value", rs->has_value);
        wj_bool(w, "value_null", rs->value_null);
        wj_bool(w, "value_plausible", rs->value_plausible);
        if (rs->has_value && !rs->value_null) {
            wj_int(w, "value", rs->value_centi);
        } else {
            wj_null(w, "value");
        }
        put_age(w, "value_age_s", rs->value_time_us);
        put_age(w, "alive_age_s", rs->alive_time_us);
        if (rs->has_bounds_min) {
            wj_int(w, "min", rs->min_centi);
        } else {
            wj_null(w, "min");
        }
        if (rs->has_bounds_max) {
            wj_int(w, "max", rs->max_centi);
        } else {
            wj_null(w, "max");
        }
        wj_uint(w, "min_interval_s", rs->min_interval_s);
        wj_uint(w, "max_interval_s", rs->max_interval_s);
        wj_uint(w, "stale_timeout_s", rs->stale_timeout_s);
        wj_uint(w, "retries", rs->retries);
        wj_uint(w, "next_retry_ms", rs->next_retry_ms);
        wj_bool(w, "local_configured", rs->local_configured);
        wj_bool(w, "local_valid", rs->local_valid);
        if (rs->local_valid) {
            wj_int(w, "local", rs->local_centi);
        } else {
            wj_null(w, "local");
        }
        put_age(w, "local_age_s", rs->local_time_us);
    }
    wj_obj_end(w);
}

static void put_climate(wj_t *w)
{
    climate_status_t cs;
    climate_control_get_status(&cs);
    wj_obj_begin(w, "climate");
    wj_bool(w, "running", cs.running);
    wj_bool(w, "ready", cs.ready);
    wj_str(w, "not_ready", cs.not_ready);
    wj_uint(w, "user_mode", cs.user_mode);
    wj_int(w, "cool_target", cs.cool_target_centi);
    wj_int(w, "heat_target", cs.heat_target_centi);
    wj_uint(w, "fan", cs.fan);
    wj_uint(w, "swing", cs.swing);
    wj_bool(w, "temp_valid", cs.temp_valid);
    if (cs.has_filtered) {
        wj_int(w, "filtered", cs.filtered_centi);
    } else {
        wj_null(w, "filtered");
    }
    wj_uint(w, "control_mode", cs.control_mode);
    wj_bool(w, "compensating", cs.compensating);
    wj_int(w, "offset", cs.offset_centi);
    wj_int(w, "ir_setpoint", cs.ir_setpoint_centi);
    wj_int(w, "reason", cs.reason);
    wj_str(w, "reason_text", climate_reason_text(cs.reason));
    put_state(w, "desired", &cs.desired);
    wj_bool(w, "has_sent", cs.has_sent);
    if (cs.has_sent) {
        put_state(w, "last_sent", &cs.last_sent);
        wj_int(w, "last_kind", cs.last_kind);
        put_age(w, "last_sent_age_s", cs.last_sent_us);
    }
    wj_uint(w, "pending_ms", cs.pending_ms);
    wj_str(w, "last_error", cs.last_error);
    wj_obj_end(w);
}

static void put_ir(wj_t *w)
{
    ir_tx_status_t t;
    ir_tx_get_status(&t);
    wj_obj_begin(w, "ir");
    wj_bool(w, "ready", t.initialised);
    wj_bool(w, "busy", t.busy);
    wj_uint(w, "sent", t.sent);
    wj_uint(w, "failed", t.failed);
    wj_uint(w, "dropped_duplicate", t.dropped_duplicate);
    wj_uint(w, "superseded", t.superseded);
    put_age(w, "last_age_s", t.last_sent_us);
    wj_str(w, "last_label", t.last_label);
    wj_uint(w, "last_source", t.last_source);
    wj_str(w, "last_error", t.last_error == ESP_OK ? "" : esp_err_to_name(t.last_error));
    wj_uint(w, "carrier_hz", t.carrier_hz);
    wj_obj_end(w);
}

static void put_learn(wj_t *w)
{
    ir_learn_status_t l;
    ir_learn_get_status(&l);
    wj_obj_begin(w, "learn");
    wj_int(w, "state", l.state);
    wj_uint(w, "session", l.session);
    wj_str(w, "message", l.message);
    wj_uint(w, "segments", l.segments);
    wj_uint(w, "levels", l.levels);
    wj_uint(w, "duration_ms", l.duration_ms);
    wj_uint(w, "noise_discarded", l.noise_discarded);
    wj_uint(w, "result_size", l.result_size);
    wj_obj_end(w);
}

static bool limits_differ(const ac_thermostat_limits_t *l, const thermostat_caps_t *t)
{
    if (l->heat != t->heat || l->dry != t->dry || l->fan_only != t->fan_only || l->cool_min != t->cool_min ||
        l->cool_max != t->cool_max) {
        return true;
    }
    return l->heat && (l->heat_min != t->heat_min || l->heat_max != t->heat_max);
}

/** Matter limits are applied at boot: tell the UI when the active profile now implies others. */
static bool restart_needed(void)
{
    ac_thermostat_limits_t lim;
    ac_profiles_thermostat_limits(ac_profiles_active_slot(), &lim);
    thermostat_caps_t tc;
    matter_thermostat_get_caps(&tc);
    return limits_differ(&lim, &tc);
}

/* ------------------------------------------------------------------------------------------ */
/* Session, overview, portal                                                                    */
/* ------------------------------------------------------------------------------------------ */

static void get_session(api_ctx_t *c)
{
    wj_str(c->w, "token", web_session_token());
    wj_str(c->w, "device", CONFIG_APP_DEVICE_NAME);
    wj_str(c->w, "version", esp_app_get_description()->version);
    wj_bool(c->w, "test_mode", TEST_MODE);
}

static void get_status(api_ctx_t *c)
{
    wj_t *w = c->w;
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    put_wifi(w, &ws);
    put_matter(w);
    rt_status_t rs;
    remote_temperature_get_status(&rs);
    put_temperature(w, &rs);
    put_sensor(w, &rs, false);
    put_climate(w);
    put_ir(w);

    int slot = ac_profiles_active_slot();
    ac_profile_meta_t meta;
    wj_obj_begin(w, "profile");
    if (slot >= 0 && ac_profiles_get_meta(slot, &meta) == ESP_OK) {
        ac_caps_t caps;
        ac_profiles_get_caps(slot, &caps);
        wj_int(w, "slot", slot);
        wj_str(w, "name", meta.name);
        wj_bool(w, "locked", meta.locked);
        wj_uint(w, "codes", caps.code_count);
    } else {
        wj_int(w, "slot", -1);
    }
    wj_bool(w, "restart_needed", restart_needed());
    wj_obj_end(w);

    thermostat_user_state_t us;
    matter_thermostat_get_user_state(&us);
    wj_obj_begin(w, "thermostat");
    wj_uint(w, "mode", us.system_mode);
    wj_int(w, "cool_sp", us.cool_setpoint);
    wj_int(w, "heat_sp", us.heat_setpoint);
    wj_obj_end(w);

    wj_uint(w, "uptime_s", app_time_s());
    wj_uint(w, "error_count", app_get_error_count());
    wj_bool(w, "test_mode", TEST_MODE);
}

static void post_portal_extend(api_ctx_t *c)
{
    (void)c; /* every POST restarts the idle timeout */
}

static void post_portal_close(api_ctx_t *c)
{
    if (app_state_get_busy() & (APP_BUSY_PAIRING | APP_BUSY_IR_LEARNING)) {
        api_fail(c, 409, "Đang ghép cảm biến hoặc học IR: hãy chờ xong rồi thoát");
        return;
    }
    if (web_defer(WEB_DEFER_CLOSE_PORTAL, 0, 1000) != ESP_OK) {
        api_fail(c, 503, "Không thể đóng cấu hình lúc này");
        return;
    }
    wj_str(c->w, "message", "Đã thoát cấu hình: Wi-Fi cấu hình sẽ tắt sau 1 giây");
}

/* ------------------------------------------------------------------------------------------ */
/* Wi-Fi                                                                                        */
/* ------------------------------------------------------------------------------------------ */

static void get_wifi(api_ctx_t *c)
{
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    put_wifi(c->w, &ws);
    wifi_scan_entry_t list[WIFI_SCAN_MAX];
    size_t n = wifi_config_scan_results(list, WIFI_SCAN_MAX);
    wj_arr_begin(c->w, "scan");
    for (size_t i = 0; i < n; i++) {
        wj_obj_begin(c->w, NULL);
        wj_str(c->w, "ssid", list[i].ssid);
        wj_int(c->w, "rssi", list[i].rssi);
        wj_uint(c->w, "channel", list[i].channel);
        wj_bool(c->w, "open", list[i].authmode == 0 /* WIFI_AUTH_OPEN */);
        wj_obj_end(c->w);
    }
    wj_arr_end(c->w);
}

static void post_wifi_scan(api_ctx_t *c)
{
    esp_err_t err = wifi_config_scan_start();
    if (err != ESP_OK) {
        api_fail(c, 409, "Không quét được Wi-Fi: %s", esp_err_to_name(err));
    }
}

static void post_wifi_sta(api_ctx_t *c)
{
    char ssid[33];
    char pass[65];
    int ls = form_get(c->body, "ssid", ssid, sizeof(ssid));
    if (ls < 1 || ls > 32) {
        api_fail(c, 400, "Tên Wi-Fi (SSID) phải dài 1-32 byte");
        return;
    }
    int lp = form_get(c->body, "password", pass, sizeof(pass));
    if (lp == -1) {
        lp = 0;
        pass[0] = 0;
    }
    if (lp < 0 || (lp > 0 && (lp < 8 || lp > 63))) {
        memset(pass, 0, sizeof(pass));
        api_fail(c, 400, "Mật khẩu Wi-Fi phải dài 8-63 ký tự (để trống nếu mạng không mật khẩu)");
        return;
    }
    esp_err_t err = wifi_config_set_sta(ssid, pass);
    memset(pass, 0, sizeof(pass));
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Đang thử kết nối một mạng khác, hãy chờ kết quả");
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không lưu được Wi-Fi: %s", esp_err_to_name(err));
    } else {
        wj_str(c->w, "message", "Đã lưu, đang kết nối...");
    }
}

static void post_wifi_ap_password(api_ctx_t *c)
{
    char p1[65];
    char p2[65];
    int l1 = form_get(c->body, "password", p1, sizeof(p1));
    int l2 = form_get(c->body, "confirm", p2, sizeof(p2));
    if (l1 < 8 || l1 > 63) {
        api_fail(c, 400, "Mật khẩu Wi-Fi cấu hình phải dài 8-63 ký tự");
    } else if (l2 != l1 || strcmp(p1, p2) != 0) {
        api_fail(c, 400, "Hai mật khẩu không khớp");
    } else {
        esp_err_t err = wifi_config_set_ap_password(p1);
        if (err == ESP_ERR_INVALID_ARG) {
            api_fail(c, 400, "Chỉ dùng ký tự ASCII in được (không dấu)");
        } else if (err != ESP_OK) {
            api_fail(c, 500, "Không lưu được: %s", esp_err_to_name(err));
        } else {
            wj_str(c->w, "message", "Đã lưu. Mật khẩu mới có hiệu lực từ lần mở cấu hình sau");
        }
    }
    memset(p1, 0, sizeof(p1));
    memset(p2, 0, sizeof(p2));
}

/* ------------------------------------------------------------------------------------------ */
/* Matter (home ecosystem, Fabric A)                                                            */
/* ------------------------------------------------------------------------------------------ */

static void get_matter(api_ctx_t *c)
{
    put_matter(c->w);
    matter_fabric_info_t fabrics[CONFIG_MAX_FABRICS];
    size_t n = matter_thermostat_list_home_fabrics(fabrics, sizeof(fabrics) / sizeof(fabrics[0]));
    wj_arr_begin(c->w, "fabrics");
    for (size_t i = 0; i < n; i++) {
        wj_obj_begin(c->w, NULL);
        wj_uint(c->w, "index", fabrics[i].fabric_index);
        wj_uint(c->w, "vendor_id", fabrics[i].vendor_id);
        wj_str(c->w, "label", fabrics[i].label);
        wj_obj_end(c->w);
    }
    wj_arr_end(c->w);
    wj_uint(c->w, "window_max_s", CONFIG_APP_COMMISSIONING_WINDOW_S);
}

#define QR_MAX_SIDE 57 /* version 10 */

typedef struct {
    int size;
    uint8_t bits[(QR_MAX_SIDE * QR_MAX_SIDE + 7) / 8];
} qr_matrix_t;

static void qr_capture(esp_qrcode_handle_t q, void *user)
{
    qr_matrix_t *m = (qr_matrix_t *)user;
    int n = esp_qrcode_get_size(q);
    if (n <= 0 || n > QR_MAX_SIDE) {
        m->size = 0;
        return;
    }
    m->size = n;
    memset(m->bits, 0, sizeof(m->bits));
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            if (esp_qrcode_get_module(q, x, y)) {
                int i = y * n + x;
                m->bits[i / 8] |= (uint8_t)(0x80u >> (i % 8));
            }
        }
    }
}

static void get_matter_onboarding(api_ctx_t *c)
{
    char qr[48];
    char manual[24];
    if (matter_thermostat_get_onboarding(qr, sizeof(qr), manual, sizeof(manual)) != ESP_OK) {
        api_fail(c, 503, "Matter chưa sẵn sàng");
        return;
    }
    qr_matrix_t *m = calloc(1, sizeof(qr_matrix_t));
    if (m == NULL) {
        api_fail(c, 503, "Hết bộ nhớ");
        return;
    }
    esp_qrcode_config_t cfg = {
        .display_func_with_cb = qr_capture,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
        .user_data = m,
    };
    esp_err_t err = esp_qrcode_generate(&cfg, qr);
    wj_str(c->w, "qr", qr);
    wj_str(c->w, "manual", manual);
    if (err == ESP_OK && m->size > 0) {
        /* one hexadecimal string per row, most significant bit = leftmost module */
        wj_uint(c->w, "qr_size", (uint64_t)m->size);
        wj_arr_begin(c->w, "qr_rows");
        char row[QR_MAX_SIDE / 4 + 2];
        for (int y = 0; y < m->size; y++) {
            int digits = (m->size + 3) / 4;
            for (int d = 0; d < digits; d++) {
                unsigned nib = 0;
                for (int b = 0; b < 4; b++) {
                    int x = d * 4 + b;
                    nib <<= 1;
                    if (x < m->size) {
                        int i = y * m->size + x;
                        nib |= (m->bits[i / 8] >> (7 - i % 8)) & 1u;
                    }
                }
                row[d] = "0123456789abcdef"[nib];
            }
            row[digits] = 0;
            wj_str(c->w, NULL, row);
        }
        wj_arr_end(c->w);
    }
    memset(qr, 0, sizeof(qr));
    memset(manual, 0, sizeof(manual));
    free(m);
}

static void post_matter_window(api_ctx_t *c)
{
    char action[8];
    if (form_get(c->body, "action", action, sizeof(action)) <= 0) {
        api_fail(c, 400, "Thiếu thao tác");
        return;
    }
    if (strcmp(action, "close") == 0) {
        esp_err_t err = matter_thermostat_close_commissioning_window();
        if (err != ESP_OK) {
            api_fail(c, 503, "Không đóng được cửa sổ ghép: %s", esp_err_to_name(err));
        }
        return;
    }
    if (strcmp(action, "open") != 0) {
        api_fail(c, 400, "Thao tác không hợp lệ");
        return;
    }
    long minutes = 0;
    if (!form_get_int(c->body, "minutes", 3, CONFIG_APP_COMMISSIONING_WINDOW_S / 60, &minutes)) {
        api_fail(c, 400, "Thời gian mở phải từ 3 đến %d phút", CONFIG_APP_COMMISSIONING_WINDOW_S / 60);
        return;
    }
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    if (!ws.sta_connected) {
        api_fail(c, 409, "ESP chưa kết nối Wi-Fi nhà: ứng dụng nhà sẽ không tìm thấy thiết bị");
        return;
    }
    esp_err_t err = matter_thermostat_open_commissioning_window((uint16_t)(minutes * 60));
    if (err != ESP_OK) {
        api_fail(c, 503, "Không mở được cửa sổ ghép: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Matter sensor (Fabric B)                                                                     */
/* ------------------------------------------------------------------------------------------ */

static void put_progress(wj_t *w, const commissioner_progress_t *p)
{
    wj_obj_begin(w, "progress");
    wj_int(w, "state", p->state);
    wj_str(w, "stage", p->stage);
    wj_str(w, "error", p->error);
    wj_bool(w, "attestation_overridden", p->attestation_overridden);
    wj_str(w, "attestation_note", p->attestation_note);
    wj_hex64(w, "node_id", p->node_id);
    wj_bool(w, "already_on_fabric", p->already_on_fabric);
    wj_uint(w, "vendor_id", p->vendor_id);
    wj_uint(w, "product_id", p->product_id);
    wj_str(w, "vendor_name", p->vendor_name);
    wj_str(w, "product_name", p->product_name);
    wj_str(w, "node_label", p->node_label);
    wj_arr_begin(w, "endpoints");
    for (uint8_t i = 0; i < p->endpoint_count && i < COMMISSIONER_MAX_ENDPOINTS; i++) {
        const commissioner_endpoint_t *e = &p->endpoints[i];
        wj_obj_begin(w, NULL);
        wj_uint(w, "endpoint", e->endpoint);
        wj_bool(w, "read_ok", e->read_ok);
        wj_bool(w, "null", e->value_null);
        if (e->read_ok && !e->value_null) {
            wj_int(w, "value", e->measured);
        } else {
            wj_null(w, "value");
        }
        if (e->has_min) {
            wj_int(w, "min", e->min_measured);
        } else {
            wj_null(w, "min");
        }
        if (e->has_max) {
            wj_int(w, "max", e->max_measured);
        } else {
            wj_null(w, "max");
        }
        wj_obj_end(w);
    }
    wj_arr_end(w);
    put_age(w, "started_age_s", p->started_us);
    put_age(w, "updated_age_s", p->updated_us);
    wj_obj_end(w);
}

static void get_sensor(api_ctx_t *c)
{
    rt_status_t rs;
    remote_temperature_get_status(&rs);
    put_sensor(c->w, &rs, true);
    put_temperature(c->w, &rs);
    commissioner_progress_t p;
    matter_commissioner_get_progress(&p);
    put_progress(c->w, &p);
    commissioner_info_t info;
    matter_commissioner_get_info(&info);
    wj_obj_begin(c->w, "commissioner");
    wj_bool(c->w, "ready", info.ready);
    wj_str(c->w, "init_error", info.init_error);
    wj_bool(c->w, "busy", matter_commissioner_busy());
    wj_obj_end(c->w);
    wj_bool(c->w, "test_mode", TEST_MODE);
}

static void post_sensor_pair(api_ctx_t *c)
{
    char code[48];
    if (form_get(c->body, "code", code, sizeof(code)) <= 0) {
        api_fail(c, 400, "Hãy nhập mã chia sẻ của cảm biến");
        return;
    }
    if (!matter_commissioner_validate_code(code)) {
        memset(code, 0, sizeof(code));
        api_fail(c, 400, "Mã chia sẻ không hợp lệ (11 hoặc 21 chữ số, hoặc mã QR bắt đầu bằng MT:)");
        return;
    }
    rt_status_t rs;
    remote_temperature_get_status(&rs);
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    commissioner_info_t info;
    matter_commissioner_get_info(&info);
    esp_err_t err = ESP_FAIL;
    if (rs.bound) {
        api_fail(c, 409, "Đang có cảm biến được ghép. Hãy gỡ cảm biến hiện tại trước");
    } else if (!ws.sta_connected) {
        api_fail(c, 409, "ESP chưa kết nối Wi-Fi nhà nên không tìm được cảm biến trên mạng");
    } else if (!info.ready) {
        api_fail(c, 503, "Commissioner chưa sẵn sàng: %s", info.init_error);
    } else {
        err = matter_commissioner_start_pairing(code, form_get_bool(c->body, "uncertified", false));
        if (err == ESP_ERR_INVALID_STATE) {
            api_fail(c, 409, "Đang có thao tác ghép/đọc/gỡ khác, hãy chờ");
        } else if (err == ESP_ERR_INVALID_ARG) {
            api_fail(c, 400, "Mã chia sẻ không hợp lệ");
        } else if (err != ESP_OK) {
            api_fail(c, 500, "Không bắt đầu ghép được: %s", esp_err_to_name(err));
        }
    }
    memset(code, 0, sizeof(code));
}

static void post_sensor_cancel(api_ctx_t *c)
{
    esp_err_t err = matter_commissioner_cancel();
    if (err != ESP_OK) {
        api_fail(c, 409, "Không có thao tác để hủy");
    }
}

static void post_sensor_bind(api_ctx_t *c)
{
    uint64_t node = 0;
    long endpoint = 0;
    if (!form_get_u64(c->body, "node", &node) || !form_get_int(c->body, "endpoint", 0, 0xFFFE, &endpoint)) {
        api_fail(c, 400, "Thiếu node hoặc endpoint");
        return;
    }
    commissioner_progress_t p;
    matter_commissioner_get_progress(&p);
    if (p.state != COMMISSIONER_DONE || p.node_id != node) {
        api_fail(c, 409, "Hãy ghép hoặc đọc thử cảm biến trước khi lưu");
        return;
    }
    const commissioner_endpoint_t *ep = NULL;
    for (uint8_t i = 0; i < p.endpoint_count && i < COMMISSIONER_MAX_ENDPOINTS; i++) {
        if (p.endpoints[i].endpoint == endpoint) {
            ep = &p.endpoints[i];
        }
    }
    if (ep == NULL) {
        api_fail(c, 400, "Endpoint %ld không có Temperature Measurement", endpoint);
        return;
    }
    if (!ep->read_ok) {
        api_fail(c, 409, "Endpoint %ld chưa đọc được, hãy đọc thử lại", endpoint);
        return;
    }
    esp_err_t err = remote_temperature_bind(node, (uint16_t)endpoint, p.vendor_id, p.product_id, p.vendor_name,
                                            p.product_name, p.node_label);
    if (err != ESP_OK) {
        api_fail(c, 500, "Không lưu được liên kết cảm biến: %s", esp_err_to_name(err));
        return;
    }
    wj_str(c->w, "message", "Đã lưu cảm biến, đang subscribe");
}

/** Node ID to act on: the bound sensor, else a paired-but-not-yet-saved node. */
static bool target_node(uint64_t *node)
{
    sensor_binding_t b;
    if (remote_temperature_get_binding(&b)) {
        *node = b.node_id;
        return true;
    }
    commissioner_progress_t p;
    matter_commissioner_get_progress(&p);
    if (p.node_id != 0 && (p.state == COMMISSIONER_DONE || p.state == COMMISSIONER_READING)) {
        *node = p.node_id;
        return true;
    }
    return false;
}

static void post_sensor_read(api_ctx_t *c)
{
    uint64_t node;
    if (!target_node(&node)) {
        api_fail(c, 409, "Chưa có cảm biến nào được ghép");
        return;
    }
    esp_err_t err = matter_commissioner_refresh(node);
    if (err != ESP_OK) {
        api_fail(c, 409, "Không đọc được lúc này (đang bận hoặc commissioner chưa sẵn sàng)");
    }
}

static void post_sensor_remove(api_ctx_t *c)
{
    uint64_t node;
    if (!target_node(&node)) {
        api_fail(c, 409, "Chưa có cảm biến nào được ghép");
        return;
    }
    esp_err_t err = matter_commissioner_remove_node(node);
    if (err != ESP_OK) {
        api_fail(c, 409, "Không gỡ được lúc này (đang bận hoặc commissioner chưa sẵn sàng)");
        return;
    }
    wj_str(c->w, "message", "Đang gửi RemoveFabric: chỉ xóa fabric của ESP, liên kết với hub cũ giữ nguyên");
}

static void post_sensor_forget(api_ctx_t *c)
{
    esp_err_t err = remote_temperature_unbind();
    if (err != ESP_OK) {
        api_fail(c, 500, "Không xóa được liên kết: %s", esp_err_to_name(err));
    }
}

static void post_sensor_retry(api_ctx_t *c)
{
    if (remote_temperature_retry_now() != ESP_OK) {
        api_fail(c, 503, "Không thể thử lại lúc này");
    }
}

static void post_sensor_simulate(api_ctx_t *c)
{
    long centi = 0;
    bool enable = form_get_bool(c->body, "enable", false);
    if (enable && !form_get_int(c->body, "centi", -4000, 8000, &centi)) {
        api_fail(c, 400, "Nhiệt độ giả lập không hợp lệ");
        return;
    }
    esp_err_t err = remote_temperature_set_simulated(enable, (int16_t)centi);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        api_fail(c, 404, "Firmware không bật chế độ thử nghiệm (CONFIG_APP_TEST_SIMULATED_SENSOR)");
    } else if (err != ESP_OK) {
        api_fail(c, 500, "%s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Air conditioner: profiles, learning, manual control                                          */
/* ------------------------------------------------------------------------------------------ */

static void put_caps(wj_t *w, const ac_caps_t *caps)
{
    wj_bool(w, "has_off", caps->has_off);
    wj_uint(w, "modes_mask", caps->modes_mask);
    wj_uint(w, "fans_mask", caps->fans_mask);
    wj_uint(w, "swing_mask", caps->swing_mask);
    wj_uint(w, "codes", caps->code_count);
    wj_arr_begin(w, "ranges");
    for (int m = 0; m < AC_MODE_COUNT; m++) {
        if (caps->modes_mask & (1u << m)) {
            wj_obj_begin(w, NULL);
            wj_int(w, "mode", m);
            wj_uint(w, "min_half", caps->temp_min_half[m]);
            wj_uint(w, "max_half", caps->temp_max_half[m]);
            wj_obj_end(w);
        }
    }
    wj_arr_end(w);
}

static void get_ac(api_ctx_t *c)
{
    wj_t *w = c->w;
    int active = ac_profiles_active_slot();
    wj_int(w, "active", active);
    wj_arr_begin(w, "profiles");
    for (int slot = 0; slot < AC_PROFILE_SLOTS; slot++) {
        ac_profile_meta_t m;
        bool present = ac_profiles_get_meta(slot, &m) == ESP_OK;
        wj_obj_begin(w, NULL);
        wj_int(w, "slot", slot);
        wj_bool(w, "present", present);
        if (present) {
            ac_caps_t caps;
            ac_profiles_get_caps(slot, &caps);
            wj_str(w, "name", m.name);
            wj_bool(w, "locked", m.locked);
            wj_uint(w, "driver", m.driver);
            wj_uint(w, "step_half", m.temp_step_half);
            wj_uint(w, "default_fan", m.default_fan);
            wj_uint(w, "default_swing", m.default_swing);
            wj_uint(w, "carrier_hz", m.carrier_hz);
            put_caps(w, &caps);
        }
        wj_obj_end(w);
    }
    wj_arr_end(w);
    const char *why = NULL;
    bool ready = ac_profiles_ready_for_control(&why);
    wj_bool(w, "ready", ready);
    wj_str(w, "not_ready", why ? why : "");
    wj_bool(w, "restart_needed", restart_needed());
    wj_uint(w, "default_carrier_hz", CONFIG_APP_IR_CARRIER_HZ);

    thermostat_user_state_t us;
    matter_thermostat_get_user_state(&us);
    thermostat_caps_t tc;
    matter_thermostat_get_caps(&tc);
    wj_obj_begin(w, "thermostat");
    wj_uint(w, "mode", us.system_mode);
    wj_int(w, "cool_sp", us.cool_setpoint);
    wj_int(w, "heat_sp", us.heat_setpoint);
    wj_bool(w, "heat", tc.heat);
    wj_bool(w, "dry", tc.dry);
    wj_bool(w, "fan_only", tc.fan_only);
    wj_int(w, "cool_min", tc.cool_min);
    wj_int(w, "cool_max", tc.cool_max);
    wj_int(w, "heat_min", tc.heat_min);
    wj_int(w, "heat_max", tc.heat_max);
    wj_obj_end(w);
    put_climate(w);
    put_ir(w);
    put_learn(w);
}

static void get_ac_codes(api_ctx_t *c)
{
    long slot;
    if (!get_slot(c, c->query, &slot)) {
        return;
    }
    ac_state_t *list = malloc(AC_PROFILE_MAX_CODES * sizeof(ac_state_t));
    if (list == NULL) {
        api_fail(c, 503, "Hết bộ nhớ");
        return;
    }
    size_t n = ac_profiles_list_codes((int)slot, list, AC_PROFILE_MAX_CODES);
    wj_int(c->w, "slot", slot);
    /* keys only ("off", "m<mode>t<temp_half>f<fan>s<swing>"): the UI decodes and sorts them */
    wj_arr_begin(c->w, "codes");
    for (size_t i = 0; i < n; i++) {
        char key[16];
        if (ac_state_key(&list[i], key, sizeof(key))) {
            wj_str(c->w, NULL, key);
        }
    }
    wj_arr_end(c->w);
    free(list);
}

static bool get_name(api_ctx_t *c, char *name, size_t len)
{
    int l = form_get(c->body, "name", name, len);
    if (l < 1) {
        api_fail(c, 400, "Tên profile phải dài 1-%u byte", (unsigned)(len - 1));
        return false;
    }
    return true;
}

static bool get_carrier(api_ctx_t *c, uint16_t *carrier)
{
    long v = 0;
    if (!form_get_int(c->body, "carrier", 0, 60000, &v) || (v != 0 && v < 30000)) {
        api_fail(c, 400, "Tần số sóng mang phải là 0 (mặc định) hoặc 30000-60000 Hz");
        return false;
    }
    *carrier = (uint16_t)v;
    return true;
}

static void profile_changed_if_active(long slot)
{
    if (slot == ac_profiles_active_slot()) {
        climate_control_profile_changed();
    }
}

static void post_profile_create(api_ctx_t *c)
{
    long slot, step;
    char name[32];
    uint16_t carrier;
    if (!get_slot(c, c->body, &slot) || !get_name(c, name, sizeof(name)) || !get_carrier(c, &carrier)) {
        return;
    }
    if (!form_get_int(c->body, "step", 1, 2, &step)) {
        api_fail(c, 400, "Bước nhiệt độ phải là 0,5 °C hoặc 1 °C");
        return;
    }
    esp_err_t err = ac_profiles_create((int)slot, name, (uint8_t)step, carrier);
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Ô profile này đã có dữ liệu");
        return;
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không tạo được profile: %s", esp_err_to_name(err));
        return;
    }
    if (ac_profiles_active_slot() < 0) {
        ac_profiles_select((int)slot, NULL);
        climate_control_profile_changed();
    }
}

static void post_profile_update(api_ctx_t *c)
{
    long slot, step, fan, swing;
    char name[32];
    uint16_t carrier;
    if (!get_slot(c, c->body, &slot) || !get_name(c, name, sizeof(name)) || !get_carrier(c, &carrier)) {
        return;
    }
    if (!form_get_int(c->body, "step", 1, 2, &step) || !form_get_int(c->body, "fan", 0, AC_FAN_COUNT - 1, &fan) ||
        !form_get_int(c->body, "swing", 0, AC_SWING_COUNT - 1, &swing)) {
        api_fail(c, 400, "Giá trị không hợp lệ");
        return;
    }
    esp_err_t err = ac_profiles_update_settings((int)slot, name, (uint8_t)step, (uint8_t)fan, (uint8_t)swing, carrier);
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Profile đã khóa: hãy mở khóa trước khi sửa");
    } else if (err == ESP_ERR_NOT_FOUND) {
        api_fail(c, 404, "Profile không tồn tại");
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không lưu được: %s", esp_err_to_name(err));
    } else {
        profile_changed_if_active(slot);
    }
}

static void post_profile_delete(api_ctx_t *c)
{
    long slot;
    if (!get_slot(c, c->body, &slot)) {
        return;
    }
    bool was_active = slot == ac_profiles_active_slot();
    esp_err_t err = ac_profiles_delete((int)slot);
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Profile đang khóa: hãy mở khóa trước khi xóa");
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không xóa được: %s", esp_err_to_name(err));
    } else if (was_active) {
        climate_control_profile_changed();
    }
}

static void post_profile_select(api_ctx_t *c)
{
    long slot;
    if (!get_slot(c, c->body, &slot)) {
        return;
    }
    esp_err_t err = ac_profiles_select((int)slot, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        api_fail(c, 404, "Profile không tồn tại");
        return;
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không chọn được: %s", esp_err_to_name(err));
        return;
    }
    climate_control_profile_changed();
    wj_bool(c->w, "restart_needed", restart_needed());
}

static void post_profile_lock(api_ctx_t *c)
{
    long slot;
    if (!get_slot(c, c->body, &slot)) {
        return;
    }
    bool locked = form_get_bool(c->body, "locked", true);
    esp_err_t err = ac_profiles_set_locked((int)slot, locked);
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Cần học lệnh Tắt và ít nhất một lệnh Làm lạnh trước khi khóa");
        return;
    } else if (err == ESP_ERR_NOT_FOUND) {
        api_fail(c, 404, "Profile không tồn tại");
        return;
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không lưu được: %s", esp_err_to_name(err));
        return;
    }
    profile_changed_if_active(slot);
    wj_bool(c->w, "restart_needed", restart_needed());
}

static void post_learn_start(api_ctx_t *c)
{
    esp_err_t err = ir_learn_start();
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Đang học một lệnh khác");
    } else if (err != ESP_OK) {
        api_fail(c, 503, "Không bật được bộ thu IR: %s", esp_err_to_name(err));
    }
}

static void post_learn_cancel(api_ctx_t *c)
{
    (void)c;
    ir_learn_cancel();
}

/** Copy of the captured code of learning session `session` (caller frees). */
static uint8_t *learned_blob(api_ctx_t *c, size_t *len)
{
    long session = 0;
    if (!form_get_int(c->body, "session", 0, 0x7FFFFFFF, &session)) {
        api_fail(c, 400, "Thiếu phiên học");
        return NULL;
    }
    ir_learn_status_t st;
    ir_learn_get_status(&st);
    if (st.state != IR_LEARN_DONE || st.session != (uint32_t)session) {
        api_fail(c, 409, "Không còn lệnh vừa học (đã học lệnh khác hoặc thất bại)");
        return NULL;
    }
    *len = st.result_size;
    uint8_t *blob = malloc(*len ? *len : 1);
    if (blob == NULL) {
        api_fail(c, 503, "Hết bộ nhớ");
        return NULL;
    }
    if (ir_learn_get_result(blob, len) != ESP_OK) {
        free(blob);
        api_fail(c, 409, "Không đọc được lệnh vừa học");
        return NULL;
    }
    return blob;
}

static void post_learn_test(api_ctx_t *c)
{
    size_t len = 0;
    uint8_t *blob = learned_blob(c, &len);
    if (blob == NULL) {
        return;
    }
    esp_err_t err = ir_send(blob, len, IR_SRC_TEST, true, 0, "Phát thử lệnh vừa học");
    free(blob);
    if (err != ESP_OK) {
        api_fail(c, 503, "Không phát được: %s", esp_err_to_name(err));
    }
}

static bool state_from_form(api_ctx_t *c, ac_state_t *s)
{
    long power, mode = 0, temp = 0, fan = 0, swing = 0;
    memset(s, 0, sizeof(*s));
    if (!form_get_int(c->body, "power", 0, 1, &power)) {
        api_fail(c, 400, "Thiếu trạng thái nguồn");
        return false;
    }
    if (power == 1) {
        if (!form_get_int(c->body, "mode", 0, AC_MODE_COUNT - 1, &mode) ||
            !form_get_int(c->body, "fan", 0, AC_FAN_COUNT - 1, &fan) ||
            !form_get_int(c->body, "swing", 0, AC_SWING_COUNT - 1, &swing)) {
            api_fail(c, 400, "Chế độ/quạt/đảo gió không hợp lệ");
            return false;
        }
        if (mode != AC_MODE_FAN && !form_get_int(c->body, "temp_half", AC_TEMP_HALF_MIN, AC_TEMP_HALF_MAX, &temp)) {
            api_fail(c, 400, "Nhiệt độ phải trong khoảng 10-35 °C");
            return false;
        }
        s->power = 1;
        s->mode = (uint8_t)mode;
        s->temp_half = mode == AC_MODE_FAN ? AC_TEMP_NONE : (uint8_t)temp;
        s->fan = (uint8_t)fan;
        s->swing = (uint8_t)swing;
    }
    if (!ac_state_valid(s)) {
        api_fail(c, 400, "Trạng thái máy lạnh không hợp lệ");
        return false;
    }
    return true;
}

static void post_learn_save(api_ctx_t *c)
{
    long slot;
    ac_state_t st;
    if (!get_slot(c, c->body, &slot) || !state_from_form(c, &st)) {
        return;
    }
    size_t len = 0;
    uint8_t *blob = learned_blob(c, &len);
    if (blob == NULL) {
        return;
    }
    esp_err_t err = ac_profiles_store_code((int)slot, &st, blob, len);
    free(blob);
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Profile đã khóa: hãy mở khóa trước khi học thêm");
    } else if (err == ESP_ERR_NOT_FOUND) {
        api_fail(c, 404, "Profile không tồn tại");
    } else if (err == ESP_ERR_NO_MEM) {
        api_fail(c, 409, "Profile đã đầy (tối đa %d lệnh)", AC_PROFILE_MAX_CODES);
    } else if (err == ESP_ERR_INVALID_ARG) {
        api_fail(c, 400, "Lệnh IR không hợp lệ");
    } else if (err != ESP_OK) {
        api_fail(c, 500, "Không lưu được (bộ nhớ IR đầy?): %s", esp_err_to_name(err));
    } else {
        profile_changed_if_active(slot);
        put_state(c->w, "state", &st);
    }
}

static bool code_from_form(api_ctx_t *c, long *slot, ac_state_t *st)
{
    char key[16];
    if (!get_slot(c, c->body, slot)) {
        return false;
    }
    if (form_get(c->body, "key", key, sizeof(key)) <= 0 || !ac_state_from_key(key, st)) {
        api_fail(c, 400, "Mã lệnh không hợp lệ");
        return false;
    }
    return true;
}

static void post_code_test(api_ctx_t *c)
{
    long slot;
    ac_state_t st;
    if (!code_from_form(c, &slot, &st)) {
        return;
    }
    size_t len = 0;
    esp_err_t err = ac_profiles_load_code((int)slot, &st, NULL, &len);
    uint8_t *blob = NULL;
    if (err == ESP_ERR_INVALID_SIZE && len > 0) {
        blob = malloc(len);
        err = blob ? ac_profiles_load_code((int)slot, &st, blob, &len) : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        free(blob);
        api_fail(c, 404, "Không đọc được lệnh đã học: %s", esp_err_to_name(err));
        return;
    }
    char label[64];
    memcpy(label, "Thử: ", sizeof("Thử: ")); /* 7 bytes + NUL */
    ac_state_describe(&st, label + 7, sizeof(label) - 7);
    utf8_trim_tail(label);
    err = ir_send(blob, len, IR_SRC_TEST, true, 0, label);
    free(blob);
    if (err != ESP_OK) {
        api_fail(c, 503, "Không phát được: %s", esp_err_to_name(err));
    }
}

static void post_code_delete(api_ctx_t *c)
{
    long slot;
    ac_state_t st;
    if (!code_from_form(c, &slot, &st)) {
        return;
    }
    esp_err_t err = ac_profiles_delete_code((int)slot, &st);
    if (err == ESP_ERR_INVALID_STATE) {
        api_fail(c, 409, "Profile đã khóa: hãy mở khóa trước khi xóa lệnh");
    } else if (err != ESP_OK) {
        api_fail(c, 404, "Không xóa được: %s", esp_err_to_name(err));
    } else {
        profile_changed_if_active(slot);
    }
}

static void post_ac_control(api_ctx_t *c)
{
    long mode, sp, fan, swing;
    if (!form_get_int(c->body, "mode", 0, 8, &mode) ||
        (mode != TSTAT_MODE_OFF && mode != TSTAT_MODE_COOL && mode != TSTAT_MODE_HEAT && mode != TSTAT_MODE_DRY &&
         mode != TSTAT_MODE_FAN_ONLY)) {
        api_fail(c, 400, "Chế độ không hợp lệ");
        return;
    }
    bool has_fan = form_get_int(c->body, "fan", 0, AC_FAN_COUNT - 1, &fan);
    bool has_swing = form_get_int(c->body, "swing", 0, AC_SWING_COUNT - 1, &swing);
    if (has_fan != has_swing) {
        api_fail(c, 400, "Cần cả quạt và đảo gió");
        return;
    }
    thermostat_user_state_t st;
    matter_thermostat_get_user_state(&st);
    st.system_mode = (uint8_t)mode;
    uint32_t mask = TSTAT_CHANGED_MODE;
    if (form_get_int(c->body, "sp", 500, 4000, &sp)) {
        if (mode == TSTAT_MODE_HEAT) {
            st.heat_setpoint = (int16_t)sp;
            mask |= TSTAT_CHANGED_HEAT_SP;
        } else if (mode == TSTAT_MODE_COOL || mode == TSTAT_MODE_DRY) {
            st.cool_setpoint = (int16_t)sp;
            mask |= TSTAT_CHANGED_COOL_SP;
        }
    }
    if (has_fan) {
        climate_control_set_fan_swing((uint8_t)fan, (uint8_t)swing);
    }
    /* Goes through the Thermostat cluster: the home app sees the same state. */
    esp_err_t err = matter_thermostat_set_user_state(&st, mask);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        api_fail(c, 409, "Chế độ này chưa có trong profile máy lạnh");
    } else if (err == ESP_ERR_INVALID_ARG) {
        api_fail(c, 400, "Nhiệt độ đặt ngoài giới hạn của máy lạnh");
    } else if (err != ESP_OK) {
        api_fail(c, 503, "Matter chưa sẵn sàng: %s", esp_err_to_name(err));
    }
}

static void post_ac_resend(api_ctx_t *c)
{
    if (climate_control_force_resend() != ESP_OK) {
        api_fail(c, 503, "Bộ điều khiển chưa chạy");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Algorithm configuration                                                                      */
/* ------------------------------------------------------------------------------------------ */

static void put_config(wj_t *w, const app_config_t *cfg)
{
    wj_obj_begin(w, "config");
    wj_uint(w, "control_mode", cfg->control_mode);
    wj_bool(w, "allow_local_fallback", cfg->allow_local_fallback);
    wj_uint(w, "deadband_centi", cfg->deadband_centi);
    wj_uint(w, "max_offset_centi", cfg->max_offset_centi);
    wj_uint(w, "comp_interval_s", cfg->comp_interval_s);
    wj_uint(w, "filter_tau_s", cfg->filter_tau_s);
    wj_uint(w, "user_debounce_ms", cfg->user_debounce_ms);
    wj_bool(w, "resend_on_boot", cfg->resend_on_boot);
    wj_uint(w, "stale_min_s", cfg->stale_min_s);
    wj_obj_end(w);
}

static void get_config(api_ctx_t *c)
{
    app_config_t cfg;
    app_config_load(&cfg);
    put_config(c->w, &cfg);
    rt_status_t rs;
    remote_temperature_get_status(&rs);
    wj_bool(c->w, "local_configured", rs.local_configured);
    put_climate(c->w);
}

static void post_config(api_ctx_t *c)
{
    app_config_t cfg;
    app_config_load(&cfg);
    long v;
    if (form_get_int(c->body, "control_mode", 0, 1, &v)) {
        cfg.control_mode = (uint8_t)v;
    }
    if (form_get_int(c->body, "deadband_centi", 0, 65535, &v)) {
        cfg.deadband_centi = (uint16_t)v;
    }
    if (form_get_int(c->body, "max_offset_centi", 0, 65535, &v)) {
        cfg.max_offset_centi = (uint16_t)v;
    }
    if (form_get_int(c->body, "comp_interval_s", 0, 65535, &v)) {
        cfg.comp_interval_s = (uint16_t)v;
    }
    if (form_get_int(c->body, "filter_tau_s", 0, 65535, &v)) {
        cfg.filter_tau_s = (uint16_t)v;
    }
    if (form_get_int(c->body, "user_debounce_ms", 0, 65535, &v)) {
        cfg.user_debounce_ms = (uint16_t)v;
    }
    if (form_get_int(c->body, "stale_min_s", 0, 0x7FFFFFFF, &v)) {
        cfg.stale_min_s = (uint32_t)v;
    }
    cfg.allow_local_fallback = form_get_bool(c->body, "allow_local_fallback", cfg.allow_local_fallback) ? 1 : 0;
    cfg.resend_on_boot = form_get_bool(c->body, "resend_on_boot", cfg.resend_on_boot) ? 1 : 0;
    const char *why = NULL;
    if (!app_config_validate(&cfg, &why)) {
        api_fail(c, 400, "%s", why ? why : "Giá trị không hợp lệ");
        return;
    }
    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        api_fail(c, 500, "Không lưu được cấu hình: %s", esp_err_to_name(err));
        return;
    }
    climate_control_config_changed(&cfg);
    remote_temperature_config_changed(&cfg);
    put_config(c->w, &cfg);
}

/* ------------------------------------------------------------------------------------------ */
/* Diagnostics                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static const char *reset_reason_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return "Cấp nguồn";
    case ESP_RST_SW:
        return "Khởi động lại bằng phần mềm";
    case ESP_RST_PANIC:
        return "Lỗi nghiêm trọng (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "Watchdog";
    case ESP_RST_BROWNOUT:
        return "Sụt áp nguồn (brownout)";
    case ESP_RST_DEEPSLEEP:
        return "Thức dậy từ deep sleep";
    case ESP_RST_EXT:
        return "Chân reset ngoài";
    default:
        return "Khác";
    }
}

static void put_heap(wj_t *w, const char *key, uint32_t caps)
{
    wj_obj_begin(w, key);
    wj_uint(w, "total", heap_caps_get_total_size(caps));
    wj_uint(w, "free", heap_caps_get_free_size(caps));
    wj_uint(w, "min_free", heap_caps_get_minimum_free_size(caps));
    wj_uint(w, "largest", heap_caps_get_largest_free_block(caps));
    wj_obj_end(w);
}

static void put_nvs(wj_t *w, const char *key, const char *part)
{
    size_t used = 0, freee = 0, total = 0;
    wj_obj_begin(w, key);
    if (cfg_get_stats(part, &used, &freee, &total) == ESP_OK) {
        wj_uint(w, "used", used);
        wj_uint(w, "free", freee);
        wj_uint(w, "total", total);
    }
    wj_obj_end(w);
}

static void get_diag(api_ctx_t *c)
{
    wj_t *w = c->w;
    const esp_app_desc_t *app = esp_app_get_description();
    wj_obj_begin(w, "firmware");
    wj_str(w, "project", app->project_name);
    wj_str(w, "version", app->version);
    wj_str(w, "idf", app->idf_ver);
    wj_str(w, "esp_matter", APP_ESP_MATTER_VERSION);
    wj_str(w, "date", app->date);
    wj_str(w, "time", app->time);
    wj_bool(w, "test_mode", TEST_MODE);
    wj_obj_end(w);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    wj_obj_begin(w, "chip");
    wj_uint(w, "model", chip.model);
    wj_uint(w, "revision", chip.revision);
    wj_uint(w, "cores", chip.cores);
    wj_uint(w, "flash", flash);
    wj_uint(w, "psram", esp_psram_get_size());
    wj_str(w, "mac", macs);
    wj_obj_end(w);

    wj_uint(w, "uptime_s", app_time_s());
    wj_str(w, "reset_reason", reset_reason_text(esp_reset_reason()));
    put_heap(w, "heap_internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    put_heap(w, "heap_psram", MALLOC_CAP_SPIRAM);
    put_nvs(w, "nvs_main", CFG_PART_MAIN);
    put_nvs(w, "nvs_ir", CFG_PART_IR);

    commissioner_info_t info;
    matter_commissioner_get_info(&info);
    wj_obj_begin(w, "fabric_b");
    wj_bool(w, "ready", info.ready);
    wj_str(w, "init_error", info.init_error);
    wj_uint(w, "fabric_index", info.fabric_index);
    wj_hex64(w, "fabric_id", info.fabric_id);
    wj_hex64(w, "compressed_fabric_id", info.compressed_fabric_id);
    wj_hex64(w, "controller_node_id", info.controller_node_id);
    wj_uint(w, "paa_count", info.paa_count);
    wj_uint(w, "listen_port", info.listen_port);
    wj_obj_end(w);
    put_matter(w);
    put_ir(w);

    app_error_t errs[APP_ERROR_LOG_LEN];
    size_t n = app_get_errors(errs, APP_ERROR_LOG_LEN);
    wj_uint(w, "error_count", app_get_error_count());
    wj_arr_begin(w, "errors");
    int64_t now = app_time_us();
    for (size_t i = 0; i < n; i++) {
        wj_obj_begin(w, NULL);
        wj_int(w, "age_s", (now - errs[i].time_us) / 1000000LL);
        wj_str(w, "module", errs[i].module);
        wj_str(w, "msg", errs[i].msg);
        wj_obj_end(w);
    }
    wj_arr_end(w);
}

static void post_system_reboot(api_ctx_t *c)
{
    if (app_state_get_busy() & (APP_BUSY_PAIRING | APP_BUSY_IR_LEARNING)) {
        api_fail(c, 409, "Đang ghép cảm biến hoặc học IR: hãy chờ xong");
        return;
    }
    if (web_defer(WEB_DEFER_REBOOT, 0, 1500) != ESP_OK) {
        api_fail(c, 503, "Không thể khởi động lại lúc này");
        return;
    }
    wj_str(c->w, "message", "Thiết bị đang khởi động lại...");
}

/* ------------------------------------------------------------------------------------------ */

const api_route_t g_api_routes[] = {
    { "session", get_session, NULL },
    { "status", get_status, NULL },
    { "portal/close", NULL, post_portal_close },
    { "portal/extend", NULL, post_portal_extend },
    { "wifi", get_wifi, NULL },
    { "wifi/scan", NULL, post_wifi_scan },
    { "wifi/sta", NULL, post_wifi_sta },
    { "wifi/ap_password", NULL, post_wifi_ap_password },
    { "matter", get_matter, NULL },
    { "matter/onboarding", get_matter_onboarding, NULL },
    { "matter/window", NULL, post_matter_window },
    { "sensor", get_sensor, NULL },
    { "sensor/pair", NULL, post_sensor_pair },
    { "sensor/cancel", NULL, post_sensor_cancel },
    { "sensor/bind", NULL, post_sensor_bind },
    { "sensor/read", NULL, post_sensor_read },
    { "sensor/remove", NULL, post_sensor_remove },
    { "sensor/forget", NULL, post_sensor_forget },
    { "sensor/retry", NULL, post_sensor_retry },
    { "sensor/simulate", NULL, post_sensor_simulate },
    { "ac", get_ac, NULL },
    { "ac/codes", get_ac_codes, NULL },
    { "ac/profile/create", NULL, post_profile_create },
    { "ac/profile/update", NULL, post_profile_update },
    { "ac/profile/delete", NULL, post_profile_delete },
    { "ac/profile/select", NULL, post_profile_select },
    { "ac/profile/lock", NULL, post_profile_lock },
    { "ac/learn/start", NULL, post_learn_start },
    { "ac/learn/cancel", NULL, post_learn_cancel },
    { "ac/learn/test", NULL, post_learn_test },
    { "ac/learn/save", NULL, post_learn_save },
    { "ac/code/test", NULL, post_code_test },
    { "ac/code/delete", NULL, post_code_delete },
    { "ac/control", NULL, post_ac_control },
    { "ac/resend", NULL, post_ac_resend },
    { "config", get_config, post_config },
    { "diag", get_diag, NULL },
    { "system/reboot", NULL, post_system_reboot },
};

const size_t g_api_route_count = sizeof(g_api_routes) / sizeof(g_api_routes[0]);
