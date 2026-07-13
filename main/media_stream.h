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

/*
 * Stream a file from its absolute VFS path with chunked transfer + HTTP Range
 * support (206/Content-Range), setting Content-Type from name_for_mime's extension.
 * Shared by the /media handler and WebDAV GET/HEAD. Sends its own error responses.
 */
esp_err_t media_send_file(httpd_req_t *req, const char *full_path, const char *name_for_mime);
