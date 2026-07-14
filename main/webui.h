#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

/*
 * Minimal built-in web media browser — the practical way to watch, bypassing
 * DLNA's slow "extract metadata" pre-scan. Server-rendered HTML:
 *   GET /            browse the SD root
 *   GET /browse?dir= browse a folder (folders + playable videos)
 *   GET /play?path=  a full-screen HTML5 <video> pointing at /media?path=
 * Listings come from content_index (fast); playback from media_send_file (2 s
 * start for a well-formed file). Register after the httpd is up.
 */
esp_err_t webui_register(httpd_handle_t server);
