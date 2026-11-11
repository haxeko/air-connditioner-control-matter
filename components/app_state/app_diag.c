/*
 * Diagnostics shared by the web page and the console: the crash report the panic handler stored in
 * the "coredump" flash partition, and the stack margin of every task.
 */
#include "app_state.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#define HAVE_COREDUMP 1
#else
#define HAVE_COREDUMP 0
#endif

#if HAVE_COREDUMP
static const char *TAG = "app_diag";
#endif

static app_crash_info_t s_crash; /* written once by app_crash_init(), before any reader exists */

void app_crash_init(void)
{
    memset(&s_crash, 0, sizeof(s_crash));
#if HAVE_COREDUMP
    /* Boards flashed with an older full image have no "coredump" partition: nothing is stored there
     * and both calls simply fail. */
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (sum == NULL) {
        return;
    }
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        s_crash.present = true;
        strlcpy(s_crash.task, sum->exc_task, sizeof(s_crash.task));
        s_crash.pc = sum->exc_pc;
        s_crash.cause = sum->ex_info.exc_cause;
        s_crash.vaddr = sum->ex_info.exc_vaddr;
        s_crash.bt_corrupted = sum->exc_bt_info.corrupted;
        uint32_t depth = sum->exc_bt_info.depth;
        if (depth > APP_CRASH_BT_MAX) {
            depth = APP_CRASH_BT_MAX;
        }
        s_crash.depth = (uint8_t)depth;
        memcpy(s_crash.bt, sum->exc_bt_info.bt, depth * sizeof(uint32_t));
        strlcpy(s_crash.elf_sha, (const char *)sum->app_elf_sha256, sizeof(s_crash.elf_sha));
        char running[sizeof(s_crash.elf_sha)];
        esp_app_get_elf_sha256(running, sizeof(running));
        s_crash.same_firmware = s_crash.elf_sha[0] != 0 && strncmp(running, s_crash.elf_sha, strlen(s_crash.elf_sha)) == 0;
        ESP_LOGW(TAG, "crash report stored: task '%s', pc 0x%08x, %u backtrace entries (%s firmware)", s_crash.task,
                 (unsigned)s_crash.pc, (unsigned)s_crash.depth, s_crash.same_firmware ? "this" : "another");
    }
    free(sum);
#endif
}

bool app_crash_get(app_crash_info_t *out)
{
    *out = s_crash;
    return s_crash.present;
}

esp_err_t app_crash_clear(void)
{
#if HAVE_COREDUMP
    esp_err_t err = s_crash.present ? esp_core_dump_image_erase() : ESP_OK;
    if (err == ESP_OK) {
        s_crash.present = false;
    }
    return err;
#else
    return ESP_OK;
#endif
}

const char *app_crash_cause_text(uint32_t cause)
{
    switch (cause) { /* Xtensa EXCCAUSE */
    case 0:
        return "IllegalInstruction";
    case 2:
        return "InstructionFetchError";
    case 3:
        return "LoadStoreError";
    case 6:
        return "IntegerDivideByZero";
    case 9:
        return "LoadStoreAlignment";
    case 20:
        return "InstFetchProhibited";
    case 28:
        return "LoadProhibited";
    case 29:
        return "StoreProhibited";
    default:
        return "";
    }
}

static int by_free(const void *a, const void *b)
{
    const app_task_info_t *x = a, *y = b;
    return (x->stack_free > y->stack_free) - (x->stack_free < y->stack_free);
}

size_t app_tasks_get(app_task_info_t *out, size_t max)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4; /* tasks created meanwhile */
    TaskStatus_t *st = malloc(n * sizeof(TaskStatus_t));
    if (st == NULL) {
        return 0;
    }
    n = uxTaskGetSystemState(st, n, NULL);
    size_t count = 0;
    for (UBaseType_t i = 0; i < n && count < max; i++) {
        strlcpy(out[count].name, st[i].pcTaskName, sizeof(out[count].name));
        out[count].stack_free = (uint32_t)st[i].usStackHighWaterMark; /* bytes on ESP-IDF */
        count++;
    }
    free(st);
    qsort(out, count, sizeof(out[0]), by_free);
    return count;
#else
    (void)out;
    (void)max;
    return 0;
#endif
}
