#include "webdav.h"
#include "media_stream.h"
#include "content_dir.h"
#include "content_index.h"
#include "sdcard.h"
#include "auth.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "webdav";

#define DAV_PREFIX "/dav"
#define IO_BUF 4096

/* ---------------------------------------------------------- small helpers */

/* Growable string buffer for building the multistatus body. */
typedef struct {
    char *buf;
    size_t len, cap;
    bool err;
} db_t;

static void db_reserve(db_t *db, size_t extra)
{
    if (db->err) {
        return;
    }
    if (db->len + extra + 1 <= db->cap) {
        return;
    }
    size_t nc = db->cap ? db->cap : 1024;
    while (nc < db->len + extra + 1) {
        nc *= 2;
    }
    char *nb = realloc(db->buf, nc);
    if (!nb) {
        db->err = true;
        return;
    }
    db->buf = nb;
    db->cap = nc;
}

static void db_puts(db_t *db, const char *s)
{
    size_t n = strlen(s);
    db_reserve(db, n);
    if (db->err) {
        return;
    }
    memcpy(db->buf + db->len, s, n);
    db->len += n;
    db->buf[db->len] = '\0';
}

static void db_printf(db_t *db, const char *fmt, ...)
{
    char tmp[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        db->err = true;
        return;
    }
    if ((size_t)n < sizeof(tmp)) {
        db_puts(db, tmp);
        return;
    }
    db_reserve(db, n);
    if (db->err) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(db->buf + db->len, db->cap - db->len, fmt, ap);
    va_end(ap);
    db->len += n;
}

static void db_escaped(db_t *db, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  db_puts(db, "&amp;");  break;
        case '<':  db_puts(db, "&lt;");   break;
        case '>':  db_puts(db, "&gt;");   break;
        case '"':  db_puts(db, "&quot;"); break;
        case '\'': db_puts(db, "&apos;"); break;
        default: {
            char c[2] = { *s, '\0' };
            db_puts(db, c);
        }
        }
    }
}

/* Percent-encode for an href, keeping '/'. */
static void db_urlenc(db_t *db, const char *s)
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
        db_puts(db, b);
    }
}

static void http_date(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static void iso_date(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static const char *rel_basename(const char *rel)
{
    if (strcmp(rel, "/") == 0) {
        return "sdcard";
    }
    const char *s = strrchr(rel, '/');
    return (s && s[1]) ? s + 1 : rel;
}

static void rel_parent(const char *rel, char *out, size_t n)
{
    if (strcmp(rel, "/") == 0) {
        snprintf(out, n, "/");
        return;
    }
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

/* Map a "/dav/..." URI (percent-encoded, maybe with query) to a safe SD-relative
 * path. Returns false if malformed, unsafe, or touching the .info mirror. */
static bool uri_to_rel(const char *uri, char *rel, size_t rel_len)
{
    if (strncmp(uri, DAV_PREFIX, strlen(DAV_PREFIX)) != 0) {
        return false;
    }
    const char *p = uri + strlen(DAV_PREFIX);
    char enc[400];
    size_t i = 0;
    while (p[i] && p[i] != '?' && i < sizeof(enc) - 1) {
        enc[i] = p[i];
        i++;
    }
    enc[i] = '\0';
    if (enc[0] == '\0') {
        snprintf(rel, rel_len, "/");
        return true;
    }
    char dec[400];
    if (!content_dir_url_decode(enc, dec, sizeof(dec))) {
        return false;
    }
    size_t l = strlen(dec);
    while (l > 1 && dec[l - 1] == '/') {
        dec[--l] = '\0';
    }
    if (!content_dir_path_is_safe(dec) || strstr(dec, "/.info")) {
        return false;
    }
    int n = snprintf(rel, rel_len, "%s", dec);
    return n > 0 && (size_t)n < rel_len;
}

static bool dest_to_rel(httpd_req_t *req, char *rel, size_t rel_len)
{
    char dest[512];
    if (httpd_req_get_hdr_value_str(req, "Destination", dest, sizeof(dest)) != ESP_OK) {
        return false;
    }
    char *path = dest;
    char *scheme = strstr(dest, "://");
    if (scheme) {
        path = strchr(scheme + 3, '/');
        if (!path) {
            return false;
        }
    }
    return uri_to_rel(path, rel, rel_len);
}

/* Drain and discard a request body (PROPFIND/etc. send an XML body we ignore). */
static void drain_body(httpd_req_t *req)
{
    int remaining = req->content_len;
    char buf[256];
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (r <= 0) {
            break;
        }
        remaining -= r;
    }
}

/* ------------------------------------------------------------ recursive fs */

static int rm_recursive(const char *full)
{
    struct stat st;
    if (stat(full, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(full);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                    continue;
                }
                char child[600];
                snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
                rm_recursive(child);
            }
            closedir(d);
        }
        return rmdir(full);
    }
    return unlink(full);
}

