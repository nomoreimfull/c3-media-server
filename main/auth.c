#include "auth.h"
#include "config.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "mbedtls/base64.h"
#include "esp_log.h"

static const char *TAG = "auth";

bool auth_ok(httpd_req_t *req)
{
    char pass[64];
    config_get_str("dav_pass", "", pass, sizeof(pass));
    if (pass[0] == '\0') {
        return true;   /* no password set → auth disabled */
    }
    char user[64];
    config_get_str("dav_user", "", user, sizeof(user));

    /* Header form: "Basic <base64(user:pass)>". */
    char hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    const char *b64 = hdr;
    if (strncasecmp(b64, "Basic ", 6) != 0) {
        return false;
    }
    b64 += 6;
    while (*b64 == ' ') {
        b64++;
    }

    unsigned char dec[192];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        return false;
    }
    dec[dlen] = '\0';

    /* Build the expected "user:pass" and constant-ish compare. */
    char expect[128];
    int n = snprintf(expect, sizeof(expect), "%s:%s", user, pass);
    if (n < 0 || (size_t)n >= sizeof(expect)) {
        return false;
    }
    return strcmp((const char *)dec, expect) == 0;
}

esp_err_t auth_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"c3-media\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Authentication required\n", HTTPD_RESP_USE_STRLEN);
    ESP_LOGD(TAG, "401 challenge for %s", req->uri);
    return ESP_FAIL;
}
