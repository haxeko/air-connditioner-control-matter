#include "status_led.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_APP_STATUS_LED_WS2812
#include "driver/rmt_tx.h"
#elif CONFIG_APP_STATUS_LED_SIMPLE
#include "driver/gpio.h"
#endif

static const char *TAG = "status_led";

#define TICK_MS 100
#define BLINK_HALF_TICKS 4 /* 0.4 s on, 0.4 s off */

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t k_off = { 0, 0, 0 };
static const rgb_t k_blue = { 0, 0, 255 };
static const rgb_t k_green = { 0, 255, 0 };
static const rgb_t k_red = { 255, 0, 0 };
static const rgb_t k_white = { 255, 255, 255 };

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static esp_timer_handle_t s_timer;
static uint32_t s_flags;
static uint32_t s_tick;
static int s_event = -1;        /* active short event, -1 = none */
static uint32_t s_event_ticks;  /* ticks left */
static bool s_ready;

/* ------------------------------------------------------------------------------------------ */
/* Output                                                                                       */
/* ------------------------------------------------------------------------------------------ */

#if CONFIG_APP_STATUS_LED_WS2812
#define WS_RESOLUTION_HZ 10000000 /* 0.1 us per tick */
#define WS_T0H 3                  /* 0.3 us */
#define WS_T0L 9                  /* 0.9 us */
#define WS_T1H 9
#define WS_T1L 3
#define WS_RESET_TICKS 1500       /* 2 x 150 us low = 300 us (WS2812B needs >= 280 us) */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
/* Read by the copy encoder from the (cache-safe) RMT ISR: must live in internal RAM. */
static DRAM_ATTR rmt_symbol_word_t s_frame[25];

static esp_err_t out_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num = CONFIG_APP_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&cfg, &s_chan);
    if (err == ESP_OK) {
        rmt_copy_encoder_config_t enc = {};
        err = rmt_new_copy_encoder(&enc, &s_enc);
    }
    if (err == ESP_OK) {
        err = rmt_enable(s_chan);
    }
    return err;
}

static void out_write(rgb_t c)
{
    const uint8_t scale = CONFIG_APP_STATUS_LED_BRIGHTNESS;
    const uint8_t bytes[3] = {
        /* WS2812 order: green, red, blue */
        (uint8_t)((c.g * scale) / 255),
        (uint8_t)((c.r * scale) / 255),
        (uint8_t)((c.b * scale) / 255),
    };
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (bytes[i] >> bit) & 1u;
            s_frame[n].level0 = 1;
            s_frame[n].duration0 = one ? WS_T1H : WS_T0H;
            s_frame[n].level1 = 0;
            s_frame[n].duration1 = one ? WS_T1L : WS_T0L;
            n++;
        }
    }
    s_frame[n].level0 = 0;
    s_frame[n].duration0 = WS_RESET_TICKS;
    s_frame[n].level1 = 0;
    s_frame[n].duration1 = WS_RESET_TICKS;
    n++;
    rmt_transmit_config_t tx = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_enc, s_frame, n * sizeof(rmt_symbol_word_t), &tx) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, 10); /* ~0.3 ms frame; s_frame is reused for the next one */
    }
}

#elif CONFIG_APP_STATUS_LED_SIMPLE

static esp_err_t out_init(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_APP_STATUS_LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    return gpio_config(&io);
}

static void out_write(rgb_t c)
{
    bool on = c.r || c.g || c.b;
#if CONFIG_APP_STATUS_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_APP_STATUS_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_APP_STATUS_LED_GPIO, on ? 1 : 0);
#endif
}

#else

static esp_err_t out_init(void)
{
    return ESP_OK;
}

static void out_write(rgb_t c)
{
    (void)c;
}

#endif

/* ------------------------------------------------------------------------------------------ */
/* Pattern                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* Called with s_lock held. */
static rgb_t current_colour(void)
{
    if (s_flags & STATUS_LED_RESET_NOW) {
        return k_red;
    }
    if (s_flags & STATUS_LED_RESET_ARMED) {
        return (s_tick % 2) == 0 ? k_red : k_off; /* 0.1 s on / 0.1 s off */
    }
    if (s_flags & STATUS_LED_IDENTIFY) {
        return ((s_tick / 5) % 2) == 0 ? k_white : k_off; /* 0.5 s on / 0.5 s off */
    }
    if (s_event == STATUS_LED_EVENT_PAIRED) {
        return k_green;
    }
    if (s_event == STATUS_LED_EVENT_ERROR) {
        return (s_event_ticks % 2) ? k_red : k_off; /* 3 fast red blinks */
    }
    if (s_event == STATUS_LED_EVENT_IDENTIFY) {
        return ((s_event_ticks / 3) % 2) ? k_white : k_off; /* 3 white blinks, 0.3 s each */
    }
    if (s_flags & STATUS_LED_PAIRING) {
        return ((s_tick / BLINK_HALF_TICKS) % 2) == 0 ? k_blue : k_off;
    }
    if (s_flags & STATUS_LED_PORTAL) {
        return k_green;
    }
    return k_off;
}

static rgb_t s_shown;
static bool s_shown_valid;

static void refresh_locked(void)
{
    rgb_t c = current_colour();
    if (s_shown_valid && memcmp(&c, &s_shown, sizeof(c)) == 0) {
        return; /* unchanged: no new frame */
    }
    out_write(c);
    s_shown = c;
    s_shown_valid = true;
}

static void timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tick++;
    if (s_event >= 0 && s_event_ticks > 0 && --s_event_ticks == 0) {
        s_event = -1;
    }
    refresh_locked();
    xSemaphoreGive(s_lock);
}

esp_err_t status_led_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    esp_err_t err = out_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED output init failed: %s", esp_err_to_name(err));
        return err;
    }
    const esp_timer_create_args_t args = { .callback = timer_cb, .name = "status_led" };
    err = esp_timer_create(&args, &s_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_timer, TICK_MS * 1000);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    refresh_locked();
    xSemaphoreGive(s_lock);
#if CONFIG_APP_STATUS_LED_WS2812
    ESP_LOGI(TAG, "RGB LED (WS2812) on GPIO %d", CONFIG_APP_STATUS_LED_GPIO);
#elif CONFIG_APP_STATUS_LED_SIMPLE
    ESP_LOGI(TAG, "LED on GPIO %d", CONFIG_APP_STATUS_LED_GPIO);
#endif
    return ESP_OK;
}

void status_led_set(status_led_flag_t flag, bool on)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t before = s_flags;
    s_flags = on ? (s_flags | flag) : (s_flags & ~(uint32_t)flag);
    if ((s_flags & (STATUS_LED_PAIRING | STATUS_LED_IDENTIFY)) & ~before) {
        s_tick = 0; /* start the blink with "on" */
    }
    refresh_locked();
    xSemaphoreGive(s_lock);
}

void status_led_event(status_led_event_t ev)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_event = (int)ev;
    s_event_ticks = (ev == STATUS_LED_EVENT_PAIRED) ? 20 /* 2 s */ : (ev == STATUS_LED_EVENT_IDENTIFY) ? 18 : 6 /* 3 blinks */;
    refresh_locked();
    xSemaphoreGive(s_lock);
}
