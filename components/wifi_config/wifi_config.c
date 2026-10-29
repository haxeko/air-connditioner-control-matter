#include "wifi_config.h"

#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_config";

#define CHECK_PERIOD_US (5 * 1000 * 1000)
#define AP_PASSWORD_LEN 10

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_portal_open;
static int64_t s_last_activity_us;
static esp_timer_handle_t s_check_timer;
static esp_timer_handle_t s_job_timer;
static wifi_portal_hook_t s_hook;
static char s_ap_ssid[33];
static char s_ap_password[65];
static uint8_t s_ap_clients;
static bool s_scanning;
static wifi_scan_entry_t s_scan[WIFI_SCAN_MAX];
static uint8_t s_scan_count;
static wifi_job_state_t s_job;
static char s_job_msg[80];
static uint8_t s_last_disconnect_reason;
static bool s_started;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void set_job(wifi_job_state_t st, const char *msg)
{
    lock();
    s_job = st;
    strlcpy(s_job_msg, msg ? msg : "", sizeof(s_job_msg));
    unlock();
}

static void status_led(bool on)
{
#if CONFIG_APP_STATUS_LED_GPIO >= 0
    gpio_set_level(CONFIG_APP_STATUS_LED_GPIO, on ? 1 : 0);
#else
    (void)on;
#endif
}

static const char *disconnect_hint(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "không thấy mạng (SSID sai hoặc ngoài vùng phủ)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
        return "sai mật khẩu hoặc router từ chối";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "router đầy kết nối";
    default:
        return "không kết nối được";
    }
}

/* ------------------------------------------------------------------------------------------ */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED:
            lock();
            s_ap_clients++;
            unlock();
            wifi_config_touch();
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            lock();
            if (s_ap_clients) {
                s_ap_clients--;
            }
            unlock();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = data;
            s_last_disconnect_reason = d ? d->reason : 0;
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            if (!s_scanning) {
                break; /* not ours (e.g. Matter NetworkCommissioning scan): leave the results alone */
            }
            uint16_t n = WIFI_SCAN_MAX;
            wifi_ap_record_t *recs = calloc(WIFI_SCAN_MAX, sizeof(wifi_ap_record_t));
            if (recs != NULL && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                lock();
                s_scan_count = 0;
                for (uint16_t i = 0; i < n && s_scan_count < WIFI_SCAN_MAX; i++) {
                    if (recs[i].ssid[0] == 0) {
                        continue; /* hidden network */
                    }
                    wifi_scan_entry_t *e = &s_scan[s_scan_count++];
                    strlcpy(e->ssid, (const char *)recs[i].ssid, sizeof(e->ssid));
                    e->rssi = recs[i].rssi;
                    e->channel = recs[i].primary;
                    e->authmode = recs[i].authmode;
                }
                unlock();
            } else {
                esp_wifi_clear_ap_list();
            }
            free(recs);
            s_scanning = false;
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_job == WIFI_JOB_CONNECTING) {
            esp_timer_stop(s_job_timer);
            cfg_erase_key(CFG_PART_MAIN, CFG_NS_WIFI, "prev");
            set_job(WIFI_JOB_CONNECTED, "Đã kết nối mạng mới");
            app_state_set_busy(APP_BUSY_WIFI_CONNECT, false);
        }
    }
}

static void job_timeout_cb(void *arg)
{
    (void)arg;
    if (s_job != WIFI_JOB_CONNECTING) {
        return;
    }
    wifi_creds_t prev;
    size_t len = sizeof(prev);
    char msg[80];
    if (cfg_get_blob(CFG_PART_MAIN, CFG_NS_WIFI, "prev", &prev, &len) == ESP_OK && len == sizeof(prev) && prev.ssid[0]) {
        snprintf(msg, sizeof(msg), "Mạng mới %s; đã quay lại mạng cũ", disconnect_hint(s_last_disconnect_reason));
        set_job(WIFI_JOB_FAILED_ROLLED_BACK, msg);
        wifi_chip_connect_sta(prev.ssid, prev.password);
        cfg_erase_key(CFG_PART_MAIN, CFG_NS_WIFI, "prev");
        memset(&prev, 0, sizeof(prev));
    } else {
        snprintf(msg, sizeof(msg), "Mạng mới %s", disconnect_hint(s_last_disconnect_reason));
        set_job(WIFI_JOB_FAILED, msg);
    }
    app_state_set_busy(APP_BUSY_WIFI_CONNECT, false);
    app_log_error("wifi", "%s", msg);
}

