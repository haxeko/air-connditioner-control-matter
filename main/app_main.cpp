/*
 * AC Matter IR controller - application entry point and glue between the modules.
 *
 *   hub (Fabric A) --Thermostat writes--> matter_thermostat --> climate_control --> ac_profiles --> ir_transceiver
 *   original remote --> ir_transceiver (monitor) --> climate_control (decode) --> matter_thermostat (shown only)
 *   sensor (Fabric B) --subscription--> remote_temperature --> LocalTemperature + climate_control
 *   BOOT x2 --> button_handler --> Matter pairing mode (commissioning window), status LED blinks blue
 *   BOOT x3 --> button_handler --> wifi_config (SoftAP) --> web_config (portal, started once)
 *   BOOT held 10 s, released --> leave all home ecosystems; held 20 s --> factory reset (app_system)
 *   firmware file uploaded on the portal --> other OTA slot --> restart --> confirmed after a trial run
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
#include "app_version.h"
#include "button_handler.h"
#include "climate_control.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ir_transceiver.h"
#include "matter_thermostat.h"
#include "remote_temperature.h"
#include "sdkconfig.h"
#include "status_led.h"
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
static_assert(CONFIG_APP_BUTTON_PAIRING_PRESS_COUNT != CONFIG_APP_BUTTON_PRESS_COUNT,
              "BOOT gestures for pairing mode and for the portal must differ");

/*
 * GPIO choices that cannot work on an ESP32-S3-WROOM-1-N16R8 are refused at build time (the Kconfig
 * range 0..48 cannot express them): GPIO26-32 are the SPI flash, GPIO33-37 the octal PSRAM, GPIO43/44
 * the UART0 console used for flashing and logs. The IR pins also stay off the strapping pins: the gate
 * pull-down of the driver or the idle-high receiver output would change the boot mode (GPIO0, 46),
 * the flash voltage (GPIO45) or the JTAG source (GPIO3).
 */
#if CONFIG_SPIRAM_MODE_OCT
#define APP_GPIO_MEMORY_LAST 37
#else
#define APP_GPIO_MEMORY_LAST 32
#endif
#define APP_GPIO_USABLE(n) (!((n) >= 26 && (n) <= APP_GPIO_MEMORY_LAST) && (n) != 43 && (n) != 44)
#define APP_GPIO_STRAPPING(n) ((n) == 0 || (n) == 3 || (n) == 45 || (n) == 46)
static_assert(APP_GPIO_USABLE(CONFIG_APP_IR_TX_GPIO) && !APP_GPIO_STRAPPING(CONFIG_APP_IR_TX_GPIO),
              "IR TX GPIO: not a flash/PSRAM, UART0 or strapping pin");
static_assert(APP_GPIO_USABLE(CONFIG_APP_IR_RX_GPIO) && !APP_GPIO_STRAPPING(CONFIG_APP_IR_RX_GPIO),
              "IR RX GPIO: not a flash/PSRAM, UART0 or strapping pin");
static_assert(APP_GPIO_USABLE(CONFIG_APP_BOOT_BUTTON_GPIO), "BOOT button GPIO: not a flash/PSRAM or UART0 pin");
static_assert(CONFIG_APP_IR_TX_GPIO != CONFIG_APP_IR_RX_GPIO && CONFIG_APP_IR_TX_GPIO != CONFIG_APP_BOOT_BUTTON_GPIO &&
                  CONFIG_APP_IR_RX_GPIO != CONFIG_APP_BOOT_BUTTON_GPIO,
              "IR TX, IR RX and the BOOT button need three different GPIOs");
#ifndef CONFIG_APP_STATUS_LED_NONE
static_assert(APP_GPIO_USABLE(CONFIG_APP_STATUS_LED_GPIO), "status LED GPIO: not a flash/PSRAM or UART0 pin");
static_assert(CONFIG_APP_STATUS_LED_GPIO != CONFIG_APP_IR_TX_GPIO && CONFIG_APP_STATUS_LED_GPIO != CONFIG_APP_IR_RX_GPIO &&
                  CONFIG_APP_STATUS_LED_GPIO != CONFIG_APP_BOOT_BUTTON_GPIO,
              "the status LED needs its own GPIO");
#endif

/* CHIP thread: only posts to the climate task queue. */
/* Fan speed / Auto / swing chosen in a home app (CHIP thread, non blocking). */
static void on_matter_fan(uint8_t fan, uint8_t swing)
{
    climate_control_set_fan_swing(fan, swing);
}

static void on_thermostat_change(const thermostat_user_state_t * s, uint32_t mask)
{
    climate_control_on_user_change(s->system_mode, s->cool_setpoint, s->heat_setpoint, mask);
}

/* Climate task: the original remote changed the AC; show it in the home apps (nothing is sent back). */
static void on_remote_update(const climate_remote_update_t * u)
{
    thermostat_user_state_t s = {};
    s.system_mode   = u->user_mode;
    s.cool_setpoint = u->cool_target_centi;
    s.heat_setpoint = u->heat_target_centi;
    esp_err_t err   = matter_thermostat_show_remote_state(&s, u->mask, u->has_fan, u->fan, u->swing);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) /* INVALID_STATE: Matter not started yet */
    {
        app_log_error("app", "remote state not shown in Matter: %s", esp_err_to_name(err));
    }
}

