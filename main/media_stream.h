#pragma once

#include "esp_http_server.h"

/*
 * URI handler for `GET /media?path=<url-encoded SD-relative path>`.
 *
 * Streams the file straight off the SD card with chunked transfer, honouring
 * HTTP Range requests (206 Partial Content + Content-Range) so Roku can seek and
 * reconnect mid-stream. Also answers the DLNA getcontentFeatures.dlna.org /
 * transferMode.dlna.org handshake headers.
 */
esp_err_t media_stream_handler(httpd_req_t *req);
