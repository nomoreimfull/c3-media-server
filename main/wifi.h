#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Bring up WiFi. If a home network is configured (CONFIG_STA_SSID non-empty), try
 * to join it as a station first; on success the box lives on that LAN (with
 * internet — no captive-drop problem). If the join fails, or no STA is
 * configured, fall back to hosting our own SoftAP. Mode is decided once at boot;
 * a station that later drops just auto-reconnects.
 *
 * Requires nvs_flash, esp_netif and the default event loop initialised first.
 */
esp_err_t wifi_start(void);

/* True if we came up as a station on an existing network (vs. hosting the AP). */
bool wifi_is_sta(void);

/* Copy the active interface's IPv4 address (STA's DHCP lease, or the AP's
 * 192.168.4.1) into out — used for SSDP LOCATION and DLNA <res> URLs. */
esp_err_t wifi_get_ip(char *out, size_t out_len);
