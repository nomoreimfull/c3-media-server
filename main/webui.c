#include "webui.h"
#include "content_dir.h"
#include "content_index.h"
#include "config.h"
#include "auth.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
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
    "h1{font-size:.95rem;color:#8a8;padding:.5rem 3rem .5rem .5rem;margin:0;word-break:break-all}"
    ".up{color:#aaa}p{color:#888;padding:.5rem}"
    ".tools{position:fixed;top:.5rem;right:.5rem;display:flex;flex-direction:column;gap:.5rem;z-index:20}"
    ".tools a,.tools button{width:32px;height:32px;padding:0;border:0;background:transparent;cursor:pointer;display:flex;align-items:center;justify-content:center}"
    ".tools svg{width:32px;height:32px;display:block}"
    ".st{display:none;position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);background:rgba(0,0,0,.88);color:#fff;padding:1rem 1.25rem;border-radius:8px;z-index:30;max-width:80vw;text-align:center}"
    "</style>";

/* 32x32 white icons (Feather set) + the browser uploader script. Attributes use
 * single quotes so the C string needs no escaping. */
static const char GEAR_SVG[] =
    "<svg width='32' height='32' viewBox='0 0 24 24' fill='none' stroke='#fff' "
    "stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<circle cx='12' cy='12' r='3'/>"
    "<path d='M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06"
    "a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4"
    "a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82"
    "a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82"
    "l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3"
    "a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83"
    "l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09"
    "a1.65 1.65 0 0 0-1.51 1z'/></svg>";

static const char UPLOAD_SVG[] =
    "<svg width='32' height='32' viewBox='0 0 24 24' fill='none' stroke='#fff' "
    "stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/>"
    "<polyline points='17 8 12 3 7 8'/>"
    "<line x1='12' y1='3' x2='12' y2='15'/></svg>";

/* Uploads each picked file to CWD via WebDAV PUT (reuses the safe temp-file path),
 * one at a time, then reloads so the new files appear. CWD comes from data-cwd. */
static const char BROWSE_JS[] =
    "<script>"
    "var CWD=document.querySelector('.tools').dataset.cwd;"
    "var fi=document.getElementById('fi'),st=document.getElementById('st');"
    "function up(fs,i){"
    "if(i>=fs.length){location.reload();return;}"
    "var f=fs[i],b=CWD==='/'?'':CWD;"
    "var u='/dav'+b.split('/').map(encodeURIComponent).join('/')+'/'+encodeURIComponent(f.name);"
    "var x=new XMLHttpRequest();x.open('PUT',u,true);st.style.display='block';"
    "x.upload.onprogress=function(e){if(e.lengthComputable){"
    "st.textContent='Uploading '+f.name+' '+Math.round(100*e.loaded/e.total)+'% ('+(i+1)+'/'+fs.length+')';}};"
    "x.onload=function(){if(x.status>=200&&x.status<300){up(fs,i+1);}"
    "else{st.textContent='Failed ('+x.status+'): '+f.name;}};"
    "x.onerror=function(){st.textContent='Error uploading '+f.name;};"
    "x.send(f);}"
    "fi.onchange=function(){if(fi.files.length)up(fi.files,0);};"
    "</script>";

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
    } else if (content_dir_is_video(e->name)) {
        sb_puts(c->sb, "<a href=\"/play?path=");
        sb_url(c->sb, child);
        sb_puts(c->sb, "\">\xF0\x9F\x8E\xAC ");   /* 🎬 */
        sb_esc(c->sb, e->name);
        sb_puts(c->sb, "</a>");
    } else {
        /* Non-video file: open/download it straight from the streamer. */
        sb_puts(c->sb, "<a href=\"/media?path=");
        sb_url(c->sb, child);
        sb_puts(c->sb, "\">\xF0\x9F\x93\x84 ");   /* 📄 */
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

    /* Fixed top-right toolbar: settings gear, then an upload button below it. The
     * current dir rides along in data-cwd (HTML-escaped) for the uploader script. */
    sb_puts(&sb, "<div class=tools data-cwd=\"");
    sb_esc(&sb, dir);
    sb_puts(&sb, "\"><a href=\"/settings\" title=\"Settings\">");
    sb_puts(&sb, GEAR_SVG);
    sb_puts(&sb, "</a><button type=button onclick=\"fi.click()\" title=\"Upload to this folder\">");
    sb_puts(&sb, UPLOAD_SVG);
    sb_puts(&sb, "</button></div>");
    sb_puts(&sb, "<input id=fi type=file multiple style=\"display:none\">");
    sb_puts(&sb, "<div id=st class=st></div>");

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
    content_index_list_all(dir, browse_row, &ctx, &num, &total);
    if (total == 0) {
        sb_puts(&sb, "<p>(empty)</p>");
    }

    sb_puts(&sb, BROWSE_JS);

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

/* ---------------------------------------------------------- settings page */

/* One editable field: config key + form label + whether to mask it as a password. */
typedef struct {
    const char *key;
    const char *label;
    bool secret;
} field_t;

static const field_t SETTINGS_FIELDS[] = {
    { "ap_ssid",  "Access-point name (SSID)",     false },
    { "ap_pass",  "Access-point password",        true  },
    { "sta_ssid", "Home WiFi name (station SSID)", false },
    { "sta_pass", "Home WiFi password",           true  },
    { "dav_user", "WebDAV / settings username",   false },
    { "dav_pass", "WebDAV / settings password",   true  },
};
#define N_SETTINGS_FIELDS (sizeof(SETTINGS_FIELDS) / sizeof(SETTINGS_FIELDS[0]))

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));   /* let the HTTP response flush first */
    esp_restart();
}

