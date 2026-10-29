#include "ac_profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ir_code.h"
#include "nvs.h"

static const char *TAG = "ac_profiles";

#define SEL_NS "ac_sel"
#define META_KEY "meta"

typedef struct {
    bool present;
    ac_profile_meta_t meta;
    ac_state_t *codes; /* learned states (PSRAM) */
    size_t count;
} slot_cache_t;

static slot_cache_t s_slots[AC_PROFILE_SLOTS];
static int s_active = -1;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void ns_name(int slot, char *out, size_t len)
{
    snprintf(out, len, "p%d", slot);
}

static bool slot_ok(int slot)
{
    return slot >= 0 && slot < AC_PROFILE_SLOTS;
}

/* ------------------------------------------------------------------------------------------ */
/* RAW driver                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static void raw_get_caps(int slot, ac_caps_t *caps)
{
    lock();
    const slot_cache_t *c = &s_slots[slot];
    if (!c->present) {
        memset(caps, 0, sizeof(*caps));
    } else {
        ac_caps_from_list(c->codes, c->count, c->meta.temp_step_half, caps);
    }
    unlock();
}

static esp_err_t raw_resolve(int slot, const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen,
                             ac_resolve_kind_t *kind)
{
    lock();
    const slot_cache_t *c = &s_slots[slot];
    ac_resolve_kind_t k = AC_RESOLVE_NONE;
    if (c->present) {
        k = ac_resolve(c->codes, c->count, c->meta.default_fan, c->meta.default_swing, desired, user_target_half, chosen);
    }
    unlock();
    if (kind) {
        *kind = k;
    }
    return k == AC_RESOLVE_NONE ? ESP_ERR_NOT_FOUND : ESP_OK;
}

static esp_err_t raw_encode(int slot, const ac_state_t *state, uint8_t **blob, size_t *len)
{
    size_t need = 0;
    esp_err_t err = ac_profiles_load_code(slot, state, NULL, &need);
    if (err != ESP_ERR_INVALID_SIZE) {
        return err == ESP_OK ? ESP_FAIL : err;
    }
    uint8_t *buf = malloc(need);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = ac_profiles_load_code(slot, state, buf, &need);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *blob = buf;
    *len = need;
    return ESP_OK;
}

static const ac_protocol_driver_t k_raw_driver = {
    .driver_id = AC_DRIVER_RAW,
    .name = "Raw (học từ remote)",
    .get_caps = raw_get_caps,
    .resolve = raw_resolve,
    .encode = raw_encode,
};

static const ac_protocol_driver_t *const k_registry[] = {
    &k_raw_driver,
    /* Native protocol drivers (generated frames for a known AC model) are added here. */
};

