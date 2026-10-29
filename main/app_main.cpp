#include "app_state.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "matter_thermostat.h"

static const char *TAG = "app_main";

static void on_user_change(const thermostat_user_state_t *s, uint32_t mask)
{
    ESP_LOGI(TAG, "user state: mode %u cool %d heat %d (mask 0x%02lx)", s->system_mode, s->cool_setpoint,
             s->heat_setpoint, (unsigned long)mask);
}

extern "C" void app_main(void)
{
    app_state_init();
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    thermostat_caps_t caps = {};
    caps.cool_min = 1600;
    caps.cool_max = 3000;
    ESP_ERROR_CHECK(matter_thermostat_create(&caps, on_user_change));
    ESP_ERROR_CHECK(matter_thermostat_start());
    ESP_LOGI(TAG, "started");
}