static void apply_ap_config(void)
{
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_password, sizeof(ap.ap.password));
    ap.ap.channel = CONFIG_APP_AP_CHANNEL;
    ap.ap.max_connection = CONFIG_APP_AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    wifi_config_t cur;
    if (esp_wifi_get_config(WIFI_IF_AP, &cur) == ESP_OK && memcmp(cur.ap.ssid, ap.ap.ssid, sizeof(ap.ap.ssid)) == 0 &&
        memcmp(cur.ap.password, ap.ap.password, sizeof(ap.ap.password)) == 0 && cur.ap.authmode == ap.ap.authmode &&
        cur.ap.max_connection == ap.ap.max_connection) {
        return; /* unchanged: avoid a flash write */
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        app_log_error("wifi", "AP config: %s", esp_err_to_name(err));
    }
}

static void check_timer_cb(void *arg)
{
    (void)arg;
    if (!s_portal_open) {
        return;
    }
    if (app_state_get_busy() != 0) {
        wifi_config_touch(); /* never close in the middle of pairing / learning / connecting */
        return;
    }
    int64_t idle = esp_timer_get_time() - s_last_activity_us;
    if (idle > (int64_t)CONFIG_APP_AP_TIMEOUT_MIN * 60 * 1000000LL) {
        ESP_LOGI(TAG, "portal idle for %d min, closing", CONFIG_APP_AP_TIMEOUT_MIN);
        wifi_config_portal_close();
    }
}

/* ------------------------------------------------------------------------------------------ */

esp_err_t wifi_config_early_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap(); /* once for the lifetime of the firmware */
        if (s_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
#if CONFIG_APP_STATUS_LED_GPIO >= 0
    gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_APP_STATUS_LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    status_led(false);
#endif
    return ESP_OK;
}

static void generate_password(char *out, size_t len)
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const uint32_t n = sizeof(alphabet) - 1;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    for (size_t i = 0; i + 1 < len; i++) {
        uint32_t r;
        do {
            r = esp_random();
        } while (r >= limit);
        out[i] = alphabet[r % n];
    }
    out[len - 1] = 0;
}

esp_err_t wifi_config_start(wifi_portal_hook_t hook)
{
    s_hook = hook;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", CONFIG_APP_AP_SSID_PREFIX, mac[4], mac[5]);

    esp_err_t err = cfg_get_str(CFG_PART_MAIN, CFG_NS_APP, "ap_pass", s_ap_password, sizeof(s_ap_password));
    if (err != ESP_OK || strlen(s_ap_password) < 8) {
        generate_password(s_ap_password, AP_PASSWORD_LEN + 1);
        err = cfg_set_str(CFG_PART_MAIN, CFG_NS_APP, "ap_pass", s_ap_password);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "per-device SoftAP password generated (shown by the console command 'secrets')");
    }

    const esp_timer_create_args_t check_args = { .callback = check_timer_cb, .name = "portal_chk" };
    ESP_ERROR_CHECK(esp_timer_create(&check_args, &s_check_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_check_timer, CHECK_PERIOD_US));
    const esp_timer_create_args_t job_args = { .callback = job_timeout_cb, .name = "wifi_job" };
    ESP_ERROR_CHECK(esp_timer_create(&job_args, &s_job_timer));

    /* Wi-Fi NVS also persists the mode: a power loss while the portal was open must not leave the
     * AP running unnoticed. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    s_started = true;

    wifi_config_t sta;
    bool provisioned = esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK && sta.sta.ssid[0] != 0;
    if (!provisioned) {
        ESP_LOGI(TAG, "no Wi-Fi configured yet: opening the configuration portal");
        return wifi_config_portal_open();
    }
    uint32_t reopen = 0;
    if (cfg_get_u32(CFG_PART_MAIN, CFG_NS_APP, "portal_rb", &reopen) == ESP_OK) {
        cfg_erase_key(CFG_PART_MAIN, CFG_NS_APP, "portal_rb"); /* one shot */
        if (reopen) {
            ESP_LOGI(TAG, "restart requested from the portal: opening it again");
            return wifi_config_portal_open();
        }
    }
    return ESP_OK;
}

