#include "web_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_state.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "matter_commissioner.h"
#include "remote_temperature.h"
#include "sdkconfig.h"
#include "web_api.h"
#include "web_util.h"
#include "wifi_config.h"

static const char *TAG = "web";

#define BODY_MAX 2048
#define RESP_MAX (16 * 1024)

/* Embedded, gzip-compressed UI (see CMakeLists.txt). */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[] asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[] asm("_binary_app_css_gz_end");

static httpd_handle_t s_server;
static char s_token[33];
static QueueHandle_t s_defer_queue;

typedef struct {
    web_defer_action_t action;
    uint64_t arg;
    uint32_t delay_ms;
} defer_msg_t;

/* ------------------------------------------------------------------------------------------ */
/* Access control                                                                               */
/* ------------------------------------------------------------------------------------------ */

/** IPv4 address of a connected socket's local (or peer) end; IPv4-mapped IPv6 on the dual-stack listener. */
static bool socket_ip4(int sockfd, bool peer, uint32_t *out)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int r = peer ? getpeername(sockfd, (struct sockaddr *)&ss, &len) : getsockname(sockfd, (struct sockaddr *)&ss, &len);
    if (r != 0) {
        return false;
    }
    if (ss.ss_family == AF_INET) {
        *out = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
        return true;
    }
#if CONFIG_LWIP_IPV6
    if (ss.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const uint8_t k_mapped_prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (memcmp(b, k_mapped_prefix, sizeof(k_mapped_prefix)) == 0) {
            memcpy(out, b + 12, 4);
            return true;
        }
    }
#endif
    return false;
}

/**
 * Connection arrived on the SoftAP interface while the portal is open: addressed to the AP address,
 * from a client of the AP subnet. Packets for the AP address that arrive on the station interface
 * are already dropped in lwIP (strong host hook, components/wifi_config/ip4_guard.c).
 */
static bool socket_on_portal(int sockfd)
{
    if (!wifi_config_portal_is_open()) {
        return false;
    }
    uint32_t local = 0;
    uint32_t peer = 0;
    return socket_ip4(sockfd, false, &local) && wifi_config_is_ap_address(local) && socket_ip4(sockfd, true, &peer) &&
        wifi_config_is_ap_client(peer);
}

static esp_err_t on_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (!socket_on_portal(sockfd)) {
        ESP_LOGW(TAG, "connection refused: not on the configuration AP");
        return ESP_FAIL; /* the server closes the socket */
    }
    return ESP_OK;
}

/** Copy a request header. False when absent; an over-long value is returned as "" (never matches). */
static bool header(httpd_req_t *req, const char *name, char *out, size_t len)
{
    out[0] = 0;
    size_t n = httpd_req_get_hdr_value_len(req, name);
    if (n == 0) {
        return false;
    }
    if (n < len && httpd_req_get_hdr_value_str(req, name, out, len) != ESP_OK) {
        out[0] = 0;
    }
    return true;
}

static bool host_is_portal(const char *host, const char *ap_ip)
{
    size_t l = strlen(ap_ip);
    if (strncmp(host, ap_ip, l) != 0) {
        return false;
    }
    return host[l] == 0 || strcmp(host + l, ":80") == 0;
}

/**
 * Common checks. Returns 0 when allowed, otherwise the HTTP status to answer with and a
 * message in *why.
 */
