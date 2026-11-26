#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs.h"
#include "app_config.h"
#include "usb_ncm_netif.h"
#include "status_led.h"
#include "wifi_sta.h"

static const char *TAG = "wifi";

#define NVS_NS            "tether"
#define USER_MAX_FAILS    3
#define USER_TIMEOUT_MS   25000
#define FAST_RETRY_MS     5000
#define MAX_SCAN          20

typedef enum {
    MSG_TICK,
    MSG_EV_DISCONNECTED,
    MSG_EV_GOT_IP,
    MSG_CMD_CONNECT,
    MSG_CMD_FORGET,
    MSG_CMD_SCAN,
    MSG_CMD_BOOT_DONE,
} msg_type_t;

typedef struct {
    msg_type_t type;
    union {
        uint8_t reason;
        struct { char ssid[33]; char pass[65]; } creds;
        struct { wifi_scan_item_t *items; uint16_t *count; esp_err_t *result; SemaphoreHandle_t done; } scan;
    };
} msg_t;

static QueueHandle_t s_queue;
static esp_netif_t *s_sta_netif;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_status_t s_status;          // snapshot shared with other tasks (guarded by s_lock)

// --- manager-task private state ---
static char s_saved_ssid[33], s_saved_pass[65];
static bool s_has_saved;
static bool s_user_attempt;             // trying credentials entered on the page
static char s_try_ssid[33], s_try_pass[65];
static int  s_user_fails;
static int64_t s_attempt_started_ms;
static bool s_connect_outstanding;      // esp_wifi_connect() issued, no result yet
static int64_t s_last_try_ms;
static int64_t s_offline_since_ms;
static bool s_portal;
static bool s_boot_done;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---------------------------------------------------------------- NVS

static void creds_load(void)
{
    nvs_handle_t h;
    s_has_saved = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t l1 = sizeof(s_saved_ssid), l2 = sizeof(s_saved_pass);
    if (nvs_get_str(h, "ssid", s_saved_ssid, &l1) == ESP_OK &&
        nvs_get_str(h, "pass", s_saved_pass, &l2) == ESP_OK && s_saved_ssid[0]) {
        s_has_saved = true;
    }
    nvs_close(h);
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(s_saved_ssid, ssid, sizeof(s_saved_ssid));
    strlcpy(s_saved_pass, pass, sizeof(s_saved_pass));
    s_has_saved = true;
}

static void creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_saved_ssid[0] = s_saved_pass[0] = 0;
    s_has_saved = false;
}

// ---------------------------------------------------------------- helpers

static void status_update(wifi_state_t st, const char *ssid, const char *err)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = st;
    s_status.portal = s_portal;
    s_status.has_saved = s_has_saved;
    if (ssid) {
        strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    }
    if (err) {
        strlcpy(s_status.error, err, sizeof(s_status.error));
    }
    if (st != WIFI_ST_ONLINE) {
        s_status.ip[0] = 0;
    }
    portEXIT_CRITICAL(&s_lock);

    if (s_portal && st != WIFI_ST_ONLINE) {
        status_led_set(st == WIFI_ST_CONNECTING ? LED_CONNECTING : (st == WIFI_ST_FAILED ? LED_ERROR : LED_SETUP));
    } else {
        status_led_set(st == WIFI_ST_ONLINE ? LED_ONLINE : LED_CONNECTING);
    }
}

static wifi_state_t cur_state(void)
{
    portENTER_CRITICAL(&s_lock);
    wifi_state_t st = s_status.state;
    portEXIT_CRITICAL(&s_lock);
    return st;
}

static void set_portal(bool on)
{
    if (s_portal == on) {
        return;
    }
    s_portal = on;
    ESP_LOGI(TAG, "setup portal %s", on ? "ON" : "OFF");
    portENTER_CRITICAL(&s_lock);
    s_status.portal = on;
    portEXIT_CRITICAL(&s_lock);
    status_update(cur_state(), NULL, NULL);
    // Make Android drop the network and validate it again, so it notices
    // the portal (or that the portal is gone and internet works).
    if (s_boot_done && usb_ncm_netif_get()) {
        usb_ncm_reenumerate(on ? 200 : 3000);   // give the page time to show "connected"
    }
}

static void start_connect(const char *ssid, const char *pass, bool clear_error)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    if (s_connect_outstanding || cur_state() == WIFI_ST_ONLINE) {
        esp_wifi_disconnect();
    }
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    s_connect_outstanding = (esp_wifi_connect() == ESP_OK);
    s_last_try_ms = now_ms();
    status_update(WIFI_ST_CONNECTING, ssid, clear_error ? "" : NULL);
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Network not found or out of range";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Wrong password";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "Access point refused the connection";
    default:
        return NULL;
    }
}

