#pragma once

#include "esp_err.h"

/*
 * Start WiFi in SoftAP mode using the Kconfig SSID/password/channel. The default
 * AP netif runs a DHCP server, so clients (the Roku, a test laptop) get an IP on
 * the C3's subnet (default 192.168.4.x, C3 itself at 192.168.4.1).
 *
 * Requires nvs_flash, esp_netif and the default event loop to be initialised first.
 * Returns ESP_OK once the AP is up.
 */
esp_err_t wifi_ap_start(void);

/*
 * Copy the AP's own IPv4 address (e.g. "192.168.4.1") into out. This is used to
 * build absolute URLs in SSDP LOCATION headers and DLNA <res> elements.
 * Returns ESP_OK on success.
 */
esp_err_t wifi_ap_get_ip(char *out, size_t out_len);