static int check_request(httpd_req_t *req, bool need_token, bool is_post, const char **why)
{
    if (!socket_on_portal(httpd_req_to_sockfd(req))) {
        *why = "Chỉ truy cập được qua Wi-Fi cấu hình của thiết bị khi đang mở cấu hình";
        return 403;
    }
    struct in_addr ap = { .s_addr = wifi_config_ap_ip4() };
    char ap_ip[16];
    inet_ntoa_r(ap, ap_ip, sizeof(ap_ip));

    char buf[96];
    if (!header(req, "Host", buf, sizeof(buf)) || !host_is_portal(buf, ap_ip)) {
        *why = "Sai địa chỉ: hãy mở http://192.168.4.1";
        return 403;
    }
    if (header(req, "Origin", buf, sizeof(buf))) {
        if (strncmp(buf, "http://", 7) != 0 || !host_is_portal(buf + 7, ap_ip)) {
            *why = "Yêu cầu chéo nguồn bị từ chối";
            return 403;
        }
    }
    if (header(req, "Sec-Fetch-Site", buf, sizeof(buf))) {
        if (strcmp(buf, "same-origin") != 0 && strcmp(buf, "none") != 0) {
            *why = "Yêu cầu chéo nguồn bị từ chối";
            return 403;
        }
    }
    if (need_token) {
        char tok[40];
        if (!header(req, "X-CSRF-Token", tok, sizeof(tok)) || !secure_str_equal(tok, s_token)) {
            *why = "Phiên không hợp lệ: hãy tải lại trang";
            return 403;
        }
    }
    if (is_post) {
        if (!header(req, "Content-Type", buf, sizeof(buf)) ||
            strncasecmp(buf, "application/x-www-form-urlencoded", 33) != 0) {
            *why = "Content-Type không được hỗ trợ";
            return 400;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Responses                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static const char *status_line(int status)
{
    switch (status) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 409:
        return "409 Conflict";
    case 413:
        return "413 Payload Too Large";
    case 503:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                       "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
}

static esp_err_t send_error(httpd_req_t *req, int status, const char *msg)
{
    char buf[512];
    wj_t w;
    wj_init(&w, buf, sizeof(buf));
    wj_obj_begin(&w, NULL);
    wj_bool(&w, "ok", false);
    wj_str(&w, "error", msg);
    wj_obj_end(&w);
    const char *json = wj_finish(&w);
    common_headers(req);
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json ? json : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

void api_fail(api_ctx_t *c, int status, const char *fmt, ...)
{
    c->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, ap);
    va_end(ap);
}

const char *web_session_token(void)
{
    return s_token;
}

/* ------------------------------------------------------------------------------------------ */
/* Handlers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static char *read_body(httpd_req_t *req, int *status)
{
    size_t len = req->content_len;
    if (len > BODY_MAX) {
        *status = 413;
        return NULL;
    }
    char *b = malloc(len + 1);
    if (b == NULL) {
        *status = 503;
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, b + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) {
            continue;
        }
        if (r <= 0) {
            free(b);
            *status = 400;
            return NULL;
        }
        got += (size_t)r;
    }
    b[len] = 0;
    if (strlen(b) != len) {
        free(b); /* embedded NUL */
        *status = 400;
        return NULL;
    }
    return b;
}

static esp_err_t api_handler(httpd_req_t *req)
{
    const bool is_post = req->method == HTTP_POST;
    const char *path = req->uri + 5; /* after "/api/" */
    size_t plen = strcspn(path, "?");
    const char *query = path[plen] == '?' ? path + plen + 1 : "";

    const api_route_t *route = NULL;
    for (size_t i = 0; i < g_api_route_count; i++) {
        if (strlen(g_api_routes[i].path) == plen && memcmp(g_api_routes[i].path, path, plen) == 0) {
            route = &g_api_routes[i];
            break;
        }
    }
    const char *why = NULL;
    int deny = check_request(req, route == NULL || strcmp(route->path, "session") != 0, is_post, &why);
    if (deny != 0) {
        return send_error(req, deny, why);
    }
    if (route == NULL) {
        return send_error(req, 404, "API không tồn tại");
    }
    api_fn_t fn = is_post ? route->post : route->get;
    if (fn == NULL) {
        return send_error(req, 405, "Phương thức không được phép");
    }
    /* Background polling of status pages is not activity, otherwise an open browser tab would
     * keep the SoftAP up forever. POSTs and explicit user navigation (X-User-Action) are. */
    char act[4];
    if (is_post || header(req, "X-User-Action", act, sizeof(act))) {
        wifi_config_touch();
    }

    char *body = NULL;
    if (is_post) {
        int st = 0;
        body = read_body(req, &st);
        if (body == NULL) {
            return send_error(req, st, "Nội dung yêu cầu không hợp lệ hoặc quá lớn");
        }
    }
    char *out = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        out = malloc(RESP_MAX);
    }
    if (out == NULL) {
        free(body);
        return send_error(req, 503, "Hết bộ nhớ");
    }
    wj_t w;
    wj_init(&w, out, RESP_MAX);
    wj_obj_begin(&w, NULL);
    wj_bool(&w, "ok", true);
    wj_uint(&w, "portal_left_s", wifi_config_portal_remaining_s());
    api_ctx_t c = { .req = req, .body = body ? body : "", .query = query, .w = &w, .status = 200 };
    fn(&c);
    wj_obj_end(&w);

    esp_err_t err;
    const char *json = wj_finish(&w);
    if (c.status != 200) {
        err = send_error(req, c.status, c.error[0] ? c.error : "Lỗi");
    } else if (json == NULL) {
        err = send_error(req, 500, "Phản hồi quá lớn");
    } else {
        common_headers(req);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }
    if (body) {
        memset(body, 0, strlen(body)); /* may contain a Wi-Fi password or a setup code */
        free(body);
    }
    free(out);
    return err;
}

