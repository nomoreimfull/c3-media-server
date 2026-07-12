#include "media_stream.h"
#include "content_dir.h"
#include "dlna_didl.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "media";

/* Read window size. Bigger reads help SDSPI throughput; kept modest for the
 * C3's small heap. Allocated per request (the httpd task serves one at a time). */
#define SCRATCH_SIZE (8 * 1024)

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

/* Stream bytes [start,end] inclusive from f using chunked transfer. */
static esp_err_t stream_window(httpd_req_t *req, FILE *f, long start, long end)
{
    if (fseek(f, start, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek to %ld failed", start);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek failed");
        return ESP_FAIL;
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
            break;   /* EOF or read error */
        }
        if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) {
            /* Client hung up (Roku stopped/seeked). Abort quietly so the socket frees. */
            ESP_LOGD(TAG, "client disconnected mid-stream");
            ret = ESP_FAIL;
            break;
        }
        remaining -= got;
    }

    free(buf);
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);   /* terminate chunked response */
    }
    return ret;
}

/* If the client asked for DLNA content features, echo the matching headers. */
static void add_dlna_headers(httpd_req_t *req, const char *mime)
{
    char val[8];
    if (httpd_req_get_hdr_value_str(req, "getcontentFeatures.dlna.org", val, sizeof(val)) == ESP_OK) {
        httpd_resp_set_hdr(req, "contentFeatures.dlna.org", dlna_content_features(mime));
    }
    /* Streaming (not interactive/background) transfer for A/V. */
    httpd_resp_set_hdr(req, "transferMode.dlna.org", "Streaming");
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
    if (httpd_req_get_url_query_str(req, query, qlen) != ESP_OK ||
        httpd_query_key_value(query, "path", enc_path, sizeof(enc_path)) != ESP_OK) {
        free(query);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?path=");
        return ESP_FAIL;
    }
    free(query);

    char rel_path[384];
    if (!content_dir_url_decode(enc_path, rel_path, sizeof(rel_path)) ||
        !content_dir_path_is_safe(rel_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    char full_path[512];
    if (!content_dir_full_path(rel_path, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    long file_size = (long)st.st_size;

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    const char *mime = content_dir_mime(rel_path);
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    add_dlna_headers(req, mime);

    long start = 0, end = file_size - 1;
    char range_hdr[64];
    bool partial = false;
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        if (parse_range(range_hdr, file_size, &start, &end)) {
            partial = true;
        } else {
            /* Unsatisfiable range. */
            char cr[48];
            snprintf(cr, sizeof(cr), "bytes */%ld", file_size);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            httpd_resp_send(req, NULL, 0);
            fclose(f);
            return ESP_OK;
        }
    }

    if (partial) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, file_size);
        httpd_resp_set_hdr(req, "Content-Range", cr);
        httpd_resp_set_status(req, "206 Partial Content");
    }

    ESP_LOGI(TAG, "%s %s [%ld-%ld/%ld]", partial ? "206" : "200",
             rel_path, start, end, file_size);

    esp_err_t ret = stream_window(req, f, start, end);
    fclose(f);
    return ret;
}
