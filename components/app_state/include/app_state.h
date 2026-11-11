#pragma once

/*
 * Small shared state that does not belong to a single module:
 *   - ring buffer of the most recent errors (shown on the diagnostics page),
 *   - "busy" flags that keep the configuration portal alive (pairing, IR learning),
 *   - the crash report of the last panic / watchdog reset and the stack margin of the tasks,
 *   - monotonic time helpers.
 *
 * Never put secrets (passwords, setup codes, keys) into the error log.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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
    APP_BUSY_OTA = 1u << 4,          /* a firmware file is being written  */
} app_busy_t;

void app_state_init(void);

/** Record an error (also printed with ESP_LOGE). Thread-safe, not ISR-safe. */
void app_log_error(const char *module, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Copy up to max errors, newest first. Returns the number copied. */
size_t app_get_errors(app_error_t *out, size_t max);
uint32_t app_get_error_count(void);

void app_state_set_busy(uint32_t flags, bool on);
uint32_t app_state_get_busy(void);

/**
 * Factory reset, implemented by the application (main/app_main.cpp): leaves the home ecosystems,
 * stores the request (config_store_request_factory_reset) and restarts after about 2.5 s; everything
 * is erased at the next start. Does not return on success. Not from the CHIP task.
 */
void app_factory_reset(void);

/*
 * Crash report: when the firmware panics (exception, abort, task or interrupt watchdog), the panic
 * handler stores a core dump in the "coredump" flash partition before the restart. Only its summary
 * is exposed: which task, where (program counter and return addresses) and why. tools/decode_backtrace.py
 * turns the addresses into function names with the symbol file published next to each firmware.
 * No memory contents are shown anywhere. The report stays until it is cleared or replaced.
 */
#define APP_CRASH_BT_MAX 16

typedef struct {
    bool present;
    bool same_firmware;        /* stored by the firmware that is running now */
    bool bt_corrupted;
    uint8_t depth;             /* valid entries of bt[] */
    char task[16];
    uint32_t pc;
    uint32_t cause;            /* Xtensa EXCCAUSE (meaningless for abort / watchdog) */
    uint32_t vaddr;            /* address that was accessed */
    uint32_t bt[APP_CRASH_BT_MAX];
    char elf_sha[17];          /* start of the ELF SHA-256 of the firmware that crashed */
} app_crash_info_t;

/** Read the stored report once. Call early in app_main(). */
void app_crash_init(void);
bool app_crash_get(app_crash_info_t *out);
esp_err_t app_crash_clear(void);
/** Name of an exception cause, "" when unknown. */
const char *app_crash_cause_text(uint32_t cause);

typedef struct {
    char name[16];
    uint32_t stack_free;       /* smallest free stack since the task started (bytes) */
} app_task_info_t;

/** Tasks sorted by stack margin, smallest first. Returns the number written (0 without trace facility). */
size_t app_tasks_get(app_task_info_t *out, size_t max);

/** Microseconds since boot (esp_timer). */
int64_t app_time_us(void);
static inline uint32_t app_time_s(void) { return (uint32_t)(app_time_us() / 1000000LL); }

#ifdef __cplusplus
}
#endif
