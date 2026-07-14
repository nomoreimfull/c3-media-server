#include "webui.h"
#include "content_dir.h"
#include "content_index.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "esp_log.h"

static const char *TAG = "webui";

/* ---------------------------------------------------------- string builder */

typedef struct {
    char *buf;
    size_t len, cap;
    bool err;
} sb_t;

static void sb_reserve(sb_t *sb, size_t extra)
{
    if (sb->err) {
        return;
    }
    if (sb->len + extra + 1 <= sb->cap) {
        return;
    }
    size_t nc = sb->cap ? sb->cap : 1024;
    while (nc < sb->len + extra + 1) {
        nc *= 2;
    }
    char *nb = realloc(sb->buf, nc);
    if (!nb) {
        sb->err = true;
        return;
    }
    sb->buf = nb;
    sb->cap = nc;
}

static void sb_puts(sb_t *sb, const char *s)
{
    size_t n = strlen(s);
    sb_reserve(sb, n);
    if (sb->err) {
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

/* HTML-escape into the buffer. */
static void sb_esc(sb_t *sb, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  sb_puts(sb, "&amp;");  break;
        case '<':  sb_puts(sb, "&lt;");   break;
        case '>':  sb_puts(sb, "&gt;");   break;
        case '"':  sb_puts(sb, "&quot;"); break;
        case '\'': sb_puts(sb, "&#39;");  break;
        default: {
            char c[2] = { *s, '\0' };
            sb_puts(sb, c);
        }
        }
    }
}

/* Percent-encode for a URL query value, keeping '/'. */
static void sb_url(sb_t *sb, const char *s)
{
    static const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == '/';
        char b[4];
        if (safe) {
            b[0] = (char)c; b[1] = '\0';
        } else {
            b[0] = '%'; b[1] = hex[c >> 4]; b[2] = hex[c & 0xF]; b[3] = '\0';
        }
        sb_puts(sb, b);
    }
}

/* ---------------------------------------------------------- path helpers */

static const char *base_name(const char *rel)
{
    const char *s = strrchr(rel, '/');
    return (s && s[1]) ? s + 1 : rel;
}

static void parent_dir(const char *rel, char *out, size_t n)
{
    const char *s = strrchr(rel, '/');
    if (!s || s == rel) {
        snprintf(out, n, "/");
        return;
    }
    size_t k = (size_t)(s - rel);
    if (k >= n) {
        k = n - 1;
    }
    memcpy(out, rel, k);
    out[k] = '\0';
}

/* Read a query key and URL-decode it into out. Returns false if absent/bad. */
static bool get_query(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        return false;
    }
    char *q = malloc(qlen);
    if (!q) {
        return false;
    }
    char enc[400];
    bool ok = (httpd_req_get_url_query_str(req, q, qlen) == ESP_OK &&
               httpd_query_key_value(q, key, enc, sizeof(enc)) == ESP_OK);
    free(q);
    return ok && content_dir_url_decode(enc, out, out_len);
}

/* ---------------------------------------------------------- browse page */

