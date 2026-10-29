#include "ir_transceiver.h"

#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ir_capture.h"
#include "ir_code.h"
#include "sdkconfig.h"

static const char *TAG = "ir";

#define IR_RESOLUTION_HZ 1000000 /* 1 tick = 1 us */
#define RX_CHUNK_SYMBOLS 128      /* partial-receive buffer handed to the driver */
#define RX_MEM_BLOCK_SYMBOLS 96   /* 2 hardware blocks: margin against interrupt latency */
#define TX_MEM_BLOCK_SYMBOLS 96
#define MIN_COMMAND_LEVELS 16     /* shorter captures are noise, not a remote */
#define SEG_QUEUE_LEN 40
#define SYMBOL_STORE_SYMBOLS (CONFIG_APP_IR_MAX_LEVELS / 2 + 2 * CONFIG_APP_IR_MAX_SEGMENTS)

/* ------------------------------------------------------------------------------------------ */
/* RX: everything touched by the ISR lives in internal RAM (ISR is cache-safe)                  */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    uint16_t offset;
    uint16_t count;
    int64_t irq_time_us;
    bool overflow;
} seg_event_t;

static rmt_channel_handle_t s_rx;
static rmt_channel_handle_t s_tx;
static rmt_encoder_handle_t s_copy_encoder;

static DRAM_ATTR rmt_symbol_word_t s_rx_chunk[RX_CHUNK_SYMBOLS];
static DRAM_ATTR rmt_receive_config_t s_rx_cfg;
static rmt_symbol_word_t *s_sym_store;      /* internal RAM, allocated per learning session */
static volatile DRAM_ATTR uint32_t s_sym_write;
static volatile DRAM_ATTR uint32_t s_seg_start;
static volatile DRAM_ATTR bool s_seg_overflow;
static volatile DRAM_ATTR bool s_rx_armed;
static QueueHandle_t s_seg_queue;
static StaticQueue_t s_seg_queue_buf;
static DRAM_ATTR uint8_t s_seg_queue_storage[SEG_QUEUE_LEN * sizeof(seg_event_t)];

static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *ctx)
{
    (void)ctx;
    BaseType_t woken = pdFALSE;
    if (s_sym_store == NULL) {
        return false;
    }
    size_t n = edata->num_symbols;
    if (s_sym_write + n > SYMBOL_STORE_SYMBOLS) {
        s_seg_overflow = true;
        n = SYMBOL_STORE_SYMBOLS - s_sym_write;
    }
    if (n > 0) {
        memcpy(&s_sym_store[s_sym_write], edata->received_symbols, n * sizeof(rmt_symbol_word_t));
        s_sym_write += n;
    }
    if (edata->flags.is_last) {
        seg_event_t ev = {
            .offset = (uint16_t)s_seg_start,
            .count = (uint16_t)(s_sym_write - s_seg_start),
            .irq_time_us = esp_timer_get_time(),
            .overflow = s_seg_overflow,
        };
        s_seg_start = s_sym_write;
        s_seg_overflow = false;
        if (xQueueSendFromISR(s_seg_queue, &ev, &woken) != pdTRUE) {
            /* queue full: more segments than allowed -> reported as overflow on the next one */
            s_seg_overflow = true;
        }
        /* Re-arm immediately so that the next frame of a multi-frame command is not missed. */
        if (s_rx_armed) {
            if (rmt_receive(channel, s_rx_chunk, sizeof(s_rx_chunk), &s_rx_cfg) != ESP_OK) {
                s_rx_armed = false;
            }
        }
    }
    return woken == pdTRUE;
}

/* ------------------------------------------------------------------------------------------ */
/* State shared between API callers and the worker task                                         */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    uint8_t *blob;
    size_t len;
    ir_source_t source;
    bool force;
    uint8_t repeats;
    uint32_t crc;
    char label[48];
} tx_request_t;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static TaskHandle_t s_task;
static ir_learn_status_t s_learn;
static uint8_t *s_learn_result;
static size_t s_learn_result_len;
static volatile bool s_learn_requested;
static volatile bool s_learn_cancel;
static tx_request_t s_pending;
static bool s_has_pending;
static ir_tx_status_t s_tx_status;
static uint32_t s_last_crc;
static int64_t s_last_tx_end_us;
static uint16_t s_carrier_hz = CONFIG_APP_IR_CARRIER_HZ;
static uint16_t s_applied_carrier_hz;

