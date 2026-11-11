#include "config_store.h"

#include <stdlib.h>
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

/* ------------------------------------------------------------------------------------------ */
/* Factory reset                                                                                */
/* ------------------------------------------------------------------------------------------ */

#define RESET_NS "ac_sys" /* on CFG_PART_IR */
#define RESET_KEY "freset"
#define KEEP_MAX 8        /* entries of CFG_NS_IDENTITY carried over the erase */

typedef struct {
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    uint32_t u32;
    void *blob;
    size_t len;
} kept_entry_t;

static bool s_was_factory_reset;

/* Read every u32 / blob entry of a namespace of the main partition. Returns the number kept. */
static size_t keep_namespace(const char *ns, kept_entry_t *out, size_t max)
{
    size_t n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(CFG_PART_MAIN, ns, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && n < max) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        kept_entry_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        strlcpy(e->key, info.key, sizeof(e->key));
        e->type = info.type;
        if (info.type == NVS_TYPE_U32) {
            if (cfg_get_u32(CFG_PART_MAIN, ns, info.key, &e->u32) == ESP_OK) {
                n++;
            }
        } else if (info.type == NVS_TYPE_BLOB) {
            if (cfg_get_blob_size(CFG_PART_MAIN, ns, info.key, &e->len) == ESP_OK && e->len > 0 &&
                (e->blob = malloc(e->len)) != NULL) {
                if (cfg_get_blob(CFG_PART_MAIN, ns, info.key, e->blob, &e->len) == ESP_OK) {
                    n++;
                } else {
                    free(e->blob);
                }
            }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

static esp_err_t factory_reset_now(void)
{
    kept_entry_t *kept = calloc(KEEP_MAX, sizeof(kept_entry_t));
    if (kept == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = keep_namespace(CFG_NS_IDENTITY, kept, KEEP_MAX);
    esp_err_t err = nvs_flash_deinit();
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        err = nvs_flash_erase();
    }
    if (err == ESP_OK) {
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        /* "ver" marks the identity as complete (device_identity.cpp): write it last. */
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < n; i++) {
                const kept_entry_t *e = &kept[i];
                if ((strcmp(e->key, "ver") == 0) != (pass == 1)) {
                    continue;
                }
                esp_err_t w = e->type == NVS_TYPE_U32 ? cfg_set_u32(CFG_PART_MAIN, CFG_NS_IDENTITY, e->key, e->u32)
                                                      : cfg_set_blob(CFG_PART_MAIN, CFG_NS_IDENTITY, e->key, e->blob, e->len);
                if (w != ESP_OK) {
                    ESP_LOGE(TAG, "pairing code not kept (%s): a new one will be generated", esp_err_to_name(w));
                    cfg_erase_namespace(CFG_PART_MAIN, CFG_NS_IDENTITY);
                    pass = 2;
                    break;
                }
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (kept[i].blob != NULL) {
            memset(kept[i].blob, 0, kept[i].len); /* salt / verifier */
            free(kept[i].blob);
        }
    }
    memset(kept, 0, KEEP_MAX * sizeof(kept_entry_t));
    free(kept);
    return err;
}

esp_err_t config_store_request_factory_reset(void)
{
    return cfg_set_u32(CFG_PART_IR, RESET_NS, RESET_KEY, 1);
}

bool config_store_was_factory_reset(void)
{
    return s_was_factory_reset;
}

esp_err_t config_store_init(void)
{
    esp_err_t err = init_partition(CFG_PART_MAIN);
    if (err != ESP_OK) {
        return err;
    }
    err = init_partition(CFG_PART_IR);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t requested = 0;
    if (cfg_get_u32(CFG_PART_IR, RESET_NS, RESET_KEY, &requested) == ESP_OK && requested == 1) {
        ESP_LOGW(TAG, "factory reset requested: erasing Wi-Fi, Matter fabrics, sensor and settings");
        err = factory_reset_now();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
            return err; /* the request stays: tried again at the next start */
        }
        cfg_erase_key(CFG_PART_IR, RESET_NS, RESET_KEY);
        s_was_factory_reset = true;
        ESP_LOGW(TAG, "factory reset done (pairing code and IR profiles kept)");
    }
    return ESP_OK;
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
