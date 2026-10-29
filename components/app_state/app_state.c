#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static app_error_t s_errors[APP_ERROR_LOG_LEN];
static size_t s_next;
static uint32_t s_total;
static volatile uint32_t s_busy;

void app_state_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

int64_t app_time_us(void)
{
    return esp_timer_get_time();
}

void app_log_error(const char *module, const char *fmt, ...)
{
    app_error_t e;
    e.time_us = esp_timer_get_time();
    strlcpy(e.module, module ? module : "?", sizeof(e.module));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);
    ESP_LOGE(e.module, "%s", e.msg);

    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_errors[s_next] = e;
    s_next = (s_next + 1) % APP_ERROR_LOG_LEN;
    s_total++;
    xSemaphoreGive(s_lock);
}

size_t app_get_errors(app_error_t *out, size_t max)
{
    if (s_lock == NULL || max == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t available = s_total < APP_ERROR_LOG_LEN ? s_total : APP_ERROR_LOG_LEN;
    size_t n = available < max ? available : max;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (s_next + APP_ERROR_LOG_LEN - 1 - i) % APP_ERROR_LOG_LEN;
        out[i] = s_errors[idx];
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t app_get_error_count(void)
{
    return s_total;
}

void app_state_set_busy(uint32_t flags, bool on)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (on) {
        s_busy |= flags;
    } else {
        s_busy &= ~flags;
    }
    xSemaphoreGive(s_lock);
}

uint32_t app_state_get_busy(void)
{
    return s_busy;
}
