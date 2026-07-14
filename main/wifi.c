#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "wifi";

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_is_sta;

/* Set while we are still deciding STA vs AP at boot. */
static EventGroupHandle_t s_events;
#define STA_GOT_IP_BIT BIT0
#define STA_FAIL_BIT   BIT1
#define STA_JOIN_RETRIES 5

static int s_retry;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_is_sta) {
            /* Decided STA already — keep trying to rejoin forever. */
            esp_wifi_connect();
        } else if (s_retry < STA_JOIN_RETRIES) {
            s_retry++;
            ESP_LOGI(TAG, "STA connect retry %d/%d", s_retry, STA_JOIN_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, STA_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_is_sta = true;   /* future disconnects now auto-reconnect */
        xEventGroupSetBits(s_events, STA_GOT_IP_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client joined (aid=%d)", e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client left (aid=%d)", e->aid);
    }
}

/* Try to join the configured home network. Returns true on success. */
static bool try_station(void)
{
    if (strlen(CONFIG_STA_SSID) == 0) {
        return false;   /* no home network configured */
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_STA_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_STA_PASS, sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "trying home network '%s' ...", CONFIG_STA_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_events, STA_GOT_IP_BIT | STA_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & STA_GOT_IP_BIT) {
        return true;
    }

    /* Failed / timed out — tear the STA path down before falling back to AP. */
    ESP_LOGW(TAG, "home network unavailable — falling back to AP");
    esp_wifi_stop();
    esp_netif_destroy_default_wifi(s_sta_netif);
    s_sta_netif = NULL;
    s_is_sta = false;
    return false;
}

static void start_ap(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(CONFIG_AP_SSID);
    ap.ap.channel = CONFIG_AP_CHANNEL;
    ap.ap.max_connection = CONFIG_AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    if (strlen(CONFIG_AP_PASS) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        strlcpy((char *)ap.ap.password, CONFIG_AP_PASS, sizeof(ap.ap.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    char ip[16] = {0};
    wifi_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "SoftAP up: SSID='%s' channel=%d ip=%s auth=%s",
             CONFIG_AP_SSID, CONFIG_AP_CHANNEL, ip,
             (ap.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2");
}

esp_err_t wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_events = xEventGroupCreate();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    if (try_station()) {
        char ip[16] = {0};
        wifi_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "joined '%s' as station, ip=%s", CONFIG_STA_SSID, ip);
    } else {
        start_ap();
    }

    /* Keep the radio fully awake — this is a streaming server, not a sensor. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

bool wifi_is_sta(void)
{
    return s_is_sta;
}

esp_err_t wifi_get_ip(char *out, size_t out_len)
{
    esp_netif_t *netif = s_is_sta ? s_sta_netif : s_ap_netif;
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}
