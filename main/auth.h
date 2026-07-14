#pragma once

#include "esp_http_server.h"
#include <stdbool.h>

/*
 * HTTP Basic auth backed by the runtime config (dav_user / dav_pass).
 *
 * If dav_pass is empty, auth is DISABLED — every request passes. This avoids
 * locking the box out before any credentials have been set from the settings
 * page. Once a password is set, Settings + WebDAV require the login; the media
 * browser and /media streaming stay open.
 */

/* True if the request may proceed (auth disabled, or a correct Basic header). */
bool auth_ok(httpd_req_t *req);

/* Send a 401 with a WWW-Authenticate challenge. Returns ESP_FAIL for convenience
 * so a handler can `return auth_challenge(req);`. */
esp_err_t auth_challenge(httpd_req_t *req);
