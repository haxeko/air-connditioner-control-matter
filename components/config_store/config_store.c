#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config_store";

static esp_err_t init_partition(const char *part)
{
    esp_err_t err = (strcmp(part, CFG_PART_MAIN) == 0) ? nvs_flash_init() : nvs_flash_init_partition(part);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Only happens after a partition-layout change or an IDF NVS format upgrade. Data in this
         * partition cannot be read anyway, so erasing is the only way forward. */
        ESP_LOGW(TAG, "NVS partition '%s' incompatible (%s), erasing", part, esp_err_to_name(err));
        err = (strcmp(part, CFG_PART_MAIN) == 0) ? nvs_flash_erase() : nvs_flash_erase_partition(part);
        if (err == ESP_OK) {
            err = (strcmp(part, CFG_PART_MAIN) == 0) ? nvs_flash_init() : nvs_flash_init_partition(part);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s", part, esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_init(void)
{
    esp_err_t err = init_partition(CFG_PART_MAIN);
    if (err != ESP_OK) {
        return err;
    }
    return init_partition(CFG_PART_IR);
}

static esp_err_t open_ns(const char *part, const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open_from_partition(part, ns, mode, out);
}

esp_err_t cfg_get_blob(const char *part, const char *ns, const char *key, void *buf, size_t *inout_len)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(h, key, buf, inout_len);
    nvs_close(h);
    return err;
}

esp_err_t cfg_get_blob_size(const char *part, const char *ns, const char *key, size_t *out_len)
{
    *out_len = 0;
    return cfg_get_blob(part, ns, key, NULL, out_len);
}

esp_err_t cfg_set_blob(const char *part, const char *ns, const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write %s/%s/%s (%u B) failed: %s", part, ns, key, (unsigned)len, esp_err_to_name(err));
    }
    return err;
}

esp_err_t cfg_get_u32(const char *part, const char *ns, const char *key, uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t cfg_set_u32(const char *part, const char *ns, const char *key, uint32_t value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cfg_get_u64(const char *part, const char *ns, const char *key, uint64_t *out)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u64(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t cfg_set_u64(const char *part, const char *ns, const char *key, uint64_t value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u64(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cfg_get_str(const char *part, const char *ns, const char *key, char *buf, size_t buf_len)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = buf_len;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err;
}

esp_err_t cfg_set_str(const char *part, const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cfg_erase_key(const char *part, const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cfg_erase_namespace(const char *part, const char *ns)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(part, ns, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cfg_get_stats(const char *part, size_t *used_entries, size_t *free_entries, size_t *total_entries)
{
    nvs_stats_t st;
    esp_err_t err = nvs_get_stats(part, &st);
    if (err != ESP_OK) {
        return err;
    }
    *used_entries = st.used_entries;
    *free_entries = st.free_entries;
    *total_entries = st.total_entries;
    return ESP_OK;
}

esp_err_t app_config_load(app_config_t *cfg)
{
    app_config_defaults(cfg);
    app_config_t stored;
    size_t len = sizeof(stored);
    esp_err_t err = cfg_get_blob(CFG_PART_MAIN, CFG_NS_APP, "cfg", &stored, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK || len != sizeof(stored) || stored.version != APP_CONFIG_VERSION) {
        ESP_LOGW(TAG, "stored app config unusable (%s, len %u), using defaults", esp_err_to_name(err), (unsigned)len);
        return ESP_OK;
    }
    const char *why = NULL;
    if (!app_config_validate(&stored, &why)) {
        ESP_LOGW(TAG, "stored app config invalid (%s), using defaults", why);
        return ESP_OK;
    }
    *cfg = stored;
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    const char *why = NULL;
    if (!app_config_validate(cfg, &why)) {
        ESP_LOGE(TAG, "refusing to save invalid config: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    return cfg_set_blob(CFG_PART_MAIN, CFG_NS_APP, "cfg", cfg, sizeof(*cfg));
}
