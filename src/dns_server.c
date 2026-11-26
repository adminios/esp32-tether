#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "app_config.h"
#include "wifi_sta.h"
#include "dns_server.h"

static const char *TAG = "dns";

#define DNS_PORT        53
#define DNS_BUF         1500
#define RELAY_SLOTS     32
#define RELAY_TIMEOUT_MS 5000
#define DNS_HDR_LEN     12

typedef struct {
    bool used;
    uint16_t orig_id;
    uint16_t relay_id;
    struct sockaddr_in client;
    int64_t t_ms;
} relay_slot_t;

static relay_slot_t s_slots[RELAY_SLOTS];
static uint16_t s_next_id;

static uint32_t upstream_dns(void)
{
    esp_netif_dns_info_t d;
    esp_netif_t *sta = wifi_sta_netif();
    if (sta) {
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &d) == ESP_OK && d.ip.u_addr.ip4.addr) {
            return d.ip.u_addr.ip4.addr;
        }
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &d) == ESP_OK && d.ip.u_addr.ip4.addr) {
            return d.ip.u_addr.ip4.addr;
        }
    }
    return ESP_IP4TOADDR(8, 8, 8, 8);
}

/** Build a hijack response in place. Returns response length or -1. */
static int build_hijack(uint8_t *buf, int len)
{
    if (len < DNS_HDR_LEN) {
        return -1;
    }
    uint16_t qdcount = (buf[4] << 8) | buf[5];
    if ((buf[2] & 0x80) || qdcount < 1) {
        return -1;                      // not a query
    }
    // walk the first QNAME
    int p = DNS_HDR_LEN;
    while (p < len && buf[p]) {
        if ((buf[p] & 0xC0) == 0xC0) {  // compression is not expected in a question
            return -1;
        }
        p += buf[p] + 1;
    }
    p += 1;                             // terminating zero
    if (p + 4 > len) {
        return -1;
    }
    uint16_t qtype = (buf[p] << 8) | buf[p + 1];
    uint16_t qclass = (buf[p + 2] << 8) | buf[p + 3];
    p += 4;                             // end of question

    bool answer = (qtype == 1 && qclass == 1);   // A, IN
    buf[2] = 0x80 | (buf[2] & 0x01) | 0x04;      // QR=1, keep RD, AA=1
    buf[3] = 0x80;                               // RA=1, RCODE=0
    buf[4] = 0; buf[5] = 1;                      // QDCOUNT=1
    buf[6] = 0; buf[7] = answer ? 1 : 0;         // ANCOUNT
    buf[8] = buf[9] = buf[10] = buf[11] = 0;     // NSCOUNT, ARCOUNT (drop EDNS)
    if (!answer) {
        return p;
    }
    if (p + 16 > DNS_BUF) {
        return -1;
    }
    const uint8_t rr[] = {
        0xC0, 0x0C,             // name -> question
        0x00, 0x01, 0x00, 0x01, // A, IN
        0x00, 0x00, 0x00, 0x00, // TTL 0: don't let Android cache portal answers
        0x00, 0x04,
    };
    memcpy(buf + p, rr, sizeof(rr));
    p += sizeof(rr);
    uint32_t ip = USB_NET_IP_U32;               // already network byte order
    memcpy(buf + p, &ip, 4);
    return p + 4;
}

static void relay_query(int up_sock, uint8_t *buf, int len, struct sockaddr_in *client)
{
    if (len < DNS_HDR_LEN || !wifi_sta_netif()) {
        return;
    }
    int64_t t = esp_timer_get_time() / 1000;
    relay_slot_t *slot = NULL;
    for (int i = 0; i < RELAY_SLOTS; i++) {
        if (!s_slots[i].used || t - s_slots[i].t_ms > RELAY_TIMEOUT_MS) {
            slot = &s_slots[i];
            break;
        }
    }
    if (!slot) {
        return;                         // table full: drop, client will retry
    }
    slot->used = true;
    slot->orig_id = (buf[0] << 8) | buf[1];
    slot->relay_id = ++s_next_id;
    slot->client = *client;
    slot->t_ms = t;
    buf[0] = slot->relay_id >> 8;
    buf[1] = slot->relay_id & 0xFF;

    struct sockaddr_in up = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = upstream_dns(),
    };
    sendto(up_sock, buf, len, 0, (struct sockaddr *)&up, sizeof(up));
}

static void relay_answer(int srv_sock, uint8_t *buf, int len)
{
    if (len < DNS_HDR_LEN) {
        return;
    }
    uint16_t id = (buf[0] << 8) | buf[1];
    for (int i = 0; i < RELAY_SLOTS; i++) {
        if (s_slots[i].used && s_slots[i].relay_id == id) {
            buf[0] = s_slots[i].orig_id >> 8;
            buf[1] = s_slots[i].orig_id & 0xFF;
            sendto(srv_sock, buf, len, 0, (struct sockaddr *)&s_slots[i].client, sizeof(s_slots[i].client));
            s_slots[i].used = false;
            return;
        }
    }
}

static void dns_task(void *arg)
{
    uint8_t *buf = malloc(DNS_BUF);
    int srv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = USB_NET_IP_U32,   // USB side only: not an open resolver on the Wi-Fi LAN
    };
    if (!buf || srv < 0 || up < 0 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot start DNS server");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS server listening on :53");

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        FD_SET(up, &rfds);
        int maxfd = (srv > up ? srv : up) + 1;
        if (select(maxfd, &rfds, NULL, NULL, NULL) <= 0) {
            continue;
        }
        if (FD_ISSET(srv, &rfds)) {
            struct sockaddr_in client;
            socklen_t clen = sizeof(client);
            int len = recvfrom(srv, buf, DNS_BUF, 0, (struct sockaddr *)&client, &clen);
            if (len > 0) {
                if (wifi_sta_portal_active()) {
                    int rlen = build_hijack(buf, len);
                    if (rlen > 0) {
                        sendto(srv, buf, rlen, 0, (struct sockaddr *)&client, clen);
                    }
                } else {
                    relay_query(up, buf, len, &client);
                }
            }
        }
        if (FD_ISSET(up, &rfds)) {
            int len = recvfrom(up, buf, DNS_BUF, 0, NULL, NULL);
            if (len > 0) {
                relay_answer(srv, buf, len);
            }
        }
    }
}

esp_err_t dns_server_start(void)
{
    return xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