static void user_attempt_failed(const char *why)
{
    ESP_LOGW(TAG, "connection to \"%s\" failed: %s", s_try_ssid, why);
    s_user_attempt = false;
    s_connect_outstanding = false;
    esp_wifi_disconnect();
    status_update(WIFI_ST_FAILED, s_try_ssid, why);
    s_last_try_ms = now_ms();   // keep the error visible for a while before retrying saved creds
    if (s_offline_since_ms == 0) {
        s_offline_since_ms = now_ms();
    }
    // Saved credentials (if any) are retried by the tick handler.
}

// ---------------------------------------------------------------- manager task

static void handle_disconnected(uint8_t reason)
{
    bool was_online = cur_state() == WIFI_ST_ONLINE;
    s_connect_outstanding = false;
    if (reason == WIFI_REASON_ASSOC_LEAVE) {
        return;     // caused by our own esp_wifi_disconnect()
    }
    ESP_LOGI(TAG, "disconnected, reason %d", reason);

    if (s_user_attempt) {
        const char *txt = reason_text(reason);
        bool hard = (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                     reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
        if (++s_user_fails >= USER_MAX_FAILS || (hard && s_user_fails >= 2)) {
            char buf[64];
            if (!txt) {
                snprintf(buf, sizeof(buf), "Connection failed (reason %d)", reason);
                txt = buf;
            }
            user_attempt_failed(txt);
        } else {
            s_connect_outstanding = (esp_wifi_connect() == ESP_OK);
        }
        return;
    }

    if (was_online || s_offline_since_ms == 0) {
        s_offline_since_ms = now_ms();
    }
    status_update(s_has_saved ? WIFI_ST_CONNECTING : WIFI_ST_IDLE, NULL, NULL);
}

static void handle_got_ip(void)
{
    s_connect_outstanding = false;
    s_offline_since_ms = 0;
    if (s_user_attempt) {
        s_user_attempt = false;
        creds_save(s_try_ssid, s_try_pass);
        ESP_LOGI(TAG, "credentials for \"%s\" saved", s_try_ssid);
    }
    esp_netif_ip_info_t ip;
    wifi_ap_record_t ap = { 0 };
    esp_netif_get_ip_info(s_sta_netif, &ip);
    esp_wifi_sta_get_ap_info(&ap);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ip.ip));
    s_status.rssi = ap.rssi;
    strlcpy(s_status.ssid, (char *)ap.ssid, sizeof(s_status.ssid));
    portEXIT_CRITICAL(&s_lock);
    status_update(WIFI_ST_ONLINE, NULL, "");
    set_portal(false);
}

static void handle_tick(void)
{
    int64_t t = now_ms();
    wifi_state_t st = cur_state();

    if (st == WIFI_ST_ONLINE) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_status.rssi = ap.rssi;
            portEXIT_CRITICAL(&s_lock);
        }
        return;
    }

    if (s_user_attempt) {
        if (t - s_attempt_started_ms > USER_TIMEOUT_MS) {
            user_attempt_failed("Timed out");
        }
        return;
    }

    // Lost the link (or never had it): open the portal after the grace period.
    if (s_boot_done && !s_portal && s_offline_since_ms && t - s_offline_since_ms > LOST_LINK_GRACE_MS) {
        set_portal(true);
    }

    // Keep retrying saved credentials: quickly at first, slowly once the portal is open
    // (slow so that scans from the setup page are not constantly interrupted).
    if (s_has_saved && !s_connect_outstanding) {
        int64_t interval = s_portal ? BACKGROUND_RETRY_MS : FAST_RETRY_MS;
        if (t - s_last_try_ms > interval) {
            start_connect(s_saved_ssid, s_saved_pass, false);
        }
    }
}