#define NOTIFY_TX (1u << 0)
#define NOTIFY_LEARN (1u << 1)

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void set_learn(ir_learn_state_t st, const char *msg)
{
    lock();
    s_learn.state = st;
    if (msg) {
        strlcpy(s_learn.message, msg, sizeof(s_learn.message));
    }
    unlock();
}

/* ------------------------------------------------------------------------------------------ */
/* Learning                                                                                      */
/* ------------------------------------------------------------------------------------------ */

static void rx_stop(void)
{
    s_rx_armed = false;
    rmt_disable(s_rx); /* aborts a pending receive */
}

static void learn_session(void)
{
    const uint32_t idle_us = CONFIG_APP_IR_RX_IDLE_US;
    uint16_t *levels = heap_caps_malloc(CONFIG_APP_IR_MAX_LEVELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
    ir_symbol_t *conv = heap_caps_malloc(SYMBOL_STORE_SYMBOLS * sizeof(ir_symbol_t), MALLOC_CAP_8BIT);
    rmt_symbol_word_t *store =
        heap_caps_malloc(SYMBOL_STORE_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (levels == NULL || conv == NULL || store == NULL) {
        free(levels);
        free(conv);
        free(store);
        set_learn(IR_LEARN_FAILED, "Không đủ bộ nhớ cho bộ đệm học IR");
        return;
    }

    ir_capture_t cap;
    ir_capture_reset(&cap, levels, CONFIG_APP_IR_MAX_LEVELS, s_carrier_hz, idle_us, CONFIG_APP_IR_MAX_SEGMENTS);

    xQueueReset(s_seg_queue);
    s_sym_write = 0;
    s_seg_start = 0;
    s_seg_overflow = false;
    s_sym_store = store;

    s_rx_cfg.signal_range_min_ns = CONFIG_APP_IR_RX_GLITCH_NS;
    s_rx_cfg.signal_range_max_ns = idle_us * 1000u;
    s_rx_cfg.flags.en_partial_rx = 1;

    esp_err_t err = rmt_enable(s_rx);
    if (err == ESP_OK) {
        s_rx_armed = true;
        err = rmt_receive(s_rx, s_rx_chunk, sizeof(s_rx_chunk), &s_rx_cfg);
    }
    if (err != ESP_OK) {
        rx_stop();
        s_sym_store = NULL;
        free(levels);
        free(conv);
        free(store);
        app_log_error("ir", "rmt_receive: %s", esp_err_to_name(err));
        set_learn(IR_LEARN_FAILED, "Không bật được bộ thu IR");
        return;
    }
    set_learn(IR_LEARN_WAITING, "Hướng remote vào đầu thu và nhấn nút cần học");
    app_state_set_busy(APP_BUSY_IR_LEARNING, true);

    const int64_t deadline = esp_timer_get_time() + (int64_t)CONFIG_APP_IR_LEARN_TIMEOUT_S * 1000000LL;
    ir_result_t final = IR_OK;
    bool done = false;
    uint16_t noise = 0;

    while (!done) {
        if (s_learn_cancel) {
            final = IR_ERR_ARG;
            break;
        }
        TickType_t wait;
        if (cap.code.segment_count == 0) {
            int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
            if (left_ms <= 0) {
                final = IR_ERR_EMPTY;
                break;
            }
            wait = pdMS_TO_TICKS(left_ms < 200 ? left_ms : 200);
        } else {
            wait = pdMS_TO_TICKS(CONFIG_APP_IR_LEARN_END_GAP_MS);
        }
        seg_event_t ev;
        if (xQueueReceive(s_seg_queue, &ev, wait) == pdTRUE) {
            if (ev.count > SYMBOL_STORE_SYMBOLS || ev.offset + ev.count > SYMBOL_STORE_SYMBOLS) {
                ev.overflow = true;
                ev.count = 0;
            }
            for (uint16_t i = 0; i < ev.count; i++) {
                const rmt_symbol_word_t *s = &store[ev.offset + i];
                conv[i].duration0 = s->duration0;
                conv[i].level0 = s->level0;
                conv[i].duration1 = s->duration1;
                conv[i].level1 = s->level1;
            }
            ir_result_t r = ir_capture_add(&cap, conv, ev.count, ev.irq_time_us, ev.overflow);
            if (r != IR_OK && r != IR_ERR_EMPTY) {
                final = r;
                break;
            }
            if (cap.code.segment_count > 0) {
                lock();
                s_learn.state = IR_LEARN_CAPTURING;
                s_learn.segments = cap.code.segment_count;
                s_learn.levels = cap.code.total_levels;
                unlock();
            }
            continue;
        }
        if (cap.code.segment_count == 0) {
            continue; /* still waiting for the first pulse */
        }
        /* Silence after the last segment: the command is complete. */
        if (ir_capture_is_noise(&cap, MIN_COMMAND_LEVELS)) {
            noise++;
            ESP_LOGW(TAG, "discarding %u level(s) of IR noise", cap.code.total_levels);
            ir_capture_reset(&cap, levels, CONFIG_APP_IR_MAX_LEVELS, s_carrier_hz, idle_us, CONFIG_APP_IR_MAX_SEGMENTS);
            /* restart the symbol store as well (the receiver keeps running) */
            rx_stop();
            s_sym_write = 0;
            s_seg_start = 0;
            s_seg_overflow = false;
            xQueueReset(s_seg_queue);
            if (rmt_enable(s_rx) == ESP_OK) {
                s_rx_armed = true;
                if (rmt_receive(s_rx, s_rx_chunk, sizeof(s_rx_chunk), &s_rx_cfg) != ESP_OK) {
                    final = IR_ERR_ARG;
                    break;
                }
            }
            lock();
            s_learn.state = IR_LEARN_WAITING;
            s_learn.noise_discarded = noise;
            unlock();
            continue;
        }
        done = true;
    }

    rx_stop();
    s_sym_store = NULL;
    app_state_set_busy(APP_BUSY_IR_LEARNING, false);

    if (done && cap.error == IR_OK) {
        size_t sz = ir_code_serialized_size(&cap.code);
        uint8_t *blob = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
        if (blob != NULL && ir_code_serialize(&cap.code, blob, sz) == sz) {
            lock();
            free(s_learn_result);
            s_learn_result = blob;
            s_learn_result_len = sz;
            s_learn.state = IR_LEARN_DONE;
            s_learn.segments = cap.code.segment_count;
            s_learn.levels = cap.code.total_levels;
            s_learn.duration_ms = (uint32_t)(ir_code_duration_us(&cap.code) / 1000);
            s_learn.result_size = sz;
            s_learn.noise_discarded = noise;
            snprintf(s_learn.message, sizeof(s_learn.message), "Đã học: %u đoạn, %u mức, %lu ms",
                     cap.code.segment_count, cap.code.total_levels, (unsigned long)s_learn.duration_ms);
            unlock();
            ESP_LOGI(TAG, "learned %u segment(s), %u level(s), %u bytes", cap.code.segment_count,
                     cap.code.total_levels, (unsigned)sz);
        } else {
            free(blob);
            set_learn(IR_LEARN_FAILED, "Không đủ bộ nhớ để lưu kết quả");
        }
    } else if (s_learn_cancel) {
        set_learn(IR_LEARN_CANCELLED, "Đã hủy");
    } else {
        ir_result_t why = (cap.error != IR_OK) ? cap.error : final;
        const char *msg;
        switch (why) {
        case IR_ERR_LEVELS_FULL:
            msg = "Lệnh quá dài: vượt bộ đệm (tăng CONFIG_APP_IR_MAX_LEVELS); không lưu bản bị cắt";
            break;
        case IR_ERR_SEGMENTS_FULL:
            msg = "Quá nhiều đoạn (tăng CONFIG_APP_IR_MAX_SEGMENTS)";
            break;
        case IR_ERR_FORMAT:
            msg = "Tín hiệu bất thường (mức kéo dài quá 65 ms) - kiểm tra đầu thu";
            break;
        case IR_ERR_EMPTY:
            msg = "Không nhận được tín hiệu remote trong thời gian chờ";
            break;
        default:
            msg = "Học IR thất bại";
            break;
        }
        set_learn(IR_LEARN_FAILED, msg);
    }
    free(levels);
    free(conv);
    free(store);
}

/* ------------------------------------------------------------------------------------------ */
/* Transmission                                                                                 */
/* ------------------------------------------------------------------------------------------ */

static esp_err_t apply_carrier(uint16_t hz)
{
    if (hz == s_applied_carrier_hz) {
        return ESP_OK;
    }
    rmt_carrier_config_t carrier = {
        .frequency_hz = hz,
        .duty_cycle = (float)CONFIG_APP_IR_CARRIER_DUTY_PERCENT / 100.0f,
    };
    esp_err_t err = rmt_disable(s_tx);
    if (err == ESP_OK) {
        err = rmt_apply_carrier(s_tx, &carrier);
    }
    esp_err_t err2 = rmt_enable(s_tx);
    if (err == ESP_OK && err2 == ESP_OK) {
        s_applied_carrier_hz = hz;
        return ESP_OK;
    }
    return err != ESP_OK ? err : err2;
}

static esp_err_t transmit_blob(const uint8_t *blob, size_t len, uint8_t repeats)
{
    uint16_t total_levels = 0;
    if (ir_code_validate_blob(blob, len, &total_levels) != IR_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t *levels = heap_caps_malloc((size_t)total_levels * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (levels == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ir_code_t code;
    ir_code_init(&code, levels, total_levels, 0);
    if (ir_code_deserialize(&code, blob, len) != IR_OK) {
        free(levels);
        return ESP_ERR_INVALID_ARG;
    }
    size_t nsym = ir_code_symbol_count(&code);
    ir_symbol_t *tmp = heap_caps_malloc(nsym * sizeof(ir_symbol_t), MALLOC_CAP_8BIT);
    /* The copy encoder is executed from the cache-safe TX ISR: symbols must be in internal RAM. */
    rmt_symbol_word_t *syms = heap_caps_malloc(nsym * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_OK;
    if (tmp == NULL || syms == NULL || ir_code_to_symbols(&code, tmp, nsym) != nsym) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    for (size_t i = 0; i < nsym; i++) {
        syms[i].duration0 = tmp[i].duration0;
        syms[i].level0 = tmp[i].level0;
        syms[i].duration1 = tmp[i].duration1;
        syms[i].level1 = tmp[i].level1;
    }
    uint16_t carrier = code.carrier_hz ? code.carrier_hz : s_carrier_hz;
    /* the configured carrier always wins: learned codes record what was configured, not measured */
    carrier = s_carrier_hz;
    err = apply_carrier(carrier);
    if (err != ESP_OK) {
        goto out;
    }
    uint64_t dur_us = ir_code_duration_us(&code);
    int timeout_ms = (int)(dur_us / 1000) + 500;
    rmt_transmit_config_t cfg = { .loop_count = 0, .flags = { .eot_level = 0 } };
    for (uint8_t r = 0; r <= repeats && err == ESP_OK; r++) {
        if (r > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_IR_TX_MIN_GAP_MS));
        }
        err = rmt_transmit(s_tx, s_copy_encoder, syms, nsym * sizeof(rmt_symbol_word_t), &cfg);
        if (err == ESP_OK) {
            err = rmt_tx_wait_all_done(s_tx, timeout_ms);
        }
    }
out:
    free(levels);
    free(tmp);
    free(syms);
    return err;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if (s_learn_requested) {
            s_learn_requested = false;
            learn_session();
        }

        for (;;) {
            tx_request_t req;
            lock();
            if (!s_has_pending) {
                unlock();
                break;
            }
            req = s_pending;
            s_has_pending = false;
            s_tx_status.busy = true;
            unlock();

            int64_t since = esp_timer_get_time() - s_last_tx_end_us;
            int64_t min_gap = (int64_t)CONFIG_APP_IR_TX_MIN_GAP_MS * 1000;
            if (s_last_tx_end_us != 0 && since < min_gap) {
                vTaskDelay(pdMS_TO_TICKS((min_gap - since) / 1000 + 1));
            }
            esp_err_t err = transmit_blob(req.blob, req.len, req.repeats);
            s_last_tx_end_us = esp_timer_get_time();

            lock();
            s_tx_status.busy = false;
            s_tx_status.last_error = err;
            if (err == ESP_OK) {
                s_tx_status.sent++;
                s_tx_status.last_sent_us = s_last_tx_end_us;
                s_tx_status.last_source = req.source;
                strlcpy(s_tx_status.last_label, req.label, sizeof(s_tx_status.last_label));
                s_last_crc = req.crc;
            } else {
                s_tx_status.failed++;
            }
            unlock();
            if (err != ESP_OK) {
                app_log_error("ir", "transmit '%s' failed: %s", req.label, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "IR sent: %s", req.label);
            }
            free(req.blob);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Public API                                                                                    */
/* ------------------------------------------------------------------------------------------ */

esp_err_t ir_transceiver_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_seg_queue = xQueueCreateStatic(SEG_QUEUE_LEN, sizeof(seg_event_t), s_seg_queue_storage, &s_seg_queue_buf);

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = CONFIG_APP_IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = TX_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 2,
#if CONFIG_APP_IR_TX_INVERT
        .flags.invert_out = 1,
#endif
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_tx), TAG, "rmt tx");
    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&enc_cfg, &s_copy_encoder), TAG, "copy encoder");
    rmt_carrier_config_t carrier = {
        .frequency_hz = s_carrier_hz,
        .duty_cycle = (float)CONFIG_APP_IR_CARRIER_DUTY_PERCENT / 100.0f,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx, &carrier), TAG, "carrier");
    s_applied_carrier_hz = s_carrier_hz;
    ESP_RETURN_ON_ERROR(rmt_enable(s_tx), TAG, "tx enable");

    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = CONFIG_APP_IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = RX_MEM_BLOCK_SYMBOLS,
#if CONFIG_APP_IR_RX_ACTIVE_LOW
        .flags.invert_in = 1, /* so that a mark (carrier detected) is level 1 */
#endif
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &s_rx), TAG, "rmt rx");
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx, &cbs, NULL), TAG, "rx callbacks");
    /* the receiver input keeps its pull-up so a disconnected module does not produce noise */
    gpio_pullup_en(CONFIG_APP_IR_RX_GPIO);

    s_tx_status.initialised = true;
    s_tx_status.carrier_hz = s_carrier_hz;
    if (xTaskCreate(worker, "ir", 4096, NULL, 9, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RMT ready: TX GPIO %d, RX GPIO %d, carrier %u Hz", CONFIG_APP_IR_TX_GPIO, CONFIG_APP_IR_RX_GPIO,
             s_carrier_hz);
    return ESP_OK;
}

