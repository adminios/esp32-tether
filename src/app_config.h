#pragma once

// IP plan for the USB link (ESP side is the gateway, Android gets an address by DHCP)
#define USB_NET_IP        "192.168.7.1"
#define USB_NET_NETMASK   "255.255.255.0"
#define USB_NET_IP_U32    ESP_IP4TOADDR(192, 168, 7, 1)

#define PORTAL_URL        "http://" USB_NET_IP "/"

// How long to try saved credentials at boot before showing the setup portal
#define BOOT_CONNECT_TIMEOUT_MS   15000
// How long a lost Wi-Fi connection may stay down before the setup portal comes back
#define LOST_LINK_GRACE_MS        30000
// Retry interval for saved credentials while the portal is active
#define BACKGROUND_RETRY_MS       30000