esp_err_t wifi_config_reopen_after_reboot(void)
{
    return cfg_set_u32(CFG_PART_MAIN, CFG_NS_APP, "portal_rb", 1);
}

esp_err_t wifi_config_portal_open(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_touch();
    if (s_portal_open) {
        ESP_LOGI(TAG, "portal already open: lifetime extended");
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA); /* station link and Matter stay up */
    if (err != ESP_OK) {
        app_log_error("wifi", "AP+STA mode: %s", esp_err_to_name(err));
        return err;
    }
    apply_ap_config();
    s_portal_open = true;
    status_led(true);
    ESP_LOGI(TAG, "portal open: SSID %s, http://192.168.4.1 (closes after %d min idle)", s_ap_ssid,
             CONFIG_APP_AP_TIMEOUT_MIN);
    if (s_hook) {
        s_hook(true);
    }
    return ESP_OK;
}

esp_err_t wifi_config_portal_close(void)
{
    if (!s_portal_open) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        app_log_error("wifi", "STA mode: %s", esp_err_to_name(err));
        return err;
    }
    s_portal_open = false;
    lock();
    s_ap_clients = 0;
    unlock();
    status_led(false);
    ESP_LOGI(TAG, "portal closed");
    if (s_hook) {
        s_hook(false);
    }
    return ESP_OK;
}

bool wifi_config_portal_is_open(void)
{
    return s_portal_open;
}

void wifi_config_touch(void)
{
    s_last_activity_us = esp_timer_get_time();
}

esp_err_t wifi_config_scan_start(void)
{
    if (s_scanning) {
        return ESP_OK;
    }
    wifi_scan_config_t cfg = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        s_scanning = false;
    }
    return err;
}

size_t wifi_config_scan_results(wifi_scan_entry_t *out, size_t max)
{
    lock();
    size_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(wifi_scan_entry_t));
    unlock();
    return n;
}

esp_err_t wifi_config_set_sta(const char *ssid, const char *password)
{
    size_t sl = ssid ? strlen(ssid) : 0;
    size_t pl = password ? strlen(password) : 0;
    if (sl == 0 || sl > 32 || pl > 63 || (pl > 0 && pl < 8)) {
        return ESP_ERR_INVALID_ARG; /* WPA2 passphrase: 8..63 characters, or empty for an open network */
    }
    if (s_job == WIFI_JOB_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Keep the working credentials for the rollback, also when only the password changes. */
    wifi_config_t cur;
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0] != 0 &&
        (strncmp((const char *)cur.sta.ssid, ssid, sizeof(cur.sta.ssid)) != 0 ||
         strncmp((const char *)cur.sta.password, password ? password : "", sizeof(cur.sta.password)) != 0)) {
        wifi_creds_t prev;
        memset(&prev, 0, sizeof(prev));
        strlcpy(prev.ssid, (const char *)cur.sta.ssid, sizeof(prev.ssid));
        strlcpy(prev.password, (const char *)cur.sta.password, sizeof(prev.password));
        cfg_set_blob(CFG_PART_MAIN, CFG_NS_WIFI, "prev", &prev, sizeof(prev));
        memset(&prev, 0, sizeof(prev));
    }
    set_job(WIFI_JOB_CONNECTING, "Đang kết nối mạng mới...");
    app_state_set_busy(APP_BUSY_WIFI_CONNECT, true);
    esp_err_t err = wifi_chip_connect_sta(ssid, password ? password : "");
    if (err != ESP_OK) {
        set_job(WIFI_JOB_FAILED, "Không gửi được yêu cầu kết nối");
        app_state_set_busy(APP_BUSY_WIFI_CONNECT, false);
        return err;
    }
    esp_timer_stop(s_job_timer);
    esp_timer_start_once(s_job_timer, (uint64_t)CONFIG_APP_WIFI_CONNECT_TIMEOUT_S * 1000000ULL);
    return ESP_OK;
}

