#pragma once

#include "esp_err.h"

/*
 * Start the SSDP task: joins the 239.255.255.250:1900 multicast group on the AP
 * interface, answers M-SEARCH discovery requests, and periodically multicasts
 * ssdp:alive NOTIFYs so control points (Roku, VLC, Windows) list the server.
 *
 * Call after the SoftAP is up and dlna_upnp_register() has set the LOCATION URL.
 */
esp_err_t dlna_ssdp_start(void);