const ac_protocol_driver_t *ac_protocol_find(uint8_t driver_id)
{
    for (size_t i = 0; i < sizeof(k_registry) / sizeof(k_registry[0]); i++) {
        if (k_registry[i]->driver_id == driver_id) {
            return k_registry[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Storage                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static esp_err_t load_slot(int slot)
{
    slot_cache_t *c = &s_slots[slot];
    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    free(c->codes);
    memset(c, 0, sizeof(*c));

    ac_profile_meta_t meta;
    size_t len = sizeof(meta);
    esp_err_t err = cfg_get_blob(CFG_PART_IR, ns, META_KEY, &meta, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK || len != sizeof(meta) || meta.version != AC_PROFILE_VERSION || ac_protocol_find(meta.driver) == NULL) {
        app_log_error("ac_profiles", "profile slot %d unreadable (%s)", slot, esp_err_to_name(err));
        return ESP_OK;
    }
    c->codes = calloc(AC_PROFILE_MAX_CODES, sizeof(ac_state_t));
    if (c->codes == NULL) {
        return ESP_ERR_NO_MEM;
    }
    nvs_iterator_t it = NULL;
    err = nvs_entry_find(CFG_PART_IR, ns, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        ac_state_t st;
        if (strcmp(info.key, META_KEY) != 0 && ac_state_from_key(info.key, &st)) {
            if (c->count < AC_PROFILE_MAX_CODES) {
                c->codes[c->count++] = st;
            }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    c->meta = meta;
    c->present = true;
    ESP_LOGI(TAG, "slot %d '%s': %u code(s)%s", slot, meta.name, (unsigned)c->count, meta.locked ? ", locked" : "");
    return ESP_OK;
}

esp_err_t ac_profiles_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    for (int i = 0; i < AC_PROFILE_SLOTS; i++) {
        esp_err_t err = load_slot(i);
        if (err != ESP_OK) {
            return err;
        }
    }
    uint32_t sel = 0;
    if (cfg_get_u32(CFG_PART_IR, SEL_NS, "slot", &sel) == ESP_OK && slot_ok((int)sel) && s_slots[sel].present) {
        s_active = (int)sel;
    } else {
        s_active = -1;
        for (int i = 0; i < AC_PROFILE_SLOTS; i++) {
            if (s_slots[i].present) {
                s_active = i;
                break;
            }
        }
    }
    return ESP_OK;
}

int ac_profiles_active_slot(void)
{
    return s_active;
}

esp_err_t ac_profiles_get_meta(int slot, ac_profile_meta_t *out)
{
    if (!slot_ok(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = s_slots[slot].present ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        *out = s_slots[slot].meta;
    }
    unlock();
    return err;
}

static esp_err_t write_meta(int slot, const ac_profile_meta_t *meta)
{
    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    return cfg_set_blob(CFG_PART_IR, ns, META_KEY, meta, sizeof(*meta));
}

esp_err_t ac_profiles_create(int slot, const char *name, uint8_t temp_step_half, uint16_t carrier_hz)
{
    if (!slot_ok(slot) || name == NULL || name[0] == 0 || strlen(name) >= sizeof(((ac_profile_meta_t *)0)->name) ||
        (temp_step_half != 1 && temp_step_half != 2) || (carrier_hz != 0 && (carrier_hz < 30000 || carrier_hz > 60000))) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool exists = s_slots[slot].present;
    unlock();
    if (exists) {
        return ESP_ERR_INVALID_STATE;
    }
    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    esp_err_t err = cfg_erase_namespace(CFG_PART_IR, ns);
    if (err != ESP_OK) {
        return err;
    }
    ac_profile_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = AC_PROFILE_VERSION;
    meta.driver = AC_DRIVER_RAW;
    strlcpy(meta.name, name, sizeof(meta.name));
    meta.temp_step_half = temp_step_half;
    meta.default_fan = AC_FAN_AUTO;
    meta.default_swing = AC_SWING_OFF;
    meta.carrier_hz = carrier_hz;
    meta.created_s = app_time_s();
    err = write_meta(slot, &meta);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    err = load_slot(slot);
    if (s_active < 0) {
        s_active = slot;
    }
    unlock();
    if (err == ESP_OK && s_active == slot) {
        cfg_set_u32(CFG_PART_IR, SEL_NS, "slot", (uint32_t)slot);
    }
    return err;
}

esp_err_t ac_profiles_update_settings(int slot, const char *name, uint8_t temp_step_half, uint8_t default_fan,
                                      uint8_t default_swing, uint16_t carrier_hz)
{
    if (!slot_ok(slot) || (temp_step_half != 1 && temp_step_half != 2) || default_fan >= AC_FAN_COUNT ||
        default_swing >= AC_SWING_COUNT || (carrier_hz != 0 && (carrier_hz < 30000 || carrier_hz > 60000))) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (!s_slots[slot].present) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_slots[slot].meta.locked) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ac_profile_meta_t meta = s_slots[slot].meta;
    unlock();
    if (name != NULL && name[0] != 0) {
        if (strlen(name) >= sizeof(meta.name)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(meta.name, name, sizeof(meta.name));
    }
    meta.temp_step_half = temp_step_half;
    meta.default_fan = default_fan;
    meta.default_swing = default_swing;
    meta.carrier_hz = carrier_hz;
    esp_err_t err = write_meta(slot, &meta);
    if (err == ESP_OK) {
        lock();
        s_slots[slot].meta = meta;
        unlock();
    }
    return err;
}

esp_err_t ac_profiles_delete(int slot)
{
    if (!slot_ok(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool locked = s_slots[slot].present && s_slots[slot].meta.locked;
    unlock();
    if (locked) {
        return ESP_ERR_INVALID_STATE; /* unlock first: protects the working profile */
    }
    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    esp_err_t err = cfg_erase_namespace(CFG_PART_IR, ns);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    free(s_slots[slot].codes);
    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    if (s_active == slot) {
        s_active = -1;
    }
    unlock();
    return ESP_OK;
}

static bool heat_of(int slot)
{
    ac_caps_t caps;
    ac_profiles_get_caps(slot, &caps);
    return (caps.modes_mask & (1u << AC_MODE_HEAT)) != 0;
}

esp_err_t ac_profiles_select(int slot, bool *caps_changed)
{
    if (!slot_ok(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool present = s_slots[slot].present;
    int prev = s_active;
    unlock();
    if (!present) {
        return ESP_ERR_NOT_FOUND;
    }
    bool before = prev >= 0 ? heat_of(prev) : false;
    esp_err_t err = cfg_set_u32(CFG_PART_IR, SEL_NS, "slot", (uint32_t)slot);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    s_active = slot;
    unlock();
    if (caps_changed) {
        *caps_changed = before != heat_of(slot);
    }
    return ESP_OK;
}

esp_err_t ac_profiles_set_locked(int slot, bool locked)
{
    if (!slot_ok(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (!s_slots[slot].present) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    ac_profile_meta_t meta = s_slots[slot].meta;
    ac_caps_t caps;
    ac_caps_from_list(s_slots[slot].codes, s_slots[slot].count, meta.temp_step_half, &caps);
    unlock();
    if (locked) {
        /* A profile that cannot switch the AC off, or has no cooling state, is not usable. */
        if (!caps.has_off || (caps.modes_mask & (1u << AC_MODE_COOL)) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    meta.locked = locked ? 1 : 0;
    esp_err_t err = write_meta(slot, &meta);
    if (err == ESP_OK) {
        lock();
        s_slots[slot].meta = meta;
        unlock();
    }
    return err;
}

esp_err_t ac_profiles_store_code(int slot, const ac_state_t *state, const uint8_t *blob, size_t len)
{
    char key[16];
    if (!slot_ok(slot) || !ac_state_key(state, key, sizeof(key)) || ir_code_validate_blob(blob, len, NULL) != IR_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    slot_cache_t *c = &s_slots[slot];
    if (!c->present) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (c->meta.locked) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    bool known = false;
    for (size_t i = 0; i < c->count; i++) {
        if (ac_state_equal(&c->codes[i], state)) {
            known = true;
            break;
        }
    }
    if (!known && c->count >= AC_PROFILE_MAX_CODES) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    unlock();

    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    esp_err_t err = cfg_set_blob(CFG_PART_IR, ns, key, blob, len);
    if (err != ESP_OK) {
        return err;
    }
    if (!known) {
        lock();
        c->codes[c->count++] = *state;
        unlock();
    }
    return ESP_OK;
}

esp_err_t ac_profiles_delete_code(int slot, const ac_state_t *state)
{
    char key[16];
    if (!slot_ok(slot) || !ac_state_key(state, key, sizeof(key))) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    slot_cache_t *c = &s_slots[slot];
    if (!c->present) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (c->meta.locked) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    unlock();
    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    esp_err_t err = cfg_erase_key(CFG_PART_IR, ns, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    lock();
    for (size_t i = 0; i < c->count; i++) {
        if (ac_state_equal(&c->codes[i], state)) {
            c->codes[i] = c->codes[--c->count];
            break;
        }
    }
    unlock();
    return ESP_OK;
}

esp_err_t ac_profiles_load_code(int slot, const ac_state_t *state, uint8_t *buf, size_t *inout_len)
{
    char key[16];
    if (!slot_ok(slot) || !ac_state_key(state, key, sizeof(key))) {
        return ESP_ERR_INVALID_ARG;
    }
    char ns[8];
    ns_name(slot, ns, sizeof(ns));
    size_t need = 0;
    esp_err_t err = cfg_get_blob_size(CFG_PART_IR, ns, key, &need);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    if (buf == NULL || *inout_len < need) {
        *inout_len = need;
        return ESP_ERR_INVALID_SIZE;
    }
    *inout_len = need;
    err = cfg_get_blob(CFG_PART_IR, ns, key, buf, inout_len);
    if (err == ESP_OK && ir_code_validate_blob(buf, *inout_len, NULL) != IR_OK) {
        app_log_error("ac_profiles", "stored code %s is corrupt", key);
        return ESP_ERR_INVALID_CRC;
    }
    return err;
}

size_t ac_profiles_list_codes(int slot, ac_state_t *out, size_t max)
{
    if (!slot_ok(slot)) {
        return 0;
    }
    lock();
    size_t n = s_slots[slot].present ? s_slots[slot].count : 0;
    if (n > max) {
        n = max;
    }
    memcpy(out, s_slots[slot].codes, n * sizeof(ac_state_t));
    unlock();
    return n;
}

void ac_profiles_get_caps(int slot, ac_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    if (!slot_ok(slot)) {
        return;
    }
    lock();
    bool present = s_slots[slot].present;
    uint8_t driver = s_slots[slot].meta.driver;
    unlock();
    if (!present) {
        return;
    }
    const ac_protocol_driver_t *drv = ac_protocol_find(driver);
    if (drv) {
        drv->get_caps(slot, caps);
    }
}

bool ac_profiles_ready_for_control(const char **why)
{
    const char *dummy;
    if (why == NULL) {
        why = &dummy;
    }
    int slot = s_active;
    if (!slot_ok(slot)) {
        *why = "Chưa có profile máy lạnh";
        return false;
    }
    ac_profile_meta_t meta;
    if (ac_profiles_get_meta(slot, &meta) != ESP_OK) {
        *why = "Profile không đọc được";
        return false;
    }
    if (!meta.locked) {
        *why = "Profile chưa được khóa (xác nhận) sau khi học";
        return false;
    }
    ac_caps_t caps;
    ac_profiles_get_caps(slot, &caps);
    if (!caps.has_off) {
        *why = "Profile thiếu lệnh Tắt";
        return false;
    }
    *why = NULL;
    return true;
}

esp_err_t ac_profiles_build_command(const ac_state_t *desired, uint8_t user_target_half, ac_state_t *chosen,
                                    ac_resolve_kind_t *kind, uint8_t **blob, size_t *len)
{
    int slot = s_active;
    if (!slot_ok(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    ac_profile_meta_t meta;
    esp_err_t err = ac_profiles_get_meta(slot, &meta);
    if (err != ESP_OK) {
        return err;
    }
    const ac_protocol_driver_t *drv = ac_protocol_find(meta.driver);
    if (drv == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = drv->resolve(slot, desired, user_target_half, chosen, kind);
    if (err != ESP_OK) {
        return err;
    }
    return drv->encode(slot, chosen, blob, len);
}

static void learned_range(const ac_caps_t *caps, ac_mode_t mode, int16_t *min_c, int16_t *max_c)
{
    *min_c = (int16_t)(caps->temp_min_half[mode] * 50);
    *max_c = (int16_t)(caps->temp_max_half[mode] * 50);
    if (*max_c <= *min_c) {
        /* a single learned temperature: give the hub one step of range, the resolver keeps
         * transmitting the learned (nearest) code */
        *max_c = (int16_t)(*min_c + caps->temp_step_half * 50);
    }
}

void ac_profiles_thermostat_limits(int slot, ac_thermostat_limits_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cool_min = AC_DEFAULT_COOL_MIN_CENTI;
    out->cool_max = AC_DEFAULT_COOL_MAX_CENTI;
    out->heat_min = AC_DEFAULT_COOL_MIN_CENTI;
    out->heat_max = AC_DEFAULT_COOL_MAX_CENTI;
    ac_profile_meta_t meta;
    if (ac_profiles_get_meta(slot, &meta) != ESP_OK || !meta.locked) {
        return;
    }
    ac_caps_t caps;
    ac_profiles_get_caps(slot, &caps);
    if ((caps.modes_mask & (1u << AC_MODE_COOL)) == 0 || caps.temp_min_half[AC_MODE_COOL] == 0) {
        return; /* cannot happen for a locked profile, keep the defaults */
    }
    out->from_profile = true;
    learned_range(&caps, AC_MODE_COOL, &out->cool_min, &out->cool_max);
    if ((caps.modes_mask & (1u << AC_MODE_HEAT)) != 0 && caps.temp_min_half[AC_MODE_HEAT] != 0) {
        out->heat = true;
        learned_range(&caps, AC_MODE_HEAT, &out->heat_min, &out->heat_max);
    }
    out->dry = (caps.modes_mask & (1u << AC_MODE_DRY)) != 0;
    out->fan_only = (caps.modes_mask & (1u << AC_MODE_FAN)) != 0;
}
