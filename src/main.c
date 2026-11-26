/*
 * ESP32-S3 USB Wi-Fi adapter for Android.
 *
 * The board shows up on the phone as a USB Ethernet adapter (CDC-NCM). Traffic is
 * NATed out through the ESP32's Wi-Fi station. Without Wi-Fi credentials, a captive
 * portal (DNS hijack + HTTP redirect) makes Android open the setup page.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "status_led.h"
#include "wifi_sta.h"
#include "usb_ncm_netif.h"
#include "dns_server.h"
#include "web_server.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_led_init();
    status_led_set(LED_CONNECTING);

    ESP_ERROR_CHECK(wifi_sta_init());

    // With saved credentials, try to get online *before* the phone sees the USB
    // adapter: Android then validates the network once and immediately has internet.
    bool online = wifi_sta_boot_wait(BOOT_CONNECT_TIMEOUT_MS);
    ESP_LOGI(TAG, "boot: %s", online ? "online" : "starting setup portal");

    ESP_ERROR_CHECK(usb_ncm_netif_start());
    ESP_ERROR_CHECK(dns_server_start());
    ESP_ERROR_CHECK(web_server_start());
}
