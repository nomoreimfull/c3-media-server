#pragma once

#include "esp_err.h"
#include <stddef.h>

/*
 * Runtime configuration backed by NVS (namespace "cfg"), with the compile-time
 * Kconfig values as first-boot defaults. Lets WiFi + WebDAV credentials be edited
 * from the settings page and survive reboots.
 *
 * Keys (<=15 chars): ap_ssid, ap_pass, sta_ssid, sta_pass, ap_chan,
 *                    dav_user, dav_pass.
 */
esp_err_t config_init(void);

/* Copy the stored value for key into out, or dflt if unset. Always NUL-terminates. */
void config_get_str(const char *key, const char *dflt, char *out, size_t out_len);
int  config_get_int(const char *key, int dflt);

esp_err_t config_set_str(const char *key, const char *val);
esp_err_t config_set_int(const char *key, int val);
