#include "media_stream.h"
#include "content_dir.h"
#include "dlna_didl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "media";

/* Read window size. Bigger reads help SDSPI throughput; kept modest for the
 * C3's small heap. Allocated per request (the httpd task serves one at a time). */
#define SCRATCH_SIZE (32 * 1024)

/*
 * Parse an HTTP Range header value ("bytes=start-end", "bytes=start-",
 * "bytes=-suffix") against a known file size. On success fills [*start,*end]
 * (inclusive) and returns true. Returns false if there is no satisfiable range.
 */
static bool parse_range(const char *hdr, long file_size, long *start, long *end)
{
    if (strncasecmp(hdr, "bytes=", 6) != 0) {
        return false;
    }
    const char *p = hdr + 6;
    const char *dash = strchr(p, '-');
    if (!dash) {
        return false;
    }

    long s, e;
    if (dash == p) {
        /* Suffix range: bytes=-N -> last N bytes. */
        long suffix = strtol(dash + 1, NULL, 10);
        if (suffix <= 0) {
            return false;
        }
        if (suffix > file_size) {
            suffix = file_size;
        }
        s = file_size - suffix;
        e = file_size - 1;
    } else {
        s = strtol(p, NULL, 10);
        if (dash[1] == '\0') {
            e = file_size - 1;          /* bytes=start- */
        } else {
            e = strtol(dash + 1, NULL, 10);
        }
    }

    if (s < 0 || s >= file_size || e < s) {
        return false;
    }
    if (e >= file_size) {
        e = file_size - 1;
    }
    *start = s;
    *end = e;
    return true;
}

/* Send exactly len bytes over the request's raw socket, retrying partial writes.
 * Returns false on close/timeout/error (the configured send_wait_timeout applies). */
static bool sock_send_all(httpd_req_t *req, int sockfd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = httpd_socket_send(req->handle, sockfd, buf + sent, len - sent, 0);
        if (r <= 0) {
            return false;
        }
        sent += (size_t)r;
    }
    return true;
}

