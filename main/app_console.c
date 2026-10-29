#include "app_console.h"

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "climate_control.h"
#include "esp_console.h"
#include "esp_log.h"
#include "ir_transceiver.h"
#include "matter_thermostat.h"
#include "qrcode.h"
#include "remote_temperature.h"
#include "sdkconfig.h"
#include "wifi_config.h"

static const char *TAG = "console";

static void print_centi(const char *label, bool valid, int16_t c)
{
    if (valid) {
        printf("%s%d.%02d C\n", label, c / 100, (c < 0 ? -c : c) % 100);
    } else {
        printf("%sunknown\n", label);
    }
}

static int cmd_secrets(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    char pass[65];
    printf("\nConfiguration Wi-Fi (SoftAP)\n");
    printf("  SSID     : %s\n", ws.ap_ssid);
    if (wifi_config_get_ap_password(pass, sizeof(pass)) == ESP_OK) {
        printf("  Password : %s\n", pass);
    }
    memset(pass, 0, sizeof(pass));
    printf("  Portal   : http://192.168.4.1 (%s)\n", ws.portal_open ? "open" : "closed, press BOOT 3x or type 'portal'");

    char qr[48];
    char manual[24];
    if (matter_thermostat_get_onboarding(qr, sizeof(qr), manual, sizeof(manual)) == ESP_OK) {
        printf("\nMatter onboarding (add the thermostat to Apple Home / Aqara / SmartThings)\n");
        printf("  Manual code : %s\n", manual);
        printf("  QR payload  : %s\n", qr);
        esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
        esp_qrcode_generate(&cfg, qr);
    } else {
        printf("\nMatter is not started yet\n");
    }
    memset(qr, 0, sizeof(qr));
    memset(manual, 0, sizeof(manual));
    return 0;
}

static int cmd_portal(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = wifi_config_portal_open();
    printf("portal: %s\n", err == ESP_OK ? "open (http://192.168.4.1)" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_status_t ws;
    wifi_config_get_status(&ws);
    printf("Wi-Fi   : %s %s %s\n", ws.sta_configured ? ws.sta_ssid : "(not configured)",
           ws.sta_connected ? "connected" : "not connected", ws.sta_ip4);
    printf("Portal  : %s, %u client(s)\n", ws.portal_open ? "open" : "closed", ws.ap_clients);

    matter_status_t ms;
    matter_thermostat_get_status(&ms);
    printf("Matter  : %s, %u home fabric(s), window %s, commissioner %s\n", ms.started ? "started" : "stopped",
           ms.home_fabric_count, ms.window_open ? "open" : "closed", ms.commissioner_ready ? "ready" : "not ready");

    rt_status_t rs;
    remote_temperature_get_status(&rs);
    printf("Sensor  : %s, %s\n", rs.bound ? "bound" : "none", rt_state_name(rs.state));
    print_centi("Temp    : ", rs.effective_valid, rs.effective_centi);

    climate_status_t cs;
    climate_control_get_status(&cs);
    printf("Climate : %s, mode %u, reason: %s\n", cs.ready ? "ready" : cs.not_ready, cs.user_mode,
           climate_reason_text(cs.reason));

    ir_tx_status_t ir;
    ir_tx_get_status(&ir);
    printf("IR      : %lu sent, %lu failed, last '%s'\n", (unsigned long)ir.sent, (unsigned long)ir.failed,
           ir.last_label);
    printf("Errors  : %lu recorded\n", (unsigned long)app_get_error_count());
    return 0;
}

void app_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ac>";
    repl_config.task_stack_size = 6144;
    repl_config.max_cmdline_length = 128;
    esp_err_t err;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw, &repl_config, &repl);
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw, &repl_config, &repl);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl);
#else
    ESP_LOGI(TAG, "no console configured");
    return;
#endif
    if (err != ESP_OK) {
        app_log_error("console", "REPL: %s", esp_err_to_name(err));
        return;
    }
    const esp_console_cmd_t cmds[] = {
        { .command = "secrets", .help = "Print the SoftAP password and the Matter onboarding code", .func = cmd_secrets },
        { .command = "portal", .help = "Open the configuration portal (same as BOOT x3)", .func = cmd_portal },
        { .command = "status", .help = "Short status", .func = cmd_status },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_register_help_command();
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        app_log_error("console", "start: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "console ready: 'help', 'status', 'secrets', 'portal'");
}
