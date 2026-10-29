#pragma once

/*
 * Small shared state that does not belong to a single module:
 *   - ring buffer of the most recent errors (shown on the diagnostics page),
 *   - "busy" flags that keep the configuration portal alive (pairing, IR learning),
 *   - monotonic time helpers.
 *
 * Never put secrets (passwords, setup codes, keys) into the error log.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_ERROR_LOG_LEN 16
#define APP_ERROR_MSG_LEN 96

typedef struct {
    int64_t time_us;           /* esp_timer time when the error was recorded */
    char module[16];
    char msg[APP_ERROR_MSG_LEN];
} app_error_t;

typedef enum {
    APP_BUSY_PAIRING = 1u << 0,      /* sensor commissioning in progress  */
    APP_BUSY_IR_LEARNING = 1u << 1,  /* waiting for / capturing a remote  */
    APP_BUSY_SENSOR_READ = 1u << 2,  /* endpoint discovery / test read    */
    APP_BUSY_WIFI_CONNECT = 1u << 3, /* testing new station credentials   */
} app_busy_t;

void app_state_init(void);

/** Record an error (also printed with ESP_LOGE). Thread-safe, not ISR-safe. */
void app_log_error(const char *module, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Copy up to max errors, newest first. Returns the number copied. */
size_t app_get_errors(app_error_t *out, size_t max);
uint32_t app_get_error_count(void);

void app_state_set_busy(uint32_t flags, bool on);
uint32_t app_state_get_busy(void);

/** Microseconds since boot (esp_timer). */
int64_t app_time_us(void);
static inline uint32_t app_time_s(void) { return (uint32_t)(app_time_us() / 1000000LL); }

#ifdef __cplusplus
}
#endif
