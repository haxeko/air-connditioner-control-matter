#pragma once

/*
 * Configuration web portal (esp_http_server), Vietnamese UI embedded in the firmware (no CDN).
 *
 * Security model:
 *   - The server is started once, the first time the portal opens, and never stopped or recreated.
 *   - It only serves connections whose local address is the SoftAP address (192.168.4.1) while the
 *     portal is open: the LAN side (station interface) is refused at accept time. Access to the
 *     SoftAP itself requires the per-device WPA2 password.
 *   - Host header must be the AP address (DNS-rebinding protection).
 *   - Every API call needs the per-boot random token in the X-CSRF-Token header (a custom header
 *     cannot be sent cross-origin without a CORS preflight, which is never granted); Origin, when
 *     present, must be the portal itself. State changes are POST only with validated form fields.
 *   - Long operations (pairing, IR learning, Wi-Fi connection, sensor reads) are started
 *     asynchronously and polled by the UI; no handler blocks on them.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the HTTP server (idempotent: later calls return ESP_OK without doing anything). */
esp_err_t web_config_start(void);

/** Portal hook for wifi_config_start(): starts the server the first time the portal opens. */
void web_config_portal_hook(bool open);

#ifdef __cplusplus
}
#endif
