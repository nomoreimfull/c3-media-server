#include "config.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "config";

#define NS "cfg"

/* Compile-time first-boot defaults for each key. Returned when NVS has no stored
 * value yet, so a fresh flash behaves exactly like the old CONFIG_*-only build. */
static const char *default_str(const char *key)
{
    if (strcmp(key, "ap_ssid") == 0)  return CONFIG_AP_SSID;
    if (strcmp(key, "ap_pass") == 0)  return CONFIG_AP_PASS;
    if (strcmp(key, "sta_ssid") == 0) return CONFIG_STA_SSID;
    if (strcmp(key, "sta_pass") == 0) return CONFIG_STA_PASS;
    if (strcmp(key, "dav_user") == 0) return "";
    if (strcmp(key, "dav_pass") == 0) return "";
    return "";
}

esp_err_t config_init(void)
{
    /* nvs_flash_init() is already done in app_main for the WiFi driver; just make
     * sure our namespace is reachable (a no-op open validates the partition). */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* namespace created lazily on first write */
    } else {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", NS, esp_err_to_name(err));
    }
    return err;
}

void config_get_str(const char *key, const char *dflt, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_len;
        esp_err_t err = nvs_get_str(h, key, out, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            return;   /* stored value (may be an empty string — that's valid) */
        }
    }
    /* Not stored yet: fall back to the caller's default, or the compile-time one. */
    const char *d = dflt ? dflt : default_str(key);
    snprintf(out, out_len, "%s", d);
}

int config_get_int(const char *key, int dflt)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        esp_err_t err = nvs_get_i32(h, key, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            return (int)v;
        }
    }
    return dflt;
}

esp_err_t config_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_set_int(const char *key, int val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, key, (int32_t)val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
