#include "captive.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "captive";

#define AP_IP "192.168.4.1"

/* ---------------------------------------------------------- HTTP 404 hook */

/*
 * Fires for any request that no route handled. If the Host header is one of ours
 * (the client really meant to talk to this box), return a true 404 so /media and
 * WebDAV behave. Otherwise it's a hijacked connectivity probe (Host is an external
 * domain resolved to us by the DNS hijack) — answer "online".
 */
static esp_err_t captive_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    char host[80] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));

    bool ours = (host[0] == '\0') || strstr(host, AP_IP) || strstr(host, "c3-media");
    if (ours) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGD(TAG, "probe: %s%s", host, req->uri);
    if (strstr(req->uri, "generate_204") || strstr(req->uri, "gen_204")) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>",
        HTTPD_RESP_USE_STRLEN);
}

void captive_register_http(httpd_handle_t server)
{
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_404);
    ESP_LOGI(TAG, "connectivity-probe responder armed");
}

/* ------------------------------------------------------------- DNS hijack */

static int parse_qname(const uint8_t *buf, int len, int off, char *out, int outlen)
{
    int o = 0;
    while (off < len) {
        uint8_t l = buf[off++];
        if (l == 0) {
            break;
        }
        if (l & 0xC0) {
            return -1;
        }
        if (off + l > len) {
            return -1;
        }
        for (int i = 0; i < l; i++) {
            if (o < outlen - 1) {
                out[o++] = buf[off + i];
            }
        }
        off += l;
        if (o < outlen - 1) {
            out[o++] = '.';
        }
    }
    if (o > 0 && out[o - 1] == '.') {
        o--;
    }
    out[o] = '\0';
    return off;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t ip[4] = {192, 168, 4, 1};

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind :53 failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack up on :53 -> %s", AP_IP);

    uint8_t rx[512], tx[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            continue;
        }
        char name[256];
        int off = parse_qname(rx, n, 12, name, sizeof(name));
        if (off < 0 || off + 4 > n) {
            continue;
        }
        uint16_t qtype = (rx[off] << 8) | rx[off + 1];
        int qend = off + 4;
        if (qend > (int)sizeof(tx) - 16) {
            continue;
        }
        memcpy(tx, rx, qend);
        tx[2] = 0x81; tx[3] = 0x80;
        tx[4] = 0; tx[5] = 1;
        int is_a = (qtype == 1);
        tx[6] = 0; tx[7] = is_a ? 1 : 0;
        tx[8] = tx[9] = tx[10] = tx[11] = 0;
        int len = qend;
        if (is_a) {
            uint8_t ans[16] = {
                0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x1E, 0x00, 0x04,
                ip[0], ip[1], ip[2], ip[3],
            };
            memcpy(tx + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(sock, tx, len, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t captive_dns_start(void)
{
    return xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
