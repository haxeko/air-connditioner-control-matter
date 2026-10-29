#pragma once

/* Internal interface between the HTTP server (web_server.c) and the API handlers (web_api.c). */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "web_util.h"

typedef struct {
    httpd_req_t *req;
    const char *body;  /* POST: form body (NUL terminated, validated length); GET: "" */
    const char *query; /* URL query string without '?', "" when absent */
    wj_t *w;           /* response object, {"ok":true already written; handlers add fields */
    int status;        /* HTTP status, 200 unless api_fail() was called */
    char error[160];   /* Vietnamese message shown by the UI */
} api_ctx_t;

typedef void (*api_fn_t)(api_ctx_t *c);

typedef struct {
    const char *path; /* below /api/ */
    api_fn_t get;
    api_fn_t post;
} api_route_t;

extern const api_route_t g_api_routes[];
extern const size_t g_api_route_count;

void api_fail(api_ctx_t *c, int status, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Implemented by web_server.c ------------------------------------------------------------- */

const char *web_session_token(void);

typedef enum {
    WEB_DEFER_CLOSE_PORTAL = 0, /* after the HTTP response has left                    */
    WEB_DEFER_REBOOT,
    WEB_DEFER_UNBIND_NODE,      /* sensor RemoveFabric succeeded: forget its binding    */
} web_defer_action_t;

/** Run an action later in the portal worker task (never in the HTTP or CHIP task). */
esp_err_t web_defer(web_defer_action_t action, uint64_t arg, uint32_t delay_ms);