static esp_err_t static_handler(httpd_req_t *req)
{
    const char *why = NULL;
    int deny = check_request(req, false, false, &why);
    if (deny != 0) {
        httpd_resp_set_status(req, status_line(deny));
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, why, HTTPD_RESP_USE_STRLEN);
    }
    const uint8_t *start;
    const uint8_t *end;
    const char *type;
    size_t plen = strcspn(req->uri, "?");
    if ((plen == 1 && req->uri[0] == '/') || (plen == 11 && strncmp(req->uri, "/index.html", 11) == 0)) {
        start = index_html_gz_start;
        end = index_html_gz_end;
        type = "text/html; charset=utf-8";
    } else if (plen == 7 && strncmp(req->uri, "/app.js", 7) == 0) {
        start = app_js_gz_start;
        end = app_js_gz_end;
        type = "text/javascript; charset=utf-8";
    } else if (plen == 8 && strncmp(req->uri, "/app.css", 8) == 0) {
        start = app_css_gz_start;
        end = app_css_gz_end;
        type = "text/css; charset=utf-8";
    } else {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "Không tìm thấy", HTTPD_RESP_USE_STRLEN);
    }
    wifi_config_touch();
    common_headers(req);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)start, end - start);
}

/* ------------------------------------------------------------------------------------------ */
/* Deferred actions                                                                             */
/* ------------------------------------------------------------------------------------------ */

static void defer_task(void *arg)
{
    (void)arg;
    defer_msg_t m;
    for (;;) {
        if (xQueueReceive(s_defer_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (m.delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(m.delay_ms));
        }
        switch (m.action) {
        case WEB_DEFER_CLOSE_PORTAL:
            wifi_config_portal_close();
            break;
        case WEB_DEFER_REBOOT:
            ESP_LOGW(TAG, "restart requested from the configuration portal");
            wifi_config_reopen_after_reboot();
            esp_restart();
            break;
        case WEB_DEFER_UNBIND_NODE: {
            sensor_binding_t b;
            if (remote_temperature_get_binding(&b) && b.node_id == m.arg) {
                esp_err_t err = remote_temperature_unbind();
                if (err != ESP_OK) {
                    app_log_error("web", "unbind after RemoveFabric: %s", esp_err_to_name(err));
                }
            }
            break;
        }
        }
    }
}

esp_err_t web_defer(web_defer_action_t action, uint64_t arg, uint32_t delay_ms)
{
    if (s_defer_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    defer_msg_t m = { .action = action, .arg = arg, .delay_ms = delay_ms };
    return xQueueSend(s_defer_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

/* CHIP thread: must not block. */
static void on_sensor_removed(uint64_t node_id, bool ok)
{
    if (ok) {
        web_defer(WEB_DEFER_UNBIND_NODE, node_id, 0);
    }
}

/* ------------------------------------------------------------------------------------------ */

esp_err_t web_config_start(void)
{
    if (s_server != NULL) {
        return ESP_OK; /* started once for the lifetime of the firmware */
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (size_t i = 0; i < sizeof(rnd); i++) {
        snprintf(&s_token[i * 2], 3, "%02x", rnd[i]);
    }

    if (s_defer_queue == NULL) {
        s_defer_queue = xQueueCreate(6, sizeof(defer_msg_t));
        if (s_defer_queue == NULL || xTaskCreate(defer_task, "web_defer", 4096, NULL, 4, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    matter_commissioner_set_removed_cb(on_sensor_removed);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 4;
    cfg.max_resp_headers = 10;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.open_fn = on_open;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        app_log_error("web", "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t api_get = { .uri = "/api/*", .method = HTTP_GET, .handler = api_handler };
    const httpd_uri_t api_post = { .uri = "/api/*", .method = HTTP_POST, .handler = api_handler };
    const httpd_uri_t files = { .uri = "/*", .method = HTTP_GET, .handler = static_handler };
    httpd_register_uri_handler(s_server, &api_get);
    httpd_register_uri_handler(s_server, &api_post);
    httpd_register_uri_handler(s_server, &files);
    ESP_LOGI(TAG, "configuration web server started (SoftAP interface only)");
    return ESP_OK;
}

void web_config_portal_hook(bool open)
{
    if (open) {
        esp_err_t err = web_config_start();
        if (err != ESP_OK) {
            app_log_error("web", "web server: %s", esp_err_to_name(err));
        }
    }
}