static int cp_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        mkdir(dst, 0777);
        DIR *d = opendir(src);
        if (!d) {
            return -1;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }
            char s[600], dd[600];
            snprintf(s, sizeof(s), "%s/%s", src, de->d_name);
            snprintf(dd, sizeof(dd), "%s/%s", dst, de->d_name);
            cp_recursive(s, dd);
        }
        closedir(d);
        return 0;
    }
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char *buf = malloc(IO_BUF);
    if (buf) {
        size_t r;
        while ((r = fread(buf, 1, IO_BUF, in)) > 0) {
            fwrite(buf, 1, r, out);
        }
        free(buf);
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* --------------------------------------------------------------- PROPFIND */

static void emit_response(db_t *db, const char *rel, const char *name,
                          bool is_dir, long size, time_t mtime)
{
    db_puts(db, "<D:response><D:href>" DAV_PREFIX);
    db_urlenc(db, rel);
    if (is_dir && strcmp(rel, "/") != 0) {
        db_puts(db, "/");
    }
    db_puts(db, "</D:href><D:propstat><D:prop>");
    if (is_dir) {
        db_puts(db, "<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        db_puts(db, "<D:resourcetype/>");
        db_printf(db, "<D:getcontentlength>%ld</D:getcontentlength>", size);
    }
    char hdate[40], idate[24];
    http_date(mtime, hdate, sizeof(hdate));
    iso_date(mtime, idate, sizeof(idate));
    db_printf(db, "<D:getlastmodified>%s</D:getlastmodified>", hdate);
    db_printf(db, "<D:creationdate>%s</D:creationdate>", idate);
    db_puts(db, "<D:displayname>");
    db_escaped(db, name);
    db_puts(db, "</D:displayname>");
    db_puts(db, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

typedef struct {
    db_t *db;
    const char *parent;   /* rel dir of the children */
} propfind_ctx_t;

static bool propfind_child_cb(const content_index_entry_t *e, void *vctx)
{
    propfind_ctx_t *c = vctx;
    char child_rel[512];
    if (strcmp(c->parent, "/") == 0) {
        snprintf(child_rel, sizeof(child_rel), "/%s", e->name);
    } else {
        snprintf(child_rel, sizeof(child_rel), "%s/%s", c->parent, e->name);
    }
    emit_response(c->db, child_rel, e->name, e->is_dir, e->size, 0);
    return true;
}

static esp_err_t dav_propfind(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        drain_body(req);
        return auth_challenge(req);
    }
    char rel[400];
    if (!uri_to_rel(req->uri, rel, sizeof(rel))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char full[512];
    content_dir_full_path(rel, full, sizeof(full));
    struct stat st;
    if (stat(full, &st) != 0) {
        drain_body(req);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    char depth[16] = "1";
    httpd_req_get_hdr_value_str(req, "Depth", depth, sizeof(depth));
    drain_body(req);

    bool is_dir = S_ISDIR(st.st_mode);
    db_t db = {0};
    db_puts(&db, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                 "<D:multistatus xmlns:D=\"DAV:\">");
    emit_response(&db, rel, rel_basename(rel), is_dir,
                  is_dir ? -1 : (long)st.st_size, st.st_mtime);

    if (is_dir && depth[0] != '0') {
        propfind_ctx_t ctx = { &db, rel };
        int num, total;
        content_index_iterate(rel, 0, 0, propfind_child_cb, &ctx, &num, &total);
    }
    db_puts(&db, "</D:multistatus>");

    if (db.err) {
        free(db.buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_set_status(req, "207 Multi-Status");
    esp_err_t r = httpd_resp_send(req, db.buf, db.len);
    free(db.buf);
    return r;
}

/* -------------------------------------------------------------- OPTIONS */

static esp_err_t dav_options(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    /* Advertise class 2 (LOCK/UNLOCK) so Windows "Map network drive" allows writes. */
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "Allow",
        "OPTIONS, GET, PROPFIND, PUT, DELETE, MKCOL, MOVE, COPY, LOCK, UNLOCK");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
}

/* ----------------------------------------------------------------- GET */

typedef struct { db_t *db; const char *parent; } list_ctx_t;

static bool html_child_cb(const content_index_entry_t *e, void *vctx)
{
    list_ctx_t *c = vctx;
    char href[512];
    if (strcmp(c->parent, "/") == 0) {
        snprintf(href, sizeof(href), "/%s", e->name);
    } else {
        snprintf(href, sizeof(href), "%s/%s", c->parent, e->name);
    }
    db_puts(c->db, "<li><a href=\"" DAV_PREFIX);
    db_urlenc(c->db, href);
    db_puts(c->db, e->is_dir ? "/\">" : "\">");
    db_escaped(c->db, e->name);
    db_puts(c->db, e->is_dir ? "/</a></li>" : "</a></li>");
    return true;
}

static esp_err_t dav_get(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    char rel[400];
    if (!uri_to_rel(req->uri, rel, sizeof(rel))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char full[512];
    content_dir_full_path(rel, full, sizeof(full));
    struct stat st;
    if (stat(full, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return media_send_file(req, full, rel, MEDIA_TX_DEFAULT);
    }
    /* GET on a folder → a minimal HTML index (handy from a browser). */
    db_t db = {0};
    db_puts(&db, "<!doctype html><meta charset=utf-8><title>");
    db_escaped(&db, rel);
    db_puts(&db, "</title><ul>");
    list_ctx_t ctx = { &db, rel };
    int num, total;
    content_index_iterate(rel, 0, 0, html_child_cb, &ctx, &num, &total);
    db_puts(&db, "</ul>");
    if (db.err) {
        free(db.buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, db.buf, db.len);
    free(db.buf);
    return r;
}

/* ----------------------------------------------------------------- PUT */

/* Build the hidden temp path "<dir>/.<name>.part" for the final path `full`. */
static void put_temp_path(const char *full, char *tmp, size_t n)
{
    const char *slash = strrchr(full, '/');
    if (!slash) {
        snprintf(tmp, n, ".%s.part", full);
        return;
    }
    size_t dir_len = (size_t)(slash - full);   /* excludes the slash */
    snprintf(tmp, n, "%.*s/.%s.part", (int)dir_len, full, slash + 1);
}

static esp_err_t dav_put(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        drain_body(req);
        return auth_challenge(req);
    }
    char rel[400];
    if (!uri_to_rel(req->uri, rel, sizeof(rel)) || strcmp(rel, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char full[512];
    content_dir_full_path(rel, full, sizeof(full));

    /* Write to a hidden temp file, then rename on full success — so a failed or
     * cancelled upload never leaves a partial file at the real name. */
    char tmp[600];
    put_temp_path(full, tmp, sizeof(tmp));

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        httpd_resp_set_status(req, "409 Conflict");   /* parent missing? */
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    char *buf = malloc(IO_BUF);
    if (!buf) {
        fclose(f);
        unlink(tmp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int want = remaining < IO_BUF ? remaining : IO_BUF;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            ok = false;   /* SD full / write error */
            break;
        }
        remaining -= r;
    }
    free(buf);
    /* Flush to the card before we decide success — a failed close is a failed write. */
    if (fflush(f) != 0 || fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        unlink(tmp);   /* no partial file survives */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }

    /* Commit: replace any existing file atomically-ish (unlink + rename on FATFS). */
    unlink(full);
    if (rename(tmp, full) != 0) {
        unlink(tmp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
        return ESP_FAIL;
    }

    char parent[400];
    rel_parent(rel, parent, sizeof(parent));
    content_index_invalidate(parent);

    ESP_LOGI(TAG, "PUT %s (%d bytes)", rel, req->content_len);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

/* --------------------------------------------------------------- DELETE */

static esp_err_t dav_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    char rel[400];
    if (!uri_to_rel(req->uri, rel, sizeof(rel)) || strcmp(rel, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char full[512];
    content_dir_full_path(rel, full, sizeof(full));
    if (rm_recursive(full) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    char parent[400];
    rel_parent(rel, parent, sizeof(parent));
    content_index_invalidate(parent);
    ESP_LOGI(TAG, "DELETE %s", rel);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------------------------------------------------------- MKCOL */

static esp_err_t dav_mkcol(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    char rel[400];
    if (!uri_to_rel(req->uri, rel, sizeof(rel)) || strcmp(rel, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char full[512];
    content_dir_full_path(rel, full, sizeof(full));
    if (mkdir(full, 0777) != 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    char parent[400];
    rel_parent(rel, parent, sizeof(parent));
    content_index_invalidate(parent);
    ESP_LOGI(TAG, "MKCOL %s", rel);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------ MOVE / COPY */

static esp_err_t move_or_copy(httpd_req_t *req, bool is_move)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    char src[400], dst[400];
    if (!uri_to_rel(req->uri, src, sizeof(src)) || strcmp(src, "/") == 0 ||
        !dest_to_rel(req, dst, sizeof(dst)) || strcmp(dst, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    char src_full[512], dst_full[512];
    content_dir_full_path(src, src_full, sizeof(src_full));
    content_dir_full_path(dst, dst_full, sizeof(dst_full));

    int rc;
    if (is_move) {
        rc = rename(src_full, dst_full);
        if (rc != 0 && cp_recursive(src_full, dst_full) == 0) {
            rm_recursive(src_full);   /* cross-dir rename fallback */
            rc = 0;
        }
    } else {
        rc = cp_recursive(src_full, dst_full);
    }
    if (rc != 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    char p[400];
    rel_parent(dst, p, sizeof(p));
    content_index_invalidate(p);
    if (is_move) {
        rel_parent(src, p, sizeof(p));
        content_index_invalidate(p);
    }
    ESP_LOGI(TAG, "%s %s -> %s", is_move ? "MOVE" : "COPY", src, dst);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_move(httpd_req_t *req) { return move_or_copy(req, true); }
static esp_err_t dav_copy(httpd_req_t *req) { return move_or_copy(req, false); }

/* ------------------------------------------------------------ LOCK / UNLOCK */

/* We don't implement real locking (single-worker httpd = one request at a time),
 * but Windows "Map network drive" refuses to write unless the server claims
 * class-2 lock support. Hand back a well-formed lock so writes are allowed. */
#define FAKE_LOCK_TOKEN "opaquelocktoken:c3-media-0000-0000-0000-000000000001"

static esp_err_t dav_lock(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        drain_body(req);
        return auth_challenge(req);
    }
    char rel[400];
    if (!uri_to_rel(req->uri, rel, sizeof(rel))) {
        drain_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    drain_body(req);   /* ignore the requested lock scope/owner */

    db_t db = {0};
    db_puts(&db, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                 "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
                 "<D:locktype><D:write/></D:locktype>"
                 "<D:lockscope><D:exclusive/></D:lockscope>"
                 "<D:depth>infinity</D:depth>"
                 "<D:timeout>Second-3600</D:timeout>"
                 "<D:locktoken><D:href>" FAKE_LOCK_TOKEN "</D:href></D:locktoken>"
                 "<D:lockroot><D:href>" DAV_PREFIX);
    db_urlenc(&db, rel);
    db_puts(&db, "</D:href></D:lockroot>"
                 "</D:activelock></D:lockdiscovery></D:prop>");
    if (db.err) {
        free(db.buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_set_hdr(req, "Lock-Token", "<" FAKE_LOCK_TOKEN ">");
    httpd_resp_set_status(req, "200 OK");
    esp_err_t r = httpd_resp_send(req, db.buf, db.len);
    free(db.buf);
    return r;
}

static esp_err_t dav_unlock(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return auth_challenge(req);
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* -------------------------------------------------------------- register */

esp_err_t webdav_register(httpd_handle_t server)
{
    struct {
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {
        { HTTP_OPTIONS,  dav_options },
        { HTTP_PROPFIND, dav_propfind },
        { HTTP_GET,      dav_get },
        { HTTP_PUT,      dav_put },
        { HTTP_DELETE,   dav_delete },
        { HTTP_MKCOL,    dav_mkcol },
        { HTTP_MOVE,     dav_move },
        { HTTP_COPY,     dav_copy },
        { HTTP_LOCK,     dav_lock },
        { HTTP_UNLOCK,   dav_unlock },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t u = {
            .uri = "/dav*",
            .method = routes[i].method,
            .handler = routes[i].handler,
        };
        esp_err_t err = httpd_register_uri_handler(server, &u);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register method %d failed: %s",
                     routes[i].method, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "WebDAV mounted at %s (read-write, class 2)", DAV_PREFIX);
    return ESP_OK;
}