void wifi_config_get_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    wifi_config_t sta;
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) == ESP_OK && sta.sta.ssid[0] != 0) {
        out->sta_configured = true;
        strlcpy(out->sta_ssid, (const char *)sta.sta.ssid, sizeof(out->sta_ssid));
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->sta_connected = true;
        out->sta_rssi = ap.rssi;
        out->sta_channel = ap.primary;
    }
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != NULL && out->sta_connected) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(out->sta_ip4, sizeof(out->sta_ip4), IPSTR, IP2STR(&ip.ip));
            out->subnet_conflict = (ip.ip.addr & PP_HTONL(0xFFFFFF00UL)) == PP_HTONL(0xC0A80400UL);
        }
        esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int n = esp_netif_get_all_ip6(sta_netif, ip6);
        for (int i = 0; i < n && out->sta_ip6_count < 3; i++) {
            snprintf(out->sta_ip6[out->sta_ip6_count++], sizeof(out->sta_ip6[0]), IPV6STR, IPV62STR(ip6[i]));
        }
    }
    lock();
    out->portal_open = s_portal_open;
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    out->ap_clients = s_ap_clients;
    out->job = s_job;
    strlcpy(out->job_message, s_job_msg, sizeof(out->job_message));
    out->scanning = s_scanning;
    out->scan_count = s_scan_count;
    unlock();
    out->portal_remaining_s = wifi_config_portal_remaining_s();
}

uint32_t wifi_config_portal_remaining_s(void)
{
    if (!s_portal_open) {
        return 0;
    }
    int64_t idle = esp_timer_get_time() - s_last_activity_us;
    int64_t left = (int64_t)CONFIG_APP_AP_TIMEOUT_MIN * 60 * 1000000LL - idle;
    return left > 0 ? (uint32_t)(left / 1000000LL) : 0;
}

esp_err_t wifi_config_get_ap_password(char *out, size_t len)
{
    if (s_ap_password[0] == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(out, s_ap_password, len);
    return ESP_OK;
}

esp_err_t wifi_config_set_ap_password(const char *password)
{
    size_t l = password ? strlen(password) : 0;
    if (l < 8 || l > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < l; i++) {
        if (password[i] < 0x20 || password[i] > 0x7E) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t err = cfg_set_str(CFG_PART_MAIN, CFG_NS_APP, "ap_pass", password);
    if (err != ESP_OK) {
        return err;
    }
    strlcpy(s_ap_password, password, sizeof(s_ap_password));
    /* applied by apply_ap_config() at the next wifi_config_portal_open() */
    return ESP_OK;
}

uint32_t wifi_config_ap_ip4(void)
{
    if (s_ap_netif == NULL) {
        return 0;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK) {
        return 0;
    }
    return ip.ip.addr;
}

bool wifi_config_is_ap_client(uint32_t ip4)
{
    if (s_ap_netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK || ip.ip.addr == 0 || ip.netmask.addr == 0) {
        return false;
    }
    return ip4 != ip.ip.addr && (ip4 & ip.netmask.addr) == (ip.ip.addr & ip.netmask.addr);
}

bool wifi_config_is_ap_address(uint32_t ip4)
{
    uint32_t ap = wifi_config_ap_ip4();
    return ap != 0 && ap == ip4;
}
