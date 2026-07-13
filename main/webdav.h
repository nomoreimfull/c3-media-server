#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

/*
 * Register a read-write WebDAV server at /dav* on the shared httpd, backed by the
 * SD card (SD_MOUNT_POINT). Class-1 (no LOCK) — works with phone WebDAV apps and
 * macOS Finder; Windows write support (LOCK + DAV:2) is a later addition.
 *
 * Directory listings go through content_index; mutations invalidate the affected
 * folder's index. Call after the SD is mounted and the httpd is started.
 */
esp_err_t webdav_register(httpd_handle_t server);
