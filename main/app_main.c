#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "wifi_ap.h"
#include "sdcard.h"
#include "http_server.h"
#include "dlna_ssdp.h"

static const char *TAG = "app";

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed (non-fatal)");
        return;
    }
    mdns_hostname_set("c3-media");
    mdns_instance_name_set(CONFIG_DLNA_FRIENDLY_NAME);
    /* Advertise the HTTP service (helps humans find the box; DLNA uses SSDP). */
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_HTTP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://c3-media.local:%d", CONFIG_HTTP_PORT);
}

void app_main(void)
{
    /* NVS is required by the WiFi driver. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Bring up the SoftAP first so the HTTP/SSDP layers have an IP to bind to. */
    ESP_ERROR_CHECK(wifi_ap_start());

    /* Mount the SD card. Non-fatal: the server still answers discovery so the
     * failure is visible, just with nothing to browse. */
    if (sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD card not mounted — browsing will be empty. Check wiring/format.");
    }

    start_mdns();

    /* HTTP server (streaming + UPnP endpoints); must precede SSDP so LOCATION is set. */
    if (http_server_start() == NULL) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }

    /* SSDP discovery so Roku lists the server. */
    ESP_ERROR_CHECK(dlna_ssdp_start());

    ESP_LOGI(TAG, "C3 media server ready: '%s'", CONFIG_DLNA_FRIENDLY_NAME);
}
