#pragma once
#include "esp_err.h"
#include "esp_netif.h"

/** Start TinyUSB (CDC-NCM) and create an esp_netif with DHCP server on 192.168.7.1. */
esp_err_t usb_ncm_netif_start(void);

/** The esp_netif for the USB link (valid after start). */
esp_netif_t *usb_ncm_netif_get(void);

/**
 * Drop off the bus and re-attach after a short pause, so the host re-enumerates,
 * renews DHCP and re-runs its network validation (captive portal detection).
 * Non-blocking: runs on a short-lived task.
 */
void usb_ncm_reenumerate(uint32_t delay_ms);
