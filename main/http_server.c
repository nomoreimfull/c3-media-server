#include "http_server.h"
#include "media_stream.h"
#include "dlna_upnp.h"
#include "webdav.h"
#include "sdkconfig.h"

#include "esp_log.h"

static const char *TAG = "http";

httpd_handle_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_HTTP_PORT;
    config.lru_purge_enable = true;       /* reclaim the oldest socket under pressure */
    config.max_uri_handlers = 20;         /* /media + 5 UPnP + 8 WebDAV + headroom */
    config.stack_size = 8192;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 30;   /* survive the player pausing after it buffers */
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t media = {
        .uri = "/media",
        .method = HTTP_GET,
        .handler = media_stream_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &media));

    const httpd_uri_t tx = {
        .uri = "/tx",
        .method = HTTP_GET,
        .handler = media_tx_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &tx));

    if (dlna_upnp_register(server) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register UPnP endpoints");
    }

    if (webdav_register(server) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register WebDAV endpoints");
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", CONFIG_HTTP_PORT);
    return server;
}
