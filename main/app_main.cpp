/*
 * AC Matter IR controller - application entry point and glue between the modules.
 *
 *   hub (Fabric A) --Thermostat writes--> matter_thermostat --> climate_control --> ac_profiles --> ir_transceiver
 *   sensor (Fabric B) --subscription--> remote_temperature --> LocalTemperature + climate_control
 *   BOOT x3 --> button_handler --> wifi_config (SoftAP) --> web_config (portal, started once)
 *
 * Start order matters:
 *   1. NVS, netif, default event loop, SoftAP netif (before esp_matter::start() creates the Wi-Fi stack)
 *   2. AC profiles (they define the Thermostat limits / Heat feature)
 *   3. IR transceiver and climate controller (the Thermostat boot state is posted to its queue)
 *   4. Matter server + Fabric B commissioner on the same CHIP stack
 *   5. remote temperature (needs the commissioner), SoftAP/portal, BOOT button, console
 */

#include "ac_profiles.h"
#include "app_console.h"
#include "app_state.h"
#include "button_handler.h"
#include "climate_control.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ir_transceiver.h"
#include "matter_thermostat.h"
#include "remote_temperature.h"
#include "sdkconfig.h"
#include "web_config.h"
#include "wifi_config.h"

static const char * TAG = "app_main";

/* The Thermostat callback masks and SystemMode values are passed through unchanged. */
static_assert(TSTAT_CHANGED_MODE == CLIMATE_USER_MODE, "mask");
static_assert(TSTAT_CHANGED_COOL_SP == CLIMATE_USER_COOL_SP, "mask");
static_assert(TSTAT_CHANGED_HEAT_SP == CLIMATE_USER_HEAT_SP, "mask");
static_assert(TSTAT_CHANGED_BOOT == CLIMATE_USER_BOOT, "mask");
static_assert(TSTAT_MODE_OFF == CLIMATE_MODE_OFF && TSTAT_MODE_COOL == CLIMATE_MODE_COOL &&
                  TSTAT_MODE_HEAT == CLIMATE_MODE_HEAT && TSTAT_MODE_FAN_ONLY == CLIMATE_MODE_FAN_ONLY &&
                  TSTAT_MODE_DRY == CLIMATE_MODE_DRY,
              "SystemMode values");

/* CHIP thread: only posts to the climate task queue. */
static void on_thermostat_change(const thermostat_user_state_t * s, uint32_t mask)
{
    climate_control_on_user_change(s->system_mode, s->cool_setpoint, s->heat_setpoint, mask);
}

/* Effective room temperature (Matter sensor, optional local fallback, or none). */
static void on_temperature(bool valid, int16_t centi, temp_source_t source)
{
    (void) source;
    matter_thermostat_set_local_temperature(valid, centi); /* null when !valid */
    climate_control_post_temperature(valid, centi);
}

/* Button task context (never the ISR). */
static void on_boot_sequence(void)
{
    ESP_LOGI(TAG, "BOOT pressed %d times: configuration portal", CONFIG_APP_BUTTON_PRESS_COUNT);
    esp_err_t err = wifi_config_portal_open(); /* opens, or extends when already open */
    if (err != ESP_OK)
    {
        app_log_error("app", "portal: %s", esp_err_to_name(err));
    }
}

static void non_fatal(const char * what, esp_err_t err)
{
    if (err != ESP_OK)
    {
        app_log_error("app", "%s: %s", what, esp_err_to_name(err));
    }
}

extern "C" void app_main(void)
{
    app_state_init();
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(wifi_config_early_init());

    app_config_t cfg;
    non_fatal("app config", app_config_load(&cfg)); /* defaults when missing or invalid */
    ESP_ERROR_CHECK(ac_profiles_init());

    ac_thermostat_limits_t lim;
    ac_profiles_thermostat_limits(ac_profiles_active_slot(), &lim);
    thermostat_caps_t caps = {};
    caps.heat              = lim.heat;
    caps.dry               = lim.dry;
    caps.fan_only          = lim.fan_only;
    caps.cool_min          = lim.cool_min;
    caps.cool_max          = lim.cool_max;
    caps.heat_min          = lim.heat_min;
    caps.heat_max          = lim.heat_max;
    ESP_LOGI(TAG, "thermostat limits: cool %d..%d%s%s%s (%s)", caps.cool_min, caps.cool_max, caps.heat ? ", heat" : "",
             caps.dry ? ", dry" : "", caps.fan_only ? ", fan" : "", lim.from_profile ? "locked profile" : "defaults");

    /* IR is not fatal: the thermostat still works (and reports why nothing is transmitted). */
    non_fatal("IR transceiver", ir_transceiver_init());
    ESP_ERROR_CHECK(climate_control_start(&cfg));

    ESP_ERROR_CHECK(matter_thermostat_create(&caps, on_thermostat_change));
    ESP_ERROR_CHECK(matter_thermostat_start());

    ESP_ERROR_CHECK(remote_temperature_init(&cfg, on_temperature));
    non_fatal("remote temperature", remote_temperature_start());

    ESP_ERROR_CHECK(wifi_config_start(web_config_portal_hook));
    non_fatal("BOOT button", button_handler_start(on_boot_sequence));
    app_console_start();
    ESP_LOGI(TAG, "running");
}
