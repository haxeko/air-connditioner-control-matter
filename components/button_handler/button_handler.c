#include "button_handler.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "press_detector.h"
#include "sdkconfig.h"

static const char *TAG = "button";

#define SAMPLE_MS 5
#define IDLE_EXIT_MS 100

static TaskHandle_t s_task;
static button_sequence_cb_t s_cb;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static bool read_pressed(void)
{
    int level = gpio_get_level(CONFIG_APP_BOOT_BUTTON_GPIO);
#if CONFIG_APP_BOOT_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

static void task(void *arg)
{
    (void)arg;
    press_detector_cfg_t cfg = {
        .debounce_ms = CONFIG_APP_BUTTON_DEBOUNCE_MS,
        .window_ms = CONFIG_APP_BUTTON_WINDOW_MS,
        .max_press_ms = CONFIG_APP_BUTTON_MAX_PRESS_MS,
        .count = CONFIG_APP_BUTTON_PRESS_COUNT,
    };
    press_detector_t det;
    press_detector_init(&det, &cfg);

    for (;;) {
        /* Sleep until an edge interrupt, then sample until the button is idle again. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t idle_since = 0;
        for (;;) {
            int64_t now = esp_timer_get_time() / 1000;
            if (press_detector_feed(&det, read_pressed(), now)) {
                ESP_LOGI(TAG, "%d short presses detected", CONFIG_APP_BUTTON_PRESS_COUNT);
                if (s_cb) {
                    s_cb();
                }
            }
            if (press_detector_busy(&det, now)) {
                idle_since = 0;
            } else if (idle_since == 0) {
                idle_since = now;
            } else if (now - idle_since > IDLE_EXIT_MS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        }
    }
}

esp_err_t button_handler_start(button_sequence_cb_t on_sequence)
{
    s_cb = on_sequence;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_APP_BOOT_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(task, "button", 3072, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_isr_handler_add(CONFIG_APP_BOOT_BUTTON_GPIO, button_isr, NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GPIO%d: %d presses within %d ms open the configuration portal", CONFIG_APP_BOOT_BUTTON_GPIO,
                 CONFIG_APP_BUTTON_PRESS_COUNT, CONFIG_APP_BUTTON_WINDOW_MS);
    }
    return err;
}