/* Effective room temperature (Matter sensor, optional local fallback, or none). */
static void on_temperature(bool valid, int16_t centi, temp_source_t source)
{
    (void) source;
    matter_thermostat_set_local_temperature(valid, centi); /* null when !valid */
    climate_control_post_temperature(valid, centi);
}

/* Pairing mode: open the thermostat commissioning window (QR code / 11-digit code). */
static void open_pairing_mode(void)
{
    matter_status_t ms;
    matter_thermostat_get_status(&ms);
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    if (!ws.sta_connected && !ms.ble_commissioning)
    {
        /* firmware built without BLE: on-network commissioning needs the home Wi-Fi */
        app_log_error("app", "pairing mode refused: not connected to the home Wi-Fi (BOOT x%d to configure it)",
                      CONFIG_APP_BUTTON_PRESS_COUNT);
        status_led_event(STATUS_LED_EVENT_ERROR);
        return;
    }
    esp_err_t err = matter_thermostat_open_commissioning_window(CONFIG_APP_COMMISSIONING_WINDOW_S);
    if (err != ESP_OK)
    {
        app_log_error("app", "pairing mode: %s", esp_err_to_name(err));
        status_led_event(STATUS_LED_EVENT_ERROR);
        return;
    }
    if (ms.ble_commissioning)
    {
        /* Over BLE the home app also hands over the Wi-Fi credentials (2.4 GHz network). */
        ESP_LOGI(TAG, "Matter pairing mode for %d s (Bluetooth%s): add \"%s\" with its QR code or 11-digit code",
                 CONFIG_APP_COMMISSIONING_WINDOW_S, ws.sta_connected ? " + home Wi-Fi" : "", APP_DEVICE_FULL_NAME);
    }
    else
    {
        ESP_LOGI(TAG, "Matter pairing mode for %d s (home Wi-Fi): add \"%s\" with its QR code or 11-digit code",
                 CONFIG_APP_COMMISSIONING_WINDOW_S, APP_DEVICE_FULL_NAME);
    }
}

/* Button task context (never the ISR). */
static void on_boot_sequence(uint8_t presses)
{
    if (presses == CONFIG_APP_BUTTON_PAIRING_PRESS_COUNT)
    {
        ESP_LOGI(TAG, "BOOT x%u: Matter pairing mode", presses);
        open_pairing_mode(); /* reopening while open restarts the timeout */
    }
    else if (presses == CONFIG_APP_BUTTON_PRESS_COUNT)
    {
        ESP_LOGI(TAG, "BOOT x%u: configuration portal", presses);
        esp_err_t err = wifi_config_portal_open(); /* opens, or extends when already open */
        if (err != ESP_OK)
        {
            app_log_error("app", "portal: %s", esp_err_to_name(err));
        }
    }
}

/* Seconds a freshly updated firmware must run before it is kept (see confirm_firmware_later()). */
#define FIRMWARE_TRIAL_S 60

/* Button task. Long holds of BOOT, see press_detector.h for the stages. */
static void on_boot_hold(hold_event_t ev)
{
    switch (ev)
    {
    case HOLD_STAGE1_REACHED:
        status_led_set(STATUS_LED_RESET_ARMED, true);
        ESP_LOGW(TAG, "BOOT held %d s: release now to leave all home apps, keep holding until %d s for a factory reset",
                 CONFIG_APP_BUTTON_HOLD_UNLINK_S, CONFIG_APP_BUTTON_HOLD_RESET_S);
        break;
    case HOLD_STAGE1_RELEASED: {
        status_led_set(STATUS_LED_RESET_ARMED, false);
        esp_err_t err = matter_thermostat_remove_all_home_fabrics();
        if (err != ESP_OK)
        {
            app_log_error("app", "leaving the home apps: %s", esp_err_to_name(err));
            status_led_event(STATUS_LED_EVENT_ERROR);
        }
        else
        {
            ESP_LOGW(TAG, "BOOT released: leaving all home apps (Wi-Fi, sensor, profiles and settings are kept)");
        }
        break;
    }
    case HOLD_STAGE2_REACHED:
        status_led_set(STATUS_LED_RESET_ARMED, false);
        status_led_set(STATUS_LED_RESET_NOW, true);
        ESP_LOGW(TAG, "BOOT held %d s: factory reset", CONFIG_APP_BUTTON_HOLD_RESET_S);
        app_factory_reset(); /* does not return when the request could be stored */
        status_led_set(STATUS_LED_RESET_NOW, false);
        status_led_event(STATUS_LED_EVENT_ERROR);
        break;
    default:
        break;
    }
}

/*
 * A firmware written by the portal's update page starts "on trial" (bootloader with app rollback):
 * if it resets before it is confirmed, the bootloader returns to the previous firmware. It is
 * confirmed once it has run FIRMWARE_TRIAL_S with Matter, Wi-Fi and the portal code started.
 */