esp_err_t media_stream_handler(httpd_req_t *req)
{
    /* --- pull ?path= out of the query string --- */
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?path=");
        return ESP_FAIL;
    }
    char *query = malloc(qlen);
    if (!query) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    char enc_path[384];
    char tx_val[12] = "";
    if (httpd_req_get_url_query_str(req, query, qlen) != ESP_OK ||
        httpd_query_key_value(query, "path", enc_path, sizeof(enc_path)) != ESP_OK) {
        free(query);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?path=");
        return ESP_FAIL;
    }
    httpd_query_key_value(query, "tx", tx_val, sizeof(tx_val));   /* optional A/B override */
    free(query);

    media_tx_t tx = MEDIA_TX_DEFAULT;
    if (strcmp(tx_val, "chunked") == 0) {
        tx = MEDIA_TX_CHUNKED;
    } else if (strcmp(tx_val, "clen") == 0) {
        tx = MEDIA_TX_CLEN;
    }

    char rel_path[384];
    if (!content_dir_url_decode(enc_path, rel_path, sizeof(rel_path)) ||
        !content_dir_path_is_safe(rel_path)) {
        ESP_LOGW(TAG, "400 bad path (raw='%s')", enc_path);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    char full_path[512];
    if (!content_dir_full_path(rel_path, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        return ESP_FAIL;
    }

    return media_send_file(req, full_path, rel_path, tx);
}

/* Chunked transfer: keep-alive, no Content-Length. Higher sustained throughput. */
static esp_err_t send_chunked(httpd_req_t *req, FILE *f, const char *mime,
                              long start, long end, long file_size, bool partial)
{
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

    char cf_val[8];
    if (httpd_req_get_hdr_value_str(req, "getcontentFeatures.dlna.org",
                                    cf_val, sizeof(cf_val)) == ESP_OK) {
        httpd_resp_set_hdr(req, "contentFeatures.dlna.org", dlna_content_features(mime));
    }
    httpd_resp_set_hdr(req, "transferMode.dlna.org", "Streaming");

    char cr[64];   /* must stay in scope until the first chunk (headers) is sent */
    if (partial) {
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, file_size);
        httpd_resp_set_hdr(req, "Content-Range", cr);
        httpd_resp_set_status(req, "206 Partial Content");
    }

    char *buf = malloc(SCRATCH_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    long remaining = end - start + 1;
    esp_err_t ret = ESP_OK;
    while (remaining > 0) {
        size_t want = remaining < SCRATCH_SIZE ? (size_t)remaining : SCRATCH_SIZE;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) {
            ESP_LOGD(TAG, "client disconnected mid-stream");
            ret = ESP_FAIL;
            break;
        }
        remaining -= got;
    }
    free(buf);
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

/* Real Content-Length written over the raw socket (Connection: close). Cleaner
 * seeking; one connection per range request. */
static esp_err_t send_clen(httpd_req_t *req, FILE *f, const char *mime,
                           long start, long end, long file_size, bool partial)
{
    long clen = end - start + 1;

    char cf_line[128] = "";
    char cf_val[8];
    if (httpd_req_get_hdr_value_str(req, "getcontentFeatures.dlna.org",
                                    cf_val, sizeof(cf_val)) == ESP_OK) {
        snprintf(cf_line, sizeof(cf_line), "contentFeatures.dlna.org: %s\r\n",
                 dlna_content_features(mime));
    }
    char cr_line[64] = "";
    if (partial) {
        snprintf(cr_line, sizeof(cr_line), "Content-Range: bytes %ld-%ld/%ld\r\n",
                 start, end, file_size);
    }

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: %ld\r\n"
        "%s"                                   /* Content-Range (partial only) */
        "transferMode.dlna.org: Streaming\r\n"
        "%s"                                   /* contentFeatures (if requested) */
        "Connection: close\r\n"
        "\r\n",
        partial ? "206 Partial Content" : "200 OK",
        mime, clen, cr_line, cf_line);
    if (hlen < 0 || hlen >= (int)sizeof(hdr)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "header too long");
        return ESP_FAIL;
    }

    /* From the first raw byte on we own the socket: return ESP_OK on success and
     * mid-stream abort alike (httpd emits nothing further; client sees close). */
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0 || !sock_send_all(req, sockfd, hdr, (size_t)hlen)) {
        return ESP_OK;
    }

    char *buf = malloc(SCRATCH_SIZE);
    if (!buf) {
        return ESP_OK;
    }
    long remaining = clen;
    while (remaining > 0) {
        size_t want = remaining < SCRATCH_SIZE ? (size_t)remaining : SCRATCH_SIZE;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;
        }
        if (!sock_send_all(req, sockfd, buf, got)) {
            ESP_LOGD(TAG, "client disconnected mid-stream");
            break;
        }
        remaining -= got;
    }
    free(buf);
    return ESP_OK;
}

esp_err_t media_send_file(httpd_req_t *req, const char *full_path,
                          const char *name_for_mime, media_tx_t tx)
{
    if (tx == MEDIA_TX_DEFAULT) {
#if defined(CONFIG_MEDIA_TX_CLEN)
        tx = MEDIA_TX_CLEN;
#else
        tx = MEDIA_TX_CHUNKED;
#endif
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "404 '%s' (not found or not a regular file)", full_path);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    long file_size = (long)st.st_size;

    /* --- parse Range (before opening the file, so 416 is cheap) --- */
    long start = 0, end = file_size - 1;
    char range_hdr[64];
    bool partial = false;
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        if (parse_range(range_hdr, file_size, &start, &end)) {
            partial = true;
        } else {
            char cr[48];
            snprintf(cr, sizeof(cr), "bytes */%ld", file_size);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }
    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek failed");
        return ESP_FAIL;
    }

    const char *mime = content_dir_mime(name_for_mime);
    ESP_LOGI(TAG, "%s %s [%ld-%ld/%ld] tx=%s", partial ? "206" : "200",
             name_for_mime, start, end, file_size,
             tx == MEDIA_TX_CLEN ? "clen" : "chunked");

    esp_err_t ret = (tx == MEDIA_TX_CLEN)
                        ? send_clen(req, f, mime, start, end, file_size, partial)
                        : send_chunked(req, f, mime, start, end, file_size, partial);
    fclose(f);
    return ret;
}