esp_err_t ir_learn_start(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    bool active = s_learn.state == IR_LEARN_WAITING || s_learn.state == IR_LEARN_CAPTURING || s_learn_requested;
    if (!active) {
        s_learn.session++;
        s_learn.state = IR_LEARN_WAITING;
        s_learn.segments = 0;
        s_learn.levels = 0;
        s_learn.duration_ms = 0;
        s_learn.noise_discarded = 0;
        s_learn.result_size = 0;
        strlcpy(s_learn.message, "Đang chuẩn bị bộ thu", sizeof(s_learn.message));
        free(s_learn_result);
        s_learn_result = NULL;
        s_learn_result_len = 0;
        s_learn_cancel = false;
        s_learn_requested = true;
    }
    unlock();
    if (active) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_task, NOTIFY_LEARN, eSetBits);
    return ESP_OK;
}

void ir_learn_cancel(void)
{
    s_learn_cancel = true;
}

void ir_learn_get_status(ir_learn_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_learn;
    unlock();
}

esp_err_t ir_learn_get_result(uint8_t *out, size_t *inout_len)
{
    lock();
    esp_err_t err = ESP_OK;
    if (s_learn_result == NULL || s_learn.state != IR_LEARN_DONE) {
        err = ESP_ERR_NOT_FOUND;
    } else if (out == NULL || *inout_len < s_learn_result_len) {
        *inout_len = s_learn_result_len;
        err = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(out, s_learn_result, s_learn_result_len);
        *inout_len = s_learn_result_len;
    }
    unlock();
    return err;
}

