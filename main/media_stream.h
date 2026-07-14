#pragma once

#include "esp_http_server.h"

/* HTTP transfer method for media responses.
 *  - CHUNKED: httpd chunked transfer, keep-alive, no Content-Length (higher
 *    sustained throughput — one connection reused across range requests).
 *  - CLEN:    real Content-Length written over the raw socket (cleaner seeking,
 *    Connection: close).
 *  - DEFAULT: whichever CONFIG_MEDIA_TX_* selects. */
typedef enum {
    MEDIA_TX_DEFAULT = 0,
    MEDIA_TX_CHUNKED,
    MEDIA_TX_CLEN,
} media_tx_t;

/*
 * URI handler for `GET /media?path=<url-encoded SD path>`. Honours HTTP Range
 * (206/Content-Range) and the DLNA getcontentFeatures/transferMode handshake.
 * A `?tx=chunked` or `?tx=clen` query param overrides the transfer method for
 * that request (for A/B testing); otherwise the Kconfig default is used.
 */
esp_err_t media_stream_handler(httpd_req_t *req);

/*
 * URI handler for `GET /tx` — the live transfer-method toggle. `?m=chunked|clen|default`
 * sets the runtime default (applies to DLNA + WebDAV + direct URLs); always replies with
 * the current effective method as plain text. Lets you A/B without rebuilding.
 */
esp_err_t media_tx_handler(httpd_req_t *req);

/*
 * Stream a file from its absolute VFS path with HTTP Range support, using the
 * given transfer method (MEDIA_TX_DEFAULT = Kconfig default). Content-Type comes
 * from name_for_mime's extension. Shared by /media and WebDAV GET; sends its own
 * error responses. GET only — it always writes a body, so don't route HEAD here.
 */
esp_err_t media_send_file(httpd_req_t *req, const char *full_path,
                          const char *name_for_mime, media_tx_t tx);
