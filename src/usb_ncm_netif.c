#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "app_config.h"
#include "usb_ncm_netif.h"

static const char *TAG = "usb_ncm";

typedef struct {
    esp_netif_driver_base_t base;
} usb_ncm_driver_t;

static usb_ncm_driver_t s_driver;
static esp_netif_t *s_netif;

// ---- esp_netif -> USB ------------------------------------------------------

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    // tinyusb_net_send_sync copies the frame inside the TinyUSB task, so the
    // lwIP buffer may be released as soon as this returns.
    esp_err_t err = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "tx drop (%s)", esp_err_to_name(err));
    }
    // Never report failure upward: a dropped frame is normal for Ethernet.
    return ESP_OK;
}

static esp_err_t netif_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return netif_transmit(h, buffer, len);
}

static void netif_free_rx_buffer(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t driver_post_attach(esp_netif_t *netif, void *args)
{
    usb_ncm_driver_t *driver = args;
    driver->base.netif = netif;
    const esp_netif_driver_ifconfig_t ifconfig = {
        .handle = driver,
        .transmit = netif_transmit,
        .transmit_wrap = netif_transmit_wrap,
        .driver_free_rx_buffer = netif_free_rx_buffer,
    };
    return esp_netif_set_driver_config(netif, &ifconfig);
}

// ---- USB -> esp_netif ------------------------------------------------------

static esp_err_t usb_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    // The TinyUSB buffer is recycled right after this callback, so copy it.
    // lwIP keeps a reference and returns it via netif_free_rx_buffer().
    if (!s_netif) {
        return ESP_OK;
    }
    void *copy = malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_netif, copy, len, NULL);
}

// ---- public ----------------------------------------------------------------

esp_err_t usb_ncm_netif_start(void)
{
    // MAC addresses: the "host" MAC is what Android's interface will use,
    // the "device" MAC is our lwIP side. Both must differ and be locally administered.
    uint8_t base_mac[6], host_mac[6], dev_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(base_mac, ESP_MAC_EFUSE_FACTORY));
    memcpy(host_mac, base_mac, 6);
    memcpy(dev_mac, base_mac, 6);
    host_mac[0] = (base_mac[0] | 0x02) & 0xFE; host_mac[5] ^= 0x55;
    dev_mac[0]  = (base_mac[0] | 0x02) & 0xFE; dev_mac[5]  ^= 0xAA;

    // --- esp_netif with DHCP server ---
    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = USB_NET_IP_U32;
    ip_info.gw.addr = USB_NET_IP_U32;
    ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .if_key = "USB_NCM",
        .if_desc = "usb",
        .route_prio = 10,   // lower than Wi-Fi STA (100): default route goes out via Wi-Fi
    };
    memcpy(base_cfg.mac, dev_mac, 6);
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "esp_netif_new failed");

    s_driver.base.post_attach = driver_post_attach;
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, &s_driver), TAG, "attach");

    // Offer ourselves as DNS server (dns_server.c relays or hijacks).
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = USB_NET_IP_U32;
    esp_netif_dhcps_stop(s_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "dns");
    uint8_t offer_dns = 0x02; // OFFER_DNS
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer_dns, sizeof(offer_dns)), TAG, "dhcps dns");
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_netif, NULL, 0, NULL);
    // dhcps_stop() above moved the server from INIT to STOPPED, and esp_netif_start()
    // only auto-starts it from INIT, so start it explicitly now that the netif is up.
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_netif), TAG, "dhcps start");

    // NAT everything arriving from the USB side out through Wi-Fi.
    ESP_RETURN_ON_ERROR(esp_netif_napt_enable(s_netif), TAG, "napt");

    // --- TinyUSB ---
    const tinyusb_config_t tusb_cfg = { .external_phy = false };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install");

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_cb,
    };
    memcpy(net_cfg.mac_addr, host_mac, 6);
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg), TAG, "tinyusb net");

    ESP_LOGI(TAG, "USB NCM up, gateway " USB_NET_IP);
    return ESP_OK;
}

esp_netif_t *usb_ncm_netif_get(void)
{
    return s_netif;
}

static void reenum_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGI(TAG, "re-enumerating USB so the host re-validates the network");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1500));
    tud_connect();
    vTaskDelete(NULL);
}

void usb_ncm_reenumerate(uint32_t delay_ms)
{
    xTaskCreate(reenum_task, "usb_reenum", 2560, (void *)(uintptr_t)delay_ms, 5, NULL);
}