esp_err_t ir_send(const uint8_t *blob, size_t len, ir_source_t source, bool force, uint8_t repeats, const char *label)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (blob == NULL || ir_code_validate_blob(blob, len, NULL) != IR_OK || repeats > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t crc = esp_rom_crc32_le(0, blob, len);
    int64_t now = esp_timer_get_time();

    lock();
    if (!force && s_tx_status.last_sent_us != 0 && crc == s_last_crc &&
        now - s_tx_status.last_sent_us < (int64_t)CONFIG_APP_IR_TX_DUPLICATE_WINDOW_MS * 1000) {
        s_tx_status.dropped_duplicate++;
        unlock();
        return ESP_OK;
    }
    if (s_has_pending && !force && s_pending.crc == crc) {
        unlock();
        return ESP_OK; /* identical command already waiting */
    }
    unlock();

    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, blob, len);

    lock();
    if (s_has_pending) {
        free(s_pending.blob); /* full-state commands: only the newest matters */
        s_tx_status.superseded++;
    }
    s_pending.blob = copy;
    s_pending.len = len;
    s_pending.source = source;
    s_pending.force = force;
    s_pending.repeats = repeats;
    s_pending.crc = crc;
    strlcpy(s_pending.label, label ? label : "", sizeof(s_pending.label));
    s_has_pending = true;
    unlock();
    xTaskNotify(s_task, NOTIFY_TX, eSetBits);
    return ESP_OK;
}

void ir_tx_get_status(ir_tx_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_tx_status;
    unlock();
}

esp_err_t ir_set_carrier(uint16_t carrier_hz)
{
    if (carrier_hz < 30000 || carrier_hz > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_carrier_hz = carrier_hz;
    s_tx_status.carrier_hz = carrier_hz;
    unlock();
    return ESP_OK;
}
