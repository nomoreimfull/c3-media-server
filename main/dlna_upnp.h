#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

/*
 * Register the UPnP MediaServer HTTP endpoints on an already-running httpd:
 *   GET  /desc.xml     device description
 *   GET  /cd_scpd.xml  ContentDirectory SCPD
 *   GET  /cm_scpd.xml  ConnectionManager SCPD
 *   POST /cd_control   ContentDirectory SOAP control (Browse, ...)
 *   POST /cm_control   ConnectionManager SOAP control (GetProtocolInfo, ...)
 *
 * Captures the server base URL (http://<ap-ip>:<port>) for <res> links, so call
 * this after the SoftAP is up. Returns ESP_OK on success.
 */
esp_err_t dlna_upnp_register(httpd_handle_t server);

/* The absolute device-description URL (http://<ip>:<port>/desc.xml), used by
 * SSDP as the LOCATION header. Valid after dlna_upnp_register(). */
const char *dlna_upnp_location(void);
