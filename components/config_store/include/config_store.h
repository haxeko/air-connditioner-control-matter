#pragma once

/*
 * Persistent configuration on NVS.
 *
 * Two NVS partitions are used:
 *   - "nvs"     : shared with ESP-IDF (Wi-Fi) and Matter (fabrics, ACL, attributes). This module only
 *                 touches its own namespaces (prefix "ac_").
 *   - "irstore" : learned IR codes and AC profiles (large blobs), see ac_profiles.
 *
 * All functions are thread-safe (NVS handles are opened per call) and must not be called from an ISR.
 * They must not be called from the Matter (CHIP) task for large writes: flash writes stall the cache.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h" /* ESP_ERR_NVS_* codes returned by this API */

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_PART_MAIN "nvs"
#define CFG_PART_IR "irstore"

/* Namespaces owned by the application (max 15 chars). */
#define CFG_NS_APP "ac_app"       /* app_config_t, AP password                     */
#define CFG_NS_IDENTITY "ac_ident" /* setup passcode / discriminator / verifier      */
#define CFG_NS_FABRIC_B "ac_fabb"  /* own fabric: root key, RCAC, fabric id, IPK     */
#define CFG_NS_SENSOR "ac_sensor"  /* paired Matter sensor binding                  */
#define CFG_NS_WIFI "ac_wifi"      /* previous STA credentials (rollback)           */
#define CFG_NS_RUNTIME "ac_rt"     /* last IR state sent, compensation offset       */
#define CFG_NS_PROFILE "ac_prof"   /* on CFG_PART_IR: profiles and IR codes          */

/** Initialise both NVS partitions (erases a partition only if its layout is incompatible). */
esp_err_t config_store_init(void);

esp_err_t cfg_get_blob(const char *part, const char *ns, const char *key, void *buf, size_t *inout_len);
esp_err_t cfg_set_blob(const char *part, const char *ns, const char *key, const void *buf, size_t len);
esp_err_t cfg_get_blob_size(const char *part, const char *ns, const char *key, size_t *out_len);
esp_err_t cfg_get_u32(const char *part, const char *ns, const char *key, uint32_t *out);
esp_err_t cfg_set_u32(const char *part, const char *ns, const char *key, uint32_t value);
esp_err_t cfg_get_u64(const char *part, const char *ns, const char *key, uint64_t *out);
esp_err_t cfg_set_u64(const char *part, const char *ns, const char *key, uint64_t value);
esp_err_t cfg_get_str(const char *part, const char *ns, const char *key, char *buf, size_t buf_len);
esp_err_t cfg_set_str(const char *part, const char *ns, const char *key, const char *value);
esp_err_t cfg_erase_key(const char *part, const char *ns, const char *key);
esp_err_t cfg_erase_namespace(const char *part, const char *ns);

/** Free/used entry statistics of a partition (for the diagnostics page). */
esp_err_t cfg_get_stats(const char *part, size_t *used_entries, size_t *free_entries, size_t *total_entries);

/* ---------------------------------------------------------------------------------------------
 * Application configuration (single versioned blob)
 * ------------------------------------------------------------------------------------------- */

typedef enum {
    CONTROL_MODE_DIRECT = 0,   /* user setpoint -> IR setpoint (rounded to AC step)          */
    CONTROL_MODE_EXTERNAL = 1, /* external sensor adjusts the IR setpoint within limits       */
} control_mode_t;

#define APP_CONFIG_VERSION 1

typedef struct {
    uint16_t version;
    uint8_t control_mode;          /* control_mode_t                                         */
    uint8_t allow_local_fallback;  /* use the wired local sensor when the Matter sensor fails */
    uint16_t deadband_centi;       /* no correction while |room-target| <= deadband (0.01 C)  */
    uint16_t max_offset_centi;     /* |IR setpoint - target| limit in external mode (0.01 C)  */
    uint16_t comp_interval_s;      /* min seconds between two automatic corrections          */
    uint16_t filter_tau_s;         /* time constant of the room temperature low-pass filter   */
    uint16_t user_debounce_ms;     /* coalesce bursts of setpoint writes from the hub         */
    uint8_t resend_on_boot;        /* re-send the last full state after power-up              */
    uint8_t reserved0;
    uint32_t stale_min_s;          /* lower bound of the stale timeout                       */
    uint32_t reserved[4];
} app_config_t;

void app_config_defaults(app_config_t *cfg);
/** Returns false (and fills *why) if a field is out of range. */
bool app_config_validate(const app_config_t *cfg, const char **why);
esp_err_t app_config_load(app_config_t *cfg);
esp_err_t app_config_save(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