static esp_err_t ui_settings_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    sb_t sb = {0};
    sb_puts(&sb, BROWSE_HEAD);
    sb_puts(&sb,
        "<h1>Settings</h1>"
        "<form method=post action=\"/settings\" "
        "style=\"padding:.5rem;display:flex;flex-direction:column;gap:.6rem\">");

    for (size_t i = 0; i < N_SETTINGS_FIELDS; i++) {
        const field_t *fp = &SETTINGS_FIELDS[i];
        char val[96];
        config_get_str(fp->key, NULL, val, sizeof(val));
        sb_puts(&sb, "<label style=\"color:#8a8;font-size:.85rem\">");
        sb_esc(&sb, fp->label);
        sb_puts(&sb, "<br><input name=\"");
        sb_puts(&sb, fp->key);
        sb_puts(&sb, "\" value=\"");
        sb_esc(&sb, val);
        sb_puts(&sb, fp->secret ? "\" type=text autocomplete=off" : "\" type=text");
        sb_puts(&sb, " style=\"width:100%;box-sizing:border-box;padding:.5rem;"
                     "background:#1a1a1a;color:#eee;border:1px solid #333;border-radius:4px\">"
                     "</label>");
    }

    {
        char chan[8];
        snprintf(chan, sizeof(chan), "%d", config_get_int("ap_chan", CONFIG_AP_CHANNEL));
        sb_puts(&sb, "<label style=\"color:#8a8;font-size:.85rem\">AP channel (1-13)"
                     "<br><input name=\"ap_chan\" value=\"");
        sb_esc(&sb, chan);
        sb_puts(&sb, "\" type=text style=\"width:100%;box-sizing:border-box;padding:.5rem;"
                     "background:#1a1a1a;color:#eee;border:1px solid #333;border-radius:4px\">"
                     "</label>");
    }

    sb_puts(&sb,
        "<button type=submit style=\"padding:.7rem;background:#1d2a33;color:#7cf;"
        "border:1px solid #345;border-radius:4px;font-size:1rem\">Save &amp; reboot</button>"
        "</form>"
        "<p>Leaving the WebDAV password blank disables the login. "
        "Saving reboots the device to apply WiFi changes.</p>"
        "<a class=up href=\"/\">\xE2\xAC\x85 back</a>");   /* ⬅ */

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

static esp_err_t ui_settings_post(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        got += r;
    }
    body[total] = '\0';

    /* Persist each known text field (leaving one absent from the form keeps its value). */
    for (size_t i = 0; i < N_SETTINGS_FIELDS; i++) {
        const char *key = SETTINGS_FIELDS[i].key;
        char enc[256], dec[128];
        if (httpd_query_key_value(body, key, enc, sizeof(enc)) == ESP_OK &&
            content_dir_url_decode(enc, dec, sizeof(dec))) {
            config_set_str(key, dec);
        }
    }
    {
        char enc[16], dec[16];
        if (httpd_query_key_value(body, "ap_chan", enc, sizeof(enc)) == ESP_OK &&
            content_dir_url_decode(enc, dec, sizeof(dec))) {
            int ch = atoi(dec);
            if (ch >= 1 && ch <= 13) {
                config_set_int("ap_chan", ch);
            }
        }
    }
    free(body);

    ESP_LOGI(TAG, "settings saved — rebooting");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta http-equiv=refresh content=\"6; url=/\">"
        "<body style=\"font-family:system-ui;background:#111;color:#eee;padding:2rem\">"
        "Saved. Rebooting to apply\xE2\x80\xA6 this page returns to the browser in a few seconds. "
        "If the WiFi name or password changed, reconnect to the new network.",
        HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------- register */

esp_err_t webui_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = ui_root },
        { .uri = "/browse",   .method = HTTP_GET,  .handler = ui_browse },
        { .uri = "/play",     .method = HTTP_GET,  .handler = ui_play },
        { .uri = "/settings", .method = HTTP_GET,  .handler = ui_settings_get },
        { .uri = "/settings", .method = HTTP_POST, .handler = ui_settings_post },
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
