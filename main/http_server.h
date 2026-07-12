#pragma once

#include "esp_http_server.h"

/*
 * Start the single shared HTTP server and register all routes: the /media
 * streamer plus the DLNA UPnP endpoints (via dlna_upnp_register). WebDAV will
 * hang off this same handle later.
 *
 * Call after the SoftAP is up. Returns the httpd handle, or NULL on failure.
 */
httpd_handle_t http_server_start(void);
