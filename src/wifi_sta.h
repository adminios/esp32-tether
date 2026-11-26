#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

typedef enum {
    WIFI_ST_IDLE = 0,     // no credentials / not trying
    WIFI_ST_CONNECTING,
    WIFI_ST_ONLINE,
    WIFI_ST_FAILED,       // last user-initiated attempt failed (see error)
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    bool portal;          // setup portal active (DNS hijack + HTTP redirects)
    bool has_saved;       // credentials stored in NVS
    char ssid[33];        // SSID being used / attempted
    char ip[16];          // STA IP when online
    int8_t rssi;
    char error[64];       // last error message for the UI
} wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_item_t;

/** Init Wi-Fi STA + manager task. Starts connecting if credentials are stored. */
esp_err_t wifi_sta_init(void);

/** Block until online or timeout; then turn the portal on if not online. Returns true if online. */
bool wifi_sta_boot_wait(uint32_t timeout_ms);

/** Start a user-initiated connection. Credentials are saved only after it succeeds. */
esp_err_t wifi_sta_connect(const char *ssid, const char *pass);

/** Erase stored credentials, disconnect and re-open the portal. */
esp_err_t wifi_sta_forget(void);

/** Scan for networks (blocks ~2-4 s). Deduplicated by SSID, strongest first. */
esp_err_t wifi_sta_scan(wifi_scan_item_t *items, uint16_t *count);

void wifi_sta_get_status(wifi_status_t *out);
bool wifi_sta_portal_active(void);
esp_netif_t *wifi_sta_netif(void);
