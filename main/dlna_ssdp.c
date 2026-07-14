#include "dlna_ssdp.h"
#include "dlna_upnp.h"
#include "wifi.h"
#include "sdkconfig.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "ssdp";

#define SSDP_MCAST_ADDR "239.255.255.250"
#define SSDP_PORT       1900
#define SSDP_MAX_AGE    1800
#define ALIVE_INTERVAL_US (30 * 1000000LL)
#define SSDP_SERVER_HDR "FreeRTOS/1.0 UPnP/1.1 C3MediaServer/1.0"

#define UUID_STR "uuid:" CONFIG_DLNA_UUID

/* One advertised discovery target: its NT/ST value and the USN suffix appended
 * after the UUID. */
typedef struct {
    const char *nt;
    const char *usn_suffix;
} ssdp_target_t;

static const ssdp_target_t TARGETS[] = {
    { "upnp:rootdevice",                                  "::upnp:rootdevice" },
    { UUID_STR,                                           "" },
    { "urn:schemas-upnp-org:device:MediaServer:1",        "::urn:schemas-upnp-org:device:MediaServer:1" },
    { "urn:schemas-upnp-org:service:ContentDirectory:1",  "::urn:schemas-upnp-org:service:ContentDirectory:1" },
    { "urn:schemas-upnp-org:service:ConnectionManager:1", "::urn:schemas-upnp-org:service:ConnectionManager:1" },
};
#define NUM_TARGETS (sizeof(TARGETS) / sizeof(TARGETS[0]))

static int s_sock = -1;
static struct sockaddr_in s_mcast_dst;

/* Case-insensitive extraction of an SSDP header value (e.g. "ST"). Trims spaces.
 * Returns true if found. */
static bool ssdp_header(const char *msg, const char *name, char *out, size_t out_len)
{
    size_t name_len = strlen(name);
    const char *line = msg;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
        if (line_len > name_len && strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *v = line + name_len + 1;
            const char *vend = line + line_len;
            while (v < vend && (*v == ' ' || *v == '\t')) {
                v++;
            }
            while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) {
                vend--;
            }
            size_t n = (size_t)(vend - v);
            if (n >= out_len) {
                n = out_len - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        line = eol ? eol + 2 : NULL;
    }
    return false;
}

static void send_msearch_reply(const struct sockaddr_in *to, const ssdp_target_t *t)
{
    char msg[512];
    int n = snprintf(msg, sizeof(msg),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=%d\r\n"
        "EXT:\r\n"
        "LOCATION: %s\r\n"
        "SERVER: %s\r\n"
        "ST: %s\r\n"
        "USN: %s%s\r\n"
        "CONTENT-LENGTH: 0\r\n"
        "\r\n",
        SSDP_MAX_AGE, dlna_upnp_location(), SSDP_SERVER_HDR,
        t->nt, UUID_STR, t->usn_suffix);
    sendto(s_sock, msg, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static void send_notify(const ssdp_target_t *t, bool alive)
{
    char msg[512];
    int n;
    if (alive) {
        n = snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "CACHE-CONTROL: max-age=%d\r\n"
            "LOCATION: %s\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:alive\r\n"
            "SERVER: %s\r\n"
            "USN: %s%s\r\n"
            "\r\n",
            SSDP_MCAST_ADDR, SSDP_PORT, SSDP_MAX_AGE, dlna_upnp_location(),
            t->nt, SSDP_SERVER_HDR, UUID_STR, t->usn_suffix);
    } else {
        n = snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:byebye\r\n"
            "USN: %s%s\r\n"
            "\r\n",
            SSDP_MCAST_ADDR, SSDP_PORT, t->nt, UUID_STR, t->usn_suffix);
    }
    sendto(s_sock, msg, n, 0, (const struct sockaddr *)&s_mcast_dst, sizeof(s_mcast_dst));
}

static void announce_all(bool alive)
{
    for (size_t i = 0; i < NUM_TARGETS; i++) {
        send_notify(&TARGETS[i], alive);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void handle_msearch(const char *msg, const struct sockaddr_in *from)
{
    char st[128];
    if (!ssdp_header(msg, "ST", st, sizeof(st))) {
        return;
    }
    bool all = (strcmp(st, "ssdp:all") == 0);
    for (size_t i = 0; i < NUM_TARGETS; i++) {
        if (all || strcmp(st, TARGETS[i].nt) == 0) {
            send_msearch_reply(from, &TARGETS[i]);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void ssdp_task(void *arg)
{
    (void)arg;

    char ip[16] = "192.168.4.1";
    wifi_get_ip(ip, sizeof(ip));

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(s_sock);
        vTaskDelete(NULL);
        return;
    }

    /* Join the SSDP multicast group on the active interface (STA or AP). */
    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MCAST_ADDR);
    mreq.imr_interface.s_addr = inet_addr(ip);
    if (setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGW(TAG, "IP_ADD_MEMBERSHIP failed (continuing)");
    }

    /* Send outgoing multicast out the active interface, small TTL. */
    struct in_addr if_addr = { .s_addr = inet_addr(ip) };
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr));
    uint8_t ttl = 4;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    /* 1s receive timeout so we can also drive periodic alive announcements. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_mcast_dst.sin_family = AF_INET;
    s_mcast_dst.sin_port = htons(SSDP_PORT);
    s_mcast_dst.sin_addr.s_addr = inet_addr(SSDP_MCAST_ADDR);

    ESP_LOGI(TAG, "SSDP up on %s, LOCATION=%s", ip, dlna_upnp_location());

    /* Initial alive burst (sent twice for reliability). */
    announce_all(true);
    announce_all(true);

    int64_t last_alive = esp_timer_get_time();
    char buf[900];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&from, &fromlen);
        if (len > 0) {
            buf[len] = '\0';
            if (strncmp(buf, "M-SEARCH", 8) == 0) {
                handle_msearch(buf, &from);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_alive >= ALIVE_INTERVAL_US) {
            announce_all(true);
            last_alive = now;
        }
    }
}

esp_err_t dlna_ssdp_start(void)
{
    BaseType_t ok = xTaskCreate(ssdp_task, "ssdp", 6144, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
