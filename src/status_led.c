#include "esp_log.h"
#include "led_strip.h"
#include "status_led.h"

// Waveshare ESP32-S3-Zero: one WS2812 on GPIO21
#define LED_GPIO        21
#define LED_LEVEL       16      // 0..255, the LED is very bright

static led_strip_handle_t s_strip;

void status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGW("led", "status LED unavailable");
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);
}

void status_led_set(led_mode_t mode)
{
    if (!s_strip) {
        return;
    }
    uint32_t r = 0, g = 0, b = 0;
    switch (mode) {
    case LED_SETUP:      b = LED_LEVEL; break;
    case LED_CONNECTING: r = LED_LEVEL; g = LED_LEVEL; break;
    case LED_ONLINE:     g = LED_LEVEL; break;
    case LED_ERROR:      r = LED_LEVEL; break;
    default: break;
    }
    // The S3-Zero's LED takes RGB byte order, while the driver sends GRB:
    // pass red and green swapped so the colours come out right.
    led_strip_set_pixel(s_strip, 0, g, r, b);
    led_strip_refresh(s_strip);
}
