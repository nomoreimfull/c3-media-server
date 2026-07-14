#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

/*
 * Captive keep-alive: makes the no-internet SoftAP look "online" enough that
 * phones/laptops don't drop it after ~a minute. Two parts:
 *   - a DNS hijack task (UDP :53) resolving every name to the AP IP, so a client's
 *     connectivity probe lands on our HTTP server;
 *   - an HTTP 404 handler that answers those probes with 204/200 "you're online".
 *
 * The 404 handler distinguishes a hijacked probe (Host is some external domain)
 * from a genuine not-found on our own server (Host is our IP) so real /media and
 * WebDAV 404s stay honest.
 *
 * Cannot beat cert-pinned HTTPS checks (that's why Roku still won't stay), but it
 * keeps ordinary clients associated. Requires DHCP to advertise the AP as DNS
 * (see wifi_ap.c).
 */

/* Register the connectivity-probe 404 handler on the shared httpd. */
void captive_register_http(httpd_handle_t server);

/* Start the DNS hijack task. Call after the AP is up. */
esp_err_t captive_dns_start(void);
