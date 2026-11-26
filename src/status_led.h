#pragma once

typedef enum {
    LED_OFF = 0,
    LED_SETUP,       // blue:   setup portal active, waiting for Wi-Fi details
    LED_CONNECTING,  // yellow: connecting to Wi-Fi
    LED_ONLINE,      // green:  internet available
    LED_ERROR,       // red:    last connection attempt failed
} led_mode_t;

void status_led_init(void);
void status_led_set(led_mode_t mode);
