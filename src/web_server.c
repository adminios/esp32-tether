#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "app_config.h"
#include "wifi_sta.h"
#include "web_server.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

#define MAX_SCAN_ITEMS 20

static void no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    no_cache(req);
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    no_cache(req);
    // EMBED_TXTFILES appends a NUL terminator; don't send it.
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static const char *state_name(wifi_state_t s)
{
    switch (s) {
    case WIFI_ST_CONNECTING: return "connecting";
    case WIFI_ST_ONLINE:     return "online";
    case WIFI_ST_FAILED:     return "failed";
    default:                 return "idle";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t st;
    wifi_sta_get_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state_name(st.state));
    cJSON_AddBoolToObject(o, "portal", st.portal);
    cJSON_AddBoolToObject(o, "saved", st.has_saved);
    cJSON_AddStringToObject(o, "ssid", st.ssid);
    cJSON_AddStringToObject(o, "ip", st.ip);
    cJSON_AddNumberToObject(o, "rssi", st.rssi);
    cJSON_AddStringToObject(o, "error", st.error);
    return send_json(req, o);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    // static: the HTTP server handles one request at a time, and this keeps the
    // buffer valid even if a scan outlives the wait in wifi_sta_scan().
    static wifi_scan_item_t items[MAX_SCAN_ITEMS];
    uint16_t n = MAX_SCAN_ITEMS;
    esp_err_t err = wifi_sta_scan(items, &n);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    if (err == ESP_OK) {
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", items[i].ssid);
            cJSON_AddNumberToObject(it, "rssi", items[i].rssi);
            cJSON_AddBoolToObject(it, "secure", items[i].secure);
            cJSON_AddItemToArray(arr, it);
        }
    } else {
        cJSON_AddStringToObject(o, "error", esp_err_to_name(err));
    }
    return send_json(req, o);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[256];
    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    cJSON *in = cJSON_Parse(body);
    const cJSON *jssid = cJSON_GetObjectItem(in, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(in, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(jssid)) {
        err = wifi_sta_connect(jssid->valuestring, cJSON_IsString(jpass) ? jpass->valuestring : "");
    }
    cJSON_Delete(in);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    if (err == ESP_ERR_INVALID_SIZE) {
        cJSON_AddStringToObject(o, "error", "Password must be 8-64 characters");
    } else if (err != ESP_OK) {
        cJSON_AddStringToObject(o, "error", "Invalid network name or password");
    }
    return send_json(req, o);
}

static esp_err_t forget_post(httpd_req_t *req)
{
    wifi_sta_forget();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

/**
 * Everything else: captive-portal probes (/generate_204, /hotspot-detect.html, ...)
 * and any other URL. While the portal is active, redirect to the setup page so the
 * OS shows "Sign in to network". When online, answer the probes like the real thing.
 */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    const char *uri = req->uri;
    no_cache(req);
    if (!wifi_sta_portal_active()) {
        if (!strncmp(uri, "/generate_204", 13) || !strncmp(uri, "/gen_204", 8)) {
            httpd_resp_set_status(req, "204 No Content");
            return httpd_resp_send(req, NULL, 0);
        }
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "Not found");
    }
    ESP_LOGD(TAG, "portal redirect for %s", uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PORTAL_URL);
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 10;
    cfg.lru_purge_enable = true;      // Android opens many probe sockets; recycle old ones
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_post },
        { .uri = "/api/forget",  .method = HTTP_POST, .handler = forget_post },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "HTTP server on " PORTAL_URL);
    return ESP_OK;
}