static void handle_scan(msg_t *m)
{
    if (s_connect_outstanding && cur_state() != WIFI_ST_ONLINE && !s_user_attempt) {
        esp_wifi_disconnect();          // background retry in progress: abort it for the scan
        s_connect_outstanding = false;
        s_last_try_ms = now_ms();
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        uint16_t n = MAX_SCAN;
        wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof(*recs));
        if (!recs) {
            err = ESP_ERR_NO_MEM;
        } else {
            esp_wifi_scan_get_ap_records(&n, recs);   // sorted by RSSI already
            uint16_t out = 0;
            for (int i = 0; i < n && out < *m->scan.count; i++) {
                if (!recs[i].ssid[0]) {
                    continue;
                }
                bool dup = false;
                for (int j = 0; j < out; j++) {
                    if (!strcmp(m->scan.items[j].ssid, (char *)recs[i].ssid)) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                strlcpy(m->scan.items[out].ssid, (char *)recs[i].ssid, 33);
                m->scan.items[out].rssi = recs[i].rssi;
                m->scan.items[out].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                out++;
            }
            *m->scan.count = out;
            free(recs);
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    *m->scan.result = err;
    xSemaphoreGive(m->scan.done);
}

static void manager_task(void *arg)
{
    msg_t m;
    if (s_has_saved) {
        start_connect(s_saved_ssid, s_saved_pass, false);
    } else {
        status_update(WIFI_ST_IDLE, "", "");
    }
    for (;;) {
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
            m.type = MSG_TICK;
        }
        switch (m.type) {
        case MSG_TICK:
            handle_tick();
            break;
        case MSG_EV_DISCONNECTED:
            handle_disconnected(m.reason);
            break;
        case MSG_EV_GOT_IP:
            handle_got_ip();
            break;
        case MSG_CMD_CONNECT:
            strlcpy(s_try_ssid, m.creds.ssid, sizeof(s_try_ssid));
            strlcpy(s_try_pass, m.creds.pass, sizeof(s_try_pass));
            s_user_attempt = true;
            s_user_fails = 0;
            s_attempt_started_ms = now_ms();
            start_connect(s_try_ssid, s_try_pass, true);
            break;
        case MSG_CMD_FORGET:
            creds_erase();
            s_user_attempt = false;
            s_connect_outstanding = false;
            esp_wifi_disconnect();
            s_offline_since_ms = now_ms();
            status_update(WIFI_ST_IDLE, "", "");
            set_portal(true);
            break;
        case MSG_CMD_SCAN:
            handle_scan(&m);
            break;
        case MSG_CMD_BOOT_DONE:
            if (cur_state() != WIFI_ST_ONLINE) {
                if (s_offline_since_ms == 0) {
                    s_offline_since_ms = now_ms();
                }
                set_portal(true);
            }
            s_boot_done = true;
            break;
        }
    }
}

// ---------------------------------------------------------------- events

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    msg_t m = { 0 };
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        m.type = MSG_EV_DISCONNECTED;
        m.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        m.type = MSG_EV_GOT_IP;
    } else {
        return;
    }
    xQueueSend(s_queue, &m, portMAX_DELAY);
}

// ---------------------------------------------------------------- public

esp_err_t wifi_sta_init(void)
{
    s_queue = xQueueCreate(8, sizeof(msg_t));
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    esp_wifi_set_ps(WIFI_PS_NONE);      // lowest latency; we are USB-powered anyway

    creds_load();
    portENTER_CRITICAL(&s_lock);
    s_status.has_saved = s_has_saved;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "saved credentials: %s", s_has_saved ? s_saved_ssid : "(none)");

    xTaskCreate(manager_task, "wifi_mgr", 4096, NULL, 6, NULL);
    return ESP_OK;
}

bool wifi_sta_boot_wait(uint32_t timeout_ms)
{
    int64_t end = now_ms() + timeout_ms;
    bool online = false;
    while (s_has_saved && now_ms() < end) {
        if (cur_state() == WIFI_ST_ONLINE) {
            online = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    msg_t m = { .type = MSG_CMD_BOOT_DONE };
    xQueueSend(s_queue, &m, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(50));      // let the manager settle the portal flag
    return online;
}

esp_err_t wifi_sta_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass && pass[0] && strlen(pass) < 8) {
        return ESP_ERR_INVALID_SIZE;    // WPA needs 8..64
    }
    msg_t m = { .type = MSG_CMD_CONNECT };
    strlcpy(m.creds.ssid, ssid, sizeof(m.creds.ssid));
    strlcpy(m.creds.pass, pass ? pass : "", sizeof(m.creds.pass));
    // Mark as connecting right away so the page's first poll doesn't see stale state.
    portENTER_CRITICAL(&s_lock);
    s_status.state = WIFI_ST_CONNECTING;
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    s_status.error[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    return xQueueSend(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_forget(void)
{
    msg_t m = { .type = MSG_CMD_FORGET };
    return xQueueSend(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_scan(wifi_scan_item_t *items, uint16_t *count)
{
    esp_err_t result = ESP_FAIL;
    msg_t m = { .type = MSG_CMD_SCAN };
    m.scan.items = items;
    m.scan.count = count;
    m.scan.result = &result;
    m.scan.done = xSemaphoreCreateBinary();
    if (!m.scan.done) {
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_queue, &m, pdMS_TO_TICKS(1000)) != pdTRUE ||
        xSemaphoreTake(m.scan.done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        // The manager may still write into items later; caller's buffer must outlive that.
        // We avoid that by never timing out in practice (scan takes < 5 s).
        result = ESP_ERR_TIMEOUT;
    } else {
        vSemaphoreDelete(m.scan.done);
    }
    return result;
}

void wifi_sta_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool wifi_sta_portal_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool p = s_status.portal;
    portEXIT_CRITICAL(&s_lock);
    return p;
}

esp_netif_t *wifi_sta_netif(void)
{
    return s_sta_netif;
}