static void confirm_firmware(void * arg)
{
    (void) arg;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "firmware %s confirmed after %d s: it stays", APP_FW_VERSION, FIRMWARE_TRIAL_S);
    }
    else
    {
        app_log_error("app", "firmware not confirmed: %s", esp_err_to_name(err));
    }
}

static void confirm_firmware_later(void)
{
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t * running = esp_ota_get_running_partition();
    if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return; /* flashed over serial, already confirmed, or a bootloader without rollback */
    }
    ESP_LOGW(TAG, "updated firmware %s on trial: kept after %d s without a reset, otherwise the previous one returns",
             APP_FW_VERSION, FIRMWARE_TRIAL_S);
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = confirm_firmware, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                           .name = "fw_confirm", .skip_unhandled_events = false };
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, static_cast<uint64_t>(FIRMWARE_TRIAL_S) * 1000000ULL) != ESP_OK)
    {
        confirm_firmware(nullptr); /* no timer: better kept than rolled back at the next restart */
    }
}

static void on_portal(bool open)
{
    web_config_portal_hook(open);
    status_led_set(STATUS_LED_PORTAL, open);
}

/* CHIP thread: non-blocking LED update only. */
static void on_matter_ui(matter_ui_event_t ev)
{
    switch (ev)
    {
    case MATTER_UI_WINDOW_OPENED:
        status_led_set(STATUS_LED_PAIRING, true);
        break;
    case MATTER_UI_WINDOW_CLOSED:
        status_led_set(STATUS_LED_PAIRING, false);
        break;
    case MATTER_UI_COMMISSIONED:
        status_led_event(STATUS_LED_EVENT_PAIRED);
        break;
    case MATTER_UI_IDENTIFY_START:
        status_led_set(STATUS_LED_IDENTIFY, true);
        break;
    case MATTER_UI_IDENTIFY_STOP:
        status_led_set(STATUS_LED_IDENTIFY, false);
        break;
    case MATTER_UI_IDENTIFY_EFFECT:
        status_led_event(STATUS_LED_EVENT_IDENTIFY);
        break;
    }
}

static void non_fatal(const char * what, esp_err_t err)
{
    if (err != ESP_OK)
    {
        app_log_error("app", "%s: %s", what, esp_err_to_name(err));
    }
}

/* See app_state.h. Any task except the CHIP task. */
extern "C" void app_factory_reset(void)
{
    esp_err_t err = config_store_request_factory_reset();
    if (err != ESP_OK)
    {
        app_log_error("app", "factory reset not started: %s", esp_err_to_name(err));
        return;
    }
    /* Tell the hubs that are reachable (Leave event) before everything is erased at the next start. */
    matter_thermostat_remove_all_home_fabrics();
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
}

extern "C" void app_main(void)
{
    app_state_init();
    app_crash_init(); /* report of a panic / watchdog reset before this start, shown on the diagnostics page */
    ESP_ERROR_CHECK(config_store_init());
    if (config_store_was_factory_reset())
    {
        app_log_error("app", "Đã đặt lại gốc: Wi-Fi, hệ sinh thái, cảm biến và cài đặt đã xóa");
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(err);
    }
    non_fatal("status LED", status_led_init());
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
    caps.fan_levels        = lim.fan_levels_mask;
    caps.swing             = lim.swing;
    ESP_LOGI(TAG, "thermostat limits: cool %d..%d%s%s%s (%s)", caps.cool_min, caps.cool_max, caps.heat ? ", heat" : "",
             caps.dry ? ", dry" : "", caps.fan_only ? ", fan" : "", lim.from_profile ? "locked profile" : "defaults");

    /* IR is not fatal: the thermostat still works (and reports why nothing is transmitted). */
    non_fatal("IR transceiver", ir_transceiver_init());
    climate_control_set_remote_callback(on_remote_update);
    ESP_ERROR_CHECK(climate_control_start(&cfg));

    ESP_ERROR_CHECK(matter_thermostat_create(&caps, on_thermostat_change));
    matter_thermostat_set_ui_callback(on_matter_ui); /* before start: an uncommissioned device opens the window at boot */
    {
        /* Fan/swing restored by climate_control: shown in the Fan Control cluster from the start. */
        climate_status_t cs;
        climate_control_get_status(&cs);
        matter_thermostat_set_fan(cs.fan, cs.swing);
        matter_thermostat_set_fan_callback(on_matter_fan);
    }
    err = matter_thermostat_start();
    if (err == ESP_ERR_TIMEOUT)
    {
        /* keep the portal, IR and console available for diagnosis instead of a reboot loop */
        app_log_error("app", "Matter post-start initialisation is late");
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(remote_temperature_init(&cfg, on_temperature));
    non_fatal("remote temperature", remote_temperature_start());

    /* Without the portal the thermostat, the IR side and the console still work. */
    non_fatal("configuration portal", wifi_config_start(on_portal));
    non_fatal("BOOT button", button_handler_start(on_boot_sequence, on_boot_hold));
    app_console_start();
    confirm_firmware_later();
    ESP_LOGI(TAG, "running");
}
