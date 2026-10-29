#pragma once

/*
 * Wi-Fi station + configuration SoftAP.
 *
 * The station is owned by the Matter platform layer (connectedhomeip ESP32 ConnectivityManager):
 * it connects and reconnects on its own. This module only
 *   - creates the SoftAP netif once (192.168.4.1/24, DHCP server) and toggles the radio between
 *     STA and AP+STA - the station link (and Matter) keeps running while the portal is open;
 *   - stores per-device AP credentials (SSID = prefix + MAC suffix, random password);
 *   - applies new station credentials through the CHIP Wi-Fi driver, with rollback when the new
 *     network does not connect in time;
 *   - closes the portal after CONFIG_APP_AP_TIMEOUT_MIN without activity, never while a pairing or
 *     IR learning is in progress.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode; /* wifi_auth_mode_t */
} wifi_scan_entry_t;

typedef enum {
    WIFI_JOB_IDLE = 0,
    WIFI_JOB_CONNECTING,
    WIFI_JOB_CONNECTED,
    WIFI_JOB_FAILED_ROLLED_BACK,
    WIFI_JOB_FAILED,
} wifi_job_state_t;

typedef struct {
    bool sta_configured;
    char sta_ssid[33];
    bool sta_connected;
    int8_t sta_rssi;
    uint8_t sta_channel;
    char sta_ip4[16];
    char sta_ip6[3][48];
    uint8_t sta_ip6_count;
    bool portal_open;
    char ap_ssid[33];
    uint8_t ap_clients;
    uint32_t portal_remaining_s;
    wifi_job_state_t job;
    char job_message[80];
    bool scanning;
    uint8_t scan_count;
    bool subnet_conflict;   /* LAN uses 192.168.4.0/24 too */
} wifi_status_t;

typedef void (*wifi_portal_hook_t)(bool open);

/** Before esp_matter::start(): creates the AP netif and event handlers. */
esp_err_t wifi_config_early_init(void);
/** After esp_matter::start(): AP credentials; opens the portal when no station is configured. */
esp_err_t wifi_config_start(wifi_portal_hook_t hook);

/** Open the portal or, when already open, extend its lifetime. */
esp_err_t wifi_config_portal_open(void);
esp_err_t wifi_config_portal_close(void);
/** Open the portal again after the next restart (restart requested from the portal itself). */
esp_err_t wifi_config_reopen_after_reboot(void);
bool wifi_config_portal_is_open(void);
/** User activity on the portal (page load, button, form): restarts the idle timeout. */
void wifi_config_touch(void);
/** Seconds before the portal closes for inactivity (0 when closed). */
uint32_t wifi_config_portal_remaining_s(void);

esp_err_t wifi_config_scan_start(void);
size_t wifi_config_scan_results(wifi_scan_entry_t *out, size_t max);

/** Save and connect new station credentials (async; see wifi_status_t.job). */
esp_err_t wifi_config_set_sta(const char *ssid, const char *password);

void wifi_config_get_status(wifi_status_t *out);

/** Per-device AP password (only for the console "secrets" command). */
esp_err_t wifi_config_get_ap_password(char *out, size_t len);
/** Store a new AP password (8..63 printable ASCII). Takes effect the next time the portal opens,
 *  so the phone that changes it is not disconnected in the middle of the request. */
esp_err_t wifi_config_set_ap_password(const char *password);

/** True if `ip4` (network byte order) is this device's SoftAP address. */
bool wifi_config_is_ap_address(uint32_t ip4);
/** SoftAP IPv4 address (network byte order), 0 if not available. */
uint32_t wifi_config_ap_ip4(void);
/** True if `ip4` (network byte order) is a client address inside the SoftAP subnet. */
bool wifi_config_is_ap_client(uint32_t ip4);

/* Implemented in wifi_chip_bridge.cpp: runs on the CHIP thread. */
esp_err_t wifi_chip_connect_sta(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
