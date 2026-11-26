#pragma once
#include "esp_err.h"

/**
 * DNS on UDP/53 for the USB side.
 *  - portal active: every A query is answered with 192.168.7.1 (captive portal)
 *  - otherwise:     queries are relayed to the Wi-Fi network's DNS server
 */
esp_err_t dns_server_start(void);
