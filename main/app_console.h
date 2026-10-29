#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Serial console (UART / USB-Serial-JTAG, whatever CONFIG_ESP_CONSOLE_* selects) with:
 *   secrets  - SoftAP SSID/password and the Matter onboarding code (only printed on request,
 *              never in the normal log)
 *   portal   - open the configuration portal (same as BOOT x3)
 *   status   - short status
 */
void app_console_start(void);

#ifdef __cplusplus
}
#endif
