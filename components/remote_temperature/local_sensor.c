#include "local_sensor.h"

#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rt_logic.h"
#include "sdkconfig.h"

#if !CONFIG_APP_LOCAL_SENSOR_NONE
#include "driver/i2c_master.h"

static const char *TAG = "local_sensor";
#endif

bool local_sensor_configured(void)
{
#if CONFIG_APP_LOCAL_SENSOR_NONE
    return false;
#else
    return true;
#endif
}

const char *local_sensor_name(void)
{
#if CONFIG_APP_LOCAL_SENSOR_SHT4X
    return "SHT4x";
#elif CONFIG_APP_LOCAL_SENSOR_SHT3X
    return "SHT3x";
#else
    return "none";
#endif
}

#if CONFIG_APP_LOCAL_SENSOR_NONE

esp_err_t local_sensor_start(local_sensor_cb_t cb)
{
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

#else

#define SHT_ADDR 0x44
#define I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev;
static local_sensor_cb_t s_cb;

static esp_err_t measure(int16_t *out_centi)
{
#if CONFIG_APP_LOCAL_SENSOR_SHT4X
    const uint8_t cmd[] = { 0xFD };   /* measure T & RH, high precision */
    const int wait_ms = 10;
#else
    const uint8_t cmd[] = { 0x24, 0x00 }; /* single shot, high repeatability, no clock stretching */
    const int wait_ms = 16;
#endif
    esp_err_t err = i2c_master_transmit(s_dev, cmd, sizeof(cmd), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    uint8_t rx[6];
    err = i2c_master_receive(s_dev, rx, sizeof(rx), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (sht_crc8(rx, 2) != rx[2]) {
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t raw = (uint16_t)((rx[0] << 8) | rx[1]);
    *out_centi = sht_raw_to_centi(raw);
    return ESP_OK;
}

static void task(void *arg)
{
    (void)arg;
    uint32_t failures = 0;
    for (;;) {
        int16_t centi = 0;
        esp_err_t err = measure(&centi);
        bool valid = (err == ESP_OK) && rt_value_plausible(centi, false, 0, false, 0);
        if (err != ESP_OK) {
            if (failures++ % 10 == 0) {
                app_log_error("local_sensor", "%s read failed: %s", local_sensor_name(), esp_err_to_name(err));
            }
        } else {
            failures = 0;
        }
        if (s_cb) {
            s_cb(valid, centi);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_LOCAL_SENSOR_PERIOD_S * 1000));
    }
}

esp_err_t local_sensor_start(local_sensor_cb_t cb)
{
    s_cb = cb;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_APP_LOCAL_SENSOR_SDA_GPIO,
        .scl_io_num = CONFIG_APP_LOCAL_SENSOR_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(task, "local_temp", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s on SDA %d SCL %d", local_sensor_name(), CONFIG_APP_LOCAL_SENSOR_SDA_GPIO,
             CONFIG_APP_LOCAL_SENSOR_SCL_GPIO);
    return ESP_OK;
}

#endif
