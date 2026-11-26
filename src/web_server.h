#pragma once
#include "esp_err.h"

/** HTTP server on port 80: setup/status page, JSON API and captive-portal redirects. */
esp_err_t web_server_start(void);
