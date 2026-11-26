idf_component_register(
    SRCS "main.c" "usb_ncm_netif.c" "wifi_sta.c" "dns_server.c" "web_server.c" "status_led.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "../web/index.html"
    PRIV_REQUIRES nvs_flash esp_netif esp_wifi esp_event esp_http_server esp_timer json lwip
)