static const char BROWSE_HEAD[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>C3 Media</title><style>"
    "body{font-family:system-ui,-apple-system,sans-serif;background:#111;color:#eee;margin:0;padding:.4rem}"
    "a{color:#7cf;text-decoration:none;display:block;padding:.75rem .5rem;border-bottom:1px solid #2a2a2a}"
    "a:active{background:#1d2a33}"
    "h1{font-size:.95rem;color:#8a8;padding:.5rem;margin:0;word-break:break-all}"
    ".up{color:#aaa}p{color:#888;padding:.5rem}</style>";

typedef struct {
    sb_t *sb;
    const char *dir;
} browse_ctx_t;

static bool browse_row(const content_index_entry_t *e, void *vctx)
{
    browse_ctx_t *c = vctx;
    char child[512];
    if (strcmp(c->dir, "/") == 0) {
        snprintf(child, sizeof(child), "/%s", e->name);
    } else {
        snprintf(child, sizeof(child), "%s/%s", c->dir, e->name);
    }
    if (e->is_dir) {
        sb_puts(c->sb, "<a href=\"/browse?dir=");
        sb_url(c->sb, child);
        sb_puts(c->sb, "\">\xF0\x9F\x93\x81 ");   /* 📁 */
        sb_esc(c->sb, e->name);
        sb_puts(c->sb, "/</a>");
    } else {
        sb_puts(c->sb, "<a href=\"/play?path=");
        sb_url(c->sb, child);
        sb_puts(c->sb, "\">\xF0\x9F\x8E\xAC ");   /* 🎬 */
        sb_esc(c->sb, e->name);
        sb_puts(c->sb, "</a>");
    }
    return true;
}

static esp_err_t render_browse(httpd_req_t *req, const char *dir)
{
    if (!content_dir_path_is_safe(dir) || strstr(dir, "/.info")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad dir");
        return ESP_FAIL;
    }

    sb_t sb = {0};
    sb_puts(&sb, BROWSE_HEAD);
    sb_puts(&sb, "<h1>");
    sb_esc(&sb, dir);
    sb_puts(&sb, "</h1>");

    if (strcmp(dir, "/") != 0) {
        char up[400];
        parent_dir(dir, up, sizeof(up));
        sb_puts(&sb, "<a class=up href=\"/browse?dir=");
        sb_url(&sb, up);
        sb_puts(&sb, "\">\xE2\xAC\x86 up</a>");   /* ⬆ */
    }

    browse_ctx_t ctx = { &sb, dir };
    int num = 0, total = 0;
    content_index_iterate(dir, 0, 0, browse_row, &ctx, &num, &total);
    if (total == 0) {
        sb_puts(&sb, "<p>(empty)</p>");
    }

    if (sb.err) {
        free(sb.buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, sb.buf, sb.len);
    free(sb.buf);
    return r;
}

static esp_err_t ui_root(httpd_req_t *req)
{
    return render_browse(req, "/");
}

static esp_err_t ui_browse(httpd_req_t *req)
{
    char dir[400] = "/";
    char q[400];
    if (get_query(req, "dir", q, sizeof(q))) {   /* only adopt a clean decode */
        snprintf(dir, sizeof(dir), "%s", q);
    }
    return render_browse(req, dir);
}

/* ---------------------------------------------------------- player page */

static esp_err_t ui_play(httpd_req_t *req)
{
    char path[400];
    if (!get_query(req, "path", path, sizeof(path)) ||
        !content_dir_path_is_safe(path) || strstr(path, "/.info")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char up[400];
    parent_dir(path, up, sizeof(up));

    sb_t sb = {0};
    sb_puts(&sb,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>");
    sb_esc(&sb, base_name(path));
    sb_puts(&sb,
        "</title><style>body{margin:0;background:#000}"
        "video{width:100vw;height:100vh;object-fit:contain}"
        ".bar{position:fixed;top:0;left:0;padding:.5rem 1rem;background:rgba(0,0,0,.55);"
        "font-family:system-ui,sans-serif}a{color:#7cf;text-decoration:none}</style>"
        "<div class=bar><a href=\"/browse?dir=");
    sb_url(&sb, up);
    sb_puts(&sb, "\">\xE2\xAC\x85 back</a></div>");   /* ⬅ */
    sb_puts(&sb, "<video controls autoplay playsinline src=\"/media?path=");
    sb_url(&sb, path);
    sb_puts(&sb, "\"></video>");

    if (sb.err) {
        free(sb.buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, sb.buf, sb.len);
    free(sb.buf);
    return r;
}

/* ---------------------------------------------------------- register */

esp_err_t webui_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET, .handler = ui_root },
        { .uri = "/browse", .method = HTTP_GET, .handler = ui_browse },
        { .uri = "/play",   .method = HTTP_GET, .handler = ui_play },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "web UI at http://<ip>/");
    return ESP_OK;
}
